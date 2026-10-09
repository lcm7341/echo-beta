#include "Logic/child_type_cache.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <random>
#include <span>
struct Object { uint32_t id; bool pause; };
int main() {
    std::mt19937 rng(2081);
    std::array<Object, 100> objects{};
    for (unsigned i=0;i<objects.size();++i) objects[i]={i+1,false};
    std::vector<Object*> children;
    replay::ChildTypeCache<Object> cache;
    uint32_t next=100;
    size_t casts=0;
    auto query=[&] {
        bool want=std::any_of(children.begin(),children.end(),[](auto* o){return o && o->pause;});
        bool got=cache.contains(children,[](auto* o){return o->id;},[&](auto* o){++casts;return o->pause;});
        assert(got==want);
    };
    for(int test=0;test<20000;++test) {
        switch(rng()%6) {
        case 0: children.push_back(&objects[rng()%objects.size()]); break;
        case 1: if(!children.empty()) children.erase(children.begin()+rng()%children.size()); break;
        case 2: std::shuffle(children.begin(),children.end(),rng); break;
        case 3: { auto& o=objects[rng()%objects.size()];o={++next,(rng()%10)==0};break; } // same address, new lifetime
        case 4: if(!children.empty()) children[rng()%children.size()]=nullptr; break;
        case 5: if(test%31==0) children.clear();break;
        }
        query(); auto before=casts; for(int i=0;i<5;++i) query(); assert(casts==before);
    }
    children.clear();for(auto& o:objects){o={++next,false};children.push_back(&o);}
    query();auto before=casts;for(int i=0;i<10000;++i) query();
    assert(casts==before);
    std::cout<<"20,000 graph/lifetime cases passed; 1,000,000 repeated type casts avoided.\n";
}
