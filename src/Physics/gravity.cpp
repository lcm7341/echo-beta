// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#include "Physics/gravity.hpp"

#include <Geode/Geode.hpp>

#include "Hack/trajectory.hpp"
#include "Logic/logic.hpp"
#include "Physics/player.hpp"

using namespace geode::prelude;

namespace phys {
void flipGravityInner(PlayerObject* player, bool gravity) {
    bool icon2 = false;
    if (auto* layer = GJBaseGameLayer::get()) {
        icon2 = player == layer->m_player2 || Trajectory::get().is_second_fake(player);
    }
    bool ignoreUpsideDown = player->m_isDart && Logic::get().maintain_gravity && Logic::get().records_player(icon2);

    placeStreakPointState(player);
    if (!ignoreUpsideDown) {
        player->m_isUpsideDown = gravity;
    }
    gameEvent(player, gravity ? (int)GJGameEvent::GravityInverted : (int)GJGameEvent::GravityRestored);

    player->m_lastFlipTime = player->m_totalTime;
    player->m_collidedBottomMaxY = 0.;
    player->m_collidedTopMinY = 0.;
    player->m_unkA29 = false;
    if (player->m_wasOnSlope || player->m_isOnSlope) {
        player->m_slopeFlipGravityRelated = !player->m_slopeFlipGravityRelated;
    }

    player->m_collisionLogTop->removeAllObjects();
    player->m_collisionLogBottom->removeAllObjects();
    player->m_collisionLogLeft->removeAllObjects();
    player->m_collisionLogRight->removeAllObjects();

    player->m_unk50C = -1;
    player->m_unk510 = -1;
    player->m_lastCollisionBottom = -1;
    player->m_lastCollisionTop = -1;
    player->m_lastCollisionLeft = -1;
    player->m_lastCollisionRight = -1;

    player->m_yVelocity = player->m_yVelocity * 0.5;
    player->m_isOnGround = false;
    if (player->m_isBall) {
        player->m_isRotating = false;
        player->m_isBallRotating = false;
        player->m_isBallRotating2 = false;
        player->m_rotationSpeed = 0.;
        player->runBallRotation2();
    }
}

void flipGravity(PlayerObject* player, bool gravity, bool linkPartner) {
    if (player->m_isUpsideDown == gravity) return;

    auto& gameState = GJBaseGameLayer::get()->m_gameState;

    flipGravityInner(player, gravity);
    if (auto* layer = player->m_gameLayer;
        !player->m_isBall && layer && !layer->m_freezeStartCamera && !player->m_isDart && !player->m_maybeReducedEffects) {
        player->m_shouldTryPlacingCheckpoint = true;
        activateStreakState(player);
    }
    if (!linkPartner || !Trajectory::get().isFake(player) ||
        gameState.m_unkBool31 || !gameState.m_isDualMode ||
        GJBaseGameLayer::get()->m_levelSettings->m_twoPlayerMode) return;
    auto* p1 = player;
    auto* p2 = Trajectory::get().otherFake(player);
    if (!p2) return;
    if (!(p1->m_isShip == p2->m_isShip && p1->m_isBall == p2->m_isBall &&
          p1->m_isBird == p2->m_isBird &&
          p1->m_isSpider == p2->m_isSpider &&
          p1->m_isRobot == p2->m_isRobot &&
          p1->m_isSwing == p2->m_isSwing)) return;
    flipGravity(p2, !gravity);
}

void propellPlayer(PlayerObject* player, float force, bool, int) {
    const float smallVehicleGravity = 0.8f;
    const float forceScale = 16.f;
    const double ballDamping = 0.6000000238418579;
    player->m_maybeIsBoosted = true;
    player->m_isOnGround2 = false;
    player->m_isOnGround = false;
    player->m_shouldTryPlacingCheckpoint = true;
    player->m_isOnSlope = false;
    player->m_wasOnSlope = false;

    float gravity = player->m_vehicleSize != 1.f ? smallVehicleGravity : 1.f;
    float gravityCoefficient = player->m_isUpsideDown ? -1.f : 1.f;
    float scaled = gravityCoefficient * (force * forceScale);
    scaled *= gravity;
    double velocity = scaled;
    player->setYVelocity(velocity, 0);

    if (player->m_isBall || player->m_isSpider || player->m_isSwing) {
        player->m_yVelocity *= ballDamping;
    }

    if (!player->m_isLocked && !player->m_isDashing) {
        player->m_isRotating = false;
        player->m_isBallRotating = false;
        player->m_isBallRotating2 = false;
        player->m_rotationSpeed = 0.;

        if (!player->m_isBall) {
            player->runNormalRotation(false, 1.f);
        } else {
            player->runBallRotation(1.f);
        }
    }

    activateStreakState(player);
    player->m_lastGroundedPos = player->getPosition();
}

void bumpPlayer(PlayerObject* player, float force, int p2, bool playBumpEffect,
                GameObject* object) {
    if (player->m_isPlatformer || !player->m_fixRobotJump) {
        player->m_touchedPad = true;
    }

    if (p2 != 44) {
        propellPlayer(player, force, playBumpEffect, p2);
        player->m_isAccelerating = false;
        if (p2 == 34) {
            player->m_isAccelerating = true;
            player->m_lastGroundedPos = CCPoint{0.0, 0.0};
        }

        return;
    }

    if (!object) {
        player->spiderTestJumpInternal(false);
        gameEvent(player, (int)GJGameEvent::SpiderTeleport);
        return;
    }

    if (!player->m_isSideways) {
        if (player->m_isUpsideDown != object->isFacingDown()) {
            flipGravity(player, !player->m_isUpsideDown);
        }
    } else {
        if (player->m_isUpsideDown != object->isFacingLeft()) {
            flipGravity(player, !player->m_isUpsideDown);
        }
    }

    player->spiderTestJumpInternal(false);
    gameEvent(player, (int)GJGameEvent::SpiderTeleport);
}
}
