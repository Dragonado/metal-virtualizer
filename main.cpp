#include <iostream>
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include "Metal/Metal.hpp"

// constexpr size_t MAXN = 100;

int main() {
    NS::AutoreleasePool *pPool = NS::AutoreleasePool::alloc()->init();

    NS::String *pString = NS::String::string("Hello World", NS::ASCIIStringEncoding);
    printf("pString = \"%s\"\n", pString->cString(NS::ASCIIStringEncoding));

    pPool->release();

    // int parallel_sum = 0;
    // int serial_sum = 0;

    // if (serial_sum == parallel_sum) {
    //     std::cout << "OK: Verified sum is correct. Sum = " << serial_sum << std::endl;
    //     return 0;
    // } else {
    //     std::cout << "ERROR: Verified sum is incorrect. Parallel Sum = " << parallel_sum << ", Serial Sum = " << serial_sum << std::endl;
    //     return 1;
    // }
    return 0;
}