#include "protodd/UrgentEvents.hpp"
#include "protodd/FrameSchedule.hpp"

#include <array>
#include <iostream>

namespace {

bool check(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;
    UrgentEventQueue queue;
    passed &= check(queue.pending() == 0, "urgent lane begins empty");
    queue.enqueue(UrgentEvent::cloakedThreat);
    queue.enqueue(UrgentEvent::cloakedThreat);
    queue.enqueue(UrgentEvent::workerLineBreach);
    const auto threatEvents = queue.consume();
    const auto threatWork = urgentWorkFor(threatEvents);
    passed &= check(threatWork.updateWorkers && threatWork.updateCombat &&
                        !threatWork.updateMacro,
                    "coalesced local and cloaked threats run worker and combat safety");
    passed &= check(queue.pending() == 0 && queue.consume() == 0,
                    "urgent work is consumed once rather than replayed forever");

    queue.enqueue(UrgentEvent::builderLost);
    queue.enqueue(UrgentEvent::powerSourceLost);
    const auto economyWork = urgentWorkFor(queue.consume());
    passed &= check(economyWork.updateMacro && economyWork.updateWorkers &&
                        !economyWork.updateCombat,
                    "builder and power loss trigger immediate macro recovery");

    queue.enqueue(UrgentEvent::areaDamage);
    const auto areaWork = urgentWorkFor(queue.consume());
    passed &= check(areaWork.updateWorkers && areaWork.updateCombat &&
                        !areaWork.updateMacro,
                    "area damage requests immediate evasion and combat safety");

    constexpr Frame afterScheduledUpdate = 23;
    const auto macroScheduled = frame_schedule::macroCadenceDue(afterScheduledUpdate);
    const auto workersScheduled = frame_schedule::workersDue(afterScheduledUpdate);
    const auto combatScheduled = afterScheduledUpdate % 12 == 0;
    passed &= check(!macroScheduled && !workersScheduled && !combatScheduled,
                    "event fixture arrives after scheduled work and before the next periodic pass");
    queue.enqueue(UrgentEvent::areaDamage);
    const auto nextDecision = frameWorkFor(queue.consume(), macroScheduled, false,
                                           workersScheduled, combatScheduled);
    passed &= check(nextDecision.updateWorkers && nextDecision.updateCombat &&
                        !nextDecision.updateMacro,
                    "area damage bypasses the remaining worker and combat cadence on the next frame");
    queue.enqueue(UrgentEvent::builderLost);
    const auto lostBuilderDispatch = frameWorkFor(queue.consume(), false, false,
                                                  false, false);
    passed &= check(lostBuilderDispatch.updateMacro && lostBuilderDispatch.updateWorkers,
                    "builder loss bypasses the macro cadence at its next decision opportunity");

    const std::array current{Position{400, 400}};
    const std::array none{Position{-1, -1}};
    std::array<UnitSnapshot, 1> friendlies{};
    friendlies[0].position = {520, 400};
    passed &= check(hasNewAreaDamageNearFriendlies(current, none, friendlies),
                    "new Storm inside friendly danger radius triggers urgent work");
    const std::array sameStorm{Position{402, 398}};
    passed &= check(!hasNewAreaDamageNearFriendlies(current, sameStorm, friendlies),
                    "one continuing Storm does not enqueue an urgent pass every frame");
    friendlies[0].position = {900, 900};
    passed &= check(!hasNewAreaDamageNearFriendlies(current, none, friendlies),
                    "distant area damage does not dispatch local urgent work");
    friendlies[0].position = {-1, -1};
    passed &= check(!hasNewAreaDamageNearFriendlies(current, none, friendlies),
                    "invalid friendly positions cannot create an urgent event");

    std::array<UnitSnapshot, 3> state{};
    state[0].role = UnitRole::resourceDepot;
    state[0].position = {1200, 1200};
    state[1].role = UnitRole::worker;
    state[1].position = {1250, 1200};
    auto enemy = UnitSnapshot{};
    enemy.id = 77;
    enemy.visible = true;
    enemy.position = {32 * 160, 32 * 160};
    enemy.groundWeapon.damage = 8;
    passed &= check(!isWorkerLineThreat(enemy, state),
                    "an armed enemy at its own main is not a local worker-line event");
    enemy.position = {1270, 1200};
    passed &= check(isWorkerLineThreat(enemy, state),
                    "the same enemy entering a Nexus worker line is recognized");
    const std::array<UnitId, 1> priorThreat{12};
    const std::array<UnitId, 2> currentThreats{12, 77};
    passed &= check(hasNewUrgentUnitIds(currentThreats, priorThreat),
                    "a newly entering enemy triggers even if another threat remains");
    passed &= check(!hasNewUrgentUnitIds(priorThreat, priorThreat),
                    "a persistent threat does not enqueue repeated urgent passes");
    return passed ? 0 : 1;
}
