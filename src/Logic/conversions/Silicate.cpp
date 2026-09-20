#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace fs = std::filesystem;

using namespace geode::prelude;

class SilicateReplay {
public:
    static constexpr char magic[] = "SLC3RPLY";
    static constexpr size_t magic_size = 8;
    static constexpr uint16_t meta_size = 64;
    static constexpr uint32_t meta_version = 2;
    static constexpr uint32_t build = 81;
    static constexpr uint32_t action_atom = 1;
    static constexpr uint8_t footer = 0xCC;
    static constexpr uint64_t size_mask = 0x00FFFFFFFFFFFFFFull; // low 56 bits
    static constexpr size_t max_section = size_t(1) << 15;
    static constexpr uint64_t max_produced = ~uint64_t{ 0 };

    enum class Special : uint8_t {
        Restart = 0,
        RestartFull = 1,
        Death = 2,
        Tps = 3,
        Bugpoint = 4
    };

    std::vector<Frame> inputs;
    std::vector<RateChange> rates;
    uint64_t frame = 0;
    uint64_t attempt_start = 0;
    uint64_t produced = 0;
    double tps = 240.0, start_tps = 240.0;

    void press(int button, bool holding, bool player2) {
        uint64_t local = frame - attempt_start;
        if (local > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return;
        Frame input;
        input.number = static_cast<int>(local);
        input.button = button;
        input.holding = holding;
        input.player2 = player2;
        inputs.push_back(input);
    }

    void state(uint64_t value) {
        frame += value >> 4;
        int button = static_cast<int>((value >> 2) & 3);
        bool player2 = (value >> 1) & 1;
        if (button == 0) {
            press(1, true, player2);
            press(1, false, player2);
            produced += 2;
        }
        else {
            press(button, value & 1, player2);
            produced += 1;
        }
    }

    bool section(ByteCursor& in, size_t end) {
        if (end - in.at < 2)
            return false;
        auto header = static_cast<uint16_t>(in.uint(2));
        switch (header >> 14) {
        case 0:
        case 1: {
            size_t width = size_t(1) << ((header >> 12) & 3);
            size_t len = size_t(1) << ((header >> 8) & 15);
            size_t repeats = (header >> 14) == 1 ? size_t(1) << ((header >> 3) & 31) : 1;
            if (end - in.at < width * len)
                return false;
            std::vector<uint64_t> cluster(len);
            for (auto& value : cluster)
                value = in.uint(width);
            for (size_t r = 0; r < repeats && !in.failed; ++r) {
                if (produced > max_produced)
                    return false;
                for (auto value : cluster)
                    state(value);
            }
            return !in.failed;
        }
        case 2: {
            size_t width = size_t(1) << ((header >> 8) & 3);
            if (end - in.at < width)
                return false;
            frame += in.uint(width);
            auto type = static_cast<Special>((header >> 10) & 15);
            switch (type) {
            case Special::Tps: {
                if (end - in.at < 8)
                    return false;
                auto value = in.pod<double>();
                if (!std::isfinite(value) || value < 1.0)
                    break;
                tps = value;
                uint64_t local = frame - attempt_start;
                if (frame <= attempt_start)
                    start_tps = value;
                else if (local <= static_cast<uint64_t>(std::numeric_limits<int>::max()))
                    rates.push_back({ static_cast<int>(local), static_cast<float>(value) });
                break;
            }
            case Special::Restart:
            case Special::RestartFull:
            case Special::Death:
                if (end - in.at < 8)
                    return false;
                in.uint(8);
                inputs.clear();
                rates.clear();
                start_tps = tps;
                attempt_start = frame + 1;
                break;
            case Special::Bugpoint:
                break;
            default:
                return false;
            }
            produced += 1;
            return !in.failed;
        }
        default:
            return false;
        }
    }

    bool actions(ByteCursor& in, size_t end) {
        if (end - in.at < 8)
            return false;
        uint64_t amt = in.uint(8);
        uint64_t target = produced + amt;
        while (produced < target) {
            if (in.at >= end || !section(in, end))
                return false;
        }
        return true;
    }

    static size_t width_for(uint64_t delta) {
        if (delta < (1ull << 4))
            return 0;
        if (delta < (1ull << 12))
            return 1;
        if (delta < (1ull << 28))
            return 2;
        return 3;
    }

    static bool swift_pair(std::vector<Frame> const& inputs, size_t i) {
        if (i + 1 >= inputs.size())
            return false;
        auto const& a = inputs[i];
        auto const& b = inputs[i + 1];
        return a.button == 1 && b.button == 1 && a.holding && !b.holding && a.player2 == b.player2 && a.number == b.number;
    }

    struct Item {
        uint64_t state = 0;
        size_t width = 0;
        bool rate = false;
        uint64_t delta = 0;
        double fps = 0.0;
    };

    static size_t special_width(uint64_t delta) {
        if (delta <= 0xFFull)
            return 0;
        if (delta <= 0xFFFFull)
            return 1;
        if (delta <= 0xFFFFFFFFull)
            return 2;
        return 3;
    }

    static void write_actions(ByteSink& out, std::vector<Frame> const& inputs, std::vector<RateChange> const& rates) {
        std::vector<Item> items;
        int prev = 0;
        size_t next_rate = 0;
        auto place_rates = [&](int up_to) {
            for (; next_rate < rates.size() && rates[next_rate].frame <= up_to; ++next_rate) {
                auto const& change = rates[next_rate];
                uint64_t delta = static_cast<uint64_t>(std::max(change.frame - prev, 0));
                prev = std::max(change.frame, prev);
                items.push_back({ 0, special_width(delta), true, delta, static_cast<double>(change.fps) });
            }
        };
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto const& input = inputs[i];
            place_rates(input.number);
            uint64_t delta = static_cast<uint64_t>(input.number - prev);
            prev = input.number;
            bool swift = swift_pair(inputs, i);
            uint64_t button = swift ? 0 : static_cast<uint64_t>(input.button);
            items.push_back({ delta << 4 | button << 2 | uint64_t(input.player2) << 1 | uint64_t(input.holding && !swift), width_for(delta) });
            if (swift)
                ++i;
        }
        place_rates(std::numeric_limits<int>::max());
        out.uint(inputs.size() + rates.size(), 8);
        size_t i = 0;
        while (i < items.size()) {
            auto const& item = items[i];
            if (item.rate) {
                out.uint(static_cast<uint16_t>(2u << 14 | static_cast<uint16_t>(Special::Tps) << 10 | item.width << 8), 2);
                out.uint(item.delta, size_t(1) << item.width);
                out.pod(item.fps);
                ++i;
                continue;
            }
            size_t run = 1;
            while (i + run < items.size() && run < max_section && !items[i + run].rate && items[i + run].width == item.width)
                ++run;
            auto chunk = std::bit_floor(run);
            auto exponent = static_cast<uint16_t>(std::countr_zero(chunk));
            out.uint(static_cast<uint16_t>(item.width << 12 | exponent << 8), 2);
            for (size_t j = 0; j < chunk; ++j)
                out.uint(items[i + j].state, size_t(1) << item.width);
            i += chunk;
        }
    }
};

bool ReplayIO::read_silicate(fs::path const& path) {
    // later
    return {};
}

bool ReplayIO::write_silicate(fs::path const& path) {
    auto inputs = ReplayIO::sorted_inputs();
    ByteSink out;
    out.text({ SilicateReplay::magic, SilicateReplay::magic_size });
    out.uint(SilicateReplay::meta_size, 2);
    size_t meta_start = out.data.size();
    out.pod(static_cast<double>(Logic::get().rate_at(0)));
    out.uint(Logic::get().lvl_seed_known ? Logic::get().lvl_seed : 0, 8);
    out.uint(SilicateReplay::meta_version, 4);
    out.uint(SilicateReplay::build, 4);
    out.uint(0, 4);
    out.data.resize(meta_start + SilicateReplay::meta_size, 0);
    out.uint(SilicateReplay::action_atom, 4);
    size_t size_at = out.data.size();
    out.uint(0, 8);
    auto rates = Logic::get().rates;
    std::stable_sort(rates.begin(), rates.end(), [](RateChange const& a, RateChange const& b) {
        return a.frame < b.frame;
    });
    std::erase_if(rates, [](RateChange const& change) {
        return change.frame <= 0 || !std::isfinite(change.fps) || change.fps < 1.f;
    });
    SilicateReplay::write_actions(out, inputs, rates);
    uint64_t atom_size = out.data.size() - size_at - 8;
    for (size_t i = 0; i < 8; ++i)
        out.data[size_at + i] = static_cast<uint8_t>(atom_size >> (8 * i));
    out.uint(SilicateReplay::footer, 1);
    return out.save(path);
}
