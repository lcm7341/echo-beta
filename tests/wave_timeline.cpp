#include "Rendering/wave_timeline.hpp"
#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

struct Event { uint32_t key,bits; bool operator==(Event const&) const = default; };
struct Target {
    std::vector<Event> events;
    void radius(float f) { events.push_back({0,std::bit_cast<uint32_t>(f)}); }
    void opacity(float f) { events.push_back({1,std::bit_cast<uint32_t>(f)}); }
    void remove() { events.push_back({2,0}); }
};
struct Request { uint32_t fused,fade,ease;float from,to;uint32_t count; };
int main(int argc,char**) {
    if(argc>1) {
        Request request;
        while(std::cin.read(reinterpret_cast<char*>(&request),sizeof(request))) {
            echo::waves::Timeline timeline;Target target;
            timeline.reset(request.from,request.to,request.fade,request.ease);
            for(uint32_t i=0;i<request.count;++i) {
                float t;std::cin.read(reinterpret_cast<char*>(&t),sizeof(t));
                if(request.fused) timeline.update<true>(t,&::powf,target);
                else timeline.update<false>(t,&::powf,target);
            }
            uint32_t count=target.events.size();
            std::cout.write(reinterpret_cast<char*>(&count),sizeof(count));
            std::cout.write(reinterpret_cast<char*>(target.events.data()),target.events.size()*sizeof(Event));
        }
        return 0;
    }
    for(bool fade:{false,true}) for(bool ease:{false,true}) {
        echo::waves::Timeline timeline;Target target;
        timeline.reset(5,30,fade,ease);
        for(float t:{0.f,0.25f,0.5f,0.75f,1.f}) timeline.update<true>(t,&::powf,target);
        assert(target.events.front().key==0 && target.events.back().key==2);
        auto before=target.events.size();timeline.update<true>(1,&::powf,target);assert(target.events.size()==before);
        timeline.reset(5,30,fade,ease);timeline.update<true>(0,&::powf,target);assert(target.events.size()>before);
    }
    std::cout<<"Wave timeline endpoints, midpoint, completion and restart checks passed.\n";
}
