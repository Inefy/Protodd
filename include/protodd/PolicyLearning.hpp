#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace protodd {

enum class PolicyAction { balanced, pressure, economy, defend, count };
using PolicyActionMask = std::uint32_t;
constexpr PolicyActionMask policyActionBit(PolicyAction action) noexcept {
    const auto index = static_cast<unsigned>(action);
    return index < 4 ? PolicyActionMask{1} << index : 0;
}
inline constexpr PolicyActionMask allPolicyActions = 15;

struct PolicyLearningConfig {
    double alpha = 0.2;
    double gamma = 0.95;
    double epsilon = 0.1;
    double rewardLimit = 1.0;
    double valueLimit = 20.0;
};

// Decision-time lookup and offline, one-transition tabular Q learning. No game
// callbacks, reward inference, persistence I/O, or automatic episode training.
// Runtime must log decisions; the offline caller validates full episodes before
// updating (in particular, a crash/timeout is not evidence of a legitimate win).
class PolicyLearner {
public:
    // Throws invalid_argument for nonfinite/out-of-range configuration.
    explicit PolicyLearner(PolicyLearningConfig config = {});

    // Optional lossless key for sharing across maps/opponents while isolating
    // race matchups and state/action ruleset versions. Context arguments to the
    // other methods are opaque: runtime owns their meaning and state bucketing.
    // Empty fields or a result larger than 4096 bytes throw invalid_argument.
    [[nodiscard]] static std::string makeContext(std::string_view ownRace,
        std::string_view opponentRace, std::string_view rulesetVersion);

    // Never inserts/trains. Unknown states have Q=0; ties prefer enum order.
    // training enables epsilon exploration only, unless frozen. Identical inputs
    // give identical choices across runs; vary seed at each decision to explore.
    // Unknown mask bits are ignored; no legal action/invalid context => nullopt.
    [[nodiscard]] std::optional<PolicyAction> choose(std::string_view context, int state,
        std::uint64_t seed, PolicyActionMask allowed, bool training = false) const;

    // Q <- (1-alpha)*Q + alpha*clamp(reward + gamma*max_legal Q(next)).
    // Reward and Q are bounded. Terminal transitions never bootstrap. An empty
    // next mask also has zero bootstrap. Current action legality must have been
    // checked against the logged decision mask by the offline episode validator.
    // Rejects frozen updates, invalid action/context, nonfinite reward or capacity
    // overflow without mutation. State buckets may be any int, including negative.
    bool update(std::string_view context, int state, PolicyAction action, double reward,
        int nextState, PolicyActionMask nextAllowed, bool terminal);

    void freeze(bool frozen = true) noexcept { frozen_ = frozen; }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }
    [[nodiscard]] double qValue(std::string_view context, int state, PolicyAction action) const;
    [[nodiscard]] std::size_t stateCount() const noexcept { return values_.size(); }

    // Version 1 snapshot includes config and sorted Q rows; no external file I/O.
    // parse replaces only on complete success, rejects duplicates/trailing junk,
    // invalid/nonfinite numbers and oversized input. Freeze is a local runtime
    // setting: it is not serialized and is preserved when loading a snapshot.
    [[nodiscard]] std::string serialize() const;
    bool parse(std::string_view snapshot);

private:
    using Values = std::array<double, 4>;
    using Key = std::pair<std::string, int>;
    PolicyLearningConfig config_;
    std::map<Key, Values> values_;
    bool frozen_ = false;
    [[nodiscard]] Values lookup(std::string_view context, int state) const;
};

}  // namespace protodd
