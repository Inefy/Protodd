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
    // Unknown route analysis is distinct from proved unsafe geometry/threat.
    bool routePending{};
    // Diagnostic attribution only; these do not change coordinator policy.
    std::uint64_t taskId{};
    Frame observedAt{-1};
    bool cancelling{};
    // Own arrived prepositioning lease at a currently legal footprint, before
    // a Build command is accepted. This grants no spending or route permission.
    bool readyAwaitingFunds{};
    UnitId builder{-1};
};

class ExpansionCoordinator {
public:
    void reset() noexcept;
    void update(StrategicPlan& plan, const GameState& state,
                const ExpansionFeedback& feedback);
    [[nodiscard]] std::string_view reason() const noexcept { return reason_; }
    [[nodiscard]] bool releaseBuilder() const noexcept { return releaseBuilder_; }
    [[nodiscard]] Frame routeKnowledgeWaitFrames() const noexcept { return routeKnowledgeWaitFrames_; }
    [[nodiscard]] bool routeKnowledgeTimedOut() const noexcept { return routeKnowledgeTimedOut_; }
    [[nodiscard]] Frame routeAdmissionWaitFrames() const noexcept { return routeAdmissionWaitFrames_; }
    [[nodiscard]] bool routeAdmissionTimedOut() const noexcept { return routeAdmissionTimedOut_; }
private:
    Frame retryAfter_{};
    std::vector<Position> failedSites_;
    Position committedSite_{-1, -1};
    Frame unsafeRouteSince_{-1};
    Frame unsafeRouteLastFrame_{-1};
    Position unsafeRouteSite_{-1, -1};
    Frame routeWaitSince_{-1};
    Frame routeWaitLastFrame_{-1};
    Position routeWaitSite_{-1, -1};
    Frame routeKnowledgeWaitFrames_{-1};
    bool routeKnowledgeTimedOut_{};
    Frame routeAdmissionSince_{-1};
    Frame routeAdmissionLastFrame_{-1};
    Position routeAdmissionSite_{-1, -1};
    Frame routeAdmissionWaitFrames_{-1};
    bool routeAdmissionTimedOut_{};
    std::string_view reason_{"No expansion mission"};
    bool releaseBuilder_{};
};

[[nodiscard]] Position expansionAssemblyPoint(const GameState& state, Position site,
                                              Position home) noexcept;
[[nodiscard]] std::vector<Command> clearExpansionFootprint(
    const GameState& state, Position site, Position assembly,
    bool constructionPending);

}  // namespace protodd
