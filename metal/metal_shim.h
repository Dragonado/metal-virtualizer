#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <iostream>

// 2. Define your Shim namespace
namespace MetalShim {

using CompileOptions = MTL::CompileOptions;

class Function {
  public:
    Function(uint32_t function_id) : function_id_(function_id) {}

    uint32_t get_function_id() const {
        return function_id_;
    }
    void release();

  private:
    uint32_t function_id_;
};

class Library {
  public:
    Library(uint32_t library_id) : library_id_(library_id) {}

    Function *newFunction(const NS::String *function_name);

    void release();

  private:
    uint32_t library_id_;
};

class ComputePipelineState {
  public:
    ComputePipelineState(uint32_t compute_pipeline_state_id) : compute_pipeline_state_id_(compute_pipeline_state_id) {}

    void release();

  private:
    uint32_t compute_pipeline_state_id_;
};

class CommandQueue {
  public:
    CommandQueue(uint32_t command_queue_id) : command_queue_id_(command_queue_id) {}

    void release();

  private:
    uint32_t command_queue_id_;
};

class Device {
  private:
    uint32_t device_id_;

  public:
    Device(uint32_t device_id) : device_id_(device_id) {}

    NS::String *name();

    CommandQueue *newCommandQueue();

    // Intercepting the dynamic loading of your add.metal file
    Library *newLibrary(const NS::String *source, const CompileOptions *options, NS::Error **error);
    ComputePipelineState *newComputePipelineState(const Function *func, NS::Error **error);

    // MTL::ComputePipelineState *newComputePipelineState(const MTL::Function *computeFunction, NS::Error **error) {
    //     return _realDevice->newComputePipelineState(computeFunction, error);
    // }
};

Device *CreateSystemDefaultDevice();

} // namespace MetalShim
