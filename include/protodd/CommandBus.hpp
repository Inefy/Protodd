#pragma once

#include "protodd/GameState.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace protodd {

enum class CommandType : std::uint8_t {
    move,
    attackMove,
    attackUnit,
    train,
    build,
    gather,
    stop,
    hold,
    recharge,
    useTech,
    load,
    unload,
    returnCargo,
    trainScarab,
    trainInterceptor,
    feedback,
    mergeArchon,
};

enum class CommandOwner : std::uint8_t {
    unspecified,
    worker,
    scouting,
    combat,
    transport,
    construction,
    maintenance,
    learned,
    observerSafety,
};

struct Command {
    UnitId actor{};
    CommandType type{CommandType::move};
    UnitId targetUnit{-1};
    Position targetPosition{-1, -1};
    UnitKind targetKind{UnitKind::unknown};
    int priority{};
    Frame earliestFrame{};
    std::string source;
    TechnologyKind technology{TechnologyKind::none};
    // The adapter can confirm a persistent engine order is still executing.
    // It retains ownership in arbitration without sending another command.
    bool alreadyActive{};

    // Shared authority envelope. `priority` remains the compatibility value;
    // issuers can migrate to `urgency` explicitly without changing call sites.
    CommandOwner owner{CommandOwner::unspecified};
    int urgency{std::numeric_limits<int>::min()};
    Frame deadlineFrame{-1};
    std::uint64_t leaseGeneration{};

    [[nodiscard]] int effectiveUrgency() const noexcept {
        return urgency == std::numeric_limits<int>::min() ? priority : urgency;
    }

    friend bool operator==(const Command&, const Command&) = default;
};

struct DisplacedCommand {
    Command proposal;
    Command winner;
};

struct CommandEffectFeedback {
    Command command;
    Frame acceptedFrame{};
    bool timeoutReported{};
};

struct CommandStats {
    std::size_t proposed{};
    std::size_t superseded{};
    std::size_t redundant{};
    std::size_t budgetDeferred{};
    std::size_t expired{};
    std::size_t staleLease{};
};

class FrameCommandClaims {
public:
    [[nodiscard]] bool available(Frame frame, UnitId actor);
    void recordAccepted(Frame frame, UnitId actor);
    void forgetUnit(UnitId actor) noexcept;
    void clear() noexcept;

private:
    Frame frame_{-1};
    std::unordered_set<UnitId> acceptedActors_;

    void beginFrame(Frame frame);
};

class CommandBus {
public:
    void beginFrame(Frame frame, int latencyFrames);
    void submit(Command command);
    [[nodiscard]] std::size_t discardPending(UnitId actor, CommandOwner owner);
    [[nodiscard]] std::vector<Command> finalize(
        std::size_t maximumCommands = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] const std::vector<Command>& authorityWinners() const noexcept {
        return authorityWinners_;
    }
    [[nodiscard]] const std::vector<DisplacedCommand>& displacedCommands() const noexcept {
        return displacedCommands_;
    }
    [[nodiscard]] const std::vector<Command>& budgetDeferredCommands() const noexcept {
        return budgetDeferredCommands_;
    }
    [[nodiscard]] Frame budgetDeferralAge(UnitId actor) const noexcept;
    void markIssued(const Command& command);
    [[nodiscard]] std::vector<CommandEffectFeedback> pendingEffectFeedback() const;
    [[nodiscard]] bool markEffectObserved(const Command& command, Frame observedFrame);
    [[nodiscard]] bool markEffectTimeoutReported(const Command& command);
    void clear();
    void forgetUnit(UnitId id);
    [[nodiscard]] const CommandStats& stats() const noexcept { return stats_; }

private:
    struct IssuedCommand {
        Command command;
        Frame frame{};
        bool effectTracked{};
        bool effectObserved{};
        bool effectTimeoutReported{};
        Frame effectFrame{-1};
    };

    Frame frame_{};
    int latencyFrames_{};
    std::vector<Command> pending_;
    std::vector<Command> authorityWinners_;
    std::vector<DisplacedCommand> displacedCommands_;
    std::vector<Command> budgetDeferredCommands_;
    std::unordered_map<UnitId, Frame> budgetDeferredSince_;
    std::unordered_map<UnitId, IssuedCommand> lastIssued_;
    std::unordered_map<UnitId, std::unordered_map<CommandOwner, std::uint64_t>>
        latestLeaseGenerations_;
    std::size_t fairnessCursor_{};
    CommandStats stats_;

    [[nodiscard]] bool redundant(const Command& command) const;
};

}  // namespace protodd
