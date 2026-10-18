// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#include "Physics/gjbasegamelayer.hpp"
#include "Logic/logic.hpp"
#include "Physics/player.hpp"

#include <Geode/Geode.hpp>

#define PI_FLOAT (3.1415927410125732f)
#define DEGREES_TO_RADIANS (0.01745329238474369f)

namespace phys {
cocos2d::CCArray* getGroup(GJBaseGameLayer* pl, int groupID) {
    if (groupID < 0) groupID = 0;
    if (groupID > 9999) groupID = 9999;

    cocos2d::CCArray* group = pl->m_groups.at(groupID);
    if (!group) {
        group = cocos2d::CCArray::create();
        pl->m_groupDict->setObject(group, groupID);
        pl->m_groups.at(groupID) = group;
    }

    return group;
}

float redirectPlayerForce(PlayerObject* player, float force,
                          float mod, float min,
                          float max) {
    float angle = force * DEGREES_TO_RADIANS;
    cocos2d::CCPoint velocity{ (float)player->m_platformerXVelocity, (float)player->m_yVelocity };
    float turn = GJBaseGameLayer::convertToClosestDirection(angle - atan2f(velocity.y, velocity.x), PI_FLOAT);
    if (turn != 0.f) {
        float cosine = cosf(turn), sine = sinf(turn);
        velocity = cocos2d::CCPoint{ velocity.x * cosine - velocity.y * sine, velocity.y * cosine + velocity.x * sine };
    }
    velocity = velocity * mod;
    float length = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
    if (max > 0.f && length > max) {
        velocity = velocity * (max / length);
    } else if (min > 0.f && min > length) {
        if (length == 0.f) velocity = cocos2d::CCPoint{ cosf(angle) * min, sinf(angle) * min };
        else velocity = velocity * (min / length);
    }
    if (player->m_isSideways) {
        float x = velocity.x;
        velocity.x = velocity.y;
        velocity.y = x;
    }
    player->m_isAccelerating = true;
    player->m_yVelocity = velocity.y;
    if (player->m_isPlatformer) {
        player->m_affectedByForces = true;
        player->m_platformerXVelocity = velocity.x;
    }
    return velocity.y;
}

void teleportPlayer(GJBaseGameLayer* pl, TeleportPortalObject* object,
                    PlayerObject* player) {
    if (!player) {
        player = reinterpret_cast<PlayerObject*>(
            reinterpret_cast<uintptr_t>(pl->m_varianceValues.data()) + 0xe5);
    }
    player->m_wasTeleported = true;

    if (object->m_orangePortal) {
        CCPoint bluePortalStartPos = object->getStartPos();
        CCPoint orangePortalStartPos = object->m_orangePortal->getStartPos();
        object->m_teleportYOffset =
            orangePortalStartPos.y - bluePortalStartPos.y;
    }

    cocos2d::CCPoint playerPos = player->getPosition();
    TeleportPortalObject* destination = object->m_orangePortal;
    if (!destination) {
        int targetGroupID = object->m_targetGroupID;
        if (targetGroupID > 0) {
            cocos2d::CCArray* group = getGroup(pl, targetGroupID);
            int groupLength = group->count();

            float someFloat;

            if (groupLength > 0) {
                if (groupLength == 1) {
                    someFloat = 0.0;
                } else {
                    auto& roll = Logic::get().teleport_random;
                    roll = (unsigned long long)((int)((214013ull * roll + 2531011ull) >> 16) & 32767);
                    someFloat = (float)roll / 32767.f;
                    if (someFloat == 1.f) someFloat = 0.f;
                }

                destination = (TeleportPortalObject*)group->objectAtIndex(
                    (int)(someFloat * groupLength));
            }
        }
    }

    if (destination) {
        CCPoint destinationPos;
        CCPoint portalPos = object->getPosition();
        player->m_lastPortalPos = destination->getPosition();
        player->m_lastActivatedPortal = destination;
        player->m_fallStartY = 0.0;
        if (object->m_objectID == 747) {
            float dy = object->m_teleportYOffset;
            portalPos = object->getRealPosition();

            destinationPos =
                cocos2d::CCPoint{player->getPosition().x, portalPos.y + dy};
        } else {
            destinationPos = destination->getPosition();
        }
        if (object->m_saveOffset) {
            portalPos = object->getRealPosition();

            portalPos -= playerPos;
            destinationPos -= portalPos;
        }
        if (object->m_ignoreX) {
            destinationPos.x = playerPos.x;
        }
        if (object->m_ignoreY) {
            destinationPos.y = playerPos.y;
        }
        player->setPosition(destinationPos);
    }

    if (object->m_gravityMode > 0) {
        bool gravity = false;

        if (object->m_gravityMode == 2) {
            gravity = true;
        } else if (object->m_gravityMode == 3) {
            gravity = !player->m_isUpsideDown;
        }

        phys::flipGravity(player, gravity, true);
    }

    if (destination) {
        player->m_isOnGround2 = false;
        player->m_lastGroundedPos = CCPoint{ 0.f, 0.f };
        placeStreakPointState(player);
    }

    const int halfTurn = 180;
    float forceRelated;
    if (destination) {
        float gravityForceMod = 90.0;
        int objectID = object->m_objectID;
        if (objectID == 38 || objectID == 747 || objectID == 749 ||
            objectID == 2064 || objectID == 2902) {
            gravityForceMod = 180.0;
        }

        float flipMod = (float)(destination->isFlipX() ? halfTurn : 0);
        forceRelated =
            flipMod + (gravityForceMod - destination->getRotationX());
    } else {
        float flipMod = (float)(object->isFlipX() ? halfTurn : 0);
        forceRelated = flipMod - object->getRotationX();
    }

    if (object->m_redirectForceEnabled) {
        phys::redirectPlayerForce(
            player, forceRelated, object->m_redirectForceMod,
            object->m_redirectForceMin, object->m_redirectForceMax);
    } else {
        if (object->m_staticForceEnabled) {
            float force = object->m_staticForce;
            if (force == 0.0 && !object->m_staticForceAdditive) {
                player->m_isAccelerating = false;
                player->m_yVelocity = 0.0;
                if (player->m_isPlatformer) {
                    player->m_platformerXVelocity = 0.0;
                    player->m_affectedByForces = false;
                }
            } else {
                float forceMod = forceRelated * DEGREES_TO_RADIANS;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID)
                cocos2d::CCPoint p = cocos2d::ccpForAngle(forceMod);
#else
                cocos2d::CCPoint p = CCPoint{
                    (float)std::cos(((float)forceMod)),
                    (float)std::sin(((float)forceMod)),
                };
#endif

                float length = p.getLength();
                if (length > 0.0) {
                    cocos2d::CCPoint forceVector = p * (force / length);
                    if (player->m_isSideways) {
                        float x = forceVector.x;
                        forceVector.x = forceVector.y;
                        forceVector.y = x;
                    }

                    player->m_isAccelerating = true;
                    double newYVelocity = forceVector.y;
                    if (object->m_staticForceAdditive) {
                        newYVelocity += player->m_yVelocity;
                    }
                    player->m_yVelocity = newYVelocity;
                    if (player->m_isPlatformer) {
                        double newXVelocity = forceVector.x;
                        if (object->m_staticForceAdditive) {
                            newXVelocity += player->m_platformerXVelocity;
                        }
                        player->m_platformerXVelocity = newXVelocity;
                        player->m_affectedByForces = true;
                    }
                }
            }
        }
    }

    if (object->m_snapGround) {
        player->m_lastGroundedPos = player->getPosition();
    }

    if (player->m_isDashing && object->m_redirectDash) {
        const float radiansToDegrees = 57.295780181884766f;
        const int quarterTurn = 90;
        const int fullTurn = 360;
        cocos2d::CCPoint dash{ (float)player->m_dashX, (float)player->m_dashY };
        float turn = GJBaseGameLayer::convertToClosestDirection(forceRelated * DEGREES_TO_RADIANS - atan2f(dash.y, dash.x), PI_FLOAT);
        if (turn != 0.f) {
            float cosine = cosf(turn), sine = sinf(turn);
            dash = cocos2d::CCPoint{ dash.x * cosine - dash.y * sine, dash.y * cosine + dash.x * sine };
        }
        int degrees = (int)(-(atan2f(dash.y, dash.x) * radiansToDegrees));
        int turned = player->m_isSideways ? degrees - quarterTurn : degrees;
        int wrapped = turned % fullTurn;
        if (wrapped < 0) wrapped += fullTurn;
        player->m_dashAngle = (double)(-wrapped);
        if (player->m_isSideways) {
            float x = dash.x;
            dash.x = dash.y;
            dash.y = x;
        }
        player->m_dashX = dash.x;
        player->m_dashY = dash.y;
        if (player->m_dashX < 0.0) player->doReversePlayer(!player->m_isSideways);
        else if (player->m_dashX > 0.0) player->doReversePlayer(player->m_isSideways);
        if (!Logic::get().predicting) player->updateDashArt();
    }
}
}
