#include "GUI/logo_texture.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace EchoUI {
    static void smooth(CCTexture2D* texture) {
        auto pot = [](unsigned n) {
            return n && !(n & (n - 1));
        };
        if (pot(texture->getPixelsWide()) && pot(texture->getPixelsHigh())) {
            texture->generateMipmap();
            ccTexParams params{ GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };
            texture->setTexParameters(&params);
        }
        else {
            texture->setAntiAliasTexParameters();
        }
    }

    static void to_hsv(float r, float g, float b, float& h, float& s, float& v) {
        float high = std::max({ r, g, b }), low = std::min({ r, g, b });
        float span = high - low;
        v = high;
        s = high > 0.f ? span / high : 0.f;
        if (span <= 0.f) h = 0.f;
        else if (high == r) h = std::fmod((g - b) / span, 6.f);
        else if (high == g) h = (b - r) / span + 2.f;
        else h = (r - g) / span + 4.f;
        h /= 6.f;
        if (h < 0.f) h += 1.f;
    }

    static void to_rgb(float h, float s, float v, float& r, float& g, float& b) {
        float k = h * 6.f;
        int sector = (int)std::floor(k) % 6;
        float f = k - std::floor(k);
        float p = v * (1.f - s);
        float q = v * (1.f - s * f);
        float t = v * (1.f - s * (1.f - f));
        switch (sector) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        default:
            r = v;
            g = p;
            b = q;
            break;
        }
    }

    struct LogoPart {
        const char* name;
        const char* file;
        ImU32 corners[4];
    };

    struct LoadedPart {
        bool tried = false;
        int side = 0;
        std::vector<unsigned char> alpha;
        CCTexture2D* texture = nullptr;
    };

    static const LogoPart LOGO_PARTS[LOGO_PART_COUNT] = {
        { "outline", "echo-logo-part-outline.png", { IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255) } },
        { "border", "echo-logo-part-border.png", { IM_COL32(158, 255, 183, 255), IM_COL32(107, 255, 187, 255), IM_COL32(56, 255, 190, 255), IM_COL32(107, 255, 187, 255) } },
        { "background", "echo-logo-part-background.png", { IM_COL32(53, 103, 87, 255), IM_COL32(54, 104, 88, 255), IM_COL32(18, 50, 41, 255), IM_COL32(17, 49, 40, 255) } },
        { "glow", "echo-logo-part-glow.png", { IM_COL32(129, 255, 208, 153), IM_COL32(92, 217, 158, 153), IM_COL32(9, 105, 70, 153), IM_COL32(45, 168, 119, 153) } },
        { "ring3", "echo-logo-part-ring3.png", { IM_COL32(115, 221, 188, 255), IM_COL32(115, 219, 186, 255), IM_COL32(50, 163, 114, 255), IM_COL32(50, 165, 116, 255) } },
        { "ring2", "echo-logo-part-ring2.png", { IM_COL32(155, 255, 225, 255), IM_COL32(152, 255, 220, 255), IM_COL32(72, 222, 176, 255), IM_COL32(75, 229, 180, 255) } },
        { "ring1", "echo-logo-part-ring1.png", { IM_COL32(247, 255, 179, 255), IM_COL32(223, 255, 195, 255), IM_COL32(51, 255, 215, 255), IM_COL32(75, 255, 199, 255) } },
        { "center", "echo-logo-part-center.png", { IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255) } },
        { "shadow", "echo-logo-part-shadow.png", { IM_COL32(0, 0, 0, 153), IM_COL32(0, 0, 0, 153), IM_COL32(0, 0, 0, 153), IM_COL32(0, 0, 0, 153) } },
        { "cursor_outline", "echo-logo-part-cursor-outline.png", { IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255) } },
        { "cursor", "echo-logo-part-cursor.png", { IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255) } },
    };

    static ImVec4 blend(const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }

    static LoadedPart& loaded_part(int part) {
        static LoadedPart loaded[LOGO_PART_COUNT];
        auto& entry = loaded[part];
        if (entry.tried) return entry;
        entry.tried = true;
        auto path = Mod::get()->getResourcesDir() / LOGO_PARTS[part].file;
        auto* image = new CCImage();
        if (!image->initWithImageFile(geode::utils::string::pathToString(path).c_str()) || !image->hasAlpha() || image->getWidth() != image->getHeight()) {
            image->release();
            return entry;
        }
        int side = image->getWidth();
        const unsigned char* source = image->getData();
        std::vector<unsigned char> pixels((size_t)side * side * 4, 255);
        entry.alpha.resize((size_t)side * side);
        for (size_t i = 0; i < entry.alpha.size(); ++i) {
            entry.alpha[i] = source[i * 4 + 3];
            pixels[i * 4 + 3] = entry.alpha[i];
        }
        image->release();
        auto* texture = new CCTexture2D();
        if (!texture->initWithData(pixels.data(), kCCTexture2DPixelFormat_RGBA8888, side, side, CCSize((float)side, (float)side))) {
            texture->release();
            entry.alpha.clear();
            return entry;
        }
        smooth(texture);
        entry.side = side;
        entry.texture = texture;
        return entry;
    }

    CCTexture2D* logo_texture(const char* sprite) {
        static std::unordered_map<std::string, CCTexture2D*> loaded_by;
        auto [at, fresh] = loaded_by.try_emplace(sprite, nullptr);
        if (!fresh) return at->second;
        auto* loaded = CCTextureCache::sharedTextureCache()->addImage(sprite, false);
        if (!loaded) return at->second;
        smooth(loaded);
        loaded->retain();
        at->second = loaded;
        return at->second;
    }

    ImTextureID logo_image(const char* sprite) {
        auto* texture = logo_texture(sprite);
        return texture ? (ImTextureID)texture->getName() : ImTextureID{};
    }

    ImTextureID logo_tinted(float hue) {
        auto* texture = logo_tinted_texture(hue);
        return texture ? (ImTextureID)texture->getName() : ImTextureID{};
    }

    CCTexture2D* logo_tinted_texture(float hue) {
        static int kept_degree = -1;
        static CCTexture2D* kept = nullptr;
        hue -= std::floor(hue);
        int degree = (int)std::lround(hue * 360.f) % 360;
        if (kept && degree == kept_degree) return kept;

        const float SOURCE_HUE = 130.f / 360.f;
        auto path = Mod::get()->getResourcesDir() / "echo-logo-green.png";
        auto* image = new CCImage();
        if (!image->initWithImageFile(geode::utils::string::pathToString(path).c_str())) {
            image->release();
            return logo_texture("logo.png"_spr);
        }
        int width = image->getWidth(), height = image->getHeight();
        const unsigned char* source = image->getData();
        bool has_alpha = image->hasAlpha();
        int step = has_alpha ? 4 : 3;
        std::vector<unsigned char> pixels((size_t)width * height * 4);
        float shift = (float)degree / 360.f - SOURCE_HUE;
        for (int i = 0; i < width * height; ++i) {
            const unsigned char* in = source + (size_t)i * step;
            float a = has_alpha ? in[3] / 255.f : 1.f;
            float r = in[0] / 255.f, g = in[1] / 255.f, b = in[2] / 255.f;
            if (image->isPremultipliedAlpha() && a > 0.f) {
                r = std::min(1.f, r / a);
                g = std::min(1.f, g / a);
                b = std::min(1.f, b / a);
            }
            float h = 0.f;
            float s = 0.f;
            float v = 0.f;
            to_hsv(r, g, b, h, s, v);
            float weight = std::clamp((s - .12f) / .12f, 0.f, 1.f);
            if (weight > 0.f) {
                float nr = 0.f;
                float ng = 0.f;
                float nb = 0.f;
                float turned = h + shift;
                turned -= std::floor(turned);
                to_rgb(turned, s, v, nr, ng, nb);
                r += (nr - r) * weight;
                g += (ng - g) * weight;
                b += (nb - b) * weight;
            }
            unsigned char* out = pixels.data() + (size_t)i * 4;
            out[0] = (unsigned char)(r * 255.f + .5f);
            out[1] = (unsigned char)(g * 255.f + .5f);
            out[2] = (unsigned char)(b * 255.f + .5f);
            out[3] = (unsigned char)(a * 255.f + .5f);
        }
        image->release();
        auto* texture = new CCTexture2D();
        if (!texture->initWithData(pixels.data(), kCCTexture2DPixelFormat_RGBA8888, width, height, CCSize((float)width, (float)height))) {
            texture->release();
            return logo_texture("logo.png"_spr);
        }
        smooth(texture);
        if (kept) kept->release();
        kept = texture;
        kept_degree = degree;
        return kept;
    }

    const char* logo_part_name(int part) {
        return LOGO_PARTS[part].name;
    }

    LogoPaint logo_part_default(int part) {
        LogoPaint paint;
        for (int i = 0; i < 4; ++i) paint.corners[i] = LOGO_PARTS[part].corners[i];
        return paint;
    }

    CCTexture2D* logo_part_texture(int part) {
        return loaded_part(part).texture;
    }

    CCTexture2D* logo_from_parts(const LogoPaint* paints) {
        int side = 0;
        for (int part = 0; part < LOGO_PART_COUNT; ++part) {
            auto& entry = loaded_part(part);
            if (!entry.texture || (side && entry.side != side)) return nullptr;
            side = entry.side;
        }
        std::vector<float> mixed((size_t)side * side * 4, 0.f);
        for (int part = 0; part < LOGO_PART_COUNT; ++part) {
            if (!paints[part].shown) continue;
            const auto& alpha = loaded_part(part).alpha;
            ImVec4 corner[4];
            for (int i = 0; i < 4; ++i) corner[i] = ImGui::ColorConvertU32ToFloat4(paints[part].corners[i]);
            for (int y = 0; y < side; ++y) {
                float down = ((float)y + .5f) / (float)side;
                ImVec4 left = blend(corner[0], corner[3], down), right = blend(corner[1], corner[2], down);
                for (int x = 0; x < side; ++x) {
                    size_t i = (size_t)y * side + x;
                    if (!alpha[i]) continue;
                    ImVec4 color = blend(left, right, ((float)x + .5f) / (float)side);
                    float cover = color.w * (float)alpha[i] / 255.f;
                    float* out = mixed.data() + i * 4;
                    out[0] = color.x * cover + out[0] * (1.f - cover);
                    out[1] = color.y * cover + out[1] * (1.f - cover);
                    out[2] = color.z * cover + out[2] * (1.f - cover);
                    out[3] = cover + out[3] * (1.f - cover);
                }
            }
        }
        std::vector<unsigned char> pixels(mixed.size());
        for (size_t i = 0; i < pixels.size(); i += 4) {
            float cover = mixed[i + 3];
            for (int c = 0; c < 3; ++c) {
                float straight = cover > 0.f ? mixed[i + c] / cover : 0.f;
                pixels[i + c] = (unsigned char)(std::clamp(straight, 0.f, 1.f) * 255.f + .5f);
            }
            pixels[i + 3] = (unsigned char)(std::clamp(cover, 0.f, 1.f) * 255.f + .5f);
        }
        auto* texture = new CCTexture2D();
        if (!texture->initWithData(pixels.data(), kCCTexture2DPixelFormat_RGBA8888, side, side, CCSize((float)side, (float)side))) {
            texture->release();
            return nullptr;
        }
        smooth(texture);
        return texture;
    }
}
