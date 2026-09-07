#pragma once

#include "protodd/GameState.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
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

    friend bool operator==(const Command&, const Command&) = default;
};

struct CommandStats {
    std::size_t proposed{};
    std::size_t superseded{};
    std::size_t redundant{};
    std::size_t budgetDeferred{};
};

class CommandBus {
public:
    void beginFrame(Frame frame, int latencyFrames);
    void submit(Command command);
    [[nodiscard]] std::vector<Command> finalize(
        std::size_t maximumCommands = std::numeric_limits<std::size_t>::max());
    void markIssued(const Command& command);
    void clear();
    [[nodiscard]] const CommandStats& stats() const noexcept { return stats_; }

private:
    struct IssuedCommand {
        Command command;
        Frame frame{};
    };

    Frame frame_{};
    int latencyFrames_{};
    std::vector<Command> pending_;
    std::unordered_map<UnitId, IssuedCommand> lastIssued_;
    std::size_t fairnessCursor_{};
    CommandStats stats_;

    [[nodiscard]] bool redundant(const Command& command) const;
};

}  // namespace protodd
