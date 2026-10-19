// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#include "Physics/player.hpp"
#include "Hack/orbs.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/predicted_world.hpp"
#include "Physics/collisions.hpp"
#include "Logic/logic.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace phys {
void placeStreakPointState(PlayerObject* player) {
    if (!Logic::get().predicting) player->placeStreakPoint();
    else player->m_justPlacedStreak = false;
}

static bool flyingMode(PlayerObject* player) {
    return player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;
}

void activateStreakState(PlayerObject* player) {
    if (!Logic::get().predicting) {
        player->activateStreak();
        return;
    }
    if (player->levelFlipping() || GameManager::get()->m_editorEnabled || player->m_isHidden)
        return;
    player->m_fadeOutStreak = true;
}

static void countJumpState(PlayerObject* player) {
    if (player->m_playEffects) player->m_hasEverJumped = true;
}

static void finishRingJump(PlayerObject* player) {
    countJumpState(player);
}

static void dashArtForTrajectory(PlayerObject* player) {
    const int sidewaysDashArtAngle = 90;
    const float leftFlipAngle = 180.f;
    const float dashSpinSpeed = 288.f;
    const float maxDashSpinLength = 17.31f;
    const float maxDashSpinScale = 2.f;
    const float dashSpinSlope = 1.7f;
    const float dashSpinBase = 0.3f;
    for (auto* sprite : { player->m_iconSprite, player->m_iconGlow })
        if (sprite) sprite->setRotation(0.f);
    float dashArtAngle = (float)((double)(player->m_isSideways ? sidewaysDashArtAngle : 0) - player->m_dashAngle);
    if (player->m_isGoingLeft) dashArtAngle += leftFlipAngle;
    player->setRotation(dashArtAngle);
    if (flyingMode(player) || player->m_isRobot || player->m_isSpider) return;
    float spin = player->m_isSideways == player->m_isUpsideDown ? 1.f : -1.f;
    spin *= dashSpinSpeed;
    spin *= player->m_gravityMod;
    if (player->m_isPlatformer) {
        float dashLength = sqrtf((float)player->m_dashX * (float)player->m_dashX + (float)player->m_dashY * (float)player->m_dashY);
        spin *= dashLength > maxDashSpinLength ? maxDashSpinScale : dashLength / maxDashSpinLength * dashSpinSlope + dashSpinBase;
    }
    Trajectory::get().dash_spin_start(player, spin);
}

void startDashing(PlayerObject* player, DashRingObject* ring) {
    const float platformerDashAngleScale = 5.7700018882751465f;
    const float radiansPerDegree = 0.01745329238474369f;
    const float dashFlashDuration = 0.2f;
    const float dashFlashDelay = 0.1f;
    if (player->m_isDashing) return;

    if (!player->m_isDart && player->m_fadeOutStreak) player->m_fadeOutStreak = false;
    player->m_isDashing = true;
    player->m_lastLandTime = 0.;
    player->m_isRotating = false;
    player->m_isBallRotating2 = false;
    player->m_isBallRotating = false;
    player->m_rotationSpeed = 0.;

    player->m_dashRing = ring;
    player->m_dashStartTime = player->m_totalTime;
    float dashAngle = ring->getObjectRotation();
    if (ring->isFlipX()) {
        dashAngle += 180.0;
    }

    if (fabs((ring->getRotationX() - ring->getRotationY())) > 179.0) {
        dashAngle += 180.0;
    }
    dashAngle = fmodf(-dashAngle, 360.0);

    if (dashAngle < -180.0) {
        dashAngle += 360.0;
    } else if (dashAngle > 180.0) {
        dashAngle -= 360.0;
    }

    float sidewaysAngle = 0.0;
    if (player->m_isPlatformer) {
        sidewaysAngle = ring->m_dashSpeed * platformerDashAngleScale;
    } else {
        if (player->m_isSideways) {
            sidewaysAngle = -90.0;
        }
        if (player->m_isGoingLeft) {
            sidewaysAngle += 180.0;
        }

        float angleHelper = dashAngle;
        if (sidewaysAngle != 0) {
            angleHelper = dashAngle + sidewaysAngle;
            if (angleHelper < -180.0) {
                angleHelper += 360.0;
            } else if (angleHelper > 180.0) {
                angleHelper -= 360.0;
            }
        }

        if (fabs(angleHelper) > 90.0) {
            if (angleHelper <= 0.0) {
                dashAngle = -180.0;
            } else {
                dashAngle = 180.0;
            }

            angleHelper = dashAngle - angleHelper;
            if (angleHelper < -180.0) {
                angleHelper += 360.0;
            } else if (angleHelper > 180.0) {
                angleHelper -= 360.0;
            }
        }

        float angleHelper2 = 70.0;
        dashAngle = angleHelper;
        if (angleHelper > 70.0 || angleHelper < -70.0) {
            dashAngle = angleHelper2;

            if (angleHelper <= 0.0) {
                if ((angleHelper < 70.0) && (angleHelper > -180.0)) {
                    dashAngle = -70.0;
                }
            } else if ((angleHelper <= 70.0) || (angleHelper >= 180.0)) {
                dashAngle = -70.0;
            }
        }

        if (sidewaysAngle != 0.0) {
            dashAngle -= sidewaysAngle;
            if (dashAngle > 180.0) {
                dashAngle -= 360.0;
                sidewaysAngle = 1.0;
            }
            if (dashAngle < -180.0) {
                dashAngle += 360.0;
            }
        }
        sidewaysAngle = 1.0;
    }

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID)
    cocos2d::CCPoint dashDirection = cocos2d::ccpForAngle(dashAngle * radiansPerDegree);
#else
    cocos2d::CCPoint dashDirection = CCPoint{
        (float)std::cos(((float)dashAngle * 0.01745329)),
        (float)std::sin(((float)dashAngle * 0.01745329)),
    };
#endif
    cocos2d::CCPoint mult = dashDirection * sidewaysAngle;

    float x = mult.x;
    if (player->m_isSideways) {
        mult.x = mult.y;
        mult.y = x;
    }

    double doubleX = mult.x;
    player->m_dashX = doubleX;
    player->m_dashY = mult.y;
    if (!player->m_isPlatformer) {
        float ratio = mult.y / std::fabs(mult.x);
        player->m_dashY = ratio;
        player->m_dashX = std::fabs(player->m_dashY);
    } else if (doubleX >= 0.0) {
        if (doubleX > 0.0) {
            player->doReversePlayer(false);
        }
    } else {
        player->doReversePlayer(true);
    }

    player->m_dashAngle = dashAngle;
    if (!Logic::get().predicting) player->updateDashArt();
    else dashArtForTrajectory(player);
    if (player->m_playEffects && !ring->m_hasNoEffects) {
        player->m_flashTime = player->m_totalTime;
        player->m_flashDuration = dashFlashDuration;
        player->m_flashDelay = dashFlashDelay;
    }
    player->m_lastGroundedPos = cocos2d::CCPoint{0.0, 0.0};
    gameEvent(player, (int)GJGameEvent::DashStart);
}

void stopDashing(PlayerObject* player) {
    if (!player->m_isDashing) return;

    player->m_isDashing = false;
    player->m_lastLandTime = 0.0;
    if (player->m_isPlatformer && player->m_dashRing) {
        cocos2d::CCPoint boosted = {
            (float)player->m_dashX * player->m_dashRing->m_endBoost,
            (float)player->m_dashY * player->m_dashRing->m_endBoost,
        };
        player->m_isAccelerating = true;
        player->m_yVelocity = boosted.y;

        player->m_platformerXVelocity = boosted.x;
        player->m_affectedByForces = true;

        if (player->m_dashRing->m_stopSlide) {
            player->m_isAccelerating = false;
            player->m_affectedByForces = false;
        }
    }
    player->m_dashRing = nullptr;

    if (player->m_isPlatformer) {
        cocos2d::CCPoint dashLoc = {(float)player->m_dashX,
                                    (float)player->m_dashY};
        float locationNormal = sqrtf(player->m_dashX * player->m_dashX +
                                     player->m_dashY * player->m_dashY);

        if (locationNormal <= 17.3) {
            locationNormal = (locationNormal / 17.3) * 1.5 + 0.5;
        } else {
            locationNormal = 2.0;
        }

        float vehicleSizeDependant =
            player->m_vehicleSize == 1.0 ? 0.433333 : 0.333333;
        float sidewaysMod = player->m_isSideways ? -1 : 1;
        int upsideDownMod = player->m_isUpsideDown ? -180 : 180;
        int leftMod = player->m_isGoingLeft ? -1 : 1;

        player->m_rotationSpeed = (upsideDownMod * leftMod * sidewaysMod *
                                   player->m_gravityMod * locationNormal) /
                                  (vehicleSizeDependant);
        player->m_isRotating = true;
    }

    if (player->m_isBall) {
        if (player->m_isPlatformer) {
            player->m_isRotating = false;
            player->m_isBallRotating = false;
            player->m_rotationSpeed = 0.0;
        }

        player->runBallRotation(1.0);
    }
    if (!flyingMode(player) && !player->m_isRobot && !player->m_isSpider && player->m_iconSprite && player->m_iconGlow) {
        if (Trajectory::get().isFake(player)) Trajectory::get().dash_spin_stop(player);
        float spin = player->m_iconSprite->getRotation();
        for (auto* sprite : { player->m_iconSprite, player->m_iconGlow }) {
            sprite->setScale(1.f);
            sprite->stopAllActions();
            sprite->setRotation(0.f);
        }
        player->setRotation(spin);
    }
    gameEvent(player, (int)GJGameEvent::DashStop);
}

void ringJump(PlayerObject* player, RingObject* ring) {
    const double swingRingYVelocityScale = 0.6000000238418579;
    const double ballRingYVelocityScale = 0.699999988079071;
    if (player->m_isDead) return;
    if (!ring) return;

    if (player->m_ringRelatedSet.count(ring->m_uniqueID)) return;

    bool isCustomOrTeleportRing =
        (ring->m_objectType == GameObjectType::CustomRing) ||
        (ring->m_objectType == GameObjectType::TeleportOrb);

    if (Orbs::blockHoldRing(player, ring)) return;
    if (!player->m_stateRingJump2) return;
    if (player->m_isDashing) return;
    if (!player->m_stateJumpBuffered) return;

    if ((player->m_touchedRing || isCustomOrTeleportRing) &&
        (player->m_touchedCustomRing ||
         ring->m_objectType != GameObjectType::CustomRing)) {
        if (player->m_touchedGravityPortal) return;
        if (ring->m_objectType != GameObjectType::TeleportOrb) return;
    }

    if (ring->m_isReverse) {
        player->reversePlayer(ring);
    }

    player->m_ringJumpRelated = true;

    player->m_ringRelatedSet.insert(ring->m_uniqueID);
    if (Trajectory::get().isFake(player)) activateForTrajectory(ring, player);
    gameEvent(player, (int)GJGameEvent::OrbActivated);
    gameEvent(player, (int)player->m_gameLayer->objectTypeToGameEvent((int)ring->m_objectType));

    if (ring->m_objectType == GameObjectType::CustomRing) {
        player->m_touchedCustomRing = true;
    } else if (ring->m_objectType == GameObjectType::TeleportOrb) {
        player->m_touchedGravityPortal = true;
    } else {
        player->m_touchedRing = true;
    }

    player->m_touchingRings->removeObject(ring, true);
    if (!isCustomOrTeleportRing) {
        player->m_padRingRelated = true;
    }

    if (ring->m_objectType == GameObjectType::CustomRing) {
        if (PredictedWorld::get().firing()) {
            gd::vector<int> keys;
            if (ring->m_isSpawnOnly)
                player->m_gameLayer->spawnGroup(ring->m_targetGroupID, false, 0.0, keys, ring->m_uniqueID, ring->m_controlID);
            else
                player->m_gameLayer->toggleGroupTriggered(ring->m_targetGroupID, ring->m_activateGroup, keys, ring->m_uniqueID, ring->m_controlID);
        }
        if (player->m_enable22Changes) player->m_stateRingJump = false;
        finishRingJump(player);
        return;
    }

    if (ring->m_objectType == GameObjectType::TeleportOrb) {
        teleportPlayer(player->m_gameLayer, (TeleportPortalObject*)ring,
                       player);
        player->m_stateRingJump = false;

        finishRingJump(player);
        return;
    }

    if (ring->m_objectType == GameObjectType::SpiderOrb) {
        if (!player->m_isSideways) {
            if (ring->isFacingDown() != player->m_isUpsideDown) {
                flipGravity(player, !player->m_isUpsideDown);
            }
        } else {
            if (ring->isFacingLeft() != player->m_isUpsideDown) {
                flipGravity(player, !player->m_isUpsideDown);
            }
        }

        player->spiderTestJump(false);
        finishRingJump(player);
        return;
    }

    if (ring->m_objectType == GameObjectType::DashRing) {
        gameEvent(player, (int)GJGameEvent::DashOrb);
        startDashing(player, (DashRingObject*)ring);

        finishRingJump(player);
        return;
    }

    if (ring->m_objectType == GameObjectType::GravityDashRing) {
        flipGravity(player, !player->m_isUpsideDown, true);
        gameEvent(player, (int)GJGameEvent::GravityDashOrb);
        startDashing(player, (DashRingObject*)ring);

        finishRingJump(player);
        return;
    }

    player->m_stateRingJump = false;
    if (ring->m_objectType == GameObjectType::DropRing) {
        float gravity = player->m_isUpsideDown ? -1.0 : 1.0;
        float gravityMultiplied = gravity * -15.0;

        if (player->m_isShip || player->m_isBird || player->m_isDart ||
            player->m_isSwing) {
            gravityMultiplied = gravity * -14.0;
            if (player->m_isBird) {
                gravityMultiplied *= 0.8;
            }
        } else if (!player->m_isRobot && player->m_isSpider) {
            gravityMultiplied *= 1.1;
        }

        player->setYVelocity(gravityMultiplied, 1);

        if (!player->m_isBall && !player->m_isLocked && !player->m_isDashing) {
            player->m_isRotating = false;
            player->m_isBallRotating2 = false;
            player->m_isBallRotating = false;
            player->m_rotationSpeed = 0.0;
            player->runNormalRotation(0, 1.0);
        } else {
            player->runBallRotation2();
        }
        activateStreakState(player);
        player->m_hasEverHitRing = true;
        player->m_isAccelerating = true;
        if (player->m_isBall || player->m_isSwing) {
            player->m_jumpBuffered = false;
        }

        finishRingJump(player);
        return;
    }

    player->m_maybeIsBoosted = true;
    player->m_isOnGround2 = false;
    player->m_isOnGround = false;
    float gravity = 1.0;
    if (player->m_vehicleSize != 1.0) {
        gravity = 0.8;
    }

    float yStart = player->m_yStart;
    if (ring->m_objectType == GameObjectType::GravityRing) {
        yStart *= 0.8f;
    } else {
        if (ring->m_objectType == GameObjectType::GreenRing) {
            if (player->m_isShip) {
                yStart *= 0.7f;
            }
        } else {
            if (ring->m_objectType == GameObjectType::PinkJumpRing) {
                if (player->m_isShip) {
                    yStart *= 0.37f;
                } else if (player->m_isBird) {
                    yStart *= 0.42f;
                } else if (player->m_isBall) {
                    yStart *= 0.77f;
                } else {
                    yStart *= 0.72f;
                }
            } else {
                if (ring->m_objectType == GameObjectType::RedJumpRing) {
                    player->m_isAccelerating = true;
                    if (!player->m_isShip) {
                        if (player->m_isBird) {
                            if (player->m_vehicleSize == 1.0) {
                                yStart *= 1.02f;
                            } else {
                                yStart *= 1.36f;
                            }
                        } else if (player->m_isBall) {
                            yStart *= 1.34f;
                        } else if (player->m_isRobot) {
                            yStart *= 1.28f;
                        } else if (player->m_isSpider) {
                            yStart *= 1.34f;
                        } else {
                            yStart *= 1.38f;
                        }
                    } else if (player->m_vehicleSize != 1.0) {
                        yStart *= 1.4f;
                    }
                } else if (player->m_isRobot) {
                    yStart *= 0.9f;
                }
            }
        }
    }

    bool gravityOrb = ring->m_objectType == GameObjectType::GravityRing || ring->m_objectID == 84;
    if (ring->m_objectType == GameObjectType::GreenRing) {
        flipGravity(player, !player->m_isUpsideDown, true);
    }

    float gravityCoeff = player->m_isUpsideDown ? -1 : 1;
    double velocity = gravityCoeff * yStart * gravity;
    player->setYVelocity(velocity, gravityCoeff);

    if (!player->m_isBall && !player->m_isLocked && !player->m_isDashing) {
        player->m_isRotating = false;
        player->m_isBallRotating2 = false;
        player->m_isBallRotating = false;
        player->m_rotationSpeed = 0.0;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID)
        player->runNormalRotation(0, 1.0);
#endif
    } else {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID)
        player->runBallRotation2();
#endif
    }

    cocos2d::CCPoint playerPos = player->getPosition();
    player->m_lastGroundedPos = playerPos;

    activateStreakState(player);
    player->m_hasEverHitRing = true;
    if (player->m_isSwing) {
        player->m_yVelocity *= swingRingYVelocityScale;
        player->m_jumpBuffered = false;
    } else if (player->m_isBall || player->m_isSpider) {
        player->m_yVelocity *= ballRingYVelocityScale;
        player->m_jumpBuffered = false;
    }

    if (gravityOrb) {
        flipGravity(player, !player->m_isUpsideDown, true);
    }

    if (ring->m_objectType == GameObjectType::RedJumpRing) {
        player->m_isAccelerating = true;
    }
    finishRingJump(player);
}

void gameEvent(PlayerObject* player, int event) {
    if (!player->m_gameLayer) return;
    if (Trajectory::get().isFake(player) && !PredictedWorld::get().firing()) return;
    player->m_gameLayer->gameEventTriggered((GJGameEvent)event, 0, player->m_uniqueID);
}

void togglePlayerScale(PlayerObject* player, bool smallSize) {
    if (player->m_vehicleSize == 1.0 && !smallSize) return;
    if (smallSize) {
        if (player->m_vehicleSize != 1.0) return;

        player->m_vehicleSize = 0.6;
    } else {
        if (player->m_isPlatformer) {
            player->m_stateScale = 2;
        }

        player->m_vehicleSize = 1;
    }

    player->m_spriteWidthScale = player->m_vehicleSize;
    player->m_spriteHeightScale = player->m_vehicleSize;
    player->setScaleX(player->m_vehicleSize);
    player->setScaleY(player->m_vehicleSize);
}

void toggleFlyMode(PlayerObject* player, bool isFly) {
    if (player->m_isShip == isFly) return;

    player->m_gameModeChangedTime = player->m_totalTime;
    player->m_isShip = isFly;

    if (isFly) player->switchedToMode(GameObjectType::ShipPortal);

    player->m_isRotating = false;
    player->m_isBallRotating = false;
    player->m_rotationSpeed = 0.0;
    player->m_yVelocity = player->m_yVelocity * 0.5;
    player->setRotation(0.0);
    player->m_isOnGround2 = false;
    player->m_isOnGround = false;
    player->m_shouldTryPlacingCheckpoint = false;
}
}
