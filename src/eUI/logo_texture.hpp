#pragma once

#include <imgui.h>
#include <cocos2d.h>

using namespace cocos2d;

namespace eui {
	ImTextureID logo_image(char const* sprite);
	ImTextureID logo_tinted(float hue);
	CCTexture2D* logo_texture(char const* sprite);
	CCTexture2D* logo_tinted_texture(float hue);
}
