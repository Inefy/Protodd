#include "WholeGameIntent.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    using namespace protodd;
    cpu::EncodedObservation encoded;
    encoded.entityIds = {10, 11, 12};
    encoded.input.width = encoded.input.height = 32;
    encoded.input.relation = {0, 1, 1};
    whole_observation::Snapshot source;
    for (int id = 10; id <= 12; ++id) {
        whole_observation::Entity entity;
        entity.id = id; entity.relation = id == 10 ? 0 : 1;
        entity.visible = id != 12;
        source.entities.emplace(id, entity);
    }
    cpu::Output output;
    output.heads["event"] = {10};
    output.heads["kind"].assign(cpu::kindNames.size(), 0);
    output.heads["kind"][1] = 5;
    output.heads["domain"].assign(cpu::domainNames.size(), 0);
    output.heads["domain"][0] = 5;
    output.heads["actor"] = {2, 9, 9};
    output.heads["target"] = {0, 2, 10};
    output.heads["target_mode"] = {0, 0, 5};
    output.heads["queued"] = {5, 0};
    output.heads["unit_type"].assign(256, 0);
    output.heads["technology"].assign(44, 0);
    output.heads["upgrade"].assign(61, 0);
    output.heads["queue_slot"].assign(16, 0);
    output.heads["position"] = {0, 0, 0, 0, 0};
    const auto require = [](bool condition) {
        if (!condition) throw std::runtime_error("whole-game intent probe mismatch");
    };
    const auto move = cpu::decodeIntent(output, encoded, source);
    require(move && move->kind == 1 && move->actorIds == std::vector<int>{10});
    require(move->targetPixel && move->targetPixel->first == 512 && move->targetPixel->second == 512);
    output.heads["kind"][3] = 6; // attack_move requires a position.
    output.heads["target_mode"] = {6, 0, 4}; // Independent mode top is incompatible.
    const auto projected = cpu::decodeIntent(output, encoded, source);
    require(projected && projected->kind == 3 && projected->targetMode == 2 &&
            projected->targetPixel);
    output.heads["kind"][3] = 0;
    output.heads["target_mode"] = {0, 5, 0};
    const auto target = cpu::decodeIntent(output, encoded, source);
    require(target && target->targetEntityId == 11);
    output.heads["event"] = {-10};
    require(!cpu::decodeIntent(output, encoded, source));
    output.heads["event"] = {10};
    require(!cpu::decodeIntent(output, encoded, source, {.eventProbability = 0.5f,
                                                           .actorLogit = 3.0f}));
    std::cout << "whole-game intent probe passed\n";
}
