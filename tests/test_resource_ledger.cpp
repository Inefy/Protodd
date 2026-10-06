#include "protodd/MacroPlanner.hpp"

#include <iostream>

int main() {
    using protodd::ResourceLedger;

    int failures = 0;
    const auto check = [&failures](const bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    ResourceLedger ledger{500, 150};
    check(ledger.reserve(100, 0), "macro can hold the imminent Pylon cost");
    check(ledger.reserve(150, 100), "macro can hold detection and research obligations");
    ledger.protect(0, 25);
    check(ledger.freeMinerals() == 250 && ledger.freeGas() == 25,
          "only resources beyond held obligations remain available");
    check(!ledger.canReserve(251, 0) && !ledger.canReserve(0, 26),
          "model and maintenance cannot plan against reserved funds");

    check(ledger.canSpendCommitted(100, 0),
          "macro can consume its own accepted Pylon reservation");
    check(ledger.spendCommitted(100, 0),
          "accepted macro spend is charged exactly once");
    check(ledger.minerals == 400 && ledger.reservedMinerals == 150 &&
              ledger.freeMinerals() == 250,
          "spending a funded action leaves detection funds held and surplus unchanged");

    check(ledger.spendAvailable(200, 25),
          "accepted model production can spend only current surplus");
    check(ledger.spendAvailable(15, 0) && ledger.spendAvailable(15, 0) &&
              ledger.spendAvailable(15, 0),
          "maintenance Scarabs or Interceptors share the same mineral balance");
    check(!ledger.spendAvailable(15, 0),
          "maintenance cannot consume the final minerals held for detection");
    check(ledger.minerals == 155 && ledger.gas == 125 &&
              ledger.reservedMinerals == 150 && ledger.reservedGas == 125,
          "macro, model, and ammunition costs reconcile in one ledger");

    ledger.beginFrame(155, 125);
    check(ledger.freeMinerals() == 5 && ledger.freeGas() == 0 &&
              ledger.committedMinerals == 150 && ledger.protectedGas == 25,
          "new observations preserve unspent obligations across frames");
    check(!ledger.spendAvailable(6, 0),
          "an unrelated accepted-command candidate cannot double-allocate held funds");

    ResourceLedger paidBuild{166, 0};
    check(paidBuild.reserve(150, 0) && paidBuild.freeMinerals() == 16,
          "reconciliation initially recognizes an outstanding construction cost");
    check(paidBuild.releaseCommitted(150, 0) && paidBuild.freeMinerals() == 166 &&
              paidBuild.reservedMinerals == 0,
          "a construction cost confirmed paid by BWAPI releases its duplicate hold");
    check(paidBuild.spendAvailable(100, 0),
          "model production can use bank resources after a pending build was charged");

    return failures == 0 ? 0 : 1;
}
