#include "Physics/moving_objects.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <immintrin.h>
using namespace echo::movement;
using Gate=bool (__attribute__((ms_abi)) *)(void*,void*);
using Native=void (__attribute__((ms_abi)) *)(void*,void*,double,double,bool);
Gate nativeGate;
std::vector<int> events;
int finishes=0;
bool tracing=true;
uint32_t __attribute__((ms_abi)) count(void* a) { return read<uint32_t>(read<void*>(a,0x38),0); }
void __attribute__((ms_abi)) sectionCallback(void* layer,void* object) {
    if(nativeGate(layer,object))return;
    int x,y;
    assert(section(read<double>(object,0x3b8),read<float>(layer,0x36a0),x));
    assert(section(read<double>(object,0x3c0),read<float>(layer,0x36a4),y));
    write(object,0x278,x);write(object,0x27c,y);
    if(tracing)events.push_back(read<int>(object,0x580));
}
void __attribute__((ms_abi)) finishCallback(void*,void*) {++finishes;}
void optimized(void* layer,void* array,double dx,double dy){
    move(layer,array,dx,dy,[](void* a){return count(a);},
        [](void* a){auto* d=read<void*>(a,0x38);return std::pair{read<void**>(d,0x10),read<uint32_t>(d,0)};},
        [&](void* o){sectionCallback(layer,o);},[&](void* a){finishCallback(layer,a);});
}
std::vector<char> file(char const* path){std::ifstream f(path,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
void* executable(std::vector<char> const& code){
    auto* memory=mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(memory!=MAP_FAILED);std::memcpy(memory,code.data(),code.size());return memory;
}
void relative(void* code,size_t operand,size_t next,size_t destination){write(code,operand,int32_t(destination-next));}
void protect(void* code){assert(mprotect(code,4096,PROT_READ|PROT_EXEC)==0);}
struct Fixture {
    std::array<char,0x4000> layer{};
    std::array<char,0x40> array{};
    std::array<char,0x20> data{};
    std::array<std::array<char,0x600>,256> objects{};
    std::array<void*,256> pointers{};
    std::array<void*,0xa0> table{};
    void wire(unsigned size=256){
        for(unsigned i=0;i<size;++i)pointers[i]=objects[i].data();
        table[0x490/8]=reinterpret_cast<void*>(&sectionCallback);
        table[0x498/8]=reinterpret_cast<void*>(&finishCallback);
        write(layer.data(),0,table.data());write(array.data(),0x38,data.data());
        write(data.data(),0,uint32_t(size));write(data.data(),0x10,pointers.data());
    }
};
int main(int argc,char** argv){
    assert(argc==3);
    auto moveCode=file(argv[1]),gateCode=file(argv[2]);
    assert(moveCode.size()==0x180&&gateCode.size()==0x117);
    auto* moveMemory=executable(moveCode);auto* gateMemory=executable(gateCode);
    // Relocate native count() import to the same ABI-compatible CCArray stub.
    relative(moveMemory,0x5b+2,0x5b+6,0x300);write(moveMemory,0x300,&count);
    // Turn the unchanged native section gate into a predicate: no-op returns
    // true; transition returns false. Retain the native math/branches verbatim.
    char epilogue[11];std::memcpy(epilogue,gateCode.data()+0x10c,11);
    unsigned char yes[]{0xb8,1,0,0,0};std::memcpy(static_cast<char*>(gateMemory)+0x10c,yes,5);
    std::memcpy(static_cast<char*>(gateMemory)+0x111,epilogue,11);
    unsigned char no[]{0xb8,0,0,0,0,0xe9,0,0,0,0};std::memcpy(static_cast<char*>(gateMemory)+0xa2,no,10);
    relative(gateMemory,0xa8,0xac,0x111);
    relative(gateMemory,0x31,0x35,0x180);write(gateMemory,0x180,10000000.);
    relative(gateMemory,0x39,0x3d,0x188);write(gateMemory,0x188,10000000.f);
    protect(moveMemory);protect(gateMemory);
    auto native=reinterpret_cast<Native>(moveMemory);nativeGate=reinterpret_cast<Gate>(gateMemory);
    std::mt19937 rng(2081);auto csr=_mm_getcsr();unsigned checks=0;
    Fixture first,second;
    for(unsigned mode:{0x1f80u,0x9fc0u,0x3f80u,0x5f80u,0x7f80u}){
        _mm_setcsr(mode);
        for(int n=0;n<10000;++n){
            first=Fixture{};first.wire();
            float factors[]{0.f,.01f,.1f,199.99f,200.f,200.001f};
            float factor=factors[n%6];write(first.layer.data(),0x36a0,factor);write(first.layer.data(),0x36a4,factor);
            double x=n%5==0?10000000.+int(rng()%100):int(rng()%1000000)*.001;
            if(n%7==0)x=-x;
            double y=int(rng()%1000000)*.001;
            auto* o=first.objects[0].data();write(o,0x3b8,x);write(o,0x3c0,y);
            int ix,iy;assert(section(x,factor,ix)&&section(y,factor,iy));
            write(o,0x278,ix+(n%3==0));write(o,0x27c,iy+(n%4==0));
            assert(nativeGate(first.layer.data(),o)==sectionUnchanged(first.layer.data(),o));++checks;
        }
    }
    _mm_setcsr(csr);
    // Sequential, then every size forced through the helper threads under each MXCSR mode.
    unsigned modes[]{0x1f80u,0x9fc0u,0x3f80u,0x5f80u,0x7f80u};
    for(unsigned minimum:{1000000u,1u})for(int n=0;n<2000;++n){
        parallelMinimum=minimum;if(minimum==1)_mm_setcsr(modes[n%5]);
        ++groupEpoch; // New contents in a reused array: what addToGroup reports.
        first=Fixture{};write(first.layer.data(),0x36a0,.01f);write(first.layer.data(),0x36a4,.01f);
        write(first.layer.data(),0x3e0,n%3);write(first.layer.data(),0x3724,uint32_t(0xfffffff0));
        unsigned size=n%257;
        for(unsigned i=0;i<size;++i){auto* o=first.objects[i].data();
            double x=double(int(rng()%1000000)-500000)*.001,y=double(rng()%1000000)*.001;
            write(o,0x3b8,x);write(o,0x3c0,y);write(o,0x580,int(i));
            int ix,iy;assert(section(x,.01f,ix)&&section(y,.01f,iy));
            write(o,0x278,i%17==0?-1:ix);write(o,0x27c,iy);
            write(o,0x2c8,bool(i%3==0));write(o,0x512,bool(i%5==0));write(o,0x4dc,int(i%3));
        }
        second=first;first.wire(size);second.wire(size);
        if(size>3&&n%9==0){first.pointers[3]=nullptr;second.pointers[3]=nullptr;}
        // A member listed twice is moved twice, in order; the threaded path must not split it.
        if(size>6&&n%11==0){first.pointers[size-1]=first.pointers[1];second.pointers[size-1]=second.pointers[1];}
        double dx=n%3?double(int(rng()%100)-50)*.125:0.;double dy=n%2?.013:0.;
        events.clear();finishes=0;native(first.layer.data(),first.array.data(),dx,dy,false);
        auto reference=events;assert(finishes==1);
        events.clear();finishes=0;optimized(second.layer.data(),second.array.data(),dx,dy);
        assert(events==reference&&finishes==1);
        assert(first.objects==second.objects);
        assert(read<uint32_t>(first.layer.data(),0x3724)==read<uint32_t>(second.layer.data(),0x3724));
    }
    _mm_setcsr(csr);
    std::cout<<checks<<" native section gate comparisons and 4,000 full native movement comparisons (2,000 threaded) passed\n";
}
