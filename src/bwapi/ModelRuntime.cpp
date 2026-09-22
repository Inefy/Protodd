#include "ModelRuntime.hpp"

#include <chrono>
#include <fstream>
#include <BWAPI.h>
#include <algorithm>

namespace protodd::bwapi {

void ModelRuntime::start(std::ostream& log) {
    policy_.clear(); observations_.reset();
    std::ifstream modeFile("bwapi-data/read/LearnedMacro-mode.txt");
    std::string mode;
    std::getline(modeFile, mode);
    if (!mode.empty() && mode.back() == '\r') mode.pop_back();
    if (mode.empty() || mode == "off") return;
    if (mode != "shadow") { log << "MODEL,status=disabled,reason=unsupported-mode\n"; return; }
    std::ifstream input("bwapi-data/read/LearnedMacro.bin", std::ios::binary);
    std::string error;
    if (!policy_.load(input, error)) {
        log << "MODEL,status=disabled,reason=" << error << '\n'; return;
    }
    log << "MODEL,status=shadow,schema=" << macroSchemaVersion << ",fingerprint="
        << macroSchemaFingerprint() << ",parameters=" << policy_.parameterCount() << '\n';
}

void ModelRuntime::observe(const GameState& state) {
    if (!enabled()) return;
    std::vector<UnitId> removals;
    // BwapiBridge sorts its legal memory by ID. Avoid a quadratic scan now
    // that brief sightings must be captured on every frame.
    for (const auto& known : observations_.rememberedEnemies()) {
        if (!std::ranges::binary_search(state.enemy.units, known.id, {}, &UnitSnapshot::id))
            removals.push_back(known.id);
    }
    observations_.observe(state, removals, [](Position position) {
        const BWAPI::TilePosition tile(position.x / 32, position.y / 32);
        return tile.isValid() && BWAPI::Broodwar->isVisible(tile);
    });
}

void ModelRuntime::infer(Frame frame, const FrameBudget& budget, std::ostream& log) {
    if (!enabled() || !observations_.inferenceDue(frame)) return;
    if (!observations_.beginInference(frame, budget)) {
        log << "MODEL_SHADOW_SKIPPED,frame=" << frame << ",observation_frame="
            << observations_.sampleFrame() << ",reason=runtime-load\n";
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto mask = observations_.mask();
    const auto prediction = policy_.predict(observations_.features(), mask);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (!prediction) {
        log << "MODEL,status=disabled,frame=" << frame << ",reason=invalid-observation\n";
        policy_.clear(); return;
    }
    log << "MODEL_SHADOW,frame=" << frame << ",observation_frame=" << observations_.sampleFrame()
        << ",action=" << learnedIntents()[prediction->action].name
        << ",probability=" << prediction->probability << ",mask=" << mask << ",elapsed_us=" << elapsed << '\n';
    // Stop shadow work that would jeopardize the real controller. Re-enabling
    // requires a new game with the same explicit shadow configuration.
    if (elapsed > 10000) {
        log << "MODEL,status=disabled,reason=inference-budget\n";
        policy_.clear();
    }
}
}  // namespace protodd::bwapi
