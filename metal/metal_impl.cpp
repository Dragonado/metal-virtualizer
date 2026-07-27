#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "metal_shim.h"

#include "proto/tnrc.grpc.pb.h"
#include "proto/tnrc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using namespace tnrc;

namespace MetalShim {

namespace {
class TnrcServiceClient {
  public:
    TnrcServiceClient(std::shared_ptr<Channel> channel)
        : stub_(TnrcService::NewStub(channel)) {}

    Device *CreateDevice() {
        CreateSystemDefaultDeviceShimRequest request;
        CreateSystemDefaultDeviceShimResponse response;
        ClientContext context;

        Status status = stub_->CreateSystemDefaultDeviceShim(&context, request, &response);

        if (status.ok()) {
            return (new Device(response.device_id(), NS::String::string(response.device_name().c_str(), NS::UTF8StringEncoding)));
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return NULL;
        }
    }

    std::optional<uint32_t> CreateCommandQueue(uint32_t device_id) {
        CreateCommandQueueShimRequest request;
        CreateCommandQueueShimResponse response;
        ClientContext context;

        request.set_device_id(device_id);

        Status status = stub_->CreateCommandQueueShim(&context, request, &response);

        if (status.ok()) {
            return response.command_queue_id();
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return std::nullopt;
        }
    }

    bool ReleaseCommandQueue(uint32_t command_queue_id) {
        ReleaseCommandQueueShimRequest request;
        ReleaseCommandQueueShimResponse response;
        ClientContext context;

        request.set_command_queue_id(command_queue_id);

        Status status = stub_->ReleaseCommandQueueShim(&context, request, &response);

        if (status.ok()) {
            return true;
        }

        std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                  << std::endl;
        return false;
    }

    std::optional<uint32_t> CreateLibrary(uint32_t device_id, const NS::String *source, const CompileOptions *options, NS::Error **error) {
        CreateLibraryShimRequest request;
        CreateLibraryShimResponse response;
        ClientContext context;

        request.set_device_id(device_id);
        request.set_source(source->cString(NS::UTF8StringEncoding));

        // TODO: Add options and error.

        Status status = stub_->CreateLibraryShim(&context, request, &response);

        if (status.ok()) {
            return response.library_id();
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return std::nullopt;
        }
    }

    bool ReleaseLibrary(uint32_t library_id) {
        ReleaseLibraryShimRequest request;
        ReleaseLibraryShimResponse response;
        ClientContext context;

        request.set_library_id(library_id);

        Status status = stub_->ReleaseLibraryShim(&context, request, &response);

        if (status.ok()) {
            return true;
        }

        std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                  << std::endl;
        return false;
    }

    std::optional<uint32_t> CreateFunction(uint32_t device_id, const NS::String *function_name) {
        CreateFunctionShimRequest request;
        CreateFunctionShimResponse response;
        ClientContext context;

        request.set_library_id(device_id);
        request.set_function_name(function_name->cString(NS::UTF8StringEncoding));

        // TODO: Add options and error.

        Status status = stub_->CreateFunctionShim(&context, request, &response);

        if (status.ok()) {
            return response.function_id();
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return std::nullopt;
        }
    }

    bool ReleaseFunction(uint32_t function_id) {
        ReleaseFunctionShimRequest request;
        ReleaseFunctionShimResponse response;
        ClientContext context;

        request.set_function_id(function_id);

        Status status = stub_->ReleaseFunctionShim(&context, request, &response);

        if (status.ok()) {
            return true;
        }

        std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                  << std::endl;
        return false;
    }

    ComputePipelineState *CreateComputePipelineState(uint32_t device_id, const Function *func, NS::Error **error) {
        CreateComputePipelineStateShimRequest request;
        CreateComputePipelineStateShimResponse response;
        ClientContext context;

        request.set_device_id(device_id);
        request.set_function_id(func->get_function_id());

        // TODO: Add options and error.

        Status status = stub_->CreateComputePipelineStateShim(&context, request, &response);

        if (status.ok()) {
            return (new ComputePipelineState(response.compute_pipeline_state_id(), response.max_total_threads_per_threadgroup()));
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return NULL;
        }
    }

    bool ReleaseComputePipelineState(uint32_t compute_pipeline_state_id) {
        ReleaseComputePipelineStateShimRequest request;
        ReleaseComputePipelineStateShimResponse response;
        ClientContext context;

        request.set_compute_pipeline_state_id(compute_pipeline_state_id);

        Status status = stub_->ReleaseComputePipelineStateShim(&context, request, &response);

        if (status.ok()) {
            return true;
        }

        std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                  << std::endl;
        return false;
    }

    Buffer *CreateBuffer(uint32_t device_id, NS::UInteger length, ResourceOptions options) {
        CreateBufferShimRequest request;
        CreateBufferShimResponse response;
        ClientContext context;

        request.set_device_id(device_id);
        request.set_length(length);
        request.set_options(options);

        // TODO: Add options and error.

        Status status = stub_->CreateBufferShim(&context, request, &response);

        if (status.ok()) {
            return (new Buffer(response.buffer_id(), length, options));
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return NULL;
        }
    }

    bool ReleaseBuffer(uint32_t buffer_id) {
        ReleaseBufferShimRequest request;
        ReleaseBufferShimResponse response;
        ClientContext context;

        request.set_buffer_id(buffer_id);

        Status status = stub_->ReleaseBufferShim(&context, request, &response);

        if (status.ok()) {
            return true;
        }

        std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                  << std::endl;
        return false;
    }

    bool CommitCommandBuffer(CommandBuffer *command_buffer) {
        if (command_buffer->get_compute_encoder() == NULL) {
            std::cerr << "[SHIM] Nothing encoded in command buffer." << std::endl;
            return true;
        }

        ComputeCommandEncoder *compute_command_encoder = command_buffer->get_compute_encoder();

        if (compute_command_encoder->get_compute_pipeline_state() == NULL) {
            std::cerr << "[SHIM] No pipeline to run in compute encoder." << std::endl;
            return true;
        }

        CommitCommandBufferRequest request;
        CommitCommandBufferResponse response;
        ClientContext context;

        request.set_command_queue_id(command_buffer->get_command_queue_id());
        request.set_compute_pipeline_state_id(compute_command_encoder->get_compute_pipeline_state()->compute_pipeline_state_id());
        request.set_grid_size(compute_command_encoder->get_grid_size().width);
        request.set_thread_group_size(compute_command_encoder->get_thread_group_size().width);

        for (const auto &binding :
             compute_command_encoder->get_all_encoder_buffer_structs()) {
            Buffer *buffer = binding.buf;

            request.add_buffer_ids(buffer->buffer_id());
            request.add_buffer_offsets(binding.offset);
            request.add_index_map(binding.index);

            request.mutable_all_buffer_data()->append(
                static_cast<const char *>(buffer->contents()),
                buffer->length());
        }

        Status status = stub_->CommitCommandBuffer(&context, request, &response);

        if (!status.ok()) {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return false;
        }

        command_buffer->set_command_buffer_id(response.command_buffer_id());

        return true;
    }

    bool WaitUnitlCompleted(CommandBuffer *command_buffer) {
        if (command_buffer->get_compute_encoder() == NULL) {
            std::cerr << "[SHIM] Nothing encoded in command buffer." << std::endl;
            return true;
        }

        WaitUntilCompletedRequest request;
        WaitUntilCompletedResponse response;
        ClientContext context;

        request.set_command_buffer_id(command_buffer->get_command_buffer_id());
        std::vector<uint32_t> buffer_ids = command_buffer->get_all_buffer_ids();
        request.mutable_buffer_ids()->Assign(buffer_ids.begin(), buffer_ids.end());

        Status status = stub_->WaitUntilCompleted(&context, request, &response);

        if (!status.ok()) {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return false;
        }

        ComputeCommandEncoder *compute_command_encoder = command_buffer->get_compute_encoder();

        size_t offset = 0;
        for (const auto &binding :
             compute_command_encoder->get_all_encoder_buffer_structs()) {
            Buffer *buffer = binding.buf;

            memcpy(buffer->contents(), response.all_buffer_data().data() + offset, buffer->length());
            offset += buffer->length();
        }

        return true;
    }

  private:
    std::unique_ptr<TnrcService::Stub> stub_;
};

// Constructed exactly once per process when its called for the first time.
// Subsequence calls returns the same client.
TnrcServiceClient &Client() {
    static TnrcServiceClient client(grpc::CreateChannel(
        "0.0.0.0:50051", grpc::InsecureChannelCredentials()));
    return client;
}
} // namespace

NS::String *Device::name() {
    return this->device_name_;
}

Device *CreateSystemDefaultDevice() {
    Device *device = Client().CreateDevice();

    if (device != NULL) {
        std::cerr << "[SHIM] Device ID: " << device->device_id() << std::endl;
        return device;
    }
    return NULL;
}

CommandQueue *Device::newCommandQueue() {
    std::optional<uint32_t> command_queue_id = Client().CreateCommandQueue(device_id_);

    if (!command_queue_id.has_value())
        return nullptr;

    std::cerr << "[SHIM] CommandQueue ID: " << command_queue_id.value() << std::endl;
    return new CommandQueue(command_queue_id.value());
}

CommandBuffer *CommandQueue::commandBuffer() {
    return (new CommandBuffer(command_queue_id_));
}

ComputeCommandEncoder *CommandBuffer::computeCommandEncoder() {
    if (compute_command_encoder_ != NULL) {
        std::cerr << "[SHIM] ERROR: Shim only supports single compute encoder per buffer for now." << std::endl;
        return NULL;
    }
    compute_command_encoder_ = new ComputeCommandEncoder();
    return compute_command_encoder_;
}

void CommandBuffer::commit() {
    bool b = Client().CommitCommandBuffer(this);
    assert(b);
}

void CommandBuffer::waitUntilCompleted() {
    bool b = Client().WaitUnitlCompleted(this);

    // ideally this function should not destroy anything.
    // we should be autorelease the compute encoder and command buffer but I cant be bothered with that as of now.
    // Since im enforcing 1 commit and 1 waitUntilCompleted this will work fine.
    if (b) {
        delete this->compute_command_encoder_;
        delete this;
    }
}
void CommandQueue::release() {
    if (Client().ReleaseCommandQueue(command_queue_id_)) {
        delete this;
    }
}

Library *Device::newLibrary(const NS::String *source, const CompileOptions *options, NS::Error **error) {
    // TODO: Change method style.
    std::optional<uint32_t> library_id = Client().CreateLibrary(device_id_, source, options, error);

    if (!library_id.has_value()) {
        return nullptr;
    }

    std::cerr << "[SHIM] Library ID: " << library_id.value() << std::endl;
    return new Library(library_id.value());
}

Function *Library::newFunction(const NS::String *function_name) {
    // TODO: Change method style.
    std::optional<uint32_t> function_id = Client().CreateFunction(library_id_, function_name);

    if (!function_id.has_value()) {
        return nullptr;
    }

    std::cerr << "[SHIM] Function ID: " << function_id.value() << std::endl;
    return new Function(function_id.value());
}

void Library::release() {
    if (Client().ReleaseLibrary(library_id_)) {
        delete this;
    }
}

void Function::release() {
    if (Client().ReleaseFunction(function_id_)) {
        delete this;
    }
}

ComputePipelineState *Device::newComputePipelineState(const Function *func, NS::Error **error) {
    ComputePipelineState *compute_pipeline_state = Client().CreateComputePipelineState(device_id_, func, error);

    if (compute_pipeline_state != NULL) {
        std::cerr << "[SHIM] ComputePipelineState ID: " << compute_pipeline_state->compute_pipeline_state_id() << std::endl;
        return compute_pipeline_state;
    }
    return nullptr;
}

NS::Integer ComputePipelineState::maxTotalThreadsPerThreadgroup() {
    return this->max_total_threads_per_threadgroup_;
}

void *Buffer::contents() {
    return buf_;
}

void Buffer::release() {
    if (Client().ReleaseBuffer(buffer_id_)) {
        free(buf_);
        delete this;
    }
}

Buffer *Device::newBuffer(NS::UInteger length, MTL::ResourceOptions options) {
    // TODO: Explore if other options are possible.
    assert(options == 0);
    Buffer *buffer = Client().CreateBuffer(device_id_, length, options);

    if (buffer != NULL) {
        std::cerr << "[SHIM] Buffer ID: " << buffer->buffer_id() << std::endl;
        return buffer;
    }
    return nullptr;
}
void ComputePipelineState::release() {
    if (Client().ReleaseComputePipelineState(compute_pipeline_state_id_)) {
        delete this;
    }
}

} // namespace MetalShim
