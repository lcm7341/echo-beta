"""Compile production circle draw hooks against a GL recorder, without a driver."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/Rendering/orb_circles.cpp').read_text()
body=source[source.index('namespace {'):source.index('class $modify(')]
fixture=r'''
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Rendering/circle_geometry.hpp"
using GLint=int;
struct CCPoint { float x,y; };
constexpr int GL_FLOAT=0x1406,GL_FALSE=0,GL_TRIANGLE_FAN=6,GL_LINE_STRIP=3;
constexpr int GL_ONE=1,GL_SRC_ALPHA=0x0302;
// Merged rings switch to additive blending and back (orb_circles.cpp).
int blendSource=GL_SRC_ALPHA,blendDestination=GL_ONE;
void ccGLBlendFunc(int source,int destination) { blendSource=source;blendDestination=destination; }
constexpr int kCCVertexAttrib_Position=0,kCCVertexAttribFlag_Position=1;
int draws=0,uses=0,transforms=0,uniforms=0,original=0,enabled=0;
int mode=-1,count=0,location=-1;std::array<float,4> uploaded;
void const* vertexPointer=nullptr;std::vector<echo::circles::Point> captured;
struct CCGLProgram {
 void use() { ++uses; }
 void setUniformsForBuiltins() { ++transforms; }
 void setUniformLocationWith4fv(int loc,float* color,unsigned n) {
  assert(n==1);++uniforms;location=loc;std::copy_n(color,4,uploaded.begin());
 }
};
void ccGLEnableVertexAttribs(int flags) { assert(flags==1);++enabled; }
void glVertexAttribPointer(int index,int components,int type,int normalized,int stride,void const* data) {
 assert(index==0 && components==2 && type==GL_FLOAT && normalized==0 && stride==0);
 vertexPointer=data;
}
void glDrawArrays(int drawMode,int first,int n) {
 assert(first==0);mode=drawMode;count=n;
 auto* p=static_cast<echo::circles::Point const*>(vertexPointer);captured.assign(p,p+n);
}
#define CC_INCREMENT_GL_DRAWS(n) draws+=n
// PRODUCTION
void originalCircle(CCPoint const&,float,float,unsigned,bool,float,float) { ++original; }
void originalFilled(CCPoint const&,float,float,unsigned) { ++original; }
int main() {
 CCGLProgram program;CCGLProgram* ptr=&program;shader=&ptr;
 std::array<float,4> color{0.25f,0.5f,0.75f,1};drawColor=color.data();
 int uniform=71;colorUniform=&uniform;
 nativeCosine=::cosf;nativeSine=::sinf;
 nativeCircle=originalCircle;nativeFilledCircle=originalFilled;
 CCPoint center{30,60};drawingWave=0;
 cachedCircle(center,50,0,50,false,1,1);assert(original==1 && draws==0);
 drawingWave=1;
 cachedCircle(center,50,0,50,false,1,1);
 assert(original==1 && draws==1 && uses==1 && transforms==1 && uniforms==1 && enabled==1);
 assert(mode==GL_LINE_STRIP && count==51 && location==71 && uploaded==color);
 assert(captured.front().x==80 && captured.front().y==60);
 cachedFilledCircle(center,50,0,50);assert(original==1 && draws==2 && mode==GL_TRIANGLE_FAN && count==51);
 color[3]=0;cachedCircle(center,50,0,50,false,1,1);assert(original==1 && draws==2);
 cachedFilledCircle(center,50,0,50);assert(draws==3 && original==1);
 color[3]=1;
 cachedCircle(center,50,-0.f,50,false,1,1);
 cachedCircle(center,50,0,50,true,1,1);
 cachedCircle(center,50,0,50,false,2,1);
 cachedCircle(center,50,0,51,false,1,1);
 cachedCircle(center,NAN,0,50,false,1,1);
 cachedFilledCircle(center,50,0,51);
 cachedFilledCircle(center,50,0.1f,50);
 assert(original==8 && draws==3);
 ptr=nullptr;cachedCircle(center,50,0,50,false,1,1);assert(original==9 && draws==3);
}
'''
fixture='#include <algorithm>\n'+fixture
with tempfile.TemporaryDirectory(prefix='echo-renderer-tests-') as d:
    cpp=Path(d)/'test.cpp';cpp.write_text(fixture.replace('// PRODUCTION',body))
    for platform in [[],['-DGEODE_IS_ANDROID64']]:
        binary=Path(d)/('test-arm' if platform else 'test-win')
        subprocess.run(['c++','-std=c++23','-O1','-g','-Wall','-Wextra','-Werror','-ffp-contract=off',
                        '-fsanitize=address,undefined','-Isrc',*platform,str(cpp),'-o',str(binary)],cwd=root,check=True)
        subprocess.run([str(binary)],check=True)
print('Production circle hooks preserve GL mode, uniforms, vertex count, alpha behavior and fallback paths in both profiles.')
