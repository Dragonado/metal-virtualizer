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

    bool CommitCommandBuffer(CommandBuffer *command_buffer) {
        // TODO: make rpc call.
        return true;
    }

  private:
    std::unique_ptr<TnrcService::Stub> stub_;
};

// Constructed exactly once per process when its called for the first time.
// Subsequence calls returns the same client.
TnrcServiceClient &Client() {
    static TnrcServiceClient client(grpc::CreateChannel(
        "localhost:50051", grpc::InsecureChannelCredentials()));
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

// TODO: Make RPC call.
void CommandBuffer::commit() {
    assert(Client().CommitCommandBuffer(this));
}

// TODO: Block until we get response from RPC.
void CommandBuffer::waitUntilCompleted() {
}
// TODO: Make rpc call.
void CommandQueue::release() {
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

// TODO: Make rpc call.
void Library::release() {}

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
    free(buf_);
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
// TODO: Make rpc call.
void ComputePipelineState::release() {}

} // namespace MetalShim
