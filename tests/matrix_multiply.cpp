#include "Rendering/matrix_multiply.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>
#include <sys/mman.h>
using Native = float* (__attribute__((ms_abi)) *)(float*,float const*,float const*);
int main(int argc,char** argv) {
    assert(argc==2);
    std::ifstream f(argv[1],std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(f)),{});
    assert(code.size()==0x555);
    void* memory=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(memory!=MAP_FAILED);
    std::memcpy(memory,code.data(),code.size());
    assert(mprotect(memory,4096,PROT_READ|PROT_EXEC)==0);
    auto native=reinterpret_cast<Native>(memory);
    std::mt19937 rng(2081);
    auto saved=_mm_getcsr();
    unsigned cases=0;
    for(unsigned mode: {0x1f80u,0x9fc0u,0x3f80u,0x5f80u,0x7f80u}) {
        for(int n=0;n<10000;++n) {
            std::array<float,64> first{},second{};
            for(auto& x:first) {
                std::uint32_t bits=rng();
                if(n%2==0) x=static_cast<float>(static_cast<int>(bits)%10000)*.001f;
                else std::memcpy(&x,&bits,4);
            }
            second=first;
            int a=0,b=16,out=(n%4==0?0:n%4==1?16:n%4==2?1:32);
            _mm_setcsr(mode);
            auto* returned=native(first.data()+out,first.data()+a,first.data()+b);
            assert(returned==first.data()+out);
            auto flags=_mm_getcsr();
            _mm_setcsr(mode);
            if(echo::matrix::supported(second.data()+a,second.data()+b))
                echo::matrix::multiply(second.data()+out,second.data()+a,second.data()+b);
            else native(second.data()+out,second.data()+a,second.data()+b);
            assert(flags==_mm_getcsr());
            assert(std::memcmp(first.data(),second.data(),sizeof(first))==0);
            ++cases;
        }
    }
    _mm_setcsr(saved);
    std::array<float,16> a{},b{},out{};
    for(int i=0;i<16;++i){a[i]=i*.07f;b[i]=i*.13f;}
    auto benchmark=[&](bool fast){
        auto start=std::chrono::steady_clock::now();
        for(int i=0;i<2000000;++i){
            a[0]=static_cast<float>(i)*.00001f;
            if(fast && echo::matrix::supported(a.data(),b.data()))echo::matrix::multiply(out.data(),a.data(),b.data());
            else native(out.data(),a.data(),b.data());
            asm volatile("" : : "m"(out) : "memory");
        }
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    auto original=benchmark(false),optimized=benchmark(true);
    std::printf("%u native result/alias/FP-status cases match bit-for-bit; %.2f ms -> %.2f ms (%.2fx)\n",cases,original,optimized,original/optimized);
    munmap(memory,4096);
}
