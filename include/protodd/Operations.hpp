#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/Strategy.hpp"

#include <vector>

namespace protodd {

// Feedback contains our own construction state, never hidden enemy information.
struct ExpansionFeedback {
    Position site{-1, -1};
    bool pending{};
    Frame stalledFrames{};
    bool rejectedFootprint{};
    bool noSafeBuilder{};
    bool noPlacement{};
    bool unsafeRoute{};
};

class ExpansionCoordinator {
public:
    void reset() noexcept;
    void update(StrategicPlan& plan, const GameState& state,
                const ExpansionFeedback& feedback);
    [[nodiscard]] std::string_view reason() const noexcept { return reason_; }
    [[nodiscard]] bool releaseBuilder() const noexcept { return releaseBuilder_; }
private:
    Frame retryAfter_{};
    std::vector<Position> failedSites_;
    Position committedSite_{-1, -1};
    Frame unsafeRouteSince_{-1};
    Frame unsafeRouteLastFrame_{-1};
    Position unsafeRouteSite_{-1, -1};
    std::string_view reason_{"No expansion mission"};
    bool releaseBuilder_{};
};

[[nodiscard]] Position expansionAssemblyPoint(const GameState& state, Position site,
                                              Position home) noexcept;
[[nodiscard]] std::vector<Command> clearExpansionFootprint(
    const GameState& state, Position site, Position assembly,
    bool constructionPending);

}  // namespace protodd
