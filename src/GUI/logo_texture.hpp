#pragma once

#include <imgui.h>
#include <cocos2d.h>

using namespace cocos2d;

namespace EchoUI {
	ImTextureID logo_image(const char* sprite);
	ImTextureID logo_tinted(float hue);
	CCTexture2D* logo_texture(const char* sprite);
	CCTexture2D* logo_tinted_texture(float hue);
}
