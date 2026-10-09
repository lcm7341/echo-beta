#include "Rendering/circle_geometry.hpp"
#include <bit>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
using namespace echo::circles;
unsigned calls=0;
float cosine(float angle) { ++calls; return ::cosf(angle); }
float sine(float angle) { ++calls; return ::sinf(angle); }

template<bool fused>
void reference(int slot, Point center, float radius, bool filled, std::array<Point,maxVertices>& out) {
    unsigned segments=segmentCounts[slot];
    float step=0x1.921fb6p+2f/static_cast<float>(segments);
    for(unsigned i=0;i<=segments;++i) {
        float angle=multiplyAdd<fused>(static_cast<float>(i),step,0.f);
        float x=cosine(angle),y=sine(angle);
        if(filled) out[i]={multiplyAdd<fused>(x,radius,center.x),multiplyAdd<fused>(y,radius,center.y)};
        else out[i]={multiplyAdd<fused>(x*radius,1.f,center.x),multiplyAdd<fused>(y*radius,1.f,center.y)};
    }
    out[segments+1]=center;
}
template<bool fused> void verify() {
    Geometry cache;std::mt19937 rng(2081);
    std::uniform_real_distribution<float> position(-100000.f,100000.f),radius(-500.f,500.f);
    std::array<Point,maxVertices> old{},fresh{};
    for(int n=0;n<10000;++n) {
        int slot=n%5;bool filled=n%2;Point center{position(rng),position(rng)};
        float r=radius(rng);
        if(n<20) { center={n%2?0.f:-0.f,n%2?-0.f:0.f};r=n%2?-0.f:0.f; }
        reference<fused>(slot,center,r,filled,old);
        cache.vertices<fused>(slot,center,r,filled,cosine,sine,fresh);
        assert(std::memcmp(old.data(),fresh.data(),(segmentCounts[slot]+2)*sizeof(Point))==0);
        unsigned before=calls;
        cache.vertices<fused>(slot,center,r,filled,cosine,sine,fresh);
        assert(calls==before);
    }
    assert(Geometry::index(0)==-1 && Geometry::index(10000)==-1);
}
struct Request { uint32_t fused,filled;float x,y,radius;uint32_t segments; };
int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--stream") {
        Geometry fusedCache,roundedCache;Request q;
        std::array<Point,maxVertices> out;
        while(std::cin.read(reinterpret_cast<char*>(&q),sizeof(q))) {
            int slot=Geometry::index(q.segments);assert(slot>=0);
            if(q.fused) fusedCache.vertices<true>(slot,{q.x,q.y},q.radius,q.filled,cosine,sine,out);
            else roundedCache.vertices<false>(slot,{q.x,q.y},q.radius,q.filled,cosine,sine,out);
            std::cout.write(reinterpret_cast<char*>(out.data()),(q.segments+1)*sizeof(Point));
        }
        return 0;
    }
    verify<true>();verify<false>();
    std::cout<<"20,000 circle geometries match bit-for-bit; warmed draws make zero trig calls.\n";
    if(argc>1) {
        Geometry cache;std::array<Point,maxVertices> out;volatile float sink=0;
        auto bench=[&](bool cached) {
            auto start=std::chrono::steady_clock::now();calls=0;
            for(int n=0;n<100000;++n) {
                Point center{n*0.25f,n*0.125f};float r=(n%500)*0.1f;
                if(cached) cache.vertices<false>(4,center,r,false,cosine,sine,out);
                else reference<false>(4,center,r,false,out);
                sink=out[n%51].x;
            }
            auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::cout<<(cached?"Cached":"Native algorithm")<<": "<<ms<<" ms, "<<calls<<" trig calls\n";
        };
        bench(false);bench(true);(void)sink;
    }
}
