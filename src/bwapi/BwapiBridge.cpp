#include "BwapiBridge.hpp"
#include "ResourceDepotLocation.hpp"
#include "ObservedCloak.hpp"
#include "protodd/CommandEffectFeedback.hpp"
#include "TechnologyProducer.hpp"
#include "PlacementFootprint.hpp"

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
#include "protodd/WorkerCargoRecovery.hpp"

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

struct PlacementPhaseCost {
    std::uint64_t calls{};
    std::chrono::nanoseconds elapsed{};
};

class PlacementCostTimer {
public:
    explicit PlacementCostTimer(PlacementPhaseCost* cost) noexcept : cost_(cost) {
        if (cost_ != nullptr) started_ = std::chrono::steady_clock::now();
    }
    ~PlacementCostTimer() {
        if (cost_ == nullptr) return;
        ++cost_->calls;
        cost_->elapsed += std::chrono::steady_clock::now() - started_;
    }
private:
    PlacementPhaseCost* cost_{};
    std::chrono::steady_clock::time_point started_{};
};

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
        case BuildBlockerReason::routePending: return "route-pending";
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
    constructionTraffic_.clear();
    trafficGeneration_ = 0;
    activeNavigation_ = nullptr;
    buildRouteContextKnown_ = false;
    buildRouteTerrainIdentity_ = 0;
    buildRouteCollisionIdentity_ = 0;
    buildRouteThreatIdentity_ = 0;
    constructionRouteWork_.reset();
    placementExitWork_.reset();
    buildBlockers_.clear();
    expansionBuilderPreferences_.clear();
    failedBuildSites_.clear();
    placementSearches_.clear();
    placementAccessBaselines_.clear();
    placementAccessSourceKeys_.clear();
    placementAccessTemplates_.clear();
    placementAccessTerrain_ = placementAccessCollision_ = 0;
    unitCommandLocks_.clear();
    learnedCommandLeases_.clear();
    activeTypedCommand_ = nullptr;
    workerCommandLeases_.clear();
    issuedCommandLeaseGenerations_.clear();
    frameCommandClaims_.clear();
    frameCommandBus_ = nullptr;
    constructionProposalFrame_ = -1;
    constructionCommandProposals_.clear();
    constructionCommandQueuedThisAction_ = false;
    enemyMainRouteSource_ = {-1, -1};
    baseTerrain_.reset();
    baseTerrainAnalysis_.reset();
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

void BwapiBridge::advanceConstructionRoutes(
    const Frame frame, const NavigationGrid& navigation) {
    std::vector<std::uint64_t> leasedOwners;
    leasedOwners.reserve(pendingBuilds_.size());
    for (const auto& [taskId,pending] : pendingBuilds_) {
        static_cast<void>(taskId);
        if (pending.builder >= 0) leasedOwners.push_back(static_cast<std::uint64_t>(pending.builder));
    }
    std::vector<std::uint64_t> inspectionOwners;
    inspectionOwners.reserve(expansionBuilderPreferences_.size());
    for (const auto& [taskId, preference] : expansionBuilderPreferences_) {
        static_cast<void>(taskId);
        const auto lifetime = selfUnitLifetimes_.find(preference.builder);
        if (preference.observedAt < 0 || frame < preference.observedAt ||
            frame - preference.observedAt >= 5 * 24 || lifetime == selfUnitLifetimes_.end() ||
            lifetime->second.firstSeen != preference.firstSeen) continue;
        const auto builder = Broodwar->getUnit(preference.builder);
        if (builder == nullptr || !builder->exists() || builder->getPlayer() != Broodwar->self() ||
            !builder->isCompleted() || builder->getType() != UnitTypes::Protoss_Probe ||
            builder->isConstructing() || builder->isTraining() || !builder->isInterruptible() ||
            learnedCommandLeases_.contains(preference.builder)) continue;
        inspectionOwners.push_back(static_cast<std::uint64_t>(preference.builder));
    }
    constructionRouteWork_.prioritizeOwners(leasedOwners);
    constructionRouteWork_.prioritizeInspectionOwners(inspectionOwners);
    constructionRouteWork_.beginFrame(navigation, frame);
}

void BwapiBridge::beginCommandBudget(const Frame frame,
                                     const std::size_t maximumCommands) noexcept {
    commandBudget_.beginFrame(frame, maximumCommands,
                              defaultUrgentCommandReserve(maximumCommands));
    dispatchingFrameCommand_ = false;
}

std::vector<ConstructionTrafficContext> BwapiBridge::constructionTrafficContexts() const {
    std::vector<ConstructionTrafficContext> result;
    for (const auto& [task, pending] : pendingBuilds_) {
        const auto lifetime = selfUnitLifetimes_.find(pending.builder);
        const auto builder = Broodwar->getUnit(pending.builder);
        const auto now = Broodwar->getFrameCount();
        if (!pending.prepositioned || pending.cancellation.awaiting() || pending.unsafeRoute ||
            pending.phase == BuildTaskPhase::constructing || pending.commandIssued >= 0 ||
            !pending.travelWaypoint.valid() || pending.hardTravelDeadline <= now ||
            pending.travelDeadline <= now || lifetime == selfUnitLifetimes_.end() ||
            builder == nullptr || !builder->exists() || builder->getPlayer() != Broodwar->self() ||
            builder->isLoaded() || builder->isConstructing() || builder->isUnderAttack() ||
            builder->isLockedDown() || builder->isStasised() || builder->isMaelstrommed() ||
            (lifetime->second.lastDamageFrame >= 0 && now - lifetime->second.lastDamageFrame <=
                std::max(8, Broodwar->getLatencyFrames() + 3)) ||
            !activeConstructionWaypointOrder(
                builder->getLastCommand().getType() == UnitCommandTypes::Move,
                builder->getOrder() == Orders::Move,
                fromBwapi(builder->getLastCommand().getTargetPosition()), pending.travelWaypoint) ||
            builder->getDistance(toBwapiPosition(pending.travelWaypoint)) <= 96) continue;
        result.push_back({task, pending.builder, lifetime->second.firstSeen,
                          pending.target, pending.travelWaypoint, pending.issued});
    }
    std::ranges::sort(result, {}, &ConstructionTrafficContext::task);
    return result;
}

bool BwapiBridge::trafficActorSafe(const UnitId id, const Frame firstSeen) const {
    const auto lifetime = selfUnitLifetimes_.find(id);
    const auto actor = Broodwar->getUnit(id);
    const auto frame = Broodwar->getFrameCount();
    if (lifetime == selfUnitLifetimes_.end() || lifetime->second.firstSeen != firstSeen ||
        actor == nullptr || !actor->exists() || actor->getPlayer() != Broodwar->self() ||
        !actor->isCompleted() || actor->isLoaded() || actor->isFlying() ||
        actor->isUnderAttack() || actor->isUnderStorm() || actor->isAttacking() ||
        actor->isAttackFrame() || actor->isStartingAttack() ||
        actor->getOrder() == Orders::AttackUnit || actor->getOrder() == Orders::AttackMove ||
        actor->isLockedDown() ||
        actor->isStasised() || actor->isMaelstrommed() || actor->isBurrowed() ||
        actor->getHitPoints() * 4 < actor->getType().maxHitPoints() * 3 ||
        (lifetime->second.lastDamageFrame >= 0 &&
         frame - lifetime->second.lastDamageFrame <= std::max(8, Broodwar->getLatencyFrames() + 3)) ||
        learnedCommandLeases_.contains(id) || unitCommandLocks_.contains(id)) return false;
    for (const auto enemy : Broodwar->enemy()->getUnits()) {
        if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
            !enemy->getPosition().isValid()) continue;
        const auto weapon = enemy->getType().groundWeapon();
        if (weapon != WeaponTypes::None && enemy->getDistance(actor) <= weapon.maxRange() + 96)
            return false;
    }
    return true;
}

void BwapiBridge::observeConstructionTraffic(const GameState& state) {
    const auto contexts = constructionTrafficContexts();
    std::erase_if(constructionTraffic_, [this](const auto& entry) {
        return !pendingBuilds_.contains(entry.first);
    });
    for (auto& [task, assistance] : constructionTraffic_) {
        static_cast<void>(task);
        const auto context = std::ranges::find(contexts, assistance.context);
        if (context == contexts.end() || !assistance.live(state.frame, *context)) {
            assistance.closed = true; continue;
        }
        for (auto& member : assistance.members) {
            const auto snapshot = std::ranges::find(state.self.units, member.actor, &UnitSnapshot::id);
            if (snapshot == state.self.units.end() || snapshot->firstSeen != member.firstSeen ||
                !constructionTrafficHelper(*snapshot) || !trafficActorSafe(member.actor, member.firstSeen)) {
                assistance.closed = true; break;
            }
            if (!member.acknowledged) continue;
            const auto native = Broodwar->getUnit(member.actor);
            const auto last = native->getLastCommand();
            if (last.getType() != UnitCommandTypes::Move ||
                fromBwapi(last.getTargetPosition()) != member.destination ||
                (native->getOrder() != Orders::Move &&
                 distanceSquared(snapshot->position, member.destination) > 24 * 24)) {
                assistance.closed = true; break;
            }
            const auto remaining = distance(snapshot->position, member.destination);
            if (remaining + 8 <= member.bestDistance) {
                member.bestDistance = remaining;
                assistance.lastProgress = state.frame;
                assistance.motionObserved = true;
            }
        }
    }
}

void BwapiBridge::prepareConstructionTraffic(
    const GameState& state, std::vector<Command>& combat,
    const InfluenceMap& influence, NavigationGrid& navigation) {
    const auto contexts = constructionTrafficContexts();
    const auto winner = [&combat](UnitId actor) -> Command* {
        Command* result = nullptr;
        for (auto& command : combat) if (command.actor == actor &&
            (result == nullptr || command.effectiveUrgency() >= result->effectiveUrgency())) result = &command;
        return result;
    };
    std::size_t inspected = 0;
    for (const auto& context : contexts) {
        if (++inspected > 2) break;
        const auto builder = std::ranges::find(state.self.units, context.builder, &UnitSnapshot::id);
        if (builder == state.self.units.end() || builder->recentlyDamaged ||
            builder->underAttack || builder->loaded || builder->disabled) continue;
        auto found = constructionTraffic_.find(context.task);
        if (found != constructionTraffic_.end() && found->second.context != context) {
            constructionTraffic_.erase(found); found = constructionTraffic_.end();
        }
        if (found == constructionTraffic_.end()) {
            const auto pending = pendingBuilds_.find(context.task);
            if (pending == pendingBuilds_.end() || !constructionTrafficNeedsClearance(
                pending->second.waypointProgress, state.frame, context.waypoint)) continue;
            const auto group = constructionTrafficCluster(*builder, context.waypoint, state.self.units);
            if (group.empty()) continue;
            ConstructionTrafficAssistance next;
            next.context = context; next.started = next.lastProgress = next.lastEligible = state.frame;
            next.generation = ++trafficGeneration_;
            next.terrain = navigation.terrainIdentity(); next.collision = navigation.collisionIdentity();
            const UnitSnapshot* home = nullptr;
            for (const auto& unit : state.self.units) if (unit.completed && unit.role == UnitRole::resourceDepot &&
                (home == nullptr || distanceSquared(unit.position, builder->position) <
                                    distanceSquared(home->position, builder->position))) home = &unit;
            if (home == nullptr) continue;
            const auto anchor = moveToward(home->position, builder->position, 192);
            bool legal = true;
            for (std::size_t i = 0; i < group.size(); ++i) {
                const auto unit = std::ranges::find(state.self.units, group[i], &UnitSnapshot::id);
                auto* command = winner(group[i]);
                if (unit == state.self.units.end() || !constructionTrafficHelper(*unit) ||
                    command == nullptr || !constructionTrafficProposal(*command) ||
                    !trafficActorSafe(unit->id, unit->firstSeen)) { legal = false; break; }
                const MovementFootprint body{unit->dimensionLeft, unit->dimensionRight,
                                             unit->dimensionUp, unit->dimensionDown};
                const Position slot{anchor.x + (static_cast<int>(i % 3) - 1) * 64,
                                    anchor.y + (static_cast<int>(i / 3) - 1) * 64};
                const auto destination = navigation.nearestWalkable(slot, 32, body);
                if (!destination.valid() || distanceSquared(destination, context.waypoint) <= 96 * 96 ||
                    distanceSquared(destination, home->position) > 384 * 384 ||
                    distanceSquared(destination, unit->position) > 1024 * 1024 ||
                    influence.at(destination).groundThreat > 0.25F ||
                    std::ranges::any_of(next.members, [destination](const auto& member) {
                        return distanceSquared(member.destination, destination) < 48 * 48;
                    })) { legal = false; break; }
                next.members.push_back({unit->id, unit->firstSeen, destination,
                                        distance(unit->position, destination), false,
                                        command->effectiveUrgency()});
            }
            if (!legal) continue;
            found = constructionTraffic_.emplace(context.task, std::move(next)).first;
        }
        auto& assistance = found->second;
        if (assistance.closed || state.frame - assistance.started >= 288 ||
            assistance.terrain != navigation.terrainIdentity() ||
            assistance.collision != navigation.collisionIdentity()) {
            assistance.closed = true; continue;
        }
        bool legal = true, reached = true;
        for (auto& member : assistance.members) {
            const auto unit = std::ranges::find(state.self.units, member.actor, &UnitSnapshot::id);
            const auto* command = winner(member.actor);
            if (unit == state.self.units.end() || unit->firstSeen != member.firstSeen ||
                !constructionTrafficHelper(*unit) || command == nullptr ||
                !constructionTrafficProposal(*command) ||
                !trafficActorSafe(member.actor, member.firstSeen)) { legal = false; break; }
            member.urgency = command->effectiveUrgency();
            const MovementFootprint body{unit->dimensionLeft, unit->dimensionRight,
                                         unit->dimensionUp, unit->dimensionDown};
            const auto route = constructionRouteWork_.query(static_cast<std::uint64_t>(member.actor), navigation,
                unit->position, member.destination, body);
            if (!route.reached()) { reached = false; continue; }
            member.routeProved = state.frame;
            for (std::size_t i = 1; i < route.points.size(); ++i)
                if (influence.maximumGroundThreat(route.points[i-1], route.points[i]) > 0.25F)
                    legal = false;
        }
        if (!legal) { assistance.closed = true; continue; }
        assistance.lastEligible = state.frame;
        if (!reached) {
            // Unknown renewed geometry grants no new API move. Keep a previously
            // acknowledged native order in arbitration while its current safety
            // and bounded transaction remain valid; do not replace it with a
            // routine return proposal solely because a search is pending.
            for (const auto& member : assistance.members) {
                const auto native = Broodwar->getUnit(member.actor);
                if (!member.acknowledged || native == nullptr || !native->exists() ||
                    native->getOrder() != Orders::Move ||
                    native->getLastCommand().getType() != UnitCommandTypes::Move ||
                    fromBwapi(native->getLastCommand().getTargetPosition()) != member.destination ||
                    influence.maximumGroundThreat(fromBwapi(native->getPosition()), member.destination) > 0.25F)
                    continue;
                auto command = *winner(member.actor);
                command.targetPosition = member.destination;
                command.source = "construction-traffic-clearance";
                command.leaseGeneration = assistance.generation;
                command.deadlineFrame = state.frame;
                command.alreadyActive = true;
                std::erase_if(combat, [&member](const auto& old) { return old.actor == member.actor; });
                combat.push_back(std::move(command));
            }
            if (placementDiagnostic && state.frame % 24 == 0)
                placementDiagnostic("CONSTRUCTION_TRAFFIC_PENDING,frame=" + std::to_string(state.frame) +
                    ",task=" + std::to_string(context.task) + ",newMovesPermitted=0");
            continue;
        }
        for (const auto& member : assistance.members) {
            auto command = *winner(member.actor);
            command.targetPosition = member.destination;
            command.source = "construction-traffic-clearance";
            command.leaseGeneration = assistance.generation;
            command.deadlineFrame = state.frame;
            const auto native = Broodwar->getUnit(member.actor);
            command.alreadyActive = commandActive(command) || (member.acknowledged && native != nullptr &&
                native->getLastCommand().getType() == UnitCommandTypes::Move &&
                fromBwapi(native->getLastCommand().getTargetPosition()) == member.destination &&
                native->getDistance(toBwapiPosition(member.destination)) <= 24);
            std::erase_if(combat, [&member](const auto& old) { return old.actor == member.actor; });
            combat.push_back(std::move(command));
        }
        if (placementDiagnostic && state.frame % 24 == 0)
            placementDiagnostic("CONSTRUCTION_TRAFFIC,frame=" + std::to_string(state.frame) +
                ",task=" + std::to_string(context.task) + ",builder=" + std::to_string(context.builder) +
                ",helpers=" + std::to_string(assistance.members.size()) +
                ",acknowledged=" + std::to_string(assistance.acknowledged()));
    }
}

bool BwapiBridge::trafficCommandCurrent(const Command& command) const {
    const auto contexts = constructionTrafficContexts();
    for (const auto& [task, assistance] : constructionTraffic_) {
        static_cast<void>(task);
        if (assistance.closed || assistance.generation != command.leaseGeneration ||
            command.owner != CommandOwner::combat || command.type != CommandType::move ||
            std::ranges::find(contexts, assistance.context) == contexts.end() ||
            activeNavigation_ == nullptr || assistance.terrain != activeNavigation_->terrainIdentity() ||
            assistance.collision != activeNavigation_->collisionIdentity() ||
            !assistance.live(Broodwar->getFrameCount(), assistance.context)) continue;
        for (const auto& member : assistance.members)
            if (command.actor == member.actor && command.targetPosition == member.destination &&
                command.effectiveUrgency() == member.urgency &&
                member.routeProved == Broodwar->getFrameCount() &&
                trafficActorSafe(member.actor, member.firstSeen)) return true;
    }
    return false;
}

void BwapiBridge::acknowledgeTrafficCommand(const Command& command, const bool accepted) {
    if (command.source != "construction-traffic-clearance") return;
    for (auto& [task, assistance] : constructionTraffic_) {
        static_cast<void>(task);
        if (assistance.generation != command.leaseGeneration) continue;
        assistance.acknowledge(command.actor, command.targetPosition, accepted);
        if (placementDiagnostic) placementDiagnostic("CONSTRUCTION_TRAFFIC_ACK,frame=" +
            std::to_string(Broodwar->getFrameCount()) + ",task=" + std::to_string(assistance.context.task) +
            ",actor=" + std::to_string(command.actor) + ",accepted=" + std::to_string(accepted));
    }
}

bool BwapiBridge::trafficWaypointHold(
    const PendingBuild& pending, const InfluenceMap& influence, NavigationGrid* navigation) {
    const auto found = constructionTraffic_.find(pending.taskId);
    if (found == constructionTraffic_.end() || navigation == nullptr) return false;
    auto& assistance = found->second;
    const auto contexts = constructionTrafficContexts();
    const auto current = std::ranges::find(contexts, assistance.context);
    if (current == contexts.end() || !assistance.canHold(Broodwar->getFrameCount(), *current) ||
        assistance.terrain != navigation->terrainIdentity() ||
        assistance.collision != navigation->collisionIdentity()) return false;
    const auto builder = Broodwar->getUnit(pending.builder);
    if (builder == nullptr || !builder->exists()) return false;
    const auto type = builder->getType();
    const auto route = constructionRouteWork_.query(static_cast<std::uint64_t>(pending.builder), *navigation,
        fromBwapi(builder->getPosition()), pending.travelWaypoint,
        {type.dimensionLeft(), type.dimensionRight(), type.dimensionUp(), type.dimensionDown()});
    if (!route.reached()) return false;
    for (std::size_t i = 1; i < route.points.size(); ++i)
        if (influence.maximumGroundThreat(route.points[i-1], route.points[i]) > 0.25F) {
            assistance.closed = true; return false;
        }
    const auto frame = Broodwar->getFrameCount();
    if (placementDiagnostic && (assistance.lastHoldDiagnostic < 0 ||
        frame - assistance.lastHoldDiagnostic >= 24)) {
        assistance.lastHoldDiagnostic = frame;
        placementDiagnostic("CONSTRUCTION_TRAFFIC_HOLD,frame=" + std::to_string(frame) +
            ",task=" + std::to_string(pending.taskId) + ",builder=" + std::to_string(pending.builder) +
            ",waypointX=" + std::to_string(pending.travelWaypoint.x) +
            ",waypointY=" + std::to_string(pending.travelWaypoint.y) +
            ",lastGroupProgress=" + std::to_string(assistance.lastProgress) +
            ",expires=" + std::to_string(assistance.started + 288));
    }
    return true;
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
    if (proposal == constructionCommandProposals_.end()) {
        const auto accepted = execute(command, resourcesPaid);
        acknowledgeTrafficCommand(command, accepted);
        return accepted;
    }

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
            const auto destination = supplyConstructionHome(home->getPosition());
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
        return entry.second.expires <= frame;
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
        recordBuildWaypointProgress(
            pending.waypointProgress, frame, builderPosition, pending.travelWaypoint);
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
            if (builder->isConstructing() && builder->getBuildType() == type) {
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
        if (pending.cancellation.awaiting() && !actionable)
            return pending.cancellation.acknowledged(false, frame);
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
                    pending.taskId,
                });
            } catch (...) { ++diagnosticErrors_; }
        };
        if (pending.cancellation.awaiting()) {
            if (!requestCancellation(pending, builder, kind)) return false;
            if (pending.cancellationInvalidatesSite) rememberFailure();
            return true;
        }
        const auto nativeConstructionActive = builder->isConstructing() &&
            builder->getBuildType() == expectedType;
        if (constructionBuildOrderLost(
                frame, pending.commandIssued, Broodwar->getLatencyFrames(),
                nativeConstructionActive)) {
            reportLeaseEnd("native-build-order-lost");
            // Retire through the existing Stop acknowledgement path so a late
            // order cannot spend after its replacement. A lost order does not
            // establish that the footprint itself is illegal.
            pending.cancellationInvalidatesSite = false;
            if (!requestCancellation(pending, builder, kind)) return false;
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
            builder->getOrder() == BWAPI::Orders::Move &&
            lastCommand.getType() == BWAPI::UnitCommandTypes::Move &&
            (lastCommand.getTargetPosition() == targetCenter ||
             (pending.routeWaypoint.valid() &&
              distanceSquared(fromBwapi(lastCommand.getTargetPosition()),
                              pending.routeWaypoint) <= 96 * 96) ||
             (pending.travelWaypoint.valid() &&
              distanceSquared(fromBwapi(lastCommand.getTargetPosition()),
                              pending.travelWaypoint) <= 96 * 96));
        const auto stillAssigned = prepositionOrder || nativeConstructionActive;
        // Give the command time to cross the latency boundary, then recover
        // quickly if another subsystem or the game rejected the order. Keep a
        // genuinely travelling builder reserved long enough for expansions.
        if (commandAge <= std::max(12, Broodwar->getLatencyFrames() + 6)) return false;
        if (!stillAssigned) {
            reportLeaseEnd("order-lost");
            pending.cancellationInvalidatesSite = false;
            if (!requestCancellation(pending, builder, kind)) return false;
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
    if (!baseTerrain_ && frame >= 24) {
        baseTerrain_.emplace(navigationGrid());
        const Position ourStart{Broodwar->self()->getStartLocation().x * 32 + 64,
                                Broodwar->self()->getStartLocation().y * 32 + 48};
        std::vector<Position> approaches{{Broodwar->mapWidth() * 16, Broodwar->mapHeight() * 16}};
        for (const auto start : Broodwar->getStartLocations())
            approaches.push_back({start.x * 32 + 64, start.y * 32 + 48});
        std::vector<Position> sites;
        for (const auto& site : resourceSites_) sites.push_back(site.depotCenter);
        baseTerrainAnalysis_.initialize(ourStart, sites, approaches);
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
    observeConstructionTraffic(state);
    return state;
}

NavigationGrid BwapiBridge::navigationGrid() const {
    const auto tileWidth = Broodwar->mapWidth();
    const auto tileHeight = Broodwar->mapHeight();
    const auto width = tileWidth * 4;
    const auto height = tileHeight * 4;
    std::vector<std::uint8_t> cells(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    auto elevation = cells;
    std::vector<std::uint8_t> tileElevation(
        static_cast<std::size_t>(tileWidth) * static_cast<std::size_t>(tileHeight));
    for (int tileY = 0; tileY < tileHeight; ++tileY)
        for (int tileX = 0; tileX < tileWidth; ++tileX)
            tileElevation[static_cast<std::size_t>(tileY) * static_cast<std::size_t>(tileWidth) +
                          static_cast<std::size_t>(tileX)] = static_cast<std::uint8_t>(
                Broodwar->getGroundHeight(tileX, tileY) / 2);
    for (auto walkY = 0; walkY < height; ++walkY) {
        for (auto walkX = 0; walkX < width; ++walkX) {
            const auto index = static_cast<std::size_t>(walkY) *
                                   static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(walkX);
            cells[index] = Broodwar->isWalkable(walkX, walkY) ? 1U : 0U;
            elevation[index] = tileElevation[static_cast<std::size_t>(walkY / 4) *
                static_cast<std::size_t>(tileWidth) + static_cast<std::size_t>(walkX / 4)];
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

std::vector<ConstructionTaskSite> BwapiBridge::leasedExpansionSites() const {
    std::vector<ConstructionTaskSite> result;
    for (const auto& [taskId, pending] : pendingBuilds_) {
        static_cast<void>(taskId);
        if (pending.kind == UnitKind::nexus && pending.constructionSite.valid())
            result.push_back(pending.constructionSite);
    }
    // Cancellation requests retain their PendingBuild until observation
    // acknowledges the order ending. They still own the same allocation.
    std::ranges::sort(result, {}, &ConstructionTaskSite::id);
    return result;
}

void BwapiBridge::refreshBuildRouteContext(
    const InfluenceMap& influence, NavigationGrid& navigation) {
    activeNavigation_ = &navigation;
    const auto terrain = navigation.terrainIdentity();
    const auto collision = navigation.collisionIdentity();
    const auto threat = influence.groundThreatIdentity();
    const auto changed = buildRouteContextKnown_ &&
        (terrain != buildRouteTerrainIdentity_ || collision != buildRouteCollisionIdentity_ ||
         threat != buildRouteThreatIdentity_);
    buildRouteContextKnown_ = true;
    buildRouteTerrainIdentity_ = terrain;
    buildRouteCollisionIdentity_ = collision;
    buildRouteThreatIdentity_ = threat;
    if (!changed) return;

    const auto frame = Broodwar->getFrameCount();
    for (auto& [taskId, feedback] : buildBlockers_) {
        if (feedback.target != UnitKind::nexus ||
            feedback.reason != BuildBlockerReason::unsafeRoute ||
            pendingBuilds_.contains(taskId)) continue;
        // The previous unsafe verdict is no longer current. Keep spending
        // blocked as unknown while making bounded inspection due now. Only
        // the existing engine and body-route checks can retire this blocker.
        feedback.reason = BuildBlockerReason::routePending;
        feedback.retryAt = frame;
        feedback.observedAt = frame;
    }
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
        Broodwar->getFrameCount() + std::max<Frame>(1, retryFrames),
        Broodwar->getFrameCount()});
}

UnitId BwapiBridge::preferredExpansionBuilder(const MacroAction& action) const {
    if (action.target != UnitKind::nexus || !action.constructionSite.valid()) return -1;
    const auto found = expansionBuilderPreferences_.find(
        buildTaskKey(action.target, action.constructionSite));
    if (found == expansionBuilderPreferences_.end()) return -1;
    const auto& ready = found->second;
    const auto frame = Broodwar->getFrameCount();
    const auto lifetime = selfUnitLifetimes_.find(ready.builder);
    if (ready.observedAt < 0 || frame < ready.observedAt || frame - ready.observedAt >= 5 * 24 ||
        ready.anchor != action.constructionSite.anchor || lifetime == selfUnitLifetimes_.end() ||
        lifetime->second.firstSeen != ready.firstSeen) return -1;
    return ready.builder;
}

void BwapiBridge::rememberExpansionBuilderPreference(const MacroAction& action, const BWAPI::Unit builder) {
    if (action.target != UnitKind::nexus || !action.constructionSite.valid() || builder == nullptr) return;
    const auto lifetime = selfUnitLifetimes_.find(builder->getID());
    if (lifetime == selfUnitLifetimes_.end()) return;
    const auto taskId = buildTaskKey(action.target, action.constructionSite);
    if (!expansionBuilderPreferences_.contains(taskId) && expansionBuilderPreferences_.size() >= 128) {
        const auto oldest = std::ranges::min_element(expansionBuilderPreferences_, {},
            [](const auto& entry) { return std::pair{entry.second.observedAt, entry.first}; });
        expansionBuilderPreferences_.erase(oldest);
    }
    expansionBuilderPreferences_.insert_or_assign(taskId, ExpansionBuilderPreference{
        builder->getID(), action.constructionSite.anchor, Broodwar->getFrameCount(),
        lifetime->second.firstSeen});
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
        expansionBuilderPreferences_.erase(taskId);
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
    if (relocatableSupplyPylon(action.target, action.reason, constructionSite.valid()))
        home = supplyConstructionHome(home);
    if (action.target == UnitKind::assimilator) {
        home = constructionSite.valid() ? toBwapiPosition(constructionSite.anchor)
                                       : refineryConstructionHome(home, influence);
        if (!home.isValid()) {
            recordBuildBlocker(action, BuildBlockerReason::noPlacement, 30 * 24);
            return;
        }
    }
    auto near = toBwapiPosition(constructionSite.valid()
        ? constructionSite.anchor
        : constructionBuilderAnchor(action.target, fromBwapi(home), plan.constructionRallyPoint(),
                                     plan.expansionTarget));
    if (!constructionSite.valid() &&
        (action.target == UnitKind::photonCannon || action.target == UnitKind::shieldBattery)) {
        const auto context = staticDefenseConstructionContext(type, plan);
        if (context.base != nullptr) near = context.anchor;
    }
    // Feasibility inspection deliberately ignores affordability. It only
    // asks whether an otherwise eligible worker and legal footprint exist,
    // so an unaffordable blocked goal cannot protect the entire current bank.
    auto rejectedForUnsafeRoute = false;
    auto routePending = false;
    BWAPI::Unit pendingCandidate = nullptr;
    const auto builder = findBuilder(type, near, influence, unavailableBuilders, false,
                                     &rejectedForUnsafeRoute, &routePending,
                                     preferredExpansionBuilder(action), &pendingCandidate);
    if (builder == nullptr) {
        if (routePending && pendingCandidate != nullptr)
            rememberExpansionBuilderPreference(action, pendingCandidate);
        else if (action.target == UnitKind::nexus)
            expansionBuilderPreferences_.erase(taskId);
        if (routePending) {
            recordBuildBlocker(action, BuildBlockerReason::routePending, 3);
            return;
        }
        recordBuildBlocker(action,
            rejectedForUnsafeRoute && action.target == UnitKind::nexus
                ? BuildBlockerReason::unsafeRoute : BuildBlockerReason::noBuilder,
            15 * 24);
        return;
    }
    const auto location = buildLocation(action.target, type, builder, plan,
                                        constructionSite, taskId, action.reason, fromBwapi(home));
    if (!location.isValid()) {
        if (!lastMacroStatus_.starts_with("placement-search-deferred-"))
            recordBuildBlocker(action, BuildBlockerReason::noPlacement, 30 * 24);
        return;
    }
    if (action.target == UnitKind::nexus && !groundConstructionSiteSafe(influence,
            {location.x * 32 + type.tileWidth() * 16,
             location.y * 32 + type.tileHeight() * 16})) {
        recordBuildBlocker(action, BuildBlockerReason::unsafeRoute, 15 * 24);
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
            auto accessStatus = NavigationStatus::invalidInput;
            auto accessBudgetExhausted = false;
            routeDestination = nearestGroundConstructionAccessPoint(
                activeNavigation_, fromBwapi(builder->getPosition()), routeDestination,
                placementFootprint(type), builderFootprint, 96, &accessStatus, &accessBudgetExhausted,
                &constructionRouteWork_, static_cast<std::uint64_t>(builder->getID()));
            if (!routeDestination.valid()) {
                reportBuildRoute(builder, fromBwapi(center), routeDestination,
                    "footprint-access-unavailable", false, 0.0, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore,
                    accessStatus, false, accessBudgetExhausted);
                recordBuildBlocker(action, accessBudgetExhausted
                    ? BuildBlockerReason::routePending : BuildBlockerReason::unsafeRoute,
                    accessBudgetExhausted ? 3 : 15 * 24);
                return;
            }
        }
        const auto route = selectSafeRouteAlternative(
            influence, fromBwapi(builder->getPosition()), routeDestination,
            false, activeNavigation_, builderFootprint, 0.4125, {-1, -1}, 0.25,
            &constructionRouteWork_, static_cast<std::uint64_t>(builder->getID()));
        reportBuildRoute(builder, fromBwapi(center), routeDestination,
            "footprint-route", route.profile.reachable, route.profile.peakThreat, influence,
            NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore,
            route.profile.navigationStatus, route.unresolvedAlternatives,
            route.profile.searchBudgetExhausted);
        if (builder->getDistance(center) > 256 &&
            (!route.profile.reachable || route.profile.peakThreat > 0.25)) {
            const auto pendingRoute = route.profile.searchBudgetExhausted || route.unresolvedAlternatives;
            recordBuildBlocker(action, pendingRoute ? BuildBlockerReason::routePending
                                                   : BuildBlockerReason::unsafeRoute,
                               pendingRoute ? 3 : 15 * 24);
            return;
        }
    }
    buildBlockers_.erase(taskId);
    rememberExpansionBuilderPreference(action, builder);
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
    // All adapter paths, including raw production/learned actions, share this
    // accepted-call claim. A failed permission, budget, resource or BWAPI call
    // leaves the actor available for a valid retry in the same frame.
    const auto frame = Broodwar->getFrameCount();
    const auto actorId = command.getUnit()->getID();
    if (!frameCommandClaims_.available(frame, actorId)) {
        lastIssueError_ = BWAPI::Errors::Unit_Busy;
        try { if (actionDiagnostic) {
            const auto target = command.getTarget();
            actionDiagnostic({actorId, target ? target->getID() : -1,
                fromBwapi(command.getTargetPosition()), command.getType().toString(),
                std::string(source), "actor-frame-authority-already-claimed",
                command.extra, false, false});
        } } catch (...) { ++diagnosticErrors_; }
        return false;
    }
    if (source != "whole-game" && learnedCommandLeases_.contains(actorId) &&
        !(activeTypedCommand_ && activeTypedCommand_->actor == actorId &&
          canPreemptLearnedCommandLease(*activeTypedCommand_))) {
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
    if (accepted) frameCommandClaims_.recordAccepted(frame, actorId);
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
                                   const Frame leaseFrames, const int urgency) {
    if (leaseFrames < 1 || !command.getUnit()) return false;
    const auto accepted = issue(command, "whole-game");
    if (accepted) {
        const auto actor = command.getUnit()->getID();
        learnedCommandLeases_[actor] = {Broodwar->getFrameCount() + leaseFrames, urgency};
        auto& generation = issuedCommandLeaseGenerations_[actor][CommandOwner::learned];
        if (generation < std::numeric_limits<std::uint64_t>::max()) ++generation;
        invalidateCommandLease(actor, CommandOwner::learned, generation);
    }
    return accepted;
}

bool BwapiBridge::reject(const Command& command, const std::string_view reason) {
    try { if (actionDiagnostic) actionDiagnostic({command.actor, command.targetUnit,
        command.targetPosition, "intent-" + std::to_string(static_cast<int>(command.type)),
        command.source, std::string(reason), static_cast<int>(command.technology), false, false});
    } catch (...) { ++diagnosticErrors_; }
    return false;
}

bool BwapiBridge::claimConfirmedFrameAuthority(const Command& command,
                                              const bool apiAccepted) {
    if (!changesActorOrder(command.type) ||
        (!apiAccepted && (!command.alreadyActive || !commandActive(command)))) return false;
    // An active primary winner keeps authority even when no new API call is
    // needed. This consumes no command budget or resources and is not an
    // accepted-command diagnostic row.
    frameCommandClaims_.recordConfirmed(Broodwar->getFrameCount(), command.actor);
    return true;
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

    const auto previous = activeTypedCommand_;
    activeTypedCommand_ = &command;
    struct RestoreTypedCommand {
        const Command*& current;
        const Command* previous;
        ~RestoreTypedCommand() { current = previous; }
    } restore{activeTypedCommand_, previous};
    const auto accepted = executeUnchecked(command, resourcesPaid);
    if (accepted) {
        frameCommandClaims_.recordAccepted(frame, command.actor);
        // Frame dispatch handles confirmed release and its trace centrally.
        if (!dispatchingFrameCommand_)
            static_cast<void>(releaseLearnedCommandLease(command, true));
    }
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

    if (command.source == "construction-traffic-clearance" && !trafficCommandCurrent(command))
        return reject(command, "construction-traffic-stale-or-unsafe");

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
        const auto now = Broodwar->getFrameCount();
        const auto builder = Broodwar->getUnit(pending.builder);
        const auto type = toBwapi(pending.kind);
        const BWAPI::Position center{pending.target.x + type.tileWidth() * 16,
                                     pending.target.y + type.tileHeight() * 16};
        const BWAPI::TilePosition tile{pending.target.x / 32,pending.target.y / 32};
        auto readyAwaitingFunds = pending.prepositioned && !pending.resourcesPaid &&
            pending.commandIssued < 0 && pending.phase == BuildTaskPhase::prepositioning &&
            !pending.cancellation.awaiting() && !pending.unsafeRoute && !pending.routePending &&
            pending.footprintAccessible && now < pending.travelDeadline &&
            builder != nullptr && builder->exists() && builder->isCompleted() &&
            builder->getPlayer() == Broodwar->self() &&
            builder->getType() == BWAPI::UnitTypes::Protoss_Probe &&
            !builder->isLoaded() && builder->isInterruptible() &&
            !builder->isUnderAttack() && !builder->isConstructing() && !builder->isTraining() &&
            builder->getDistance(center) <= 96;
        if (readyAwaitingFunds) {
            const auto last = builder->getLastCommand();
            readyAwaitingFunds = last.getType() == BWAPI::UnitCommandTypes::Move &&
                distanceSquared(fromBwapi(last.getTargetPosition()),fromBwapi(center)) <= 96 * 96 &&
                Broodwar->canBuildHere(tile,type,builder,true);
            if (const auto enemy = Broodwar->enemy(); readyAwaitingFunds && enemy != nullptr)
                readyAwaitingFunds = !std::ranges::any_of(enemy->getUnits(),[builder](const Unit threat) {
                    if (threat == nullptr || !threat->exists() || !threat->isVisible() ||
                        !threat->isCompleted()) return false;
                    const auto weapon = threat->getType().groundWeapon();
                    return weapon != WeaponTypes::None &&
                        threat->getDistance(builder) <= weapon.maxRange() + 96;
                });
        }
        return {{pending.target.x + 64, pending.target.y + 48}, true,
            pending.lastRouteProgress >= 0 ? now - pending.lastRouteProgress : 0,
            false, false, false, pending.unsafeRoute, pending.routePending,
            found->first, now, pending.cancellation.awaiting(), readyAwaitingFunds, pending.builder};
    }
    const auto frame = Broodwar->getFrameCount();
    const auto actionable = [frame](const auto& entry) {
            const auto& blocker = entry.second;
            return blocker.target == UnitKind::nexus &&
                (blocker.reason == BuildBlockerReason::rejectedFootprint ||
                 blocker.reason == BuildBlockerReason::noBuilder ||
                 blocker.reason == BuildBlockerReason::unsafeRoute ||
                 blocker.reason == BuildBlockerReason::routePending ||
                 blocker.reason == BuildBlockerReason::noPlacement) &&
                (blocker.retryAt > frame || pendingRouteObservationCurrent(blocker, frame)) &&
                blocker.constructionSite.valid() &&
                blocker.constructionSite.anchor.valid();
        };
    const auto matching = std::ranges::find_if(buildBlockers_,
        [plannedSite, &actionable](const auto& entry) {
            return actionable(entry) &&
                distanceSquared(entry.second.constructionSite.anchor, plannedSite) <= 96 * 96;
        });
    const auto blocked = matching != buildBlockers_.end()
        ? matching : std::ranges::find_if(buildBlockers_,
            [plannedSite, &actionable](const auto& entry) {
                return actionable(entry) &&
                    (!plannedSite.valid() || entry.second.reason != BuildBlockerReason::routePending);
            });
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
                unsafeRoute, blocked->second.reason == BuildBlockerReason::routePending,
                blocked->first, blocked->second.observedAt, false};
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
        if (found->second.cancellation.awaiting() &&
            !found->second.cancellation.acknowledged(false, Broodwar->getFrameCount()))
            return false;
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
    std::erase_if(expansionBuilderPreferences_, [frame, actions](const auto& entry) {
        if (entry.second.observedAt < 0 || frame < entry.second.observedAt ||
            frame - entry.second.observedAt >= 5 * 24) return true;
        return std::ranges::none_of(actions, [&entry](const MacroAction& action) {
            return action.target == UnitKind::nexus && action.constructionSite.valid() &&
                buildTaskKey(action.target, action.constructionSite) == entry.first;
        });
    });
    std::erase_if(buildBlockers_, [frame](const auto& entry) {
        return entry.second.retryAt <= frame &&
            !pendingRouteObservationCurrent(entry.second, frame);
    });
    for (const auto& action : actions) {
        const auto actionStarted = std::chrono::steady_clock::now();
        const auto searchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
        const auto recordExecution = [&](std::string outcome, const bool accepted) {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - actionStarted).count();
            const auto searches = NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore;
            const auto taskId = action.action == MacroActionKind::build ||
                                action.action == MacroActionKind::expand
                ? buildTaskKey(action.target, action.constructionSite) : 0;
            macroExecutions_.push_back({action, std::move(outcome), accepted, elapsedUs, searches, taskId});
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
            const auto routeInspectionDue = [&]() {
                if (!construction) return false;
                const auto taskId = buildTaskKey(action.target, action.constructionSite);
                const auto blocker = buildBlockers_.find(taskId);
                return blocker != buildBlockers_.end() && blocker->second.retryAt <= frame &&
                    pendingRouteObservationCurrent(blocker->second, frame);
            }();
            if (action.blocksLowerPriority && construction &&
                (action.executable || routeInspectionDue)) {
                inspectUnfundedBuild(action, plan, influence, unavailableBuilders);
            }
            const auto preparationTaskId = construction
                ? buildTaskKey(action.target, action.constructionSite) : 0;
            if (action.target == UnitKind::nexus && construction &&
                action.preparationFundingFrames >= 0 &&
                action.preparationFundingFrames <= 30 * 24 &&
                action.blocksLowerPriority && action.constructionSite.valid() &&
                !plan.deferExpansion &&
                !buildBlockers_.contains(preparationTaskId)) {
                const auto remainingBefore = commandBudget_.remaining();
                static_cast<void>(build(action, plan, influence, unavailableBuilders,
                                        navigation, true));
                const auto proposed = constructionCommandQueuedThisAction_;
                if (proposed || commandBudget_.remaining() < remainingBefore) ++issued;
                if (lastMacroStatus_ == "build-route-pending")
                    recordBuildBlocker(action, BuildBlockerReason::routePending, 3);
                else if (lastMacroStatus_.starts_with("build-route-danger"))
                    recordBuildBlocker(action, BuildBlockerReason::unsafeRoute, 15 * 24);
                else if (lastMacroStatus_ == "build-no-builder")
                    recordBuildBlocker(action, BuildBlockerReason::noBuilder, 15 * 24);
                recordExecution(proposed ? "preparation-command-proposed/" + lastMacroStatus_
                                        : "preparation/" + lastMacroStatus_, false);
                continue;
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
            } else if (!success && lastMacroStatus_ == "build-route-pending") {
                reason = BuildBlockerReason::routePending;
                retryFrames = 3;
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
        if ((assignment.job == WorkerJob::minerals || assignment.job == WorkerJob::gas ||
             assignment.job == WorkerJob::transfer) &&
            (type == CommandType::gather || type == CommandType::returnCargo))
            command.activity = CommandActivity::workerEconomy;
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
                                      worker->isCarryingGas(),
                                  assignment.preserveNativeOrder});
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
        if (assignment.preserveNativeOrder) continue;
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
            const auto idle = worker->isIdle();
            if (shouldReturnWorkerCargo(assignment.job, true, idle,
                                       worker->getOrder() == Orders::AttackUnit)) {
                static_cast<void>(submitWorkerCommand(worker, assignment, CommandType::returnCargo,
                    -1, {-1, -1}, idle ? "worker-return-cargo" : "worker-cargo-handoff"));
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

void BwapiBridge::invalidateCommandLease(const UnitId actor, const CommandOwner owner,
                                          const std::uint64_t generation) {
    if (actor < 0 || owner == CommandOwner::unspecified || generation == 0) return;
    auto& latest = issuedCommandLeaseGenerations_[actor][owner];
    latest = std::max(latest, generation);
    if (frameCommandBus_ != nullptr)
        frameCommandBus_->invalidateLease(actor, owner, latest);
}

bool BwapiBridge::canPreemptLearnedCommandLease(const Command& winner) const noexcept {
    const auto lease = learnedCommandLeases_.find(winner.actor);
    return lease != learnedCommandLeases_.end() &&
        winner.owner != CommandOwner::learned &&
        winner.owner != CommandOwner::unspecified &&
        changesActorOrder(winner.type) &&
        winner.effectiveUrgency() > lease->second.urgency;
}

std::uint64_t BwapiBridge::releaseLearnedCommandLease(
    const Command& winner, const bool apiAccepted) {
    if (!canPreemptLearnedCommandLease(winner) ||
        (!apiAccepted && (!winner.alreadyActive || !commandActive(winner)))) return 0;
    learnedCommandLeases_.erase(winner.actor);
    auto& generation = issuedCommandLeaseGenerations_[winner.actor][CommandOwner::learned];
    if (generation < std::numeric_limits<std::uint64_t>::max()) ++generation;
    invalidateCommandLease(winner.actor, CommandOwner::learned, generation);
    return generation;
}

std::uint64_t BwapiBridge::releaseConstructionCommandLease(
    const Command& winner, const bool apiAccepted) {
    if (winner.owner == CommandOwner::construction || !changesActorOrder(winner.type) ||
        (!apiAccepted && (!winner.alreadyActive || !commandActive(winner)))) return 0;
    const auto frame = Broodwar->getFrameCount();
    auto handedOff = false;
    for (auto& [taskId, pending] : pendingBuilds_) {
        static_cast<void>(taskId);
        if (pending.builder == winner.actor)
            handedOff = pending.cancellation.handoff(frame, true) || handedOff;
    }
    if (!handedOff) return 0;
    auto& generation = issuedCommandLeaseGenerations_[winner.actor][CommandOwner::construction];
    if (generation < std::numeric_limits<std::uint64_t>::max()) ++generation;
    invalidateCommandLease(winner.actor, CommandOwner::construction, generation);
    std::erase_if(constructionCommandProposals_, [&winner](const auto& proposal) {
        return proposal.command.actor == winner.actor;
    });
    return generation;
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
        std::ranges::any_of(highTemplarUnits, [](const Unit templar) {
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
    const auto completed = unit->isCompleted();
    auto result = UnitSnapshot{
        unit->getID(), type.getID(), kind, toRace(type.getRace()), roleOf(type, kind),
        fromBwapi(unit->getPosition()), fromBwapi(unit->getPosition()),
        Broodwar->getFrameCount(), unit->getHitPoints(), type.maxHitPoints(),
        unit->getShields(), type.maxShields(), unit->getEnergy(), type.armor(),
        std::clamp((buildTime - unit->getRemainingBuildTime()) * 100 / buildTime, 0, 100),
        std::max(unit->getGroundWeaponCooldown(), unit->getAirWeaponCooldown()),
        type.topSpeed(), weapon(type.groundWeapon()), weapon(type.airWeapon()), ours,
        completed, unit->isFlying(), unit->isVisible(), unit->isDetected(),
        unit->isBurrowed(), bwapi::observedUnitCloaked(type, unit->isCloaked(), completed),
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
    result.gatheringMinerals = ours && unit->isGatheringMinerals();
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
                        unit->isResearching(), unit->isUpgrading(), {},
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
    if (baseTerrain_) {
        // Enemy-main routes are scheduled only after legal observation; all
        // terrain jobs share one retained frontier and one callback allowance.
        baseTerrainAnalysis_.scheduleEnemy(enemyMainRouteSource_);
        for (const auto& update : baseTerrainAnalysis_.advance(*baseTerrain_)) {
            auto& site = resourceSites_[update.site];
            if (update.work == BaseTerrainWork::homeDistance) {
                site.groundDistanceFromStart = update.groundDistance;
                site.groundReachabilityKnown = update.reachabilityKnown;
            } else if (update.work == BaseTerrainWork::enemyDistance) {
                site.enemyGroundDistanceFromMain = update.groundDistance;
                site.enemyGroundReachabilityKnown = update.reachabilityKnown;
            } else if (update.defense.valid() && std::ranges::none_of(site.defenses,
                [&update](const DefensivePosition& prior) {
                    return distanceSquared(prior.entrance, update.defense.entrance) < 160 * 160;
                })) site.defenses.push_back(update.defense);
        }
        try {
            if (placementDiagnostic && baseTerrainAnalysis_.lastWork() > 0)
                placementDiagnostic("BASE_TERRAIN_WORK," + std::to_string(state.frame) +
                    ",work=" + std::to_string(baseTerrainAnalysis_.lastWork()) +
                    ",remaining=" + std::to_string(baseTerrainAnalysis_.remaining()));
        } catch (...) { ++diagnosticErrors_; }
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
        bases.back().groundReachabilityKnown = site.groundReachabilityKnown;
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
            const auto searchStarted = std::chrono::steady_clock::now();
            depotTile = resourceDepotLocation(
                TilePosition(resourceCenter.x / 32, resourceCenter.y / 32));
            if (placementDiagnostic) placementDiagnostic("BASE_LOCATION_PROFILE,x=" +
                std::to_string(resourceCenter.x) + ",y=" + std::to_string(resourceCenter.y) +
                ",searchUs=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - searchStarted).count()) +
                ",found=" + std::to_string(depotTile.isValid()) +
                ",tileX=" + std::to_string(depotTile.x) + ",tileY=" + std::to_string(depotTile.y));
        }
        if (!depotTile.isValid()) {
            // getBuildLocation can reject every neutral base while its tiles
            // are still in fog during onStart. Search the static map footprint
            // with checkExplored=false so a real natural is not discarded in
            // favor of the next known start location.
            const TilePosition origin(resourceCenter.x / 32, resourceCenter.y / 32);
            std::vector<TilePosition> candidates;
            candidates.reserve(41U * 41U);
            for (auto y = origin.y - 20; y <= origin.y + 20; ++y) {
                for (auto x = origin.x - 20; x <= origin.x + 20; ++x) {
                    const TilePosition candidate(x, y);
                    if (!candidate.isValid() ||
                        x + UnitTypes::Protoss_Nexus.tileWidth() > Broodwar->mapWidth() ||
                        y + UnitTypes::Protoss_Nexus.tileHeight() > Broodwar->mapHeight()) {
                        continue;
                    }
                    candidates.push_back(candidate);
                }
            }
            // Same closest legal tile and y/x tie break as the exhaustive
            // scan, with no API probes after the first legal candidate.
            std::ranges::sort(candidates, [resourceCenter](const TilePosition a, const TilePosition b) {
                const auto first = distanceSquared({a.x * 32 + 64, a.y * 32 + 48}, resourceCenter);
                const auto second = distanceSquared({b.x * 32 + 64, b.y * 32 + 48}, resourceCenter);
                if (first != second) return first < second;
                return a.y != b.y ? a.y < b.y : a.x < b.x;
            });
            for (const auto candidate : candidates) {
                if (Broodwar->canBuildHere(candidate, UnitTypes::Protoss_Nexus, nullptr, false)) {
                    depotTile = candidate;
                    break;
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
    const InfluenceMap& influence, const std::uint64_t searches,
    const NavigationStatus navigationStatus, const bool unresolvedAlternatives,
    const bool searchBudgetExhausted) noexcept {
    if (!buildRouteDiagnostic || builder == nullptr) return;
    try {
        const auto from = fromBwapi(builder->getPosition());
        const auto footprint = placementFootprint(builder->getType());
        auto terrainWalkable = from.x - footprint.left >= 0 && from.y - footprint.up >= 0 &&
            from.x + footprint.right < Broodwar->mapWidth() * 32 &&
            from.y + footprint.down < Broodwar->mapHeight() * 32;
        for (auto y = (from.y - footprint.up) / 8;
             terrainWalkable && y <= (from.y + footprint.down) / 8; ++y)
            for (auto x = (from.x - footprint.left) / 8;
                 terrainWalkable && x <= (from.x + footprint.right) / 8; ++x)
                terrainWalkable = Broodwar->isWalkable(x, y);
        auto mineral = builder->getOrderTarget();
        if (mineral == nullptr || !mineral->exists() || !mineral->isVisible() ||
            !mineral->getType().isMineralField()) mineral = builder->getTarget();
        if (mineral == nullptr || !mineral->exists() || !mineral->isVisible() ||
            !mineral->getType().isMineralField()) mineral = nullptr;
        const auto bounds = mineral != nullptr ? NavigationObstacleBounds{
            mineral->getLeft(), mineral->getTop(), mineral->getRight(), mineral->getBottom()}
            : NavigationObstacleBounds{};
        buildRouteDiagnostic({Broodwar->getFrameCount(), builder->getID(), from, anchor,
            destination, std::string(stage), reachable, peakThreat,
            influence.at(anchor).groundThreat,
            activeNavigation_ == nullptr || activeNavigation_->walkable(from, footprint),
            destination.valid() && (activeNavigation_ == nullptr ||
                activeNavigation_->walkable(destination, footprint)),
            destination.valid() && builder->hasPath(toBwapiPosition(destination)), searches,
            terrainWalkable, footprint, mineral != nullptr ? mineral->getID() : -1,
            bounds, builder->getOrder().toString(), navigationStatus, unresolvedAlternatives,
            searchBudgetExhausted});
    } catch (...) { ++diagnosticErrors_; }
}

BWAPI::Unit BwapiBridge::findBuilder(
    const BWAPI::UnitType type,
    const BWAPI::Position near,
    const InfluenceMap& influence,
    const std::span<const UnitId> unavailableBuilders,
    const bool requireCanBuild,
    bool* rejectedForUnsafeRoute,
    bool* routePending,
    const UnitId preferredBuilder,
    BWAPI::Unit* pendingCandidate) {
    if (rejectedForUnsafeRoute != nullptr) *rejectedForUnsafeRoute = false;
    if (routePending != nullptr) *routePending = false;
    if (pendingCandidate != nullptr) *pendingCandidate = nullptr;
    std::vector<std::pair<long long, BWAPI::Unit>> candidates;
    auto hadUnsafeRouteCandidate = false;
    auto hadPendingRouteCandidate = false;
    NavigationSearchAdmissionScope admission{constructionRouteWork_};
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
    // The task-scoped inspection candidate is a preference, never a worker
    // lease or cached safety proof. Keep it through small mining-position changes;
    // eligibility, exposure, route and placement still run on every attempt.
    if (preferredBuilder >= 0 && !candidates.empty()) {
        const auto preferred = std::ranges::find_if(candidates, [preferredBuilder](const auto& entry) {
            return entry.second->getID() == preferredBuilder;
        });
        if (preferred != candidates.end() && preferred->first <= candidates.front().first + 96)
            std::rotate(candidates.begin(), preferred, std::next(preferred));
    }
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
            auto screeningStatus = NavigationStatus::invalidInput;
            auto screeningBudgetExhausted = false;
            const auto screeningDestination = groundConstructionScreeningDestination(
                influence, activeNavigation_, builderPosition, destination, placementFootprint(type), footprint,
                &screeningStatus, &screeningBudgetExhausted,
                &constructionRouteWork_, static_cast<std::uint64_t>(unit->getID()));
            if (!screeningDestination.valid()) {
                if (traceCandidate) reportBuildRoute(unit, destination, screeningDestination,
                    influence.at(destination).groundThreat > 0.25F ? "anchor-threat" : "access-unavailable",
                    false, 0.0, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore,
                    screeningStatus, false, screeningBudgetExhausted);
                if (screeningBudgetExhausted) {
                    hadPendingRouteCandidate = true;
                    admission.restrictNewSearches();
                    if (pendingCandidate != nullptr && *pendingCandidate == nullptr)
                        *pendingCandidate = unit;
                } else hadUnsafeRouteCandidate = true;
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
                    footprint, 0.4125, {-1, -1}, 0.25,
                    &constructionRouteWork_, static_cast<std::uint64_t>(unit->getID()));
                if (traceCandidate) reportBuildRoute(unit, destination, screeningDestination,
                    "builder-screen", route.profile.reachable, route.profile.peakThreat, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore,
                    route.profile.navigationStatus, route.unresolvedAlternatives,
                    route.profile.searchBudgetExhausted);
                if (!route.profile.reachable || route.profile.peakThreat > 0.25) {
                    if (route.profile.searchBudgetExhausted || route.unresolvedAlternatives) {
                        hadPendingRouteCandidate = true;
                    admission.restrictNewSearches();
                        if (pendingCandidate != nullptr && *pendingCandidate == nullptr)
                            *pendingCandidate = unit;
                    } else hadUnsafeRouteCandidate = true;
                    continue;
                }
            } else if (traceCandidate) {
                reportBuildRoute(unit, destination, screeningDestination, "clear-chord",
                    true, straightPathPeakThreat, influence,
                    NavigationGrid::diagnosticsForCurrentThread().searches - searchesBefore,
                    NavigationStatus::reached);
            }
        }
        return unit;
    }
    if (rejectedForUnsafeRoute != nullptr)
        *rejectedForUnsafeRoute = hadUnsafeRouteCandidate;
    if (routePending != nullptr) *routePending = hadPendingRouteCandidate;
    return nullptr;
}

BwapiBridge::StaticDefenseConstructionContext BwapiBridge::staticDefenseConstructionContext(
    const BWAPI::UnitType type, const StrategicPlan& plan) const {
    Unit defendedBase = nullptr;
    auto fewestNearbyDefenses = std::numeric_limits<int>::max();
    auto bestDistance = std::numeric_limits<double>::infinity();
    const auto rally = plan.constructionRallyPoint();
    const auto fallback = rally.valid() ? toBwapiPosition(rally)
        : BWAPI::Position(Broodwar->self()->getStartLocation());
    for (const auto base : Broodwar->self()->getUnits()) {
        if (base == nullptr || !base->exists() ||
            base->getType() != UnitTypes::Protoss_Nexus) continue;
        const auto localPylon = Broodwar->getClosestUnit(base->getPosition(),
            Filter::GetType == UnitTypes::Protoss_Pylon && Filter::IsCompleted && Filter::IsOwned);
        if (localPylon == nullptr || localPylon->getDistance(base) > 384) continue;
        const auto defenses = static_cast<int>(Broodwar->getUnitsInRadius(base->getPosition(), 416,
            Filter::IsOwned && Filter::GetType == type).size());
        const auto destination = plan.attackTarget.valid() ? plan.attackTarget : fromBwapi(fallback);
        const auto candidateDistance = distance(fromBwapi(base->getPosition()), destination);
        if (defenses < fewestNearbyDefenses ||
            (defenses == fewestNearbyDefenses && candidateDistance < bestDistance)) {
            fewestNearbyDefenses = defenses;
            bestDistance = candidateDistance;
            defendedBase = base;
        }
    }
    if (defendedBase == nullptr) return {};
    const auto directionKnown = Broodwar->getStartLocations().size() == 2U ||
        std::ranges::any_of(enemyMemory_, [](const auto& entry) {
            return entry.second.role == UnitRole::resourceDepot;
        });
    const auto basePosition = fromBwapi(defendedBase->getPosition());
    const auto forwardDistance = rally.valid() ? distance(basePosition, rally) : 0.0;
    const auto forwardLayout = directionKnown && forwardDistance >= 64.0 && forwardDistance <= 256.0;
    const auto enemy = Broodwar->enemy();
    const auto groundPressure = forwardLayout && enemy != nullptr &&
        std::ranges::any_of(enemy->getUnits(), [defendedBase](const Unit threat) {
            if (threat == nullptr || !threat->exists() || !threat->isVisible() ||
                !threat->isCompleted()) return false;
            return threat->getType().groundWeapon() != WeaponTypes::None &&
                   threat->getDistance(defendedBase) <= 512;
        });
    const auto anchor = constructionBuilderAnchor(UnitKind::photonCannon,
        basePosition, rally, {-1, -1}, directionKnown, groundPressure);
    return {defendedBase, toBwapiPosition(anchor), forwardLayout, groundPressure};
}

BWAPI::Position BwapiBridge::supplyConstructionHome(const BWAPI::Position home) const {
    const auto threatened = [](const BWAPI::Unit base) {
        if (base->isUnderAttack()) return true;
        const auto enemy = Broodwar->enemy();
        return enemy != nullptr && std::ranges::any_of(enemy->getUnits(), [base](const Unit threat) {
            if (threat == nullptr || !threat->exists() || !threat->isVisible() ||
                !threat->isCompleted()) return false;
            const auto weapon = threat->getType().groundWeapon();
            return weapon != WeaponTypes::None &&
                   threat->getDistance(base) <= weapon.maxRange() + 96;
        });
    };
    const auto homeNexus = Broodwar->getClosestUnit(home,
        Filter::IsOwned && Filter::IsCompleted && Filter::GetType == UnitTypes::Protoss_Nexus);
    if (homeNexus == nullptr || !threatened(homeNexus)) return home;
    std::vector<SupplyConstructionBase> bases;
    for (const auto base : Broodwar->self()->getUnits()) {
        if (base == nullptr || !base->exists() || !base->isCompleted() ||
            base->getType() != UnitTypes::Protoss_Nexus) continue;
        bases.push_back({base->getID(), fromBwapi(base->getPosition()), true,
                         base == homeNexus || threatened(base)});
    }
    return toBwapiPosition(selectSupplyConstructionAnchor(fromBwapi(home), bases));
}

BWAPI::Position BwapiBridge::refineryConstructionHome(
    const BWAPI::Position home, const InfluenceMap& influence) const {
    std::vector<RefineryConstructionSite> sites;
    for (const auto geyser : Broodwar->getGeysers()) {
        if (geyser == nullptr || !geyser->exists() || geyser->getResources() <= 0) continue;
        const auto nexus = Broodwar->getClosestUnit(geyser->getPosition(),
            Filter::IsOwned && Filter::IsCompleted && Filter::GetType == UnitTypes::Protoss_Nexus);
        if (nexus == nullptr || nexus->getDistance(geyser) > 448) continue;
        const auto threatened = nexus->isUnderAttack() ||
            influence.at(fromBwapi(nexus->getPosition())).groundThreat > 0.25F ||
            influence.at(fromBwapi(geyser->getPosition())).groundThreat > 0.25F;
        sites.push_back({geyser->getID(), fromBwapi(geyser->getPosition()), true, threatened});
    }
    return toBwapiPosition(selectRefineryConstructionAnchor(fromBwapi(home), sites));
}

BWAPI::TilePosition BwapiBridge::buildLocation(
    const UnitKind kind,
    const BWAPI::UnitType type,
    const BWAPI::Unit builder,
    const StrategicPlan& plan,
    const ConstructionTaskSite& constructionSite,
    const std::uint64_t taskKey,
    const std::string_view reason,
    const Position constructionAnchor) {
    const auto placementStarted = std::chrono::steady_clock::now();
    const auto placementWorkAvailable = [placementStarted] {
        return std::chrono::steady_clock::now() - placementStarted < std::chrono::milliseconds(12);
    };
    const auto reportPlacement = [this](const std::string& record) noexcept {
        if (!placementDiagnostic) return;
        try { placementDiagnostic(record); } catch (...) { ++diagnosticErrors_; }
    };
    // The first five costs are disjoint call sites. Map/builder footprint
    // costs additionally break down the enclosing footprint category.
    struct PlacementCostProfile {
        const decltype(reportPlacement)& report;
        std::chrono::steady_clock::time_point started;
        int frame;
        std::string kind;
        std::uint64_t task;
        bool enabled;
        std::array<PlacementPhaseCost, 7> phases{};
        ~PlacementCostProfile() noexcept {
            if (!enabled) return;
            try {
                auto record = "PLACEMENT_COST," + std::to_string(frame) +
                    ",kind=" + kind + ",task=" + std::to_string(task) +
                    ",elapsedUs=" + std::to_string(std::chrono::duration_cast<
                        std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
                constexpr std::array names{"power", "footprint", "access", "nativePath", "miningLane",
                                           "mapFootprint", "builderFootprint"};
                for (auto index = std::size_t{}; index < phases.size(); ++index) {
                    record += ',' + std::string(names[index]) + "Calls=" +
                        std::to_string(phases[index].calls) + ',' + names[index] + "Us=" +
                        std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                            phases[index].elapsed).count());
                }
                report(record);
            } catch (...) { /* Diagnostic collection must never alter placement. */ }
        }
        PlacementPhaseCost* phase(const std::size_t index) noexcept {
            return enabled ? &phases[index] : nullptr;
        }
    } placementCost{reportPlacement, placementStarted, Broodwar->getFrameCount(),
                    type.getName(), taskKey, static_cast<bool>(placementDiagnostic)};
    const auto nativeHasPath = [&](const BWAPI::Position destination) {
        const PlacementCostTimer timer{placementCost.phase(3)};
        return builder->hasPath(destination);
    };
    const auto footprintBuildable = [&](const TilePosition location, const bool explored) {
        const PlacementCostTimer timer{placementCost.phase(1)};
        const PlacementBuilderBounds bounds = builder != nullptr
            ? PlacementBuilderBounds{builder->getLeft(), builder->getTop(),
                                     builder->getRight(), builder->getBottom()}
            : PlacementBuilderBounds{};
        return placementFootprintBuildable(location, type, bounds,
            [&] {
                const PlacementCostTimer mapTimer{placementCost.phase(5)};
                return Broodwar->canBuildHere(location, type, nullptr, explored);
            }, [&] {
                const PlacementCostTimer builderTimer{placementCost.phase(6)};
                return Broodwar->canBuildHere(location, type, builder, explored);
            });
    };
    const auto powered = [&](const TilePosition location) {
        const PlacementCostTimer timer{placementCost.phase(0)};
        return Broodwar->hasPower(location, type);
    };
    const auto miningLaneBlocked = [&](const TilePosition location) {
        const PlacementCostTimer timer{placementCost.phase(4)};
        return blocksMiningLane(location, type);
    };
    lastMacroStatus_ = "placement-search";
    if (kind == UnitKind::assimilator) {
        Unit bestGeyser = nullptr;
        auto bestScore = std::numeric_limits<int>::max();
        for (const auto geyser : Broodwar->getGeysers()) {
            if (geyser == nullptr || !geyser->exists() || geyser->getResources() <= 0) continue;
            // Builder feasibility and placement must ask about the same
            // geyser. Choosing a new site after screening against the army
            // rally repeatedly stranded multi-frame construction searches.
            if (constructionAnchor.valid() && fromBwapi(geyser->getPosition()) != constructionAnchor)
                continue;
            const auto nexus = Broodwar->getClosestUnit(
                geyser->getPosition(),
                Filter::IsOwned && Filter::IsCompleted && Filter::GetType == UnitTypes::Protoss_Nexus);
            if (nexus == nullptr || nexus->getDistance(geyser) > 448 ||
                !nativeHasPath(geyser->getPosition())) {
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
    const auto retainPlacementBaselines = [this, &placementAccessRequirements] {
        // Candidate detours become valid global witnesses after the temporary
        // obstacle is removed. Retain the final, validated witnesses from this
        // call rather than its original paths through rejected footprints.
        placementAccessBaselines_.clear();
        auto cachedBytes = std::size_t{};
        for (const auto& requirement : placementAccessRequirements) {
            for (const auto& route : requirement.alternatives) {
                if (!route.baseline.reached() || !route.validatedBaseline) continue;
                const auto bytes = sizeof(PlacementAccessRoute) + 64 +
                    sizeof(Position) * (route.baseline.points.size() * 2);
                if (placementAccessBaselines_.size() >= 128 ||
                    bytes > 128 * 1024 - cachedBytes) continue;
                placementAccessBaselines_.push_back(route);
                cachedBytes += bytes;
            }
        }
    };
    struct PlacementBaselineRetention {
        const decltype(retainPlacementBaselines)& retain;
        ~PlacementBaselineRetention() noexcept {
            try { retain(); } catch (...) { /* Cache failure cannot alter placement. */ }
        }
    } retention{retainPlacementBaselines};
    if (activeNavigation_ != nullptr && !activeNavigation_->empty()) {
        std::vector<std::array<int, 5>> sourceKeys;
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit == nullptr || !unit->exists()) continue;
            const auto unitType = unit->getType();
            if (unitType != UnitTypes::Protoss_Nexus &&
                unitType != UnitTypes::Protoss_Gateway &&
                unitType != UnitTypes::Protoss_Robotics_Facility) continue;
            const auto position = unit->getPosition();
            sourceKeys.push_back({unit->getID(), unitType.getID(), position.x, position.y,
                                  unit->isCompleted() ? 1 : 0});
        }
        for (const auto mineral : Broodwar->getMinerals()) {
            if (mineral == nullptr || !mineral->exists() || mineral->getResources() <= 0) continue;
            const auto position = mineral->getPosition();
            sourceKeys.push_back({mineral->getID(), mineral->getType().getID(),
                                  position.x, position.y, 1});
        }
        std::ranges::sort(sourceKeys);
        const auto sameAccessGeometry = placementAccessTerrain_ == activeNavigation_->terrainIdentity() &&
            placementAccessCollision_ == activeNavigation_->collisionIdentity() &&
            placementAccessSourceKeys_ == sourceKeys;
        if (sameAccessGeometry) {
            placementAccessRequirements = placementAccessTemplates_;
        } else {
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
            // Store only endpoint geometry. Every use below still refreshes its
            // path proofs against the current grid and tests candidate overlays.
            if (placementAccessRequirements.size() <= 128 && sourceKeys.size() <= 2048) {
                placementAccessSourceKeys_ = std::move(sourceKeys);
                placementAccessTemplates_ = placementAccessRequirements;
                placementAccessTerrain_ = activeNavigation_->terrainIdentity();
                placementAccessCollision_ = activeNavigation_->collisionIdentity();
            } else {
                placementAccessTemplates_.clear();
                placementAccessSourceKeys_.clear();
                placementAccessTerrain_ = placementAccessCollision_ = 0;
            }
        }
        for (auto& requirement : placementAccessRequirements) {
            for (auto& route : requirement.alternatives) {
                const auto prior = std::ranges::find_if(placementAccessBaselines_,
                    [&route](const PlacementAccessRoute& cached) {
                        return cached.from == route.from && cached.to == route.to &&
                            cached.footprint == route.footprint;
                    });
                if (prior != placementAccessBaselines_.end()) route = *prior;
            }
        }
        placementExitWork_.beginFrame(*activeNavigation_, Broodwar->getFrameCount(), 2048);
        if (capturePlacementAccessBaselinesBounded(
                *activeNavigation_, placementAccessRequirements, placementExitWork_) ==
            PlacementAccessCaptureStatus::deferred) {
            lastMacroStatus_ = "placement-search-deferred-baseline";
            return TilePositions::None;
        }
    }
    if (placementDiagnostic) {
        try {
            auto alternatives = std::size_t{};
            for (const auto& requirement : placementAccessRequirements)
                alternatives += requirement.alternatives.size();
            const auto setupUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - placementStarted).count();
            reportPlacement("PLACEMENT_BASELINE_PROFILE," + std::to_string(Broodwar->getFrameCount()) +
                ",kind=" + type.getName() + ",elapsedUs=" + std::to_string(setupUs) +
                ",requirements=" + std::to_string(placementAccessRequirements.size()) +
                ",alternatives=" + std::to_string(alternatives));
        } catch (...) { ++diagnosticErrors_; }
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
            if (!footprintBuildable(site.depotTile, false)) continue;
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
                if (capturePlacementAccessBaselinesBounded(
                        *activeNavigation_, expansionAccessRequirements, placementExitWork_) ==
                    PlacementAccessCaptureStatus::deferred) {
                    lastMacroStatus_ = "placement-search-deferred-baseline";
                    return TilePositions::None;
                }
                const auto missingRequirements = expansionAccessRequirements.empty();
                const auto hasUnreachableMineral = missingRequirements ||
                    std::ranges::any_of(expansionAccessRequirements,
                        [](const PlacementAccessRequirement& requirement) {
                            return !requirement.reachableBefore;
                        });
                const auto preservesRoutes = !hasUnreachableMineral &&
                    preservesPlacementAccess(center, UnitTypes::Protoss_Nexus,
                        std::span<PlacementAccessRequirement>(expansionAccessRequirements));
                if (hasUnreachableMineral && placementDiagnostic) {
                    try {
                        const auto failed = std::ranges::find_if(expansionAccessRequirements,
                            [](const PlacementAccessRequirement& requirement) { return !requirement.reachableBefore; });
                        std::string routes;
                        if (failed != expansionAccessRequirements.end()) {
                            for (const auto& route : failed->alternatives) {
                                if (!routes.empty()) routes += ';';
                                routes += std::to_string(route.from.x) + ':' + std::to_string(route.from.y) +
                                    '>' + std::to_string(route.to.x) + ':' + std::to_string(route.to.y) +
                                    ':' + std::string(navigationStatusName(route.baseline.status)) +
                                    ":budget" + std::to_string(route.baseline.searchBudgetExhausted) +
                                    ":clear" + std::to_string(activeNavigation_->walkable(route.from,route.footprint)) +
                                    ':' + std::to_string(activeNavigation_->walkable(route.to,route.footprint));
                            }
                        }
                        reportPlacement("NEXUS_ACCESS_BASELINE," + std::to_string(Broodwar->getFrameCount()) +
                            ",task=" + std::to_string(taskKey) +
                            ",builder=" + std::to_string(builder->getID()) +
                            ",site=" + std::to_string(center.x) + ':' + std::to_string(center.y) +
                            ",sources=" + std::to_string(sources.size()) +
                            ",minerals=" + std::to_string(siteMineralCount) +
                            ",requirements=" + std::to_string(expansionAccessRequirements.size()) +
                            ",routes=" + routes);
                    } catch (...) { ++diagnosticErrors_; }
                }
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
        if (placementDiagnostic) {
            try {
                reportPlacement("NEXUS_PLACEMENT," + std::to_string(Broodwar->getFrameCount()) +
                    ",task=" + std::to_string(taskKey) +
                    ",builder=" + std::to_string(builder->getID()) +
                    ",desired=" + std::to_string(desiredSite.x) + ':' + std::to_string(desiredSite.y) +
                    ",selected=" + (best != nullptr ? std::to_string(best->depotCenter.x) + ':' +
                                                     std::to_string(best->depotCenter.y) : "none") +
                    ",valid=" + std::to_string(validSites) + ",reachable=" + std::to_string(reachableSites) +
                    ",free=" + std::to_string(freeSites) + ",accessRejected=" + std::to_string(accessRejectedSites) +
                    ",missingRequirements=" + std::to_string(accessMissingRequirements) +
                    ",unreachableBaseline=" + std::to_string(accessUnreachableBaselines) +
                    ",wouldBlock=" + std::to_string(accessWouldBlockRoutes) +
                    ",missingSources=" + std::to_string(accessSitesMissingSources) +
                    ",missingTargets=" + std::to_string(accessSitesMissingTargets) +
                    ",noMinerals=" + std::to_string(accessSitesWithoutMinerals));
            } catch (...) { ++diagnosticErrors_; }
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

    auto anchorPosition = plan.constructionRallyPoint().valid()
                              ? toBwapiPosition(plan.constructionRallyPoint())
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
            const auto forwardDistance = plan.constructionRallyPoint().valid()
                                             ? distance(fromBwapi(leastPoweredBase->getPosition()),
                                                        plan.constructionRallyPoint())
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
                        {(basePosition.x + plan.constructionRallyPoint().x) / 2,
                         (basePosition.y + plan.constructionRallyPoint().y) / 2});
                    startLayoutAtCenter = true;
                    avoidEnemyFire = true;
                } else {
                    anchorPosition = toBwapiPosition(plan.constructionRallyPoint());
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
        const auto context = staticDefenseConstructionContext(type, plan);
        if (context.base == nullptr) {
            lastMacroStatus_ = "placement-defense-no-powered-base";
            return TilePositions::None;
        }
        // Builder admission and placement share the powered defended base,
        // compact forward intercept and pressure-adjusted midpoint.
        defendedNexus = context.base;
        anchorPosition = context.anchor;
        useForwardLayout = context.forwardLayout;
        startLayoutAtCenter = context.underPressure;
        avoidEnemyFire = context.underPressure;
    }

    if (homeAnchoredSupplyPylon(kind, reason, constructionSite.valid())) {
        // Supply forecasts and builder selection share this anchor. A safe
        // surviving base can replace the threatened home, while the usual
        // least-powered-base layout must not add an unforecast trip.
        const auto start = BWAPI::Position(Broodwar->self()->getStartLocation());
        const auto home = Broodwar->getClosestUnit(
            start, Filter::IsOwned && Filter::IsCompleted &&
                       Filter::GetType == UnitTypes::Protoss_Nexus);
        anchorPosition = home != nullptr ? home->getPosition() : start;
        if (relocatableSupplyPylon(kind, reason, constructionSite.valid()) && constructionAnchor.valid()) {
            anchorPosition = toBwapiPosition(constructionAnchor);
            if (fromBwapi(anchorPosition) != (home != nullptr ? fromBwapi(home->getPosition()) : fromBwapi(start)))
                reportPlacement("SUPPLY_ANCHOR," + std::to_string(Broodwar->getFrameCount()) +
                    ",x=" + std::to_string(constructionAnchor.x) + ",y=" + std::to_string(constructionAnchor.y));
        }
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
    auto pylonAccessChecks = 0;
    OpeningCannonPairSupport openingCannonSupport;
    if (kind == UnitKind::photonCannon && defendedNexus != nullptr) {
        auto nexusCount = 0;
        std::vector<CannonConstructionSupport> cannons;
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit == nullptr || !unit->exists()) continue;
            if (unit->getType() == UnitTypes::Protoss_Nexus) ++nexusCount;
            if (unit->getType() != UnitTypes::Protoss_Photon_Cannon) continue;
            cannons.push_back({unit->getID(), fromBwapi(unit->getPosition()),
                unit->getType().groundWeapon().maxRange(),
                unit->getHitPoints() + unit->getShields() > 0 &&
                    !unit->isHallucination() && !unit->isStasised() &&
                    !unit->isLockedDown() && !unit->isMaelstrommed(),
                unit->isPowered(), unit->isCompleted(), unit->getRemainingBuildTime()});
        }
        openingCannonSupport = selectOpeningCannonPairSupport(kind,
            Broodwar->getFrameCount(), nexusCount, fromBwapi(defendedNexus->getPosition()),
            constructionSite.valid(), type.buildTime(), cannons);
        if (openingCannonSupport.active()) {
            reportPlacement("CANNON_PAIR_SUPPORT," + std::to_string(Broodwar->getFrameCount()) +
                ",support=" + std::to_string(openingCannonSupport.id) +
                ",x=" + std::to_string(openingCannonSupport.center.x) +
                ",y=" + std::to_string(openingCannonSupport.center.y) +
                ",range=" + std::to_string(openingCannonSupport.groundRange));
        }
    }
    auto& continuation = placementSearches_[taskKey];
    if (continuation.anchor != fromBwapi(anchorPosition)) {
        continuation = PlacementSearchState{};
        continuation.anchor = fromBwapi(anchorPosition);
    }
    auto candidateAccessPending = false;
    auto candidateTimePending = false;
    if (activeNavigation_ != nullptr && !activeNavigation_->empty())
        placementExitWork_.beginFrame(*activeNavigation_, Broodwar->getFrameCount(), 2048);
    const auto candidatePreservesAccess = [&](const TilePosition location) {
        const PlacementCostTimer timer{placementCost.phase(2)};
        if (activeNavigation_ == nullptr || activeNavigation_->empty()) return true;
        if (kind == UnitKind::pylon) ++pylonAccessChecks;
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
                const auto sameRoutes = [](const std::vector<PlacementAccessRoute>& cached,
                                           const PlacementAccessRequirement& current) {
                    if (cached.size() != current.alternatives.size()) return false;
                    for (auto index = std::size_t{}; index < cached.size(); ++index) {
                        const auto& a = cached[index]; const auto& b = current.alternatives[index];
                        if (a.from != b.from || a.to != b.to || a.footprint != b.footprint) return false;
                    }
                    return true;
                };
                std::erase_if(continuation.failedExits, [&](const PlacementSearchState::FailedExit& cached) {
                    return cached.terrain != activeNavigation_->terrainIdentity() ||
                        cached.collision != activeNavigation_->collisionIdentity();
                });
                if (!candidateRequirements.empty() && std::ranges::any_of(continuation.failedExits,
                    [&](const PlacementSearchState::FailedExit& cached) {
                        return cached.tile == location && sameRoutes(cached.routes, candidateRequirements.front());
                    })) return false;
                const auto captured = capturePlacementAccessBaselinesBounded(
                    *activeNavigation_, candidateRequirements, placementExitWork_);
                if (captured == PlacementAccessCaptureStatus::deferred) {
                    candidateAccessPending = true;
                    continuation.pendingAccessTile = location;
                    lastMacroStatus_ = "placement-search-deferred-producer-exit";
                    return false;
                }
                if (candidateRequirements.empty() ||
                    !candidateRequirements.front().reachableBefore) {
                    if (!candidateRequirements.empty() && continuation.failedExits.size() < 256) {
                        auto routes = candidateRequirements.front().alternatives;
                        for (auto& route : routes) { route.baseline = {}; route.validatedBaseline.reset(); }
                        continuation.failedExits.push_back({location, activeNavigation_->terrainIdentity(),
                            activeNavigation_->collisionIdentity(), std::move(routes)});
                    }
                    return false;
                }
            }
        }
        return preservesPlacementAccess(center, type,
            std::span<PlacementAccessRequirement>(candidateRequirements));
    };
    const auto usable = [&](const TilePosition location, const bool validateAccess = true) {
        if (candidateAccessPending) return false;
        if (!location.isValid() || location.x < 0 || location.y < 0 ||
            location.x + type.tileWidth() > Broodwar->mapWidth() ||
            location.y + type.tileHeight() > Broodwar->mapHeight()) {
            return false;
        }
        if (!placementWorkAvailable()) {
            // Keep this untested candidate for the next pass. Spending the
            // allowance neither rejects its geometry nor permits a build.
            candidateAccessPending = true;
            candidateTimePending = true;
            continuation.pendingAccessTile = location;
            lastMacroStatus_ = "placement-search-deferred-time";
            return false;
        }
        ++validCandidates;
        const Position supportCandidate{location.x * 32 + type.tileWidth() * 16,
                                        location.y * 32 + type.tileHeight() * 16};
        if (!openingCannonPlacementAllowed(supportCandidate, openingCannonSupport))
            return false;
        if (type.requiresPsi() && !powered(location)) return false;
        ++poweredCandidates;
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
            const auto exitGap = type.canProduce() || otherType.canProduce() ? 32 :
                openingCannonStructureGap(kind, toKind(otherType),
                    fromBwapi(building->getPosition()), structureGap, openingCannonSupport);
            if (!separatedByGap(footprint, occupied, exitGap)) return false;
        }
        for (const auto& [taskId, pending] : pendingBuilds_) {
            static_cast<void>(taskId);
            const auto pendingKind = pending.kind;
            const auto pendingType = toBwapi(pendingKind);
            if (pending.target.x < 0 || pending.target.y < 0 ||
                pendingType == UnitTypes::None) continue;
            const Position pendingCenter{
                pending.target.x + pendingType.tileWidth() * 16,
                pending.target.y + pendingType.tileHeight() * 16};
            const auto pendingGap = type.canProduce() || pendingType.canProduce() ? 32 :
                openingCannonStructureGap(kind, pendingKind, pendingCenter,
                    structureGap, openingCannonSupport);
            if (!separatedByGap(footprint,
                    {pending.target, pendingType.tileWidth() * 32, pendingType.tileHeight() * 32},
                    pendingGap)) return false;
        }
        // Reject reserved footprints, entrances and structure gaps before
        // invoking BWAPI's map-wide occupancy query. These same exclusions
        // previously ran after the query, wasting hundreds of native checks
        // during crowded defensive placement without admitting another tile.
        // The explored map/builder checks and all later access validation
        // still run for every candidate that survives these exclusions.
        if (!footprintBuildable(location, true)) return false;
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
                if (nativeHasPath(toBwapiPosition(center)) &&
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
        if (!nativeHasPath(toBwapiPosition(center))) return false;
        ++reachableCandidates;
        return true;
    };
    const auto consider = [&](const TilePosition location) {
        if (!usable(location)) return TilePositions::None;
        if (!miningLaneBlocked(location)) return location;
        ++laneCandidates;
        if (!miningLaneFallback.isValid()) miningLaneFallback = location;
        return TilePositions::None;
    };
    // Finish the same deferred tile before revisiting earlier preferences.
    // A changed live footprint/power check can reject it, but a pending route
    // never falls through to a less-tested tile or a remembered fallback.
    if (continuation.pendingAccessTile.isValid()) {
        const auto pendingTile = continuation.pendingAccessTile;
        continuation.pendingAccessTile = TilePositions::None;
        const auto resumed = consider(pendingTile);
        if (candidateAccessPending) return TilePositions::None;
        if (resumed.isValid()) {
            lastMacroStatus_ = "placement-resumed-producer-exit";
            return resumed;
        }
    }
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
            if (usable(location) && !miningLaneBlocked(location)) {
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
                if (!usable(location, false) || miningLaneBlocked(location)) continue;
                const Position center{location.x * 32 + type.tileWidth() * 16,
                                      location.y * 32 + type.tileHeight() * 16};
                auto score = -distance(center, nexusPosition) * 0.04;
                if (intercept.valid()) score -= distance(center, intercept) * 0.30;
                else if (plan.constructionRallyPoint().valid()) score -= distance(center, plan.constructionRallyPoint()) * 0.08;

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
    PlacementSearchWindow pylonSearch(continuation.offset);
    const auto considerPreferred = [&](const TilePosition location) {
        if (kind == UnitKind::pylon) {
            // Preferred tiles need the same continuation as the broad sweep.
            // Rechecking them on each retry used the entire macro budget before
            // the sweep could advance in a crowded main. Retain only the cursor;
            // every admitted tile still receives the live access proof.
            const auto spentUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - placementStarted).count();
            const auto workAvailable = pylonAccessChecks == 0 ||
                (pylonAccessChecks < 3 && spentUs < 8'000);
            if (!pylonSearch.visit(workAvailable)) return TilePositions::None;
        }
        return consider(location);
    };
    for (std::size_t attempt = 0; attempt < layout.size(); ++attempt) {
        const auto offset = layout[(layoutStart + attempt) % layout.size()];
        // Test the preferred tile directly. Calling BWAPI's nested radius
        // searches here repeatedly scanned the same crowded area before our
        // own complete fallback search even began.
        const auto location = anchor + offset;
        const auto accepted = considerPreferred(location);
        if (accepted.isValid()) {
            lastMacroStatus_ = "placement-safe";
            return accepted;
        }
    }

    const auto broadAccepted = considerPreferred(anchor);
    if (broadAccepted.isValid()) {
        lastMacroStatus_ = "placement-safe";
        return broadAccepted;
    }
    // Search all fallback tiles across successive macro passes. Keep the
    // compact scored defense/production search above responsive every time,
    // but bound the expensive BWAPI checks in the broad sweep. Late-game
    // traces had more than 8,000 candidates per structure on every retry.
    PlacementSearchWindow fallbackSearch(continuation.offset);
    auto& search = kind == UnitKind::pylon ? pylonSearch : fallbackSearch;
    const auto considerFallback = [&](const TilePosition location) {
        if (kind == UnitKind::pylon) return considerPreferred(location);
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

    if (candidateAccessPending) {
        lastMacroStatus_ = candidateTimePending ? "placement-search-deferred-time" :
            "placement-search-deferred-producer-exit";
        return TilePositions::None;
    }
    continuation.offset = search.nextOffset();
    if (search.deferred()) {
        if (enemyFireFallback.isValid()) continuation.fireFallback = enemyFireFallback;
        if (miningLaneFallback.isValid()) continuation.laneFallback = miningLaneFallback;
        lastMacroStatus_ = "placement-search-deferred-" + std::to_string(continuation.offset);
        return TilePositions::None;
    }
    // Keep continuation alive while these callbacks use its failed-exit cache.
    // Only relax enemy-fire and mining-lane preferences after the full sweep;
    // power, occupancy, failed sites, builder paths and exits remain mandatory.
    const auto fallbackTile = [](const TilePosition tile) {
        return tile.isValid() ? Position{tile.x, tile.y} : Position{-1, -1};
    };
    // Prefer a remembered tile that now satisfies the ordinary layout rules.
    // A hazardous fallback must not displace a newly safe remembered choice.
    const std::array remembered{
        fallbackTile(continuation.fireFallback), fallbackTile(continuation.laneFallback)};
    const auto preferred = finishPlacementFallbacks(remembered,
        [&](const Position tile) {
            const auto accepted = consider(TilePosition{tile.x, tile.y});
            if (candidateAccessPending) return PlacementFallbackStatus::deferred;
            return accepted.isValid() ? PlacementFallbackStatus::accepted
                                      : PlacementFallbackStatus::rejected;
        }, [] {});
    if (preferred.status == PlacementFallbackStatus::deferred) {
        lastMacroStatus_ = candidateTimePending ? "placement-search-deferred-time" :
            "placement-search-deferred-producer-exit";
        return TilePositions::None;
    }
    if (preferred.status == PlacementFallbackStatus::accepted) {
        placementSearches_.erase(taskKey);
        lastMacroStatus_ = "placement-revalidated-fallback";
        return {preferred.tile.x, preferred.tile.y};
    }
    const std::array fallbacks{
        fallbackTile(enemyFireFallback), fallbackTile(miningLaneFallback)};
    avoidEnemyFire = false;
    const auto fallback = finishPlacementFallbacks(fallbacks,
        [&](const Position tile) {
            const auto accepted = usable(TilePosition{tile.x, tile.y});
            if (candidateAccessPending) return PlacementFallbackStatus::deferred;
            return accepted ? PlacementFallbackStatus::accepted
                            : PlacementFallbackStatus::rejected;
        }, [&] { placementSearches_.erase(taskKey); });
    if (fallback.status == PlacementFallbackStatus::deferred) {
        lastMacroStatus_ = candidateTimePending ? "placement-search-deferred-time" :
            "placement-search-deferred-producer-exit";
        return TilePositions::None;
    }
    if (fallback.status == PlacementFallbackStatus::accepted) {
        lastMacroStatus_ = "placement-revalidated-fallback";
        return {fallback.tile.x, fallback.tile.y};
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
    NavigationGrid* navigation,
    const bool prepositionOnly) {
    if (prepositionOnly && action.target != UnitKind::nexus) {
        lastMacroStatus_ = "build-preparation-invalid-kind";
        return false;
    }
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
        expansionBuilderPreferences_.erase(taskId);
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
        pending->second.routePending = false;
        const auto leasedBuilder = Broodwar->getUnit(pending->second.builder);
        const Position routeTarget{
            pending->second.target.x + type.tileWidth() * 16,
            pending->second.target.y + type.tileHeight() * 16,
        };
        // A safe approach or a nearby Probe does not prove the Nexus site is
        // safe. Revalidate before travel/funding waits can retain an old check.
        // An already-paid structure is handled by the protection mission.
        if (action.target == UnitKind::nexus && !pending->second.resourcesPaid &&
            !groundConstructionSiteSafe(influence, routeTarget)) {
            pending->second.unsafeRoute = true;
            const auto proposed = requestBuildCancellation(
                pending->second, leasedBuilder, "build-site-danger");
            lastMacroStatus_ = pending->second.cancellation.awaiting()
                ? "build-site-danger-awaiting-ack" : proposed
                    ? "build-site-danger-cancel-proposed"
                    : "build-site-danger-cancel-rejected";
            return false;
        }
        if (leasedBuilder != nullptr && leasedBuilder->exists()) {
            const auto last = leasedBuilder->getLastCommand();
            const auto builderPosition = fromBwapi(leasedBuilder->getPosition());
            const auto activeWaypoint = pending->second.travelWaypoint;
            const auto progressingOnWaypoint = pending->second.prepositioned &&
                constructionWaypointMakingProgress(
                    pending->second.waypointProgress, Broodwar->getFrameCount(),
                    builderPosition, last.getType() == BWAPI::UnitCommandTypes::Move,
                    leasedBuilder->getOrder() == BWAPI::Orders::Move,
                    fromBwapi(last.getTargetPosition()));
            const auto assistedWaypoint = !progressingOnWaypoint &&
                trafficWaypointHold(pending->second, influence, navigation);
            if ((progressingOnWaypoint || assistedWaypoint) &&
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
                auto accessStatus = NavigationStatus::invalidInput;
                auto accessPending = false;
                const auto access = nearestGroundConstructionAccessPoint(
                    navigation, builderPosition, routeTarget,
                    placementFootprint(type), footprint, 96, &accessStatus, &accessPending,
                    &constructionRouteWork_, static_cast<std::uint64_t>(leasedBuilder->getID()));
                if (access.valid()) {
                    routeDestination = access;
                } else if (accessPending) {
                    pending->second.routePending = true;
                    lastMacroStatus_ = "build-route-pending";
                    return false;
                } else {
                    // A wider partial prefix does not prove construction
                    // access after every legal staging candidate has settled.
                    routeDestination = {-1,-1};
                }
            }
            builderRoute = selectSafeRouteAlternative(
                influence, builderPosition, routeDestination,
                false, navigation, footprint, 0.4125,
                pending->second.routeWaypoint, 0.25,
                &constructionRouteWork_, static_cast<std::uint64_t>(leasedBuilder->getID()));
            if (builderRoute.profile.searchBudgetExhausted || builderRoute.unresolvedAlternatives) {
                pending->second.routePending = true;
                lastMacroStatus_ = "build-route-pending";
                return false;
            }
            const auto routeUnsafe = !builderRoute.profile.reachable ||
                builderRoute.profile.peakThreat > 0.25;
            const auto builderDistance =
                leasedBuilder->getDistance(toBwapiPosition(routeTarget));
            pending->second.unsafeRoute = action.target == UnitKind::nexus &&
                routeUnsafe && (builderDistance > 256 ||
                    (pending->second.prepositioned && builderDistance > 96));
            if (builderRoute.profile.reachable && builderRoute.profile.peakThreat <= 0.25) {
                // The current profile proves this selected route, not an old
                // detour that was no longer selected or validated.
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
            const auto previousCommand = leasedBuilder->getLastCommand();
            const auto holdingTravelWaypoint = constructionWaypointMakingProgress(
                pending->second.waypointProgress, Broodwar->getFrameCount(),
                builderPosition, previousCommand.getType() == BWAPI::UnitCommandTypes::Move,
                leasedBuilder->getOrder() == BWAPI::Orders::Move,
                fromBwapi(previousCommand.getTargetPosition()));
            const auto legDestination = pending->second.routeWaypoint.valid()
                ? pending->second.routeWaypoint : routeDestination;
            const MovementFootprint builderFootprint{
                leasedBuilder->getType().dimensionLeft(),
                leasedBuilder->getType().dimensionRight(),
                leasedBuilder->getType().dimensionUp(),
                leasedBuilder->getType().dimensionDown()};
            const auto stagingTarget = (holdingTravelWaypoint ||
                trafficWaypointHold(pending->second, influence, navigation))
                ? pending->second.travelWaypoint
                : nextGroundRouteWaypoint(navigation, builderPosition, legDestination,
                                          builderFootprint, 7,
                                          &constructionRouteWork_, static_cast<std::uint64_t>(leasedBuilder->getID()));
            if (!stagingTarget.valid()) {
                pending->second.routePending = true;
                lastMacroStatus_ = "build-route-pending";
                return false;
            }
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
                    prepositionOnly ? "expansion-preparation-travel"
                        : pending->second.routeWaypoint.valid() ||
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
        if (prepositionOnly) {
            lastMacroStatus_ = "build-ready-awaiting-funds";
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
    if (relocatableSupplyPylon(action.target, action.reason, constructionSite.valid()))
        home = supplyConstructionHome(home);
    if (action.target == UnitKind::assimilator) {
        home = constructionSite.valid() ? toBwapiPosition(constructionSite.anchor)
                                       : refineryConstructionHome(home, influence);
        if (!home.isValid()) {
            lastMacroStatus_ = "build-no-location-refinery-site";
            return false;
        }
    }
    // Builder route checks share the placement anchor, including emergency
    // supply at a safer base, rather than a separate forward rally.
    auto near = toBwapiPosition(constructionSite.valid()
        ? constructionSite.anchor
        : constructionBuilderAnchor(action.target, fromBwapi(home), plan.constructionRallyPoint(),
                                     plan.expansionTarget));
    if (!constructionSite.valid() &&
        (action.target == UnitKind::photonCannon || action.target == UnitKind::shieldBattery)) {
        const auto context = staticDefenseConstructionContext(type, plan);
        if (context.base != nullptr) near = context.anchor;
    }
    auto rejectedForUnsafeRoute = false;
    auto routePending = false;
    BWAPI::Unit pendingCandidate = nullptr;
    const auto builder = findBuilder(type, near, influence, unavailableBuilders, !prepositionOnly,
                                     &rejectedForUnsafeRoute, &routePending,
                                     preferredExpansionBuilder(action), &pendingCandidate);
    if (builder == nullptr) {
        if (routePending && pendingCandidate != nullptr)
            rememberExpansionBuilderPreference(action, pendingCandidate);
        else if (action.target == UnitKind::nexus)
            expansionBuilderPreferences_.erase(taskId);
        lastMacroStatus_ = routePending ? "build-route-pending"
            : rejectedForUnsafeRoute && action.target == UnitKind::nexus
            ? "build-route-danger-no-safe-builder" : "build-no-builder";
        return false;
    }
    if (!prepositionOnly && !Broodwar->canMake(type, builder)) {
        lastMacroStatus_ = "build-cannot-make";
        return false;
    }
    const auto location = buildLocation(action.target, type, builder, plan,
                                        constructionSite, taskId, action.reason, fromBwapi(home));
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
    if (action.target == UnitKind::nexus &&
        !groundConstructionSiteSafe(influence, buildCenter)) {
        recordBuildBlocker(action, BuildBlockerReason::unsafeRoute, 15 * 24);
        lastMacroStatus_ = "build-site-danger";
        return false;
    }
    auto routeDestination = buildCenter;
    if (navigation != nullptr && !navigation->empty()) {
        auto accessStatus = NavigationStatus::invalidInput;
        auto accessPending = false;
        const auto access = nearestGroundConstructionAccessPoint(
            navigation, fromBwapi(builder->getPosition()), buildCenter,
            placementFootprint(type), builderFootprint, 96, &accessStatus, &accessPending,
            &constructionRouteWork_, static_cast<std::uint64_t>(builder->getID()));
        if (access.valid()) {
            routeDestination = access;
        } else if (accessPending) {
            lastMacroStatus_ = "build-route-pending";
            return false;
        } else {
            routeDestination = {-1,-1};
        }
    }
    const auto builderRoute = selectSafeRouteAlternative(
        influence, fromBwapi(builder->getPosition()), routeDestination,
        false, navigation, builderFootprint, 0.4125, {-1, -1}, 0.25,
        &constructionRouteWork_, static_cast<std::uint64_t>(builder->getID()));
    if ((!builderRoute.profile.reachable || builderRoute.profile.peakThreat > 0.25) &&
        (builderRoute.profile.searchBudgetExhausted || builderRoute.unresolvedAlternatives)) {
        lastMacroStatus_ = "build-route-pending";
        return false;
    }
    const auto builderPosition = fromBwapi(builder->getPosition());
    const auto distantBuilder = builder->getDistance(buildCenterBw) > 256;
    const auto routeNeedsStaging = (distantBuilder || prepositionOnly) &&
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
            navigation, builderPosition, legDestination, builderFootprint, 7,
            &constructionRouteWork_, static_cast<std::uint64_t>(builder->getID()));
        if (!travelWaypoint.valid()) {
            lastMacroStatus_ = "build-route-pending";
            return false;
        }
        auto resourcesPaid = false;
        const auto moveAccepted = executeConstructionCommand(
            builder->getID(), CommandType::move, action.target, travelWaypoint,
            action.priority, prepositionOnly ? "expansion-preparation-travel"
                : builderRoute.waypoint.valid() || travelWaypoint != routeDestination
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
