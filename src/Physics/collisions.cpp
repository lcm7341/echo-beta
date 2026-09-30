// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#include "Physics/collisions.hpp"

#include <Geode/Enums.hpp>
#include <Geode/Geode.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>

#include "Physics/gravity.hpp"
#include "Physics/player.hpp"
#include "Hack/orbs.hpp"
#include "Hack/trajectory.hpp"
#include "Logic/logic.hpp"

namespace phys {
int checkPlayerCollisions(GJBaseGameLayer* gameLayer, PlayerObject* player) {
    player->m_wasTeleported = false;
    player->m_ringJumpRelated = false;
    player->m_collidedTopMinY = 0.0;
    player->m_collidedBottomMaxY = 0.0;
    player->m_collidedLeftMaxX = 0.0;
    player->m_collidedRightMinX = 0.0;

    player->m_wasOnSlope = player->m_isOnSlope;
    player->m_isOnSlope = false;

    bool isOnGround = player->m_isOnGround2;
    player->m_isOnGround4 = isOnGround;
    if (isOnGround && !player->m_platformerMovingLeft &&
        !player->m_platformerMovingRight && player->m_maybeSlidingTime > 0) {
        player->m_maybeSlidingTime = 0;
        player->m_maybeSlidingStartTime = player->m_totalTime;
    }

    if (player->m_unk669) {
        player->m_currentPotentialSlope = nullptr;
    }
    [[maybe_unused]] GameObject* addedSlope;
    player->m_unk669 = true;
    player->m_potentialSlopeMap.clear();
    GameObject* currentSlope = player->m_currentPotentialSlope;
    if (currentSlope) {
        player->m_potentialSlopeMap.insert(
            {currentSlope->m_uniqueID, currentSlope});
        addedSlope = currentSlope;
    }

    currentSlope = player->m_currentSlope;
    if (currentSlope) {
        player->m_potentialSlopeMap.insert(
            {currentSlope->m_uniqueID, currentSlope});
        addedSlope = currentSlope;
    }

    float someValue = 0.0;
    float vehicleSize = player->m_vehicleSize;
    float unkAngle = player->m_unkAngle1;
    if (vehicleSize != 1.0) {
        someValue = (1.0 - vehicleSize) * unkAngle * 0.5;
    }
    float unkAngleHalved = unkAngle * 0.5;
    float angleTransformed = (unkAngleHalved + 90.0) - someValue;

    bool groundExists =
        !gameLayer->m_gameState.m_isFreeMode &&
        (player->m_isShip || player->m_isBird || player->m_isDart ||
         player->m_isSwing || player->m_isBall || player->m_isSpider ||
         gameLayer->m_gameState.m_isDualMode);

    player->m_isOutOfBounds = false;

    [[maybe_unused]] bool exceededBounds = false;

    if (gameLayer->m_isPlatformer) {
        auto playerPosition = player->getPosition();
        if (playerPosition.x < -30.0) {
            player->m_platformerXVelocity = 0.0;
            player->setPosition({-30.0, playerPosition.y});
        }
    } else if (player->m_isGoingLeft) {
        exceededBounds = player->getPosition().x < -30.0;
    }

    if (angleTransformed <= player->getPosition().y ||
        (groundExists &&
         (player->m_isShip || player->m_isBird || player->m_isDart ||
          player->m_isSwing || player->m_isBall || player->m_isSpider ||
          gameLayer->m_gameState.m_isDualMode))) {
        if (player->getPosition().y >
            (float)(someValue + gameLayer->m_maxGameplayY)) {
            exceededBounds = true;
        }
    } else if (player->m_isUpsideDown) {
        if (player->m_lastFlipTime != 0 &&
            player->m_totalTime - player->m_lastFlipTime < 0.1) {
            auto playerPosition = player->getPosition();
            player->setPosition({-1.0842022e-19, playerPosition.x});
            player->hitGround(nullptr, true);
            player->updateCollide(PlayerCollisionDirection::Top, nullptr);
            player->m_isOnGround2 = false;
            return 0;
        }
        exceededBounds = true;
    } else if (!player->m_maybeIsBoosted) {
    }

    if (!groundExists || player->m_wasTeleported) {
    }

    return 0;
}

int collidedWithObjectInternal(PlayerObject* player, float, GameObject* object,
                               CCRect* p4, bool) {
    [[maybe_unused]] bool holdingLeft = player->m_holdingLeft;
    [[maybe_unused]] bool holdingRight = player->m_holdingLeft;
    if (player->m_leftPressedFirst) {
        holdingRight = false;
    } else {
        holdingLeft = false;
    }

    GameObject* someObj = nullptr;
    cocos2d::CCRect playerRect;
    if (p4->equals(cocos2d::CCRect{0.0, 0.0, 0.0, 0.0})) {
        someObj = object;
        playerRect = player->getObjectRect();
        if (object) {
            *p4 = object->getObjectRect();
            playerRect = object->getObjectRect();
        }
    } else {
        someObj = nullptr;
        playerRect = player->getObjectRect();
    }

    float unk1 = 5.0;
    if (!player->m_isPlatformer || player->m_wasOnSlope ||
        player->m_isOnSlope) {
        unk1 = 10.0;
    }
    unk1 = player->m_stateScale > 0 ? 15.0 : unk1;

    double unk2 = player->m_isUpsideDown ? -unk1 : unk1;
    if ((player->m_isShip || player->m_isBird || player->m_isDart ||
         player->m_isSwing) &&
        !player->m_isPlatformer) {
        unk2 = player->m_isUpsideDown ? -6.0 : 6.0;
    }
    if (player->m_wasOnSlope) {
        unk2 = unk2 + (player->m_isUpsideDown ? -1.0 : 1.0) * player->unk_584;
    }

    if (someObj) {
    }

    return 0;
}

void preSlopeCollision(PlayerObject* player, float dt, GameObject* slope) {
    if (slope->m_uniqueID == player->m_collidingWithSlopeId) return;

    cocos2d::CCRect playerRect = player->getObjectRect();
    cocos2d::CCRect slopeRect = slope->getObjectRect();

    int slopeDir = slope->m_slopeDirection;

    [[maybe_unused]] bool someBool = false;

    bool unk1 = false;
    if ((slopeDir < 5) || slopeDir == 6) {
        unk1 = true;
    }

    [[maybe_unused]] bool unk2 = false;
    if ((slopeDir < 7) && ((0x6aU >> (slopeDir & 0x1f) & 1) != 0)) {
        unk2 = true;
    }

    float unk3 = 5.0;
    if (!player->m_isPlatformer ||
        (!player->m_wasOnSlope && !player->m_isOnSlope)) {
        unk3 = 0.0;
    }

    cocos2d::CCPoint pos = slope->getRealPosition();
    cocos2d::CCPoint diff = pos - slope->m_lastPosition;

    if (player->m_isSideways) {
        diff.x = diff.y;
    }

    float playerSpeed = player->m_playerSpeed * player->m_speedMultiplier * dt;
    float slopePosX = slope->getPosition().x;
    int unk4 = 0;
    if (unk1) {
        if ((!player->m_isGoingLeft || player->m_isPlatformer ||
             diff.x < playerSpeed) &&
            slopePosX < slopeRect.origin.x) {
            unk4 = (!player->m_isGoingLeft || player->m_isPlatformer) ? 0 : 1;
            unk3 += unk4;
            float heightDiff = slopeRect.size.height - (unk3 * 2);
            playerSpeed = slopeRect.origin.x;
            cocos2d::CCRect newPlayerRect = {
                playerSpeed, unk3 + slopeRect.origin.y, 1.0, heightDiff};
            someBool = true;
            if (newPlayerRect.intersectsRect(playerRect)) {
                player->collidedWithObjectInternal(dt, slope, newPlayerRect,
                                                   false);
            }
        }
    } else if (player->m_isGoingLeft || player->m_isPlatformer ||
               (playerSpeed < diff.x)) {
        playerSpeed = slopeRect.getMaxX();
        if (playerSpeed < slopePosX) {
            unk4 = (!player->m_isGoingLeft && !player->m_isPlatformer) ? 1 : 0;
            playerSpeed = (slopeRect.size.width + slopeRect.origin.x) - 1.0;
            unk3 += unk4;
            float heightDiff = slopeRect.size.height - (unk3 * 2);
            cocos2d::CCRect newPlayerRect = {
                playerSpeed, unk3 + slopeRect.origin.y, 1.0, heightDiff};
            someBool = true;
            if (newPlayerRect.intersectsRect(playerRect)) {
                player->collidedWithObjectInternal(dt, slope, newPlayerRect,
                                                   false);
            }
        }
    }
}

void collidedWithSlopeInternal(PlayerObject*) {}

void activateForTrajectory(EffectGameObject* obj, PlayerObject* player) {
    Trajectory::get().rememberActivation(obj, player);
}

void bumpPlayerFromGJBGL(GJBaseGameLayer* pl, PlayerObject* player,
                         EffectGameObject* object) {
    if (pl->canBeActivatedByPlayer(player, object)) {
        cocos2d::CCPoint objPos = object->getPosition();

        player->m_lastPortalPos = objPos;
        activateForTrajectory(object, player);
        float force = 1.0;

        if (object->m_objectType == GameObjectType::PinkJumpPad) {
            if (player->m_isShip) {
                force = 0.35;
            } else if (player->m_isBird) {
                force = 0.4;
            } else if (player->m_isBall) {
                force = 0.7;
            } else if (player->m_isSpider) {
                force = 0.7;
            } else {
                force = 0.65;
            }
        } else if (object->m_objectType == GameObjectType::RedJumpPad) {
            if (player->m_isShip) {
                if (player->m_vehicleSize >= 1.0) {
                    force = 0.63;
                } else {
                    force = 0.95;
                }
            } else if (player->m_isBird) {
                if (player->m_vehicleSize >= 1.0) {
                    force = 0.6;
                } else {
                    force = 0.98;
                }
            } else {
                force = 1.25;
            }
        }
        player->m_lastActivatedPortal = object;
        if (auto* play = PlayLayer::get()) Orbs::notePredictionPad(play, player, object);
        phys::bumpPlayer(player, force, static_cast<int>(object->m_objectType),
                         false, object);
    }
}

void collisionCheckObjects(GJBaseGameLayer* pl, PlayerObject* player,
                           gd::vector<GameObject*>* objects, int objectCount,
                           float dt) {
    if (!objects || objectCount <= 0) return;

    CCRect playerRect = player->getObjectRect();

    [[maybe_unused]] float playerMinX = playerRect.getMinX();
    [[maybe_unused]] float playerMaxX = playerRect.getMaxX();
    [[maybe_unused]] float playerMinY = playerRect.getMinY();
    [[maybe_unused]] float playerMaxY = playerRect.getMaxY();

    for (int i = 0; i < objectCount; i++) {
        if (static_cast<size_t>(i) >= objects->size()) break;
        GameObject* object = objects->at(i);

        if (!object) continue;

        if (object->m_objectType == GameObjectType::Decoration ||
            object->m_objectType == GameObjectType::CollisionObject ||
            object->m_objectType == GameObjectType::SecretCoin ||
            object->m_objectType == GameObjectType::UserCoin ||
            object->m_objectType == GameObjectType::Collectible ||
            object->m_objectType == GameObjectType::EnterEffectObject ||
            object->m_isGroupDisabled || object->m_isDisabled)
            continue;

        if (object->m_objectType == GameObjectType::Solid ||
            object->m_objectType == GameObjectType::Breakable) {
            if (pl->m_solidCollisionObjectsCount <
                pl->m_solidCollisionObjectsIndex) {
                pl->m_solidCollisionObjects.at(
                    pl->m_solidCollisionObjectsCount) = object;
                pl->m_solidCollisionObjectsCount++;
            } else {
                pl->m_solidCollisionObjects.push_back(object);
                pl->m_solidCollisionObjectsCount++;
                pl->m_solidCollisionObjectsIndex++;
            }

            continue;
        }

        if (object == pl->m_anticheatSpike) continue;

        if (object->m_objectType == GameObjectType::Hazard ||
            object->m_objectType == GameObjectType::AnimatedHazard) {
            if (pl->m_hazardCollisionObjectsCount <
                pl->m_hazardCollisionObjectsIndex) {
                pl->m_hazardCollisionObjects.at(
                    pl->m_hazardCollisionObjectsCount) = object;
                pl->m_hazardCollisionObjectsCount++;
            } else {
                pl->m_hazardCollisionObjects.push_back(object);
                pl->m_hazardCollisionObjectsCount++;
                pl->m_hazardCollisionObjectsIndex++;
            }

            continue;
        }
        EffectGameObject* obj = nullptr;
        if (object->m_objectType != GameObjectType::Slope) {
            obj = static_cast<EffectGameObject*>(object);
            if (obj && (Trajectory::get().fakeActivated(player, obj) ||
                        Trajectory::get().realActivated(player, obj)))
                continue;
        }

        cocos2d::CCRect rect;
        if (object->m_objectType == GameObjectType::Slope) {
            rect = object->getObjectRect(2.0, 2.0);
        } else {
            rect = object->getObjectRect();
        }

        if (object->m_objectRadius <= 0.0) {
            if (!playerRect.intersectsRect(rect)) continue;
        } else if (!pl->playerCircleCollision(player, object)) {
            continue;
        }

        bool overlaps = true;

        if (object->m_shouldUseOuterOb &&
            (!pl->m_levelSettings->m_fixRadiusCollision ||
             object->m_objectRadius <= 0.0)) {
            OBB2D* box = object->getOrientedBox();
            player->updateOrientedBox();
            OBB2D* playerBox = ((GameObject*)(player))->getOrientedBox();
            overlaps = box->overlaps1Way(playerBox);
        }

        if (object->m_objectType == GameObjectType::Slope) {
            rect = object->getObjectRect();
        }

        if (!overlaps) continue;

        if (object->m_objectID == 286 || object->m_objectID == 287) {
            if (Trajectory::get().isFake(player) && pl->canBeActivatedByPlayer(player, obj)) {
                activateForTrajectory(obj, player);
                Trajectory::get().dualPortal(pl, player, obj, object->m_objectID == 286);
            }
            continue;
        }

        switch (object->m_objectType) {
            case GameObjectType::InverseGravityPortal:
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                phys::flipGravity(player, true, true);
                playerMinX = player->getObjectRect().getMinX();
                playerMaxX = player->getObjectRect().getMaxX();
                playerMinY = player->getObjectRect().getMinY();
                playerMaxY = player->getObjectRect().getMaxY();
                break;
            case GameObjectType::NormalGravityPortal:
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                phys::flipGravity(player, false, true);
                playerMinX = player->getObjectRect().getMinX();
                playerMaxX = player->getObjectRect().getMaxX();
                playerMinY = player->getObjectRect().getMinY();
                playerMaxY = player->getObjectRect().getMaxY();
                break;
            case GameObjectType::GravityTogglePortal:
                player->m_lastPortalPos = object->getPosition();
                player->m_lastActivatedPortal = object;
                activateForTrajectory(obj, player);
                phys::flipGravity(player, !player->m_isUpsideDown, true);
                break;
            case GameObjectType::TeleportPortal:
                if (pl->canBeActivatedByPlayer(player,
                                               (EffectGameObject*)object)) {
                    phys::teleportPlayer(pl, (TeleportPortalObject*)object,
                                         player);
                    activateForTrajectory(obj, player);
                }
                break;
            case GameObjectType::Slope:
                if (!player->m_isSideways) {
                    player->collidedWithSlopeInternal(dt, object, false);
                } else {
                    cocos2d::CCRect emptyRect =
                        cocos2d::CCRect{0.0, 0.0, 0.0, 0.0};

                    player->handleRotatedCollisionInternal(
                        dt, object, emptyRect, false, false, true);
                }

                playerMinX = player->getObjectRect().getMinX();
                playerMaxX = player->getObjectRect().getMaxX();
                playerMinY = player->getObjectRect().getMinY();
                playerMaxY = player->getObjectRect().getMaxY();

                break;
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::DropRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::SpiderOrb:
            case GameObjectType::YellowJumpRing:
            case GameObjectType::TeleportOrb:
                if (!player->m_touchingRings->containsObject(object)) {
                    player->m_touchingRings->addObject(object);
                }
                player->m_touchedRings.insert(object->m_uniqueID);

                if (!player->m_isShip && !player->m_isBird &&
                    !player->m_isDart && !player->m_isSwing &&
                    !((RingObject*)object)->m_isSpawnOnly) {
                    bool used = player->m_ringRelatedSet.count(object->m_uniqueID) != 0;
                    phys::ringJump(player, (RingObject*)object);
                    if (!used && player->m_ringRelatedSet.count(object->m_uniqueID) != 0)
                        activateForTrajectory(obj, player);
                }
                break;
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad:
                phys::bumpPlayerFromGJBGL(pl, player, obj);
                break;
            case GameObjectType::GravityPad: {
                bool isFacingDown = false;
                if (player->m_isSideways) {
                    isFacingDown = object->isFacingLeft();
                } else {
                    isFacingDown = object->isFacingDown();
                }

                bool canBeActivated = pl->canBeActivatedByPlayer(player, obj);
                if (player->m_isUpsideDown == isFacingDown && canBeActivated) {
                    if (obj->m_isReverse) {
                        player->reversePlayer(obj);
                    }

                    player->m_lastPortalPos = obj->getPosition();
                    player->m_lastActivatedPortal = obj;
                    activateForTrajectory(obj, player);

                    phys::propellPlayer(player, 0.8, false, 10);
                    phys::flipGravity(player, !isFacingDown, true);
                    if (auto* live = PlayLayer::get()) Orbs::notePredictionPad(live, player, obj);
                    player->m_padRingRelated = true;
                }
                break;
            }
            case GameObjectType::MiniSizePortal:
                if (pl->canBeActivatedByPlayer(player, obj)) {
                    player->m_lastPortalPos = obj->getPosition();
                    player->m_lastActivatedPortal = obj;
                    activateForTrajectory(obj, player);

                    phys::togglePlayerScale(player, true);
                }
                break;
            case GameObjectType::RegularSizePortal:
                if (pl->canBeActivatedByPlayer(player, obj)) {
                    player->m_lastPortalPos = obj->getPosition();
                    player->m_lastActivatedPortal = obj;
                    activateForTrajectory(obj, player);

                    phys::togglePlayerScale(player, false);
                }
                break;
            case GameObjectType::Special:
                if (object->m_objectID == 0x743) {
                    player->m_stateHitHead = 2;
                } else if (object->m_objectID == 0x6db) {
                    player->m_stateDartSlide = 2;
                } else if (object->m_objectID == 0x715) {
                    player->m_stateNoAutoJump = 2;
                } else if (object->m_objectID == 0x725 && player->m_isDashing) {
                    phys::stopDashing(player);
                    player->m_jumpBuffered = false;
                } else if (object->m_objectID == 0xb32) {
                    player->m_stateFlipGravity = 2;
                } else if (object->m_objectID == 2069 ||
                           object->m_objectID == 3645) {
                    player->m_stateForce = 2;
                    ForceBlockGameObject* forceBlock =
                        (ForceBlockGameObject*)object;
                    int forceID = forceBlock->m_forceID;
                    if (forceID > 0) {
                        if (player->m_jumpPadRelated.count(forceID)) {
                            if (player->m_jumpPadRelated.at(forceID)) {
                                break;
                            }
                        }
                        player->m_jumpPadRelated.insert({forceID, true});
                    }

                    CCPoint force = forceBlock->calculateForceToTarget(player);
                    player->m_stateForceVector += force;
                }
                break;
            case GameObjectType::CubePortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
            case GameObjectType::RobotPortal:
                activatingPortal(pl, player, obj);
                break;
            case GameObjectType::EnterEffectObject:
            case GameObjectType::Modifier:
                obj->activatedByPlayer(player);

                if (obj->m_isTouchTriggered) {
                    phys::triggerObject(obj, pl, player);
                }
                break;
            default:
                break;
        }
    }
}

void triggerObject(EffectGameObject* object, GJBaseGameLayer* pl,
                   PlayerObject* player) {
    switch (object->m_objectID) {
        case 200:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 0.7;
            break;
        case 201:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 0.9;
            break;
        case 202:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 1.1;
            break;
        case 203:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 1.3;
            break;
        case 1334:
            *(float*)(&pl->m_gameState.m_timeModRelated) = 1.6;
            break;
        case 2066: {
            if (object->m_followCPP) {
                break;
            }

            bool isP2 =
                Trajectory::get().fake2 == player;
            if (!object->m_targetPlayer2 && !isP2) {
                player->m_gravityMod = object->m_gravityValue;
            }
            if (object->m_targetPlayer2 && isP2) {
                player->m_gravityMod = object->m_gravityValue;
            }
            break;
        }
        case 2900: {
            RotateGameplayGameObject* rotateObj =
                (RotateGameplayGameObject*)object;

            if (rotateObj->m_changeChannel) {
                pl->m_gameState.m_currentChannel = rotateObj->m_targetChannelID;
                pl->m_gameState
                    .m_spawnChannelRelated1[rotateObj->m_targetChannelID] =
                    rotateObj->m_groundDirection == 2 ||
                    rotateObj->m_groundDirection == 3;
            }

            if (rotateObj->m_channelOnly) break;

            auto rotate = [rotateObj](PlayerObject* p) {
                p->rotateGameplay(
                    rotateObj->m_moveDirection, rotateObj->m_groundDirection,
                    rotateObj->m_editVelocity, rotateObj->m_velocityModX,
                    rotateObj->m_velocityModY, rotateObj->m_overrideVelocity,
                    rotateObj->m_dontSlide);
            };

            rotate(player);

            if (pl->m_gameState.m_isDualMode &&
                Trajectory::get().isFake(player)) {
                PlayerObject* other = Trajectory::get().otherFake(player);
                if (other && other != player &&
                    !Trajectory::get().fakeActivated(other, object) &&
                    !Trajectory::get().realActivated(other, object)) {
                    rotate(other);
                    phys::activateForTrajectory(object, other);
                }
            }

            break;
        }
        case 3022: {
            phys::teleportPlayer(pl, (TeleportPortalObject*)object, player);
            break;
        }
        case 901:
            if (!Logic::get().predicting) pl->triggerMoveCommand(object);
            break;
        default: {
            break;
        }
    }

    phys::activateForTrajectory(object, player);
}

void checkSpawnObjects(GJBaseGameLayer* pl, PlayerObject* player) {
    CCPoint position = player->getPosition();

    cocos2d::CCArray* objects =
        (cocos2d::CCArray*)pl->m_spawnObjects->objectForKey(
            pl->m_gameState.m_currentChannel);
    if (!objects) {
        return;
    }

    const int channel = pl->m_gameState.m_currentChannel;
    const auto& related0 = pl->m_gameState.m_spawnChannelRelated0;
    const auto& related1 = pl->m_gameState.m_spawnChannelRelated1;

    auto startIt = related0.find(channel);
    auto backIt = related1.find(channel);

    int startingIndex = startIt == related0.end() ? 0 : startIt->second;
    bool goingBack = backIt == related1.end() ? false : backIt->second;

    for (int i = startingIndex; static_cast<unsigned int>(i) < objects->count();
         i++) {
        SpawnTriggerGameObject* object =
            (SpawnTriggerGameObject*)objects->objectAtIndex(i);
        if (object->m_objectID != 2066 && object->m_objectID != 2900 &&
            object->m_objectID != 3022 && object->m_objectID != 901) {
            continue;
        }

        CCPoint objectPos = object->m_speedStart;

        if (player->m_isSideways) {
            if (goingBack) {
                if (objectPos.y < position.y) break;
            } else {
                if (objectPos.y > position.y) break;
            }
        } else {
            if (goingBack) {
                if (objectPos.x < position.x) break;
            } else {
                if (objectPos.x > position.x) break;
            }
        }

        if (object->m_isGroupDisabled) continue;
        if (Trajectory::get().fakeActivated(player, object) ||
            Trajectory::get().realActivated(player, object))
            continue;

        if (!object->m_isTouchTriggered) {
            phys::triggerObject(object, pl, player);
        }
    }
}
}
