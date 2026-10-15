#pragma once
#include <memory>

class GJBaseGameLayer;

// the pictures camera (hooks.cpp) as it was at a backstep save. w/out it a step back put physics' camera back but the
// picture kept showing where it was before, until the gap got big enough to snap
struct ShownCamera;
std::shared_ptr<ShownCamera> echo_save_shown_camera(GJBaseGameLayer* layer);
void echo_load_shown_camera(GJBaseGameLayer* layer, ShownCamera const* saved);
