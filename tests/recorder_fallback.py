"""Compile production codec/filter attempts against a failing encoder stub."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/Recorder/recorder.cpp').read_text()
source=source[source.index('\tauto colorModes ='):source.index('\n#endif',source.index('\tapi = std::move(encoder);',source.index('\tauto colorModes =')))]
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
namespace ffmpeg {
enum class PixelFormat { RGBA, BGRA };
struct RenderSettings {
 PixelFormat m_pixelFormat; std::string m_codec,m_colorspaceFilters;
 int64_t m_bitrate; uint32_t m_width,m_height; uint16_t m_fps;
 std::filesystem::path m_outputFile; bool m_doVerticalFlip;
};
namespace events {
struct Result {
 bool okay; bool isErr(){return !okay;} bool isOk(){return okay;}
 std::string unwrapErr(){return "Failed to create colorspace";}
};
struct Recorder {
 static inline std::vector<RenderSettings> attempts;
 static inline bool rejectFilter=false, rejectAll=false;
 bool used=false;
 Result init(RenderSettings const& settings){
  assert(!used); used=true; attempts.push_back(settings);
  assert(settings.m_colorspaceFilters.empty() || settings.m_colorspaceFilters=="all=bt709:iall=bt470bg:fast=1");
  std::ofstream(settings.m_outputFile)<<"partial";
  return {!rejectAll && (!rejectFilter || settings.m_colorspaceFilters.empty())};
 }
};
}
}
struct Value { bool renderColorFix=true; int64_t renderBitrate=0; std::string renderPreset="fast"; };
char const* s_echoPreset=nullptr;
std::unique_ptr<ffmpeg::events::Recorder> api;
std::string open(Value const& value,std::filesystem::path const& output){
 int width=1920,height=1080,fps=60;
 std::vector<std::string> picks{"h264_mediacodec","libx264"};
 auto encoder=std::make_unique<ffmpeg::events::Recorder>();
// PRODUCTION
int main(int argc,char** argv){
 assert(argc==2); std::filesystem::path output=argv[1]; Value value;
 using R=ffmpeg::events::Recorder;
 assert(open(value,output).empty()); assert(R::attempts.size()==1 && api);
 R::attempts.clear(); R::rejectFilter=true;
 assert(open(value,output).empty()); assert(R::attempts.size()==2);
 assert(R::attempts[0].m_codec==R::attempts[1].m_codec);
 assert(R::attempts[1].m_colorspaceFilters.empty() && value.renderColorFix);
 R::attempts.clear(); value.renderColorFix=false;
 assert(open(value,output).empty()); assert(R::attempts.size()==1);
 R::attempts.clear(); value.renderColorFix=true; R::rejectAll=true;
 assert(!open(value,output).empty()); assert(R::attempts.size()==4 && !std::filesystem::exists(output));
 std::cout<<"Colorspace arguments, fresh encoder retries, codec fallback and partial-file cleanup passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='echo-render-tests-') as directory:
 cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test'
 cpp.write_text(fixture.replace('// PRODUCTION',source))
 subprocess.run(['c++','-std=c++23','-DGEODE_IS_ANDROID','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(Path(directory)/'test.mp4')],check=True)
