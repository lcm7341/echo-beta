#include <cstdlib>
#include <Geode/Geode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;

// we skid hpe
namespace {
    bool other_mod_saves() {
        // unless its here :}
        static bool const other = Loader::get()->isModLoaded("kxtbit.high_precision_editor");
        return other;
    }

    float less_turns(float degrees) {
        return degrees - static_cast<float>(static_cast<int>(degrees / 360.f) * 360);
    }

    std::string exact(float value) {
        auto text = fmt::format("{}", value);
        if (text.find_first_of("eEni") != std::string::npos)
            return {};
        return text;
    }

    std::optional<float> number(std::string_view text) {
        float value = 0.f;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }
}

class $modify(EchoPrecisionSave, GameObject) {
    gd::string getSaveString(GJBaseGameLayer* layer) {
        auto saved = GameObject::getSaveString(layer);
        if (other_mod_saves())
            return saved;
        std::string_view text(saved.data(), saved.size());
        auto position = getPosition();
        auto exact_for = [&](int key) -> std::optional<std::pair<float, float>> {
            switch (key) {
            case 2:
                return std::pair{ position.x, 0.051f };
            case 3:
                return std::pair{ position.y - 90.f, 0.051f };
            case 6:
            case 131: {
                float value = less_turns(getRotationX());
                return std::pair{ value, std::max(0.001f, std::abs(value) * 1e-4f) };
            }
            case 132: {
                float value = less_turns(getRotationY());
                return std::pair{ value, std::max(0.001f, std::abs(value) * 1e-4f) };
            }
            case 128:
                return std::pair{ m_scaleX, std::max(0.001f, std::abs(m_scaleX) * 1e-4f) };
            case 129:
                return std::pair{ m_scaleY, std::max(0.001f, std::abs(m_scaleY) * 1e-4f) };
            default:
                return std::nullopt;
            }
        };
        std::string out;
        out.reserve(saved.size() + 32);
        bool changed = false;
        std::size_t at = 0;
        int key = 0;
        bool is_key = true;
        while (at <= text.size()) {
            auto comma = text.find(',', at);
            if (comma == std::string_view::npos)
                comma = text.size();
            auto token = text.substr(at, comma - at);
            std::string_view written = token;
            std::string replacement;
            if (is_key) {
                key = 0;
                std::from_chars(token.data(), token.data() + token.size(), key);
            }
            else if (auto want = exact_for(key)) {
                auto [value, slack] = *want;
                auto gd = number(token);
                if (gd && std::isfinite(value) && *gd != value && std::abs(*gd - value) <= slack) {
                    replacement = exact(value);
                    if (!replacement.empty()) {
                        written = replacement;
                        changed = true;
                    }
                }
            }
            out.append(written);
            if (comma == text.size())
                break;
            out.push_back(',');
            at = comma + 1;
            is_key = !is_key;
        }
        if (!changed)
            return saved;
        return gd::string(out);
    }
};
