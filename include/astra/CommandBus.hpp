#pragma once

#include "astra/GameState.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace astra {

enum class CommandType : std::uint8_t {
    move,
    attackMove,
    attackUnit,
    train,
    build,
    gather,
    stop,
    hold,
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

    friend bool operator==(const Command&, const Command&) = default;
};

class CommandBus {
public:
    void beginFrame(Frame frame, int latencyFrames);
    void submit(Command command);
    [[nodiscard]] std::vector<Command> finalize(
        std::size_t maximumCommands = std::numeric_limits<std::size_t>::max());
    void markIssued(const Command& command);
    void clear();

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

    [[nodiscard]] bool redundant(const Command& command) const;
};

}  // namespace astra
