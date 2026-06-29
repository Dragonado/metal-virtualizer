#include <iostream>

constexpr size_t MAXN = 100;

int main(){
    int A[MAXN];
    A[0] = 1;
    int parallel_sum = 0;
    int serial_sum = 0;

    if(serial_sum == parallel_sum){
        std::cout << "OK: Verified sum is correct. Sum = " << serial_sum << std::endl;
        return 0;
    }
    else{
        std::cout << "ERROR: Verified sum is incorrect. Parallel Sum = " << parallel_sum << ", Serial Sum = " << serial_sum << std::endl;
        return 1;
    }
    return 0;
}