#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <vector>



using namespace geode::prelude;

// Packed replays (.echo, magic "ECH2"). A click is mostly predictable from the clicks before it, so the
// file stores the surprises, not the clicks. The writer and the reader run the same guesses. A guess that
// comes true costs almost nothing. A wrong guess is what actually gets written.
//
//   v1  raw header, then one Elias-gamma frame gap per click. Kept so old files still open.
//   v2  same raw header, but the clicks are range-coded from "what just happened".
//   v3  the header is range-coded too, and repeated click patterns collapse to a single "yes, again" bit.
//   v4  v3 plus the TPS changes that happen in the middle of a macro.
//   The seed flag, when set, stores the attempt's random seed after those sections. Older readers ignore it.
//
// A channel is one button on one player: 0 jump, 1 left, 2 right, then the same three for player 2.
// A hold bit is 1 for a press and 0 for a release. Frames are stored as the gap since the previous click,
// so a click on the same frame is a gap of 0.
//
// Every code() below is shared by the writer and the reader. The writer passes the real bit in and the
// reader ignores that argument and returns the bit it decoded. Both then update the same probabilities,
// which is why the two sides stay in lockstep. Do not change a constant or the order of these calls
// unless you are writing a new file version. Old files would stop decoding.

// v1 only. Bits come out of each byte from the top. gamma() reads an Elias-gamma integer: count the
// leading zeros, then read that many bits after the 1. The stored gap is one bigger than the real gap,
// because gamma cannot represent 0. A decoded 0 means the stream ended early.
struct BitReader {
	uint8_t const* data = nullptr;
	size_t size = 0;
	size_t index = 0;
	int left = 0;
	uint8_t acc = 0;

	int bit() {
		if (left == 0) {
			if (index >= size) return 0;
			acc = data[index++];
			left = 8;
		}
		int value = (acc >> 7) & 1;
		acc <<= 1;
		--left;
		return value;
	}
	uint32_t gamma() {
		uint32_t zeros = 0;
		while (bit() == 0) {
			if (++zeros > 31 || index > size) return 0;
		}
		uint32_t n = 1;
		for (uint32_t i = 0; i < zeros; ++i) n = (n << 1) | static_cast<uint32_t>(bit());
		return n;
	}
};

// Chance of a 0, out of 65536. Starts at 50/50. Each bit seen pulls it toward that bit, quickly at first
// and then slower, and it is never allowed to reach 0 or 65536 (a certain bit would make the range coder
// unable to encode the opposite). v2 uses this. v3 uses Tally instead.
struct Odds {
	uint16_t value = 1 << 15;
	uint8_t seen = 0;

	uint32_t chance() const {
		return std::clamp<uint32_t>(value, 64, 65535 - 64);
	}
	void learn(int bit) {
		int target = bit ? 0 : 65535;
		value = static_cast<uint16_t>(value + (target - static_cast<int>(value)) / (static_cast<int>(seen) + 2));
		if (seen < 10) ++seen;
	}
};

// Chance of a 1, in 32-bit fixed point (1<<31 is 50/50). zero16() is the chance of a 0 out of 65536, which
// is what the range coder wants. one12() is a coarser 12-bit chance for the mixer below. `cap` is how many
// bits this context is allowed to learn from. A rare context keeps a low cap so it adapts in a few clicks.
// A context that sees thousands of bits (the match model) uses a high cap so it can settle on a precise odds.
struct Tally {
	uint32_t one = 1u << 31;
	uint16_t seen = 0;

	uint32_t zero16() const {
		return 65536 - std::clamp<uint32_t>(one >> 16, 1, 65535);
	}
	int one12() const {
		return static_cast<int>(one >> 20);
	}
	void learn(int bit, int cap) {
		int64_t target = bit ? 0xFFFFFFFFll : 0;
		one = static_cast<uint32_t>(one + (target - static_cast<int64_t>(one)) / (seen + 2));
		if (seen < cap) ++seen;
	}
};

// How many observations a probability may learn from before it stops getting sharper.
constexpr int kQuick = 30;
constexpr int kSteady = 4000;

// Arithmetic (range) coder, the same idea as LZMA. Instead of writing one byte per bit, both sides share a
// number range. A likely bit only nibbles that range, so many likely bits fit in one output byte. An unlikely
// bit cuts the range in half and forces a new byte out.
//
// bit(zero, value): `zero` is the chance of 0 out of 65536. The writer passes the real bit. The reader passes
// anything and returns the bit it found.
//
// Bytes are not emitted immediately. A later bit can carry into a byte already "written", so `cache` holds the
// byte that might still change and `pending` counts the 0xFF bytes waiting on that carry. shiftLow() resolves
// the carry once the top of `low` is known. finish() flushes the last bytes and strips trailing zeros.
struct RangeWriter {
	std::vector<uint8_t> bytes;
	uint64_t low = 0;
	uint32_t range = 0xFFFFFFFF;
	uint8_t cache = 0;
	uint64_t pending = 1;
	bool first = true;
	bool failed = false;

	void shiftLow() {
		if (static_cast<uint32_t>(low) < 0xFF000000u || (low >> 32) != 0) {
			uint8_t carry = static_cast<uint8_t>(low >> 32);
			uint8_t out = cache;
			do {
				if (!first) bytes.push_back(static_cast<uint8_t>(out + carry));
				first = false;
				out = 0xFF;
			} while (--pending != 0);
			cache = static_cast<uint8_t>(low >> 24);
		}
		++pending;
		low = (low & 0x00FFFFFF) << 8;
	}
	int bit(uint32_t zero, int value) {
		uint32_t bound = (range >> 16) * zero;
		if (value) {
			low += bound;
			range -= bound;
		} else {
			range = bound;
		}
		while (range < (1u << 24)) {
			range <<= 8;
			shiftLow();
		}
		return value;
	}
	int code(Tally& tally, int value, int cap = kQuick) {
		bit(tally.zero16(), value);
		tally.learn(value, cap);
		return value;
	}
	void finish() {
		for (int zeros = 32; zeros > 0; --zeros) {
			uint64_t mask = (uint64_t{1} << zeros) - 1;
			uint64_t rounded = (low + mask) & ~mask;
			if (rounded < low + range) {
				low = rounded;
				break;
			}
		}
		for (int i = 0; i < 5; ++i) shiftLow();
		while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
	}
};

// Mirror of RangeWriter. start() loads the first four bytes into `value`, which is the point currently inside
// the range. Each bit asks which half of the range that point sits in. Past the end of the buffer, next()
// returns 0 for a while (the flush bytes) and then marks the stream broken.
struct RangeReader {
	uint8_t const* data = nullptr;
	size_t size = 0;
	size_t index = 0;
	uint32_t range = 0xFFFFFFFF;
	uint32_t value = 0;
	bool failed = false;

	uint8_t next() {
		if (index >= size) {
			if (++index > size + 64) failed = true;
			return 0;
		}
		return data[index++];
	}
	void start() {
		for (int i = 0; i < 4; ++i) value = (value << 8) | next();
	}
	int bit(uint32_t zero, int) {
		uint32_t bound = (range >> 16) * zero;
		int result = value >= bound ? 1 : 0;
		if (result) {
			value -= bound;
			range -= bound;
		} else {
			range = bound;
		}
		while (range < (1u << 24)) {
			range <<= 8;
			value = (value << 8) | next();
		}
		return result;
	}
	int code(Odds& odds, int) {
		int result = bit(odds.chance(), 0);
		odds.learn(result);
		return result;
	}
	int code(Tally& tally, int, int cap = kQuick) {
		int result = bit(tally.zero16(), 0);
		tally.learn(result, cap);
		return result;
	}
};

// A small integer is stored as "how many bits long is it", then the bits under its leading 1. The length is
// what varies (a 1-frame gap and a 400-frame gap), so it gets its own contexts (`slot`). The first four bits
// under the leading 1 get a tiny tree (`high`) because they decide the rough size. The leftover low bits
// (`low`) are nearly noise and share one context per position.
// SlotBits is how many bits the length itself is allowed. MaxSlot is the longest number this model will accept.
template <int SlotBits, int MaxSlot, class Cell = Odds>
struct NumberOdds {
	std::array<Cell, 1 << SlotBits> slot{};
	std::array<std::array<Cell, 16>, MaxSlot + 1> high{};
	std::array<std::array<Cell, MaxSlot>, MaxSlot + 1> low{};
};

template <class Coder, int SlotBits, int MaxSlot, class Cell>
uint64_t codeNumber(Coder& coder, NumberOdds<SlotBits, MaxSlot, Cell>& odds, uint64_t number) {
	int length = std::bit_width(number);
	int node = 1;
	for (int i = SlotBits - 1; i >= 0; --i) node = (node << 1) | coder.code(odds.slot[node], (length >> i) & 1);
	length = node - (1 << SlotBits);
	if (length > MaxSlot) {
		coder.failed = true;
		return 0;
	}
	if (length <= 1) return static_cast<uint64_t>(length);
	uint64_t result = 1;
	int tree = 1;
	for (int i = length - 2; i >= 0; --i) {
		int depth = length - 2 - i;
		int wanted = static_cast<int>((number >> i) & 1);
		int bit = depth < 4 ? coder.code(odds.high[length][tree], wanted) : coder.code(odds.low[length][i], wanted);
		if (depth < 4) tree = (tree << 1) | bit;
		result = (result << 1) | static_cast<uint64_t>(bit);
	}
	return result;
}

// Fold a signed delta into an unsigned integer so -1 and +1 are both small. Used for player positions,
// where the change since the last sample is usually tiny and often negative.
uint64_t zigzag(int64_t value) {
	return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}
int64_t unzigzag(uint64_t value) {
	return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

// Optional debug pose stored with each click when the file's sample flag is set: x, y, rotation as float
// bits and the two velocities as double bits. Each field is the difference from that player's previous
// sample, on the raw bits, not on the float value. Smooth movement keeps those differences small.
// The reader writes the rebuilt bits back onto the Frame. The writer already has them and only updates
// its own running previous-value, so both sides learn the same contexts.
template <class Coder, class Numbers>
void codeSamples(Coder& coder, std::vector<Frame> const& inputs, std::vector<FramePose>& poses, Numbers& sample) {
	constexpr bool reading = std::is_same_v<Coder, RangeReader>;
	if constexpr (reading) poses.resize(inputs.size());
	std::array<std::array<uint64_t, 5>, 2> previous{};
	for (size_t i = 0; i < inputs.size() && !coder.failed; ++i) {
		FramePose pose = i < poses.size() ? poses[i] : FramePose{};
		int player = inputs[i].player2 ? 1 : 0;
		std::array<uint64_t, 5> raw{
			std::bit_cast<uint32_t>(pose.x),
			std::bit_cast<uint32_t>(pose.y),
			std::bit_cast<uint32_t>(pose.rotation),
			std::bit_cast<uint64_t>(pose.yVelocity),
			std::bit_cast<uint64_t>(pose.xVelocity)
		};
		for (int field = 0; field < 5; ++field) {
			bool wide = field >= 3;
			int64_t change = wide
				? static_cast<int64_t>(raw[field] - previous[player][field])
				: static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(raw[field]) - static_cast<uint32_t>(previous[player][field])));
			int64_t coded = unzigzag(codeNumber(coder, sample[player * 5 + field], zigzag(change)));
			raw[field] = wide
				? previous[player][field] + static_cast<uint64_t>(coded)
				: static_cast<uint32_t>(static_cast<uint32_t>(previous[player][field]) + static_cast<uint32_t>(coded));
			previous[player][field] = raw[field];
		}
		if constexpr (reading) {
			poses[i].x = std::bit_cast<float>(static_cast<uint32_t>(raw[0]));
			poses[i].y = std::bit_cast<float>(static_cast<uint32_t>(raw[1]));
			poses[i].rotation = std::bit_cast<float>(static_cast<uint32_t>(raw[2]));
			poses[i].yVelocity = std::bit_cast<double>(raw[3]);
			poses[i].xVelocity = std::bit_cast<double>(raw[4]);
		}
	}
}

// v2 contexts. There is no writer for v2 anymore. New files are v3 or v4. This only has to read old files,
// but it still has to learn the probabilities in the same order the old writer did.
// player: was the last click player 2, given the previous channel and whether it was a press.
// button: two bits, jump/left/right. The value 3 is unused and means the file is corrupt.
// hold: press or release, given whether this channel is currently held.
// delta: the frame gap, split by press-vs-release and "same channel as last time".
// sample: the ten debug fields (2 players, 5 values each).
struct InputOdds {
	std::array<Odds, 12> player{};
	std::array<std::array<Odds, 4>, 24> button{};
	std::array<Odds, 6> hold{};
	std::array<NumberOdds<6, 32>, 4> delta{};
	std::array<NumberOdds<7, 64>, 10> sample{};
};

// v2 clicks, in frame order. `state` per channel is 0 unseen, 1 held, 2 released.
void readVersion2(RangeReader& coder, std::vector<Frame>& inputs, std::vector<FramePose>& poses, size_t count, bool samples) {
	auto odds = std::make_unique<InputOdds>();
	std::array<int, 6> state{};
	int lastChannel = 0;
	int lastHold = 0;
	int lastFrame = 0;
	for (size_t i = 0; i < count && !coder.failed; ++i) {
		Frame input;
		int context = lastChannel * 2 + lastHold;
		int player2 = coder.code(odds->player[context], 0);
		auto& button = odds->button[context * 2 + player2];
		int high = coder.code(button[1], 0);
		int low = coder.code(button[2 + high], 0);
		int code = (high << 1) | low;
		if (code == 3) {
			coder.failed = true;
			break;
		}
		int channel = player2 * 3 + code;
		int hold = coder.code(odds->hold[state[channel] * 2 + lastHold], 0);
		auto& gap = odds->delta[hold * 2 + (channel == lastChannel ? 1 : 0)];
		uint64_t step = codeNumber(coder, gap, 0);
		if (step > static_cast<uint64_t>(INT32_MAX - lastFrame)) {
			coder.failed = true;
			break;
		}
		int frame = lastFrame + static_cast<int>(step);
		input.number = frame;
		input.holding = hold != 0;
		input.player2 = player2 != 0;
		input.button = ReplayIO::unpackButton(code);
		inputs.push_back(input);
		state[channel] = hold ? 1 : 2;
		lastChannel = channel;
		lastHold = hold;
		lastFrame = frame;
	}
	if (samples && !coder.failed) codeSamples(coder, inputs, poses, odds->sample);
}

// v3 mixes several guesses instead of trusting one. Probabilities multiply badly, so each guess is converted
// to a signed score (stretch) where they can be added, then converted back (squash). The curve is a hand-tuned
// sigmoid: score -2047..2047 maps to a probability 1..4095. stretch is the inverse, filled in once by walking
// every score. Both sides build the same table, so this stays deterministic.
struct Logistic {
	static int squash(int d) {
		static constexpr int curve[33] = {1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
			2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094};
		if (d > 2047) return 4095;
		if (d < -2047) return 1;
		int w = d & 127;
		int i = (d >> 7) + 16;
		return (curve[i] * (128 - w) + curve[i + 1] * w + 64) >> 7;
	}
	static int stretch(int p) {
		static auto const table = [] {
			std::array<int16_t, 4096> result{};
			int at = 0;
			for (int x = -2047; x <= 2047; ++x) {
				int v = squash(x);
				for (int i = at; i <= v; ++i) result[i] = static_cast<int16_t>(x);
				at = v + 1;
			}
			for (int i = at; i < 4096; ++i) result[i] = 2047;
			return result;
		}();
		return table[p];
	}
};

// Weights for N guesses plus one bias term (the extra slot). They start equal. codeBlend nudges a weight up
// when that guess agreed with the bit that actually happened, and down when it didn't. `rate` is how hard
// to nudge. Integer math only, so the reader applies the exact same nudges as the writer.
template <size_t N>
struct Blend {
	std::array<int32_t, N + 1> weight;
	Blend() {
		weight.fill((1 << 16) / static_cast<int32_t>(N));
		weight[N] = 0;
	}
};

template <class Coder, size_t N>
int codeBlend(Coder& coder, std::array<Tally*, N> const& cells, Blend<N>& blend, int bit, int rate) {
	std::array<int, N + 1> st;
	int64_t dot = 0;
	for (size_t i = 0; i < N; ++i) {
		st[i] = Logistic::stretch(cells[i]->one12());
		dot += static_cast<int64_t>(blend.weight[i]) * st[i];
	}
	st[N] = 256;
	dot += static_cast<int64_t>(blend.weight[N]) * st[N];
	int one = std::clamp(Logistic::squash(static_cast<int>(dot >> 16)), 1, 4095);
	bit = coder.bit(65536 - (static_cast<uint32_t>(one) << 4), bit);
	int error = ((bit << 12) - one) * rate;
	for (size_t i = 0; i <= N; ++i) blend.weight[i] += (st[i] * error) >> 10;
	for (auto* cell : cells) cell->learn(bit, kQuick);
	return bit;
}

// Coarse size class for a run length or a frame gap. 0..3 stay themselves. Bigger values share a bucket with
// other values of about the same magnitude, capped at 39. This lets "a 2-click repeat" and "a 5000-click
// repeat" learn separate odds without one context per exact length.
int lengthBucket(uint64_t length) {
	if (length < 4) return static_cast<int>(length);
	int width = std::bit_width(length);
	return std::min(4 + (width - 3) * 2 + static_cast<int>((length >> (width - 2)) & 1), 39);
}

// One frame-gap model, laid out as a flat array so a bit's context is just an index.
//   [0, 64)             the 6-bit length, walked as a binary tree (node 1 is the root)
//   [64, 64+33*16)      the first four bits under the leading 1, per length
//   the rest            the remaining low bits, per length and bit position
constexpr int kSlotNodes = 64;
constexpr int kHighNodes = 33 * 16;
using GapTree = std::array<Tally, kSlotNodes + kHighNodes + 33 * 32>;

// Every probability table v3 needs. EventOdds is huge on purpose: most of these cells stay near 50/50 and
// only the ones a macro actually hits learn anything. It is heap-allocated at the call site for that reason.
//
// match        "the repeated-pattern guess is right", by how long the repeat already is and how big the next gap looks.
// channel*     which button comes next. Three guesses are mixed: the last two clicks, the last click alone,
//              and which of jump/left are currently held (base 3, both players; right is left out to keep this smaller).
// hold*        press or release. Mixed from the recent gap, whether this button is already held, and the last gap's size.
// gap*         the frame gap. Three copies of GapTree (this kind of click, a similar earlier gap, the previous gap)
//              are mixed. `base` is hold*2 + "same button as last time", so presses and releases learn apart.
// sample       debug pose, same ten fields as v2.
// recent       hash of the last four clicks -> where in the history that pattern ended. -1 means never seen.
struct EventOdds {
	std::array<std::array<Tally, 6>, 40> match{};
	std::array<std::array<Tally, 5>, 144> channelPair{};
	std::array<std::array<Tally, 5>, 12> channelLast{};
	std::array<std::array<Tally, 5>, 81 * 12> channelHeld{};
	std::array<Blend<3>, 5> channelBlend{};
	std::array<Tally, 72> holdShort{};
	std::array<Tally, 12> holdState{};
	std::array<Tally, 80> holdGap{};
	std::array<Blend<3>, 2> holdBlend{};
	std::array<GapTree, 4> gapKind{};
	std::array<GapTree, 4 * 40> gapSame{};
	std::array<GapTree, 4 * 40> gapLast{};
	std::array<Blend<3>, 4 * 3> gapBlend{};
	std::array<NumberOdds<7, 64, Tally>, 10> sample{};
	std::vector<int32_t> recent = std::vector<int32_t>(1 << 18, -1);
};

// Frame gap, mixed from the three GapTrees selected by the caller. `stage` is 0 for the length bits, 1 for the
// top four payload bits, 2 for the rest, because those three parts want different mixer weights.
template <class Coder>
uint64_t codeGap(Coder& coder, EventOdds& odds, int base, int sameBucket, int lastBucket, uint64_t number) {
	std::array<GapTree*, 3> trees{&odds.gapKind[base], &odds.gapSame[base * 40 + sameBucket], &odds.gapLast[base * 40 + lastBucket]};
	auto blended = [&](int node, int wanted, int stage) {
		std::array<Tally*, 3> cells{&(*trees[0])[node], &(*trees[1])[node], &(*trees[2])[node]};
		return codeBlend(coder, cells, odds.gapBlend[base * 3 + stage], wanted, 1);
	};
	int length = std::bit_width(number);
	int node = 1;
	for (int i = 5; i >= 0; --i) node = (node << 1) | blended(node, (length >> i) & 1, 0);
	length = node - kSlotNodes;
	if (length > 32) {
		coder.failed = true;
		return 0;
	}
	if (length <= 1) return static_cast<uint64_t>(length);
	uint64_t result = 1;
	int tree = 1;
	for (int i = length - 2; i >= 0; --i) {
		int depth = length - 2 - i;
		int wanted = static_cast<int>((number >> i) & 1);
		int bit = depth < 4
			? blended(kSlotNodes + length * 16 + tree, wanted, 1)
			: blended(kSlotNodes + kHighNodes + length * 32 + i, wanted, 2);
		if (depth < 4) tree = (tree << 1) | bit;
		result = (result << 1) | static_cast<uint64_t>(bit);
	}
	return result;
}

// Header numbers. One value per file has nothing to learn, so every bit is stored at exactly 50/50.
// codeCount writes the bit length in 7 bits, then the bits under the leading 1.
// codeBits is for raw float and double patterns. Those often end in a string of zero bits, so the trailing-zero
// count is stored instead of the zeros themselves, and the lowest 1 is implied.
template <class Coder>
uint64_t codePlain(Coder& coder, uint64_t value, int width) {
	uint64_t result = 0;
	for (int i = width - 1; i >= 0; --i) result |= static_cast<uint64_t>(coder.bit(32768, static_cast<int>((value >> i) & 1))) << i;
	return result;
}
template <class Coder>
uint64_t codeCount(Coder& coder, uint64_t value) {
	int length = static_cast<int>(codePlain(coder, static_cast<uint64_t>(std::bit_width(value)), 7));
	if (length > 64) {
		coder.failed = true;
		return 0;
	}
	if (length <= 1) return static_cast<uint64_t>(length);
	return (uint64_t{1} << (length - 1)) | codePlain(coder, value, length - 1);
}
template <class Coder>
uint64_t codeBits(Coder& coder, uint64_t bits, int width) {
	int zeros = static_cast<int>(codePlain(coder, bits == 0 ? static_cast<uint64_t>(width) : static_cast<uint64_t>(std::countr_zero(bits)), width == 64 ? 7 : 6));
	if (zeros >= width) {
		if (zeros > width) coder.failed = true;
		return 0;
	}
	uint64_t rest = codePlain(coder, zeros + 1 < 64 ? bits >> (zeros + 1) : 0, width - zeros - 1);
	return ((rest << 1) | 1) << zeros;
}

// TPS changes after the clicks (v4 only). The frame is a gap from the previous change. The rate is its exact
// float bits, xor'd with the previous rate, so flipping between round values like 240 and 60 stays cheap.
template <class Coder>
void codeRates(Coder& coder, std::vector<RateChange>& rates, size_t count) {
	constexpr bool writing = std::is_same_v<Coder, RangeWriter>;
	int last = 0;
	uint32_t prev = 0;
	if constexpr (!writing) rates.clear();
	for (size_t i = 0; i < count && !coder.failed; ++i) {
		uint64_t gap = 0;
		uint32_t bits = 0;
		if constexpr (writing) {
			gap = static_cast<uint64_t>(std::max(rates[i].frame - last, 0));
			bits = std::bit_cast<uint32_t>(rates[i].fps) ^ prev;
		}
		gap = codeCount(coder, gap);
		bits = static_cast<uint32_t>(codeBits(coder, bits, 32));
		if constexpr (!writing) {
			if (gap > static_cast<uint64_t>(INT32_MAX - last)) {
				coder.failed = true;
				break;
			}
			last += static_cast<int>(gap);
			bits ^= prev;
			float fps = std::bit_cast<float>(bits);
			if (!(fps >= 1.f) || !std::isfinite(fps)) {
				coder.failed = true;
				break;
			}
			rates.push_back({last, fps});
			prev = bits;
		} else {
			last = rates[i].frame;
			prev = std::bit_cast<uint32_t>(rates[i].fps);
		}
	}
}

// Match finder. The last four clicks are hashed into one of 2^18 slots. Each slot remembers where that
// pattern ended in `history`. A hash collision is checked by walking up to 64 clicks backward.
constexpr int kMinMatch = 4;
constexpr uint64_t kVerify = 64;
constexpr uint64_t kMaxInputs = uint64_t{1} << 24;
constexpr uint64_t kHashSeed = 0x2545F4914F6CDD1Dull;
constexpr uint64_t kHashMix = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kHashAdd = 0x632BE59BD9B4E019ull;

uint32_t hashTail(std::vector<uint64_t> const& history) {
	uint64_t hash = static_cast<uint64_t>(kMinMatch) * kHashSeed;
	size_t start = history.size() - kMinMatch;
	for (size_t k = start; k < history.size(); ++k) hash = (hash ^ history[k]) * kHashMix + kHashAdd;
	return static_cast<uint32_t>(hash >> 46);
}

// One click packed into an integer: the frame gap, then 3 bits of channel, then the hold bit.
// The low 4 bits (channel and hold, no gap) are what the match finder compares when it asks "same buttons?".
constexpr int kSymbolShift = 4;
constexpr int kChannelMask = 7;

struct Symbol {
	uint64_t gap = 0;
	int channel = 0;
	int hold = 0;

	uint64_t bits() const {
		return (gap << kSymbolShift) | static_cast<uint64_t>((channel << 1) | hold);
	}
	static Symbol unpack(uint64_t raw) {
		return Symbol{
			raw >> kSymbolShift,
			static_cast<int>((raw >> 1) & kChannelMask),
			static_cast<int>(raw & 1),
		};
	}
	static Symbol fromInput(Frame const& input, int lastFrame) {
		int channel = (input.player2 ? 3 : 0) + ReplayIO::packButton(input.button);
		int hold = input.holding ? 1 : 0;
		return Symbol{static_cast<uint64_t>(input.number - lastFrame), channel, hold};
	}
	Frame toInput(int frame) const {
		Frame input;
		input.number = frame;
		input.holding = hold != 0;
		input.player2 = channel >= 3;
		input.button = ReplayIO::unpackButton(channel % 3);
		return input;
	}
	bool overflows(int lastFrame) const {
		return gap > static_cast<uint64_t>(INT32_MAX - lastFrame) || channel > 5;
	}
};

// v3/v4 clicks. First ask the match finder "is the next click the same one that followed this pattern last
// time?". Spam and copied sections become a long run of yes-bits. A no-bit falls through and pays for the
// channel, the press/release, and the frame gap.
//
// `history` is every click so far. `matched` is how many clicks into a repeated run we are, and `ahead` is
// the history index of the click we expect next. `misses` stays above 0 right after a wrong guess so the
// model can learn that a broken run usually stays broken for a bit.
template <class Coder>
void codeEvents(Coder& coder, EventOdds& odds, std::vector<Frame>& inputs, size_t count) {
	constexpr bool writing = std::is_same_v<Coder, RangeWriter>;
	std::vector<uint64_t> history;
	history.reserve(std::min<size_t>(count, 1u << 20));
	size_t ahead = 0;
	uint64_t matched = 0;
	int misses = 0;
	std::array<int, 6> state{};
	std::array<uint64_t, 12> kindGap{};
	int lastFrame = 0;
	auto symbolAt = [&](size_t back) {
		return history.size() > back ? static_cast<int>(history[history.size() - 1 - back] & 15) : 0;
	};
	for (size_t i = 0; i < count && !coder.failed; ++i) {
		uint64_t want = 0;
		if constexpr (writing) want = Symbol::fromInput(inputs[i], lastFrame).bits();

		uint64_t got = 0;
		bool guessed = false;
		// One bit: did the click we predicted from the earlier copy actually come next?
		if (matched > 0) {
			uint64_t guess = history[ahead];
			uint64_t guessGap = guess >> kSymbolShift;
			int gapShape = guessGap == 0 ? 0 : guessGap == 1 ? 1 : 2;
			int kind = gapShape * 2 + std::min(misses, 1);
			guessed = coder.code(odds.match[lengthBucket(matched)][kind], want == guess ? 1 : 0, kSteady) != 0;
			if (guessed) got = guess;
			misses = guessed ? 0 : misses + 1;
		}
		// Wrong guess, or no pattern yet. Pay for the button, then press/release, then the frame gap.
		if (!guessed) {
			int last = symbolAt(0);
			int prev = symbolAt(1);
			uint64_t lastGap = history.empty() ? 0 : history.back() >> kSymbolShift;
			int wantChannel = static_cast<int>((want >> 1) & kChannelMask);
			// Jump and left, for both players, packed in base 3. Right is not part of this context.
			int held = state[0] + state[1] * 3 + state[3] * 9 + state[4] * 27;
			auto channelBit = [&](int node, int wanted) {
				std::array<Tally*, 3> cells{
					&odds.channelPair[last * 12 + prev][node],
					&odds.channelLast[last][node],
					&odds.channelHeld[held * 12 + last][node],
				};
				return codeBlend(coder, cells, odds.channelBlend[node], wanted, 2);
			};
			int player2 = channelBit(0, wantChannel >= 3 ? 1 : 0);
			int high = channelBit(1 + player2, (wantChannel % 3) >> 1);
			int low = high ? 0 : channelBit(3 + player2, (wantChannel % 3) & 1);
			int channel = player2 * 3 + ((high << 1) | low);
			int same = channel == (last >> 1) ? 1 : 0;
			int base = (state[channel] * 2 + (last & 1)) * 2 + same;
			int shortGap = static_cast<int>(std::min<uint64_t>(lastGap, 5));
			int gapBand = (state[channel] == 1 ? 40 : 0) + lengthBucket(lastGap);
			std::array<Tally*, 3> holdCells{
				&odds.holdShort[base * 6 + shortGap],
				&odds.holdState[base],
				&odds.holdGap[gapBand],
			};
			int hold = codeBlend(coder, holdCells, odds.holdBlend[same], static_cast<int>(want & 1), 2);
			uint64_t gap = codeGap(
				coder,
				odds,
				hold * 2 + same,
				lengthBucket(kindGap[channel * 2 + hold]),
				lengthBucket(lastGap),
				want >> kSymbolShift
			);
			got = Symbol{gap, channel, hold}.bits();
		}
		Symbol symbol = Symbol::unpack(got);
		if (symbol.overflows(lastFrame)) {
			coder.failed = true;
			break;
		}
		int frame = lastFrame + static_cast<int>(symbol.gap);
		if constexpr (!writing) inputs.push_back(symbol.toInput(frame));
		int channel = symbol.channel;
		int hold = symbol.hold;
		uint64_t gap = symbol.gap;

		// Extend the current repeat, or search history for a previous copy of the last four clicks.
		if (matched > 0 && history[ahead] == got) {
			++matched;
			++ahead;
		} else {
			matched = 0;
		}
		history.push_back(got);
		if (history.size() >= kMinMatch) {
			uint32_t slot = hashTail(history);
			int32_t at = odds.recent[slot];
			if (matched == 0 && at >= 0) {
				uint64_t length = 0;
				while (length < static_cast<uint64_t>(at) && length < kVerify) {
					size_t here = history.size() - 1 - length;
					size_t there = static_cast<size_t>(at) - 1 - length;
					if (history[there] != history[here]) break;
					++length;
				}
				if (length >= kMinMatch) {
					matched = length;
					ahead = static_cast<size_t>(at);
				}
			}
			odds.recent[slot] = static_cast<int32_t>(history.size());
		}
		state[channel] = hold ? 1 : 2;
		kindGap[channel * 2 + hold] = gap;
		lastFrame = frame;
	}
}

// Jump is 0, left is 1, right is 2. Anything else, including a normal click, is jump.
int ReplayIO::packButton(int button) {
	if (button == static_cast<int>(PlayerButton::Left)) return 1;
	if (button == static_cast<int>(PlayerButton::Right)) return 2;
	return 0;
}
int ReplayIO::unpackButton(int code) {
	if (code == 1) return static_cast<int>(PlayerButton::Left);
	if (code == 2) return static_cast<int>(PlayerButton::Right);
	return static_cast<int>(PlayerButton::Jump);
}

// Little-endian raw bytes. v1 and v2 headers are plain structs. v3 puts the same numbers inside the range coder.
template <typename A1>
void ReplayIO::writePod(std::ostream& file, A1 const& value) {
	file.write(reinterpret_cast<char const*>(&value), sizeof(value));
}
template <typename A1>
bool ReplayIO::readPod(std::istream& file, A1& value) {
	file.read(reinterpret_cast<char*>(&value), sizeof(value));
	return static_cast<bool>(file);
}

// 7-bit chunks, low bits first, high bit set on every byte except the last. v2 uses this for attempts and click count.
bool readVarint(std::istream& file, uint64_t& value) {
	value = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		int byte = file.get();
		if (byte == EOF) return false;
		value |= static_cast<uint64_t>(byte & 0x7F) << shift;
		if ((byte & 0x80) == 0) return true;
	}
	return false;
}

// Flag bit 0: debug pose samples follow the clicks. Flag bit 2 (v2 only): fps is a whole number stored as a varint
// instead of a float. v3/v4 never set bit 2.
constexpr uint8_t kFlagSamples = 1;
constexpr uint8_t kFlagWholeFps = 2;
constexpr uint8_t kFlagSeed = 4;
// Recorded with Velocity Unrounding. Older Echo builds ignore the bit.
constexpr uint8_t kFlagUnrounded = 8;
// Came from Silicate; moving objects use Silicate's variance indexes. Older Echo builds ignore it.
constexpr uint8_t kFlagSilicateVariance = 16;

struct PackedBody {
	float fps = 240.f;
	float endX = 0.f;
	double seconds = 0.0;
	uint64_t attempts = 0;
	uint64_t seed = 0;
	bool hasSeed = false;
	bool unrounded = false;
	bool silicateVariance = false;
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	std::vector<RateChange> rates;
};

// Clicks have to come out in the same order they go in, or the gaps and the match finder see a different
// sequence on load. Frame first, then player 1 before player 2, then jump, left, right.
std::vector<Frame> inputsInPackOrder(std::vector<Frame> inputs, std::vector<FramePose>* poses) {
	std::vector<size_t> order(inputs.size());
	std::iota(order.begin(), order.end(), 0);
	for (auto& input : inputs) input.number = std::max(input.number, 0);
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		auto const& left = inputs[a];
		auto const& right = inputs[b];
		if (left.number != right.number) return left.number < right.number;
		if (left.player2 != right.player2) return left.player2 < right.player2;
		return ReplayIO::packButton(left.button) < ReplayIO::packButton(right.button);
	});
	std::vector<Frame> next;
	next.reserve(inputs.size());
	for (size_t i : order) next.push_back(inputs[i]);
	if (poses && poses->size() == inputs.size()) {
		std::vector<FramePose> sorted;
		sorted.reserve(poses->size());
		for (size_t i : order) sorted.push_back((*poses)[i]);
		*poses = std::move(sorted);
	}
	return next;
}

std::vector<uint8_t> readRest(std::istream& file) {
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

RangeReader openRange(std::vector<uint8_t> const& packed) {
	RangeReader body;
	body.data = packed.data();
	body.size = packed.size();
	body.start();
	return body;
}

// v1 debug samples are raw floats and doubles packed on the next byte after the bit stream, not range-coded.
// A short read stops the loop and leaves the remaining clicks without a pose. Old files sometimes did that.
void readVersion1Samples(std::vector<Frame> const& inputs, std::vector<FramePose>& poses, std::vector<uint8_t> const& packed, BitReader const& stream) {
	size_t cursor = (stream.index * 8 - static_cast<size_t>(stream.left)) / 8;
	if (stream.left != 0) ++cursor;
	auto readAt = [&](auto& pod) {
		if (cursor + sizeof(pod) > packed.size()) return false;
		std::memcpy(&pod, packed.data() + cursor, sizeof(pod));
		cursor += sizeof(pod);
		return true;
	};
	poses.resize(inputs.size());
	for (auto& pose : poses) {
		if (!readAt(pose.x) || !readAt(pose.y) || !readAt(pose.rotation)) break;
		if (!readAt(pose.yVelocity) || !readAt(pose.xVelocity)) break;
	}
}

// v1: reserved u16, float fps, float endX, double seconds, u32 attempts, u32 count, then the bit stream.
bool loadVersion1(std::istream& file, uint8_t flags, PackedBody& out) {
	uint16_t reserved = 0;
	uint32_t attempts = 0;
	uint32_t count = 0;
	if (!ReplayIO::readPod(file, reserved)) return false;
	if (!ReplayIO::readPod(file, out.fps) || !ReplayIO::readPod(file, out.endX) || !ReplayIO::readPod(file, out.seconds)) return false;
	if (!ReplayIO::readPod(file, attempts) || !ReplayIO::readPod(file, count)) return false;
	out.attempts = attempts;
	auto packed = readRest(file);
	BitReader stream;
	stream.data = packed.data();
	stream.size = packed.size();
	out.inputs.reserve(count);
	int frame = 0;
	for (uint32_t i = 0; i < count; ++i) {
		Frame input;
		input.holding = stream.bit() != 0;
		input.player2 = stream.bit() != 0;
		int code = (stream.bit() << 1) | stream.bit();
		input.button = ReplayIO::unpackButton(code);
		uint32_t delta = stream.gamma();
		if (delta == 0) return false;
		frame += static_cast<int>(delta - 1);
		input.number = frame;
		out.inputs.push_back(input);
	}
	if (flags & kFlagSamples) readVersion1Samples(out.inputs, out.poses, packed, stream);
	return true;
}

// v2: fps (float, or a varint whole number), float endX, double seconds, varint attempts, varint count,
// then one range-coded body.
bool loadVersion2(std::istream& file, uint8_t flags, PackedBody& out) {
	if (flags & kFlagWholeFps) {
		uint64_t whole = 0;
		if (!readVarint(file, whole)) return false;
		out.fps = static_cast<float>(whole);
	} else if (!ReplayIO::readPod(file, out.fps)) {
		return false;
	}
	if (!ReplayIO::readPod(file, out.endX) || !ReplayIO::readPod(file, out.seconds)) return false;
	uint64_t count = 0;
	if (!readVarint(file, out.attempts) || !readVarint(file, count)) return false;
	auto packed = readRest(file);
	RangeReader body = openRange(packed);
	out.inputs.reserve(static_cast<size_t>(std::min<uint64_t>(count, 1u << 20)));
	readVersion2(body, out.inputs, out.poses, static_cast<size_t>(count), flags & kFlagSamples);
	return !body.failed && out.inputs.size() == count;
}

// v3 and v4 have no raw header past the version byte. The range coder itself carries fps, endX, seconds,
// attempts, click count, the clicks, optional poses, and (v4) the TPS changes. The order is the format.
bool loadVersion3(std::istream& file, uint8_t flags, int version, PackedBody& out) {
	auto packed = readRest(file);
	RangeReader body = openRange(packed);
	auto odds = std::make_unique<EventOdds>();
	out.fps = std::bit_cast<float>(static_cast<uint32_t>(codeBits(body, 0, 32)));
	out.endX = std::bit_cast<float>(static_cast<uint32_t>(codeBits(body, 0, 32)));
	out.seconds = std::bit_cast<double>(codeBits(body, 0, 64));
	out.attempts = codeCount(body, 0);
	uint64_t count = codeCount(body, 0);
	if (body.failed || count > kMaxInputs || out.attempts > INT32_MAX) return false;
	out.inputs.reserve(static_cast<size_t>(std::min<uint64_t>(count, 1u << 20)));
	codeEvents(body, *odds, out.inputs, static_cast<size_t>(count));
	if (!body.failed && (flags & kFlagSamples)) codeSamples(body, out.inputs, out.poses, odds->sample);
	if (version == 4 && !body.failed) {
		uint64_t rateCount = codeCount(body, 0);
		if (body.failed || rateCount > (uint64_t{1} << 16)) return false;
		codeRates(body, out.rates, static_cast<size_t>(rateCount));
		if (body.failed || out.rates.size() != rateCount) return false;
	}
	if (!body.failed && (flags & kFlagSeed)) {
		out.seed = codeBits(body, 0, 64);
		out.hasSeed = !body.failed;
	}
	return !body.failed && out.inputs.size() == count;
}

void storePacked(PackedBody& body) {
	auto& value = Logic::get();
	value.inputs = std::move(body.inputs);
	value.poses = std::move(body.poses);
	value.inputIds.clear();
	value.touchInputs();
	Logic::get().refreshLiveHold();
	value.rates = std::move(body.rates);
	value.fps = body.fps > 0.f ? body.fps : value.fps;
	value.macroFps = value.fps;
	value.endX = body.endX;
	value.recordingSeconds = body.seconds;
	value.attempts = static_cast<int>(body.attempts);
	value.levelSeed = body.seed;
	value.levelSeedKnown = body.hasSeed;
	value.levelSeedFromFile = body.hasSeed;
	value.macroUnrounded = body.unrounded;
	value.macroSilicateVariance = body.silicateVariance;
	value.format = ReplayFormat::Meta;
}

// Current files are v3, or v4 when the macro changes TPS. The body order matches loadVersion3.
bool ReplayIO::writePackedEcho(std::filesystem::path const& path) {
	Logic::get().normalizeRates();
	auto const& value = Logic::get();
	auto poses = value.poses.size() == value.inputs.size() ? value.poses : std::vector<FramePose>{};
	auto inputs = inputsInPackOrder(value.inputs, poses.empty() ? nullptr : &poses);
	bool samples = value.saveDebug && poses.size() == inputs.size();
	bool rates = !value.rates.empty();

	auto odds = std::make_unique<EventOdds>();
	RangeWriter body;
	float headerFps = rates ? value.macroFps : static_cast<float>(value.sampleRate());
	codeBits(body, std::bit_cast<uint32_t>(headerFps), 32);
	codeBits(body, std::bit_cast<uint32_t>(value.endX), 32);
	codeBits(body, std::bit_cast<uint64_t>(value.recordingSeconds), 64);
	codeCount(body, static_cast<uint64_t>(std::max(value.attempts, 0)));
	codeCount(body, inputs.size());
	codeEvents(body, *odds, inputs, inputs.size());
	if (samples) codeSamples(body, inputs, poses, odds->sample);
	if (rates) {
		auto stored = value.rates;
		codeCount(body, stored.size());
		codeRates(body, stored, stored.size());
	}
	if (value.levelSeedKnown) codeBits(body, value.levelSeed, 64);
	body.finish();

	std::ofstream file(path, std::ios::binary);
	if (!file) return false;
	char magic[4] = {'E', 'C', 'H', '2'};
	file.write(magic, 4);
	uint8_t version = rates ? 4 : 3;
	uint8_t flags = samples ? kFlagSamples : 0;
	if (value.levelSeedKnown) flags = static_cast<uint8_t>(flags | kFlagSeed);
	if (value.macroUnrounded) flags = static_cast<uint8_t>(flags | kFlagUnrounded);
	if (value.macroSilicateVariance) flags = static_cast<uint8_t>(flags | kFlagSilicateVariance);
	ReplayIO::writePod(file, version);
	ReplayIO::writePod(file, flags);
	if (!body.bytes.empty()) file.write(reinterpret_cast<char const*>(body.bytes.data()), static_cast<std::streamsize>(body.bytes.size()));
	return static_cast<bool>(file);
}

// "ECH2", then one version byte and one flags byte. The rest depends on the version.
bool ReplayIO::readPackedEcho(std::filesystem::path const& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	char magic[4] = {};
	file.read(magic, 4);
	if (!file || std::string_view(magic, 4) != "ECH2") return false;
	uint8_t version = 0;
	uint8_t flags = 0;
	if (!ReplayIO::readPod(file, version) || !ReplayIO::readPod(file, flags)) return false;

	PackedBody body;
	bool loaded = false;
	body.unrounded = (flags & kFlagUnrounded) != 0;
	body.silicateVariance = (flags & kFlagSilicateVariance) != 0;
	if (version == 1) loaded = loadVersion1(file, flags, body);
	else if (version == 2) loaded = loadVersion2(file, flags, body);
	else if (version == 3 || version == 4) loaded = loadVersion3(file, flags, version, body);
	if (!loaded) return false;
	storePacked(body);
	return true;
}
