"""Exercise the exact production path reducer at high TPS; verify every sample."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r'''
#include "Hack/trajectory_path.hpp"
#include <cassert>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iostream>
struct Point { float x,y; };
using Path=std::vector<Point>;
void check(Path const& input, double tolerance, size_t maxSegments) {
    std::vector<std::pair<Point,Point>> segments;
    auto start=std::chrono::steady_clock::now();
    echo::trajectory::drawPath(input,[&](Point a,Point b){segments.emplace_back(a,b);});
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    assert(segments.size()<=maxSegments);
    if(input.size()<2) { assert(segments.empty()); return; }
    assert(segments.front().first.x==input.front().x && segments.front().first.y==input.front().y);
    assert(segments.back().second.x==input.back().x && segments.back().second.y==input.back().y);
    size_t cursor=0;
    for(auto [a,b]:segments) {
        assert(a.x==input[cursor].x && a.y==input[cursor].y);
        double dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
        do {
            auto p=input[cursor];
            double x=p.x-a.x,y=p.y-a.y;
            double t=length?std::clamp((x*dx+y*dy)/length,0.,1.):0.;
            assert(std::abs(x*dy-y*dx)<=tolerance);
            assert(t>=0 && t<=1);
            if(cursor>0 && p.x==b.x && p.y==b.y) break;
            ++cursor;
            assert(cursor<input.size());
        } while(true);
    }
    std::cout<<input.size()-1<<" ticks -> "<<segments.size()<<" segments, reduction "
             <<double(input.size()-1)/segments.size()<<"x, reducer "<<us<<" us\n";
}
int main() {
    assert(echo::trajectory::predictionSteps(1.f,31010.f,1.f)==31010);
    assert(echo::trajectory::predictionSteps(1.f,31010.f,.5f)==62020);
    assert(echo::trajectory::predictionSteps(0.f,31010.f,1.f)==1);
    check({},.05,0);check({{0,0}},.05,0);
    check({{0,0},{0,0},{0,0}},.05,1);
    check({{0,0},{10,0},{0,0},{-10,0}},.05,3); // reversing wave
    check({{0,0},{1,0},{1,10},{2,10}},.05,3); // portal/corner
    for(int tps:{240,1000,10000,20000,31010}) {
        Path straight,curve,wave;
        for(int i=0;i<=tps;++i) {
            float t=float(i)/tps;
            straight.push_back({600*t,50.f});
            curve.push_back({600*t,300*t*t});
            wave.push_back({600*t,100.f*std::abs(std::sin(t*12))});
        }
        check(straight,0.,1);check(curve,0.,tps);check(wave,0.,tps);
    }
    Path zigzag;
    for(int i=0;i<1000;++i)zigzag.push_back({float(i),float(i%2)});
    check(zigzag,0.,999); // zero tolerance keeps all corners
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test'
    cpp.write_text(source)
    subprocess.run(['c++','-std=c++20','-O2','-I',str(root/'src'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
