#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <iostream>

// 2. Define your Shim namespace
namespace MetalShim {

using CompileOptions = MTL::CompileOptions;
using Size = MTL::Size;

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
    ComputePipelineState(uint32_t compute_pipeline_state_id, NS::Integer max_total_threads_per_threadgroup) : compute_pipeline_state_id_(compute_pipeline_state_id), max_total_threads_per_threadgroup_(max_total_threads_per_threadgroup) {}

    void release();

    NS::Integer maxTotalThreadsPerThreadgroup();

    uint32_t compute_pipeline_state_id() {
        return compute_pipeline_state_id_;
    }

  private:
    uint32_t compute_pipeline_state_id_;
    NS::Integer max_total_threads_per_threadgroup_;
};

class ComputeCommandEncoder {
  public:
    void setComputePipelineState(ComputePipelineState *compute_pipeline_state);
    void dispatchThreads(Size grid_size, Size thread_group_size);
    void endEncoding();

  private:
};

class CommandBuffer {
  public:
    CommandBuffer(uint32_t command_queue_id) : command_queue_id_(command_queue_id) {}

    ComputeCommandEncoder *computeCommandEncoder();

    void commit();

    void waitUntilCompleted();

  private:
    uint32_t command_queue_id_;
};

class CommandQueue {
  public:
    CommandQueue(uint32_t command_queue_id) : command_queue_id_(command_queue_id) {}

    CommandBuffer *commandBuffer();
    void release();

  private:
    uint32_t command_queue_id_;
};

class Device {

  public:
    Device(uint32_t device_id, NS::String *device_name) : device_id_(device_id), device_name_(device_name) {}

    NS::String *name();

    CommandQueue *newCommandQueue();

    // Intercepting the dynamic loading of your add.metal file
    Library *newLibrary(const NS::String *source, const CompileOptions *options, NS::Error **error);
    ComputePipelineState *newComputePipelineState(const Function *func, NS::Error **error);

    // MTL::ComputePipelineState *newComputePipelineState(const MTL::Function *computeFunction, NS::Error **error) {
    //     return _realDevice->newComputePipelineState(computeFunction, error);
    // }

    uint32_t device_id() {
        return device_id_;
    }

  private:
    uint32_t device_id_;
    NS::String *device_name_;
};

Device *CreateSystemDefaultDevice();

} // namespace MetalShim
