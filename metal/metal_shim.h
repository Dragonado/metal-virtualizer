#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <iostream>

// 2. Define your Shim namespace
namespace MetalShim {
using namespace MTL;

class CommandQueue;
class LibraryShim;

class CommandQueue {
  private:
    MTL::CommandQueue *_realQueue;

  public:
    CommandQueue(MTL::CommandQueue *real) : _realQueue(real) {}

    MTL::CommandBuffer *commandBuffer() {
        std::cout << "[SHIM] Intercepted: Creating Command Buffer" << std::endl;
        return _realQueue->commandBuffer();
    }

    void release() {
        _realQueue->release();
    }
};

class Device {
  private:
    MTL::Device *_realDevice;

  public:
    Device(MTL::Device *real) : _realDevice(real) {}

    CommandQueue *newCommandQueue() {
        std::cout << "[SHIM] Intercepted: newCommandQueue()" << std::endl;
        return new CommandQueue(_realDevice->newCommandQueue());
    }

    // Intercepting the dynamic loading of your add.metal file
    MTL::Library *newLibrary(const NS::String *source, const MTL::CompileOptions *options, NS::Error **error) {
        std::cout << "[SHIM] Intercepted: Compiling dynamically loaded .metal source!" << std::endl;
        // You could even print/modify the 'source' string here before passing it to the real Metal API
        return _realDevice->newLibrary(source, options, error);
    }

    MTL::Buffer *newBuffer(NS::UInteger length, MTL::ResourceOptions options) {
        return _realDevice->newBuffer(length, options);
    }

    MTL::ComputePipelineState *newComputePipelineState(const MTL::Function *computeFunction, NS::Error **error) {
        return _realDevice->newComputePipelineState(computeFunction, error);
    }
    // Add other device methods your adder uses (like newBufferWithLength)...
};

inline Device *
CreateSystemDefaultDevice() {
    std::cout << "[SHIM] Intercepted: MTL::CreateSystemDefaultDevice()" << std::endl;
    MTL::Device *realDevice = MTL::CreateSystemDefaultDevice();
    return new Device(realDevice);
}

} // namespace MetalShim

// 3. The Hijack: Replace the MTL namespace with your Shim namespace.
// Any code in adder.cpp below this point will use MetalShim instead of MTL.
#define MTL MetalShim