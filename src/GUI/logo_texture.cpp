#include "GUI/logo_texture.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace eui {
    namespace {
        void smooth(CCTexture2D* texture) {
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

        void to_hsv(float r, float g, float b, float& h, float& s, float& v) {
            float high = std::max({ r, g, b }), low = std::min({ r, g, b });
            float span = high - low;
            v = high;
            s = high > 0.f ? span / high : 0.f;
            if (span <= 0.f)
                h = 0.f;
            else if (high == r)
                h = std::fmod((g - b) / span, 6.f);
            else if (high == g)
                h = (b - r) / span + 2.f;
            else
                h = (r - g) / span + 4.f;
            h /= 6.f;
            if (h < 0.f)
                h += 1.f;
        }

        void to_rgb(float h, float s, float v, float& r, float& g, float& b) {
            float k = h * 6.f;
            int sector = static_cast<int>(std::floor(k)) % 6;
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
    }

    CCTexture2D* logo_texture(char const* sprite) {
        static std::unordered_map<std::string, CCTexture2D*> loaded_by;
        auto [at, fresh] = loaded_by.try_emplace(sprite, nullptr);
        if (!fresh)
            return at->second;
        auto* loaded = CCTextureCache::sharedTextureCache()->addImage(sprite, false);
        if (!loaded)
            return at->second;
        smooth(loaded);
        loaded->retain();
        at->second = loaded;
        return at->second;
    }

    ImTextureID logo_image(char const* sprite) {
        auto* texture = logo_texture(sprite);
        return texture ? static_cast<ImTextureID>(texture->getName()) : ImTextureID{};
    }

    ImTextureID logo_tinted(float hue) {
        auto* texture = logo_tinted_texture(hue);
        return texture ? static_cast<ImTextureID>(texture->getName()) : ImTextureID{};
    }

    CCTexture2D* logo_tinted_texture(float hue) {
        static int kept_degree = -1;
        static CCTexture2D* kept = nullptr;
        hue -= std::floor(hue);
        int degree = static_cast<int>(std::lround(hue * 360.f)) % 360;
        if (kept && degree == kept_degree)
            return kept;

        constexpr float SOURCE_HUE = 130.f / 360.f;
        auto path = Mod::get()->getResourcesDir() / "echo-logo-green.png";
        auto* image = new CCImage();
        if (!image->initWithImageFile(geode::utils::string::pathToString(path).c_str())) {
            image->release();
            return logo_texture("logo.png"_spr);
        }
        int width = image->getWidth(), height = image->getHeight();
        unsigned char const* source = image->getData();
        bool hasAlpha = image->hasAlpha();
        int step = hasAlpha ? 4 : 3;
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        float shift = static_cast<float>(degree) / 360.f - SOURCE_HUE;
        for (int i = 0; i < width * height; ++i) {
            unsigned char const* in = source + static_cast<size_t>(i) * step;
            float a = hasAlpha ? in[3] / 255.f : 1.f;
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
            unsigned char* out = pixels.data() + static_cast<size_t>(i) * 4;
            out[0] = static_cast<unsigned char>(r * 255.f + .5f);
            out[1] = static_cast<unsigned char>(g * 255.f + .5f);
            out[2] = static_cast<unsigned char>(b * 255.f + .5f);
            out[3] = static_cast<unsigned char>(a * 255.f + .5f);
        }
        image->release();
        auto* texture = new CCTexture2D();
        if (!texture->initWithData(pixels.data(), kCCTexture2DPixelFormat_RGBA8888, width, height, CCSize(static_cast<float>(width), static_cast<float>(height)))) {
            texture->release();
            return logo_texture("logo.png"_spr);
        }
        smooth(texture);
        if (kept)
            kept->release();
        kept = texture;
        kept_degree = degree;
        return kept;
    }
}
