#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "proto/tnrc.grpc.pb.h"
#include "proto/tnrc.pb.h"

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");

using grpc::Channel;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using tnrc::HelloRequest;
using tnrc::HelloResponse;
using tnrc::TnrcService;

class TnrcServiceClient {
  public:
    TnrcServiceClient(std::shared_ptr<Channel> channel)
        : stub_(TnrcService::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    std::string SayHello(const std::string &user) {
        // Data we are sending to the server.
        HelloRequest request;
        request.set_name(user);

        // Container for the data we expect from the server.
        HelloResponse response;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->Hello(&context, request, &response);

        // Act upon its status.
        if (status.ok()) {
            return response.message();
        } else {
            std::cout << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return "RPC failed";
        }
    }

  private:
    std::unique_ptr<TnrcService::Stub> stub_;
};

int main(int argc, char **argv) {
    absl::ParseCommandLine(argc, argv);

    std::string target_str = absl::GetFlag(FLAGS_target);
    TnrcServiceClient greeter(
        grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));
    std::string user("world");
    std::string reply = greeter.SayHello(user);
    std::cout << "Greeter received: " << reply << std::endl;

    return 0;
}