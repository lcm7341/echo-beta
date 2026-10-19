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
    static inline const char magic[] = "SLC3RPLY";
    static const size_t magic_size = 8;
    static const unsigned short meta_size = 64;
    static const unsigned meta_version = 2;
    static const unsigned build = 81;
    static const unsigned action_atom = 1;
    static const unsigned char footer = 0xCC;
    static const unsigned long long size_mask = (1ull << 56) - 1; // low 56 bits
    static const size_t max_section = size_t(1) << 15;

    enum class Special : unsigned char {
        Restart = 0,
        RestartFull = 1,
        Death = 2,
        Tps = 3,
        Bugpoint = 4
    };

    std::vector<Frame> inputs;
    std::vector<RateChange> rates;
    std::vector<unsigned long long> cluster;
    unsigned long long frame = 0;
    unsigned long long attempt_start = 0;
    unsigned long long produced = 0;
    unsigned long long seed = 0;
    double tps = 240.0, start_tps = 240.0;

    void press(int button, bool holding, bool player2) {
        unsigned long long local = frame - attempt_start;
        if (local > (unsigned long long)(std::numeric_limits<int>::max())) return;
        Frame input;
        input.number = (int)local;
        input.button = button;
        input.holding = holding;
        input.player2 = player2;
        inputs.push_back(input);
    }

    void state(unsigned long long value) {
        frame += value >> 4;
        int button = (int)((value >> 2) & 3);
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

    bool section(ByteReader& in, size_t end, unsigned long long target) {
        if (end - in.at < 2) return false;
        auto header = (unsigned short)in.uint(2);
        switch (header >> 14) {
        case 0:
        case 1: {
            size_t width = size_t(1) << ((header >> 12) & 3);
            size_t len = size_t(1) << ((header >> 8) & 15);
            size_t repeats = (header >> 14) == 1 ? size_t(1) << ((header >> 3) & 31) : 1;
            if (end - in.at < width * len) return false;
            cluster.resize(len);
            for (auto& value : cluster)
                value = in.uint(width);
            unsigned long long count = 0;
            for (auto value : cluster)
                count += ((value >> 2) & 3) == 0 ? 2 : 1;
            if (count > (target - produced) / repeats) return false;
            for (size_t r = 0; r < repeats && !in.failed; ++r) {
                for (auto value : cluster)
                    state(value);
            }
            return !in.failed;
        }
        case 2: {
            size_t width = size_t(1) << ((header >> 8) & 3);
            if (end - in.at < width) return false;
            frame += in.uint(width);
            auto type = (Special)((header >> 10) & 15);
            switch (type) {
            case Special::Tps: {
                if (end - in.at < 8) return false;
                auto value = in.pod<double>();
                if (!std::isfinite(value) || value < 1.0) break;
                tps = value;
                unsigned long long local = frame - attempt_start;
                if (frame <= attempt_start) start_tps = value;
                else if (local <= (unsigned long long)(std::numeric_limits<int>::max()))
                    rates.push_back({ (int)local, (float)value });
                break;
            }
            case Special::Restart:
            case Special::RestartFull:
            case Special::Death:
                if (end - in.at < 8) return false;
                seed = in.uint(8);
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

    bool actions(ByteReader& in, size_t end) {
        if (end - in.at < 8) return false;
        unsigned long long amt = in.uint(8);
        if (amt > ReplayIO::max_inputs - produced) return false;
        unsigned long long target = produced + amt;
        const size_t max_inputs_per_byte = 2;
        if (target <= (end - in.at) * max_inputs_per_byte) inputs.reserve((size_t)target);
        while (produced < target) {
            if (in.at >= end || !section(in, end, target)) return false;
        }
        return true;
    }

    static size_t width_for(unsigned long long delta) {
        if (delta < (1ull << 4)) return 0;
        if (delta < (1ull << 12)) return 1;
        if (delta < (1ull << 28)) return 2;
        return 3;
    }

    static bool swift_pair(const std::vector<Frame>& inputs, size_t i) {
        if (i + 1 >= inputs.size()) return false;
        const auto& a = inputs[i];
        const auto& b = inputs[i + 1];
        return a.button == 1 && b.button == 1 && a.holding && !b.holding && a.player2 == b.player2 && a.number == b.number;
    }

    struct ActionToWrite {
        unsigned long long state = 0;
        size_t width = 0;
        bool rate = false;
        unsigned long long delta = 0;
        double fps = 0.0;
    };

    static size_t special_width(unsigned long long delta) {
        if (delta <= 255ull) return 0;
        if (delta <= 65535ull) return 1;
        if (delta <= 4294967295ull) return 2;
        return 3;
    }

    static void write_actions(ByteWriter& out, const std::vector<Frame>& inputs, const std::vector<RateChange>& rates) {
        std::vector<ActionToWrite> items;
        int prev = 0;
        size_t next_rate = 0;
        auto place_rates = [&](int up_to) {
            for (; next_rate < rates.size() && rates[next_rate].frame <= up_to; ++next_rate) {
                const auto& change = rates[next_rate];
                unsigned long long delta = (unsigned long long)std::max(change.frame - prev, 0);
                prev = std::max(change.frame, prev);
                items.push_back({ 0, special_width(delta), true, delta, (double)change.fps });
            }
        };
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            place_rates(input.number);
            unsigned long long delta = (unsigned long long)(input.number - prev);
            prev = input.number;
            bool swift = swift_pair(inputs, i);
            unsigned long long button = swift ? 0 : (unsigned long long)input.button;
            items.push_back({ delta << 4 | button << 2 | (unsigned long long)input.player2 << 1 | (unsigned long long)(input.holding && !swift), width_for(delta) });
            if (swift) ++i;
        }
        place_rates(std::numeric_limits<int>::max());
        out.uint(inputs.size() + rates.size(), 8);
        size_t i = 0;
        while (i < items.size()) {
            const auto& item = items[i];
            if (item.rate) {
                out.uint((unsigned short)(2u << 14 | (unsigned short)Special::Tps << 10 | item.width << 8), 2);
                out.uint(item.delta, size_t(1) << item.width);
                out.pod(item.fps);
                ++i;
                continue;
            }
            size_t run = 1;
            while (i + run < items.size() && run < max_section && !items[i + run].rate && items[i + run].width == item.width)
                ++run;
            auto chunk = std::bit_floor(run);
            auto exponent = (unsigned short)std::countr_zero(chunk);
            out.uint((unsigned short)(item.width << 12 | exponent << 8), 2);
            for (size_t j = 0; j < chunk; ++j)
                out.uint(items[i + j].state, size_t(1) << item.width);
            i += chunk;
        }
    }
};

bool ReplayIO::read_silicate(const fs::path& path, MacroData& out) {
    ByteReader in{ ReplayIO::read_all(path) };
    auto& data = in.data;
    if (data.size() < SilicateReplay::magic_size + 2 + 16 + 1) return false;
    if (std::memcmp(data.data(), SilicateReplay::magic, SilicateReplay::magic_size) != 0)
        return false;
    in.at = SilicateReplay::magic_size;
    auto meta_size = (size_t)in.uint(2);
    if (meta_size < 16 || in.left() < meta_size + 1) return false;
    size_t meta_start = in.at;
    SilicateReplay replay;
    replay.tps = in.pod<double>();
    if (!std::isfinite(replay.tps) || replay.tps < 1.0) replay.tps = 240.0;
    replay.start_tps = replay.tps;
    replay.seed = in.uint(8);
    in.at = meta_start + meta_size;
    size_t end = data.back() == SilicateReplay::footer ? data.size() - 1 : data.size();
    while (end - in.at >= 12) {
        auto id = (unsigned)in.uint(4);
        unsigned long long size = in.uint(8) & SilicateReplay::size_mask;
        if (size > end - in.at) return false;
        size_t atom_end = size == 0 ? end : in.at + (size_t)size;
        if (id == SilicateReplay::action_atom && !replay.actions(in, atom_end)) return false;
        in.at = atom_end;
    }
    if (in.failed) return false;
    ReplayIO::adopt_inputs(out, std::move(replay.inputs), (float)replay.start_tps);
    out.rates = std::move(replay.rates);
    tidy_rates(out.rates, out.fps);
    out.lvl_seed = replay.seed;
    out.lvl_seed_known = true;
    out.silicate_variance = true;
    return true;
}

bool ReplayIO::write_silicate(const fs::path& path, const MacroData& macro) {
    auto inputs = ReplayIO::sorted_inputs(macro);
    ByteWriter out;
    out.text({ SilicateReplay::magic, SilicateReplay::magic_size });
    out.uint(SilicateReplay::meta_size, 2);
    size_t meta_start = out.data.size();
    out.pod((double)rate_at_frame(macro.rates, macro.fps, 0));
    out.uint(macro.lvl_seed_known ? macro.lvl_seed : 0, 8);
    out.uint(SilicateReplay::meta_version, 4);
    out.uint(SilicateReplay::build, 4);
    out.uint(0, 4);
    out.data.resize(meta_start + SilicateReplay::meta_size, 0);
    out.uint(SilicateReplay::action_atom, 4);
    size_t size_at = out.data.size();
    out.uint(0, 8);
    auto rates = macro.rates;
    std::stable_sort(rates.begin(), rates.end(), [](const RateChange& a, const RateChange& b) {
        return a.frame < b.frame;
    });
    std::erase_if(rates, [](const RateChange& change) {
        return change.frame <= 0;
    });
    SilicateReplay::write_actions(out, inputs, rates);
    unsigned long long atom_size = out.data.size() - size_at - 8;
    for (size_t i = 0; i < 8; ++i)
        out.data[size_at + i] = (unsigned char)(atom_size >> (8 * i));
    out.uint(SilicateReplay::footer, 1);
    return out.save(path);
}
