// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#include "Physics/player.hpp"
#include "Hack/orbs.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace phys {
void startDashing(PlayerObject* player, DashRingObject* ring) {
    if (player->m_isDashing) return;

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
        sidewaysAngle = ring->m_dashSpeed * 5.77;
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
    cocos2d::CCPoint p = cocos2d::ccpForAngle(dashAngle * 0.01745329);
#else
    cocos2d::CCPoint p = CCPoint{
        (float)std::cos(((float)dashAngle * 0.01745329)),
        (float)std::sin(((float)dashAngle * 0.01745329)),
    };
#endif
    cocos2d::CCPoint mult = p * sidewaysAngle;

    float x = mult.x;
    if (player->m_isSideways) {
        mult.x = mult.y;
        mult.y = x;
    }

    double doubleX = mult.x;
    player->m_dashX = doubleX;
    player->m_dashY = mult.y;
    if (!player->m_isPlatformer) {
        doubleX = mult.y / fabs(mult.x);
        player->m_dashY = doubleX;
        player->m_dashX = fabs(doubleX);
    } else if (doubleX >= 0.0) {
        if (doubleX > 0.0) {
            player->doReversePlayer(false);
        }
    } else {
        player->doReversePlayer(true);
    }

    player->m_dashAngle = dashAngle;
    player->m_lastGroundedPos = cocos2d::CCPoint{0.0, 0.0};
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
}

void ringJump(PlayerObject* player, RingObject* ring) {
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
        return;
    }

    if (ring->m_objectType == GameObjectType::TeleportOrb) {
        teleportPlayer(player->m_gameLayer, (TeleportPortalObject*)ring,
                       player);

        return;
    }

    if (ring->m_objectType == GameObjectType::SpiderOrb) {
        if (!player->m_isSideways) {
            if (ring->isFacingDown() != player->m_isUpsideDown) {
                flipGravity(player, !player->m_isUpsideDown, true);
            }
        } else {
            if (ring->isFacingLeft() != player->m_isUpsideDown) {
                flipGravity(player, !player->m_isUpsideDown, true);
            }
        }

        player->spiderTestJumpInternal(false);
    }

    if (ring->m_objectType == GameObjectType::DashRing) {
        startDashing(player, (DashRingObject*)ring);

        return;
    }

    if (ring->m_objectType == GameObjectType::GravityDashRing) {
        flipGravity(player, !player->m_isUpsideDown, true);
        startDashing(player, (DashRingObject*)ring);

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
        player->m_hasEverHitRing = true;
        player->m_isAccelerating = true;
        if (player->m_isBall || player->m_isSwing) {
            player->m_jumpBuffered = false;
        }

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
        yStart *= 0.8;
    } else {
        if (ring->m_objectType == GameObjectType::GreenRing) {
            if (player->m_isShip) {
                yStart *= 0.7;
            }
        } else {
            if (ring->m_objectType == GameObjectType::PinkJumpRing) {
                if (player->m_isShip) {
                    yStart *= 0.37;
                } else if (player->m_isBird) {
                    yStart *= 0.42;
                } else if (player->m_isBall) {
                    yStart *= 0.77;
                } else {
                    yStart *= 0.72;
                }
            } else {
                if (ring->m_objectType == GameObjectType::RedJumpRing) {
                    player->m_isAccelerating = true;
                    if (!player->m_isShip) {
                        if (player->m_isBird) {
                            if (player->m_vehicleSize == 1.0) {
                                yStart *= 1.02;
                            } else {
                                yStart *= 1.36;
                            }
                        } else if (player->m_isBall) {
                            yStart *= 1.34;
                        } else if (player->m_isRobot) {
                            yStart *= 1.28;
                        } else if (player->m_isSpider) {
                            yStart *= 1.34;
                        } else {
                            yStart *= 1.38;
                        }
                    } else if (player->m_vehicleSize != 1.0) {
                        yStart *= 1.4;
                    }
                } else if (player->m_isRobot) {
                    yStart *= 0.9;
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

    player->m_hasEverHitRing = true;
    if (player->m_isSwing) {
        player->m_yVelocity *= 0.6;
    } else if (player->m_isBall || player->m_isSpider) {
        player->m_yVelocity *= 0.7;
    }

    if (gravityOrb) {
        flipGravity(player, !player->m_isUpsideDown, true);
    }

    if (ring->m_objectType == GameObjectType::RedJumpRing) {
        player->m_isAccelerating = true;
    }
}

template <typename T>
T clamp(T value, T min, T max) {
    if (value <= max) {
        if (value < min) {
            return min;
        }
    } else {
        return max;
    }

    return value;
}

bool playerIsFalling(PlayerObject* player) {
    double gravity = 0.0;
    double velocity = 0.0;

    if (player->m_isSideways || player->m_isPlatformer || player->m_isSwing ||
        player->m_fixGravityBug) {
        gravity = player->m_gravity;
        velocity = player->m_yVelocity;
        double doubledGravity = gravity * 2;
        if (player->m_isUpsideDown) {
            return fabs(doubledGravity) < velocity;
        } else {
            return doubledGravity > velocity;
        }
    }

    if (player->m_unkA99) {
        velocity = fabs(gravity);
    }

    double playerVelocity = player->m_yVelocity;
    if (player->m_isUpsideDown) {
        return (double)gravity * 2 < playerVelocity;
    } else {
        return playerVelocity < (double)velocity * 2;
    }
}

void updateJump(PlayerObject* player, float dt) {
    bool holdingLeft = player->m_holdingLeft;
    bool holdingRight = player->m_holdingRight;

    if (holdingLeft && holdingRight) {
        if (player->m_leftPressedFirst) {
            holdingRight = false;
        } else {
            holdingLeft = false;
        }
    }

    float slopeAngleAbs = fabs(player->m_slopeAngleRadians * 57.29578);
    if (player->m_isPlatformer && player->m_isOnSlope &&
        slopeAngleAbs >= 80.0 && player->m_currentSlope->m_unk4F8) {
        player->m_isOnGround = false;
    }

    int jumpACRelated = -(player->m_isOnGround ? 1 : 0) & 213;
    int randNumber = rand();
    int jumpACSeed = (int)(((float)randNumber / 32767) * 1000.0);

    player->m_jumpRelatedAC2 = geode::SeedValueRSV(jumpACRelated, jumpACSeed);

    float gravity = player->m_gravity;
    bool isJumpBuffered = player->m_jumpBuffered;
    if (player->m_isRobot) {
        if (!player->m_stateRingJump || !isJumpBuffered) {
            isJumpBuffered = false;
        } else {
            isJumpBuffered = true;
        }
    }

    if (player->m_isBall || player->m_isShip || player->m_isBird ||
        player->m_isDart || player->m_isSwing || player->m_isSpider) {
        gravity = 0.958199;
    }

    float origGravity = gravity;

    float gravityMod = player->m_gravityMod;
    gravity *= gravityMod;
    double what = 0.8;

    float vehicleSizeRelated = player->m_vehicleSize != 1.0 ? 0.8 : 1.0;
    if (player->m_isShip || player->m_isBird || player->m_isDart ||
        player->m_isSwing) {
        if (player->m_vehicleSize != 1.0) {
            vehicleSizeRelated = 0.85;
        }

        double velocity = player->m_yVelocity;
        double v64 = 8.0 / vehicleSizeRelated;
        double v65 = -6.4 / vehicleSizeRelated;

        if (player->m_isUpsideDown) {
            if (!((velocity > 0.0 || velocity <= -v64) &&
                  (velocity < 0.0 || -v65 <= velocity))) {
                player->m_isAccelerating = false;
            }
        } else {
            if (!((velocity < 0.0 || v64 <= velocity) &&
                  (velocity > 0.0 || velocity <= v65))) {
                player->m_isAccelerating = false;
            }
        }

        if (player->m_isDart) {
            int gravityMult = player->m_isUpsideDown ? -1 : 1;
            int jumpBufferedMult = player->m_jumpBuffered ? 1 : -1;
            player->setYVelocity(player->m_playerSpeed *
                                     player->m_speedMultiplier * gravityMult *
                                     8.0 * jumpBufferedMult,
                                 0);
        } else if (player->m_isSwing) {
            if (player->m_stateRingJump && player->m_jumpBuffered) {
                player->m_stateRingJump = false;
                velocity = player->m_yVelocity;
                phys::flipGravity(player, !player->m_isUpsideDown, false);
                player->setYVelocity(velocity * 0.8, 0);
            }

            what = 1.0;
            gravity = 0.4;
            if (player->m_vehicleSize != 1.0) {
                gravity = 0.6;
            }

            int gravityMult = player->m_isUpsideDown ? -1 : 1;
            velocity = player->m_yVelocity -
                       (gravityMult * dt * gravity * origGravity);
            player->setYVelocity(velocity, 0);
        } else if (player->m_isBird) {
            if (player->m_stateRingJump && player->m_jumpBuffered) {
                player->m_stateRingJump = false;
                float sizeMod = player->m_vehicleSize != 1.0 ? 8.0 : 7.0;

                float newVel = 0.0;
                bool gt = false;

                if (player->m_isUpsideDown) {
                    newVel = -1 * sizeMod * vehicleSizeRelated;
                    gt = player->m_yVelocity <= newVel;
                } else {
                    newVel = sizeMod * vehicleSizeRelated;
                    gt = player->m_yVelocity >= newVel;
                }

                if (!gt) {
                    player->setYVelocity(newVel, 0);
                    if (player->m_isOnSlope || player->m_wasOnSlope) {
                        if (player->m_slopeVelocity > 0) {
                            float prevVelocity = player->m_yVelocity;
                            player->setYVelocity(player->m_slopeVelocity * 0.5 +
                                                     player->m_yVelocity,
                                                 0);
                            player->setYVelocity(
                                fmin(player->m_yVelocity, prevVelocity * 1.4),
                                0);
                        }
                    }
                }
            }

            float fallingMod = 0.8;
            if (!phys::playerIsFalling(player)) {
                fallingMod = 1.2;
            }

            int gravityMult = player->m_isUpsideDown ? -1 : 1;
            velocity = -(fallingMod * dt * origGravity * gravityMult * 0.5) /
                           vehicleSizeRelated +
                       player->m_yVelocity;
            player->setYVelocity(velocity, 0);
            if (player->m_jumpBuffered) {
                player->m_isOnGround2 = false;
            }
        } else if (player->m_isShip) {
            float something = 0.8;
            if (player->m_jumpBuffered) {
                if (!player->m_isAccelerating) {
                    something = -1.0;
                    if (player->m_isPlatformer) {
                        gravity *= 0.8;
                    }

                    if (something >= 0.0) {
                        origGravity = gravity;
                    }

                    float fallingMod =
                        phys::playerIsFalling(player) ? 0.5 : 0.4;
                    float upsideDownMod = player->m_isUpsideDown ? -1 : 1;
                    player->setYVelocity(
                        -(upsideDownMod * dt * fallingMod * origGravity) /
                                vehicleSizeRelated +
                            player->m_yVelocity,
                        0);

                    if (phys::playerIsFalling(player)) {
                        player->m_maybeIsBoosted = false;
                    }

                    goto end;
                }
            } else if (!player->m_isAccelerating) {
                if (player->m_isPlatformer) {
                    gravity *= 0.8;
                }

                if (something >= 0.0) {
                    origGravity = gravity;
                }

                float fallingMod =
                    phys::playerIsFalling(player) && player->m_jumpBuffered
                        ? 0.5
                        : 0.4;
                float upsideDownMod = player->m_isUpsideDown ? -1 : 1;
                player->setYVelocity(
                    -(upsideDownMod * dt * fallingMod * origGravity) /
                            vehicleSizeRelated +
                        player->m_yVelocity,
                    0);

                if (phys::playerIsFalling(player)) {
                    player->m_maybeIsBoosted = false;
                }

                goto end;
            }

            if (!player->m_isUpsideDown) {
                if (player->m_yVelocity >= 0.0) {
                    if (player->m_isPlatformer) {
                        gravity *= 0.8;
                    }

                    if (something >= 0.0) {
                        origGravity = gravity;
                    }

                    float fallingMod =
                        phys::playerIsFalling(player) && player->m_jumpBuffered
                            ? 0.5
                            : 0.4;
                    float upsideDownMod = player->m_isUpsideDown ? -1 : 1;
                    player->setYVelocity(
                        -(upsideDownMod * dt * fallingMod * origGravity) /
                                vehicleSizeRelated +
                            player->m_yVelocity,
                        0);

                    if (phys::playerIsFalling(player)) {
                        player->m_maybeIsBoosted = false;
                    }

                    goto end;
                }

                something = -1.0;
            }

            if (player->m_yVelocity > 0.0) {
                something = -1.0;
            }

            if (player->m_jumpBuffered && !phys::playerIsFalling(player)) {
                something = 1.2;
            }

            if (player->m_isPlatformer) {
                gravity *= 0.8;
            }

            if (something >= 0.0) {
                origGravity = gravity;
            }

            float fallingMod =
                phys::playerIsFalling(player) && player->m_jumpBuffered ? 0.5
                                                                        : 0.4;
            float upsideDownMod = player->m_isUpsideDown ? -1 : 1;
            player->setYVelocity(
                -(upsideDownMod * dt * fallingMod * origGravity) /
                        vehicleSizeRelated +
                    player->m_yVelocity,
                0);

            if (phys::playerIsFalling(player)) {
                player->m_maybeIsBoosted = false;
            }

            goto end;
        }

    end:
        if (!player->m_isAccelerating && !player->m_isDart) {
            velocity = player->m_yVelocity;
            if (!player->m_isUpsideDown) {
                player->setYVelocity(
                    fmax(player->m_yVelocity, what * -8.0 / vehicleSizeRelated),
                    0);
                velocity = (what * 8.0) / vehicleSizeRelated;
            } else {
                player->setYVelocity(
                    fmax(player->m_yVelocity, -8.0 / vehicleSizeRelated), 0);
                velocity = (8.0) / vehicleSizeRelated;
            }

            player->setYVelocity(fmin(player->m_yVelocity, velocity), 0);
        }

        if (phys::playerIsFalling(player)) {
            player->m_maybeIsBoosted = false;
        }

        player->m_wasJumpBuffered = player->m_jumpBuffered;
        player->m_wasRobotJump = player->m_touchedPad;

        return;
    }
}

void playerUpdate(PlayerObject* player, float) {
    player->m_yVelocity = clamp(player->m_yVelocity, -1000.0, 1000.0);

    if (player->m_isPlatformer) {
        player->m_platformerXVelocity =
            clamp(player->m_platformerXVelocity, -1000.0, 1000.0);
    }
    if (player->m_isDead) return;

    player->m_lastPosition = player->getPosition();
    player->m_yVelocityRelated3 = 0.0;
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
