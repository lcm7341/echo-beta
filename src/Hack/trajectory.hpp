#pragma once

#include <array>
#include <chrono>

#include <Geode/Geode.hpp>

#include <unordered_set>
#include <vector>

class Trajectory {
public:
	PlayerObject* fake1 = nullptr;
	PlayerObject* fake2 = nullptr;

	static Trajectory& get() {
		static Trajectory value;
		return value;
	}

	void attach(PlayLayer* play);
	void detach();
	void syncSeat(PlayLayer* play);
	cocos2d::CCDrawNode* overlayNode() const { return node; }
	bool attached() const { return node != nullptr; }
	bool drawing() const { return busy; }
	void setDelta(float dt) { delta = dt; }
	void noteButton(bool player1, bool down);
	void refresh(PlayLayer* play);
	// Pause Before Kill without a drawn trajectory: one predicted step of the live input per
	// recorded player, setting deathNextP1/P2.
	void lookahead(PlayLayer* play);
	void invalidate() { cached = false; }
	bool survives(PlayLayer* play, bool player1, bool press, int delay);

	bool isFake(PlayerObject* player) const {
		return player && (player == fake1 || player == fake2);
	}
	PlayerObject* otherFake(PlayerObject* player) const {
		return player == fake1 ? fake2 : fake1;
	}
	bool markDead(PlayerObject* player);
	// During predict, solid/slope kills may hit the live body; count them on the matching fake.
	bool markPredictDeath(PlayerObject* player);
	bool fakeActivated(PlayerObject* player, EnhancedGameObject* object) const;
	bool realActivated(PlayerObject* player, EnhancedGameObject* object) const;
	void rememberActivation(EnhancedGameObject* object, PlayerObject* player);
	// A fake touched a dual (on) or solo (off) portal (Physics/collisions.cpp). Does what
	// GJBaseGameLayer::toggleDualMode does to the players, on the fakes.
	void dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on);

private:
	enum class Click { Hold, Swift, Release, Replay };
	struct Key {
		int frame = -1;
		float p1[6] = {};
		float p2[6] = {};
		bool dual = false;
		bool heldP1 = false;
		bool heldP2 = false;
		bool dead = false;
		bool playing = false;
		bool recording = false;
		bool hold = false;
		bool swift = false;
		bool release = false;
		bool dashOrbs = false;
		bool blackOrbs = false;
		bool padBlack = false;
		bool padBlue = false;
		bool pauseKill = false;
		bool pauseLength = false;
		unsigned pauseMs = 0;
		bool hideP1 = false;
		bool hideP2 = false;
		float seconds = 0.f;
		uint32_t orbs = 0;
		float width = 0.f;
		float zoom = 0.f;
		float color[4] = {};
		float swiftColor[4] = {};
		float releaseColor[4] = {};
		bool operator==(Key const&) const = default;
	};

	cocos2d::CCDrawNode* node = nullptr;
	bool busy = false;
	bool cached = false;
	Key lastKey;
	float delta = 0.f;
	// Fake currently inside checkCollisions — solid/slope kills may destroy the live body instead.
	PlayerObject* collidingFake = nullptr;
	bool deadP1 = false;
	bool deadP2 = false;
	// Dual portals inside a prediction: the live second player fake2 starts from, whether fake2
	// may join, whether the pair is being predicted, and what the portals did this step.
	PlayerObject* joinReal = nullptr;
	bool joinable = false;
	bool pairing = false;
	bool joined = false;
	bool parted = false;
	bool heldP1 = false;
	bool heldP2 = false;
	std::unordered_set<EnhancedGameObject*> usedP1;
	std::unordered_set<EnhancedGameObject*> usedP2;

	void park(PlayerObject* player);
	PlayerObject* spawnFake(PlayLayer* play, char const* id);
	Key makeKey(PlayLayer* play) const;
	void seed(PlayerObject* fake, PlayerObject* real);
	void press(PlayLayer* play, PlayerObject* fake, Click click);
	void simulate(PlayLayer* play, bool player1, Click click, bool both, bool draw, int limit, int measure);
	bool advance(PlayLayer* play, PlayerObject* fake, float stepDelta, bool draw, Click click);
	std::vector<cocos2d::CCPoint> paths[2];
	void flushPath(PlayLayer* play, PlayerObject* fake, Click click);
	void drawDeath(PlayerObject* fake);
	cocos2d::ccColor4F colorFor(PlayerObject* fake, Click click) const;
	float lineWidth(PlayLayer* play) const;
};
