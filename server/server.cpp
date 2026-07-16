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
        return Status::OK;
    }

    Status GetDeviceNameShim(ServerContext *context, const GetDeviceNameShimRequest *request, GetDeviceNameShimResponse *response) override {
        if (device_map_.find(request->device_id()) == device_map_.end()) {
            return Status(StatusCode::INTERNAL, "Could not find metal device.");
        }
        MTL::Device *device = device_map_[request->device_id()];
        response->set_name(device->name()->cString(NS::UTF8StringEncoding));
        return Status::OK;
    }

    Status CreateCommandQueueShim(ServerContext *context, const CreateCommandQueueShimRequest *request, CreateCommandQueueShimResponse *response) override {
        MTL::CommandQueue *command_queue = device_map_[request->device_id()]->newCommandQueue();
        if (command_queue == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create command queue.");
        }
        counter_++;
        command_queue_map_[counter_] = command_queue;
        response->set_command_queue_id(counter_);
        return Status::OK;
    }

    Status CreateLibraryShim(ServerContext *context, const CreateLibraryShimRequest *request, CreateLibraryShimResponse *response) override {
        NS::Error *err;
        MTL::Library *library = device_map_[request->device_id()]->newLibrary(
            NS::String::string(request->source().c_str(), NS::UTF8StringEncoding), nullptr, &err);
        if (library == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create library");
        }
        counter_++;
        library_map_[counter_] = library;
        response->set_library_id(counter_);
        return Status::OK;
    }

    Status CreateFunctionShim(ServerContext *context, const CreateFunctionShimRequest *request, CreateFunctionShimResponse *response) override {
        MTL::Function *function = library_map_[request->library_id()]->newFunction(
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
        auto function = function_map_.find(request->function_id());
        if (function == function_map_.end()) {
            return Status(StatusCode::NOT_FOUND, "Could not find function.");
        }

        function->second->release();
        function_map_.erase(function);
        return Status::OK;
    }

    Status CreateComputePipelineStateShim(ServerContext *context, const CreateComputePipelineStateShimRequest *request, CreateComputePipelineStateShimResponse *response) override {
        NS::Error *err;
        MTL::ComputePipelineState *compute_pipeline_state = device_map_[request->device_id()]->newComputePipelineState(
            function_map_[request->function_id()], &err);
        if (compute_pipeline_state == nullptr) {
            return Status(StatusCode::INTERNAL, "Could not create compute_pipeline_state");
        }
        counter_++;
        compute_pipeline_state_map_[counter_] = compute_pipeline_state;
        response->set_compute_pipeline_state_id(counter_);
        return Status::OK;
    }

    ~ShimmerImpl() {
        for (auto &[id, device] : device_map_) {
            if (device != nullptr)
                device->release();
        }
    }

  private:
    uint32_t counter_;
    std::map<uint32_t, MTL::Device *> device_map_;
    std::map<uint32_t, MTL::CommandQueue *> command_queue_map_;
    std::map<uint32_t, MTL::Library *> library_map_;
    std::map<uint32_t, MTL::Function *> function_map_;
    std::map<uint32_t, MTL::ComputePipelineState *> compute_pipeline_state_map_;
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
