#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <array>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

// Velocity Unrounding, after chizz's mod (https://github.com/ch-zzzy/GDVelocityUnRounding):
// GD rounds the player's y velocity to three decimals after a jump, a collision, a boost and
// postCollision. This removes those five roundings and setYVelocity's. It changes physics, so a
// macro records whether it was on (Session::macroUnrounded) and plays back with the same.
namespace {
#ifdef GEODE_IS_WINDOWS
// Each block rounds xmm to 1/1000 and adds the integer part back. Without it the register keeps
// the unrounded value, which the next instruction stores. Original bytes are checked first.
struct Site {
	std::uintptr_t rva;
	std::size_t size;
	std::uint64_t fingerprint;
};
constexpr std::array kSites{
	Site{0x38c329, 0x24, 0xc4f7983894ccfe0bull}, // updateJump
	Site{0x213ea2, 0x32, 0xec65c38c7b82b7a9ull}, // checkCollisions
	Site{0x38dac7, 0x38, 0x7a9ae823fdb4a085ull}, // postCollision
	Site{0x39323b, 0x40, 0xbf27b9f4033918feull}, // collidedWithObjectInternal
	Site{0x39ff18, 0x32, 0x71836103b10abd48ull}, // boostPlayer
};
std::uint64_t fingerprint(std::uint8_t const* bytes, std::size_t size) {
	std::uint64_t hash = 14695981039346656037ull;
	for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
}
#endif
std::vector<Patch*> patches;
bool prepared = false;
bool available = false;
bool applied = false;

void prepare() {
	if (prepared) return;
	prepared = true;
	// chizz's mod patches the same bytes on its own setting; two owners would fight over them.
	if (Loader::get()->isModLoaded("chizz.velocity-unrounding")) {
		log::warn("Echo Velocity Unrounding off: chizz.velocity-unrounding is loaded");
		return;
	}
#ifdef GEODE_IS_WINDOWS
	auto base = geode::base::get();
	for (auto const& site : kSites) {
		if (fingerprint(reinterpret_cast<std::uint8_t const*>(base + site.rva), site.size) != site.fingerprint) {
			log::warn("Echo Velocity Unrounding off: native code differs at {:#x}", site.rva);
			return;
		}
	}
	for (auto const& site : kSites) {
		auto result = Mod::get()->patch(reinterpret_cast<void*>(base + site.rva), ByteVector(site.size, 0x90));
		if (!result) {
			log::warn("Echo Velocity Unrounding patch failed at {:#x}", site.rva);
			for (auto* patch : patches) (void)patch->disable();
			patches.clear();
			return;
		}
		auto* patch = result.unwrap();
		patch->setAutoEnable(false);
		(void)patch->disable();
		patches.push_back(patch);
	}
	available = true;
#endif
}
}

// Called every frame. Only a change of run state or of the setting while idle moves it, and the
// menu does not offer the setting while recording or playing.
void Session::syncUnrounding() {
	bool want = unroundedPhysics();
	if (want == applied) return;
	prepare();
	if (!available) return;
	for (auto* patch : patches) (void)(want ? patch->enable() : patch->disable());
	applied = want;
	log::info("Echo Velocity Unrounding {}", want ? "on" : "off");
}

class $modify(UnroundedPlayer, PlayerObject) {
	void setYVelocity(double velocity, int type) {
		if (applied) {
			m_yVelocity = velocity;
			return;
		}
		PlayerObject::setYVelocity(velocity, type);
	}
};
