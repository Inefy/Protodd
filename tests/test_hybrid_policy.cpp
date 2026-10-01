#include "protodd/HybridPolicy.hpp"

#include <iostream>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
void check(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot unit(int id, bool ours, Position position) {
    UnitSnapshot result;
    result.id = id; result.kind = UnitKind::dragoon; result.position = position;
    result.ours = ours; result.completed = result.visible = result.detected = true;
    result.hitPoints = result.maxHitPoints = 100;
    result.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    return result;
}
}

int main() {
    Squad squad;
    squad.units = {unit(1, true, {512, 512})};
    std::vector targets{unit(2, false, {600, 512})};
    CombatEstimate estimate;
    estimate.decision = FightDecision::engage;
    Command proposal;
    proposal.actor = 1; proposal.targetUnit = 2; proposal.type = CommandType::attackUnit;
    DefenseArea defense;
    const auto accept = [&] { return hybridCombatProposal(proposal, 100, 104, squad, estimate, targets, defense); };
    auto accepted = accept();
    check(accepted && accepted->priority == 81 && accepted->source == "hybrid-trained",
          "learned immediate shot participates in ordinary arbitration");
    check(!hybridCombatProposal(proposal, 100, 124, squad, estimate, targets, defense), "expires at 24 frames");
    check(!hybridCombatProposal(proposal, 105, 104, squad, estimate, targets, defense), "future proposal rejected");
    for (const auto kind : {UnitKind::probe, UnitKind::gateway, UnitKind::observer,
                           UnitKind::shuttle, UnitKind::highTemplar, UnitKind::reaver}) {
        squad.units[0].kind = kind;
        check(!accept(), "economy, detector, transport, and specialist ownership preserved");
    }
    squad.units[0].kind = UnitKind::dragoon;
    for (const auto type : {CommandType::move, CommandType::attackMove, CommandType::stop,
                           CommandType::train, CommandType::build, CommandType::useTech}) {
        proposal.type = type;
        check(!accept(), "learned controller cannot replace strategic or production orders");
    }
    proposal.type = CommandType::attackUnit;
    for (auto member : {&UnitSnapshot::loaded, &UnitSnapshot::disabled, &UnitSnapshot::hallucination,
                        &UnitSnapshot::attackFrame, &UnitSnapshot::attackWindup, &UnitSnapshot::underStorm}) {
        squad.units[0].*member = true;
        check(!accept(), "unsafe or protected actor rejected");
        squad.units[0].*member = false;
    }
    squad.units[0].weaponCooldown = 1;
    check(!accept(), "kite cycle remains native");
    squad.units[0].weaponCooldown = 0;
    for (const auto decision : {FightDecision::kite, FightDecision::retreat}) {
        estimate.decision = decision; check(!accept(), "losing or kiting fight cannot be overridden");
    }
    estimate.decision = FightDecision::engage;
    estimate.advanceBlocked = true; check(!accept(), "mobile detection wait preserved"); estimate.advanceBlocked = false;
    estimate.holdScreen = true; check(!accept(), "screen preserved"); estimate.holdScreen = false;
    squad.withdrawing = true; check(!accept(), "withdrawal preserved"); squad.withdrawing = false;
    squad.emergencyDefense = true; check(!accept(), "emergency ownership preserved"); squad.emergencyDefense = false;
    targets[0].visible = false; check(!accept(), "hidden target rejected"); targets[0].visible = true;
    targets[0].detected = false; check(!accept(), "undetected target rejected"); targets[0].detected = true;
    targets[0].invincible = true; check(!accept(), "invincible target rejected"); targets[0].invincible = false;
    targets[0].incomingDamage = 100; check(!accept(), "already lethal projectile avoids overkill"); targets[0].incomingDamage = 0;
    targets[0].position = {1000, 512}; check(!accept(), "long-range chase rejected"); targets[0].position = {600, 512};
    targets[0].flying = true; check(!accept(), "weapon capability enforced"); targets[0].flying = false;
    defense.center = {512, 512}; defense.pursuitRadius = 32;
    check(!accept(), "defensive boundary preserved"); defense = {};
    proposal.targetUnit = 999; check(!accept(), "target outside squad candidates rejected"); proposal.targetUnit = 2;
    proposal.actor = 999; check(!accept(), "actor outside squad rejected"); proposal.actor = 1;

    for (const int priority : {82, 84, 85, 94, 100, 109}) {
        CommandBus bus;
        bus.beginFrame(104, 2);
        auto safety = proposal;
        safety.type = CommandType::move; safety.priority = priority; safety.source = "native-safety";
        bus.submit(*accept()); bus.submit(safety);
        const auto selected = bus.finalize();
        check(selected.size() == 1 && selected.front().source == "native-safety", "native safety outranks trained target");
    }
    CommandBus bus;
    bus.beginFrame(104, 2);
    auto native = proposal; native.priority = 80; native.source = "focus-fire";
    bus.submit(native); bus.submit(*accept());
    check(bus.finalize().front().source == "hybrid-trained", "learned target can replace ordinary focus-fire");
    return failures == 0 ? 0 : 1;
}
