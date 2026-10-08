#include "protodd/BuildCancellation.hpp"
#include "protodd/Combat.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
using namespace protodd;
int failures{};
int checks{};

void check(const bool condition, const std::string_view message) {
    ++checks;
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

class DeterministicRandom {
public:
    explicit DeterministicRandom(const std::uint32_t seed) : state_(seed) {}

    std::uint32_t next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return state_;
    }

    int between(const int minimum, const int maximum) noexcept {
        const auto width = static_cast<std::uint32_t>(maximum - minimum + 1);
        return minimum + static_cast<int>(next() % width);
    }

private:
    std::uint32_t state_;
};

bool validLedger(const ResourceLedger& ledger) {
    return ledger.minerals >= 0 && ledger.gas >= 0 &&
        ledger.reservedMinerals >= 0 && ledger.reservedMinerals <= ledger.minerals &&
        ledger.reservedGas >= 0 && ledger.reservedGas <= ledger.gas &&
        ledger.protectedMinerals >= 0 &&
        ledger.protectedMinerals <= ledger.reservedMinerals &&
        ledger.protectedGas >= 0 && ledger.protectedGas <= ledger.reservedGas &&
        ledger.committedMinerals >= 0 &&
        ledger.committedMinerals == ledger.reservedMinerals - ledger.protectedMinerals &&
        ledger.committedGas >= 0 &&
        ledger.committedGas == ledger.reservedGas - ledger.protectedGas &&
        ledger.freeMinerals() >= 0 && ledger.freeGas() >= 0;
}

bool uniqueActors(const std::span<const Command> commands) {
    std::unordered_set<UnitId> actors;
    for (const auto& command : commands) {
        if (!actors.insert(command.actor).second) return false;
    }
    return true;
}

void resourceLedgerBounds() {
    DeterministicRandom random{0x61A7D00DU};
    ResourceLedger malformed{10, 4, 11, 0, 0, 0, 0, 0};
    check(!validLedger(malformed), "invariant checker rejects an injected over-reservation");

    for (int caseIndex = 0; caseIndex < 96; ++caseIndex) {
        ResourceLedger ledger;
        ledger.beginFrame(random.between(-100, 350), random.between(-80, 220));
        check(validLedger(ledger), "negative and positive observations normalize to a valid ledger");
        for (int event = 0; event < 64; ++event) {
            const auto minerals = random.between(-12, 90);
            const auto gas = random.between(-12, 70);
            switch (random.between(0, 6)) {
                case 0:
                    static_cast<void>(ledger.reserve(minerals, gas));
                    break;
                case 1:
                    ledger.protect(minerals, gas);
                    break;
                case 2:
                    static_cast<void>(ledger.spendCommitted(minerals, gas));
                    break;
                case 3:
                    static_cast<void>(ledger.releaseCommitted(minerals, gas));
                    break;
                case 4:
                    static_cast<void>(ledger.spendAvailable(minerals, gas));
                    break;
                case 5:
                    static_cast<void>(ledger.canSpendCommitted(minerals, gas));
                    break;
                default:
                    ledger.beginFrame(random.between(-100, 350), random.between(-80, 220));
                    break;
            }
            check(validLedger(ledger), "bounded resource events preserve nonnegative funded obligations");
        }
    }
}

Command proposal(const UnitId actor, const int urgency, const int frame,
                 const int deadline, const CommandOwner owner) {
    Command command{actor, CommandType::move, -1, {actor * 8, frame},
                    UnitKind::unknown, urgency, frame - 1, "generated-proposal"};
    command.owner = owner;
    command.urgency = urgency;
    command.deadlineFrame = deadline;
    return command;
}

void commandAuthorityAndStaleIds() {
    DeterministicRandom random{0xC04D8A11U};
    constexpr std::array owners{
        CommandOwner::worker, CommandOwner::scouting, CommandOwner::combat,
        CommandOwner::construction, CommandOwner::maintenance};

    for (int caseIndex = 0; caseIndex < 80; ++caseIndex) {
        CommandBus bus;
        const auto frame = 100 + caseIndex;
        bus.beginFrame(frame, random.between(0, 12));
        std::array<int, 9> expectedUrgency;
        expectedUrgency.fill(-100000);
        std::vector<Command> generated;
        for (UnitId actor = 1; actor <= 8; ++actor) {
            const auto proposalCount = random.between(1, 7);
            for (int index = 0; index < proposalCount; ++index) {
                const auto urgency = random.between(-30, 160);
                const auto deadline = random.between(0, 4) == 0 ? frame - 1 : frame + 2;
                auto command = proposal(actor, urgency, frame,
                                        deadline, owners[static_cast<std::size_t>(random.between(0, 4))]);
                if (deadline >= frame) {
                    expectedUrgency[static_cast<std::size_t>(actor)] = std::max(
                        expectedUrgency[static_cast<std::size_t>(actor)], urgency);
                }
                generated.push_back(std::move(command));
            }
        }
        for (auto remaining = generated.size(); remaining > 1; --remaining) {
            const auto selected = static_cast<std::size_t>(random.next() % remaining);
            std::swap(generated[remaining - 1], generated[selected]);
        }
        for (auto& command : generated) bus.submit(std::move(command));
        auto winners = bus.finalize();
        check(uniqueActors(bus.authorityWinners()),
              "generated cross-owner proposals yield one authority winner per actor");
        check(winners.size() == bus.authorityWinners().size(),
              "non-issued generated proposals have a matching executable winner");
        for (const auto& winner : bus.authorityWinners()) {
            check(expectedUrgency[static_cast<std::size_t>(winner.actor)] == winner.effectiveUrgency(),
                  "arbitration selects the highest nonexpired urgency for each actor");
        }
        const std::array injectedDuplicate{
            proposal(4, 30, frame, frame + 1, CommandOwner::worker),
            proposal(4, 31, frame, frame + 1, CommandOwner::combat)};
        check(!uniqueActors(injectedDuplicate),
              "invariant checker rejects an injected duplicate-authority counterexample");
    }

    CommandBus bus;
    const auto oldLease = proposal(77, 80, 300, -1, CommandOwner::scouting);
    auto issuedLease = oldLease;
    issuedLease.leaseGeneration = 8;
    bus.beginFrame(300, 0);
    bus.markIssued(issuedLease);
    auto staleLease = oldLease;
    staleLease.leaseGeneration = 7;
    bus.beginFrame(301, 0);
    bus.submit(staleLease);
    check(bus.finalize().empty() && bus.stats().staleLease == 1,
          "older lease generations cannot reclaim an actor after newer authority was issued");

    bus.forgetUnit(77);
    staleLease.leaseGeneration = 1;
    bus.submit(staleLease);
    check(bus.finalize().size() == 1,
          "forgetting a unit removes its stale generation before a new identity reuses the ID");

    bus.clear();
    auto targeted = proposal(88, 80, 310, -1, CommandOwner::combat);
    targeted.type = CommandType::attackUnit;
    targeted.targetUnit = 77;
    bus.beginFrame(310, 0);
    bus.markIssued(targeted);
    check(!bus.pendingEffectFeedback().empty(), "issued command starts tracked effect feedback");
    bus.forgetUnit(77);
    check(bus.pendingEffectFeedback().empty(),
          "forgetting a stale target removes feedback that refers to that unit ID");
}

void cancellationEventOrders() {
    for (std::uint32_t seed = 1; seed <= 80; ++seed) {
        DeterministicRandom random{seed * 0x9E3779B9U};
        BuildCancellation cancellation;
        bool expectedAwaiting = false;
        Frame lastRequest = -1;
        Frame frame = 0;
        const auto latency = random.between(0, 30);
        const auto retryAfter = std::max(12, latency + 6);

        for (int event = 0; event < 96; ++event) {
            frame += random.between(0, 8);
            const bool due = lastRequest < 0 || frame - lastRequest >= retryAfter;
            check(cancellation.requestDue(frame) == due,
                  "generated event order preserves the command retry deadline");
            check(cancellation.retryDue(frame) == (expectedAwaiting && due),
                  "retry eligibility requires both an accepted request and elapsed latency");

            switch (random.between(0, 2)) {
                case 0: {
                    const bool accepted = random.between(0, 1) != 0;
                    const bool result = cancellation.request(frame, latency, accepted);
                    lastRequest = frame;
                    expectedAwaiting = expectedAwaiting || accepted;
                    check(result == accepted && cancellation.awaiting() == expectedAwaiting,
                          "rejected retries do not discard an earlier accepted cancellation");
                    break;
                }
                case 1: {
                    const bool oldOrderActionable = random.between(0, 1) != 0;
                    const bool expectedAcknowledgment = expectedAwaiting && !oldOrderActionable;
                    const bool acknowledged = cancellation.acknowledged(oldOrderActionable);
                    check(acknowledged == expectedAcknowledgment,
                          "only a cleared old engine order acknowledges cancellation");
                    if (expectedAcknowledgment) {
                        expectedAwaiting = false;
                        lastRequest = -1;
                    }
                    check(cancellation.awaiting() == expectedAwaiting,
                          "actionable orders retain their builder lease through interruption");
                    break;
                }
                default:
                    break;
            }
        }
    }
}

void impossibleNavigationInputs() {
    constexpr int width = 12;
    constexpr int height = 10;
    constexpr int cell = 32;
    NavigationGrid grid{width, height, cell,
        std::vector<std::uint8_t>(width * height, 1U)};
    constexpr Position validStart{16, 16};
    DeterministicRandom random{0x1BADB002U};

    for (int index = 0; index < 192; ++index) {
        const auto side = random.between(0, 3);
        Position impossible;
        switch (side) {
            case 0: impossible = {-1 - random.between(0, 1000), random.between(0, height * cell - 1)}; break;
            case 1: impossible = {width * cell + random.between(0, 1000), random.between(0, height * cell - 1)}; break;
            case 2: impossible = {random.between(0, width * cell - 1), -1 - random.between(0, 1000)}; break;
            default: impossible = {random.between(0, width * cell - 1), height * cell + random.between(0, 1000)}; break;
        }
        const auto path = grid.findPath(validStart, impossible, 100);
        const auto waypoint = grid.nextWaypoint(validStart, impossible, 7, 100);
        check(path.status == NavigationStatus::invalidInput && path.points.empty(),
              "out-of-map goals never produce a route fallback");
        check(waypoint.status == NavigationStatus::invalidInput && !waypoint.hasUsableWaypoint(),
              "out-of-map goals never produce an actionable waypoint");

        const auto invalidStart = grid.findPath(impossible, validStart, 100);
        check(invalidStart.status == NavigationStatus::invalidInput && invalidStart.points.empty(),
              "out-of-map starts are rejected before path expansion");
    }
}

UnitSnapshot combatUnit(const UnitId id, const UnitKind kind, const Position position,
                        const bool ours) {
    UnitSnapshot unit;
    unit.id = id;
    unit.kind = kind;
    unit.position = position;
    unit.lastPosition = position;
    unit.ours = ours;
    unit.visible = unit.detected = unit.completed = true;
    unit.powered = true;
    unit.hitPoints = unit.maxHitPoints = 100;
    unit.groundWeapon = {20, 30, 0, 224, DamageType::normal, false, true, 1};
    return unit;
}

void powerLossPermutations() {
    DeterministicRandom random{0x50A3E11U};
    auto enemy = combatUnit(99, UnitKind::marine, {160, 128}, false);
    const std::array enemies{enemy};
    const std::array cannonKinds{UnitKind::photonCannon, UnitKind::dragoon,
                                 UnitKind::highTemplar, UnitKind::darkTemplar};
    for (const auto kind : cannonKinds) {
        if (!unitStats(kind).requiresPsi ||
            (!isCombatUnit(kind) && !isStaticDefense(kind))) continue;
        std::array poweredUnits{
            combatUnit(1, kind, {128, 128}, true),
            combatUnit(2, kind, {128, 144}, true),
            combatUnit(3, kind, {128, 160}, true)};
        const auto perUnit = CombatEvaluator{}.evaluate(
            std::span<const UnitSnapshot>(&poweredUnits[0], 1), enemies, 1.0, 0.0, false);
        check(perUnit.friendlyPower > 0.0, "powered psi-dependent combat units contribute threat");

        for (int permutation = 0; permutation < 8; ++permutation) {
            int poweredCount = 0;
            for (std::size_t index = 0; index < poweredUnits.size(); ++index) {
                poweredUnits[index].powered = (permutation & (1 << index)) != 0;
                poweredCount += poweredUnits[index].powered ? 1 : 0;
            }
            const auto estimate = CombatEvaluator{}.evaluate(
                poweredUnits, enemies, 1.0, 0.0, false);
            check(std::abs(estimate.friendlyPower - poweredCount * perUnit.friendlyPower) < 1e-9,
                  "power loss removes exactly the affected units from combat authority");
            const auto shuffled = random.between(0, 1) != 0;
            if (shuffled) std::ranges::reverse(poweredUnits);
            const auto reordered = CombatEvaluator{}.evaluate(
                poweredUnits, enemies, 1.0, 0.0, false);
            check(std::abs(reordered.friendlyPower - estimate.friendlyPower) < 1e-9,
                  "powered-unit ordering cannot change aggregate defensive strength");
            if (shuffled) std::ranges::reverse(poweredUnits);
        }
    }
}
}  // namespace

int main() {
    resourceLedgerBounds();
    commandAuthorityAndStaleIds();
    cancellationEventOrders();
    impossibleNavigationInputs();
    powerLossPermutations();
    std::cout << "Invariant and adversarial checks: " << checks << " checks, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
