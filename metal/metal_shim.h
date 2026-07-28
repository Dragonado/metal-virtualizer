#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>

// 2. Define your Shim namespace
namespace MetalShim {

using CompileOptions = MTL::CompileOptions;
using ResourceOptions = MTL::ResourceOptions;
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

class Buffer {
  public:
    Buffer(uint32_t buffer_id, NS::UInteger length, ResourceOptions)
        : buf_(std::malloc(length)), length_(length), buffer_id_(buffer_id) {
        if (buf_ == nullptr && length != 0) {
            throw std::bad_alloc();
        }
        if (length != 0) {
            std::memset(buf_, 0, length);
        }
    }
    void *contents();

    void release();

    NS::UInteger length() {
        return length_;
    }

    uint32_t buffer_id() {
        return buffer_id_;
    }

  private:
    void *buf_;
    NS::UInteger length_;
    uint32_t buffer_id_;
};

class ComputeCommandEncoder {
    struct encoderBufferStruct {
        Buffer *buf;
        NS::UInteger offset;
        NS::UInteger index;
    };

  public:
    ComputeCommandEncoder()
        : end_encoding_(false), compute_pipeline_state_(nullptr), grid_size_{},
          thread_group_size_{} {}

    void setComputePipelineState(ComputePipelineState *compute_pipeline_state) {
        if (end_encoding_) {
            std::cerr << "[SHIM] ERROR: Cannot change an encoder after endEncoding()." << std::endl;
            return;
        }
        compute_pipeline_state_ = compute_pipeline_state;
    }
    void dispatchThreads(Size grid_size, Size thread_group_size) {
        if (end_encoding_) {
            std::cerr << "[SHIM] ERROR: Cannot dispatch after endEncoding()." << std::endl;
            return;
        }

        // TODO: For now supporting only 1D.
        if (grid_size.height != 1 || grid_size.depth != 1 ||
            thread_group_size.height != 1 || thread_group_size.depth != 1) {
            std::cerr << "[SHIM] ERROR: Shim currently supports only 1D dispatches." << std::endl;
            return;
        }

        grid_size_ = grid_size;
        thread_group_size_ = thread_group_size;
    }

    void setBuffer(Buffer *buf, NS::UInteger offset, NS::UInteger index) {
        if (end_encoding_) {
            std::cerr << "[SHIM] ERROR: Cannot bind a buffer after endEncoding()." << std::endl;
            return;
        }
        encoder_buffer_structs_.push_back({buf, offset, index});
    }

    void endEncoding() {
        end_encoding_ = true;
    }

    ComputePipelineState *get_compute_pipeline_state() {
        return compute_pipeline_state_;
    }

    const std::vector<encoderBufferStruct> &get_all_encoder_buffer_structs() const {
        return encoder_buffer_structs_;
    }

    Size get_grid_size() {
        return grid_size_;
    }

    Size get_thread_group_size() {
        return thread_group_size_;
    }

  private:
    bool end_encoding_;
    ComputePipelineState *compute_pipeline_state_;
    Size grid_size_;
    Size thread_group_size_;
    std::vector<encoderBufferStruct> encoder_buffer_structs_;
};

class CommandBuffer {
  public:
    CommandBuffer(uint32_t command_queue_id) : command_queue_id_(command_queue_id) {
        command_buffer_id_ = 0;
        compute_command_encoder_ = NULL;
    }

    ComputeCommandEncoder *computeCommandEncoder();

    uint32_t get_command_queue_id() {
        return command_queue_id_;
    }

    ComputeCommandEncoder *get_compute_encoder() {
        return compute_command_encoder_;
    }

    void commit();

    // The current MVP supports exactly one wait call. The client proxy still
    // requires bound buffers to remain alive until this call completes.
    void waitUntilCompleted();

    uint32_t get_command_buffer_id() {
        return command_buffer_id_;
    }

    std::vector<uint32_t> get_all_buffer_ids() {
        if (compute_command_encoder_ == NULL)
            return {};
        std::vector<uint32_t> buffer_ids;
        for (auto encoder_buffer_struct : compute_command_encoder_->get_all_encoder_buffer_structs()) {
            buffer_ids.push_back(encoder_buffer_struct.buf->buffer_id());
        }
        return buffer_ids;
    }

    void set_command_buffer_id(uint32_t command_buffer_id) {
        command_buffer_id_ = command_buffer_id;
    }

  private:
    uint32_t command_queue_id_;
    // little bit different from others because its not really minted like others. Its more of a job id.
    uint32_t command_buffer_id_;
    ComputeCommandEncoder *compute_command_encoder_;
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
    Device(uint32_t device_id, NS::String *device_name) : device_id_(device_id), device_name_(device_name) {
        device_name_->retain();
    }

    ~Device() {
        device_name_->release();
    }

    NS::String *name();

    CommandQueue *newCommandQueue();

    // Intercepting the dynamic loading of your add.metal file
    Library *newLibrary(const NS::String *source, const CompileOptions *options, NS::Error **error);
    ComputePipelineState *newComputePipelineState(const Function *func, NS::Error **error);

    Buffer *newBuffer(NS::UInteger length, MTL::ResourceOptions options);
    void release();
    uint32_t device_id() {
        return device_id_;
    }

  private:
    uint32_t device_id_;
    NS::String *device_name_;
};

Device *CreateSystemDefaultDevice();

} // namespace MetalShim
