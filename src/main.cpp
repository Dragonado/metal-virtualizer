#include <fstream>
#include <iostream>
#include <random> // Required header

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include "Foundation/Foundation.hpp"
#include "Metal/Metal.hpp"

constexpr size_t MAXN = 100;
constexpr float EPS = 1e-5;

// class Adder {
//   public:
//     Adder *initWithDevice(MTL::Device *device) {
//         device_ = device;
//         buffer_a_ = device_->newBuffer(MAXN * sizeof(float), 0);
//         buffer_b_ = device_->newBuffer(MAXN * sizeof(float), 0);
//         buffer_c_ = device_->newBuffer(MAXN * sizeof(float), 0);

//         queue_ = device_->newCommandQueue();

//         NS::Error *err;

//         // Metal code is added at run time. Under `bazel run` the cwd is the
//         // runfiles root, so the shader sits at metal/add.metal; fall back to
//         // the bare name when running from inside metal/ directly.
//         std::ifstream f("src/add.metal");
//         if (!f.is_open()) {
//             f.open("add.metal");
//         }
//         if (!f.is_open()) {
//             std::cerr << "ERROR: cannot find add.metal" << '\n';
//             exit(1);
//         }
//         std::string src((std::istreambuf_iterator<char>(f)), {});
//         MTL::Library *library = device_->newLibrary(
//             NS::String::string(src.c_str(), NS::UTF8StringEncoding), nullptr, &err);

//         if (library == NULL) {
//             std::cerr << "ERROR: library compile failed: "
//                       << err->localizedDescription()->utf8String() << '\n';
//             exit(1);
//         }

//         MTL::Function *add_func = library->newFunction(NS::String::string("add_arrays", NS::UTF8StringEncoding));

//         if (add_func == NULL) {
//             std::cerr << "ERROR: Failed to find add function in the metal code: " << std::endl;
//             exit(1);
//         }

//         add_pso_ = device_->newComputePipelineState(add_func, &err);

//         if (add_pso_ == NULL) {
//             std::cerr << "ERROR: Failed to create pipeline state object: "
//                       << err->localizedDescription()->utf8String() << '\n';
//             exit(1);
//         }

//         add_func->release();
//         library->release();

//         return this;
//     }

//     static Adder *create(MTL::Device *device) {
//         return (new Adder())->initWithDevice(device);
//     }

//     void sendComputeCommand() {
//         MTL::CommandBuffer *command_buffer = queue_->commandBuffer();
//         MTL::ComputeCommandEncoder *encoder = command_buffer->computeCommandEncoder();

//         encodeAddCommand(encoder);

//         command_buffer->commit();

//         command_buffer->waitUntilCompleted();
//     }

//     void encodeAddCommand(MTL::ComputeCommandEncoder *encoder) {
//         encoder->setComputePipelineState(add_pso_);
//         encoder->setBuffer(buffer_a_, 0, 0);
//         encoder->setBuffer(buffer_b_, 0, 1);
//         encoder->setBuffer(buffer_c_, 0, 2);

//         MTL::Size grid_size = MTL::Size(MAXN, 1, 1);
//         NS::Integer tg_size = add_pso_->maxTotalThreadsPerThreadgroup();
//         if (tg_size > (int)MAXN) {
//             tg_size = MAXN;
//         }
//         MTL::Size thread_group_size = MTL::Size(tg_size, 1, 1);

//         encoder->dispatchThreads(grid_size, thread_group_size);

//         encoder->endEncoding();
//     }

//     void prepareData() {
//         populate_random_float(buffer_a_, MAXN);
//         populate_random_float(buffer_b_, MAXN);
//     }

//     ~Adder() {
//         buffer_a_->release();
//         buffer_b_->release();
//         buffer_c_->release();
//         add_pso_->release();
//         queue_->release();
//     }

//     void verify() {
//         float *a = (float *)buffer_a_->contents();
//         float *b = (float *)buffer_b_->contents();
//         float *c = (float *)buffer_c_->contents();

//         std::cout << "First 10 elements of each: " << std::endl;
//         for (size_t i = 0; i < 10; i++) {
//             std::cout << a[i] << " ";
//         }
//         std::cout << std::endl;
//         for (size_t i = 0; i < 10; i++) {
//             std::cout << b[i] << " ";
//         }
//         std::cout << std::endl;
//         for (size_t i = 0; i < 10; i++) {
//             std::cout << c[i] << " ";
//         }
//         std::cout << std::endl;

//         for (size_t i = 0; i < MAXN; i++) {
//             if (fabs(c[i] - a[i] - b[i]) > EPS) {
//                 std::cerr << "ERROR: Wrong answer!" << std::endl;
//                 exit(1);
//             }
//         }
//         std::cout << "OK: Computation is correct" << std::endl;
//     }

//   private:
//     void populate_random_float(MTL::Buffer *b, size_t n) {
//         std::random_device rd;
//         std::mt19937 gen(rd());
//         float min_val = 1.0f;
//         float max_val = 10.0f;
//         std::uniform_real_distribution<float> dis(min_val, max_val);

//         float *content = (float *)b->contents();
//         for (size_t i = 0; i < n; i++) {
//             content[i] = dis(gen);
//         }
//     }

//     MTL::Device *device_;

//     MTL::ComputePipelineState *add_pso_;
//     MTL::CommandQueue *queue_;
//     MTL::Buffer *buffer_a_, *buffer_b_, *buffer_c_;
// };

int main() {

    MTL::Device *device = MTL::CreateSystemDefaultDevice();
    std::cout << "Name = " << device->name() << std::endl;
    assert(device != nullptr);
    // Adder *adder = Adder::create(device);
    // adder->prepareData();
    // adder->sendComputeCommand();

    // adder->verify();

    return 0;
}