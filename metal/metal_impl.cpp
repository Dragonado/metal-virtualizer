#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>

#include "metal_shim.h"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/strings/str_format.h"
#include "proto/tnrc.grpc.pb.h"
#include "proto/tnrc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::StatusCode;
using tnrc::CreateSystemDefaultDeviceShimRequest;
using tnrc::CreateSystemDefaultDeviceShimResponse;
using tnrc::TnrcService;

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");

namespace MetalShim {

class TnrcServiceClient {
  public:
    TnrcServiceClient(std::shared_ptr<Channel> channel)
        : stub_(TnrcService::NewStub(channel)) {}

    uint32_t GetDeviceId() {
        CreateSystemDefaultDeviceShimRequest request;
        CreateSystemDefaultDeviceShimResponse response;
        ClientContext context;

        Status status = stub_->CreateSystemDefaultDeviceShim(&context, request, &response);

        if (status.ok()) {
            return response.gpu_id();
        } else {
            std::cout << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return -1;
        }
    }

  private:
    std::unique_ptr<TnrcService::Stub> stub_;
};

Device *CreateSystemDefaultDevice() {
    std::string target_str = absl::GetFlag(FLAGS_target);

    TnrcServiceClient client(
        grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

    uint32_t device_id = client.GetDeviceId();
    std::cout << "GPU ID: " << device_id << std::endl;
    return new Device(device_id);
}

} // namespace MetalShim