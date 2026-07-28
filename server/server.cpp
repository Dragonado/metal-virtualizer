// The server is standalone: it emits BOTH implementation halves in this TU
// (it does not dep on //third_party/metal-cpp:foundation-impl, which exists
// for client binaries where Metal's impl is deliberately absent).
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/strings/str_format.h"
#include "proto/metal_remote.grpc.pb.h"
#include "proto/metal_remote.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

using namespace metal_remote;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

class ScopedAutoreleasePool {
  public:
    ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}

    ~ScopedAutoreleasePool() {
        pool_->release();
    }

  private:
    NS::AutoreleasePool *pool_;
};

enum class JobState {
    NOT_STARTED,
    QUEUED,
    RUNNING,
    COMPLETED,
    FAILED,
};

struct Job {
    uint32_t command_buffer_id;
    CommitCommandBufferRequest request;
    MTL::CommandBuffer *command_buffer = nullptr;
    JobState state = JobState::NOT_STARTED;

    Status failure_status;
    std::condition_variable completed_cv;
};

// Logic and data behind the server's behavior.
class ShimmerImpl final : public MetalRemoteService::Service {
  public:
    void fail_job_locked(const std::shared_ptr<Job> &job, StatusCode code,
                         const std::string &message) {
        job->state = JobState::FAILED;
        job->failure_status = Status(code, message);
        job->completed_cv.notify_all();
    }

    void update_job_status_after_completion(const std::shared_ptr<Job> job) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (job->command_buffer->status() == MTL::CommandBufferStatus::CommandBufferStatusCompleted) {
            job->state = JobState::COMPLETED;
        } else {
            job->state = JobState::FAILED;
            job->failure_status = Status(StatusCode::INTERNAL, "Metal command buffer failed.");
        }

        job->command_buffer->release();
        job->command_buffer = nullptr;
        job->completed_cv.notify_all();
    }

    void commit_job(std::shared_ptr<Job> &job) {
        ScopedAutoreleasePool autorelease_pool;
        CommitCommandBufferRequest &request = job->request;
        std::unique_lock<std::mutex> lock(mtx_);

        auto command_queue_itr = command_queue_map_.find(request.command_queue_id());
        if (command_queue_itr == command_queue_map_.end()) {
            fail_job_locked(job, StatusCode::NOT_FOUND, "Command queue was released before submission.");
            return;
        }

        auto compute_pipeline_state_itr =
            compute_pipeline_state_map_.find(request.compute_pipeline_state_id());
        if (compute_pipeline_state_itr == compute_pipeline_state_map_.end()) {
            fail_job_locked(job, StatusCode::NOT_FOUND,
                            "Compute pipeline state was released before submission.");
            return;
        }

        job->command_buffer = command_queue_itr->second->commandBuffer();
        if (job->command_buffer == nullptr) {
            fail_job_locked(job, StatusCode::INTERNAL,
                            "Metal failed to create a command buffer.");
            return;
        }

        MTL::ComputeCommandEncoder *compute_command_encoder = job->command_buffer->computeCommandEncoder();
        if (compute_command_encoder == nullptr) {
            job->command_buffer = nullptr;
            fail_job_locked(job, StatusCode::INTERNAL,
                            "Metal failed to create a compute command encoder.");
            return;
        }
        compute_command_encoder->setComputePipelineState(compute_pipeline_state_itr->second);

        size_t offset = 0;
        for (size_t i = 0; i < request.buffer_ids().size(); i++) {
            auto buffer_itr = buffer_map_.find(request.buffer_ids().Get(i));

            if (buffer_itr == buffer_map_.end()) {
                job->command_buffer = nullptr;
                fail_job_locked(job, StatusCode::NOT_FOUND,
                                "Buffer was released before submission.");
                return;
            }

            size_t buffer_length = buffer_itr->second->length();
            if (offset > request.all_buffer_data().size() ||
                buffer_length > request.all_buffer_data().size() - offset) {
                job->command_buffer = nullptr;
                fail_job_locked(job, StatusCode::INVALID_ARGUMENT,
                                "Command buffer payload is shorter than its buffer metadata.");
                return;
            }

            memcpy(buffer_itr->second->contents(), request.all_buffer_data().data() + offset, buffer_itr->second->length());
            compute_command_encoder->setBuffer(buffer_itr->second, request.buffer_offsets().Get(i), request.index_map().Get(i));
            offset += buffer_length;
        }

        if (offset != request.all_buffer_data().size()) {
            job->command_buffer = nullptr;
            fail_job_locked(job, StatusCode::INVALID_ARGUMENT,
                            "Command buffer payload contains extra bytes.");
            return;
        }

        lock.unlock();

        compute_command_encoder->dispatchThreads(MTL::Size(request.grid_size(), 1, 1), MTL::Size(request.thread_group_size(), 1, 1));
        compute_command_encoder->endEncoding();
        job->command_buffer->addCompletedHandler([this, job](MTL::CommandBuffer *command_buffer) {
            update_job_status_after_completion(job);
        });
        job->command_buffer->retain();
        job->command_buffer->commit();
    }

    // Suppose there are two jobs (q1, c1) and (q2, c2). Where q = command queue and c = command buffer and c1 was commited before c2 by the client.
    // Then the ONLY schedule ordering invariant the server must hold is: if q1 == q2 then c1 commits before c2.
    void scheduler_loop() {
        while (true) {
            std::unique_lock<std::mutex> lock(mtx_);
            scheduler_cv_.wait(lock, [&]() { return is_server_shutdown || !ready_jobs_.empty(); });

            if (is_server_shutdown)
                break;

            // ENTER COMPLEX SCHEDULING LOGIC.
            // CURRENT LOGIC: FIFO
            auto job = *ready_jobs_.begin();
            ready_jobs_.pop_front();

            if (job->state != JobState::QUEUED) {
                fail_job_locked(job, StatusCode::INTERNAL,
                                "Scheduler received a job in an invalid state.");
                continue;
            }
            job->state = JobState::RUNNING;

            lock.unlock();
            commit_job(job);
        }
    }

    ShimmerImpl() {
        counter_ = 0;
        is_server_shutdown = false;
        scheduler_thread_ = std::thread(&ShimmerImpl::scheduler_loop, this);
    }

    Status CreateSystemDefaultDeviceShim(ServerContext *context, const CreateSystemDefaultDeviceShimRequest *request, CreateSystemDefaultDeviceShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        MTL::Device *device;
        device = MTL::CreateSystemDefaultDevice();
        if (device == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create metal device.");
        }

        counter_++;
        device_map_[counter_] = device;
        response->set_device_id(counter_);
        response->set_device_name(device->name()->cString(NS::UTF8StringEncoding));

        return Status::OK;
    }

    Status ReleaseDeviceShim(ServerContext *context, const ReleaseDeviceShimRequest *request, ReleaseDeviceShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        device_itr->second->release();
        device_map_.erase(device_itr);
        return Status::OK;
    }

    Status CreateCommandQueueShim(ServerContext *context, const CreateCommandQueueShimRequest *request, CreateCommandQueueShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        MTL::CommandQueue *command_queue = device_itr->second->newCommandQueue();

        if (command_queue == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create command queue.");
        }
        counter_++;
        command_queue_map_[counter_] = command_queue;
        response->set_command_queue_id(counter_);
        return Status::OK;
    }

    Status ReleaseCommandQueueShim(ServerContext *context, const ReleaseCommandQueueShimRequest *request, ReleaseCommandQueueShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto command_queue_itr = command_queue_map_.find(request->command_queue_id());
        if (command_queue_itr == command_queue_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find command queue.");
        }

        command_queue_itr->second->release();
        command_queue_map_.erase(command_queue_itr);
        return Status::OK;
    }

    Status CreateLibraryShim(ServerContext *context, const CreateLibraryShimRequest *request, CreateLibraryShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        NS::Error *err = nullptr;

        MTL::Library *library = device_itr->second->newLibrary(
            NS::String::string(request->source().c_str(), NS::UTF8StringEncoding), nullptr, &err);
        if (library == nullptr) {
            std::string message = "Could not create library.";
            if (err != nullptr && err->localizedDescription() != nullptr) {
                message += " ";
                message += err->localizedDescription()->utf8String();
            }
            return Status(StatusCode::INTERNAL, message);
        }
        counter_++;
        library_map_[counter_] = library;
        response->set_library_id(counter_);
        return Status::OK;
    }

    Status ReleaseLibraryShim(ServerContext *context, const ReleaseLibraryShimRequest *request, ReleaseLibraryShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto library_itr = library_map_.find(request->library_id());
        if (library_itr == library_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find library.");
        }

        library_itr->second->release();
        library_map_.erase(library_itr);
        return Status::OK;
    }

    Status CreateFunctionShim(ServerContext *context, const CreateFunctionShimRequest *request, CreateFunctionShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto library_itr = library_map_.find(request->library_id());
        if (library_itr == library_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find library.");
        }

        MTL::Function *function = library_itr->second->newFunction(
            NS::String::string(request->function_name().c_str(), NS::UTF8StringEncoding));
        if (function == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create function");
        }
        counter_++;
        function_map_[counter_] = function;
        response->set_function_id(counter_);
        return Status::OK;
    }

    Status ReleaseFunctionShim(ServerContext *context, const ReleaseFunctionShimRequest *request, ReleaseFunctionShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto function_itr = function_map_.find(request->function_id());
        if (function_itr == function_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find function.");
        }

        function_itr->second->release();
        function_map_.erase(function_itr);
        return Status::OK;
    }

    Status CreateComputePipelineStateShim(ServerContext *context, const CreateComputePipelineStateShimRequest *request, CreateComputePipelineStateShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        auto function_itr = function_map_.find(request->function_id());
        if (function_itr == function_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find function.");
        }

        NS::Error *err = nullptr;
        MTL::ComputePipelineState *compute_pipeline_state = device_itr->second->newComputePipelineState(
            function_itr->second, &err);
        if (compute_pipeline_state == nullptr) {
            std::string message = "Could not create compute pipeline state.";
            if (err != nullptr && err->localizedDescription() != nullptr) {
                message += " ";
                message += err->localizedDescription()->utf8String();
            }
            return Status(StatusCode::INTERNAL, message);
        }
        counter_++;
        compute_pipeline_state_map_[counter_] = compute_pipeline_state;
        response->set_compute_pipeline_state_id(counter_);
        response->set_max_total_threads_per_threadgroup(compute_pipeline_state->maxTotalThreadsPerThreadgroup());

        return Status::OK;
    }

    Status ReleaseComputePipelineStateShim(ServerContext *context, const ReleaseComputePipelineStateShimRequest *request, ReleaseComputePipelineStateShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto compute_pipeline_state_itr = compute_pipeline_state_map_.find(request->compute_pipeline_state_id());
        if (compute_pipeline_state_itr == compute_pipeline_state_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find compute pipeline state.");
        }

        compute_pipeline_state_itr->second->release();
        compute_pipeline_state_map_.erase(compute_pipeline_state_itr);
        return Status::OK;
    }

    Status CreateBufferShim(ServerContext *context, const CreateBufferShimRequest *request, CreateBufferShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        MTL::Buffer *buffer = device_itr->second->newBuffer(request->length(), request->options());
        if (buffer == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create buffer.");
        }
        counter_++;
        buffer_map_[counter_] = buffer;
        response->set_buffer_id(counter_);
        return Status::OK;
    }

    Status ReleaseBufferShim(ServerContext *context, const ReleaseBufferShimRequest *request, ReleaseBufferShimResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::lock_guard<std::mutex> lock(mtx_);
        auto buffer_itr = buffer_map_.find(request->buffer_id());
        if (buffer_itr == buffer_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find buffer.");
        }

        buffer_itr->second->release();
        buffer_map_.erase(buffer_itr);
        return Status::OK;
    }

    Status CommitCommandBuffer(ServerContext *context, const CommitCommandBufferRequest *request, CommitCommandBufferResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::unique_lock<std::mutex> lock(mtx_);

        if (request->buffer_ids_size() != request->buffer_offsets_size() ||
            request->buffer_ids_size() != request->index_map_size()) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "Buffer IDs, offsets, and indices must have equal lengths.");
        }

        if (request->grid_size() <= 0 || request->thread_group_size() <= 0) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "Grid and thread-group sizes must be positive.");
        }

        auto command_queue_itr = command_queue_map_.find(request->command_queue_id());
        if (command_queue_itr == command_queue_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find command queue.");
        }

        auto compute_pipeline_state_itr = compute_pipeline_state_map_.find(request->compute_pipeline_state_id());
        if (compute_pipeline_state_itr == compute_pipeline_state_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find compute pipeline state.");
        }

        size_t expected_payload_size = 0;
        for (int i = 0; i < request->buffer_ids_size(); ++i) {
            auto buffer_itr = buffer_map_.find(request->buffer_ids(i));
            if (buffer_itr == buffer_map_.end()) {
                return Status(StatusCode::NOT_FOUND, "Could not find buffer.");
            }

            size_t buffer_length = buffer_itr->second->length();
            if (expected_payload_size > request->all_buffer_data().size() ||
                request->buffer_offsets(i) > buffer_length ||
                buffer_length > request->all_buffer_data().size() - expected_payload_size) {
                return Status(StatusCode::INVALID_ARGUMENT,
                              "Invalid buffer offset or packed buffer-data size.");
            }
            expected_payload_size += buffer_length;
        }

        if (expected_payload_size != request->all_buffer_data().size()) {
            return Status(StatusCode::INVALID_ARGUMENT,
                          "Packed buffer data contains extra bytes.");
        }

        lock.unlock();
        auto job = std::make_shared<Job>(); // allocate job on heap.
        job->request = *request;

        lock.lock();
        job->command_buffer_id = ++counter_;
        job->state = JobState::QUEUED;
        job_map_[job->command_buffer_id] = job;
        ready_jobs_.push_back(job);
        response->set_command_buffer_id(job->command_buffer_id);
        lock.unlock();
        scheduler_cv_.notify_one();

        return Status::OK;
    }

    Status WaitUntilCompleted(ServerContext *context, const WaitUntilCompletedRequest *request, WaitUntilCompletedResponse *response) override {
        ScopedAutoreleasePool autorelease_pool;
        std::unique_lock<std::mutex> lock(mtx_);
        auto job_itr = job_map_.find(request->command_buffer_id());

        if (job_itr == job_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find command buffer.");
        }

        auto job = job_itr->second;
        job->completed_cv.wait(lock, [&job]() { return job->state == JobState::COMPLETED || job->state == JobState::FAILED; });

        if (job->state == JobState::FAILED) {
            Status failure_status = job->failure_status;
            job_map_.erase(job_itr);
            return failure_status;
        }

        if (request->buffer_ids_size() != job->request.buffer_ids_size()) {
            job_map_.erase(job_itr);
            return Status(StatusCode::INVALID_ARGUMENT,
                          "Wait request buffers do not match the committed command buffer.");
        }

        size_t total_size = 0;
        std::vector<MTL::Buffer *> buffers;

        for (int i = 0; i < request->buffer_ids_size(); ++i) {
            if (request->buffer_ids(i) != job->request.buffer_ids(i)) {
                job_map_.erase(job_itr);
                return Status(StatusCode::INVALID_ARGUMENT,
                              "Wait request buffers do not match the committed command buffer.");
            }

            auto buffer_itr = buffer_map_.find(request->buffer_ids(i));

            if (buffer_itr == buffer_map_.end()) {
                job_map_.erase(job_itr);
                return Status(StatusCode::NOT_FOUND, "Could not find buffer.");
            }

            MTL::Buffer *buffer = buffer_itr->second;
            buffers.push_back(buffer);
            total_size += buffer->length();
        }

        for (MTL::Buffer *buffer : buffers) {
            buffer->retain();
        }

        job_map_.erase(job_itr);
        lock.unlock();

        response->mutable_all_buffer_data()->resize(total_size);

        size_t offset = 0;
        for (MTL::Buffer *buffer : buffers) {
            memcpy(response->mutable_all_buffer_data()->data() + offset, buffer->contents(), buffer->length());
            offset += buffer->length();
            buffer->release();
        }

        return Status::OK;
    }

    ~ShimmerImpl() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            is_server_shutdown = true;
        }

        scheduler_cv_.notify_one();
        scheduler_thread_.join();

        // Order is important.
        for (auto &[id, compute_pipeline_state] : compute_pipeline_state_map_) {
            if (compute_pipeline_state != nullptr)
                compute_pipeline_state->release();
        }
        for (auto &[id, function] : function_map_) {
            if (function != nullptr)
                function->release();
        }
        for (auto &[id, library] : library_map_) {
            if (library != nullptr)
                library->release();
        }
        for (auto &[id, command_queue] : command_queue_map_) {
            if (command_queue != nullptr)
                command_queue->release();
        }
        for (auto &[id, device] : device_map_) {
            if (device != nullptr)
                device->release();
        }
        for (auto &[id, buffer] : buffer_map_) {
            if (buffer != nullptr)
                buffer->release();
        }
    }

  private:
    std::atomic<uint32_t> counter_;
    std::map<uint32_t, MTL::ComputePipelineState *> compute_pipeline_state_map_;
    std::map<uint32_t, MTL::Function *> function_map_;
    std::map<uint32_t, MTL::Library *> library_map_;
    std::map<uint32_t, MTL::CommandQueue *> command_queue_map_;
    std::map<uint32_t, MTL::Buffer *> buffer_map_;
    std::map<uint32_t, MTL::Device *> device_map_;

    std::mutex mtx_;
    std::condition_variable scheduler_cv_;

    std::deque<std::shared_ptr<Job>> ready_jobs_;
    std::map<uint32_t, std::shared_ptr<Job>> job_map_;

    std::thread scheduler_thread_;
    bool is_server_shutdown;
};

void RunServer(uint16_t port) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    ShimmerImpl service;

    ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0); // only one server per port
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        std::cerr << "Could not start server on " << server_address << std::endl;
        return;
    }
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
}

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();
    RunServer(absl::GetFlag(FLAGS_port));
    return 0;
}
