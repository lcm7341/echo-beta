#pragma once

#include <imgui.h>
#include <cocos2d.h>

#define LOGO_PART_COUNT 11

using namespace cocos2d;

namespace EchoUI {
	struct LogoPaint {
		bool shown = true;
		ImU32 corners[4] = {};
	};

	ImTextureID logo_image(const char* sprite);
	ImTextureID logo_tinted(float hue);
	CCTexture2D* logo_texture(const char* sprite);
	CCTexture2D* logo_tinted_texture(float hue);
	const char* logo_part_name(int part);
	LogoPaint logo_part_default(int part);
	CCTexture2D* logo_part_texture(int part);
	CCTexture2D* logo_from_parts(const LogoPaint* paints);
}
