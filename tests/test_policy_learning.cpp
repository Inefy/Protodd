#include "protodd/PolicyLearning.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-10; }
}

int main() {
    using namespace protodd;
    constexpr auto balanced = PolicyAction::balanced;
    constexpr auto pressure = PolicyAction::pressure;
    constexpr auto economy = PolicyAction::economy;
    constexpr auto defend = PolicyAction::defend;
    constexpr auto all = allPolicyActions;
    const auto context = PolicyLearner::makeContext("protoss", "terran", "coarse-v1");
    PolicyLearner learner({0.5, 0.9, 1.0, 10.0, 20.0});
    expect(learner.choose(context, 0, 0, all) == balanced, "cold evaluation prefers balanced");
    expect(learner.stateCount() == 0, "choose does not insert unknown states");
    expect(learner.update(context, 1, pressure, 4.0, 999, all, true), "valid terminal update");
    expect(near(learner.qValue(context, 1, pressure), 2.0), "alpha scales terminal reward");
    expect(learner.update(context, 0, economy, 1.0, 1, all, false), "valid bootstrapped update");
    expect(near(learner.qValue(context, 0, economy), 1.4), "Q update uses gamma and next state max");
    expect(learner.update(context, 0, economy, 1.0, 1, all, false), "repeat update");
    expect(near(learner.qValue(context, 0, economy), 2.1), "Q update retains old estimate");
    expect(learner.choose(context, 0, 1, all) == economy, "evaluation exploits learned Q");
    expect(learner.update(context, 2, balanced, 1.0, 1, policyActionBit(defend), false), "masked target update");
    expect(near(learner.qValue(context, 2, balanced), 0.5), "illegal next pressure cannot bootstrap");
    expect(learner.update(context, 3, balanced, 1.0, 1, 0, false), "dead end update");
    expect(near(learner.qValue(context, 3, balanced), 0.5), "empty next mask zero bootstrap");
    expect(learner.update(context, 4, balanced, 1.0, 1, all, true), "terminal update ignores successor");
    expect(near(learner.qValue(context, 4, balanced), 0.5), "terminal never bootstraps");
    expect(!learner.choose(context, 0, 0, 0, true), "empty mask has no unsafe fallback");
    expect(!learner.choose(context, 0, 0, 0x100, true), "unknown mask bits cannot authorize action");
    expect(policyActionBit(PolicyAction::count) == 0 &&
           policyActionBit(static_cast<PolicyAction>(-1)) == 0, "invalid enum masks are safe");

    const auto beforeChoice = learner.serialize();
    std::array<bool, 4> visited{};
    for (std::uint64_t seed = 0; seed < 512; ++seed) {
        const auto chosen = learner.choose(context, 0, seed, all, true);
        expect(chosen == learner.choose(context, 0, seed, all, true), "seed reproducibility");
        if (chosen) visited[static_cast<std::size_t>(*chosen)] = true;
        expect(learner.choose(context, 0, seed, policyActionBit(defend), true) == defend,
               "exploration honors singleton legal mask");
        expect(learner.choose(context, 0, seed, policyActionBit(defend), false) == defend,
               "greedy selection honors singleton legal mask");
        expect(learner.choose(context, 0, seed, all, false) == economy, "evaluation never explores");
    }
    for (bool seen : visited) expect(seen, "epsilon one explores every action");
    expect(learner.serialize() == beforeChoice, "all choices are read-only");
    learner.freeze();
    expect(learner.frozen(), "freeze state exposed");
    expect(learner.choose(context, 0, 42, all, true) == economy, "freeze overrides exploration");
    expect(!learner.update(context, 0, economy, -10, 1, all, true), "freeze rejects training");
    expect(learner.serialize() == beforeChoice, "freeze/update do not alter snapshot");
    learner.freeze(false);

    const auto otherRace = PolicyLearner::makeContext("protoss", "zerg", "coarse-v1");
    const auto otherSelf = PolicyLearner::makeContext("terran", "terran", "coarse-v1");
    const auto otherRules = PolicyLearner::makeContext("protoss", "terran", "coarse-v2");
    for (const auto& other : {otherRace, otherSelf, otherRules}) {
        expect(near(learner.qValue(other, 0, economy), 0), "race and ruleset isolation");
        expect(learner.choose(other, 0, 1, all) == balanced, "unseen context has no borrowed Q");
    }
    expect(PolicyLearner::makeContext("a:b", "c", "d") !=
           PolicyLearner::makeContext("a", "b:c", "d"), "context delimiters cannot collide");
    expect(near(learner.qValue(context, 200, economy), 0), "state buckets isolated");

    PolicyLearner bounded({1.0, 1.0, 0.0, 5.0, 6.0});
    expect(bounded.update(context, -1, defend, 1e300, -1, all, false), "large finite reward accepted");
    expect(near(bounded.qValue(context, -1, defend), 5), "positive reward clipped");
    expect(bounded.update(context, -1, defend, 1e300, -1, all, false), "self transition");
    expect(near(bounded.qValue(context, -1, defend), 6), "value clipped with gamma one");
    expect(bounded.update(context, -2, defend, -1e300, 0, 0, true), "negative finite reward");
    expect(near(bounded.qValue(context, -2, defend), -5), "negative reward clipped");
    expect(bounded.update(context, -3, pressure, 0, -2, policyActionBit(defend), false), "negative successor");
    expect(near(bounded.qValue(context, -3, pressure), -5), "all-negative legal maximum stays negative");
    for (std::uint64_t seed = 0; seed < 20; ++seed)
        expect(bounded.choose(context, -1, seed, all, true) == defend, "epsilon zero exploits");

    const auto unchanged = learner.serialize();
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()})
        expect(!learner.update(context, 0, balanced, bad, 0, all, true), "nonfinite reward rejected");
    expect(!learner.update(context, 0, PolicyAction::count, 1, 0, all, true), "invalid action rejected");
    expect(!learner.update("", 0, balanced, 1, 0, all, true), "empty context rejected");
    expect(!learner.update(std::string(4097, 'x'), 0, balanced, 1, 0, all, true), "oversized context rejected");
    expect(learner.serialize() == unchanged, "invalid updates are transactional");

    PolicyLearner restored;
    restored.freeze();
    expect(restored.parse(learner.serialize()), "snapshot loads while frozen");
    expect(restored.frozen(), "loading preserves local freeze");
    expect(restored.serialize() == learner.serialize(), "exact config and Q round trip");
    restored.freeze(false);
    for (std::uint64_t seed = 0; seed < 20; ++seed)
        expect(restored.choose(context, 0, seed, all, true) == learner.choose(context, 0, seed, all, true),
               "round trip preserves exploration choices");
    const std::string exotic("opaque\n\"\\\0context", 18);
    expect(restored.update(exotic, std::numeric_limits<int>::min(), defend, 0.25, 0, 0, true),
           "opaque contexts and full int buckets accepted");
    PolicyLearner escaped;
    expect(escaped.parse(restored.serialize()), "escaped and binary context round trip");
    expect(escaped.serialize() == restored.serialize(), "lossless context serialization");
    const auto snapshot = restored.serialize();
    const std::string header = "PROTODD_POLICY 1\n0.5 0.9 1 10 20\n";
    for (const auto& bad : std::array<std::string, 16>{
            "", "PROTODD_POLICY 2\n0.5 0.9 1 10 20\n0\n", snapshot + "junk",
            header + "1\n\"x\" 0 nan 0 0 0\n", header + "1\n\"x\" 0 inf 0 0 0\n",
            header + "1\n\"x\" 0 1e999 0 0 0\n", header + "1\n\"x\" 0 21 0 0 0\n",
            header + "1\n\"x\" 2147483648 0 0 0 0\n", header + "1\n\"x\" 0 0 0\n",
            header + "2\n\"x\" 0 0 0 0 0\n\"x\" 0 0 0 0 0\n",
            header + "1\n\"\" 0 0 0 0 0\n", header + "100001\n",
            header + "-1\n", header + "1\nunquoted 0 0 0 0 0\n",
            "PROTODD_POLICY 1\n0.5 nan 1 10 20\n0\n",
            "PROTODD_POLICY 1\n0.5 2 1 10 20\n0\n"}) {
        expect(!restored.parse(bad), "malformed snapshot rejected");
        expect(restored.serialize() == snapshot, "failed parse preserves full learner");
    }
    for (int field = 0; field < 5; ++field) {
        auto config = PolicyLearningConfig{};
        std::array<double*, 5> fields{&config.alpha, &config.gamma, &config.epsilon,
                                     &config.rewardLimit, &config.valueLimit};
        *fields[static_cast<std::size_t>(field)] = std::numeric_limits<double>::quiet_NaN();
        bool threw = false;
        try { PolicyLearner invalid(config); } catch (const std::invalid_argument&) { threw = true; }
        expect(threw, "all nonfinite configuration fields rejected");
    }

    // Offline, validated two-decision episodes: terminal outcome propagates
    // backward through repeated episodes, making an earlier action preferable.
    PolicyLearner episodic({0.5, 0.9, 0.0, 1.0, 20.0});
    for (int episode = 0; episode < 40; ++episode) {
        expect(episodic.update(context, 10, economy, 0, 11, all, false), "episode decision one");
        expect(episodic.update(context, 11, pressure, 1, 12, 0, true), "episode decision two");
    }
    expect(episodic.qValue(context, 10, economy) > 0.89, "delayed credit reaches early decision");
    expect(episodic.choose(context, 10, 0, all) == economy, "learned early policy");
    expect(episodic.choose(context, 11, 0, all) == pressure, "learned later policy");
    expect(episodic.stateCount() == 2, "terminal successor never inserted");
    std::cout << "policy learning: " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
