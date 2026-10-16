#include <algorithm>
#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace fs = std::filesystem;

using namespace geode::prelude;

class TcbotReplay {
public:
    static inline const std::array<unsigned char, 16> header = {
        0x9f, 0x88, 0x89, 0x84, 0x9f, 0x3b, 0x1d, 0xd8,
        0xcc, 0xa1, 0x86, 0x8a, 0x88, 0x99, 0x84, 0x00,
    };
    static const size_t meta_size = 64;
    static const unsigned char tps_flag = 1 << 1;
    static const unsigned char end_of_inputs = 0xCC;

    enum class Blob {
        Action,
        Delta,
        Tps,
        Seed
    };

    std::vector<Frame> inputs;

    void push(unsigned long long frame, int button, bool holding, bool player2) {
        if (frame > (unsigned long long)(std::numeric_limits<int>::max())) return;
        Frame input;
        input.number = (int)frame;
        input.button = button;
        input.holding = holding;
        input.player2 = player2;
        inputs.push_back(input);
    }

    bool read_v1(ByteReader& in) {
        auto amt = in.leb128();
        for (unsigned long long i = 0; i < amt && !in.failed; ++i) {
            auto frame = 1 + in.leb128();
            auto data = (unsigned char)in.uint(1);
            if (in.failed) return false;
            int input = data & 0b111;
            if (input < 3) push(frame, input + 1, data & 0x80, data & 0x40);
            else if (input <= 5) inputs.clear();
            else return false;
        }
        return !in.failed;
    }

    static unsigned long long read_delta(ByteReader& in, unsigned char info, unsigned long long& last_delta) {
        bool magic = info & 1;
        int blob = info >> 1;
        unsigned long long base = magic ? last_delta : 0;
        if (blob == 0) return base;
        size_t width = blob == 3 ? 4 : (size_t)blob;
        unsigned long long res = base + in.uint(width);
        if (res != 0) last_delta = res;
        return res;
    }

    bool read_v2(ByteReader& in) {
        unsigned long long frame = 1 + in.leb128();
        if (in.failed) return false;
        unsigned long long last_delta = 0;
        unsigned char delta_info = 0;
        auto next = Blob::Action;
        for (;;) {
            switch (next) {
            case Blob::Action: {
                if (in.left() == 0) return true;
                auto byte = (unsigned char)in.uint(1);
                delta_info = byte >> 5;
                int button = byte & 0b11;
                bool extra = byte & 0x10;
                if (button > 0) {
                    bool holding = byte & 0x04;
                    bool player2 = byte & 0x08;
                    push(frame, button, holding, player2);
                    if (extra) push(frame, button, !holding, player2);
                    next = Blob::Delta;
                    break;
                }
                int custom = (byte >> 2) & 0b11;
                if (custom == 3) {
                    next = extra ? Blob::Delta : Blob::Tps;
                    break;
                }
                inputs.clear();
                frame = 0;
                next = extra ? Blob::Seed : Blob::Delta;
                break;
            }
            case Blob::Delta:
                frame += read_delta(in, delta_info, last_delta);
                if (in.failed) return true;
                next = Blob::Action;
                break;
            case Blob::Tps:
                in.uint(4);
                if (in.failed) return true;
                next = Blob::Delta;
                break;
            case Blob::Seed:
                in.uint(8);
                if (in.failed) return true;
                next = Blob::Delta;
                break;
            }
        }
    }

    static unsigned char delta_info(unsigned long long delta, unsigned long long last_delta, unsigned long long& written) {
        const std::array<unsigned long long, 4> limits = { 0, 0xff, 0xffff, 0xffffffff };
        for (unsigned char blob = 0; blob < 4; ++blob) {
            if (delta <= limits[blob]) {
                written = delta;
                return (unsigned char)(blob << 1);
            }
            if (last_delta != 0 && last_delta <= delta && delta - last_delta <= limits[blob]) {
                written = delta - last_delta;
                return (unsigned char)(blob << 1 | 1);
            }
        }
        written = std::min<unsigned long long>(delta, limits[3]);
        return 3 << 1;
    }

    static void write_v2(ByteWriter& out, const std::vector<Frame>& inputs) {
        out.leb128(inputs.empty() ? 0 : (unsigned long long)std::max(inputs.front().number - 1, 0));
        unsigned long long last_delta = 0;
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            bool swift = i + 1 < inputs.size() && inputs[i + 1].number == input.number && inputs[i + 1].button == input.button &&
                         inputs[i + 1].holding != input.holding && inputs[i + 1].player2 == input.player2;
            size_t following = swift ? i + 2 : i + 1;
            unsigned long long delta = following < inputs.size() ? (unsigned long long)std::max(inputs[following].number - input.number, 0) : 0;
            unsigned long long written = 0;
            unsigned char info = delta_info(delta, last_delta, written);
            if (delta != 0) last_delta = delta;
            unsigned char byte = (unsigned char)(input.button & 0b11) | (unsigned char)input.holding << 2 | (unsigned char)input.player2 << 3 | (unsigned char)swift << 4 | info << 5;
            out.uint(byte, 1);
            int blob = info >> 1;
            if (blob != 0) out.uint(written, blob == 3 ? 4 : (size_t)blob);
            if (swift) ++i;
        }
    }
};

bool ReplayIO::read_tcbot(const fs::path& path, MacroData& out) {
    ByteReader in{ ReplayIO::read_all(path) };
    if (in.data.size() < TcbotReplay::header.size() + TcbotReplay::meta_size) return false;
    if (std::memcmp(in.data.data(), TcbotReplay::header.data(), TcbotReplay::header.size()) != 0)
        return false;
    size_t meta = TcbotReplay::header.size();
    unsigned char version = in.data[meta];
    unsigned char flags = in.data[meta + 2];
    float rate = 0.f;
    std::memcpy(&rate, in.data.data() + meta + 4, sizeof(rate));
    float tps = rate;
    if (version == 2 && !(flags & TcbotReplay::tps_flag) && rate > 0.f) {
        tps = 1.f / rate;
        float whole = std::round(tps);
        if (whole > 0.f && (float)(1.0 / whole) == rate) tps = whole;
    }
    in.at = meta + TcbotReplay::meta_size;
    TcbotReplay replay;
    bool ok = version == 1 ? replay.read_v1(in) : version == 2 ? replay.read_v2(in)
                                                               : false;
    if (!ok) return false;
    for (auto& input : replay.inputs)
        input.number = std::max(input.number - 2, 0);
    ReplayIO::adopt_inputs(out, std::move(replay.inputs), tps);
    return true;
}

bool ReplayIO::write_tcbot(const fs::path& path, const MacroData& macro) {
    ByteWriter out;
    out.data.assign(TcbotReplay::header.begin(), TcbotReplay::header.end());
    size_t meta = out.data.size();
    out.data.resize(meta + TcbotReplay::meta_size, 0);
    out.data[meta] = 2;
    out.data[meta + 2] = TcbotReplay::tps_flag;
    float tps = macro.fps;
    std::memcpy(out.data.data() + meta + 4, &tps, sizeof(tps));
    auto inputs = ReplayIO::sorted_inputs(macro);
    for (auto& input : inputs) {
        long long shifted = (long long)input.number + 2;
        if (shifted > std::numeric_limits<int>::max()) shifted = std::numeric_limits<int>::max();
        input.number = (int)shifted;
    }
    TcbotReplay::write_v2(out, inputs);
    return out.save(path);
}
