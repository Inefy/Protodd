#include "BwapiBridge.hpp"
#include "protodd/CommandEffectFeedback.hpp"
#include "TechnologyProducer.hpp"

#include "protodd/ArbiterManagement.hpp"
#include "protodd/Combat.hpp"
#include "protodd/ConstructionAnchor.hpp"
#include "protodd/PlacementSearch.hpp"
#include "protodd/PlacementSafety.hpp"
#include "protodd/RouteSafety.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/TemplarManagement.hpp"
#include "protodd/Technology.hpp"
#include "protodd/UnitMemory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace protodd::bwapi {
namespace {

using namespace BWAPI;

Position fromBwapi(const BWAPI::Position position) noexcept {
    return {position.x, position.y};
}

bool closeTo(const Position left, const Position right, const int radius) noexcept {
    return left.valid() && right.valid() && distanceSquared(left, right) <= radius * radius;
}

MovementFootprint placementFootprint(const UnitType type) noexcept {
    return {type.dimensionLeft(), type.dimensionRight(),
            type.dimensionUp(), type.dimensionDown()};
}

std::vector<Position> accessPointsAroundFootprint(
    NavigationGrid& navigation, const Position center, const UnitType structure,
    const MovementFootprint mover) {
    static constexpr std::array directions{
        Position{0, -1}, Position{1, -1}, Position{1, 0}, Position{1, 1},
        Position{0, 1}, Position{-1, 1}, Position{-1, 0}, Position{-1, -1},
    };
    std::vector<Position> points;
    const auto margin = std::max(16, navigation.cellSize() * 2);
    const auto searchRadius = std::max(64, navigation.cellSize() * 4);
    for (const auto direction : directions) {
        const auto dx = direction.x < 0
            ? -(structure.dimensionLeft() + mover.right + margin)
            : direction.x > 0
                ? structure.dimensionRight() + mover.left + margin : 0;
        const auto dy = direction.y < 0
            ? -(structure.dimensionUp() + mover.down + margin)
            : direction.y > 0
                ? structure.dimensionDown() + mover.up + margin : 0;
        const Position requested{center.x + dx, center.y + dy};
        const auto point = navigation.nearestWalkable(requested, searchRadius, mover);
        if (point.valid() && std::ranges::find(points, point) == points.end())
            points.push_back(point);
    }
    return points;
}

std::vector<Position> accessPointsAroundPosition(
    NavigationGrid& navigation, const Position center, const MovementFootprint mover) {
    static constexpr std::array directions{
        Position{0, 0}, Position{0, -1}, Position{1, -1}, Position{1, 0},
        Position{1, 1}, Position{0, 1}, Position{-1, 1}, Position{-1, 0},
        Position{-1, -1},
    };
    std::vector<Position> points;
    const auto radius = std::max(64, navigation.cellSize() * 4);
    for (const auto direction : directions) {
        const Position requested{center.x + direction.x * radius,
                                 center.y + direction.y * radius};
        const auto point = navigation.nearestWalkable(requested, radius, mover);
        if (point.valid() && std::ranges::find(points, point) == points.end())
            points.push_back(point);
    }
    return points;
}

void addPlacementAccessRequirement(
    std::vector<PlacementAccessRequirement>& requirements,
    const std::span<const Position> sources,
    const std::span<const Position> targets,
    const MovementFootprint footprint) {
    if (sources.empty() || targets.empty()) return;
    PlacementAccessRequirement requirement;
    requirement.alternatives.reserve(std::min<std::size_t>(16, sources.size() + targets.size()));
    // Cover every mineral-side access point from one source, then try other
    // producer/depot exits to a representative target. This keeps a failed
    // candidate bounded while still allowing either end of a lane to reroute.
    for (const auto target : targets) {
        if (sources.front().valid() && target.valid())
            requirement.alternatives.push_back(
                {sources.front(), target, footprint, {}, 0});
        if (requirement.alternatives.size() >= 9) break;
    }
    if (!targets.empty()) {
        for (std::size_t index = 1; index < sources.size() &&
             requirement.alternatives.size() < 16; ++index) {
            if (sources[index].valid() && targets.front().valid())
                requirement.alternatives.push_back(
                    {sources[index], targets.front(), footprint, {}, 0});
        }
    }
    if (!requirement.alternatives.empty()) requirements.push_back(std::move(requirement));
}

NavigationObstacleBounds placementObstacleBounds(
    const Position center, const UnitType type) noexcept {
    return {center.x - type.dimensionLeft(), center.y - type.dimensionUp(),
            center.x + type.dimensionRight(), center.y + type.dimensionDown()};
}

bool arbiterStagingSafe(const Unit arbiter) {
    if (arbiter == nullptr || !arbiter->exists() || arbiter->isUnderAttack()) return false;
    for (const auto enemy : Broodwar->enemy()->getUnits()) {
        if (enemy == nullptr || !enemy->exists() || !enemy->isVisible()) continue;
        const auto distanceToStaging = enemy->getDistance(arbiter);
        const auto airWeapon = enemy->getType().airWeapon();
        const auto groundWeapon = enemy->getType().groundWeapon();
        if ((airWeapon != WeaponTypes::None &&
             distanceToStaging <= airWeapon.maxRange() + 128) ||
            (groundWeapon != WeaponTypes::None &&
             distanceToStaging <= std::max(160, groundWeapon.maxRange() + 128))) {
            return false;
        }
    }
    return true;
}

int availableRecallLandingSlots(const BWAPI::Position center) {
    auto slots = 0;
    const auto mapRight = Broodwar->mapWidth() * 32;
    const auto mapBottom = Broodwar->mapHeight() * 32;
    for (auto dy = -112; dy <= 112; dy += 32) {
        for (auto dx = -112; dx <= 112; dx += 32) {
            if (dx * dx + dy * dy > 112 * 112) continue;
            const BWAPI::Position landing{center.x + dx, center.y + dy};
            if (landing.x < 16 || landing.y < 16 || landing.x >= mapRight - 16 ||
                landing.y >= mapBottom - 16 ||
                !Broodwar->isWalkable(landing.x / 8, landing.y / 8)) {
                continue;
            }
            const auto occupied = std::ranges::any_of(
                Broodwar->getUnitsInRadius(landing, 20), [](const Unit unit) {
                    return unit != nullptr && unit->exists() && !unit->isFlying();
                });
            if (!occupied) ++slots;
        }
    }
    return slots;
}

bool canAttackSoon(const Unit attacker, const Unit target, const int margin) {
    if (attacker == nullptr || target == nullptr || !attacker->exists() ||
        !target->exists()) return false;
    const auto weapon = target->isFlying() ? attacker->getType().airWeapon()
                                           : attacker->getType().groundWeapon();
    return weapon != WeaponTypes::None &&
           attacker->getDistance(target) <= weapon.maxRange() + margin;
}

ArbiterStasisOpportunity assessArbiterStasis(
    const Unit center, const Player self, const Player enemyPlayer) {
    ArbiterStasisOpportunity assessment;
    if (center == nullptr || self == nullptr || enemyPlayer == nullptr) return assessment;

    const auto centerPosition = fromBwapi(center->getPosition());
    const auto insideSpell = [&centerPosition](const Unit unit) {
        return unit != nullptr && closeTo(centerPosition, fromBwapi(unit->getPosition()), 96);
    };
    for (const auto unit : Broodwar->getUnitsInRadius(center->getPosition(), 96)) {
        if (unit == nullptr || !unit->exists() || unit->getType().isBuilding()) continue;
        const auto value = unitStats(BwapiBridge::toKind(unit->getType())).combatValue;
        if (unit->getPlayer() == enemyPlayer && unit->isVisible() && !unit->isStasised())
            assessment.enemyValueAffected += value;
        else if (unit->getPlayer() == self)
            assessment.friendlyValueAffected += value;
    }

    const auto allies = Broodwar->getUnitsInRadius(center->getPosition(), 320, Filter::IsOwned);
    const auto targets = Broodwar->getUnitsInRadius(center->getPosition(), 480, Filter::IsEnemy);
    std::unordered_set<int> actionableTargets;
    for (const auto ally : allies) {
        if (ally == nullptr || !ally->exists() || !ally->isCompleted() ||
            ally->getType().isBuilding() ||
            !isCombatUnit(BwapiBridge::toKind(ally->getType())) || insideSpell(ally)) {
            continue;
        }
        auto canFollowThrough = false;
        for (const auto target : targets) {
            if (target == nullptr || !target->exists() || !target->isVisible() ||
                target->isStasised() || insideSpell(target)) continue;
            if (canAttackSoon(ally, target, 192)) {
                actionableTargets.insert(target->getID());
                canFollowThrough = true;
            }
        }
        if (canFollowThrough)
            assessment.friendlyFollowThroughValue +=
                unitStats(BwapiBridge::toKind(ally->getType())).combatValue;
    }
    for (const auto target : targets) {
        if (target == nullptr || !target->exists() || !target->isVisible() ||
            target->isStasised() || insideSpell(target) ||
            !actionableTargets.contains(target->getID())) continue;
        assessment.actionableEnemyValueAfterCast +=
            unitStats(BwapiBridge::toKind(target->getType())).combatValue;
    }

    for (const auto ally : Broodwar->getUnitsInRadius(center->getPosition(), 320, Filter::IsOwned)) {
        if (ally == nullptr || !ally->exists() || ally->getType().isBuilding() ||
            insideSpell(ally)) continue;
        for (const auto target : Broodwar->getUnitsInRadius(center->getPosition(), 96, Filter::IsEnemy)) {
            if (target != nullptr && target->exists() && target->isVisible() &&
                !target->isStasised() && canAttackSoon(target, ally, 64)) {
                assessment.protectsThreatenedAlly = true;
                break;
            }
        }
        if (assessment.protectsThreatenedAlly) break;
    }
    return assessment;
}

struct ResourceCost {
    int minerals{};
    int gas{};
};

ResourceCost resourceCost(const BWAPI::UnitCommand& command,
                          const BWAPI::Player self) noexcept {
    using namespace BWAPI;
    switch (command.getType()) {
        case UnitCommandTypes::Build:
        case UnitCommandTypes::Build_Addon:
        case UnitCommandTypes::Train:
        case UnitCommandTypes::Morph: {
            const auto type = command.getUnitType();
            if (type == UnitTypes::None) return {};
            return {type.mineralPrice(), type.gasPrice()};
        }
        case UnitCommandTypes::Research: {
            const auto type = command.getTechType();
            if (type == TechTypes::None) return {};
            return {type.mineralPrice(), type.gasPrice()};
        }
        case UnitCommandTypes::Upgrade: {
            const auto type = command.getUpgradeType();
            if (type == UpgradeTypes::None || self == nullptr) return {};
            const auto level = self->getUpgradeLevel(type) + 1;
            if (level > type.maxRepeats()) return {};
            return {type.mineralPrice(level), type.gasPrice(level)};
        }
        default:
            return {};
    }
}

constexpr std::uint64_t automaticBuildTaskBit = std::uint64_t{1} << 63U;
constexpr std::uint64_t plannedNavigationObstacleBit = std::uint64_t{1} << 63U;

std::uint64_t plannedNavigationObstacleId(const std::uint64_t taskId) noexcept {
    return plannedNavigationObstacleBit | (taskId & ~plannedNavigationObstacleBit);
}

std::uint64_t liveNavigationObstacleId(const UnitId id) noexcept {
    return static_cast<std::uint32_t>(id);
}

std::uint64_t buildTaskKey(const UnitKind kind,
                           const ConstructionTaskSite& constructionSite) noexcept {
    if (constructionSite.valid()) return constructionSite.id;
    return automaticBuildTaskBit | static_cast<std::uint64_t>(kind);
}

std::string_view buildBlockerName(const BuildBlockerReason reason) noexcept {
    switch (reason) {
        case BuildBlockerReason::noBuilder: return "no-builder";
        case BuildBlockerReason::unsafeRoute: return "unsafe-route";
        case BuildBlockerReason::missingPrerequisite: return "missing-prerequisite";
        case BuildBlockerReason::noPlacement: return "no-placement";
        case BuildBlockerReason::noPower: return "no-power";
        case BuildBlockerReason::rejectedFootprint: return "rejected-footprint";
    }
    return "unknown";
}

template <class PendingBuilds>
auto findPendingBuild(PendingBuilds& builds, const UnitKind kind) {
    return std::ranges::find_if(builds, [kind](const auto& entry) {
        return entry.second.kind == kind;
    });
}

}  // namespace

void BwapiBridge::onStart() {
    diagnosticErrors_ = 0;
    spendingLedger_ = nullptr;
    lastIssueError_ = BWAPI::Errors::None;
    selfUnitLifetimes_.clear();
    enemyMemory_.clear();
    incomingProjectileMemory_.clear();
    resourceIncomeTracker_.reset();
    mineralAllocator_.reset();
    baseLastScouted_.clear();
    baseLastConfirmedEmpty_.clear();
    pendingBuilds_.clear();
    activeNavigation_ = nullptr;
    buildBlockers_.clear();
    failedBuildSites_.clear();
    placementSearches_.clear();
    unitCommandLocks_.clear();
    learnedCommandLeases_.clear();
    workerCommandLeases_.clear();
    issuedCommandLeaseGenerations_.clear();
    frameCommandClaims_.clear();
    frameCommandBus_ = nullptr;
    constructionProposalFrame_ = -1;
    constructionCommandProposals_.clear();
    constructionCommandQueuedThisAction_ = false;
    enemyMainRouteSource_ = {-1, -1};
    enemyRoutesInitialized_ = false;
    defensesInitialized_ = false;
    recentAreaSpells_.clear();
    lastMacroStatus_ = "idle";
    discoverResourceClusters();
}

void BwapiBridge::beginFrameCommands(CommandBus& commands, const Frame frame) {
    if (constructionProposalFrame_ != frame) {
        constructionCommandProposals_.clear();
        constructionProposalFrame_ = frame;
    }
    frameCommandBus_ = &commands;
    constructionCommandQueuedThisAction_ = false;
}

void BwapiBridge::beginCommandBudget(const Frame frame,
                                     const std::size_t maximumCommands) noexcept {
    commandBudget_.beginFrame(frame, maximumCommands,
                              defaultUrgentCommandReserve(maximumCommands));
    dispatchingFrameCommand_ = false;
}

void BwapiBridge::endFrameCommands() noexcept {
    constructionCommandProposals_.clear();
    frameCommandBus_ = nullptr;
    constructionCommandQueuedThisAction_ = false;
}

bool BwapiBridge::executeFrameCommand(const Command& command, bool* resourcesPaid) {
    const auto wasDispatchingFrameCommand = dispatchingFrameCommand_;
    dispatchingFrameCommand_ = true;
    struct RestoreDispatchState {
        bool& state;
        bool previous;
        ~RestoreDispatchState() { state = previous; }
    } restore{dispatchingFrameCommand_, wasDispatchingFrameCommand};
    const auto proposal = std::ranges::find_if(
        constructionCommandProposals_, [&command](const ConstructionCommandProposal& candidate) {
            return candidate.command == command;
        });
    if (proposal == constructionCommandProposals_.end())
        return execute(command, resourcesPaid);

    auto feedback = std::move(proposal->feedback);
    constructionCommandProposals_.erase(proposal);
    bool paid = false;
    const auto accepted = execute(command, &paid);
    if (resourcesPaid != nullptr) *resourcesPaid = paid;
    if (feedback) feedback(accepted, paid);
    return accepted;
}

GameState BwapiBridge::observe() {
    GameState state;
    state.frame = Broodwar->getFrameCount();
    state.latencyFrames = Broodwar->getLatencyFrames();
    state.mapWidthPixels = Broodwar->mapWidth() * 32;
    state.mapHeightPixels = Broodwar->mapHeight() * 32;
    state.mapName = Broodwar->mapName();
    state.self = snapshotPlayer(Broodwar->self(), true);
    state.enemy = snapshotPlayer(Broodwar->enemy(), false);
    const auto income = resourceIncomeTracker_.update(
        state.frame, state.self.gatheredMinerals, state.self.gatheredGas);
    state.estimatedMineralIncomePerMinute = income.mineralsPerMinute;
    state.estimatedGasIncomePerMinute = income.gasPerMinute;
    std::unordered_set<UnitId> observedSelfUnits;
    observedSelfUnits.reserve(state.self.units.size());
    for (auto& unit : state.self.units) {
        observedSelfUnits.insert(unit.id);
        auto [lifetime, inserted] = selfUnitLifetimes_.try_emplace(
            unit.id, UnitLifetime{unit.kind, state.frame, unit.hitPoints, unit.shields, -1});
        if (!inserted && lifetime->second.kind != unit.kind) {
            lifetime->second = {unit.kind, state.frame, unit.hitPoints, unit.shields, -1};
        } else if (!inserted) {
            if (unit.hitPoints < lifetime->second.lastHitPoints ||
                unit.shields < lifetime->second.lastShields)
                lifetime->second.lastDamageFrame = state.frame;
            lifetime->second.lastHitPoints = unit.hitPoints;
            lifetime->second.lastShields = unit.shields;
        }
        unit.firstSeen = lifetime->second.firstSeen;
        unit.recentlyDamaged = unit.underAttack ||
            (lifetime->second.lastDamageFrame >= 0 &&
             state.frame - lifetime->second.lastDamageFrame <=
                 std::max(8, state.latencyFrames + 3));
    }
    std::erase_if(selfUnitLifetimes_, [&observedSelfUnits](const auto& entry) {
        return !observedSelfUnits.contains(entry.first);
    });
    if (const auto self = Broodwar->self()) {
        const auto home = Broodwar->getClosestUnit(
            BWAPI::Position(self->getStartLocation()),
            BWAPI::Filter::IsOwned && BWAPI::Filter::IsCompleted &&
                BWAPI::Filter::GetType == BWAPI::UnitTypes::Protoss_Nexus);
        if (home != nullptr) {
            auto bestTravel = std::numeric_limits<int>::max();
            const auto unavailable = reservedBuilders();
            for (const auto probe : self->getUnits()) {
                if (probe == nullptr || !probe->exists() || !probe->isCompleted() ||
                    probe->getType() != BWAPI::UnitTypes::Protoss_Probe ||
                    probe->isConstructing() || probe->isTraining() || probe->isLoaded() ||
                    !probe->isInterruptible() ||
                    std::ranges::find(unavailable, probe->getID()) != unavailable.end() ||
                    learnedCommandLeases_.contains(probe->getID()) ||
                    std::ranges::any_of(pendingBuilds_, [probe](const auto& entry) {
                        return entry.second.builder == probe->getID();
                    })) {
                    continue;
                }
                const auto destination = BWAPI::Position(home->getPosition());
                if (!probe->hasPath(destination)) continue;
                const auto speed = std::max(0.001, probe->getType().topSpeed());
                const auto travel = static_cast<int>(std::ceil(
                    probe->getDistance(destination) / speed)) +
                    std::max(1, state.latencyFrames);
                bestTravel = std::min(bestTravel, std::max(1, travel));
            }
            if (bestTravel != std::numeric_limits<int>::max())
                state.pylonBuilderTravelFrames = std::clamp(bestTravel, 1, 2 * 60 * 24);
        }
    }

    if (const auto enemy = Broodwar->enemy()) {
        for (const auto unit : enemy->getUnits()) {
            if (unit != nullptr && unit->exists() && unit->isVisible() &&
                unit->isDetected() &&
                !enemyMemory_.contains(unit->getID())) {
                remember(unit);
            }
        }
    }
    state.enemy.units.clear();
    state.enemy.units.reserve(enemyMemory_.size());
    for (auto iterator = enemyMemory_.begin(); iterator != enemyMemory_.end();) {
        auto& memory = iterator->second;
        const auto live = Broodwar->getUnit(memory.id);
        // BWAPI can report a cloaked/burrowed unit as visible while withholding
        // detection. On that path getType(), position and order fields still
        // describe live engine state, so keep the last detected snapshot until
        // the unit is detected again.
        std::optional<UnitSnapshot> current;
        if (live != nullptr && live->exists() && live->isVisible() &&
            live->isDetected()) {
            current = snapshotUnit(live, false);
        }
        auto emptyFootprintFullyVisible = false;
        auto typeCanFly = false;
        if (!current && isBuilding(memory.kind) && memory.position.valid()) {
            const BWAPI::UnitType type(memory.typeId);
            const auto width = std::max(1, type.tileWidth());
            const auto height = std::max(1, type.tileHeight());
            const auto left = (memory.position.x - width * 16) / 32;
            const auto top = (memory.position.y - height * 16) / 32;
            std::vector<std::uint8_t> visibleTiles;
            visibleTiles.reserve(static_cast<std::size_t>(width * height));
            auto insideMap = true;
            for (auto y = 0; y < height; ++y) {
                for (auto x = 0; x < width; ++x) {
                    const auto tileX = left + x;
                    const auto tileY = top + y;
                    if (tileX < 0 || tileY < 0 || tileX >= Broodwar->mapWidth() ||
                        tileY >= Broodwar->mapHeight()) {
                        insideMap = false;
                        visibleTiles.push_back(0);
                        continue;
                    }
                    visibleTiles.push_back(static_cast<std::uint8_t>(
                        Broodwar->isVisible(BWAPI::TilePosition(tileX, tileY))));
                }
            }
            emptyFootprintFullyVisible = insideMap &&
                fullyVisibleFootprint(width, height, visibleTiles);
            typeCanFly = type.isFlyingBuilding();
        }
        auto result = reconcileEnemyMemory(memory, std::move(current), state.frame,
                                           emptyFootprintFullyVisible, typeCanFly);
        if (result.forget) {
            iterator = enemyMemory_.erase(iterator);
            continue;
        }
        memory = std::move(result.snapshot);
        state.enemy.units.push_back(memory);
        ++iterator;
    }
    std::ranges::sort(state.enemy.units, {}, &UnitSnapshot::id);

    // Only count visible, still-travelling shots whose damage semantics we
    // can identify unambiguously. Hitscan effects and impact animations must
    // never reserve damage that has already been applied to observed HP.
    std::vector<IncomingProjectile> incoming;
    std::unordered_set<int> observedProjectileIds;
    for (const auto bullet : Broodwar->getBullets()) {
        if (bullet == nullptr || !bullet->exists() || !bullet->isVisible() ||
            bullet->getPlayer() != Broodwar->self()) continue;
        const auto bulletId = bullet->getID();
        if (bulletId < 0) continue;
        observedProjectileIds.insert(bulletId);
        const auto source = bullet->getSource();
        const auto target = bullet->getTarget();
        if (target == nullptr || !target->exists() || !target->isVisible() ||
            !target->isDetected() || target->getPlayer() != Broodwar->enemy()) continue;
        const auto familyForLiveSource = [&]() {
            if (source == nullptr || !source->exists() ||
                source->getPlayer() != Broodwar->self())
                return IncomingProjectileFamily::unknown;
            if (source->getType() == BWAPI::UnitTypes::Protoss_Dragoon &&
                bullet->getType() == BWAPI::BulletTypes::Phase_Disruptor)
                return IncomingProjectileFamily::dragoonPhaseDisruptor;
            if (source->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon &&
                bullet->getType() == BWAPI::BulletTypes::STA_STS_Cannon_Overlay)
                return IncomingProjectileFamily::photonCannonOverlay;
            return IncomingProjectileFamily::unknown;
        };
        const auto bulletTypeMatchesFamily = [&bullet](const IncomingProjectileFamily family) {
            return (family == IncomingProjectileFamily::dragoonPhaseDisruptor &&
                    bullet->getType() == BWAPI::BulletTypes::Phase_Disruptor) ||
                   (family == IncomingProjectileFamily::photonCannonOverlay &&
                    bullet->getType() == BWAPI::BulletTypes::STA_STS_Cannon_Overlay);
        };
        if (!bullet->getPosition().isValid()) continue;
        const auto targetSnapshot = std::ranges::find(state.enemy.units, target->getID(),
                                                       &UnitSnapshot::id);
        if (targetSnapshot == state.enemy.units.end()) continue;

        auto tracked = incomingProjectileMemory_.find(bulletId);
        if (tracked == incomingProjectileMemory_.end()) {
            const auto family = familyForLiveSource();
            if (family == IncomingProjectileFamily::unknown) continue;
            const auto sourceSnapshot = std::ranges::find(state.self.units, source->getID(),
                                                           &UnitSnapshot::id);
            if (sourceSnapshot == state.self.units.end()) continue;
            tracked = incomingProjectileMemory_.emplace(bulletId, TrackedProjectile{
                family, source->getID(), target->getID(), sourceSnapshot->firstSeen,
                targetSnapshot->firstSeen, sourceSnapshot->groundWeapon,
                sourceSnapshot->airWeapon}).first;
        }
        const auto& record = tracked->second;
        if (record.target != target->getID() ||
            record.targetFirstSeen != targetSnapshot->firstSeen ||
            !bulletTypeMatchesFamily(record.family)) continue;
        if (source != nullptr) {
            if (source->getID() != record.source) continue;
            if (source->exists()) {
                if (familyForLiveSource() != record.family) continue;
                const auto sourceSnapshot = std::ranges::find(
                    state.self.units, source->getID(), &UnitSnapshot::id);
                if (sourceSnapshot == state.self.units.end() ||
                    sourceSnapshot->firstSeen != record.sourceFirstSeen) continue;
            }
        }
        const auto speed = std::hypot(bullet->getVelocityX(), bullet->getVelocityY());
        if (speed < 0.5 || bullet->getRemoveTimer() <= 0) continue;
        // Estimate travel to the target's collision bounds, not its center. A
        // bolt already inside those bounds has impacted even if its visual
        // overlay remains in BWAPI's bullet set for another frame.
        const auto bulletPosition = fromBwapi(bullet->getPosition());
        const auto targetPosition = fromBwapi(target->getPosition());
        const auto offsetX = bulletPosition.x - targetPosition.x;
        const auto offsetY = bulletPosition.y - targetPosition.y;
        const auto extentX = offsetX < 0 ? targetSnapshot->dimensionLeft
                                         : targetSnapshot->dimensionRight;
        const auto extentY = offsetY < 0 ? targetSnapshot->dimensionUp
                                         : targetSnapshot->dimensionDown;
        const auto impactDistance = std::hypot(
            std::max(0, std::abs(offsetX) - extentX),
            std::max(0, std::abs(offsetY) - extentY));
        const auto flightFrames = impactDistance / speed;
        if (flightFrames <= 1.0) {
            incomingProjectileMemory_.erase(tracked);
            continue;
        }
        if (flightFrames > std::min(24, bullet->getRemoveTimer())) continue;
        incoming.push_back({.id = bulletId, .source = record.source,
            .target = record.target, .sourceFirstSeen = record.sourceFirstSeen,
            .targetFirstSeen = record.targetFirstSeen, .impactFrames = flightFrames,
            .family = record.family, .groundWeapon = record.groundWeapon,
            .airWeapon = record.airWeapon, .sourceLifetimeVerified = true});
    }
    std::erase_if(incomingProjectileMemory_, [&observedProjectileIds](const auto& entry) {
        return !observedProjectileIds.contains(entry.first);
    });
    accountIncomingDamage(state, incoming);

    const auto frame = state.frame;
    std::erase_if(failedBuildSites_, [frame](const FailedBuildSite& site) {
        return site.expires <= frame;
    });
    std::erase_if(unitCommandLocks_, [frame](const auto& entry) {
        return entry.second <= frame;
    });
    std::erase_if(learnedCommandLeases_, [frame](const auto& entry) {
        return entry.second <= frame;
    });
    for (auto& [taskId, pending] : pendingBuilds_) {
        static_cast<void>(taskId);
        const auto builder = Broodwar->getUnit(pending.builder);
        if (builder == nullptr || !builder->exists()) continue;
        const auto type = toBwapi(pending.kind);
        const BWAPI::TilePosition targetTile{pending.target.x / 32, pending.target.y / 32};
        const BWAPI::Position center{
            pending.target.x + type.tileWidth() * 16,
            pending.target.y + type.tileHeight() * 16};
        const auto builderPosition = fromBwapi(builder->getPosition());
        const auto targetDistance = builder->getDistance(center);
        auto travelProgress = BuildTravelProgress{
            pending.travelDeadline, pending.hardTravelDeadline,
            pending.bestDistanceToTarget};
        if (recordBuildTravelProgress(
                travelProgress, frame, static_cast<int>(targetDistance))) {
            pending.travelDeadline = travelProgress.travelDeadline;
            pending.bestDistanceToTarget = travelProgress.bestDistancePixels;
        }
        if (!pending.lastPosition.valid() ||
            distanceSquared(pending.lastPosition, builderPosition) >= 16 * 16) {
            pending.lastPosition = builderPosition;
            pending.lastRouteProgress = frame;
        }
        if (pending.commandIssued < 0) {
            pending.phase = BuildTaskPhase::prepositioning;
        } else {
            const auto last = builder->getLastCommand();
            const auto commandedBuild = last.getType() == BWAPI::UnitCommandTypes::Build &&
                last.getUnitType() == type &&
                fromBwapi(last.getTargetPosition()) == pending.target;
            if (builder->getBuildType() == type || commandedBuild) {
                pending.phase = BuildTaskPhase::acknowledged;
                if (pending.commandAcknowledged < 0) pending.commandAcknowledged = frame;
            } else if (targetDistance > 96) {
                pending.phase = BuildTaskPhase::travelling;
            } else {
                pending.phase = BuildTaskPhase::commandPending;
            }
        }
        if (pending.commandIssued < 0 && targetDistance <= 96) {
            pending.footprintAccessible =
                Broodwar->canBuildHere(targetTile, type, builder, true) ||
                Broodwar->canBuildHere(targetTile, type, nullptr, true);
        }
    }
    const auto requestCancellation = [this, frame](PendingBuild& pending, BWAPI::Unit builder,
                                                    const UnitKind kind) {
        if (builder == nullptr || !builder->exists()) return true;
        const auto type = toBwapi(kind);
        const auto last = builder->getLastCommand();
        const auto commandedBuild = last.getType() == BWAPI::UnitCommandTypes::Build &&
            last.getUnitType() == type && fromBwapi(last.getTargetPosition()) == pending.target;
        const auto prepositionOrder = pending.prepositioned &&
            last.getType() == BWAPI::UnitCommandTypes::Move &&
            (last.getTargetPosition() == BWAPI::Position(
                 pending.target.x + type.tileWidth() * 16,
                 pending.target.y + type.tileHeight() * 16) ||
             (pending.routeWaypoint.valid() &&
              distanceSquared(fromBwapi(last.getTargetPosition()),
                              pending.routeWaypoint) <= 96 * 96) ||
             (pending.travelWaypoint.valid() &&
              distanceSquared(fromBwapi(last.getTargetPosition()),
                              pending.travelWaypoint) <= 96 * 96));
        const auto actionable = commandedBuild || builder->getBuildType() == type || prepositionOrder;
        if (pending.cancellation.awaiting() && !actionable) {
            static_cast<void>(pending.cancellation.acknowledged(false));
            return true;
        }
        if (!actionable) return true;
        if (pending.cancellation.awaiting()) {
            if (pending.cancellation.retryDue(frame))
                requestBuildCancellation(pending, builder, "builder-cancel-retry");
            return false;
        }
        if (!pending.cancellation.requestDue(frame)) return false;
        requestBuildCancellation(pending, builder, "builder-release");
        return false;
    };
    const auto pendingBuildShouldExpire = [this, frame, &requestCancellation](
        const std::uint64_t taskId, PendingBuild& pending) {
        static_cast<void>(taskId);
        const auto kind = pending.kind;
        const auto started = std::ranges::any_of(
            Broodwar->self()->getUnits(), [kind, &pending](const Unit unit) {
            if (unit == nullptr || !unit->exists() || toKind(unit->getType()) != kind) {
                return false;
            }
            // A nearby older Pylon/Gateway cannot fulfill this reservation.
            return fromBwapi(BWAPI::Position(unit->getTilePosition())) == pending.target;
        });
        if (started) {
            pending.phase = BuildTaskPhase::constructing;
            return true;
        }

        const auto rememberFailure = [this, kind, frame, &pending] {
            failedBuildSites_.push_back(
                {kind, pending.target, frame + 20 * 24});
        };

        const auto builder = Broodwar->getUnit(pending.builder);
        if (builder == nullptr || !builder->exists()) {
            rememberFailure();
            return true;
        }
        const auto commandAge = frame -
            (pending.commandIssued >= 0 ? pending.commandIssued : pending.issued);
        const auto expectedType = toBwapi(kind);
        const auto lastCommand = builder->getLastCommand();
        const auto commandedBuild = lastCommand.getType() == BWAPI::UnitCommandTypes::Build &&
            lastCommand.getUnitType() == expectedType &&
            fromBwapi(lastCommand.getTargetPosition()) == pending.target;
        const auto reportLeaseEnd = [this, kind, frame, &pending, builder, expectedType,
                                     commandedBuild](std::string_view reason) {
            if (!buildLeaseDiagnostic) return;
            try {
                const BWAPI::TilePosition tile{pending.target.x / 32, pending.target.y / 32};
                const BWAPI::Position center{
                    pending.target.x + expectedType.tileWidth() * 16,
                    pending.target.y + expectedType.tileHeight() * 16};
                buildLeaseDiagnostic({
                    kind, pending.builder, pending.target, fromBwapi(builder->getPosition()),
                    pending.issued, frame, pending.lastRouteProgress,
                    pending.travelDeadline, pending.hardTravelDeadline,
                    pending.bestDistanceToTarget, std::string(reason),
                    builder->getOrder().toString(), commandedBuild,
                    builder->getBuildType() == expectedType,
                    Broodwar->canBuildHere(tile, expectedType, builder, true),
                    Broodwar->canBuildHere(tile, expectedType, nullptr, true),
                    builder->hasPath(center),
                });
            } catch (...) { ++diagnosticErrors_; }
        };
        if (pending.cancellation.awaiting()) {
            if (!requestCancellation(pending, builder, kind)) return false;
            rememberFailure();
            return true;
        }
        const BWAPI::Position targetCenter{
            pending.target.x + expectedType.tileWidth() * 16,
            pending.target.y + expectedType.tileHeight() * 16};
        const auto distanceToTarget = builder->getDistance(targetCenter);
        if (pending.commandIssued >= 0 && distanceToTarget > 96 &&
            commandAge > std::max(2 * 24, Broodwar->getLatencyFrames() + 12) &&
            pending.lastRouteProgress >= 0 && frame - pending.lastRouteProgress > 2 * 24) {
            // Keep travelling builders leased. A stalled order must be
            // cancelled before its resources and worker can be reassigned,
            // otherwise it may complete after a replacement is already sent.
            // Physical movement is route progress even around terrain. The
            // fixed, distance-scaled deadline below still catches an orbiting
            // worker whose order never reaches its exact footprint.
            reportLeaseEnd("stalled-builder");
            if (!requestCancellation(pending, builder, kind)) return false;
            rememberFailure();
            return true;
        }
        if (frame >= pending.travelDeadline) {
            // Distance-scaled deadlines preserve long routes but cap orbiting,
            // inaccessible footprints, and pre-positioned expansion orders.
            reportLeaseEnd(pending.footprintAccessible
                ? "travel-deadline" : "footprint-inaccessible-deadline");
            if (!requestCancellation(pending, builder, kind)) return false;
            rememberFailure();
            return true;
        }
        // A pre-positioned expansion Probe is intentionally only moving until
        // its fogged footprint becomes commandable, so BWAPI reports no
        // build type during that interval. Ordinary construction leases still
        // require the explicit build type to prevent a redirected Probe from
        // blocking another macro action.
        const auto prepositionOrder = pending.prepositioned &&
            lastCommand.getType() == BWAPI::UnitCommandTypes::Move &&
            (lastCommand.getTargetPosition() == targetCenter ||
             (pending.routeWaypoint.valid() &&
              distanceSquared(fromBwapi(lastCommand.getTargetPosition()),
                              pending.routeWaypoint) <= 96 * 96) ||
             (pending.travelWaypoint.valid() &&
              distanceSquared(fromBwapi(lastCommand.getTargetPosition()),
                              pending.travelWaypoint) <= 96 * 96));
        const auto stillAssigned = prepositionOrder || commandedBuild ||
                                   builder->getBuildType() == expectedType;
        // Give the command time to cross the latency boundary, then recover
        // quickly if another subsystem or the game rejected the order. Keep a
        // genuinely travelling builder reserved long enough for expansions.
        if (commandAge <= std::max(12, Broodwar->getLatencyFrames() + 6)) return false;
        if (!stillAssigned) {
            reportLeaseEnd("order-lost");
            rememberFailure();
            return true;
        }
        return false;
    };
    for (auto pending = pendingBuilds_.begin(); pending != pendingBuilds_.end();) {
        if (pendingBuildShouldExpire(pending->first, pending->second)) {
            if (activeNavigation_ != nullptr)
                activeNavigation_->removeDynamicObstacle(
                    plannedNavigationObstacleId(pending->first));
            pending = pendingBuilds_.erase(pending);
        } else {
            ++pending;
        }
    }

    // Defer expensive pathfinding until the game has entered its frame loop.
    // Doing this in onStart can leave StarCraft responsive but prevent the
    // injected module from reaching its first telemetry record.
    if (!defensesInitialized_ && frame >= 24) {
        const auto terrain = navigationGrid();
        const Position ourStart{Broodwar->self()->getStartLocation().x * 32 + 64,
                                Broodwar->self()->getStartLocation().y * 32 + 48};
        std::vector<Position> approaches{{Broodwar->mapWidth() * 16, Broodwar->mapHeight() * 16}};
        for (const auto start : Broodwar->getStartLocations())
            approaches.push_back({start.x * 32 + 64, start.y * 32 + 48});
        for (auto& site : resourceSites_) {
            const auto route = terrain.findPath(ourStart, site.depotCenter, 100000);
            if (route.reached()) {
                double length = 0.0;
                for (std::size_t index = 1; index < route.points.size(); ++index) {
                    length += distance(route.points[index - 1], route.points[index]);
                }
                site.groundDistanceFromStart = static_cast<int>(std::lround(length));
            }
            for (const auto approach : approaches) {
                if (distanceSquared(site.depotCenter, approach) < 384 * 384) continue;
                const auto defense = terrain.defensivePosition(site.depotCenter, approach);
                if (!defense.valid() || std::ranges::any_of(site.defenses,
                    [&defense](const DefensivePosition& prior) {
                        return distanceSquared(prior.entrance, defense.entrance) < 160 * 160;
                    })) continue;
                site.defenses.push_back(defense);
            }
        }
        defensesInitialized_ = true;
    }
    state.bases = snapshotBases(state);
    for (const auto bullet : Broodwar->getBullets()) {
        if (bullet != nullptr && bullet->exists() && bullet->isVisible() &&
            bullet->getType() == BulletTypes::Psionic_Storm && bullet->getPosition().isValid())
            state.storms.push_back(fromBwapi(bullet->getPosition()));
    }
    for (const auto effect : Broodwar->getAllUnits()) {
        if (effect != nullptr && effect->exists() && effect->isVisible() &&
            effect->getType() == UnitTypes::Spell_Scanner_Sweep &&
            effect->getPosition().isValid())
            state.scannerSweeps.push_back(fromBwapi(effect->getPosition()));
    }
    return state;
}

NavigationGrid BwapiBridge::navigationGrid() const {
    const auto width = Broodwar->mapWidth() * 4;
    const auto height = Broodwar->mapHeight() * 4;
    std::vector<std::uint8_t> cells(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    auto elevation = cells;
    for (auto walkY = 0; walkY < height; ++walkY) {
        for (auto walkX = 0; walkX < width; ++walkX) {
            const auto index = static_cast<std::size_t>(walkY) *
                                   static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(walkX);
            cells[index] = Broodwar->isWalkable(walkX, walkY) ? 1U : 0U;
            elevation[index] = static_cast<std::uint8_t>(
                Broodwar->getGroundHeight(walkX / 4, walkY / 4) / 2);
        }
    }
    return {width, height, 8, std::move(cells), std::move(elevation)};
}

void BwapiBridge::initializeNavigationObstacles(NavigationGrid& navigation) {
    for (const auto unit : Broodwar->getAllUnits())
        updateNavigationObstacle(navigation, unit);
}

void BwapiBridge::updateNavigationObstacle(
    NavigationGrid& navigation, const BWAPI::Unit unit) {
    if (unit == nullptr || !unit->exists()) {
        removeNavigationObstacle(navigation, unit);
        return;
    }
    const auto type = unit->getType();
    const auto blocks = (type.isBuilding() && !unit->isFlying()) ||
        type.isResourceContainer() || type.isMineralField();
    if (!blocks || ((type.isResourceContainer() || type.isMineralField()) &&
                    unit->getResources() <= 0)) {
        navigation.removeDynamicObstacle(liveNavigationObstacleId(unit->getID()));
        return;
    }

    navigation.updateDynamicObstacle(liveNavigationObstacleId(unit->getID()), {
        unit->getLeft(), unit->getTop(), unit->getRight(), unit->getBottom()});

    // Replace the accepted-build reservation with the actual structure as
    // soon as BWAPI publishes it, avoiding a stale duplicate blocker.
    const auto tile = unit->getTilePosition();
    for (const auto& [taskId, pending] : pendingBuilds_) {
        if (toBwapi(pending.kind) != type || pending.target.x / 32 != tile.x ||
            pending.target.y / 32 != tile.y) continue;
        navigation.removeDynamicObstacle(plannedNavigationObstacleId(taskId));
    }
}

void BwapiBridge::removeNavigationObstacle(
    NavigationGrid& navigation, const BWAPI::Unit unit) const {
    if (unit != nullptr)
        navigation.removeDynamicObstacle(liveNavigationObstacleId(unit->getID()));
}

void BwapiBridge::remember(const BWAPI::Unit unit) {
    if (unit == nullptr || !unit->exists() || unit->getPlayer() != Broodwar->enemy() ||
        !unit->isVisible() || !unit->isDetected()) {
        return;
    }
    auto current = snapshotUnit(unit, false);
    if (const auto previous = enemyMemory_.find(unit->getID());
        previous != enemyMemory_.end()) {
        current.inheritObservationHistory(previous->second);
    } else {
        current.firstSeen = current.lastSeen;
    }
    enemyMemory_.insert_or_assign(unit->getID(), std::move(current));
}

void BwapiBridge::forget(const BWAPI::Unit unit) {
    if (unit != nullptr) {
        const auto id = unit->getID();
        selfUnitLifetimes_.erase(id);
        enemyMemory_.erase(id);
        std::erase_if(incomingProjectileMemory_, [id](const auto& entry) {
            return entry.second.source == id || entry.second.target == id;
        });
        unitCommandLocks_.erase(id);
        learnedCommandLeases_.erase(id);
        workerCommandLeases_.erase(id);
        issuedCommandLeaseGenerations_.erase(id);
        frameCommandClaims_.forgetUnit(id);
        std::erase_if(constructionCommandProposals_, [id](const auto& proposal) {
            return proposal.command.actor == id || proposal.command.targetUnit == id;
        });
        for (auto& [taskId, pending] : pendingBuilds_) {
            static_cast<void>(taskId);
            if (pending.builder == id) pending.builder = -1;
        }
    }
}

std::vector<UnitId> BwapiBridge::reservedBuilders() const {
    std::vector<UnitId> result;
    result.reserve(pendingBuilds_.size() + learnedCommandLeases_.size());
    for (const auto& [taskId, pending] : pendingBuilds_) {
        static_cast<void>(taskId);
        if (pending.builder >= 0) result.push_back(pending.builder);
    }
    for (const auto& [id, expiry] : learnedCommandLeases_) {
        static_cast<void>(expiry);
        result.push_back(id);
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool BwapiBridge::pendingBuildAlreadyPaid(const MacroAction& action) const {
    if (action.action != MacroActionKind::build && action.action != MacroActionKind::expand)
        return false;
    if (action.constructionSite.valid()) {
        const auto found = pendingBuilds_.find(buildTaskKey(action.target, action.constructionSite));
        return found != pendingBuilds_.end() && found->second.resourcesPaid;
    }
    return std::ranges::any_of(pendingBuilds_, [&action](const auto& entry) {
        return entry.second.kind == action.target && entry.second.resourcesPaid;
    });
}

std::vector<BuildBlockerFeedback> BwapiBridge::buildBlockerFeedback() const {
    std::vector<BuildBlockerFeedback> result;
    result.reserve(buildBlockers_.size());
    for (const auto& [taskId, feedback] : buildBlockers_) {
        static_cast<void>(taskId);
        result.push_back(feedback);
    }
    std::ranges::sort(result, [](const BuildBlockerFeedback& lhs,
                                 const BuildBlockerFeedback& rhs) {
        if (lhs.target != rhs.target) return lhs.target < rhs.target;
        return lhs.constructionSite.id < rhs.constructionSite.id;
    });
    return result;
}

void BwapiBridge::recordBuildBlocker(const MacroAction& action,
                                     const BuildBlockerReason reason,
                                     const Frame retryFrames) {
    const auto taskId = buildTaskKey(action.target, action.constructionSite);
    buildBlockers_.insert_or_assign(taskId, BuildBlockerFeedback{
        action.target, action.constructionSite, reason,
        Broodwar->getFrameCount() + std::max<Frame>(1, retryFrames)});
}

void BwapiBridge::inspectUnfundedBuild(
    const MacroAction& action,
    const StrategicPlan& plan,
    const InfluenceMap& influence,
    const std::span<const UnitId> unavailableBuilders) {
    if (action.action != MacroActionKind::build && action.action != MacroActionKind::expand)
        return;
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None || !type.isBuilding()) return;
    const auto constructionSite = action.constructionSite.valid()
        ? action.constructionSite : ConstructionTaskSite{};
    const auto taskId = buildTaskKey(action.target, constructionSite);
    if (pendingBuilds_.contains(taskId)) {
        buildBlockers_.erase(taskId);
        return;
    }
    if (const auto feedback = buildBlockers_.find(taskId);
        feedback != buildBlockers_.end() && feedback->second.retryAt > Broodwar->getFrameCount()) {
        return;
    }

    auto home = BWAPI::Position(Broodwar->self()->getStartLocation());
    if (usesHomeConstructionAnchor(action.target)) {
        const auto homeNexus = Broodwar->getClosestUnit(
            home, Filter::IsOwned && Filter::IsCompleted &&
                      Filter::GetType == UnitTypes::Protoss_Nexus);
        if (homeNexus != nullptr) home = homeNexus->getPosition();
    }
    const auto near = toBwapiPosition(constructionSite.valid()
        ? constructionSite.anchor
        : constructionBuilderAnchor(action.target, fromBwapi(home), plan.rallyPoint,
                                     plan.expansionTarget));
    // Feasibility inspection deliberately ignores affordability. It only
    // asks whether an otherwise eligible worker and legal footprint exist,
    // so an unaffordable blocked goal cannot protect the entire current bank.
    auto rejectedForUnsafeRoute = false;
    const auto builder = findBuilder(type, near, influence, unavailableBuilders, false,
                                     &rejectedForUnsafeRoute);
    if (builder == nullptr) {
        recordBuildBlocker(action,
            rejectedForUnsafeRoute && action.target == UnitKind::nexus
                ? BuildBlockerReason::unsafeRoute : BuildBlockerReason::noBuilder,
            15 * 24);
        return;
    }
    const auto location = buildLocation(action.target, type, builder, plan,
                                        constructionSite, taskId, action.reason);
    if (!location.isValid()) {
        if (!lastMacroStatus_.starts_with("placement-search-deferred-"))
            recordBuildBlocker(action, BuildBlockerReason::noPlacement, 30 * 24);
        return;
    }
    if (type.requiresPsi() && !Broodwar->hasPower(location, type)) {
        recordBuildBlocker(action, BuildBlockerReason::noPower, 15 * 24);
        return;
    }
    if (!Broodwar->canBuildHere(location, type, builder, true)) {
        if (action.target == UnitKind::nexus &&
            Broodwar->canBuildHere(location, type, builder, false)) {
            buildBlockers_.erase(taskId);
            return;
        }
        recordBuildBlocker(action, BuildBlockerReason::rejectedFootprint, 15 * 24);
        return;
    }
    if (action.target == UnitKind::nexus) {
        const BWAPI::Position center{
            location.x * 32 + type.tileWidth() * 16,
            location.y * 32 + type.tileHeight() * 16,
        };
        const auto builderFootprint = placementFootprint(builder->getType());
        const auto searchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
        auto routeDestination = fromBwapi(center);
        if (activeNavigation_ != nullptr && !activeNavigation_->empty()) {
            routeDestination = nearestGroundConstructionAccessPoint(
                activeNavigation_, fromBwapi(builder->getPosition()), routeDestination,
                placementFootprint(type), builderFootprint);
            if (!routeDestination.valid()) {
                reportBuildRoute(builder, fromBwapi(center), routeDestination,
                    "footprint-access-unavailable", false, 0.0, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore);
                recordBuildBlocker(action, BuildBlockerReason::unsafeRoute, 15 * 24);
                return;
            }
        }
        const auto route = selectSafeRouteAlternative(
            influence, fromBwapi(builder->getPosition()), routeDestination,
            false, activeNavigation_, builderFootprint, 0.4125, {-1, -1}, 0.25);
        reportBuildRoute(builder, fromBwapi(center), routeDestination,
            "footprint-route", route.profile.reachable, route.profile.peakThreat, influence,
            NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore);
        if (builder->getDistance(center) > 256 &&
            (!route.profile.reachable || route.profile.peakThreat > 0.25)) {
            recordBuildBlocker(action, BuildBlockerReason::unsafeRoute, 15 * 24);
            return;
        }
    }
    buildBlockers_.erase(taskId);
}

bool BwapiBridge::issue(const BWAPI::UnitCommand& command, const std::string_view source,
                        const ResourceUse resourceUse, bool* resourcesPaid) {
    if (resourcesPaid != nullptr) *resourcesPaid = false;
    lastIssueError_ = BWAPI::Errors::None;
    if (productionPermission && !productionPermission(command, source)) {
        lastIssueError_ = BWAPI::Errors::Unit_Busy;
        try { if (actionDiagnostic) {
            const auto actor = command.getUnit();
            const auto target = command.getTarget();
            actionDiagnostic({actor ? actor->getID() : -1, target ? target->getID() : -1,
                fromBwapi(command.getTargetPosition()), command.getType().toString(),
                std::string(source), "production-permission-rejected", command.extra,
                false, false});
        } } catch (...) { ++diagnosticErrors_; }
        return false;
    }
    if (!command.getUnit()) {
        lastIssueError_ = BWAPI::Errors::Unit_Does_Not_Exist;
        return false;
    }
    if (source != "whole-game" &&
        learnedCommandLeases_.contains(command.getUnit()->getID())) {
        lastIssueError_ = BWAPI::Errors::Unit_Busy;
        try { if (actionDiagnostic) {
            const auto target = command.getTarget();
            actionDiagnostic({command.getUnit()->getID(), target ? target->getID() : -1,
                fromBwapi(command.getTargetPosition()), command.getType().toString(),
                std::string(source), "learned-command-lease-active", command.extra,
                false, false});
        } } catch (...) { ++diagnosticErrors_; }
        return false;
    }
    const auto cost = resourceCost(command, Broodwar->self());
    const auto resourceAction = cost.minerals > 0 || cost.gas > 0;
    if (resourceAction && spendingLedger_ != nullptr) {
        const auto affordable = resourceUse == ResourceUse::committed
            ? spendingLedger_->canSpendCommitted(cost.minerals, cost.gas)
            : spendingLedger_->canReserve(cost.minerals, cost.gas);
        if (!affordable) {
            const auto mineralShort = resourceUse == ResourceUse::committed
                ? spendingLedger_->minerals < cost.minerals ||
                    spendingLedger_->committedMinerals < cost.minerals
                : spendingLedger_->freeMinerals() < cost.minerals;
            const auto gasShort = resourceUse == ResourceUse::committed
                ? spendingLedger_->gas < cost.gas || spendingLedger_->committedGas < cost.gas
                : spendingLedger_->freeGas() < cost.gas;
            lastIssueError_ = mineralShort ? BWAPI::Errors::Insufficient_Minerals
                              : gasShort ? BWAPI::Errors::Insufficient_Gas
                                         : BWAPI::Errors::Unit_Busy;
            try { if (actionDiagnostic) {
                const auto target = command.getTarget();
                actionDiagnostic({command.getUnit()->getID(), target ? target->getID() : -1,
                    fromBwapi(command.getTargetPosition()), command.getType().toString(),
                    std::string(source), "resource-ledger-deferred", command.extra, false, false});
            } } catch (...) { ++diagnosticErrors_; }
            return false;
        }
    }
    if (productionDiagnostic && !productionDiagnostic(command, true, false)) return false;
    if (!commandBudget_.consume(dispatchingFrameCommand_)) {
        lastIssueError_ = BWAPI::Errors::Unit_Busy;
        if (productionDiagnostic) productionDiagnostic(command, false, false);
        try { if (actionDiagnostic) {
            const auto target = command.getTarget();
            actionDiagnostic({command.getUnit()->getID(), target ? target->getID() : -1,
                fromBwapi(command.getTargetPosition()), command.getType().toString(),
                std::string(source), "global-command-budget-deferred", command.extra,
                false, false});
        } } catch (...) { ++diagnosticErrors_; }
        return false;
    }
    const auto mineralsBefore = resourceAction ? Broodwar->self()->minerals() : 0;
    const auto gasBefore = resourceAction ? Broodwar->self()->gas() : 0;
    const auto accepted = command.getUnit()->issueCommand(command);
    if (accepted && resourceAction && resourcesPaid != nullptr) {
        const auto mineralsAfter = Broodwar->self()->minerals();
        const auto gasAfter = Broodwar->self()->gas();
        *resourcesPaid = mineralsBefore - mineralsAfter >= cost.minerals &&
                         gasBefore - gasAfter >= cost.gas;
    }
    // Capture immediately: later BWAPI queries can replace the last error.
    lastIssueError_ = Broodwar->getLastError();
    if (productionDiagnostic) productionDiagnostic(command, false, accepted);
    if (accepted && resourceAction && spendingLedger_ != nullptr) {
        const auto charged = resourceUse == ResourceUse::committed
            ? spendingLedger_->spendCommitted(cost.minerals, cost.gas)
            : spendingLedger_->spendAvailable(cost.minerals, cost.gas);
        if (!charged) ++diagnosticErrors_;
    }
    try { if (actionDiagnostic) {
        const auto target = command.getTarget();
        actionDiagnostic({command.getUnit()->getID(), target ? target->getID() : -1,
            fromBwapi(command.getTargetPosition()), command.getType().toString(),
            std::string(source), accepted ? "accepted" : lastIssueError_.toString(),
            command.extra, true, accepted});
    } } catch (...) { ++diagnosticErrors_; }
    return accepted;
}

bool BwapiBridge::requestBuildCancellation(PendingBuild& pending, const BWAPI::Unit builder,
                                           const std::string_view source) {
    if (builder == nullptr || !builder->exists()) return true;
    const auto taskId = pending.taskId;
    const auto onResult = [this, taskId](const bool accepted, const bool) {
        const auto found = pendingBuilds_.find(taskId);
        if (found == pendingBuilds_.end()) return;
        static_cast<void>(found->second.cancellation.request(
            Broodwar->getFrameCount(), Broodwar->getLatencyFrames(), accepted));
    };
    const auto accepted = executeConstructionCommand(
        builder->getID(), CommandType::stop, pending.kind, {-1, -1},
        std::numeric_limits<int>::max(), source, nullptr, onResult);
    return accepted;
}

bool BwapiBridge::executeOwnedCommand(
    const UnitId actor, const CommandOwner owner, const CommandType type,
    const UnitId targetUnit, const Position targetPosition,
    const UnitKind targetKind, const TechnologyKind technology,
    const int urgency, const std::string_view source, bool* resourcesPaid) {
    if (actor < 0) return false;
    const auto frame = Broodwar->getFrameCount();
    const auto latest = issuedCommandLeaseGenerations_[actor][owner];
    Command command{actor, type, targetUnit, targetPosition, targetKind, urgency,
                    frame, std::string(source)};
    command.owner = owner;
    command.urgency = urgency;
    command.deadlineFrame = frame;
    command.leaseGeneration = latest == std::numeric_limits<std::uint64_t>::max()
        ? latest : latest + 1;
    command.technology = technology;
    return execute(command, resourcesPaid);
}

bool BwapiBridge::submitOwnedCommand(
    CommandBus& commands, const UnitId actor, const CommandOwner owner,
    const CommandType type, const UnitId targetUnit, const Position targetPosition,
    const UnitKind targetKind, const TechnologyKind technology, const int urgency,
    const std::string_view source) {
    if (actor < 0) return false;
    const auto frame = Broodwar->getFrameCount();
    const auto latest = issuedCommandLeaseGenerations_[actor][owner];
    Command command{actor, type, targetUnit, targetPosition, targetKind, urgency,
                    frame, std::string(source)};
    command.owner = owner;
    command.urgency = urgency;
    command.deadlineFrame = frame;
    command.leaseGeneration = latest == std::numeric_limits<std::uint64_t>::max()
        ? latest : latest + 1;
    command.technology = technology;
    command.alreadyActive = commandActive(command);
    const auto proposedBefore = commands.stats().proposed;
    commands.submit(std::move(command));
    return commands.stats().proposed > proposedBefore;
}

bool BwapiBridge::executeConstructionCommand(
    const UnitId actor, const CommandType type, const UnitKind targetKind,
    const Position targetPosition, const int urgency, const std::string_view source,
    bool* resourcesPaid,
    std::function<void(bool accepted, bool resourcesPaid)> feedback) {
    if (resourcesPaid != nullptr) *resourcesPaid = false;
    if (actor < 0) {
        if (feedback) feedback(false, false);
        return false;
    }

    if (frameCommandBus_ != nullptr) {
        const auto frame = Broodwar->getFrameCount();
        const auto latest = issuedCommandLeaseGenerations_[actor][CommandOwner::construction];
        Command command{actor, type, -1, targetPosition, targetKind, urgency,
                        frame, std::string(source)};
        command.owner = CommandOwner::construction;
        command.urgency = urgency;
        command.deadlineFrame = frame;
        command.leaseGeneration = latest == std::numeric_limits<std::uint64_t>::max()
            ? latest : latest + 1;
        command.alreadyActive = commandActive(command);
        command.technology = TechnologyKind::none;
        const auto proposedBefore = frameCommandBus_->stats().proposed;
        frameCommandBus_->submit(command);
        if (frameCommandBus_->stats().proposed == proposedBefore) {
            if (feedback) feedback(false, false);
            return false;
        }
        constructionCommandProposals_.push_back({std::move(command), std::move(feedback)});
        constructionCommandQueuedThisAction_ = true;
        return true;
    }

    bool paid = false;
    const auto accepted = executeOwnedCommand(
        actor, CommandOwner::construction, type, -1, targetPosition, targetKind,
        TechnologyKind::none, urgency, source, &paid);
    if (resourcesPaid != nullptr) *resourcesPaid = paid;
    if (feedback) feedback(accepted, paid);
    return accepted;
}

bool BwapiBridge::executeWholeGame(const BWAPI::UnitCommand& command,
                                   const Frame leaseFrames) {
    if (leaseFrames < 1 || !command.getUnit()) return false;
    const auto accepted = issue(command, "whole-game");
    if (accepted)
        learnedCommandLeases_[command.getUnit()->getID()] =
            Broodwar->getFrameCount() + leaseFrames;
    return accepted;
}

bool BwapiBridge::reject(const Command& command, const std::string_view reason) {
    try { if (actionDiagnostic) actionDiagnostic({command.actor, command.targetUnit,
        command.targetPosition, "intent-" + std::to_string(static_cast<int>(command.type)),
        command.source, std::string(reason), static_cast<int>(command.technology), false, false});
    } catch (...) { ++diagnosticErrors_; }
    return false;
}

bool BwapiBridge::commandActive(const Command& command) const {
    const auto actor = Broodwar->getUnit(command.actor);
    if (actor == nullptr || !actor->exists() || actor->getPlayer() != Broodwar->self() ||
        !actor->isCompleted() || actor->isLoaded()) return false;
    const auto last = actor->getLastCommand();
    const auto order = actor->getOrder();
    const auto moving = std::hypot(actor->getVelocityX(), actor->getVelocityY()) > 0.1;
    const auto sameDestination = command.targetPosition.valid() &&
        distanceSquared(command.targetPosition, fromBwapi(last.getTargetPosition())) <= 8 * 8;
    if (command.type == CommandType::recharge) {
        const auto battery = Broodwar->getUnit(command.targetUnit);
        return battery != nullptr && battery->exists() &&
            battery->getPlayer() == Broodwar->self() && battery->isCompleted() &&
            battery->getType() == UnitTypes::Protoss_Shield_Battery &&
            battery->isPowered() && battery->getEnergy() >= 10 &&
            actor->getShields() < actor->getType().maxShields() &&
            last.getType() == UnitCommandTypes::Right_Click_Unit &&
            last.getTarget() == battery && actor->getOrderTarget() == battery &&
            (order == Orders::RechargeShieldsUnit || order == Orders::RechargeShieldsBattery);
    }
    if (command.type == CommandType::move) {
        // A stationary blocked mover remains eligible for a route retry.
        return last.getType() == UnitCommandTypes::Move && sameDestination && moving &&
            (order == Orders::Move || order == Orders::ReaverCarrierMove);
    }
    if (command.type == CommandType::attackMove) {
        return last.getType() == UnitCommandTypes::Attack_Move && sameDestination && !actor->isIdle() &&
            (moving || actor->isAttacking() || actor->isStartingAttack());
    }
    if (command.type == CommandType::attackUnit) {
        const auto target = Broodwar->getUnit(command.targetUnit);
        if (target == nullptr || !target->exists() || !target->isVisible() ||
            last.getType() != UnitCommandTypes::Attack_Unit || last.getTarget() != target ||
            (actor->getOrderTarget() != target && actor->getTarget() != target)) return false;
        // BWAPI normalizes Carrier/Reaver attack states to AttackUnit.
        if (order != Orders::AttackUnit) return false;
        const auto type = actor->getType();
        const auto payload = type == UnitTypes::Protoss_Reaver || type == UnitTypes::Protoss_Carrier;
        const auto weaponType = target->isFlying() ? type.airWeapon() : type.groundWeapon();
        const auto range = payload ? 8 * 32 : actor->getPlayer()->weaponMaxRange(weaponType);
        // A stopped attacker outside weapon range is not an active volley.
        // It may have lost its path and must remain eligible for a retry.
        const auto waitingInRange = !actor->isMoving() && !actor->isStuck() &&
            actor->getDistance(target) <= range;
        // Keep a newly accepted order through ordinary turn/path latency,
        // but let the CommandBus retry a confirmed-stuck unit after its short
        // duplicate-suppression window instead of hiding it for 48 frames.
        const auto transientAttackOrder = !actor->isStuck() &&
            Broodwar->getFrameCount() - actor->getLastCommandFrame() <= 48;
        return waitingInRange || moving || actor->isAttacking() || actor->isStartingAttack() ||
            transientAttackOrder;
    }
    if (command.type == CommandType::load) {
        const auto passenger = Broodwar->getUnit(command.targetUnit);
        if (passenger == nullptr || !passenger->exists()) return false;
        const auto pickupOrder = order == Orders::PickupTransport || order == Orders::EnterTransport;
        const auto matchingPickup = pickupOrder && last.getType() == UnitCommandTypes::Load &&
            last.getTarget() == passenger && actor->getOrderTarget() == passenger;
        return loadCommandStillConfirmed(
            passenger->isLoaded() && passenger->getTransport() == actor,
            matchingPickup, Broodwar->getFrameCount(), actor->getLastCommandFrame(),
            Broodwar->getLatencyFrames());
    }
    return command.type == CommandType::hold && last.getType() == UnitCommandTypes::Hold_Position &&
        order == Orders::HoldPosition;
}

std::vector<Position> BwapiBridge::reservedStormZones(const Frame currentFrame) const {
    std::vector<Position> result;
    result.reserve(recentAreaSpells_.size());
    for (const auto& zone : recentAreaSpells_)
        if (zone.psionicStorm && zone.expires > currentFrame)
            result.push_back(zone.center);
    return result;
}

bool BwapiBridge::psionicStormUsefulNow(const Position center) const {
    if (!center.valid() || Broodwar->self() == nullptr || Broodwar->enemy() == nullptr)
        return false;
    std::vector<UnitSnapshot> allies;
    std::vector<UnitSnapshot> enemies;
    const auto ownUnits = Broodwar->self()->getUnits();
    const auto enemyUnits = Broodwar->enemy()->getUnits();
    allies.reserve(std::min<std::size_t>(ownUnits.size(), 32));
    enemies.reserve(std::min<std::size_t>(enemyUnits.size(), 32));
    for (const auto unit : ownUnits) {
        if (unit == nullptr || !unit->exists()) continue;
        const auto position = fromBwapi(unit->getPosition());
        if (distanceSquared(position, center) >
            psionicStormRadiusPixels * psionicStormRadiusPixels) continue;
        allies.push_back(snapshotUnit(unit, true));
    }
    for (const auto unit : enemyUnits) {
        // Check legal visibility before reading any enemy details.
        if (unit == nullptr || !unit->isVisible() || !unit->isDetected() || !unit->exists())
            continue;
        const auto position = fromBwapi(unit->getPosition());
        if (distanceSquared(position, center) >
            psionicStormRadiusPixels * psionicStormRadiusPixels) continue;
        enemies.push_back(snapshotUnit(unit, false));
    }
    return psionicStormSafe(center, enemies, allies);
}

bool BwapiBridge::execute(const Command& command, bool* resourcesPaid) {
    if (resourcesPaid != nullptr) *resourcesPaid = false;
    const auto frame = Broodwar->getFrameCount();
    if (!frameCommandClaims_.available(frame, command.actor))
        return reject(command, "actor-frame-authority-already-claimed");

    const auto accepted = executeUnchecked(command, resourcesPaid);
    if (accepted) frameCommandClaims_.recordAccepted(frame, command.actor);
    return accepted;
}

bool BwapiBridge::executeUnchecked(const Command& command, bool* resourcesPaid) {
    if (resourcesPaid != nullptr) *resourcesPaid = false;
    const auto frame = Broodwar->getFrameCount();
    if (command.deadlineFrame >= 0 && frame > command.deadlineFrame)
        return reject(command, "command-deadline-expired");
    const auto actor = Broodwar->getUnit(command.actor);
    if (actor == nullptr || !actor->exists() || actor->getPlayer() != Broodwar->self() ||
        !actor->isCompleted() || actor->isLoaded() || actor->isLockedDown() ||
        actor->isMaelstrommed() || actor->isStasised()) {
        return reject(command, "actor-unavailable");
    }
    if (const auto lock = unitCommandLocks_.find(command.actor);
        lock != unitCommandLocks_.end() &&
        lock->second > frame) {
        return reject(command, "spell-command-lock");
    }
    if (command.owner != CommandOwner::unspecified && command.leaseGeneration > 0) {
        auto& latest = issuedCommandLeaseGenerations_[command.actor][command.owner];
        if (command.leaseGeneration < latest)
            return reject(command, "stale-command-lease");
        latest = std::max(latest, command.leaseGeneration);
    }

    switch (command.type) {
        case CommandType::move:
            if (command.source == "splash-spacing" &&
                (!actor->hasPath(toBwapiPosition(command.targetPosition)) ||
                 !Broodwar->isWalkable(command.targetPosition.x / 8, command.targetPosition.y / 8)))
                return reject(command, "spacing-path-blocked");
            return (command.targetPosition.valid() || reject(command, "invalid-position")) &&
                   issue(UnitCommand::move(actor, toBwapiPosition(command.targetPosition)), command.source);
        case CommandType::attackMove:
            return (command.targetPosition.valid() || reject(command, "invalid-position")) &&
                   issue(UnitCommand::attack(actor, toBwapiPosition(command.targetPosition)), command.source);
        case CommandType::attackUnit: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            return ((target != nullptr && target->exists() && target->isVisible()) ||
                    reject(command, "target-unavailable")) &&
                   issue(UnitCommand::attack(actor, target), command.source);
        }
        case CommandType::train:
            return issue(UnitCommand::train(actor, toBwapi(command.targetKind)), command.source);
        case CommandType::trainScarab:
            if (actor->getType() != UnitTypes::Protoss_Reaver ||
                !actor->canTrain(UnitTypes::Protoss_Scarab))
                return reject(command, "scarab-training-unavailable");
            return issue(UnitCommand::train(actor, UnitTypes::Protoss_Scarab), command.source);
        case CommandType::trainInterceptor:
            if (actor->getType() != UnitTypes::Protoss_Carrier ||
                !actor->canTrain(UnitTypes::Protoss_Interceptor))
                return reject(command, "interceptor-training-unavailable");
            return issue(UnitCommand::train(actor, UnitTypes::Protoss_Interceptor), command.source);
        case CommandType::hold: return issue(UnitCommand::holdPosition(actor), command.source);
        case CommandType::stop: return issue(UnitCommand::stop(actor), command.source);
        case CommandType::recharge: {
            const auto battery = Broodwar->getUnit(command.targetUnit);
            return ((battery != nullptr && battery->exists() &&
                   battery->getPlayer() == Broodwar->self() &&
                   battery->getType() == UnitTypes::Protoss_Shield_Battery) ||
                   reject(command, "battery-unavailable")) &&
                   issue(UnitCommand::rightClick(actor, battery), command.source);
        }
        case CommandType::load: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            return (target != nullptr || reject(command, "target-unavailable")) &&
                   issue(UnitCommand::load(actor, target), command.source);
        }
        case CommandType::unload:
            return (command.targetPosition.valid() || reject(command, "invalid-position")) &&
                   issue(UnitCommand::unloadAll(actor, toBwapiPosition(command.targetPosition)), command.source);
        case CommandType::returnCargo:
            return issue(UnitCommand::returnCargo(actor), command.source);
        case CommandType::gather: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            if (target == nullptr || !target->exists()) return reject(command, "gather-target-unavailable");
            const auto type = target->getType();
            const auto mineral = type.isMineralField();
            const auto refinery = target->getPlayer() == Broodwar->self() &&
                target->isCompleted() && type.isRefinery();
            if (!mineral && !refinery) return reject(command, "invalid-gather-target");
            return issue(UnitCommand::gather(actor, target), command.source);
        }
        case CommandType::useTech: {
            const auto tech = toBwapiTech(command.technology);
            if (tech == TechTypes::None || !command.targetPosition.valid())
                return reject(command, "invalid-spell-target");
            const auto currentFrame = Broodwar->getFrameCount();
            if (command.technology == TechnologyKind::psionicStorm &&
                std::ranges::any_of(recentAreaSpells_, [&command, currentFrame](const SpellZone& zone) {
                    return zone.expires > currentFrame &&
                        closeTo(zone.center, command.targetPosition,
                                psionicStormReservationDistance);
                })) {
                return reject(command, "overlapping-storm");
            }
            if (command.technology == TechnologyKind::psionicStorm &&
                !psionicStormUsefulNow(command.targetPosition)) {
                return reject(command, "storm-exposure-changed");
            }
            const auto target = toBwapiPosition(command.targetPosition);
            if (!actor->canUseTech(tech, target)) return reject(command, "cannot-use-tech");
            if (!issue(UnitCommand::useTech(actor, tech, target), command.source)) {
                return false;
            }
            unitCommandLocks_.insert_or_assign(
                command.actor, frame + std::max(12, Broodwar->getLatencyFrames() + 8));
            if (command.technology == TechnologyKind::psionicStorm) {
                // Reserve after BWAPI accepts the cast, through the effect and
                // a conservative command/cast lead window.
                recentAreaSpells_.push_back({command.targetPosition,
                    frame + 96 + std::max(0, Broodwar->getLatencyFrames()), true});
            } else if (command.owner == CommandOwner::maintenance &&
                       command.technology == TechnologyKind::stasisField) {
                recentAreaSpells_.push_back(
                    {command.targetPosition, frame + 100, false});
            }
            return true;
        }
        case CommandType::build: {
            const auto type = toBwapi(command.targetKind);
            if (type == UnitTypes::None || !type.isBuilding())
                return reject(command, "invalid-build-type");
            if (!command.targetPosition.valid())
                return reject(command, "invalid-build-position");
            const BWAPI::TilePosition tile(
                command.targetPosition.x / 32, command.targetPosition.y / 32);
            if (!Broodwar->canBuildHere(tile, type, actor, true))
                return reject(command, "build-footprint-unavailable");
            return issue(UnitCommand::build(actor, tile, type), command.source,
                         ResourceUse::committed, resourcesPaid);
        }
        case CommandType::feedback: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            if (actor->getType() != UnitTypes::Protoss_Dark_Archon ||
                target == nullptr || !target->exists() || !target->isVisible() ||
                !target->getType().isSpellcaster())
                return reject(command, "feedback-target-unavailable");
            if (!actor->canUseTech(TechTypes::Feedback, target))
                return reject(command, "cannot-use-feedback");
            if (!issue(UnitCommand::useTech(actor, TechTypes::Feedback, target), command.source))
                return false;
            unitCommandLocks_.insert_or_assign(
                command.actor, frame + std::max(12, Broodwar->getLatencyFrames() + 8));
            return true;
        }
        case CommandType::mergeArchon: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            if (actor->getType() != UnitTypes::Protoss_High_Templar ||
                target == nullptr || !target->exists() ||
                target->getPlayer() != Broodwar->self() ||
                target->getType() != UnitTypes::Protoss_High_Templar ||
                target->getID() == command.actor)
                return reject(command, "archon-merge-target-unavailable");
            if (!actor->canUseTech(TechTypes::Archon_Warp, target))
                return reject(command, "cannot-merge-archon");
            if (!issue(UnitCommand::useTech(actor, TechTypes::Archon_Warp, target), command.source))
                return false;
            unitCommandLocks_.insert_or_assign(
                command.actor, frame + std::max(12, Broodwar->getLatencyFrames() + 8));
            return true;
        }
    }
    return reject(command, "unsupported-command");
}

ExpansionFeedback BwapiBridge::expansionFeedback(const Position plannedSite) const {
    const auto found = findPendingBuild(pendingBuilds_, UnitKind::nexus);
    if (found != pendingBuilds_.end()) {
        const auto& pending = found->second;
        return {{pending.target.x + 64, pending.target.y + 48}, true,
            pending.lastRouteProgress >= 0
                ? Broodwar->getFrameCount() - pending.lastRouteProgress : 0,
            false, false, false, pending.unsafeRoute};
    }
    const auto frame = Broodwar->getFrameCount();
    const auto actionable = [frame](const auto& entry) {
            const auto& blocker = entry.second;
            return blocker.target == UnitKind::nexus &&
                (blocker.reason == BuildBlockerReason::rejectedFootprint ||
                 blocker.reason == BuildBlockerReason::noBuilder ||
                 blocker.reason == BuildBlockerReason::unsafeRoute ||
                 blocker.reason == BuildBlockerReason::noPlacement) &&
                blocker.retryAt > frame && blocker.constructionSite.valid() &&
                blocker.constructionSite.anchor.valid();
        };
    const auto matching = std::ranges::find_if(buildBlockers_,
        [plannedSite, &actionable](const auto& entry) {
            return actionable(entry) &&
                distanceSquared(entry.second.constructionSite.anchor, plannedSite) <= 96 * 96;
        });
    const auto blocked = matching != buildBlockers_.end()
        ? matching : std::ranges::find_if(buildBlockers_, actionable);
    if (blocked != buildBlockers_.end()) {
        const auto rejectedFootprint =
            blocked->second.reason == BuildBlockerReason::rejectedFootprint;
        const auto noSafeBuilder =
            blocked->second.reason == BuildBlockerReason::noBuilder;
        const auto unsafeRoute =
            blocked->second.reason == BuildBlockerReason::unsafeRoute;
        return {blocked->second.constructionSite.anchor, false, 0,
                rejectedFootprint, noSafeBuilder,
                blocked->second.reason == BuildBlockerReason::noPlacement,
                unsafeRoute};
    }
    return {};
}

bool BwapiBridge::cancelExpansion() {
    const auto found = findPendingBuild(pendingBuilds_, UnitKind::nexus);
    if (found == pendingBuilds_.end()) return true;
    const auto worker = Broodwar->getUnit(found->second.builder);
    if (worker == nullptr || !worker->exists()) {
        if (activeNavigation_ != nullptr)
            activeNavigation_->removeDynamicObstacle(
                plannedNavigationObstacleId(found->first));
        pendingBuilds_.erase(found);
        return true;
    }
    const auto last = worker->getLastCommand();
    const auto nexusType = toBwapi(UnitKind::nexus);
    const auto actionable = worker->getBuildType() == nexusType ||
        (last.getType() == BWAPI::UnitCommandTypes::Build &&
         last.getUnitType() == nexusType &&
         fromBwapi(last.getTargetPosition()) == found->second.target) ||
        (found->second.prepositioned && last.getType() == BWAPI::UnitCommandTypes::Move);
    if (!actionable) {
        if (activeNavigation_ != nullptr)
            activeNavigation_->removeDynamicObstacle(
                plannedNavigationObstacleId(found->first));
        pendingBuilds_.erase(found);
        return true;
    }
    if (found->second.cancellation.awaiting()) {
        if (found->second.cancellation.retryDue(Broodwar->getFrameCount()))
            requestBuildCancellation(found->second, worker, "expansion-cancel-retry");
    } else if (found->second.cancellation.requestDue(Broodwar->getFrameCount())) {
        requestBuildCancellation(found->second, worker, "expansion-cancel");
    }
    return false;
}

int BwapiBridge::executeMacro(
    const std::span<const MacroAction> actions,
    const StrategicPlan& plan,
    const InfluenceMap& influence,
    const std::span<const UnitId> unavailableBuilders,
    const int maximumCommands,
    NavigationGrid* navigation,
    const std::int64_t planningBudgetUs) {
    const auto planningStarted = std::chrono::steady_clock::now();
    activeNavigation_ = navigation;
    auto issued = 0;
    macroExecutions_.clear();
    std::string firstFailure;
    lastMacroStatus_ = actions.empty() ? "idle" : "saving";
    const auto frame = Broodwar->getFrameCount();
    std::erase_if(buildBlockers_, [frame](const auto& entry) {
        return entry.second.retryAt <= frame;
    });
    for (const auto& action : actions) {
        const auto actionStarted = std::chrono::steady_clock::now();
        const auto searchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
        const auto recordExecution = [&](std::string outcome, const bool accepted) {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - actionStarted).count();
            const auto searches = NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore;
            macroExecutions_.push_back({action, std::move(outcome), accepted, elapsedUs, searches});
        };
        constructionCommandQueuedThisAction_ = false;
        if (issued >= maximumCommands) {
            recordExecution("command-budget-deferred", false);
            continue;
        }
        const auto construction = action.action == MacroActionKind::build ||
                                  action.action == MacroActionKind::expand;
        if (construction) {
            // Admit expensive construction planning before it starts, while
            // allowing disjoint training and technology commands to continue.
            // Deferred tasks retain their reservation and retry next pass.
            const auto estimateUs = action.target == UnitKind::pylon ? 8'000 : 20'000;
            const auto spentUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - planningStarted).count();
            if (planningBudgetUs < estimateUs || spentUs > planningBudgetUs - estimateUs) {
                recordExecution("planning-budget-deferred", false);
                continue;
            }
        }
        // A blocking goal may be present solely to protect a future bank
        // (for example, a Forge or Reaver checkpoint whose prerequisite is
        // still under construction).  It is not executable this pass, but it
        // must not terminate the queue: lower-priority actions that already
        // have disjoint reservations still need to reach their producers.
        if (!action.reserved) {
            if (action.blocksLowerPriority && action.executable &&
                (action.action == MacroActionKind::build ||
                 action.action == MacroActionKind::expand)) {
                inspectUnfundedBuild(action, plan, influence, unavailableBuilders);
            }
            auto outcome = std::string("saving-resources");
            if (action.action == MacroActionKind::build ||
                action.action == MacroActionKind::expand) {
                const auto taskId = buildTaskKey(action.target, action.constructionSite);
                if (const auto blocker = buildBlockers_.find(taskId);
                    blocker != buildBlockers_.end() && blocker->second.retryAt > frame) {
                    outcome = "blocked-" + std::string(buildBlockerName(blocker->second.reason)) +
                              "-retry-" + std::to_string(blocker->second.retryAt);
                }
            }
            recordExecution(std::move(outcome), false);
            continue;
        }
        // A non-executable action is a deliberate reservation for a target
        // whose prerequisite is already under construction. Its resources
        // remain protected by the planner, but it must not block valid
        // commands funded from the remaining surplus.
        if (!action.executable) {
            recordExecution("waiting-prerequisite", false);
            continue;
        }
        lastMacroStatus_ = "command-rejected-a" +
                           std::to_string(static_cast<int>(action.action)) + "-" +
                           std::string(unitStats(action.target).name);
        bool success = false;
        std::string buildBlockerOutcome;
        switch (action.action) {
            case MacroActionKind::build:
            case MacroActionKind::expand:
                success = build(action, plan, influence, unavailableBuilders, navigation);
                break;
            case MacroActionKind::train: success = train(action); break;
            case MacroActionKind::research:
            case MacroActionKind::upgrade: success = executeTechnology(action); break;
        }
        const auto constructionProposal = constructionCommandQueuedThisAction_;
        if (action.action == MacroActionKind::build ||
            action.action == MacroActionKind::expand) {
            const auto taskId = buildTaskKey(action.target, action.constructionSite);
            auto reason = BuildBlockerReason::noPlacement;
            auto blocked = false;
            auto retryFrames = 15 * 24;
            if (!success && lastMacroStatus_ == "build-no-builder") {
                reason = BuildBlockerReason::noBuilder;
                blocked = true;
            } else if (!success && action.target == UnitKind::nexus &&
                       lastMacroStatus_.starts_with("build-route-danger")) {
                reason = BuildBlockerReason::unsafeRoute;
                blocked = true;
            } else if (!success && lastMacroStatus_ == "build-cannot-make") {
                reason = BuildBlockerReason::missingPrerequisite;
                blocked = true;
            } else if (!success && lastMacroStatus_ == "build-unpowered-location") {
                reason = BuildBlockerReason::noPower;
                blocked = true;
            } else if (!success && (lastMacroStatus_ == "build-location-rejected" ||
                                    lastMacroStatus_ == "nexus-footprint-blocked")) {
                reason = BuildBlockerReason::rejectedFootprint;
                blocked = true;
            } else if (!success && lastMacroStatus_.starts_with("build-no-location-") &&
                       !lastMacroStatus_.starts_with(
                           "build-no-location-placement-search-deferred")) {
                reason = BuildBlockerReason::noPlacement;
                retryFrames = 30 * 24;
                blocked = true;
            }
            if (blocked) {
                recordBuildBlocker(action, reason, retryFrames);
                buildBlockerOutcome = "blocked-" + std::string(buildBlockerName(reason)) +
                                      "-retry-" + std::to_string(frame + retryFrames);
            } else if (success || lastMacroStatus_.starts_with("build-preposition-")) {
                buildBlockers_.erase(taskId);
            }
        }
        if (success || constructionProposal) {
            ++issued;
            if (!constructionProposal)
                lastMacroStatus_ = "issued-" + std::string(unitStats(action.target).name);
        } else if (firstFailure.empty() || lastMacroStatus_.starts_with("build-")) {
            firstFailure = lastMacroStatus_;
        }
        recordExecution(
            constructionProposal
                ? "command-proposed/" + lastMacroStatus_
                : buildBlockerOutcome.empty() ? lastMacroStatus_ : std::move(buildBlockerOutcome),
            success && !constructionProposal);
        // Every emitted reserved action has a disjoint allocation in the
        // ledger. A rejected placement keeps its allocation, but must not
        // freeze unrelated producers funded from the remaining surplus.
    }
    if (issued == 0 && !firstFailure.empty()) lastMacroStatus_ = firstFailure;
    return issued;
}

std::size_t BwapiBridge::submitWorkerCommands(
    const std::span<const WorkerAssignment> assignments, CommandBus& commands) {
    const auto frame = Broodwar->getFrameCount();
    const auto proposedBefore = commands.stats().proposed;
    const auto submitWorkerCommand = [this, frame, &commands](
        const Unit worker, const WorkerAssignment& assignment, const CommandType type,
        const UnitId targetUnit, const Position targetPosition, const std::string_view source) {
        if (worker == nullptr) return false;
        const auto actor = worker->getID();
        auto lease = workerCommandLeases_.find(actor);
        const auto changedLease = lease == workerCommandLeases_.end() ||
            lease->second.job != assignment.job || lease->second.baseId != assignment.baseId ||
            lease->second.targetUnit != assignment.targetUnit;
        if (changedLease) {
            const auto generation = lease == workerCommandLeases_.end()
                ? std::uint64_t{1}
                : lease->second.generation == std::numeric_limits<std::uint64_t>::max()
                    ? lease->second.generation : lease->second.generation + 1;
            workerCommandLeases_.insert_or_assign(actor, WorkerCommandLease{
                assignment.job, assignment.baseId, assignment.targetUnit, generation});
            lease = workerCommandLeases_.find(actor);
        }
        Command command{actor, type, targetUnit, targetPosition, UnitKind::unknown,
                        assignment.priority, frame, std::string(source)};
        command.owner = CommandOwner::worker;
        command.urgency = assignment.priority;
        command.deadlineFrame = frame;
        command.leaseGeneration = lease->second.generation;
        command.alreadyActive = commandActive(command);
        const auto proposed = commands.stats().proposed;
        commands.submit(std::move(command));
        return commands.stats().proposed > proposed;
    };
    std::vector<MineralWorker> mineralWorkers;
    std::vector<MineralPatchCandidate> patches;
    for (const auto& assignment : assignments) {
        if (assignment.job != WorkerJob::minerals && assignment.job != WorkerJob::transfer) continue;
        const auto worker = Broodwar->getUnit(assignment.worker);
        if (worker == nullptr || !worker->exists() || !worker->isCompleted() ||
            worker->isConstructing()) continue;
        const auto target = worker->getOrderTarget();
        mineralWorkers.push_back({assignment.worker, fromBwapi(worker->getPosition()),
                                  assignment.targetPosition,
                                  target != nullptr && target->getType().isMineralField()
                                      ? target->getID() : -1,
                                  worker->getType().topSpeed(),
                                  worker->isCarryingMinerals() ||
                                      worker->isCarryingGas()});
    }
    for (const auto patch : Broodwar->getMinerals()) {
        if (patch != nullptr && patch->exists() && patch->getResources() > 0) {
            patches.push_back({patch->getID(), fromBwapi(patch->getPosition()), 0});
        }
    }
    const auto& mineralTargets = mineralAllocator_.assign(mineralWorkers, patches);
    std::unordered_map<UnitId, int> escapePatchLoad;
    for (const auto& [workerId, patchId] : mineralTargets) ++escapePatchLoad[patchId];

    for (const auto& assignment : assignments) {
        const auto worker = Broodwar->getUnit(assignment.worker);
        if (worker == nullptr || !worker->exists() || !worker->isCompleted()) {
            continue;
        }
        if (assignment.job == WorkerJob::evacuate) {
            const auto pending = std::ranges::find_if(
                pendingBuilds_, [&assignment](const auto& entry) {
                    return entry.second.builder == assignment.worker;
                });
            if (pending != pendingBuilds_.end() &&
                pending->second.phase != BuildTaskPhase::constructing) {
                // Do not overwrite an en-route Build/Move with a retreat:
                // request Stop through the T021 acknowledgment state machine
                // and keep ownership until BWAPI reports the old order gone.
                // A started Protoss structure is already detached from its
                // Probe; it finishes while the worker takes the safe route.
                auto& build = pending->second;
                if (build.cancellation.awaiting()) {
                    if (build.cancellation.retryDue(frame))
                        requestBuildCancellation(build, worker, "worker-evacuation-retry");
                } else if (build.cancellation.requestDue(frame)) {
                    requestBuildCancellation(build, worker, "worker-evacuation");
                }
                continue;
            }
        }
        // A Probe can still report the just-started Protoss build as its
        // current order after the structure has detached. The lease phase is
        // already `constructing`, so let emergency evacuation issue Move; the
        // warping structure continues without its builder.
        if ((worker->isConstructing() && assignment.job != WorkerJob::evacuate) ||
            assignment.job == WorkerJob::build) continue;
        if (assignment.job == WorkerJob::rebuild) {
            const auto retryFrame = worker->getLastCommandFrame() +
                std::max(8, Broodwar->getLatencyFrames());
            if (assignment.targetPosition.valid() &&
                distanceSquared(fromBwapi(worker->getPosition()),
                                assignment.targetPosition) > 96 * 96) {
                const auto last = worker->getLastCommand();
                if (last.getType() == UnitCommandTypes::Move &&
                    distanceSquared(fromBwapi(last.getTargetPosition()),
                                    assignment.targetPosition) <= 96 * 96) {
                    continue;
                }
                if (retryFrame < frame)
                    static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::move,
                        -1, assignment.targetPosition, "worker-rebuild-stage"));
            } else if (!worker->isIdle() && retryFrame < frame) {
                static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::stop,
                    -1, {-1, -1}, "worker-rebuild-hold"));
            }
            continue;
        }
        if (assignment.job == WorkerJob::evacuate && assignment.targetPosition.valid()) {
            // Mineral walking removes unit collision, which is crucial when a
            // damaged Probe must escape a melee surround. Prefer a patch that
            // increases separation from the unit attacking it; fall back to a
            // normal safe-step move when no useful patch exists.
            const auto threat = Broodwar->getUnit(assignment.targetUnit);
            Unit escapePatch = nullptr;
            auto bestScore = std::numeric_limits<long long>::max();
            if (threat != nullptr && threat->exists()) {
                for (const auto patch : Broodwar->getMinerals()) {
                    if (patch == nullptr || !patch->exists() ||
                        patch->getResources() <= 0 || worker->getDistance(patch) > 640) {
                        continue;
                    }
                    if (threat->getDistance(patch) <= threat->getDistance(worker) + 32) continue;
                    const auto unsafe = std::ranges::any_of(enemyMemory_, [patch](const auto& entry) {
                        const auto& enemy = entry.second;
                        if (!enemy.visible || !enemy.position.valid() ||
                            enemy.groundWeapon.damage <= 0) return false;
                        const auto margin = std::max(96, enemy.groundWeapon.maxRange + 64);
                        return distanceSquared(fromBwapi(patch->getPosition()), enemy.position) <
                               margin * margin;
                    });
                    if (unsafe) continue;
                    const auto score = static_cast<long long>(escapePatchLoad[patch->getID()]) *
                                           1'000'000LL + worker->getDistance(patch) * 64LL -
                                       (worker->getOrderTarget() == patch ? 4096LL : 0LL);
                    if (score < bestScore) {
                        bestScore = score;
                        escapePatch = patch;
                    }
                }
            }
            if (escapePatch != nullptr) ++escapePatchLoad[escapePatch->getID()];
            // Wait for the previous command to arrive before issuing another.
            // The old branch alternated move/gather inside the latency window.
            if (worker->getLastCommandFrame() + std::max(6, Broodwar->getLatencyFrames()) >=
                Broodwar->getFrameCount()) continue;
            if (escapePatch != nullptr) {
                if (worker->getOrderTarget() != escapePatch ||
                    !worker->isGatheringMinerals()) {
                    static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::gather,
                        escapePatch->getID(), {-1, -1}, "worker-mineral-walk"));
                }
                continue;
            }
            if (assignment.priority >= 80) {
                const auto last = worker->getLastCommand();
                if (last.getType() == UnitCommandTypes::Move &&
                    !worker->isIdle() &&
                    distanceSquared(fromBwapi(last.getTargetPosition()),
                                    assignment.targetPosition) <= 96 * 96) {
                    continue;
                }
            }
            static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::move,
                -1, assignment.targetPosition, "worker-evacuate"));
            continue;
        }
        if (assignment.job == WorkerJob::defend) {
            const auto target = Broodwar->getUnit(assignment.targetUnit);
            if (target != nullptr && target->exists() && target->isVisible()) {
                const auto race = Broodwar->enemy()->getRace();
                const auto healthLimit = race == Races::Protoss ? 16 :
                                         (race == Races::Terran ? 12 : 10);
                const auto shouldMineralWalk =
                    worker->getGroundWeaponCooldown() >
                        Broodwar->getRemainingLatencyFrames() + 6 ||
                    worker->getHitPoints() + worker->getShields() <= healthLimit;
                Unit escapePatch = nullptr;
                auto furthestDistance = 0;
                if (shouldMineralWalk) {
                    for (const auto patch : Broodwar->getMinerals()) {
                        if (patch == nullptr || !patch->exists() ||
                            patch->getResources() <= 0) {
                            continue;
                        }
                        const auto workerDistance = worker->getDistance(patch);
                        if (workerDistance < 10 || workerDistance > 480 ||
                            workerDistance < furthestDistance ||
                            target->getDistance(patch) < workerDistance) {
                            continue;
                        }
                        escapePatch = patch;
                        furthestDistance = workerDistance;
                    }
                }
                if (escapePatch != nullptr) {
                    if (worker->getOrderTarget() != escapePatch ||
                        !worker->isGatheringMinerals()) {
                        static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::gather,
                            escapePatch->getID(), {-1, -1}, "worker-mineral-walk"));
                    }
                    continue;
                }
                // Attack orders are persistent. Reissuing the same order every
                // worker tick interrupts the short Probe attack animation and
                // turns a militia surround into harmless chasing.
                if (worker->isIdle() || worker->getOrderTarget() != target) {
                    static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::attackUnit,
                        target->getID(), {-1, -1}, "worker-defend"));
                }
            }
            continue;
        }
        if (worker->isCarryingGas() || worker->isCarryingMinerals()) {
            if (worker->isIdle()) {
                static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::returnCargo,
                    -1, {-1, -1}, "worker-return-cargo"));
            }
            continue;
        }
        const auto currentTarget = worker->getOrderTarget();
        Unit target = nullptr;
        if (assignment.job == WorkerJob::gas) {
            target = Broodwar->getUnit(assignment.targetUnit);
            if (target == nullptr || !target->exists() || target->getPlayer() != Broodwar->self() ||
                !target->isCompleted() || !target->getType().isRefinery()) continue;
        } else if (assignment.job == WorkerJob::minerals ||
                   assignment.job == WorkerJob::transfer) {
            const auto selected = mineralTargets.find(assignment.worker);
            if (selected != mineralTargets.end()) {
                target = Broodwar->getUnit(selected->second);
            }
        }
        const auto atAssignedBase = currentTarget != nullptr &&
                                    closeTo(fromBwapi(currentTarget->getPosition()),
                                            assignment.targetPosition, 384);
        const auto wrongJob = assignment.job == WorkerJob::gas
                                  ? !worker->isGatheringGas() || !atAssignedBase || currentTarget != target
                                  : !worker->isGatheringMinerals() ||
                                        currentTarget != target;
        if (target != nullptr && (worker->isIdle() || wrongJob) &&
            worker->getLastCommandFrame() + 12 < Broodwar->getFrameCount()) {
            static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::gather,
                target->getID(), {-1, -1},
                assignment.job == WorkerJob::gas ? "worker-gas" : "worker-minerals"));
        }
    }
    return commands.stats().proposed - proposedBefore;
}

std::uint64_t BwapiBridge::releaseWorkerCommandLease(const UnitId actor) noexcept {
    const auto found = workerCommandLeases_.find(actor);
    if (found == workerCommandLeases_.end()) return 0;
    auto& lease = found->second;
    const auto active = lease.job != WorkerJob::idle || lease.baseId >= 0 ||
                        lease.targetUnit >= 0;
    if (active) {
        lease.job = WorkerJob::idle;
        lease.baseId = -1;
        lease.targetUnit = -1;
        if (lease.generation < std::numeric_limits<std::uint64_t>::max())
            ++lease.generation;
    }
    return lease.generation;
}

std::vector<ScoutCommandFeedback> BwapiBridge::submitScouts(
    const std::span<const ScoutOrder> orders, CommandBus& commands) {
    std::vector<ScoutCommandFeedback> feedback;
    feedback.reserve(orders.size());
    const auto frame = Broodwar->getFrameCount();
    for (const auto& order : orders) {
        const auto scout = Broodwar->getUnit(order.scout);
        if (scout == nullptr || !scout->exists() || !scout->isCompleted() ||
            !order.target.valid()) {
            feedback.push_back({order.scout, ScoutCommandStatus::rejected,
                                order.leaseGeneration});
            continue;
        }
        const auto commandTarget = order.routeWaypoint.valid()
            ? order.routeWaypoint : order.target;
        const auto urgency = static_cast<int>(std::clamp(order.score * 10.0, 0.0, 1000.0));
        Command scoutCommand{order.scout, CommandType::move, -1, commandTarget,
                             UnitKind::unknown, urgency, frame, "scout-travel"};
        scoutCommand.owner = CommandOwner::scouting;
        scoutCommand.urgency = urgency;
        scoutCommand.deadlineFrame = frame;
        scoutCommand.leaseGeneration = order.leaseGeneration;
        if (commandActive(scoutCommand)) {
            feedback.push_back({order.scout, ScoutCommandStatus::alreadyActive,
                                order.leaseGeneration});
            continue;
        }
        if (scout->getLastCommandFrame() + std::max(8, Broodwar->getLatencyFrames()) >= frame) {
            feedback.push_back({order.scout, ScoutCommandStatus::deferred,
                                order.leaseGeneration});
            continue;
        }
        const auto proposedBefore = commands.stats().proposed;
        commands.submit(std::move(scoutCommand));
        if (commands.stats().proposed == proposedBefore)
            feedback.push_back({order.scout, ScoutCommandStatus::rejected,
                                order.leaseGeneration});
    }
    return feedback;
}

void BwapiBridge::runMaintenance(const StrategicPlan& plan, CommandBus& commands) {
    const auto self = Broodwar->self();
    if (self == nullptr) {
        return;
    }
    const auto frame = Broodwar->getFrameCount();
    std::erase_if(recentAreaSpells_, [frame](const SpellZone& zone) {
        return zone.expires <= frame;
    });

    std::unordered_set<UnitId> spellcastersCommitted;
    const auto reaverCapacity = self->getUpgradeLevel(UpgradeTypes::Reaver_Capacity) > 0
                                    ? 10
                                    : 5;
    const auto carrierCapacity = self->getUpgradeLevel(UpgradeTypes::Carrier_Capacity) > 0
                                     ? 8
                                     : 4;

    for (const auto unit : self->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted()) {
            continue;
        }
        const auto type = unit->getType();
        if (type == UnitTypes::Protoss_Reaver &&
            unit->getScarabCount() < reaverCapacity && unit->getTrainingQueue().empty()) {
            const auto ammo = UnitTypes::Protoss_Scarab;
            // Payload training shares the macro bank. Even the first Scarab
            // cannot cross a held supply, detection, or technology obligation.
            if (unit->canTrain(ammo))
                static_cast<void>(submitOwnedCommand(commands,
                    unit->getID(), CommandOwner::maintenance, CommandType::trainScarab,
                    -1, {-1, -1}, UnitKind::unknown, TechnologyKind::none, 20,
                    "maintenance-scarab"));
        } else if (type == UnitTypes::Protoss_Carrier &&
                   unit->getInterceptorCount() < carrierCapacity &&
                   unit->getTrainingQueue().empty() &&
                   unit->canTrain(UnitTypes::Protoss_Interceptor))
            static_cast<void>(submitOwnedCommand(commands,
                unit->getID(), CommandOwner::maintenance, CommandType::trainInterceptor,
                -1, {-1, -1}, UnitKind::unknown, TechnologyKind::none, 20,
                "maintenance-interceptor"));
    }

    std::unordered_set<UnitId> recallEnergyReserved;
    // Recall belongs to the active attack plan. The Arbiter must already be
    // staged beside its objective, outside visible anti-air and ground threat,
    // with room to receive a useful remote force that can damage that objective.
    if (self->hasResearched(TechTypes::Recall)) {
        auto recallIssued = false;
        for (const auto arbiter : self->getUnits()) {
            if (recallIssued || arbiter == nullptr || !arbiter->exists() ||
                !arbiter->isCompleted() ||
                arbiter->getType() != UnitTypes::Protoss_Arbiter ||
                arbiter->getEnergy() < arbiterRecallEnergyCost ||
                spellcastersCommitted.contains(arbiter->getID())) {
                continue;
            }
            const auto arbiterPosition = fromBwapi(arbiter->getPosition());
            const auto offensiveMission = arbiterHasOffensiveMission(
                plan.posture, plan.attackTarget);
            const auto nearObjective = offensiveMission &&
                closeTo(arbiterPosition, plan.attackTarget, 640);
            auto destinationValue = 0.0;
            const auto objectiveEnemies =
                Broodwar->getUnitsInRadius(arbiter->getPosition(), 640);
            for (const auto enemy : objectiveEnemies) {
                if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                    enemy->getPlayer() != Broodwar->enemy()) continue;
                const auto kind = toKind(enemy->getType());
                destinationValue += enemy->getType().isBuilding()
                                        ? (enemy->getType().isResourceDepot() ? 2.0 : 0.35)
                                        : unitStats(kind).combatValue;
            }
            const auto objectiveActionable = nearObjective && destinationValue >= 2.0;
            const auto stagingSafe = objectiveActionable && arbiterStagingSafe(arbiter);
            if (!objectiveActionable || !stagingSafe) continue;
            const auto landingSlots = availableRecallLandingSlots(arbiter->getPosition());

            Unit bestCenter = nullptr;
            auto bestValue = arbiterRecallMinimumFollowThroughValue;
            for (const auto candidate : self->getUnits()) {
                if (candidate == nullptr || !candidate->exists() ||
                    !candidate->isCompleted() || candidate->isFlying() ||
                    !isCombatUnit(toKind(candidate->getType())) ||
                    candidate->getDistance(arbiter) < 900) {
                    continue;
                }
                auto clusterValue = 0.0;
                auto followThroughValue = 0.0;
                auto clusterUnits = 0;
                for (const auto nearby : Broodwar->getUnitsInRadius(
                         candidate->getPosition(), 128, Filter::IsOwned)) {
                    if (nearby != nullptr && nearby->exists() && nearby->isCompleted() &&
                        !nearby->isFlying() && isCombatUnit(toKind(nearby->getType()))) {
                        clusterValue += unitStats(toKind(nearby->getType())).combatValue;
                        ++clusterUnits;
                        const auto canDamageObjective = std::ranges::any_of(
                            objectiveEnemies, [nearby](const Unit target) {
                                if (target == nullptr || !target->exists() ||
                                    !target->isVisible() ||
                                    target->getPlayer() != Broodwar->enemy()) return false;
                                const auto weapon = target->isFlying()
                                    ? nearby->getType().airWeapon()
                                    : nearby->getType().groundWeapon();
                                return weapon != WeaponTypes::None;
                            });
                        if (canDamageObjective)
                            followThroughValue +=
                                unitStats(toKind(nearby->getType())).combatValue;
                    }
                }
                const ArbiterRecallOpportunity opportunity{
                    arbiter->getEnergy(), clusterUnits, landingSlots,
                    destinationValue, clusterValue, followThroughValue,
                    true, objectiveActionable, stagingSafe};
                if (!arbiterRecallIsWorthwhile(opportunity) ||
                    !arbiter->canUseTech(TechTypes::Recall, candidate->getPosition())) {
                    continue;
                }
                const auto value = followThroughValue + std::min(8.0, clusterValue * 0.2);
                if (value > bestValue ||
                    (std::abs(value - bestValue) < 0.001 &&
                     (bestCenter == nullptr || candidate->getID() < bestCenter->getID()))) {
                    bestValue = value;
                    bestCenter = candidate;
                }
            }
            if (bestCenter != nullptr) {
                recallEnergyReserved.insert(arbiter->getID());
                if (submitOwnedCommand(commands,
                        arbiter->getID(), CommandOwner::maintenance, CommandType::useTech,
                        -1, fromBwapi(bestCenter->getPosition()), UnitKind::unknown,
                        TechnologyKind::recall, 90, "maintenance-recall")) {
                    spellcastersCommitted.insert(arbiter->getID());
                    recallIssued = true;
                }
            }
        }
    }

    if (self->hasResearched(TechTypes::Stasis_Field)) {
        for (const auto arbiter : self->getUnits()) {
            if (arbiter == nullptr || !arbiter->exists() ||
                arbiter->getType() != UnitTypes::Protoss_Arbiter ||
                arbiter->getEnergy() < arbiterStasisEnergyCost || !arbiter->isCompleted() ||
                spellcastersCommitted.contains(arbiter->getID()) ||
                recallEnergyReserved.contains(arbiter->getID())) continue;
            Unit best = nullptr;
            auto bestScore = arbiterStasisMinimumValue;
            for (const auto enemy : Broodwar->enemy()->getUnits()) {
                if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                    enemy->isStasised() || enemy->getType().isBuilding()) continue;
                if (std::ranges::any_of(recentAreaSpells_, [enemy](const SpellZone& zone) {
                        return closeTo(zone.center, fromBwapi(enemy->getPosition()), 112);
                    })) continue;
                const auto score = arbiterStasisValue(
                    assessArbiterStasis(enemy, self, Broodwar->enemy()));
                if (score > bestScore &&
                    arbiter->canUseTech(TechTypes::Stasis_Field, enemy->getPosition())) {
                    bestScore = score;
                    best = enemy;
                }
            }
            if (best != nullptr && submitOwnedCommand(commands,
                    arbiter->getID(), CommandOwner::maintenance, CommandType::useTech,
                    -1, fromBwapi(best->getPosition()), UnitKind::unknown,
                    TechnologyKind::stasisField, 85, "maintenance-stasis")) {
                spellcastersCommitted.insert(arbiter->getID());
                break;
            }
        }
    }

    // Feedback converts enemy caster energy directly into damage and is most
    // valuable before those units can cast. Prefer lethal, high-energy hits.
    for (const auto darkArchon : self->getUnits()) {
        if (darkArchon == nullptr || !darkArchon->isCompleted() ||
            darkArchon->getType() != UnitTypes::Protoss_Dark_Archon ||
            darkArchon->getEnergy() < 50) continue;
        Unit best = nullptr;
        auto bestScore = 49;
        for (const auto enemy : Broodwar->enemy()->getUnits()) {
            if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                !enemy->getType().isSpellcaster() || enemy->getEnergy() <= 0) continue;
            const auto lethalBonus = enemy->getEnergy() >= enemy->getHitPoints() ? 200 : 0;
            const auto score = enemy->getEnergy() + lethalBonus;
            if (score > bestScore &&
                darkArchon->canUseTech(TechTypes::Feedback, enemy)) {
                bestScore = score;
                best = enemy;
            }
        }
        if (best != nullptr && submitOwnedCommand(commands,
                darkArchon->getID(), CommandOwner::maintenance, CommandType::feedback,
                best->getID(), {-1, -1}, UnitKind::unknown, TechnologyKind::none,
                80, "maintenance-feedback")) break;
    }

    // Keep Storm energy only while the current plan has the spell package.
    // Under imminent contact, preserve a larger reserve and never select a
    // Storm-ready caster as a merge candidate.
    const auto stormPlanned = std::ranges::any_of(
        plan.goals, [](const ProductionGoal& goal) {
            return (goal.goal == GoalKind::research &&
                    goal.technology == TechnologyKind::psionicStorm) ||
                   (goal.goal == GoalKind::train &&
                    goal.target == UnitKind::highTemplar);
        });
    const auto stormRelevant = self->hasResearched(TechTypes::Psionic_Storm) ||
                               self->isResearching(TechTypes::Psionic_Storm) ||
                               stormPlanned;
    std::vector<Unit> highTemplarUnits;
    for (const auto unit : self->getUnits()) {
        if (unit != nullptr && unit->exists() && unit->isCompleted() &&
            unit->getType() == UnitTypes::Protoss_High_Templar) {
            highTemplarUnits.push_back(unit);
        }
    }
    std::ranges::sort(highTemplarUnits, {}, [](const Unit unit) { return unit->getID(); });
    const auto fightImminent = plan.prioritizeReinforcements ||
        plan.posture == Posture::defend ||
        std::ranges::any_of(highTemplarUnits, [this](const Unit templar) {
            if (templar->isUnderAttack()) return true;
            return std::ranges::any_of(Broodwar->enemy()->getUnits(), [templar](const Unit enemy) {
                return enemy != nullptr && enemy->exists() && enemy->isVisible() &&
                       enemy->getDistance(templar) <= 384;
            });
        });
    std::vector<int> templarEnergy;
    templarEnergy.reserve(highTemplarUnits.size());
    for (const auto templar : highTemplarUnits) templarEnergy.push_back(templar->getEnergy());
    const auto mergePair = selectTemplarMergePair(
        templarEnergy, templarMergePolicy(stormRelevant, fightImminent),
        [&highTemplarUnits](const std::size_t first, const std::size_t second) {
            return highTemplarUnits[first]->canUseTech(
                TechTypes::Archon_Warp, highTemplarUnits[second]);
        });
    if (mergePair) {
        const auto first = highTemplarUnits[(*mergePair)[0]];
        const auto second = highTemplarUnits[(*mergePair)[1]];
        static_cast<void>(submitOwnedCommand(commands,
            first->getID(), CommandOwner::maintenance, CommandType::mergeArchon,
            second->getID(), {-1, -1}, UnitKind::unknown, TechnologyKind::none,
            60, "maintenance-archon"));
    }
}

void BwapiBridge::drawDebug(
    const GameState& state,
    const StrategicPlan& plan,
    const ThreatAssessment& threat,
    const DebugOverlay& debug) const {
    if (debug.level == 0) return;
    const auto count = [&state](const UnitKind kind) {
        return static_cast<int>(std::ranges::count_if(state.self.units, [kind](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed;
        }));
    };
    const auto army = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind) &&
                   !isWorker(unit.kind) && !isBuilding(unit.kind);
        }));
    const auto matchup = state.enemy.race == Race::zerg ? "PvZ" :
                         state.enemy.race == Race::terran ? "PvT" :
                         state.enemy.race == Race::protoss ? "PvP" : "Pv?";
    const auto nextAction = debug.macro.empty() ? nullptr : &debug.macro.front();
    const auto actionName = nextAction == nullptr ? "" :
        nextAction->technology != TechnologyKind::none
            ? technologyStats(nextAction->technology).name.data()
            : unitStats(nextAction->target).name.data();
    const auto actionState = nextAction == nullptr ? "IDLE" :
        !nextAction->executable ? "WAIT" : nextAction->reserved ? "READY" : "SAVE";
    const auto largestSquad = debug.squads.empty() ? debug.squads.end() :
        std::ranges::max_element(debug.squads, {}, &DebugSquad::units);
    const auto userInput = Broodwar->isFlagEnabled(BWAPI::Flag::UserInput);

    // Keep the everyday view shallow and group the information by decision.
    // Coordinates, reasons, leases and map geometry belong in /debug 2.
    if (debug.level == 1) {
        // Tournament Manager draws its own match text in the upper left after
        // BWAPI's draw callbacks. Keep this panel clear of that text and the
        // game's resource counters along the top edge.
        constexpr int x = 340;
        Broodwar->drawBoxScreen(332, 28, 638, 211, Colors::Black, true);
        Broodwar->drawBoxScreen(332, 28, 638, 211, Colors::Cyan, false);
        Broodwar->drawTextScreen(x, 32, "%cPROTODD%c  %s  %02d:%02d",
            Text::Teal, Text::White, matchup, state.frame / (24 * 60), (state.frame / 24) % 60);
        Broodwar->drawTextScreen(x, 45, "%c%s%c  %.32s", Text::Yellow,
            postureName(plan.posture).data(), Text::White, plan.name.c_str());
        Broodwar->drawLineScreen(337, 59, 633, 59, Colors::Grey);

        Broodwar->drawTextScreen(x, 63, "%cECONOMY", Text::Teal);
        Broodwar->drawTextScreen(x, 76, "%cProbes %d/%d   Bases %d/%d", Text::White,
            count(UnitKind::probe), plan.desiredWorkers,
            count(UnitKind::nexus), plan.desiredBases);
        Broodwar->drawTextScreen(x, 89, "%c%dM  %dG   Supply %d/%d", Text::White,
            state.self.minerals, state.self.gas,
            state.self.supplyUsed / 2, state.self.supplyTotal / 2);
        Broodwar->drawTextScreen(x, 104, "%cARMY", Text::Teal);
        Broodwar->drawTextScreen(x, 117, "%c%d total   D %d  Z %d  R %d", Text::White,
            army, count(UnitKind::dragoon), count(UnitKind::zealot), count(UnitKind::reaver));
        if (largestSquad != debug.squads.end()) {
            const auto decision = largestSquad->decision == FightDecision::engage ? "FIGHT" :
                                  largestSquad->decision == FightDecision::kite ? "KITE" : "BACK";
            if (largestSquad->enemies > 0)
                Broodwar->drawTextScreen(x, 130, "%cFront %.11s  %d:%d  %s", Text::White,
                    largestSquad->role.c_str(), largestSquad->units,
                    largestSquad->enemies, decision);
            else Broodwar->drawTextScreen(x, 130, "%cFront %.14s  %d  clear", Text::White,
                largestSquad->role.c_str(), largestSquad->units);
        } else Broodwar->drawTextScreen(x, 130, "%cNo squad assigned", Text::White);

        Broodwar->drawLineScreen(337, 145, 633, 145, Colors::Grey);
        Broodwar->drawTextScreen(x, 149, "%cEnemy%c %.16s  %.0f%% unsure", Text::Teal, Text::White,
            enemyPlanName(threat.mostLikely).data(), threat.uncertainty * 100.0);
        if (nextAction != nullptr) {
            Broodwar->drawTextScreen(x, 162, "%cNext %s%c %.24s",
                nextAction->executable && nextAction->reserved ? Text::Green : Text::Yellow,
                actionState, Text::White, actionName);
            Broodwar->drawTextScreen(x, 175, "%cCost %dM / %dG", Text::White,
                nextAction->minerals, nextAction->gas);
        } else Broodwar->drawTextScreen(x, 162, "%cNo pending spend", Text::White);

        const auto supplyBlocked = state.self.supplyTotal > 0 &&
            state.self.supplyTotal < 400 && state.self.supplyUsed >= state.self.supplyTotal;
        if (supplyBlocked)
            Broodwar->drawTextScreen(x, 188, "%cSUPPLY BLOCKED", Text::Red);
        else if (debug.unpoweredBuildings > 0)
            Broodwar->drawTextScreen(x, 188, "%c%d unpowered buildings", Text::Red,
                debug.unpoweredBuildings);
        else if (debug.idleGateways > 0)
            Broodwar->drawTextScreen(x, 188, "%cGateways idle %d/%d", Text::Yellow,
                debug.idleGateways, debug.usableGateways);
        else if (debug.idleWorkers > 0)
            Broodwar->drawTextScreen(x, 188, "%c%d idle Probes", Text::Yellow,
                debug.idleWorkers);
        else if (!debug.operation.empty())
            Broodwar->drawTextScreen(x, 188, "%cTask%c %.34s", Text::Teal,
                Text::White, debug.operation.c_str());
        else Broodwar->drawTextScreen(x, 188, "%cNo production alerts", Text::Green);

        Broodwar->drawTextScreen(x, 198, "%c%s", Text::White,
            userInput ? "/debug 2 details   /debug 0 hide" : "Controls disabled by host");
        return;
    }

    const auto expansion = findPendingBuild(pendingBuilds_, UnitKind::nexus);
    constexpr std::size_t macroLimit = 3;
    const auto rows = 11 + static_cast<int>(std::min<std::size_t>(macroLimit, debug.macro.size())) +
        (!debug.scout.empty() ? 1 : 0) +
        (expansion != pendingBuilds_.end() ? 1 : 0) +
        2 * static_cast<int>(std::min<std::size_t>(3, debug.squads.size()));
    Broodwar->drawBoxScreen(4, 4, 556, 12 + rows * 12, Colors::Black, true);
    Broodwar->drawBoxScreen(4, 4, 556, 12 + rows * 12, Colors::Cyan, false);
    auto y = 8;
    const auto row = [&y](const char* format, auto... args) {
        Broodwar->drawTextScreen(10, y, format, args...);
        y += 12;
    };
    const auto weight = [&plan](const UnitKind kind) {
        const auto found = std::ranges::find(plan.composition, kind, &CompositionTarget::kind);
        return found == plan.composition.end() ? 0 : static_cast<int>(found->weight * 100.0 + 0.5);
    };
    row("PROTODD  %s  /debug 1 compact", matchup);
    row("PLAN    %.65s", plan.name.c_str());
    row("STATE   %s | enemy %s | uncertainty %.0f%%", postureName(plan.posture).data(),
        enemyPlanName(threat.mostLikely).data(), threat.uncertainty * 100.0);
    row("ARMY    %d total | D/Z/R %d/%d/%d | mix %d/%d/%d%%", army, count(UnitKind::dragoon),
        count(UnitKind::zealot), count(UnitKind::reaver), weight(UnitKind::dragoon),
        weight(UnitKind::zealot), weight(UnitKind::reaver));
    row("ECO     Probes %d/%d | Bases %d/%d | gas workers goal %d",
        count(UnitKind::probe), plan.desiredWorkers, count(UnitKind::nexus),
        plan.desiredBases, plan.desiredGasWorkers);
    row("BANK    %d minerals | %d gas | supply %d/%d", state.self.minerals,
        state.self.gas, state.self.supplyUsed / 2, state.self.supplyTotal / 2);
    row("MAP     Rally %d,%d | expansion %s", plan.rallyPoint.x, plan.rallyPoint.y,
        plan.expansionTarget.valid() ? "target selected" : plan.sustainEconomy ? "growth enabled" : "waiting");
    row("MACRO   %.61s", lastMacroStatus_.c_str());
    row("MISSION %.59s", debug.operation.c_str());
    row("HEALTH  %.60s", debug.health.c_str());
    if (!debug.scout.empty()) row("SCOUT   %.61s", debug.scout.c_str());
    if (expansion != pendingBuilds_.end()) {
        const auto& pending = expansion->second;
        const auto builder = Broodwar->getUnit(pending.builder);
        const auto remaining = builder != nullptr && builder->exists()
            ? static_cast<int>(distance(fromBwapi(builder->getPosition()),
                                       {pending.target.x + 64, pending.target.y + 48})) : -1;
        row("NEXUS   Probe %d | %d px to site | waiting %ds | no route progress %ds", pending.builder,
            remaining, (state.frame - pending.issued) / 24,
            pending.lastRouteProgress >= 0
                ? (state.frame - pending.lastRouteProgress) / 24 : 0);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(macroLimit, debug.macro.size()); ++i) {
        const auto& action = debug.macro[i];
        const auto label = action.technology != TechnologyKind::none
            ? technologyStats(action.technology).name.data() : unitStats(action.target).name.data();
        row("NEXT    %s %.21s (%dM %dG): %.26s", !action.executable ? "PREREQ" : action.reserved ? "FUNDED" : "SAVING",
            label, action.minerals, action.gas, action.reason.c_str());
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(3, debug.squads.size()); ++i) {
        const auto& squad = debug.squads[i];
        if (squad.enemies > 0)
            row("SQUAD   %.16s %d vs %d | ratio %.2f / need %.2f", squad.role.c_str(),
                squad.units, squad.enemies, squad.ratio, squad.required);
        else row("SQUAD   %.16s %d | no local enemy", squad.role.c_str(), squad.units);
        row("        %.51s -> %d,%d", squad.reason.c_str(), squad.objective.x, squad.objective.y);
    }
    row(userInput ? "/debug 1 compact  |  /debug 0 hide" :
                    "Passive display; game host blocks controls");

    const auto marker = [](const Position point, const Color color, const char* label) {
        if (!point.valid()) return;
        Broodwar->drawCircleMap(point.x, point.y, 24, color);
        Broodwar->drawTextMap(point.x + 26, point.y, "%s", label);
    };
    marker(plan.rallyPoint, Colors::Cyan, "RALLY");
    marker(plan.attackTarget, Colors::Red, "STRATEGIC TARGET");
    marker(plan.expansionTarget, Colors::Green, "EXPANSION COVER");
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.defense.valid()) continue;
        const auto& defense = base.defense;
        Broodwar->drawLineMap(defense.left.x, defense.left.y, defense.right.x, defense.right.y, Colors::Yellow);
        Broodwar->drawCircleMap(defense.anchor.x, defense.anchor.y,
            std::clamp(defense.width, 192, 320), Colors::Cyan);
        marker(defense.anchor, Colors::Cyan, defense.highGround ? "HIGH-GROUND HOLD" : "CHOKE HOLD");
        Broodwar->drawTextMap(defense.entrance.x, defense.entrance.y, "FRONT / no pursuit (%d px)", defense.width);
    }
    for (const auto& squad : debug.squads) {
        if (!squad.center.valid() || !squad.objective.valid()) continue;
        const auto color = squad.decision == FightDecision::retreat ? Colors::Red :
                           squad.decision == FightDecision::kite ? Colors::Yellow : Colors::Green;
        Broodwar->drawLineMap(squad.center.x, squad.center.y, squad.objective.x, squad.objective.y, color);
        Broodwar->drawTextMap(squad.center.x, squad.center.y - 20, "%s: %s", squad.role.c_str(), squad.reason.c_str());
    }
    for (const auto selected : Broodwar->getSelectedUnits()) {
        if (selected->getPlayer() != Broodwar->self()) continue;
        const auto found = debug.orders.find(selected->getID());
        if (found != debug.orders.end())
            Broodwar->drawTextMap(selected->getPosition().x, selected->getPosition().y + 20,
                                 "ORDER: %s", found->second.c_str());
    }
}

Race BwapiBridge::toRace(const BWAPI::Race race) noexcept {
    if (race == Races::Protoss) return Race::protoss;
    if (race == Races::Terran) return Race::terran;
    if (race == Races::Zerg) return Race::zerg;
    if (race == Races::Random) return Race::random;
    return Race::unknown;
}

DamageType BwapiBridge::toDamageType(const BWAPI::DamageType type) noexcept {
    if (type == DamageTypes::Explosive) return DamageType::explosive;
    if (type == DamageTypes::Concussive) return DamageType::concussive;
    if (type == DamageTypes::Ignore_Armor) return DamageType::ignoreArmor;
    return DamageType::normal;
}

UnitSize BwapiBridge::toUnitSize(const BWAPI::UnitSizeType type) noexcept {
    if (type == UnitSizeTypes::Small) return UnitSize::small;
    if (type == UnitSizeTypes::Medium) return UnitSize::medium;
    if (type == UnitSizeTypes::Large) return UnitSize::large;
    return UnitSize::unknown;
}

UnitRole BwapiBridge::roleOf(const BWAPI::UnitType type, const UnitKind kind) noexcept {
    if (type.isWorker()) return UnitRole::worker;
    if (type.isResourceDepot()) return UnitRole::resourceDepot;
    if (type.isDetector()) return UnitRole::detector;
    if (isStaticDefense(kind)) return UnitRole::staticDefense;
    if (type.spaceProvided() > 0) return UnitRole::transport;
    if (kind == UnitKind::highTemplar || kind == UnitKind::darkArchon ||
        kind == UnitKind::arbiter || kind == UnitKind::defiler ||
        kind == UnitKind::scienceVessel || kind == UnitKind::ghost ||
        kind == UnitKind::queen) return UnitRole::spellcaster;
    if (type.isBuilding() && type.canProduce()) return UnitRole::production;
    if (type.isBuilding()) return UnitRole::other;
    if (type.isFlyer()) return UnitRole::airArmy;
    return isCombatUnit(kind) ? UnitRole::groundArmy : UnitRole::other;
}

WeaponSnapshot BwapiBridge::weapon(const BWAPI::WeaponType type) noexcept {
    if (type == WeaponTypes::None || type == WeaponTypes::Unknown) {
        return {};
    }
    auto result = WeaponSnapshot{type.damageAmount(), type.damageCooldown(), type.minRange(), type.maxRange(),
            toDamageType(type.damageType()), type.targetsAir(), type.targetsGround(),
            type.damageFactor()};
    const auto explosion = type.explosionType();
    if (explosion == BWAPI::ExplosionTypes::Radial_Splash ||
        explosion == BWAPI::ExplosionTypes::Enemy_Splash ||
        explosion == BWAPI::ExplosionTypes::Air_Splash ||
        type == WeaponTypes::Scarab) {
        result.splashInner = type.innerSplashRadius();
        result.splashMiddle = type.medianSplashRadius();
        result.splashOuter = type.outerSplashRadius();
        result.splashFriendlyFire = explosion == BWAPI::ExplosionTypes::Radial_Splash;
    }
    return result;
}

UnitSnapshot BwapiBridge::snapshotUnit(const BWAPI::Unit unit, const bool ours) {
    const auto type = unit->getType();
    const auto kind = toKind(type);
    const auto buildTime = std::max(1, type.buildTime());
    auto result = UnitSnapshot{
        unit->getID(), type.getID(), kind, toRace(type.getRace()), roleOf(type, kind),
        fromBwapi(unit->getPosition()), fromBwapi(unit->getPosition()),
        Broodwar->getFrameCount(), unit->getHitPoints(), type.maxHitPoints(),
        unit->getShields(), type.maxShields(), unit->getEnergy(), type.armor(),
        std::clamp((buildTime - unit->getRemainingBuildTime()) * 100 / buildTime, 0, 100),
        std::max(unit->getGroundWeaponCooldown(), unit->getAirWeaponCooldown()),
        type.topSpeed(), weapon(type.groundWeapon()), weapon(type.airWeapon()), ours,
        unit->isCompleted(), unit->isFlying(), unit->isVisible(), unit->isDetected(),
        unit->isBurrowed(), unit->isCloaked(),
        unit->isCarryingGas() || unit->isCarryingMinerals(), unit->isUnderAttack(),
        unit->isHallucination(),
    };
    result.size = toUnitSize(type.size());
    result.footprintWidthTiles = std::max(1, type.tileWidth());
    result.footprintHeightTiles = std::max(1, type.tileHeight());
    result.powered = unit->isPowered();
    result.loaded = unit->isLoaded();
    const auto transport = unit->getTransport();
    result.transportId = transport != nullptr ? transport->getID() : -1;
    result.cargoSpace = type.spaceProvided() > 0 ? unit->getSpaceRemaining() : 0;
    result.sightRange = ours && unit->getPlayer() != nullptr
                            ? unit->getPlayer()->sightRange(type)
                            : type.sightRange();
    result.attackFrame = unit->isAttackFrame();
    if (ours && kind == UnitKind::reaver && unit->isAttacking())
        result.attackWindup = true;
    const auto orderTarget = unit->getOrderTarget();
    result.orderTargetId = orderTarget != nullptr ? orderTarget->getID() : -1;
    const auto order = unit->getOrder();
    result.recharging = ours &&
        (order == BWAPI::Orders::RechargeShieldsUnit ||
         order == BWAPI::Orders::RechargeShieldsBattery);
    result.underStorm = unit->isUnderStorm();
    result.firstSeen = result.lastSeen;
    result.updateMemoryConfidence(result.lastSeen);
    result.dimensionLeft = type.dimensionLeft();
    result.dimensionRight = type.dimensionRight();
    result.dimensionUp = type.dimensionUp();
    result.dimensionDown = type.dimensionDown();
    result.disabled = unit->isLockedDown() || unit->isMaelstrommed() || unit->isStasised();
    result.invincible = unit->isInvincible() || unit->isStasised();
    result.gatheringGas = ours && unit->isGatheringGas();
    result.remainingTrainFrames = ours ? unit->getRemainingTrainTime() : 0;
    if (!ours && !result.completed) {
        // BWAPI's inside-only remaining-build-time field is zero for enemies.
        // Zero here means unavailable, not a construction that is 100% done.
        result.buildProgress = -1;
    }
    if (isBuilding(kind)) {
        const auto elapsed = result.completed ? buildTime :
                             (ours ? buildTime * result.buildProgress / 100 : 0);
        result.constructionStartUpperBound = std::max(0, result.lastSeen - elapsed);
    }
    result.groundWeapon.hits = std::max(result.groundWeapon.hits, type.maxGroundHits());
    result.airWeapon.hits = std::max(result.airWeapon.hits, type.maxAirHits());
    if (kind == UnitKind::reaver) result.ammo = unit->getScarabCount();
    if (kind == UnitKind::carrier) result.ammo = unit->getInterceptorCount();

    // BWAPI exposes the payload weapons on Scarabs/Interceptors rather than
    // their parent unit types. Model them on the controllable parent so combat
    // evaluation, influence, and focus fire do not treat these expensive units
    // as harmless. Bunkers similarly inherit four Marines' Gauss Rifles.
    if (kind == UnitKind::reaver) {
        result.groundWeapon = weapon(WeaponTypes::Scarab);
        // Scarab's BWAPI weapon range is 128; the controllable Reaver launches
        // from eight tiles and fires once per 60 frames.
        result.groundWeapon.maxRange = 8 * 32;
        result.groundWeapon.cooldown = 60;
    } else if (kind == UnitKind::carrier) {
        auto payload = weapon(WeaponTypes::Pulse_Cannon);
        payload.maxRange = 8 * 32;
        payload.hits = std::max(1, result.ammo);
        result.groundWeapon = payload;
        result.airWeapon = payload;
    } else if (kind == UnitKind::bunker) {
        auto garrison = weapon(WeaponTypes::Gauss_Rifle);
        garrison.maxRange += 2 * 32;
        garrison.hits = 4;
        result.groundWeapon = garrison;
        result.airWeapon = garrison;
    }
    // BWAPI 4.4 exposes enemy upgrade levels when a completed unit using the
    // upgrade is visible (PlayerImpl::updateData). Use that legal observation
    // instead of treating every enemy as permanently unupgraded. Fog snapshots
    // retain these observed values; they never query an unseen player's tech.
    if ((ours || (result.visible && result.detected && result.completed)) &&
        unit->getPlayer() != nullptr) {
        const auto owner = unit->getPlayer();
        result.armor = owner->armor(type);
        result.shieldArmor = owner->getUpgradeLevel(UpgradeTypes::Protoss_Plasma_Shields);
        result.topSpeed = owner->topSpeed(type);
        result.sightRange = owner->sightRange(type);
        if (kind == UnitKind::reaver) {
            result.groundWeapon.damage = unit->getPlayer()->damage(WeaponTypes::Scarab);
        } else if (kind == UnitKind::carrier) {
            const auto damage = unit->getPlayer()->damage(WeaponTypes::Pulse_Cannon);
            result.groundWeapon.damage = damage;
            result.airWeapon.damage = damage;
        }
        if (type.groundWeapon() != WeaponTypes::None) {
            result.groundWeapon.damage = owner->damage(type.groundWeapon()) /
                                         std::max(1, type.groundWeapon().damageFactor());
            result.groundWeapon.maxRange = owner->weaponMaxRange(type.groundWeapon());
        }
        if (type.airWeapon() != WeaponTypes::None) {
            result.airWeapon.damage = owner->damage(type.airWeapon()) /
                                      std::max(1, type.airWeapon().damageFactor());
            result.airWeapon.maxRange = owner->weaponMaxRange(type.airWeapon());
        }
    }
    if (ours && result.weaponCooldown == 0) {
        const auto lastAttack = unit->getLastCommand();
        const auto target = lastAttack.getTarget();
        const auto age = Broodwar->getFrameCount() - unit->getLastCommandFrame();
        if (lastAttack.getType() == UnitCommandTypes::Attack_Unit && target != nullptr &&
            target->exists() && target->isVisible() && target->isDetected() &&
            age >= 0 && age <= Broodwar->getLatencyFrames() + 10) {
            const auto& attack = target->isFlying() ? result.airWeapon : result.groundWeapon;
            const auto separation = unit->getDistance(target);
            result.attackWindup = result.attackWindup ||
                (attack.damage > 0 && separation >= attack.minRange &&
                 separation <= attack.maxRange);
        }
    }
    return result;
}

PlayerSnapshot BwapiBridge::snapshotPlayer(const BWAPI::Player player, const bool ours) {
    PlayerSnapshot result;
    if (player == nullptr) {
        return result;
    }
    result.id = player->getID();
    result.race = toRace(player->getRace());
    result.minerals = ours ? player->minerals() : 0;
    result.gas = ours ? player->gas() : 0;
    result.supplyUsed = ours ? player->supplyUsed() : 0;
    result.supplyTotal = ours ? player->supplyTotal() : 0;
    result.gatheredMinerals = ours ? player->gatheredMinerals() : 0;
    result.gatheredGas = ours ? player->gatheredGas() : 0;
    if (ours) {
        result.technologies.reserve(
            static_cast<std::size_t>(TechnologyKind::count) - 1U);
        for (auto value = static_cast<int>(TechnologyKind::none) + 1;
             value < static_cast<int>(TechnologyKind::count); ++value) {
            const auto kind = static_cast<TechnologyKind>(value);
            const auto tech = toBwapiTech(kind);
            const auto upgrade = toBwapiUpgrade(kind);
            if (tech != TechTypes::None) {
                result.technologies.push_back(
                    {kind, player->hasResearched(tech) ? 1 : 0,
                     player->isResearching(tech)});
            } else if (upgrade != UpgradeTypes::None) {
                result.technologies.push_back(
                    {kind, player->getUpgradeLevel(upgrade),
                     player->isUpgrading(upgrade)});
            }
        }
        result.units.reserve(player->getUnits().size());
        for (const auto unit : player->getUnits()) {
            if (unit != nullptr && unit->exists()) {
                result.units.push_back(snapshotUnit(unit, true));
                // BWAPI's training queue is only meaningful on production
                // buildings. Reading it from mobile units can expose stale
                // order bytes (observed as a permanently queued Pylon on an
                // idle Probe), which suppresses the real construction goal.
                if (unit->getType().isBuilding()) {
                    const auto trainingQueue = unit->getTrainingQueue();
                    const auto buildUnit = unit->getBuildUnit();
                    const auto lastCommand = unit->getLastCommand();
                    // BWAPI updates isTraining()/the queue asynchronously. A
                    // just-issued train command can therefore look idle for
                    // one latency window. Treat it as occupied immediately so
                    // the planner neither double-orders nor leaves a producer
                    // reservation out of the next snapshot.
                    const auto recentTrainingCommand =
                        lastCommand.getType() == BWAPI::UnitCommandTypes::Train &&
                        unit->getLastCommandFrame() +
                                std::max(1, Broodwar->getLatencyFrames()) >=
                            Broodwar->getFrameCount();
                    const auto activeTraining = unit->isTraining() ||
                                                unit->getRemainingTrainTime() > 0 ||
                                                recentTrainingCommand ||
                                                (buildUnit != nullptr &&
                                                 buildUnit->exists() &&
                                                 !buildUnit->isCompleted());
                    ProducerSlotSnapshot producerSlot{
                        unit->getID(), toKind(unit->getType()), activeTraining,
                        static_cast<int>(trainingQueue.size()),
                        unit->getRemainingTrainTime(),
                        Broodwar->getRemainingLatencyFrames(), recentTrainingCommand,
                        unit->isResearching(), unit->isUpgrading(),
                    };
                    auto skippedActiveQueueEntry = false;
                    for (const auto queuedType : trainingQueue) {
                        const auto queuedKind = toKind(queuedType);
                        if (queuedKind != UnitKind::unknown) {
                            // The exposed in-progress Protoss unit is already
                            // present in player->getUnits(). Count the producer
                            // as busy and retain only entries waiting behind it.
                            if (activeTraining && !skippedActiveQueueEntry) {
                                skippedActiveQueueEntry = true;
                            } else {
                                result.queuedUnits.push_back(queuedKind);
                                producerSlot.queuedUnits.push_back(queuedKind);
                            }
                        }
                    }
                    result.producerSlots.push_back(std::move(producerSlot));
                    // In live BWAPI 4.4 games a Protoss producer can report an
                    // active, non-empty transitional queue whose UnitType is
                    // not yet usable by the adapter. isTraining() can also
                    // trail the underlying build timer. In either case the
                    // producer is occupied and must not reserve another unit
                    // ahead of throughput structures or workers.
                    if (activeTraining && !trainingSlotAvailable(
                            activeTraining, static_cast<int>(trainingQueue.size()),
                            unit->getRemainingTrainTime(),
                            Broodwar->getRemainingLatencyFrames(), recentTrainingCommand)) {
                        const auto producer = toKind(unit->getType());
                        if (producer != UnitKind::unknown) {
                            result.busyProducers.push_back(producer);
                        }
                    }
                }
            }
        }
        std::ranges::sort(result.units, {}, &UnitSnapshot::id);
    }
    return result;
}

std::vector<BaseSnapshot> BwapiBridge::snapshotBases(const GameState& state) {
    std::vector<BaseSnapshot> bases;
    bases.reserve(resourceSites_.size());
    if (!enemyMainRouteSource_.valid()) {
        for (const auto& site : resourceSites_) {
            const auto isStartingSite = std::ranges::any_of(
                Broodwar->getStartLocations(), [center = site.depotCenter](
                    const TilePosition startTile) {
                    return closeTo(center, fromBwapi(BWAPI::Position(startTile)), 256);
                });
            if (!isStartingSite) continue;
            const auto observedEnemyDepot = std::ranges::any_of(
                state.enemy.units, [&site](const UnitSnapshot& unit) {
                    return unit.role == UnitRole::resourceDepot && !unit.flying &&
                           unit.position.valid() &&
                           distanceSquared(unit.position, site.depotCenter) <= 320 * 320;
                });
            if (observedEnemyDepot) {
                enemyMainRouteSource_ = site.depotCenter;
                break;
            }
        }
    }
    if (state.frame >= 24 && enemyMainRouteSource_.valid() && !enemyRoutesInitialized_) {
        const auto terrain = navigationGrid();
        for (auto& site : resourceSites_) {
            const auto route = terrain.findPath(
                enemyMainRouteSource_, site.depotCenter, 100'000);
            site.enemyGroundReachabilityKnown = route.status == NavigationStatus::reached ||
                route.status == NavigationStatus::unreachable;
            if (!route.reached()) continue;
            double length = 0.0;
            for (std::size_t index = 1; index < route.points.size(); ++index) {
                length += distance(route.points[index - 1], route.points[index]);
            }
            site.enemyGroundDistanceFromMain = static_cast<int>(std::lround(length));
        }
        enemyRoutesInitialized_ = true;
    }
    auto id = 0;
    for (const auto& site : resourceSites_) {
        // Resource clustering can identify a mineral group for which BWAPI
        // cannot find any legal depot footprint (split or edge clusters are
        // common examples). Such a point is useful for mining-lane avoidance,
        // but it is not an expansion. Advertising it to strategy made the
        // nearest "natural" impossible to build and let placement fall onward
        // to an unrelated fourth-base site.
        if (!site.depotTile.isValid()) continue;
        ++id;
        const auto center = site.depotCenter;
        auto minerals = 0;
        auto gas = 0;
        auto mineralPatches = 0;
        auto geysers = 0;
        for (const auto patch : Broodwar->getMinerals()) {
            if (closeTo(site.resourceCenter, fromBwapi(patch->getInitialPosition()), 352)) {
                minerals += patch->getResources();
                ++mineralPatches;
            }
        }
        // Refinery construction removes the neutral unit from getGeysers().
        // The site's gas geometry is permanent: otherwise our own Assimilator
        // makes the natural look mineral-only and redirects its pending Nexus
        // (and the covering army) to a distant, supposedly gas-bearing base.
        // Static resources are initial map information; getResources retains
        // BWAPI's legally observed amount, including when the unit is hidden.
        for (const auto geyser : Broodwar->getStaticGeysers()) {
            if (closeTo(site.resourceCenter, fromBwapi(geyser->getInitialPosition()), 352)) {
                gas += geyser->getResources();
                ++geysers;
            }
        }

        auto owner = -1;
        for (const auto& depot : state.self.units) {
            if (depot.role == UnitRole::resourceDepot && depot.completed &&
                closeTo(center, depot.position, 320)) {
                owner = state.self.id;
            }
        }
        for (const auto& depot : state.enemy.units) {
            if (depot.role == UnitRole::resourceDepot && closeTo(center, depot.position, 320)) {
                owner = state.enemy.id;
            }
        }
        const TilePosition tile(center.x / 32, center.y / 32);
        if (tile.isValid() && Broodwar->isVisible(tile)) {
            baseLastScouted_[id] = state.frame;
        }
        auto footprintVisible = true;
        for (auto x = -2; x < 2; ++x) {
            for (auto y = -1; y < 2; ++y) {
                const TilePosition footprintTile(tile.x + x, tile.y + y);
                footprintVisible = footprintVisible && footprintTile.isValid() &&
                                   Broodwar->isVisible(footprintTile);
            }
        }
        if (owner == -1 && footprintVisible) baseLastConfirmedEmpty_[id] = state.frame;
        if (owner != -1) baseLastConfirmedEmpty_.erase(id);
        const auto start = std::ranges::any_of(
            Broodwar->getStartLocations(),
            [center](const TilePosition startTile) {
                return closeTo(center, fromBwapi(BWAPI::Position(startTile)), 256);
            });
        const auto startPosition = BWAPI::Position(Broodwar->self()->getStartLocation());
        const auto island = !Broodwar->hasPath(startPosition, toBwapiPosition(center));
        bases.push_back({id, center, site.mineralLine, minerals, gas, owner,
                         baseLastScouted_[id],
                         start, island, mineralPatches, geysers,
                         baseLastConfirmedEmpty_.contains(id) ? baseLastConfirmedEmpty_.at(id) : -1});
        bases.back().groundDistanceFromMain = site.groundDistanceFromStart;
        bases.back().enemyGroundDistanceFromMain = site.enemyGroundDistanceFromMain;
        bases.back().enemyGroundReachabilityKnown = site.enemyGroundReachabilityKnown;
        bases.back().depotFootprintAvailable = site.depotTile.isValid();
        const UnitSnapshot* approach = nullptr;
        auto closest = 1600 * 1600;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.visible || enemy.flying || !isCombatUnit(enemy.kind) ||
                enemy.groundWeapon.damage <= 0 || !enemy.position.valid()) continue;
            const auto separation = distanceSquared(center, enemy.position);
            if (separation < closest) { closest = separation; approach = &enemy; }
        }
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto& defense : site.defenses) {
            const auto score = (approach ? distance(defense.entrance, approach->position) : 0.0) +
                defense.width * (approach ? 0.25 : 1.5) +
                distance(center, defense.anchor) * 0.25 - (defense.highGround ? 180.0 : 0.0);
            if (score < bestScore) { bestScore = score; bases.back().defense = defense; }
        }
    }
    return bases;
}

void BwapiBridge::discoverResourceClusters() {
    resourceSites_.clear();
    struct ResourceEntry {
        Position position{-1, -1};
        bool mineral{};
    };
    std::vector<ResourceEntry> resources;
    for (const auto mineral : Broodwar->getStaticMinerals()) {
        resources.push_back({fromBwapi(mineral->getInitialPosition()), true});
    }
    for (const auto geyser : Broodwar->getStaticGeysers()) {
        resources.push_back({fromBwapi(geyser->getInitialPosition()), false});
    }
    std::ranges::sort(resources, [](const ResourceEntry& left, const ResourceEntry& right) {
        if (left.position.x != right.position.x) return left.position.x < right.position.x;
        return left.position.y < right.position.y;
    });
    std::vector<bool> claimed(resources.size(), false);
    for (std::size_t seed = 0; seed < resources.size(); ++seed) {
        if (claimed[seed]) continue;
        claimed[seed] = true;
        std::vector<std::size_t> members{seed};
        for (std::size_t cursor = 0; cursor < members.size(); ++cursor) {
            for (std::size_t candidate = 0; candidate < resources.size(); ++candidate) {
                if (!claimed[candidate] &&
                    closeTo(resources[members[cursor]].position,
                            resources[candidate].position, 320)) {
                    claimed[candidate] = true;
                    members.push_back(candidate);
                }
            }
        }
        long long sumX = 0;
        long long sumY = 0;
        long long mineralX = 0;
        long long mineralY = 0;
        auto mineralCount = 0;
        for (const auto member : members) {
            const auto& resource = resources[member];
            sumX += resource.position.x;
            sumY += resource.position.y;
            if (resource.mineral) {
                mineralX += resource.position.x;
                mineralY += resource.position.y;
                ++mineralCount;
            }
        }
        if (members.size() < 4U) continue;
        const auto divisor = static_cast<long long>(members.size());
        const Position resourceCenter{static_cast<int>(sumX / divisor),
                                      static_cast<int>(sumY / divisor)};
        const Position mineralLine = mineralCount > 0
                                         ? Position{static_cast<int>(mineralX / mineralCount),
                                                    static_cast<int>(mineralY / mineralCount)}
                                         : resourceCenter;
        auto depotTile = TilePositions::None;
        for (const auto start : Broodwar->getStartLocations()) {
            if (closeTo(resourceCenter, fromBwapi(BWAPI::Position(start)), 384)) {
                depotTile = start;
                break;
            }
        }
        if (!depotTile.isValid()) {
            depotTile = Broodwar->getBuildLocation(
                UnitTypes::Protoss_Nexus,
                TilePosition(resourceCenter.x / 32, resourceCenter.y / 32), 12);
        }
        if (!depotTile.isValid()) {
            // getBuildLocation can reject every neutral base while its tiles
            // are still in fog during onStart. Search the static map footprint
            // with checkExplored=false so a real natural is not discarded in
            // favor of the next known start location.
            const TilePosition origin(resourceCenter.x / 32, resourceCenter.y / 32);
            auto bestDistance = std::numeric_limits<int>::max();
            for (auto y = origin.y - 20; y <= origin.y + 20; ++y) {
                for (auto x = origin.x - 20; x <= origin.x + 20; ++x) {
                    const TilePosition candidate(x, y);
                    if (!candidate.isValid() ||
                        x + UnitTypes::Protoss_Nexus.tileWidth() > Broodwar->mapWidth() ||
                        y + UnitTypes::Protoss_Nexus.tileHeight() > Broodwar->mapHeight() ||
                        !Broodwar->canBuildHere(candidate, UnitTypes::Protoss_Nexus,
                                               nullptr, false)) {
                        continue;
                    }
                    const Position center{x * 32 + 64, y * 32 + 48};
                    const auto separation = distanceSquared(center, resourceCenter);
                    if (separation < bestDistance) {
                        bestDistance = separation;
                        depotTile = candidate;
                    }
                }
            }
        }
        const auto depotCenter = depotTile.isValid()
                                     ? Position{depotTile.x * 32 + 64,
                                                depotTile.y * 32 + 48}
                                     : resourceCenter;
        resourceSites_.push_back({resourceCenter, depotCenter, mineralLine, depotTile});
    }
    for (const auto start : Broodwar->getStartLocations()) {
        const Position center{start.x * 32 + 64, start.y * 32 + 48};
        if (std::ranges::none_of(resourceSites_, [center](const ResourceSite& site) {
                return closeTo(center, site.depotCenter, 320);
            })) {
            resourceSites_.push_back({center, center, center, start});
        }
    }
}

void BwapiBridge::reportBuildRoute(
    const BWAPI::Unit builder, const Position anchor, const Position destination,
    const std::string_view stage, const bool reachable, const double peakThreat,
    const InfluenceMap& influence, const std::uint64_t searches) noexcept {
    if (!buildRouteDiagnostic || builder == nullptr) return;
    try {
        const auto from = fromBwapi(builder->getPosition());
        const auto footprint = placementFootprint(builder->getType());
        buildRouteDiagnostic({Broodwar->getFrameCount(), builder->getID(), from, anchor,
            destination, std::string(stage), reachable, peakThreat,
            influence.at(anchor).groundThreat,
            activeNavigation_ == nullptr || activeNavigation_->walkable(from, footprint),
            destination.valid() && (activeNavigation_ == nullptr ||
                activeNavigation_->walkable(destination, footprint)),
            destination.valid() && builder->hasPath(toBwapiPosition(destination)), searches});
    } catch (...) { ++diagnosticErrors_; }
}

BWAPI::Unit BwapiBridge::findBuilder(
    const BWAPI::UnitType type,
    const BWAPI::Position near,
    const InfluenceMap& influence,
    const std::span<const UnitId> unavailableBuilders,
    const bool requireCanBuild,
    bool* rejectedForUnsafeRoute) {
    if (rejectedForUnsafeRoute != nullptr) *rejectedForUnsafeRoute = false;
    std::vector<std::pair<long long, BWAPI::Unit>> candidates;
    auto hadUnsafeRouteCandidate = false;
    const auto builderType = type.whatBuilds().first;
    const auto destination = fromBwapi(near);
    for (const auto unit : Broodwar->self()->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted() ||
            unit->getType() != builderType || unit->isConstructing() || unit->isTraining() ||
            !unit->isInterruptible() || (requireCanBuild && !unit->canBuild(type))) {
            continue;
        }
        if (std::ranges::find(unavailableBuilders, unit->getID()) !=
            unavailableBuilders.end() || learnedCommandLeases_.contains(unit->getID())) {
            continue;
        }
        // Pending Protoss construction is a real lease even while the Probe
        // is merely walking to the tile. Reusing it for a second structure
        // cancels the first order and can create an indefinite supply block.
        if (std::ranges::any_of(pendingBuilds_, [unit](const auto& entry) {
                return entry.second.builder == unit->getID();
            })) {
            continue;
        }
        // A construction proposal is not committed to pendingBuilds_ until
        // command dispatch acknowledges it. Still reserve its Probe for this
        // macro pass so another site-scoped task cannot queue a conflicting
        // same-frame command for the same actor.
        if (std::ranges::any_of(constructionCommandProposals_, [unit](const auto& proposal) {
                return proposal.command.actor == unit->getID();
            })) {
            continue;
        }
        auto exposed = false;
        if (const auto enemy = Broodwar->enemy()) {
            exposed = std::ranges::any_of(enemy->getUnits(), [unit](const Unit threat) {
                if (threat == nullptr || !threat->exists() || !threat->isVisible() ||
                    !threat->isCompleted()) {
                    return false;
                }
                const auto weapon = threat->getType().groundWeapon();
                return weapon != WeaponTypes::None &&
                       threat->getDistance(unit) <= weapon.maxRange() + 96;
            });
        }
        const auto missingDurability =
            unit->getType().maxHitPoints() + unit->getType().maxShields() -
            unit->getHitPoints() - unit->getShields();
        const auto score = static_cast<long long>(unit->getDistance(near)) +
                           static_cast<long long>(std::max(0, missingDurability)) * 16LL +
                           (exposed ? 1'000'000LL : 0LL);
        candidates.emplace_back(score, unit);
    }
    std::ranges::sort(candidates, [](const auto& first, const auto& second) {
        return first.first != second.first ? first.first < second.first
                                          : first.second->getID() < second.second->getID();
    });
    for (const auto& [score, unit] : candidates) {
        static_cast<void>(score);
        const auto builderPosition = fromBwapi(unit->getPosition());
        if (unit->getDistance(near) > 640) {
            const auto traceCandidate = type == UnitTypes::Protoss_Nexus &&
                unit == candidates.front().second;
            const auto searchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
            const MovementFootprint footprint{
                unit->getType().dimensionLeft(), unit->getType().dimensionRight(),
                unit->getType().dimensionUp(), unit->getType().dimensionDown()};
            const auto screeningDestination = groundConstructionScreeningDestination(
                influence, activeNavigation_, builderPosition, destination, placementFootprint(type), footprint);
            if (!screeningDestination.valid()) {
                if (traceCandidate) reportBuildRoute(unit, destination, screeningDestination,
                    influence.at(destination).groundThreat > 0.25F ? "anchor-threat" : "access-unavailable",
                    false, 0.0, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore);
                hadUnsafeRouteCandidate = true;
                continue;
            }
            const auto terrainMayChangeRoute = activeNavigation_ != nullptr &&
                !activeNavigation_->empty() &&
                !activeNavigation_->lineWalkable(builderPosition, screeningDestination, footprint);
            const auto straightPathPeakThreat =
                influence.maximumGroundThreat(builderPosition, screeningDestination);
            const auto straightPathThreatened = straightPathPeakThreat > 0.25F;
            if (terrainMayChangeRoute || straightPathThreatened) {
                // A threatening straight chord does not prove the terrain route
                // is unsafe. Let the same bounded route selector used by build
                // staging search for a reachable, lower-risk path before
                // discarding this Probe.
                const auto route = selectSafeRouteAlternative(
                    influence, builderPosition, screeningDestination, false, activeNavigation_,
                    footprint, 0.4125, {-1, -1}, 0.25);
                if (traceCandidate) reportBuildRoute(unit, destination, screeningDestination,
                    "builder-screen", route.profile.reachable, route.profile.peakThreat, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore);
                if (!route.profile.reachable || route.profile.peakThreat > 0.25) {
                    hadUnsafeRouteCandidate = true;
                    continue;
                }
            } else if (traceCandidate) {
                reportBuildRoute(unit, destination, screeningDestination, "clear-chord",
                    true, straightPathPeakThreat, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore);
            }
        }
        return unit;
    }
    if (rejectedForUnsafeRoute != nullptr)
        *rejectedForUnsafeRoute = hadUnsafeRouteCandidate;
    return nullptr;
}

BWAPI::TilePosition BwapiBridge::buildLocation(
    const UnitKind kind,
    const BWAPI::UnitType type,
    const BWAPI::Unit builder,
    const StrategicPlan& plan,
    const ConstructionTaskSite& constructionSite,
    const std::uint64_t taskKey,
    const std::string_view reason) {
    lastMacroStatus_ = "placement-search";
    if (kind == UnitKind::assimilator) {
        Unit bestGeyser = nullptr;
        auto bestScore = std::numeric_limits<int>::max();
        for (const auto geyser : Broodwar->getGeysers()) {
            if (geyser == nullptr || !geyser->exists() || geyser->getResources() <= 0) continue;
            const auto nexus = Broodwar->getClosestUnit(
                geyser->getPosition(),
                Filter::IsOwned && Filter::GetType == UnitTypes::Protoss_Nexus);
            if (nexus == nullptr || nexus->getDistance(geyser) > 448 ||
                !builder->hasPath(geyser->getPosition())) {
                continue;
            }
            // Taken geysers disappear from getGeysers(), so the remaining
            // candidate nearest an owned base naturally advances from main gas
            // to natural and third without repeatedly selecting an occupied tile.
            const auto score = builder->getDistance(geyser) + nexus->getDistance(geyser) * 2;
            if (score < bestScore || (score == bestScore &&
                                      (bestGeyser == nullptr ||
                                       geyser->getID() < bestGeyser->getID()))) {
                bestScore = score;
                bestGeyser = geyser;
            }
        }
        return bestGeyser != nullptr ? bestGeyser->getTilePosition()
                                     : TilePositions::None;
    }

    std::vector<PlacementAccessRequirement> placementAccessRequirements;
    if (activeNavigation_ != nullptr && !activeNavigation_->empty()) {
        const auto probeFootprint = placementFootprint(UnitTypes::Protoss_Probe);
        for (const auto nexus : Broodwar->self()->getUnits()) {
            if (nexus == nullptr || !nexus->exists() ||
                nexus->getType() != UnitTypes::Protoss_Nexus) continue;
            const auto nexusPosition = fromBwapi(nexus->getPosition());
            const auto site = std::ranges::find_if(resourceSites_, [&nexusPosition](
                const ResourceSite& candidate) {
                    return closeTo(candidate.depotCenter, nexusPosition, 320);
                });
            if (site == resourceSites_.end()) continue;
            const auto sources = accessPointsAroundFootprint(
                *activeNavigation_, nexusPosition, UnitTypes::Protoss_Nexus,
                probeFootprint);
            for (const auto mineral : Broodwar->getMinerals()) {
                if (mineral == nullptr || !mineral->exists() || mineral->getResources() <= 0 ||
                    !closeTo(site->resourceCenter,
                             fromBwapi(mineral->getInitialPosition()), 352)) continue;
                const auto targets = accessPointsAroundPosition(
                    *activeNavigation_, fromBwapi(mineral->getPosition()), probeFootprint);
                addPlacementAccessRequirement(placementAccessRequirements,
                    std::span<const Position>(sources), std::span<const Position>(targets),
                    probeFootprint);
            }
        }

        for (const auto producer : Broodwar->self()->getUnits()) {
            if (producer == nullptr || !producer->exists() || !producer->isCompleted()) continue;
            const auto producerType = producer->getType();
            UnitType outputType = UnitTypes::None;
            if (producerType == UnitTypes::Protoss_Gateway)
                outputType = UnitTypes::Protoss_Dragoon;
            else if (producerType == UnitTypes::Protoss_Robotics_Facility)
                outputType = UnitTypes::Protoss_Reaver;
            if (outputType == UnitTypes::None) continue;
            const auto nexus = Broodwar->getClosestUnit(
                producer->getPosition(), Filter::IsOwned && Filter::IsCompleted &&
                    Filter::GetType == UnitTypes::Protoss_Nexus);
            if (nexus == nullptr) continue;
            const auto outputFootprint = placementFootprint(outputType);
            const auto sources = accessPointsAroundFootprint(
                *activeNavigation_, fromBwapi(producer->getPosition()), producerType,
                outputFootprint);
            const auto targets = accessPointsAroundFootprint(
                *activeNavigation_, fromBwapi(nexus->getPosition()),
                UnitTypes::Protoss_Nexus, outputFootprint);
            addPlacementAccessRequirement(placementAccessRequirements,
                std::span<const Position>(sources), std::span<const Position>(targets),
                outputFootprint);
        }
        capturePlacementAccessBaselines(*activeNavigation_, placementAccessRequirements);
    }
    const auto preservesPlacementAccess = [this, &placementAccessRequirements](
        const Position center, const UnitType structure,
        std::span<PlacementAccessRequirement> additionalRequirements = {}) {
        if (activeNavigation_ == nullptr || activeNavigation_->empty()) return true;
        return placementPreservesAccess(*activeNavigation_, std::uint64_t{1} << 62U,
            placementObstacleBounds(center, structure), placementAccessRequirements,
            12000, additionalRequirements);
    };

    if (kind == UnitKind::nexus) {
        const auto desiredSite = constructionSite.valid()
            ? constructionSite.anchor : plan.expansionTarget;
        if (!desiredSite.valid()) {
            lastMacroStatus_ = "nexus-site-unidentified";
            return TilePositions::None;
        }
        const ResourceSite* best = nullptr;
        auto bestScore = std::numeric_limits<double>::infinity();
        auto validSites = 0;
        auto reachableSites = 0;
        auto freeSites = 0;
        auto accessRejectedSites = 0;
        auto accessMissingRequirements = 0;
        auto accessUnreachableBaselines = 0;
        auto accessWouldBlockRoutes = 0;
        auto accessSitesMissingSources = 0;
        auto accessSitesMissingTargets = 0;
        auto accessSitesWithoutMinerals = 0;
        for (const auto& site : resourceSites_) {
            if (!site.depotTile.isValid()) continue;
            if (!closeTo(site.depotCenter, desiredSite, 64)) continue;
            ++validSites;
            // Use the full-map placement query here: neutral bases and starts
            // can still be fogged when resource clusters are discovered, but
            // a newly blocked footprint must not keep receiving a Nexus task.
            if (!Broodwar->canBuildHere(site.depotTile, type, builder, false) &&
                !Broodwar->canBuildHere(site.depotTile, type, nullptr, false)) continue;
            const auto center = site.depotCenter;
            if (!Broodwar->hasPath(builder->getPosition(), toBwapiPosition(center))) continue;
            ++reachableSites;
            const auto occupied = std::ranges::any_of(
                Broodwar->getAllUnits(),
                [center](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           unit->getType().isResourceDepot() &&
                           closeTo(center, fromBwapi(unit->getPosition()), 320);
                });
            const auto rememberedDepot = std::ranges::any_of(
                enemyMemory_, [center](const auto& entry) {
                    const auto& unit = entry.second;
                    return unit.role == UnitRole::resourceDepot &&
                           closeTo(center, unit.position, 320);
                });
            if (occupied || rememberedDepot) continue;
            ++freeSites;

            if (activeNavigation_ != nullptr && !activeNavigation_->empty()) {
                std::vector<PlacementAccessRequirement> expansionAccessRequirements;
                const auto sources = accessPointsAroundFootprint(
                    *activeNavigation_, center, UnitTypes::Protoss_Nexus,
                    placementFootprint(UnitTypes::Protoss_Probe));
                auto siteMineralCount = 0;
                // Expansion sites are commonly fogged. Use the same static
                // resource positions that formed this site instead of the
                // visible-only mineral list, or an unseen natural appears to
                // have no worker access requirements and is rejected.
                for (const auto mineral : Broodwar->getStaticMinerals()) {
                    if (mineral == nullptr ||
                        !closeTo(site.resourceCenter,
                                 fromBwapi(mineral->getInitialPosition()), 352)) continue;
                    ++siteMineralCount;
                    const auto targets = accessPointsAroundPosition(
                        *activeNavigation_, fromBwapi(mineral->getInitialPosition()),
                        placementFootprint(UnitTypes::Protoss_Probe));
                    if (targets.empty()) ++accessSitesMissingTargets;
                    addPlacementAccessRequirement(expansionAccessRequirements,
                        std::span<const Position>(sources), std::span<const Position>(targets),
                        placementFootprint(UnitTypes::Protoss_Probe));
                }
                if (sources.empty()) ++accessSitesMissingSources;
                if (siteMineralCount == 0) ++accessSitesWithoutMinerals;
                capturePlacementAccessBaselines(*activeNavigation_, expansionAccessRequirements);
                const auto missingRequirements = expansionAccessRequirements.empty();
                const auto hasUnreachableMineral = missingRequirements ||
                    std::ranges::any_of(expansionAccessRequirements,
                        [](const PlacementAccessRequirement& requirement) {
                            return !requirement.reachableBefore;
                        });
                const auto preservesRoutes = !hasUnreachableMineral &&
                    preservesPlacementAccess(center, UnitTypes::Protoss_Nexus,
                        std::span<PlacementAccessRequirement>(expansionAccessRequirements));
                if (!preservesRoutes) {
                    ++accessRejectedSites;
                    if (missingRequirements) ++accessMissingRequirements;
                    else if (hasUnreachableMineral) ++accessUnreachableBaselines;
                    else ++accessWouldBlockRoutes;
                    continue;
                }
            }

            auto resources = 0;
            for (const auto patch : Broodwar->getMinerals()) {
                if (closeTo(site.resourceCenter, fromBwapi(patch->getInitialPosition()), 352))
                    resources += patch->getResources();
            }
            auto nearestEnemy = std::numeric_limits<double>::infinity();
            for (const auto& [id, enemy] : enemyMemory_) {
                static_cast<void>(id);
                if (enemy.position.valid())
                    nearestEnemy = std::min(nearestEnemy, distance(center, enemy.position));
            }
            const auto danger = std::max(0.0, 1200.0 - nearestEnemy) *
                                (plan.posture == Posture::defend ? 1.8 : 0.8);
            const auto travel = distance(fromBwapi(builder->getPosition()), center);
            const auto strategicSite = closeTo(center, desiredSite, 64);
            const auto score = travel + danger - static_cast<double>(resources) / 40.0 -
                (strategicSite ? 100000.0 : 0.0);
            if (score < bestScore) {
                bestScore = score;
                best = &site;
            }
        }
        if (best != nullptr) {
            return best->depotTile;
        }
        // Never fall through to generic placement and warp a Nexus into the
        // main when no real resource site is currently available.
        lastMacroStatus_ = "placement-nexus-s" + std::to_string(resourceSites_.size()) +
                           "-v" + std::to_string(validSites) + "-r" +
                           std::to_string(reachableSites) + "-f" +
                           std::to_string(freeSites) + "-a" +
                           std::to_string(accessRejectedSites) + "-am" +
                           std::to_string(accessMissingRequirements) + "-au" +
                           std::to_string(accessUnreachableBaselines) + "-ab" +
                           std::to_string(accessWouldBlockRoutes) + "-as" +
                           std::to_string(accessSitesMissingSources) + "-at" +
                           std::to_string(accessSitesMissingTargets) + "-an" +
                           std::to_string(accessSitesWithoutMinerals);
        return TilePositions::None;
    }

    auto anchorPosition = plan.rallyPoint.valid()
                              ? toBwapiPosition(plan.rallyPoint)
                              : BWAPI::Position(Broodwar->self()->getStartLocation());
    const auto defenseDirectionKnown = Broodwar->getStartLocations().size() == 2U ||
        std::ranges::any_of(enemyMemory_, [](const auto& entry) {
            return entry.second.role == UnitRole::resourceDepot;
        });
    auto useForwardLayout = false;
    auto startLayoutAtCenter = false;
    auto preserveBaseAnchor = false;
    auto avoidEnemyFire = false;
    Unit defendedNexus = nullptr;
    if (kind == UnitKind::pylon) {
        const auto emergencySupply = Broodwar->self()->supplyUsed() >=
                                     Broodwar->self()->supplyTotal();
        Unit disabledProduction = nullptr;
        for (const auto building : Broodwar->self()->getUnits()) {
            if (building == nullptr || !building->exists() || !building->isCompleted() ||
                !building->getType().isBuilding() || building->isPowered()) {
                continue;
            }
            if (disabledProduction == nullptr ||
                building->getID() < disabledProduction->getID()) {
                disabledProduction = building;
            }
        }
        if (emergencySupply) {
            // Restore production first. Do not send the emergency builder to
            // the least-powered remote base or a forward rally point.
            const auto safeNexus = Broodwar->getClosestUnit(
                builder->getPosition(), Filter::IsOwned && Filter::IsCompleted &&
                    Filter::GetType == UnitTypes::Protoss_Nexus);
            if (safeNexus != nullptr) anchorPosition = safeNexus->getPosition();
            disabledProduction = nullptr;
        } else if (disabledProduction != nullptr) {
            anchorPosition = disabledProduction->getPosition();
        }
        Unit leastPoweredBase = nullptr;
        auto fewestNearbyPylons = std::numeric_limits<int>::max();
        for (const auto nexus : Broodwar->self()->getUnits()) {
            if (nexus == nullptr || !nexus->exists() ||
                nexus->getType() != UnitTypes::Protoss_Nexus) continue;
            const auto nearby = static_cast<int>(Broodwar->getUnitsInRadius(
                nexus->getPosition(), 384,
                Filter::IsOwned && Filter::GetType == UnitTypes::Protoss_Pylon).size());
            if (nearby < fewestNearbyPylons) {
                fewestNearbyPylons = nearby;
                leastPoweredBase = nexus;
            }
        }
        if (!emergencySupply && disabledProduction == nullptr && leastPoweredBase != nullptr) {
            const auto pylonCount = std::ranges::count_if(
                Broodwar->self()->getUnits(), [](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           unit->getType() == UnitTypes::Protoss_Pylon;
                });
            const auto hasForwardDefense = std::ranges::any_of(
                Broodwar->self()->getUnits(), [](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           (unit->getType() == UnitTypes::Protoss_Photon_Cannon ||
                            unit->getType() == UnitTypes::Protoss_Shield_Battery);
                });
            const auto needsFirstForwardPower = pylonCount == 0;
            const auto needsRedundantForwardPower = pylonCount == 1 && hasForwardDefense;
            const auto forwardDistance = plan.rallyPoint.valid()
                                             ? distance(fromBwapi(leastPoweredBase->getPosition()),
                                                        plan.rallyPoint)
                                             : 0.0;
            if (defenseDirectionKnown &&
                (needsFirstForwardPower || needsRedundantForwardPower) &&
                forwardDistance >= 64.0 && forwardDistance <= 256.0) {
                // Once the enemy-facing direction is known, power the intercept
                // with the first Pylon, then
                // put redundant power halfway back toward the Nexus. The
                // backup still overlaps the Cannon screen without sending its
                // builder through the forward Marine lane.
                if (needsRedundantForwardPower) {
                    const auto basePosition = fromBwapi(leastPoweredBase->getPosition());
                    anchorPosition = toBwapiPosition(
                        {(basePosition.x + plan.rallyPoint.x) / 2,
                         (basePosition.y + plan.rallyPoint.y) / 2});
                    startLayoutAtCenter = true;
                    avoidEnemyFire = true;
                } else {
                    anchorPosition = toBwapiPosition(plan.rallyPoint);
                }
                useForwardLayout = true;
            } else {
                anchorPosition = leastPoweredBase->getPosition();
            }
        }
    }
    const auto vulnerableTech = kind == UnitKind::forge ||
                                kind == UnitKind::cyberneticsCore ||
                                kind == UnitKind::roboticsFacility ||
                                kind == UnitKind::observatory ||
                                kind == UnitKind::roboticsSupportBay ||
                                kind == UnitKind::stargate ||
                                kind == UnitKind::citadelOfAdun ||
                                kind == UnitKind::templarArchives ||
                                kind == UnitKind::fleetBeacon ||
                                kind == UnitKind::arbiterTribunal;
    const auto highValueTech = kind == UnitKind::roboticsFacility ||
                               kind == UnitKind::observatory ||
                               kind == UnitKind::roboticsSupportBay;
    const auto protectProduction = kind == UnitKind::gateway;
    if ((defenseDirectionKnown || protectProduction) && type.requiresPsi() &&
        (vulnerableTech || protectProduction)) {
        const auto base = Broodwar->getClosestUnit(
            builder->getPosition(),
            Filter::GetType == UnitTypes::Protoss_Nexus && Filter::IsCompleted &&
                Filter::IsOwned);
        if (base != nullptr) {
            // The first Pylons intentionally sit on the intercept. Tech built
            // around those Pylons was exposed ahead of the Cannons and had to
            // be rebuilt. Search the powered tiles nearest the Nexus instead.
            anchorPosition = base->getPosition();
            useForwardLayout = true;
            preserveBaseAnchor = true;
            // Robotics and detection are the bridge from a hold to a
            // counterattack.  If an enemy army is already in the home area,
            // reject forward-cluster tiles inside a generous weapon buffer so
            // the building survives long enough to produce its first unit.
            if (highValueTech || protectProduction) {
                Unit danger = nullptr;
                auto dangerDistance = std::numeric_limits<int>::max();
                for (const auto enemy : Broodwar->enemy()->getUnits()) {
                    if (enemy == nullptr || !enemy->exists() ||
                        !enemy->isVisible() || !enemy->isCompleted()) continue;
                    const auto weapon = enemy->getType().groundWeapon();
                    if (weapon == WeaponTypes::None) continue;
                    const auto distanceToBase = enemy->getDistance(base);
                    if (distanceToBase < dangerDistance) {
                        danger = enemy;
                        dangerDistance = distanceToBase;
                    }
                }
                if (danger != nullptr && dangerDistance <= 1024) {
                    const auto basePosition = fromBwapi(base->getPosition());
                    const auto threatPosition = fromBwapi(danger->getPosition());
                    const auto dx = basePosition.x - threatPosition.x;
                    const auto dy = basePosition.y - threatPosition.y;
                    const auto length = std::hypot(static_cast<double>(dx),
                                                   static_cast<double>(dy));
                    if (length > 0.001) {
                        anchorPosition = {
                            basePosition.x + static_cast<int>(std::lround(dx / length * 192.0)),
                            basePosition.y + static_cast<int>(std::lround(dy / length * 192.0)),
                        };
                        startLayoutAtCenter = true;
                    }
                    avoidEnemyFire = true;
                }
            }
        }
    }
    if (type.requiresPsi() && !preserveBaseAnchor) {
        const auto pylon = Broodwar->getClosestUnit(
            anchorPosition,
            Filter::GetType == UnitTypes::Protoss_Pylon && Filter::IsCompleted &&
                Filter::IsOwned);
        if (pylon != nullptr) anchorPosition = pylon->getPosition();
    }
    if (kind == UnitKind::photonCannon || kind == UnitKind::shieldBattery) {
        Unit forwardNexus = nullptr;
        auto fewestNearbyDefenses = std::numeric_limits<int>::max();
        auto bestDistance = std::numeric_limits<double>::infinity();
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit == nullptr || !unit->exists() ||
                unit->getType() != UnitTypes::Protoss_Nexus) continue;
            // An unpowered natural is not a legal Cannon destination. If it
            // wins the "fewest defenses" comparison, returning immediately
            // below also prevents a second Cannon at the powered main. That
            // left the mineral line exposed through an entire Zergling flood.
            const auto localPylon = Broodwar->getClosestUnit(
                unit->getPosition(),
                Filter::GetType == UnitTypes::Protoss_Pylon && Filter::IsCompleted &&
                    Filter::IsOwned);
            if (localPylon == nullptr || localPylon->getDistance(unit) > 384)
                continue;
            const auto nearby = static_cast<int>(Broodwar->getUnitsInRadius(
                unit->getPosition(), 416,
                Filter::IsOwned && Filter::GetType == type).size());
            const auto candidate = plan.attackTarget.valid()
                                       ? distance(fromBwapi(unit->getPosition()),
                                                  plan.attackTarget)
                                       : distance(fromBwapi(unit->getPosition()),
                                                  fromBwapi(anchorPosition));
            if (nearby < fewestNearbyDefenses ||
                (nearby == fewestNearbyDefenses && candidate < bestDistance)) {
                fewestNearbyDefenses = nearby;
                bestDistance = candidate;
                forwardNexus = unit;
            }
        }
        if (forwardNexus != nullptr) {
            defendedNexus = forwardNexus;
            // The Pylon is already offset from the Nexus. Anchoring the layout
            // to it and applying another layout offset placed Cannons beyond
            // useful mineral-line coverage. Search around the defended Nexus
            // instead; the normal hasPower check below still guarantees psi.
            const auto forwardDistance = plan.rallyPoint.valid()
                                             ? distance(fromBwapi(forwardNexus->getPosition()),
                                                        plan.rallyPoint)
                                             : 0.0;
            if (defenseDirectionKnown && forwardDistance >= 64.0 &&
                forwardDistance <= 256.0) {
                // Intercept ranged rushes before they acquire the Probe line.
                // Keeping the anchor within eight tiles of the Nexus preserves
                // compact power coverage and short reinforcement paths.
                const auto groundPressure = std::ranges::any_of(
                    Broodwar->enemy()->getUnits(), [forwardNexus](const Unit enemy) {
                        if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                            !enemy->isCompleted()) {
                            return false;
                        }
                        const auto weapon = enemy->getType().groundWeapon();
                        return weapon != WeaponTypes::None &&
                               enemy->getDistance(forwardNexus) <= 512;
                    });
                if (groundPressure) {
                    const auto basePosition = fromBwapi(forwardNexus->getPosition());
                    anchorPosition = toBwapiPosition(
                        {(basePosition.x + plan.rallyPoint.x) / 2,
                         (basePosition.y + plan.rallyPoint.y) / 2});
                    startLayoutAtCenter = true;
                    avoidEnemyFire = true;
                } else {
                    anchorPosition = toBwapiPosition(plan.rallyPoint);
                }
                useForwardLayout = true;
            } else {
                anchorPosition = forwardNexus->getPosition();
            }
        } else {
            lastMacroStatus_ = "placement-defense-no-powered-base";
            return TilePositions::None;
        }
    }

    if (homeAnchoredSupplyPylon(kind, reason, constructionSite.valid())) {
        // Supply forecasts and builder selection measure a Probe's route to
        // the home Nexus. Keep ordinary supply Pylons on that same anchor;
        // choosing the least-powered expansion here adds an unforecast trip.
        const auto start = BWAPI::Position(Broodwar->self()->getStartLocation());
        const auto home = Broodwar->getClosestUnit(
            start, Filter::IsOwned && Filter::IsCompleted &&
                       Filter::GetType == UnitTypes::Protoss_Nexus);
        anchorPosition = home != nullptr ? home->getPosition() : start;
        useForwardLayout = true;
        startLayoutAtCenter = true;
        preserveBaseAnchor = true;
        avoidEnemyFire = false;
        defendedNexus = nullptr;
    }

    // A site-scoped task owns its anchor. Apply it after the legacy tactical
    // layout selection so another base's defense count cannot redirect it.
    if (constructionSite.valid()) {
        anchorPosition = toBwapiPosition(constructionSite.anchor);
        useForwardLayout = true;
        startLayoutAtCenter = true;
        preserveBaseAnchor = true;
        avoidEnemyFire = false;
        defendedNexus = nullptr;
    }

    static const std::array standardLayout{
        TilePosition{4, 2}, TilePosition{-4, 2}, TilePosition{4, -3},
        TilePosition{-4, -3}, TilePosition{7, 1}, TilePosition{-7, 1},
        TilePosition{2, 6}, TilePosition{-2, 6}, TilePosition{2, -6},
        TilePosition{-2, -6},
    };
    static const std::array forwardLayout{
        TilePosition{0, 0}, TilePosition{2, 0}, TilePosition{-2, 0},
        TilePosition{0, 2}, TilePosition{0, -2}, TilePosition{2, 2},
        TilePosition{-2, 2}, TilePosition{2, -2}, TilePosition{-2, -2},
        TilePosition{4, 0},
    };
    const auto& layout = useForwardLayout ? forwardLayout : standardLayout;
    // Non-producing tech can share edges, but Robotics and Gateway exits must
    // stay open for large units. A legal Probe path does not prove that the
    // Reaver produced there can leave the building cluster.
    const auto structureGap = vulnerableTech && !type.canProduce() ? 0 : 32;
    const auto existing = static_cast<std::size_t>(std::ranges::count_if(
        Broodwar->self()->getUnits(), [type](const Unit unit) {
            return unit != nullptr && unit->exists() && unit->getType() == type;
        }));
    const auto layoutStart = startLayoutAtCenter ? std::size_t{0} : existing;
    const auto anchor = TilePosition(anchorPosition);
    auto miningLaneFallback = TilePositions::None;
    auto validCandidates = 0;
    auto poweredCandidates = 0;
    auto buildableCandidates = 0;
    auto reachableCandidates = 0;
    auto laneCandidates = 0;
    auto enemyFireFallback = TilePositions::None;
    auto enemyFireFallbackScore = -std::numeric_limits<double>::infinity();
    std::vector<DefensivePosition> reservedEntrances;
    for (const auto& site : resourceSites_) {
        const auto owned = std::ranges::any_of(Broodwar->self()->getUnits(), [&site](const Unit unit) {
            return unit != nullptr && unit->exists() && unit->getType().isResourceDepot() &&
                   closeTo(site.depotCenter, fromBwapi(unit->getPosition()), 320);
        });
        if (owned) reservedEntrances.insert(reservedEntrances.end(), site.defenses.begin(), site.defenses.end());
    }
    const auto candidatePreservesAccess = [&](const TilePosition location) {
        if (activeNavigation_ == nullptr || activeNavigation_->empty()) return true;
        const Position center{
            location.x * 32 + type.tileWidth() * 16,
            location.y * 32 + type.tileHeight() * 16,
        };
        std::vector<PlacementAccessRequirement> candidateRequirements;
        const auto needsCombatExit = kind == UnitKind::gateway ||
            kind == UnitKind::roboticsFacility || kind == UnitKind::photonCannon ||
            kind == UnitKind::shieldBattery;
        if (needsCombatExit) {
            const auto outputType = kind == UnitKind::gateway
                ? UnitTypes::Protoss_Dragoon
                : kind == UnitKind::roboticsFacility ? UnitTypes::Protoss_Reaver
                                                      : UnitTypes::Protoss_Dragoon;
            const auto nexus = Broodwar->getClosestUnit(
                toBwapiPosition(center), Filter::IsOwned && Filter::IsCompleted &&
                    Filter::GetType == UnitTypes::Protoss_Nexus);
            if (nexus != nullptr) {
                const auto outputFootprint = placementFootprint(outputType);
                const auto sources = accessPointsAroundFootprint(
                    *activeNavigation_, center, type, outputFootprint);
                const auto targets = accessPointsAroundFootprint(
                    *activeNavigation_, fromBwapi(nexus->getPosition()),
                    UnitTypes::Protoss_Nexus, outputFootprint);
                addPlacementAccessRequirement(candidateRequirements,
                    std::span<const Position>(sources), std::span<const Position>(targets),
                    outputFootprint);
                capturePlacementAccessBaselines(*activeNavigation_, candidateRequirements);
                if (candidateRequirements.empty() ||
                    !candidateRequirements.front().reachableBefore) return false;
            }
        }
        return preservesPlacementAccess(center, type,
            std::span<PlacementAccessRequirement>(candidateRequirements));
    };
    const auto usable = [&](const TilePosition location, const bool validateAccess = true) {
        if (!location.isValid() || location.x < 0 || location.y < 0 ||
            location.x + type.tileWidth() > Broodwar->mapWidth() ||
            location.y + type.tileHeight() > Broodwar->mapHeight()) {
            return false;
        }
        ++validCandidates;
        if (type.requiresPsi() && !Broodwar->hasPower(location, type)) return false;
        ++poweredCandidates;
        // Unit::build performs this same check with checkExplored=true.  Using
        // false here can select a nominally buildable fogged tile which the
        // command then rejects as Unbuildable_Location.
        // `canBuildHere` with a Probe argument is stricter than the actual
        // placement test on some BWAPI builds: a Probe that is still carrying
        // a mineral or finishing a previous move can make every otherwise
        // legal tile report false for one frame.  Stardust separates the
        // footprint test from the worker schedule.  Accept the location when
        // the map-level check is legal, then keep the explicit path and
        // command-time validation below as the worker-side guard.
        if (!Broodwar->canBuildHere(location, type, builder, true) &&
            !Broodwar->canBuildHere(location, type, nullptr, true)) {
            return false;
        }
        const BuildingFootprint footprint{
            {location.x * 32, location.y * 32}, type.tileWidth() * 32, type.tileHeight() * 32};
        if (kind != UnitKind::nexus && plan.expansionTarget.valid() &&
            !separatedByGap(footprint,
                {{plan.expansionTarget.x - 64, plan.expansionTarget.y - 48}, 128, 96}, 32))
            return false;
        if (kind != UnitKind::nexus) {
            for (const auto& entrance : reservedEntrances) {
                for (const auto point : {entrance.anchor, entrance.entrance}) {
                    if (!separatedByGap(footprint, {{point.x - 32, point.y - 32}, 64, 64}, 32)) return false;
                }
            }
        }
        for (const auto building : Broodwar->self()->getUnits()) {
            if (building == nullptr || !building->exists() || !building->getType().isBuilding() ||
                building->isLifted()) continue;
            const auto otherType = building->getType();
            const auto otherTile = building->getTilePosition();
            const BuildingFootprint occupied{
                {otherTile.x * 32, otherTile.y * 32}, otherType.tileWidth() * 32,
                otherType.tileHeight() * 32};
            // Adjacent buildings created sealed pockets of fresh Dragoons.
            // Reserve a full build tile for movement between structures.
            const auto exitGap = type.canProduce() || otherType.canProduce() ? 32 : structureGap;
            if (!separatedByGap(footprint, occupied, exitGap)) return false;
        }
        for (const auto& [taskId, pending] : pendingBuilds_) {
            static_cast<void>(taskId);
            const auto pendingKind = pending.kind;
            const auto pendingType = toBwapi(pendingKind);
            if (pending.target.x < 0 || pending.target.y < 0 ||
                pendingType == UnitTypes::None) continue;
            if (!separatedByGap(footprint,
                    {pending.target, pendingType.tileWidth() * 32, pendingType.tileHeight() * 32},
                    type.canProduce() || pendingType.canProduce() ? 32 : structureGap)) return false;
        }
        ++buildableCandidates;
        const Position center{
            location.x * 32 + type.tileWidth() * 16,
            location.y * 32 + type.tileHeight() * 16,
        };
        if (avoidEnemyFire) {
            auto unsafe = false;
            auto nearestThreatDistance = std::numeric_limits<double>::infinity();
            for (const auto enemy : Broodwar->enemy()->getUnits()) {
                if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                    !enemy->isCompleted()) continue;
                const auto weapon = enemy->getType().groundWeapon();
                if (weapon == WeaponTypes::None) continue;
                const auto threatDistance = enemy->getDistance(toBwapiPosition(center));
                nearestThreatDistance = std::min(nearestThreatDistance,
                                                 static_cast<double>(threatDistance));
                unsafe = unsafe || threatDistance <= weapon.maxRange() +
                                      (highValueTech ? 256 : 96);
            }
            if (unsafe) {
                // Do not deadlock critical tech simply because every powered
                // tile is inside a live ranged weapon's conservative margin.
                // Keep the safest legal fallback (farthest from the nearest
                // threat, then closest to the base anchor) and use it only if
                // the exhaustive safe search finds no alternative.
                const auto anchorDistance = distance(center, fromBwapi(anchorPosition));
                const auto score = nearestThreatDistance * 4.0 - anchorDistance * 0.1;
                if (builder->hasPath(toBwapiPosition(center)) &&
                    score > enemyFireFallbackScore) {
                    enemyFireFallback = location;
                    enemyFireFallbackScore = score;
                }
                return false;
            }
        }
        if (std::ranges::any_of(
                failedBuildSites_, [kind, center](const FailedBuildSite& failed) {
                    return failed.kind == kind &&
                           distanceSquared(failed.target, center) <= 64 * 64;
                })) {
            return false;
        }
        if (!validateAccess) return true;
        if (!candidatePreservesAccess(location)) return false;
        if (!builder->hasPath(toBwapiPosition(center))) return false;
        ++reachableCandidates;
        return true;
    };
    const auto consider = [&](const TilePosition location) {
        if (!usable(location)) return TilePositions::None;
        if (!blocksMiningLane(location, type)) return location;
        ++laneCandidates;
        if (!miningLaneFallback.isValid()) miningLaneFallback = location;
        return TilePositions::None;
    };
    if (protectProduction) {
        // getBuildLocation returns its first legal tile, which can be on the
        // exposed side of a distant Pylon. Rank the compact powered footprint
        // around the economy instead of accepting that search-order accident.
        std::vector<std::pair<double, TilePosition>> rankedLocations;
        rankedLocations.reserve(21U * 21U);
        for (auto dy = -10; dy <= 10; ++dy) {
            for (auto dx = -10; dx <= 10; ++dx) {
                const auto location = anchor + TilePosition{dx, dy};
                const Position center{location.x * 32 + type.tileWidth() * 16,
                                      location.y * 32 + type.tileHeight() * 16};
                const auto score = distance(center, fromBwapi(anchorPosition)) +
                    distance(center, fromBwapi(builder->getPosition())) * 0.10;
                rankedLocations.emplace_back(score, location);
            }
        }
        // Utility does not depend on the access proof. Validate candidates in
        // score order, retaining the same best legal tile while avoiding route
        // searches for every successively better tile in scan order.
        std::ranges::stable_sort(rankedLocations, {}, [](const auto& entry) { return entry.first; });
        for (const auto& [score, location] : rankedLocations) {
            static_cast<void>(score);
            if (usable(location) && !blocksMiningLane(location, type)) {
                lastMacroStatus_ = "placement-protected-production";
                return location;
            }
        }
    }
    if (defendedNexus != nullptr) {
        // Rank defensive structures by the targets they can actually protect.
        // A nearby Cannon outside weapon range contributes no coverage, while
        // a Battery should sit near shield users rather than merely near ore.
        std::vector<std::pair<double, TilePosition>> rankedLocations;
        rankedLocations.reserve(17U * 17U);
        const auto nexusPosition = fromBwapi(defendedNexus->getPosition());
        const auto nexusTile = defendedNexus->getTilePosition();
        Position intercept{-1, -1};
        auto nearestApproach = 1100 * 1100;
        for (const auto& [id, enemy] : enemyMemory_) {
            static_cast<void>(id);
            if (!enemy.visible || !enemy.completed || enemy.flying ||
                enemy.groundWeapon.damage <= 0 || !enemy.position.valid() ||
                isWorker(enemy.kind)) continue;
            const auto separation = distanceSquared(enemy.position, nexusPosition);
            if (separation < nearestApproach) {
                nearestApproach = separation;
                intercept = moveToward(nexusPosition, enemy.position, 160.0);
            }
        }
        std::vector<Unit> mineralPatches;
        std::vector<Unit> existingCannonUnits;
        std::vector<Unit> existingBatteries;
        std::vector<Unit> powerSources;
        for (const auto patch : Broodwar->getMinerals()) {
            if (patch->exists() && patch->getResources() > 0 &&
                patch->getDistance(defendedNexus) <= 288) {
                mineralPatches.push_back(patch);
            }
        }
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit == nullptr || !unit->exists() || !unit->isCompleted()) continue;
            if (unit->getType() == UnitTypes::Protoss_Photon_Cannon && unit->isPowered()) {
                existingCannonUnits.push_back(unit);
            } else if (unit->getType() == UnitTypes::Protoss_Shield_Battery &&
                       unit->isPowered() && unit->getEnergy() >= 10) {
                existingBatteries.push_back(unit);
            } else if (unit->getType() == UnitTypes::Protoss_Pylon) {
                powerSources.push_back(unit);
            }
        }
        std::vector<Unit> shieldUsers;
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit == nullptr || !unit->exists() || !unit->isCompleted() ||
                unit->getType().isBuilding() || unit->getType().maxShields() <= 0 ||
                unit->getDistance(defendedNexus) > 640) continue;
            shieldUsers.push_back(unit);
        }
        const auto cannonGroundRange = UnitTypes::Protoss_Photon_Cannon
                                           .groundWeapon().maxRange();
        const auto cannonAirRange = UnitTypes::Protoss_Photon_Cannon
                                        .airWeapon().maxRange();
        const auto batteryActiveFor = [&existingBatteries](const Unit unit) {
            return std::ranges::any_of(existingBatteries, [unit](const Unit battery) {
                return distanceSquared(fromBwapi(battery->getPosition()),
                                       fromBwapi(unit->getPosition())) <= 256 * 256;
            });
        };
        for (auto dy = -8; dy <= 8; ++dy) {
            for (auto dx = -8; dx <= 8; ++dx) {
                const auto location = nexusTile + TilePosition{dx, dy};
                if (!usable(location, false) || blocksMiningLane(location, type)) continue;
                const Position center{location.x * 32 + type.tileWidth() * 16,
                                      location.y * 32 + type.tileHeight() * 16};
                auto score = -distance(center, nexusPosition) * 0.04;
                if (intercept.valid()) score -= distance(center, intercept) * 0.30;
                else if (plan.rallyPoint.valid()) score -= distance(center, plan.rallyPoint) * 0.08;

                if (kind == UnitKind::photonCannon) {
                    const auto candidateRadius = std::max({
                        type.dimensionLeft(), type.dimensionRight(),
                        type.dimensionUp(), type.dimensionDown()});
                    for (const auto patch : mineralPatches) {
                        const auto candidateCoversPatch =
                            patch->getDistance(toBwapiPosition(center)) <=
                                cannonGroundRange + candidateRadius;
                        if (!candidateCoversPatch) continue;
                        const auto alreadyCovered = std::ranges::any_of(
                            existingCannonUnits, [patch](const Unit cannon) {
                                return cannon->getDistance(patch) <=
                                    cannon->getType().groundWeapon().maxRange();
                            });
                        score += alreadyCovered ? 1.5 : (intercept.valid() ? 12.0 : 20.0);
                    }
                    const auto candidateHeight = Broodwar->getGroundHeight(location);
                    for (const auto threat : Broodwar->enemy()->getUnits()) {
                        if (threat == nullptr || !threat->exists() || !threat->isVisible() ||
                            !threat->isDetected() || !threat->isCompleted() ||
                            isWorker(toKind(threat->getType()))) continue;
                        const auto weaponRange = threat->isFlying()
                            ? cannonAirRange : cannonGroundRange;
                        if (weaponRange <= 0) continue;
                        // Unit::getDistance(Position) measures from the target's
                        // collision edge to this candidate's center. Add the
                        // candidate edge to compare it with weapon range.
                        const auto edgeDistance = threat->getDistance(
                            toBwapiPosition(center)) - candidateRadius;
                        const auto threatValue = std::max(1.0,
                            unitStats(toKind(threat->getType())).combatValue);
                        const auto alreadyCovered = std::ranges::any_of(
                            existingCannonUnits, [threat](const Unit cannon) {
                                const auto weapon = threat->isFlying()
                                    ? cannon->getType().airWeapon()
                                    : cannon->getType().groundWeapon();
                                return weapon != WeaponTypes::None &&
                                       cannon->getDistance(threat) <= weapon.maxRange();
                            });
                        if (edgeDistance >= 0 && edgeDistance <= weaponRange) {
                            auto terrainWeight = 1.0;
                            if (!threat->isFlying()) {
                                const auto threatHeight = Broodwar->getGroundHeight(
                                    threat->getTilePosition());
                                if (candidateHeight > threatHeight) terrainWeight = 1.15;
                                else if (candidateHeight < threatHeight) terrainWeight = 0.80;
                            }
                            score += threatValue * terrainWeight *
                                (alreadyCovered ? 3.0 : 18.0);
                        } else if (edgeDistance < weaponRange + 192) {
                            // A nearby but currently uncovered approach affects
                            // direction preference without counting as coverage.
                            const auto gap = std::max(0, edgeDistance - weaponRange);
                            score += threatValue * (1.0 - gap / 192.0) * 1.5;
                        }
                    }

                } else {
                    for (const auto worker : shieldUsers) {
                        const auto separation = distanceSquared(
                            center, fromBwapi(worker->getPosition()));
                        if (separation > 256 * 256 || batteryActiveFor(worker)) continue;
                        const auto missingShields = std::max(
                            0, worker->getType().maxShields() - worker->getShields());
                        score += 1.0 + std::min(120, missingShields) * 0.08;
                    }
                }
                auto nearbyPowerSources = 0;
                for (const auto pylon : powerSources) {
                    if (pylon->getDistance(toBwapiPosition(center)) <= 224)
                        ++nearbyPowerSources;
                }
                score += std::min(2, std::max(0, nearbyPowerSources - 1)) * 10.0;
                rankedLocations.emplace_back(score, location);
            }
        }
        std::ranges::stable_sort(rankedLocations, [](const auto& first, const auto& second) {
            return first.first > second.first;
        });
        for (const auto& [score, location] : rankedLocations) {
            static_cast<void>(score);
            if (usable(location)) {
                lastMacroStatus_ = "placement-defensive-coverage";
                return location;
            }
        }
    }
    for (std::size_t attempt = 0; attempt < layout.size(); ++attempt) {
        const auto offset = layout[(layoutStart + attempt) % layout.size()];
        // Test the preferred tile directly. Calling BWAPI's nested radius
        // searches here repeatedly scanned the same crowded area before our
        // own complete fallback search even began.
        const auto location = anchor + offset;
        const auto accepted = consider(location);
        if (accepted.isValid()) {
            lastMacroStatus_ = "placement-safe";
            return accepted;
        }
    }

    const auto broadAccepted = consider(anchor);
    if (broadAccepted.isValid()) {
        lastMacroStatus_ = "placement-safe";
        return broadAccepted;
    }
    // Search all fallback tiles across successive macro passes. Keep the
    // compact scored defense/production search above responsive every time,
    // but bound the expensive BWAPI checks in the broad sweep. Late-game
    // traces had more than 8,000 candidates per structure on every retry.
    auto& continuation = placementSearches_[taskKey];
    PlacementSearchWindow search(continuation.offset);
    const auto considerFallback = [&](const TilePosition location) {
        return search.visit() ? consider(location) : TilePositions::None;
    };
    constexpr auto maximumRadius = 16;
    for (auto radius = 2; radius <= maximumRadius; ++radius) {
        for (auto dx = -radius; dx <= radius; ++dx) {
            for (const auto dy : {-radius, radius}) {
                const auto accepted = considerFallback(anchor + TilePosition(dx, dy));
                if (accepted.isValid()) {
                    lastMacroStatus_ = "placement-safe";
                    return accepted;
                }
            }
        }
        for (auto dy = -radius + 1; dy < radius; ++dy) {
            for (const auto dx : {-radius, radius}) {
                const auto accepted = considerFallback(anchor + TilePosition(dx, dy));
                if (accepted.isValid()) {
                    lastMacroStatus_ = "placement-safe";
                    return accepted;
                }
            }
        }
    }

    // A crowded main or forward base must not make a rich multi-base economy
    // incapable of adding production. The primary search is deliberately
    // compact, but when it is exhausted, inspect powered space around every
    // completed Pylon before declaring placement failure. This also avoids
    // tying all late-game construction to whichever Probe happened to be
    // closest to an attack rally point.
    if (type.requiresPsi()) {
        std::vector<Unit> pylons;
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit != nullptr && unit->exists() && unit->isCompleted() &&
                unit->getType() == UnitTypes::Protoss_Pylon) {
                if (constructionSite.valid() &&
                    distanceSquared(fromBwapi(unit->getPosition()),
                                    constructionSite.anchor) > 512 * 512) {
                    continue;
                }
                pylons.push_back(unit);
            }
        }
        // Stable ordering lets the next window progress even when a different
        // Probe is selected or the previous builder has moved.
        std::ranges::sort(pylons, [](const Unit lhs, const Unit rhs) {
            return lhs->getID() < rhs->getID();
        });
        for (const auto pylon : pylons) {
            const auto poweredAnchor = pylon->getTilePosition();
            for (auto radius = 1; radius <= 10; ++radius) {
                for (auto dx = -radius; dx <= radius; ++dx) {
                    for (const auto dy : {-radius, radius}) {
                        const auto accepted = considerFallback(poweredAnchor + TilePosition(dx, dy));
                        if (accepted.isValid()) {
                            lastMacroStatus_ = "placement-powered-base-fallback";
                            return accepted;
                        }
                    }
                }
                for (auto dy = -radius + 1; dy < radius; ++dy) {
                    for (const auto dx : {-radius, radius}) {
                        const auto accepted = considerFallback(poweredAnchor + TilePosition(dx, dy));
                        if (accepted.isValid()) {
                            lastMacroStatus_ = "placement-powered-base-fallback";
                            return accepted;
                        }
                    }
                }
            }
        }
    }

    continuation.offset = search.nextOffset();
    if (continuation.offset != 0) {
        if (enemyFireFallback.isValid()) continuation.fireFallback = enemyFireFallback;
        if (miningLaneFallback.isValid()) continuation.laneFallback = miningLaneFallback;
        lastMacroStatus_ = "placement-search-deferred-" + std::to_string(continuation.offset);
        return TilePositions::None;
    }
    // A fallback discovered in an earlier window must survive the sweep, but
    // never its validation. Recheck power, occupancy, paths and current enemy
    // fire before accepting it; these are only two additional tile checks.
    const std::array rememberedFallbacks{continuation.fireFallback, continuation.laneFallback};
    placementSearches_.erase(taskKey);
    for (const auto location : rememberedFallbacks) {
        if (!location.isValid()) continue;
        const auto accepted = consider(location);
        if (accepted.isValid()) {
            lastMacroStatus_ = "placement-revalidated-fallback";
            return accepted;
        }
    }
    if (enemyFireFallback.isValid()) {
        lastMacroStatus_ = "placement-enemy-fire-fallback";
        return enemyFireFallback;
    }

    // A suboptimal mineral-side building is preferable to a permanent supply
    // block. This is reached only if the exhaustive safe search found no
    // legal alternative.
    if (miningLaneFallback.isValid()) {
        lastMacroStatus_ = "placement-lane-fallback";
        return miningLaneFallback;
    }
    lastMacroStatus_ = "placement-v" + std::to_string(validCandidates) +
                       "-p" + std::to_string(poweredCandidates) +
                       "-b" + std::to_string(buildableCandidates) +
                       "-r" + std::to_string(reachableCandidates) +
                       "-l" + std::to_string(laneCandidates);
    return TilePositions::None;
}

bool BwapiBridge::blocksMiningLane(
    const BWAPI::TilePosition tile,
    const BWAPI::UnitType type) const {
    if (!tile.isValid()) return true;
    const Position buildingCenter{
        tile.x * 32 + type.tileWidth() * 16,
        tile.y * 32 + type.tileHeight() * 16,
    };
    const auto clearance = std::max(type.tileWidth(), type.tileHeight()) * 16 + 40;
    for (const auto& site : resourceSites_) {
        if (!closeTo(site.depotCenter, buildingCenter, 640)) continue;
        for (const auto mineral : Broodwar->getStaticMinerals()) {
            const auto resource = fromBwapi(mineral->getInitialPosition());
            if (!closeTo(resource, site.resourceCenter, 352)) continue;
            if (closeTo(buildingCenter, resource, clearance)) return true;

            const auto segmentX = resource.x - site.depotCenter.x;
            const auto segmentY = resource.y - site.depotCenter.y;
            const auto lengthSquared = segmentX * segmentX + segmentY * segmentY;
            if (lengthSquared <= 0) continue;
            const auto projection = std::clamp(
                static_cast<double>((buildingCenter.x - site.depotCenter.x) * segmentX +
                                    (buildingCenter.y - site.depotCenter.y) * segmentY) /
                    static_cast<double>(lengthSquared),
                0.0, 1.0);
            const Position closest{
                site.depotCenter.x + static_cast<int>(std::lround(segmentX * projection)),
                site.depotCenter.y + static_cast<int>(std::lround(segmentY * projection)),
            };
            if (distanceSquared(buildingCenter, closest) <= clearance * clearance) return true;
        }
    }
    return false;
}

bool BwapiBridge::build(
    const MacroAction& action,
    const StrategicPlan& plan,
    const InfluenceMap& influence,
    const std::span<const UnitId> unavailableBuilders,
    NavigationGrid* navigation) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None || !type.isBuilding()) {
        lastMacroStatus_ = "build-invalid-type";
        return false;
    }
    const auto constructionSite = action.constructionSite.valid()
        ? action.constructionSite : ConstructionTaskSite{};
    const auto taskId = buildTaskKey(action.target, constructionSite);
    const auto armBuildLease = [](PendingBuild& pending, const BWAPI::Unit builder,
                                  const bool prepositioned) {
        const auto type = toBwapi(pending.kind);
        const BWAPI::Position center{
            pending.target.x + type.tileWidth() * 16,
            pending.target.y + type.tileHeight() * 16};
        const auto now = Broodwar->getFrameCount();
        pending.prepositioned = prepositioned;
        if (!prepositioned) {
            pending.routeWaypoint = {-1, -1};
            pending.travelWaypoint = {-1, -1};
        }
        pending.commandIssued = prepositioned ? -1 : now;
        pending.commandAcknowledged = -1;
        pending.phase = prepositioned ? BuildTaskPhase::prepositioning
                                      : BuildTaskPhase::commandPending;
        pending.lastPosition = fromBwapi(builder->getPosition());
        pending.lastRouteProgress = now;
        pending.footprintAccessible = !prepositioned;
        const auto speedMilli = static_cast<int>(std::lround(
            std::max(0.001, builder->getType().topSpeed()) * 1000.0));
        const auto initialDistance = builder->getDistance(center);
        pending.bestDistanceToTarget = static_cast<int>(initialDistance);
        pending.travelDeadline = now + buildTaskTravelDeadlineFrames(
            pending.kind, initialDistance, speedMilli,
            pending.plannedRemotePower);
        pending.hardTravelDeadline = now + buildTaskHardTravelDeadlineFrames(
            pending.kind, initialDistance, pending.plannedRemotePower);
    };
    const auto rememberPending = [this, action, constructionSite, armBuildLease,
                                  navigation, taskId](
                                    const BWAPI::Unit builder,
                                    const BWAPI::TilePosition location,
                                    const bool prepositioned,
                                    const bool resourcesPaid,
                                    const Position routeWaypoint,
                                    const Position travelWaypoint) {
        PendingBuild pending{};
        pending.taskId = taskId;
        pending.kind = action.target;
        pending.constructionSite = constructionSite;
        pending.resourcesPaid = resourcesPaid;
        pending.builder = builder->getID();
        pending.issued = Broodwar->getFrameCount();
        pending.target = fromBwapi(BWAPI::Position(location));
        pending.routeWaypoint = routeWaypoint;
        pending.travelWaypoint = travelWaypoint;
        pending.plannedRemotePower = action.target == UnitKind::pylon &&
            action.reason == "power the new PvZ natural before pressure";
        armBuildLease(pending, builder, prepositioned);
        pendingBuilds_.insert_or_assign(taskId, std::move(pending));
        if (navigation != nullptr) {
            const Position center{location.x * 32 + toBwapi(action.target).tileWidth() * 16,
                                  location.y * 32 + toBwapi(action.target).tileHeight() * 16};
            const auto type = toBwapi(action.target);
            navigation->updateDynamicObstacle(
                plannedNavigationObstacleId(taskId),
                {center.x - type.dimensionLeft(), center.y - type.dimensionUp(),
                 center.x + type.dimensionRight(), center.y + type.dimensionDown()});
        }
    };
    const auto pending = pendingBuilds_.find(taskId);
    if (pending != pendingBuilds_.end()) {
        if (pending->second.cancellation.awaiting()) {
            lastMacroStatus_ = "build-cancel-awaiting-ack";
            return false;
        }
            pending->second.unsafeRoute = false;
        const auto leasedBuilder = Broodwar->getUnit(pending->second.builder);
        if (leasedBuilder != nullptr && leasedBuilder->exists()) {
            const auto last = leasedBuilder->getLastCommand();
            const auto builderPosition = fromBwapi(leasedBuilder->getPosition());
            const auto activeWaypoint = pending->second.travelWaypoint;
            const auto progressingOnWaypoint = pending->second.prepositioned &&
                activeWaypoint.valid() &&
                distanceSquared(builderPosition, activeWaypoint) > 96 * 96 &&
                last.getType() == BWAPI::UnitCommandTypes::Move &&
                leasedBuilder->getOrder() == BWAPI::Orders::Move &&
                distanceSquared(fromBwapi(last.getTargetPosition()), activeWaypoint) <= 24 * 24 &&
                pending->second.lastRouteProgress >= 0 &&
                Broodwar->getFrameCount() - pending->second.lastRouteProgress <= 2 * 24;
            if (progressingOnWaypoint &&
                influence.maximumGroundThreat(builderPosition, activeWaypoint) <= 0.25F) {
                pending->second.unsafeRoute = false;
                lastMacroStatus_ = "build-builder-travelling";
                return false;
            }
            const auto sameBuild = last.getType() == BWAPI::UnitCommandTypes::Build &&
                last.getUnitType() == type && fromBwapi(last.getTargetPosition()) == pending->second.target;
            if (!leasedBuilder->isInterruptible() || leasedBuilder->isConstructing() ||
                leasedBuilder->getLastCommandFrame() + std::max(6, Broodwar->getLatencyFrames()) >=
                    Broodwar->getFrameCount() ||
                (sameBuild && leasedBuilder->getBuildType() == type)) {
                lastMacroStatus_ = "build-command-in-progress";
                return false;
            }
        }
        const Position routeTarget{
            pending->second.target.x + type.tileWidth() * 16,
            pending->second.target.y + type.tileHeight() * 16,
        };
        auto routeDestination = routeTarget;
        auto builderRoute = RouteAlternative{};
        if (leasedBuilder != nullptr && leasedBuilder->exists()) {
            const auto unitType = leasedBuilder->getType();
            const auto builderPosition = fromBwapi(leasedBuilder->getPosition());
            const MovementFootprint footprint{
                unitType.dimensionLeft(), unitType.dimensionRight(),
                unitType.dimensionUp(), unitType.dimensionDown()};
            if (pending->second.prepositioned &&
                pending->second.routeWaypoint.valid() &&
                distanceSquared(builderPosition, pending->second.routeWaypoint) <= 96 * 96) {
                pending->second.routeWaypoint = {-1, -1};
                pending->second.travelWaypoint = {-1, -1};
            }
            if (navigation != nullptr && !navigation->empty()) {
                const auto access = nearestGroundConstructionAccessPoint(
                    navigation, builderPosition, routeTarget,
                    placementFootprint(type), footprint);
                if (access.valid()) {
                    routeDestination = access;
                } else {
                    const auto fallback = navigation->nearestReachable(
                        builderPosition, routeTarget, 12000, footprint);
                    if (fallback.hasUsableWaypoint()) routeDestination = fallback.waypoint;
                }
            }
            builderRoute = selectSafeRouteAlternative(
                influence, builderPosition, routeDestination,
                false, navigation, footprint, 0.4125,
                pending->second.routeWaypoint, 0.25);
            const auto routeUnsafe = !builderRoute.profile.reachable ||
                builderRoute.profile.peakThreat > 0.25;
            const auto builderDistance =
                leasedBuilder->getDistance(toBwapiPosition(routeTarget));
            pending->second.unsafeRoute = action.target == UnitKind::nexus &&
                routeUnsafe && (builderDistance > 256 ||
                    (pending->second.prepositioned && builderDistance > 96));
            if (builderRoute.profile.reachable && builderRoute.profile.peakThreat <= 0.25) {
                if (!pending->second.prepositioned ||
                    !pending->second.routeWaypoint.valid())
                    pending->second.routeWaypoint = builderRoute.waypoint;
            } else if (!pending->second.prepositioned) {
                pending->second.routeWaypoint = {-1, -1};
            }
        }
        if (action.target == UnitKind::nexus && leasedBuilder != nullptr && leasedBuilder->exists() &&
            leasedBuilder->getDistance(toBwapiPosition(routeTarget)) > 256 &&
            (!builderRoute.profile.reachable || builderRoute.profile.peakThreat > 0.25)) {
            const auto proposed = requestBuildCancellation(
                pending->second, leasedBuilder, "build-route-danger");
            lastMacroStatus_ = pending->second.cancellation.awaiting() ?
                "build-route-danger-awaiting-ack" : proposed
                    ? "build-route-danger-cancel-proposed"
                    : "build-route-danger-cancel-rejected";
            return false;
        }
        if (pending->second.prepositioned && leasedBuilder != nullptr &&
            leasedBuilder->exists() &&
            leasedBuilder->getDistance(toBwapiPosition(routeTarget)) > 96) {
            if (!builderRoute.profile.reachable || builderRoute.profile.peakThreat > 0.25) {
                const auto proposed = requestBuildCancellation(
                    pending->second, leasedBuilder, "build-route-danger");
                lastMacroStatus_ = pending->second.cancellation.awaiting()
                    ? "build-route-danger-awaiting-ack" : proposed
                        ? "build-route-danger-cancel-proposed"
                        : "build-route-danger-cancel-rejected";
                return false;
            }
            const auto builderPosition = fromBwapi(leasedBuilder->getPosition());
            const auto holdingTravelWaypoint = pending->second.travelWaypoint.valid() &&
                distanceSquared(builderPosition, pending->second.travelWaypoint) > 96 * 96;
            const auto legDestination = pending->second.routeWaypoint.valid()
                ? pending->second.routeWaypoint : routeDestination;
            const MovementFootprint builderFootprint{
                leasedBuilder->getType().dimensionLeft(),
                leasedBuilder->getType().dimensionRight(),
                leasedBuilder->getType().dimensionUp(),
                leasedBuilder->getType().dimensionDown()};
            const auto stagingTarget = holdingTravelWaypoint
                ? pending->second.travelWaypoint
                : nextGroundRouteWaypoint(navigation, builderPosition, legDestination,
                                          builderFootprint);
            if (!stagingTarget.valid()) {
                lastMacroStatus_ = "build-route-waypoint-unavailable";
                return false;
            }
            pending->second.travelWaypoint = stagingTarget;
            const auto last = leasedBuilder->getLastCommand();
            const auto alreadyMovingThere = activeConstructionWaypointOrder(
                last.getType() == BWAPI::UnitCommandTypes::Move,
                leasedBuilder->getOrder() == BWAPI::Orders::Move,
                fromBwapi(last.getTargetPosition()), stagingTarget);
            if (!alreadyMovingThere &&
                leasedBuilder->getLastCommandFrame() + std::max(6, Broodwar->getLatencyFrames()) <
                    Broodwar->getFrameCount()) {
                const auto builderId = leasedBuilder->getID();
                const auto accepted = executeConstructionCommand(
                    leasedBuilder->getID(), CommandType::move, action.target,
                    stagingTarget, action.priority,
                    pending->second.routeWaypoint.valid() ||
                            stagingTarget != routeDestination
                        ? "build-route-detour" : "build-preposition", nullptr,
                    [this, taskId, builderId, stagingTarget,
                     routeWaypoint = pending->second.routeWaypoint](
                        const bool issued, const bool) {
                        if (!issued) return;
                        const auto found = pendingBuilds_.find(taskId);
                        if (found != pendingBuilds_.end() && found->second.builder == builderId) {
                            found->second.routeWaypoint = routeWaypoint;
                            found->second.travelWaypoint = stagingTarget;
                        }
                    });
                if (accepted && !constructionCommandQueuedThisAction_)
                    pending->second.travelWaypoint = stagingTarget;
            }
            lastMacroStatus_ = pending->second.routeWaypoint.valid() ||
                    stagingTarget != routeDestination
                ? "build-route-detour" : "build-builder-travelling";
            return false;
        }
        // Expansions may be pre-positioned into unexplored fog before BWAPI
        // accepts the build command.  The old guard treated that pending
        // lease as terminal: every later macro pass returned here, so the
        // Probe could stand on the natural for 45 seconds and the Nexus was
        // never actually issued.  Retry the exact reserved tile once the
        // worker arrives and the footprint is explored.
        if (action.target == UnitKind::nexus) {
            const auto builder = Broodwar->getUnit(pending->second.builder);
            const auto targetTile = BWAPI::TilePosition(
                pending->second.target.x / 32,
                pending->second.target.y / 32);
            const BWAPI::Position center{
                pending->second.target.x + type.tileWidth() * 16,
                pending->second.target.y + type.tileHeight() * 16,
            };
            if (builder == nullptr || !builder->exists()) {
                lastMacroStatus_ = "nexus-builder-missing";
                return false;
            }
            if (builder->getDistance(center) > 96) {
                lastMacroStatus_ = "nexus-builder-travelling";
                return false;
            }
            if (builder->getLastCommandFrame() + std::max(6, Broodwar->getLatencyFrames()) >=
                Broodwar->getFrameCount()) {
                lastMacroStatus_ = "nexus-command-latency";
                return false;
            }
            if (!Broodwar->canBuildHere(targetTile, type, builder, true)) {
                lastMacroStatus_ = "nexus-footprint-blocked";
                return false;
            }
            if (builder != nullptr && builder->exists() && builder->isCompleted() &&
                builder->getDistance(center) <= 96 &&
                Broodwar->canBuildHere(targetTile, type, builder, true) &&
                executeConstructionCommand(builder->getID(), CommandType::build,
                    action.target, {targetTile.x * 32, targetTile.y * 32},
                    action.priority, action.reason, nullptr,
                    [this, taskId, builderId = builder->getID(), armBuildLease](
                        const bool issued, const bool paid) {
                        if (!issued) return;
                        const auto found = pendingBuilds_.find(taskId);
                        const auto worker = Broodwar->getUnit(builderId);
                        if (found == pendingBuilds_.end() || worker == nullptr || !worker->exists())
                            return;
                        found->second.resourcesPaid = found->second.resourcesPaid || paid;
                        armBuildLease(found->second, worker, false);
                    })) {
                lastMacroStatus_ = constructionCommandQueuedThisAction_
                    ? "build-command-proposed-Nexus" : "issued-pending-Nexus";
                return true;
            }
        } else {
            // A Protoss build command can be acknowledged while the Probe is
            // still walking to the footprint.  The old lease only retried
            // Nexus placement, so a Forge/Core/Gateway that missed that
            // transient command stayed "pending" until the long lease timed
            // out and silently blocked the strategic checkpoint.  Retry the
            // exact tile as soon as the leased Probe arrives; the lifecycle
            // code still owns the worker until construction is observed.
            const auto builder = Broodwar->getUnit(pending->second.builder);
            const auto targetTile = BWAPI::TilePosition(
                pending->second.target.x / 32,
                pending->second.target.y / 32);
            const BWAPI::Position center{
                targetTile.x * 32 + type.tileWidth() * 16,
                targetTile.y * 32 + type.tileHeight() * 16,
            };
            if (builder != nullptr && builder->exists() && builder->isCompleted() &&
                builder->getDistance(center) <= 96 &&
                Broodwar->canBuildHere(targetTile, type, builder, true) &&
                executeConstructionCommand(builder->getID(), CommandType::build,
                    action.target, {targetTile.x * 32, targetTile.y * 32},
                    action.priority, action.reason, nullptr,
                    [this, taskId, builderId = builder->getID(), armBuildLease](
                        const bool issued, const bool paid) {
                        if (!issued) return;
                        const auto found = pendingBuilds_.find(taskId);
                        const auto worker = Broodwar->getUnit(builderId);
                        if (found == pendingBuilds_.end() || worker == nullptr || !worker->exists())
                            return;
                        found->second.resourcesPaid = found->second.resourcesPaid || paid;
                        armBuildLease(found->second, worker, false);
                    })) {
                lastMacroStatus_ = constructionCommandQueuedThisAction_
                    ? "build-command-proposed-" + std::string(unitStats(action.target).name)
                    : "issued-pending-" + std::string(unitStats(action.target).name);
                return true;
            }
        }
        lastMacroStatus_ = "build-pending";
        return false;
    }
    auto home = BWAPI::Position(Broodwar->self()->getStartLocation());
    if (usesHomeConstructionAnchor(action.target)) {
        const auto homeNexus = Broodwar->getClosestUnit(home,
            Filter::IsOwned && Filter::IsCompleted &&
                Filter::GetType == UnitTypes::Protoss_Nexus);
        if (homeNexus != nullptr) home = homeNexus->getPosition();
    }
    // Builder route checks must use the home placement anchor, not an unsafe
    // forward rally which the placement search will never use for this tech.
    const auto near = toBwapiPosition(constructionSite.valid()
        ? constructionSite.anchor
        : constructionBuilderAnchor(action.target, fromBwapi(home), plan.rallyPoint,
                                    plan.expansionTarget));
    auto rejectedForUnsafeRoute = false;
    const auto builder = findBuilder(type, near, influence, unavailableBuilders, true,
                                     &rejectedForUnsafeRoute);
    if (builder == nullptr) {
        lastMacroStatus_ = rejectedForUnsafeRoute && action.target == UnitKind::nexus
            ? "build-route-danger-no-safe-builder" : "build-no-builder";
        return false;
    }
    if (!Broodwar->canMake(type, builder)) {
        lastMacroStatus_ = "build-cannot-make";
        return false;
    }
    const auto location = buildLocation(action.target, type, builder, plan,
                                        constructionSite, taskId, action.reason);
    if (!location.isValid()) {
        lastMacroStatus_ = "build-no-location-" + lastMacroStatus_;
        return false;
    }
    placementSearches_.erase(taskId);
    if (type.requiresPsi() && !Broodwar->hasPower(location, type)) {
        lastMacroStatus_ = "build-unpowered-location";
        return false;
    }
    const MovementFootprint builderFootprint{
        builder->getType().dimensionLeft(), builder->getType().dimensionRight(),
        builder->getType().dimensionUp(), builder->getType().dimensionDown()};
    const BWAPI::Position buildCenterBw{
        location.x * 32 + type.tileWidth() * 16,
        location.y * 32 + type.tileHeight() * 16,
    };
    const Position buildCenter{buildCenterBw.x, buildCenterBw.y};
    auto routeDestination = buildCenter;
    if (navigation != nullptr && !navigation->empty()) {
        const auto access = nearestGroundConstructionAccessPoint(
            navigation, fromBwapi(builder->getPosition()), buildCenter,
            placementFootprint(type), builderFootprint);
        if (access.valid()) {
            routeDestination = access;
        } else {
            const auto fallback = navigation->nearestReachable(
                fromBwapi(builder->getPosition()), buildCenter, 12000, builderFootprint);
            if (fallback.hasUsableWaypoint()) routeDestination = fallback.waypoint;
        }
    }
    const auto builderRoute = selectSafeRouteAlternative(
        influence, fromBwapi(builder->getPosition()), routeDestination,
        false, navigation, builderFootprint, 0.4125, {-1, -1}, 0.25);
    const auto builderPosition = fromBwapi(builder->getPosition());
    const auto distantBuilder = builder->getDistance(buildCenterBw) > 256;
    const auto routeNeedsStaging = distantBuilder &&
        (action.target == UnitKind::nexus || builderRoute.waypoint.valid() ||
         (navigation != nullptr &&
          !navigation->lineWalkable(builderPosition, routeDestination, builderFootprint)));
    if (routeNeedsStaging &&
        (!builderRoute.profile.reachable || builderRoute.profile.peakThreat > 0.25)) {
        lastMacroStatus_ = "build-route-danger-no-safe-detour";
        return false;
    }
    if (routeNeedsStaging) {
        const auto legDestination = builderRoute.waypoint.valid()
            ? builderRoute.waypoint : routeDestination;
        const auto travelWaypoint = nextGroundRouteWaypoint(
            navigation, builderPosition, legDestination, builderFootprint);
        if (!travelWaypoint.valid()) {
            lastMacroStatus_ = "build-route-waypoint-unavailable";
            return false;
        }
        auto resourcesPaid = false;
        const auto moveAccepted = executeConstructionCommand(
            builder->getID(), CommandType::move, action.target, travelWaypoint,
            action.priority, builderRoute.waypoint.valid() ||
                    travelWaypoint != routeDestination
                ? "build-route-detour" : "build-travel-waypoint",
            &resourcesPaid,
            [rememberPending, builderId = builder->getID(), location,
             safeWaypoint = builderRoute.waypoint, travelWaypoint](
                const bool issued, const bool paid) {
                if (!issued) return;
                const auto worker = Broodwar->getUnit(builderId);
                if (worker != nullptr && worker->exists())
                    rememberPending(worker, location, true, paid, safeWaypoint,
                                    travelWaypoint);
            });
        if (!moveAccepted) {
            lastMacroStatus_ = constructionCommandQueuedThisAction_
                ? "build-route-waypoint-proposed"
                : "build-route-waypoint-rejected-" + lastIssueError_.toString();
            return false;
        }
        lastMacroStatus_ = constructionCommandQueuedThisAction_
            ? "build-route-waypoint-proposed"
            : builderRoute.waypoint.valid() || travelWaypoint != routeDestination
                ? "build-route-detour" : "build-builder-travelling";
        return false;
    }
    // Match Unit::build's command-time validation; otherwise placement can
    // succeed in unexplored fog and fail forever when the command is issued.
    if (!Broodwar->canBuildHere(location, type, builder, true)) {
        // Expansion footprints are commonly still in fog. BWAPI refuses a
        // build command until every footprint tile is explored, so reserve
        // the worker and reveal the location first; subsequent macro passes
        // will issue the Nexus as soon as the footprint becomes commandable.
        if (action.target == UnitKind::nexus &&
            Broodwar->canBuildHere(location, type, builder, false)) {
            const auto stagingTarget = builderRoute.waypoint.valid()
                ? builderRoute.waypoint : routeDestination;
            if (executeConstructionCommand(builder->getID(), CommandType::move,
                    action.target,
                    stagingTarget,
                    action.priority,
                    builderRoute.waypoint.valid() ? "build-route-detour" : "build-preposition",
                    nullptr,
                    [rememberPending, builderId = builder->getID(), location,
                      waypoint = builderRoute.waypoint,
                      travelWaypoint = stagingTarget](
                        const bool issued, const bool paid) {
                        if (!issued) return;
                        const auto worker = Broodwar->getUnit(builderId);
                        if (worker != nullptr && worker->exists())
                             rememberPending(worker, location, true, paid, waypoint,
                                             travelWaypoint);
                    })) {
                lastMacroStatus_ = constructionCommandQueuedThisAction_
                    ? "build-preposition-Nexus-proposed" : "build-preposition-Nexus";
                return false;
            }
        }
        lastMacroStatus_ = "build-location-rejected";
        return false;
    }
    if (buildSelectionDiagnostic && Broodwar->getFrameCount() <= 12'000 &&
        (action.target == UnitKind::pylon || action.target == UnitKind::cyberneticsCore)) {
        try {
            const BWAPI::Position center{
                location.x * 32 + type.tileWidth() * 16,
                location.y * 32 + type.tileHeight() * 16};
            const auto siteCandidate = findBuilder(type, center, influence, unavailableBuilders);
            buildSelectionDiagnostic({
                action.target, Broodwar->getFrameCount(), builder->getID(),
                siteCandidate != nullptr ? siteCandidate->getID() : -1,
                fromBwapi(BWAPI::Position(location)), fromBwapi(near),
                builder->getDistance(center),
                siteCandidate != nullptr ? siteCandidate->getDistance(center) : -1,
                siteCandidate != nullptr && Broodwar->canBuildHere(location, type, siteCandidate, true),
                siteCandidate != nullptr && siteCandidate->hasPath(center),
            });
        } catch (...) { ++diagnosticErrors_; }
    }
    auto resourcesPaid = false;
    if (executeConstructionCommand(builder->getID(), CommandType::build,
            action.target, {location.x * 32, location.y * 32}, action.priority,
            action.reason, &resourcesPaid,
            [rememberPending, builderId = builder->getID(), location](
                const bool issued, const bool paid) {
                if (!issued) return;
                const auto worker = Broodwar->getUnit(builderId);
                if (worker != nullptr && worker->exists())
                     rememberPending(worker, location, false, paid, {-1, -1},
                                     {-1, -1});
            })) {
        if (constructionCommandQueuedThisAction_)
            lastMacroStatus_ = "build-command-proposed";
        return true;
    }
    // Only placement failures invalidate terrain. A transient worker/resource
    // rejection says nothing about the footprint and must not scatter the base.
    if (lastIssueError_ == Errors::Unbuildable_Location) {
        failedBuildSites_.push_back({
            action.target, fromBwapi(BWAPI::Position(location)),
            Broodwar->getFrameCount() + 20 * 24,
        });
    }
    lastMacroStatus_ = "build-command-rejected-" + lastIssueError_.toString() +
                       "-u" + std::to_string(builder->getID()) + "-at" +
                       std::to_string(builder->getTilePosition().x) + "x" +
                       std::to_string(builder->getTilePosition().y) + "-to" +
                       std::to_string(location.x) + "x" + std::to_string(location.y);
    return false;
}

bool BwapiBridge::train(const MacroAction& action) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None) {
        lastMacroStatus_ = "train-invalid-type";
        return false;
    }
    const auto producerType = type.whatBuilds().first;
    std::vector<TrainingProducerCandidate> candidates;
    for (const auto producer : Broodwar->self()->getUnits()) {
        if (producer == nullptr || !producer->exists() ||
            producer->getType() != producerType) continue;
        const auto lastCommand = producer != nullptr ? producer->getLastCommand()
                                                     : BWAPI::UnitCommand{};
        const auto recentTrainingCommand =
            lastCommand.getType() == BWAPI::UnitCommandTypes::Train &&
            producer->getLastCommandFrame() +
                    std::max(1, Broodwar->getLatencyFrames()) >=
                Broodwar->getFrameCount();
        const auto queue = producer->getTrainingQueue();
        candidates.push_back({
            producer->getID(), producer->isCompleted(), producer->isPowered(),
            producer->isLockedDown() || producer->isMaelstrommed() ||
                producer->isStasised(),
            producer->isLoaded(), producer->isHallucination(), producer->canTrain(type),
            producer->isTraining() || producer->getRemainingTrainTime() > 0,
            static_cast<int>(queue.size()), producer->getRemainingTrainTime(),
            Broodwar->getRemainingLatencyFrames(), recentTrainingCommand,
            producer->isResearching(), producer->isUpgrading(),
        });
    }
    auto foundCandidate = false;
    auto failedCanMake = false;
    std::string rejectedReason;
    while (const auto selectedId = selectTrainingProducer(candidates)) {
        foundCandidate = true;
        std::erase_if(candidates, [&selectedId](const TrainingProducerCandidate& candidate) {
            return candidate.id == *selectedId;
        });
        const auto selected = Broodwar->getUnit(*selectedId);
        if (selected == nullptr || !selected->exists()) continue;
        if (!Broodwar->canMake(type, selected)) {
            failedCanMake = true;
            continue;
        }
        if (issue(UnitCommand::train(selected, type), action.reason,
                  ResourceUse::committed)) return true;
        rejectedReason = lastIssueError_.toString();
        // BWAPI can observe a producer becoming busy between snapshot and
        // command issue. The shared ledger is charged only on acceptance, so
        // a busy/vanished actor can safely retry the next concrete producer.
        if (lastIssueError_ != Errors::Unit_Busy &&
            lastIssueError_ != Errors::Unit_Does_Not_Exist) break;
    }
    if (!rejectedReason.empty()) {
        lastMacroStatus_ = "train-command-rejected-" + rejectedReason;
    } else if (foundCandidate && failedCanMake) {
        lastMacroStatus_ = "train-cannot-make-" + std::string(unitStats(action.target).name);
    } else {
        lastMacroStatus_ = "train-no-idle-producer-" +
                           std::string(unitStats(action.target).name);
    }
    return false;
}

bool BwapiBridge::executeTechnology(const MacroAction& action) {
    const auto self = Broodwar->self();
    if (self == nullptr || action.technology == TechnologyKind::none) return false;

    const auto tech = toBwapiTech(action.technology);
    if (tech != TechTypes::None) {
        if (self->hasResearched(tech) || self->isResearching(tech)) return false;
        const auto producer = technologyProducer(self->getUnits(), [tech](const Unit candidate) {
            return candidate->getType() == tech.whatResearches() && candidate->canResearch(tech);
        });
        return producer != nullptr && issue(UnitCommand::research(producer, tech), action.reason,
                                             ResourceUse::committed);
    }

    const auto upgrade = toBwapiUpgrade(action.technology);
    if (upgrade == UpgradeTypes::None || self->isUpgrading(upgrade) ||
        self->getUpgradeLevel(upgrade) >= upgrade.maxRepeats()) {
        return false;
    }
    const auto producer = technologyProducer(self->getUnits(), [upgrade](const Unit candidate) {
        return candidate->getType() == upgrade.whatUpgrades() && candidate->canUpgrade(upgrade);
    });
    return producer != nullptr && issue(UnitCommand::upgrade(producer, upgrade), action.reason,
                                         ResourceUse::committed);
}

BWAPI::Position BwapiBridge::toBwapiPosition(const Position position) noexcept {
    return {position.x, position.y};
}

UnitKind BwapiBridge::toKind(const BWAPI::UnitType type) noexcept {
    using namespace UnitTypes;
    if (type == Protoss_Probe) return UnitKind::probe;
    if (type == Protoss_Nexus) return UnitKind::nexus;
    if (type == Protoss_Pylon) return UnitKind::pylon;
    if (type == Protoss_Assimilator) return UnitKind::assimilator;
    if (type == Protoss_Gateway) return UnitKind::gateway;
    if (type == Protoss_Forge) return UnitKind::forge;
    if (type == Protoss_Photon_Cannon) return UnitKind::photonCannon;
    if (type == Protoss_Cybernetics_Core) return UnitKind::cyberneticsCore;
    if (type == Protoss_Shield_Battery) return UnitKind::shieldBattery;
    if (type == Protoss_Robotics_Facility) return UnitKind::roboticsFacility;
    if (type == Protoss_Observatory) return UnitKind::observatory;
    if (type == Protoss_Robotics_Support_Bay) return UnitKind::roboticsSupportBay;
    if (type == Protoss_Stargate) return UnitKind::stargate;
    if (type == Protoss_Citadel_of_Adun) return UnitKind::citadelOfAdun;
    if (type == Protoss_Templar_Archives) return UnitKind::templarArchives;
    if (type == Protoss_Fleet_Beacon) return UnitKind::fleetBeacon;
    if (type == Protoss_Arbiter_Tribunal) return UnitKind::arbiterTribunal;
    if (type == Protoss_Zealot) return UnitKind::zealot;
    if (type == Protoss_Dragoon) return UnitKind::dragoon;
    if (type == Protoss_High_Templar) return UnitKind::highTemplar;
    if (type == Protoss_Dark_Templar) return UnitKind::darkTemplar;
    if (type == Protoss_Archon) return UnitKind::archon;
    if (type == Protoss_Dark_Archon) return UnitKind::darkArchon;
    if (type == Protoss_Reaver) return UnitKind::reaver;
    if (type == Protoss_Observer) return UnitKind::observer;
    if (type == Protoss_Shuttle) return UnitKind::shuttle;
    if (type == Protoss_Scout) return UnitKind::scout;
    if (type == Protoss_Corsair) return UnitKind::corsair;
    if (type == Protoss_Carrier) return UnitKind::carrier;
    if (type == Protoss_Arbiter) return UnitKind::arbiter;
    if (type == Terran_SCV) return UnitKind::scv;
    if (type == Terran_Command_Center) return UnitKind::commandCenter;
    if (type == Terran_Supply_Depot) return UnitKind::supplyProvider;
    if (type == Terran_Refinery) return UnitKind::refinery;
    if (type == Terran_Barracks) return UnitKind::barracks;
    if (type == Terran_Factory) return UnitKind::factory;
    if (type == Terran_Starport) return UnitKind::starport;
    if (type == Terran_Bunker) return UnitKind::bunker;
    if (type == Terran_Missile_Turret) return UnitKind::missileTurret;
    if (type == Terran_Marine) return UnitKind::marine;
    if (type == Terran_Medic) return UnitKind::medic;
    if (type == Terran_Firebat) return UnitKind::firebat;
    if (type == Terran_Vulture) return UnitKind::vulture;
    if (type == Terran_Siege_Tank_Tank_Mode || type == Terran_Siege_Tank_Siege_Mode)
        return UnitKind::siegeTank;
    if (type == Terran_Goliath) return UnitKind::goliath;
    if (type == Terran_Wraith) return UnitKind::wraith;
    if (type == Terran_Science_Vessel) return UnitKind::scienceVessel;
    if (type == Terran_Dropship) return UnitKind::dropship;
    if (type == Terran_Battlecruiser) return UnitKind::battlecruiser;
    if (type == Terran_Ghost) return UnitKind::ghost;
    if (type == Terran_Valkyrie) return UnitKind::valkyrie;
    if (type == Terran_Vulture_Spider_Mine) return UnitKind::spiderMine;
    if (type == Terran_Academy) return UnitKind::academy;
    if (type == Terran_Engineering_Bay) return UnitKind::engineeringBay;
    if (type == Terran_Armory) return UnitKind::armory;
    if (type == Terran_Machine_Shop) return UnitKind::machineShop;
    if (type == Terran_Control_Tower) return UnitKind::controlTower;
    if (type == Terran_Science_Facility) return UnitKind::scienceFacility;
    if (type == Terran_Covert_Ops) return UnitKind::covertOps;
    if (type == Terran_Physics_Lab) return UnitKind::physicsLab;
    if (type == Terran_Comsat_Station) return UnitKind::comsatStation;
    if (type == Terran_Nuclear_Silo) return UnitKind::nuclearSilo;
    if (type == Zerg_Drone) return UnitKind::drone;
    if (type == Zerg_Hatchery) return UnitKind::hatchery;
    if (type == Zerg_Infested_Command_Center) return UnitKind::commandCenter;
    if (type == Zerg_Extractor) return UnitKind::refinery;
    if (type == Zerg_Lair) return UnitKind::lair;
    if (type == Zerg_Hive) return UnitKind::hive;
    if (type == Zerg_Spawning_Pool) return UnitKind::spawningPool;
    if (type == Zerg_Hydralisk_Den) return UnitKind::hydraliskDen;
    if (type == Zerg_Spire) return UnitKind::spire;
    if (type == Zerg_Greater_Spire) return UnitKind::greaterSpire;
    if (type == Zerg_Sunken_Colony) return UnitKind::sunkenColony;
    if (type == Zerg_Spore_Colony) return UnitKind::sporeColony;
    if (type == Zerg_Zergling) return UnitKind::zergling;
    if (type == Zerg_Hydralisk) return UnitKind::hydralisk;
    if (type == Zerg_Lurker) return UnitKind::lurker;
    if (type == Zerg_Mutalisk) return UnitKind::mutalisk;
    if (type == Zerg_Scourge) return UnitKind::scourge;
    if (type == Zerg_Ultralisk) return UnitKind::ultralisk;
    if (type == Zerg_Defiler) return UnitKind::defiler;
    if (type == Zerg_Overlord) return UnitKind::overlord;
    if (type == Zerg_Queen) return UnitKind::queen;
    if (type == Zerg_Guardian) return UnitKind::guardian;
    if (type == Zerg_Devourer) return UnitKind::devourer;
    if (type == Zerg_Broodling) return UnitKind::broodling;
    if (type == Zerg_Infested_Terran) return UnitKind::infestedTerran;
    if (type == Zerg_Creep_Colony) return UnitKind::creepColony;
    if (type == Zerg_Evolution_Chamber) return UnitKind::evolutionChamber;
    if (type == Zerg_Queens_Nest) return UnitKind::queensNest;
    if (type == Zerg_Ultralisk_Cavern) return UnitKind::ultraliskCavern;
    if (type == Zerg_Defiler_Mound) return UnitKind::defilerMound;
    if (type == Zerg_Nydus_Canal) return UnitKind::nydusCanal;
    if (type == Zerg_Lurker_Egg) return UnitKind::lurkerEgg;
    if (type == Zerg_Cocoon) return UnitKind::cocoon;
    return UnitKind::unknown;
}

BWAPI::UnitType BwapiBridge::toBwapi(const UnitKind kind) noexcept {
    using namespace UnitTypes;
    switch (kind) {
        case UnitKind::probe: return Protoss_Probe;
        case UnitKind::nexus: return Protoss_Nexus;
        case UnitKind::pylon: return Protoss_Pylon;
        case UnitKind::assimilator: return Protoss_Assimilator;
        case UnitKind::gateway: return Protoss_Gateway;
        case UnitKind::forge: return Protoss_Forge;
        case UnitKind::photonCannon: return Protoss_Photon_Cannon;
        case UnitKind::cyberneticsCore: return Protoss_Cybernetics_Core;
        case UnitKind::shieldBattery: return Protoss_Shield_Battery;
        case UnitKind::roboticsFacility: return Protoss_Robotics_Facility;
        case UnitKind::observatory: return Protoss_Observatory;
        case UnitKind::roboticsSupportBay: return Protoss_Robotics_Support_Bay;
        case UnitKind::stargate: return Protoss_Stargate;
        case UnitKind::citadelOfAdun: return Protoss_Citadel_of_Adun;
        case UnitKind::templarArchives: return Protoss_Templar_Archives;
        case UnitKind::fleetBeacon: return Protoss_Fleet_Beacon;
        case UnitKind::arbiterTribunal: return Protoss_Arbiter_Tribunal;
        case UnitKind::zealot: return Protoss_Zealot;
        case UnitKind::dragoon: return Protoss_Dragoon;
        case UnitKind::highTemplar: return Protoss_High_Templar;
        case UnitKind::darkTemplar: return Protoss_Dark_Templar;
        case UnitKind::archon: return Protoss_Archon;
        case UnitKind::darkArchon: return Protoss_Dark_Archon;
        case UnitKind::reaver: return Protoss_Reaver;
        case UnitKind::observer: return Protoss_Observer;
        case UnitKind::shuttle: return Protoss_Shuttle;
        case UnitKind::scout: return Protoss_Scout;
        case UnitKind::corsair: return Protoss_Corsair;
        case UnitKind::carrier: return Protoss_Carrier;
        case UnitKind::arbiter: return Protoss_Arbiter;
        default: return None;
    }
}

BWAPI::TechType BwapiBridge::toBwapiTech(const TechnologyKind kind) noexcept {
    using namespace TechTypes;
    switch (kind) {
        case TechnologyKind::psionicStorm: return Psionic_Storm;
        case TechnologyKind::stasisField: return Stasis_Field;
        case TechnologyKind::recall: return Recall;
        default: return None;
    }
}

BWAPI::UpgradeType BwapiBridge::toBwapiUpgrade(const TechnologyKind kind) noexcept {
    using namespace UpgradeTypes;
    switch (kind) {
        case TechnologyKind::singularityCharge: return Singularity_Charge;
        case TechnologyKind::legEnhancements: return Leg_Enhancements;
        case TechnologyKind::khaydarinAmulet: return Khaydarin_Amulet;
        case TechnologyKind::graviticDrive: return Gravitic_Drive;
        case TechnologyKind::graviticBoosters: return Gravitic_Boosters;
        case TechnologyKind::sensorArray: return Sensor_Array;
        case TechnologyKind::reaverCapacity: return Reaver_Capacity;
        case TechnologyKind::scarabDamage: return Scarab_Damage;
        case TechnologyKind::carrierCapacity: return Carrier_Capacity;
        case TechnologyKind::protossGroundWeapons: return Protoss_Ground_Weapons;
        case TechnologyKind::protossGroundArmor: return Protoss_Ground_Armor;
        case TechnologyKind::protossPlasmaShields: return Protoss_Plasma_Shields;
        case TechnologyKind::protossAirWeapons: return Protoss_Air_Weapons;
        case TechnologyKind::protossAirArmor: return Protoss_Air_Armor;
        default: return None;
    }
}

}  // namespace protodd::bwapi
