"""Compare production section repair with its previous implementation under ASan."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/Hooks/hooks.cpp').read_text()
a=s.index('\tvoid removeObjectFromSection(GameObject* object)',s.index('// A checkpoint restore'))
b=s.index('\n#else',a);production=s[a:b]
reference=production.replace('if (write != i) begin[write] = item;','begin[write] = item;').replace('if (write != i) objects[write] = item;','objects[write] = item;').replace('if (*index != write) *index = write;','*index = write;')
fixture=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
struct alignas(8) GameObject {std::array<uint8_t,0x600> raw{};};
struct alignas(8) GJBaseGameLayer {
 std::array<uint8_t,0x3700> raw{};int calls=0,calledID=0;
 void removeObjectFromSection(GameObject* o){++calls;std::memcpy(&calledID,o->raw.data()+0x40c,4);}
};
struct Optimized:GJBaseGameLayer {// OPTIMIZED
};
struct Original:GJBaseGameLayer {// ORIGINAL
};
template<class T>void write(void* base,size_t at,T value){std::memcpy(static_cast<uint8_t*>(base)+at,&value,sizeof(value));}
int read(GameObject const& o,size_t at){int v;std::memcpy(&v,o.raw.data()+at,4);return v;}
template<class Layer> struct Model {
 Layer layer;std::array<GameObject,40> objects{};
 std::vector<GameObject*> main,blocks;
 std::vector<void*> rows,columns,blockRows,blockColumns,countColumns,blockCountColumns;
 std::vector<int> counts,blockCounts;
 explicit Model(unsigned seed) {
  std::mt19937 rng(seed);int n=1+rng()%40;
  for(int i=0;i<n;++i){main.push_back(&objects[i]);blocks.push_back(&objects[i]);
   write(&objects[i],0x270,i);write(&objects[i],0x274,i);}
  for(int i=0;i<n;++i){if(rng()%7==0)main[i]=reinterpret_cast<GameObject*>(~uintptr_t{0});
   if(rng()%7==0)blocks[i]=nullptr;
   if(rng()%11==0)write(&objects[i],0x270,-1);
   if(rng()%11==0)write(&objects[i],0x274,-1);}
  rows={&main};columns={&rows};blockRows={&blocks};blockColumns={&blockRows};
  counts={int(rng()%(n+5))-2};blockCounts={int(rng()%(n+5))-2};
  countColumns={&counts};blockCountColumns={&blockCounts};
  auto fields=[&](size_t at,auto& v){write(&layer,at,v.data());write(&layer,at+8,v.data()+v.size());};
  fields(0x3598,columns);fields(0x3640,countColumns);
  fields(0x35c8,blockColumns);fields(0x3670,blockCountColumns);
  fields(0x3600,blocks);write(&layer,0x3618,blockCounts[0]);
  write(&objects[0],0x278,int(rng()%3)-1);write(&objects[0],0x27c,int(rng()%3)-1);
  write(&objects[0],0x40c,seed%2?0x718:1022);
  write(&objects[0],0x280,uint8_t(seed%3==0));
 }
 std::vector<int> snapshot(){
  std::vector<int> result{layer.calls,layer.calledID,counts[0],blockCounts[0]};
  int count;std::memcpy(&count,layer.raw.data()+0x3618,4);result.push_back(count);
  for(auto const& o:objects)for(size_t at:{0x270,0x274,0x278,0x27c,0x40c})result.push_back(read(o,at));
  auto* endMain=main.data()+main.size(); // main repair intentionally retains native vector end
  for(auto* p:main){auto it=std::find_if(objects.begin(),objects.end(),[&](auto const& o){return &o==p;});result.push_back(it==objects.end()?-1:int(it-objects.begin()));}
  (void)endMain;
  void* end;std::memcpy(&end,layer.raw.data()+0x3608,8);
  result.push_back(int(static_cast<GameObject**>(end)-blocks.data()));
  for(auto* p:blocks){auto it=std::find_if(objects.begin(),objects.end(),[&](auto const& o){return &o==p;});result.push_back(it==objects.end()?-1:int(it-objects.begin()));}
  return result;
 }
};
int main(){for(unsigned i=0;i<5000;++i){
 Model<Original> old(i);Model<Optimized> fast(i);
 old.layer.removeObjectFromSection(&old.objects[0]);fast.layer.removeObjectFromSection(&fast.objects[0]);
 assert(old.snapshot()==fast.snapshot());
 }
 std::cout<<"5,000 production section-repair comparisons passed (stale slots, tombstones, counts, object 1816).\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 cpp=Path(d)/'test.cpp';exe=Path(d)/'test'
 cpp.write_text(fixture.replace('// OPTIMIZED',production).replace('// ORIGINAL',reference))
 subprocess.run(['c++','-std=c++23','-O1','-g','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
