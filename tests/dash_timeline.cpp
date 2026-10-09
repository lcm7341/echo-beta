#include "Rendering/dash_timeline.hpp"
#include <array>
#include <cstdio>
#include <cstdint>
struct Target {
    float x,y,sx,sy; std::uint8_t alpha; bool removed=false;
    float positionX(){return x;} float positionY(){return y;}
    float scaleX(){return sx;} float scaleY(){return sy;}
    std::uint8_t opacity(){return alpha;}
    void position(float a,float b){x=a;y=b;}
    void scale(float a,float b){sx=a;sy=b;}
    void opacity(std::uint8_t a){alpha=a;}
    void remove(){removed=true;}
};
int main() {
    std::array<float,6> start; std::uint32_t opacity,n;
    while(std::fread(start.data(),sizeof(float),6,stdin)==6) {
        if(std::fread(&opacity,4,1,stdin)!=1 || std::fread(&n,4,1,stdin)!=1)return 2;
        Target target{start[0],start[1],start[2],start[3],static_cast<std::uint8_t>(opacity)};
        echo::dash::Timeline timeline; timeline.start(target,start[4],start[5]);
        for(std::uint32_t i=0;i<n;++i) {
            std::array<float,3> step;
            if(std::fread(step.data(),sizeof(float),3,stdin)!=3)return 3;
            target.x+=step[1];target.y+=step[2];
            timeline.update(step[0],target);
            std::array<float,4> state{target.x,target.y,target.sx,target.sy};
            std::array<std::uint32_t,2> flags{target.alpha,target.removed};
            std::fwrite(state.data(),sizeof(float),4,stdout);
            std::fwrite(flags.data(),4,2,stdout);
        }
    }
}
