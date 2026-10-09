"""Compile the production selection tracking and compare to the previous search."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[1]
s=(root/'src/GUI/tabs.cpp').read_text();s=s[s.index('void GUI::followSelection()'):s.index('void strokeSelectedButton()')]
reference=re.sub(r'#ifdef GEODE_IS_WINDOWS.*?#endif','',s,flags=re.S).replace('GUI::followSelection','Reference::followSelection')
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>
struct Logic {
 std::vector<int> inputs;std::vector<uint64_t> ids;size_t reads=0;
 static Logic& get(){static Logic l;return l;}
 uint64_t inputId(size_t i){++reads;return i<ids.size()?ids[i]:0;}
};
struct GUI {
 int selected=-1,anchor=-1,draft=0;uint64_t selectedId=0,anchorId=0;
 void followSelection();
 bool operator==(GUI const&)const=default;
};
struct Reference:GUI {void followSelection();};
// FUNCTIONS
int main(){
 auto& l=Logic::get();std::mt19937 rng(2081);uint64_t next=0;
 for(int test=0;test<10000;++test){
  l.inputs.resize(rng()%200);std::iota(l.inputs.begin(),l.inputs.end(),0);
  l.ids.resize(l.inputs.size());for(auto& id:l.ids)id=++next;
  GUI g;g.selected=int(rng()%(l.inputs.size()+5))-2;g.anchor=int(rng()%(l.inputs.size()+5))-2;
  g.selectedId=g.selected>=0&&size_t(g.selected)<l.ids.size()?l.ids[g.selected]:++next;
  g.anchorId=g.anchor>=0&&size_t(g.anchor)<l.ids.size()?l.ids[g.anchor]:++next;
  if(test%3==0)g.selectedId=0;if(test%7==0)g.anchorId=0;
  Reference ref;static_cast<GUI&>(ref)=g;
  if(test%4==0){std::reverse(l.inputs.begin(),l.inputs.end());std::reverse(l.ids.begin(),l.ids.end());}
  if(test%5==0&&!l.ids.empty()){l.ids.erase(l.ids.begin());l.inputs.erase(l.inputs.begin());}
  if(test%6==0){l.inputs.push_back(1);l.ids.push_back(++next);}
  ref.followSelection();g.followSelection();assert(g==static_cast<GUI const&>(ref));
 }
 l.inputs.resize(4000000);l.ids.resize(l.inputs.size());std::iota(l.ids.begin(),l.ids.end(),uint64_t{1});
 GUI g;g.selected=3999999;g.anchor=3999998;g.selectedId=4000000;g.anchorId=3999999;
 Reference ref;static_cast<GUI&>(ref)=g;
 l.reads=0;g.followSelection();auto fast=l.reads;l.reads=0;ref.followSelection();auto slow=l.reads;
 assert(g==static_cast<GUI const&>(ref));assert(fast==2&&slow==7999999);
 std::cout<<"10,000 selection/edit cases passed; stable 4M-input selection reads "<<fast<<" IDs instead of "<<slow<<".\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(fixture.replace('// FUNCTIONS',s+reference))
 subprocess.run(['c++','-std=c++23','-O1','-g','-DGEODE_IS_WINDOWS','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
