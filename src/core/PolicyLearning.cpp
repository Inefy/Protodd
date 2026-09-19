#include "protodd/PolicyLearning.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace protodd {
namespace {
constexpr std::size_t maxContext = 4096;
constexpr std::size_t maxStates = 100000;
constexpr std::size_t maxSnapshot = 64 * 1024 * 1024;

bool validContext(std::string_view context) {
    return !context.empty() && context.size() <= maxContext;
}

bool validConfig(const PolicyLearningConfig& c) {
    const auto unit = [](double x) { return std::isfinite(x) && x >= 0.0 && x <= 1.0; };
    const auto bound = [](double x) { return std::isfinite(x) && x > 0.0 && x <= 1.0e6; };
    return unit(c.alpha) && c.alpha > 0.0 && unit(c.gamma) && unit(c.epsilon) &&
           bound(c.rewardLimit) && bound(c.valueLimit);
}

// Specified unsigned arithmetic, independent of standard-library distributions
// and std::hash implementations. All randomness is local to a choose call.
std::uint64_t randomWord(std::uint64_t& seed) {
    seed += UINT64_C(0x9e3779b97f4a7c15);
    auto x = seed;
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
}  // namespace

PolicyLearner::PolicyLearner(PolicyLearningConfig config) : config_(config) {
    if (!validConfig(config)) throw std::invalid_argument("Invalid policy learning config");
}

std::string PolicyLearner::makeContext(std::string_view ownRace,
    std::string_view opponentRace, std::string_view rulesetVersion) {
    if (ownRace.empty() || opponentRace.empty() || rulesetVersion.empty())
        throw std::invalid_argument("Policy context requires both races and ruleset");
    std::string result = "race-context-v1:";
    for (auto field : {ownRace, opponentRace, rulesetVersion}) {
        if (field.size() > maxContext) throw std::invalid_argument("Policy context too long");
        result += std::to_string(field.size()) + ':';
        result.append(field);
    }
    if (!validContext(result)) throw std::invalid_argument("Policy context too long");
    return result;
}

PolicyLearner::Values PolicyLearner::lookup(std::string_view context, int state) const {
    const auto found = values_.find({std::string(context), state});
    return found == values_.end() ? Values{} : found->second;
}

double PolicyLearner::qValue(std::string_view context, int state, PolicyAction action) const {
    if (!validContext(context) || policyActionBit(action) == 0) return 0.0;
    return lookup(context, state)[static_cast<std::size_t>(action)];
}

std::optional<PolicyAction> PolicyLearner::choose(std::string_view context, int state,
    std::uint64_t seed, PolicyActionMask allowed, bool training) const {
    allowed &= allPolicyActions;
    if (!allowed || !validContext(context)) return std::nullopt;
    const auto values = lookup(context, state);
    std::array<PolicyAction, 4> legal{};
    std::size_t count = 0;
    auto best = PolicyAction::count;
    double bestValue = -std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto action = static_cast<PolicyAction>(i);
        if (!(allowed & policyActionBit(action))) continue;
        legal[count++] = action;
        if (values[i] > bestValue) { best = action; bestValue = values[i]; }
    }
    if (training && !frozen_ && config_.epsilon > 0.0) {
        for (unsigned char byte : context) {
            seed ^= byte;
            seed *= UINT64_C(0x100000001b3);
        }
        seed ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(state));
        const double draw = static_cast<double>(randomWord(seed) >> 11) * 0x1.0p-53;
        if (draw < config_.epsilon) {
            const auto size = static_cast<std::uint64_t>(count);
            const auto threshold = (std::uint64_t{0} - size) % size;
            auto word = randomWord(seed);
            while (word < threshold) word = randomWord(seed);
            return legal[static_cast<std::size_t>(word % size)];
        }
    }
    return best;
}

bool PolicyLearner::update(std::string_view context, int state, PolicyAction action,
    double reward, int nextState, PolicyActionMask nextAllowed, bool terminal) {
    if (frozen_ || !validContext(context) || policyActionBit(action) == 0 ||
        !std::isfinite(reward)) return false;
    const Key key{std::string(context), state};
    if (values_.size() >= maxStates && values_.find(key) == values_.end()) return false;
    double bootstrap = 0.0;
    if (!terminal) {
        const auto best = choose(context, nextState, 0, nextAllowed, false);
        if (best) bootstrap = qValue(context, nextState, *best);
    }
    const auto target = std::clamp(std::clamp(reward, -config_.rewardLimit, config_.rewardLimit) +
        config_.gamma * bootstrap, -config_.valueLimit, config_.valueLimit);
    auto& value = values_[key][static_cast<std::size_t>(action)];
    value = std::clamp((1.0 - config_.alpha) * value + config_.alpha * target,
                       -config_.valueLimit, config_.valueLimit);
    return true;
}

std::string PolicyLearner::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    out << "PROTODD_POLICY 1\n" << config_.alpha << ' ' << config_.gamma << ' '
        << config_.epsilon << ' ' << config_.rewardLimit << ' ' << config_.valueLimit
        << '\n' << values_.size() << '\n';
    for (const auto& [key, values] : values_) {
        out << std::quoted(key.first) << ' ' << key.second;
        for (double value : values) out << ' ' << value;
        out << '\n';
    }
    return out.str();
}

bool PolicyLearner::parse(std::string_view snapshot) {
    if (snapshot.size() > maxSnapshot) return false;
    std::istringstream in{std::string(snapshot)};
    in.imbue(std::locale::classic());
    std::string magic;
    int version = 0;
    PolicyLearningConfig config;
    std::size_t rows = 0;
    if (!(in >> magic >> version) || magic != "PROTODD_POLICY" || version != 1 ||
        !(in >> config.alpha >> config.gamma >> config.epsilon >> config.rewardLimit >>
          config.valueLimit >> rows) || !validConfig(config) || rows > maxStates) return false;
    std::map<Key, Values> parsed;
    for (std::size_t i = 0; i < rows; ++i) {
        Key key;
        Values values{};
        in >> std::ws;
        if (in.peek() != '"' || !(in >> std::quoted(key.first) >> key.second) ||
            !validContext(key.first)) return false;
        for (auto& value : values) {
            if (!(in >> value) || !std::isfinite(value) || std::abs(value) > config.valueLimit)
                return false;
        }
        if (!parsed.emplace(std::move(key), values).second) return false;
    }
    in >> std::ws;
    if (!in.eof()) return false;
    config_ = config;
    values_.swap(parsed);
    return true;
}

}  // namespace protodd
