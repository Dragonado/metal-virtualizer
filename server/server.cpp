// The server is standalone: it emits BOTH implementation halves in this TU
// (it does not dep on //third_party/metal-cpp:foundation-impl, which exists
// for client binaries where Metal's impl is deliberately absent).
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/strings/str_format.h"
#include "proto/tnrc.grpc.pb.h"
#include "proto/tnrc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;

using namespace tnrc;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

// Logic and data behind the server's behavior.
class ShimmerImpl final : public TnrcService::Service {
  public:
    ShimmerImpl() {
        counter_ = 0;
    }
    Status CreateSystemDefaultDeviceShim(ServerContext *context, const CreateSystemDefaultDeviceShimRequest *request, CreateSystemDefaultDeviceShimResponse *response) override {
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

    Status CreateCommandQueueShim(ServerContext *context, const CreateCommandQueueShimRequest *request, CreateCommandQueueShimResponse *response) override {
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
        auto command_queue_itr = command_queue_map_.find(request->command_queue_id());
        if (command_queue_itr == command_queue_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find command queue.");
        }

        command_queue_itr->second->release();
        command_queue_map_.erase(command_queue_itr);
        return Status::OK;
    }

    Status CreateLibraryShim(ServerContext *context, const CreateLibraryShimRequest *request, CreateLibraryShimResponse *response) override {
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        NS::Error *err;

        MTL::Library *library = device_itr->second->newLibrary(
            NS::String::string(request->source().c_str(), NS::UTF8StringEncoding), nullptr, &err);
        if (library == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create library");
        }
        counter_++;
        library_map_[counter_] = library;
        response->set_library_id(counter_);
        return Status::OK;
    }

    Status ReleaseLibraryShim(ServerContext *context, const ReleaseLibraryShimRequest *request, ReleaseLibraryShimResponse *response) override {
        auto library_itr = library_map_.find(request->library_id());
        if (library_itr == library_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find library.");
        }

        library_itr->second->release();
        library_map_.erase(library_itr);
        return Status::OK;
    }

    Status CreateFunctionShim(ServerContext *context, const CreateFunctionShimRequest *request, CreateFunctionShimResponse *response) override {
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
        auto function_itr = function_map_.find(request->function_id());
        if (function_itr == function_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find function.");
        }

        function_itr->second->release();
        function_map_.erase(function_itr);
        return Status::OK;
    }

    Status CreateComputePipelineStateShim(ServerContext *context, const CreateComputePipelineStateShimRequest *request, CreateComputePipelineStateShimResponse *response) override {
        auto device_itr = device_map_.find(request->device_id());
        if (device_itr == device_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find device.");
        }

        auto function_itr = function_map_.find(request->function_id());
        if (function_itr == function_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find function.");
        }

        NS::Error *err;
        MTL::ComputePipelineState *compute_pipeline_state = device_itr->second->newComputePipelineState(
            function_itr->second, &err);
        if (compute_pipeline_state == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create compute_pipeline_state");
        }
        counter_++;
        compute_pipeline_state_map_[counter_] = compute_pipeline_state;
        response->set_compute_pipeline_state_id(counter_);
        response->set_max_total_threads_per_threadgroup(compute_pipeline_state->maxTotalThreadsPerThreadgroup());

        return Status::OK;
    }

    Status ReleaseComputePipelineStateShim(ServerContext *context, const ReleaseComputePipelineStateShimRequest *request, ReleaseComputePipelineStateShimResponse *response) override {
        auto compute_pipeline_state_itr = compute_pipeline_state_map_.find(request->compute_pipeline_state_id());
        if (compute_pipeline_state_itr == compute_pipeline_state_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find compute pipeline state.");
        }

        compute_pipeline_state_itr->second->release();
        compute_pipeline_state_map_.erase(compute_pipeline_state_itr);
        return Status::OK;
    }

    Status CreateBufferShim(ServerContext *context, const CreateBufferShimRequest *request, CreateBufferShimResponse *response) override {
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
        auto buffer_itr = buffer_map_.find(request->buffer_id());
        if (buffer_itr == buffer_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find buffer.");
        }

        buffer_itr->second->release();
        buffer_map_.erase(buffer_itr);
        return Status::OK;
    }
    Status CommitCommandBuffer(ServerContext *context, const CommitCommandBufferRequest *request, CommitCommandBufferResponse *response) override {
        auto command_queue_itr = command_queue_map_.find(request->command_queue_id());
        if (command_queue_itr == command_queue_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find command queue.");
        }

        auto compute_pipeline_state_itr = compute_pipeline_state_map_.find(request->compute_pipeline_state_id());
        if (compute_pipeline_state_itr == compute_pipeline_state_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find compute pipeline state.");
        }

        MTL::CommandBuffer *command_buffer = command_queue_itr->second->commandBuffer();
        MTL::ComputeCommandEncoder *compute_command_encoder = command_buffer->computeCommandEncoder();
        compute_command_encoder->setComputePipelineState(compute_pipeline_state_itr->second);

        assert(request->buffer_ids_size() == request->buffer_offsets_size());
        assert(request->buffer_ids_size() == request->index_map_size());

        size_t offset = 0;
        for (size_t i = 0; i < request->buffer_ids().size(); i++) {
            auto buffer_itr = buffer_map_.find(request->buffer_ids().Get(i));
            if (buffer_itr == buffer_map_.end()) {
                return Status(StatusCode::NOT_FOUND, "Could not find buffer.");
            }

            memcpy(buffer_itr->second->contents(), request->all_buffer_data().data() + offset, buffer_itr->second->length());
            compute_command_encoder->setBuffer(buffer_itr->second, request->buffer_offsets().Get(i), request->index_map().Get(i));
            offset += buffer_itr->second->length();
        }

        compute_command_encoder->dispatchThreads(MTL::Size(request->grid_size(), 1, 1), MTL::Size(request->thread_group_size(), 1, 1));
        compute_command_encoder->endEncoding();
        command_buffer->commit();

        counter_++;
        command_buffer_map_[counter_] = command_buffer;

        response->set_command_buffer_id(counter_);
        return Status::OK;
    }
    Status WaitUntilCompleted(ServerContext *context, const WaitUntilCompletedRequest *request, WaitUntilCompletedResponse *response) override {
        auto command_buffer_itr = command_buffer_map_.find(request->command_buffer_id());
        if (command_buffer_itr == command_buffer_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find command buffer.");
        }

        command_buffer_itr->second->waitUntilCompleted();

        size_t total_size = 0;
        std::vector<MTL::Buffer *> buffers;

        for (int i = 0; i < request->buffer_ids_size(); ++i) {
            auto buffer_itr = buffer_map_.find(request->buffer_ids(i));

            if (buffer_itr == buffer_map_.end()) {
                return Status(StatusCode::NOT_FOUND, "Could not find buffer.");
            }

            MTL::Buffer *buffer = buffer_itr->second;
            buffers.push_back(buffer);
            total_size += buffer->length();
        }
        response->mutable_all_buffer_data()->resize(total_size);

        size_t offset = 0;
        for (MTL::Buffer *buffer : buffers) {
            memcpy(response->mutable_all_buffer_data()->data() + offset, buffer->contents(), buffer->length());
            offset += buffer->length();
        }

        return Status::OK;
    }
    ~ShimmerImpl() {
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
    uint32_t counter_;
    std::map<uint32_t, MTL::ComputePipelineState *> compute_pipeline_state_map_;
    std::map<uint32_t, MTL::Function *> function_map_;
    std::map<uint32_t, MTL::Library *> library_map_;
    std::map<uint32_t, MTL::CommandBuffer *> command_buffer_map_;
    std::map<uint32_t, MTL::CommandQueue *> command_queue_map_;
    std::map<uint32_t, MTL::Buffer *> buffer_map_;
    std::map<uint32_t, MTL::Device *> device_map_;
};

void RunServer(uint16_t port) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    ShimmerImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
}

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);
    absl::InitializeLog();
    RunServer(absl::GetFlag(FLAGS_port));
    return 0;
}
