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
static bool high_precision_editor_loaded() {
    static bool const loaded = Loader::get()->isModLoaded("kxtbit.high_precision_editor");
    return loaded;
}

static float without_full_turns(float degrees) {
    return degrees - (float)((int)(degrees / 360.f) * 360);
}

static std::string exact_text(float value) {
    auto text = fmt::format("{}", value);
    if (text.find_first_of("eEni") != std::string::npos)
        return {};
    return text;
}

static std::optional<float> parse_float(std::string_view text) {
#if defined(GEODE_IS_IOS) || defined(GEODE_IS_MACOS)
    if (text.empty() || text.find_first_not_of("+-.0123456789eE") != std::string_view::npos)
        return std::nullopt;
    std::string copy(text);
    char* stop = nullptr;
    float value = std::strtof(copy.c_str(), &stop);
    if (stop != copy.c_str() + copy.size()) return std::nullopt;
    return value;
#else
    float value = 0.f;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
#endif
}

class $modify(EchoPrecisionSave, GameObject) {
    gd::string getSaveString(GJBaseGameLayer* layer) {
        auto saved = GameObject::getSaveString(layer);
        if (high_precision_editor_loaded()) return saved;
        std::string_view text(saved.data(), saved.size());
        auto position = getPosition();
        auto exact_value_for_key = [&](int key) -> std::optional<std::pair<float, float>> {
            switch (key) {
            case 2:
                return std::pair{ position.x, 0.051f };
            case 3:
                return std::pair{ position.y - 90.f, 0.051f };
            case 6:
            case 131: {
                float value = without_full_turns(getRotationX());
                return std::pair{ value, std::max(0.001f, std::abs(value) * 1e-4f) };
            }
            case 132: {
                float value = without_full_turns(getRotationY());
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
        std::string rewritten;
        rewritten.reserve(saved.size() + 32);
        bool changed = false;
        std::size_t cursor = 0;
        int key = 0;
        bool is_key = true;
        while (cursor <= text.size()) {
            auto comma = text.find(',', cursor);
            if (comma == std::string_view::npos) comma = text.size();
            auto token = text.substr(cursor, comma - cursor);
            std::string_view written = token;
            std::string replacement;
            if (is_key) {
                key = 0;
                std::from_chars(token.data(), token.data() + token.size(), key);
            }
            else if (auto exact_value = exact_value_for_key(key)) {
                auto [value, tolerance] = *exact_value;
                auto saved_value = parse_float(token);
                if (saved_value && std::isfinite(value) && *saved_value != value && std::abs(*saved_value - value) <= tolerance) {
                    replacement = exact_text(value);
                    if (!replacement.empty()) {
                        written = replacement;
                        changed = true;
                    }
                }
            }
            rewritten.append(written);
            if (comma == text.size()) break;
            rewritten.push_back(',');
            cursor = comma + 1;
            is_key = !is_key;
        }
        if (!changed) return saved;
        return gd::string(rewritten);
    }
};
