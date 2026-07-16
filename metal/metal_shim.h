#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <iostream>

// 2. Define your Shim namespace
namespace MetalShim {
using namespace MTL;

// class CommandQueue;
// class LibraryShim;

// class CommandQueue {
//   private:
//     MTL::CommandQueue *_realQueue;

//   public:
//     CommandQueue(MTL::CommandQueue *real) : _realQueue(real) {}

//     MTL::CommandBuffer *commandBuffer() {
//         std::cout << "[SHIM] Intercepted: Creating Command Buffer" << std::endl;
//         return _realQueue->commandBuffer();
//     }

//     void release() {
//         _realQueue->release();
//     }
// };

class Device {
  private:
    uint32_t device_id_;

  public:
    Device(uint32_t device_id) : device_id_(device_id) {}

    NS::String *name();

    // CommandQueue *newCommandQueue() {
    //     std::cout << "[SHIM] Intercepted: newCommandQueue()" << std::endl;
    //     return new CommandQueue(_realDevice->newCommandQueue());
    // }

    // // Intercepting the dynamic loading of your add.metal file
    // MTL::Library *newLibrary(const NS::String *source, const MTL::CompileOptions *options, NS::Error **error) {
    //     std::cout << "[SHIM] Intercepted: Compiling dynamically loaded .metal source!" << std::endl;
    //     // You could even print/modify the 'source' string here before passing it to the real Metal API
    //     return _realDevice->newLibrary(source, options, error);
    // }

    // MTL::Buffer *newBuffer(NS::UInteger length, MTL::ResourceOptions options) {
    //     return _realDevice->newBuffer(length, options);
    // }

    // MTL::ComputePipelineState *newComputePipelineState(const MTL::Function *computeFunction, NS::Error **error) {
    //     return _realDevice->newComputePipelineState(computeFunction, error);
    // }
};

Device *CreateSystemDefaultDevice();

} // namespace MetalShim
