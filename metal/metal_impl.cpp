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
using tnrc::CreateSystemDefaultDeviceShimRequest;
using tnrc::CreateSystemDefaultDeviceShimResponse;
using tnrc::GetDeviceNameShimRequest;
using tnrc::GetDeviceNameShimResponse;
using tnrc::TnrcService;

namespace MetalShim {

namespace {
class TnrcServiceClient {
  public:
    TnrcServiceClient(std::shared_ptr<Channel> channel)
        : stub_(TnrcService::NewStub(channel)) {}

    std::optional<uint32_t> CreateDeviceId() {
        CreateSystemDefaultDeviceShimRequest request;
        CreateSystemDefaultDeviceShimResponse response;
        ClientContext context;

        Status status = stub_->CreateSystemDefaultDeviceShim(&context, request, &response);

        if (status.ok()) {
            return response.gpu_id();
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return std::nullopt;
        }
    }

    NS::String *GetDeviceName(uint32_t device_id) {
        GetDeviceNameShimRequest request;
        GetDeviceNameShimResponse response;
        ClientContext context;

        request.set_device_id(device_id);

        Status status = stub_->GetDeviceNameShim(&context, request, &response);

        if (status.ok()) {
            return NS::String::string(response.name().c_str(), NS::UTF8StringEncoding);
        } else {
            std::cerr << "[SHIM] ERROR: " << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return nullptr;
        }
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
    return Client().GetDeviceName(device_id_);
}

Device *CreateSystemDefaultDevice() {
    std::optional<uint32_t> device_id = Client().CreateDeviceId();

    if (!device_id.has_value())
        return nullptr;

    std::cerr << "[SHIM] GPU ID: " << device_id.value() << std::endl;
    return new Device(device_id.value());
}

} // namespace MetalShim
