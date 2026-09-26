#pragma once

#include <type_traits>

namespace protodd::bwapi {

// Check engine legality before choosing the stable lowest ID. Selecting first
// and then checking canResearch/canUpgrade strands a legal second producer
// behind an unpowered building or one busy with the other kind of operation.
template <class Units, class CanExecute>
auto technologyProducer(const Units& units, CanExecute canExecute) {
    using Unit = std::remove_cvref_t<decltype(*units.begin())>;
    Unit selected = nullptr;
    for (const auto candidate : units) {
        if (candidate == nullptr || !candidate->exists() || !candidate->isCompleted() ||
            !canExecute(candidate)) continue;
        if (selected == nullptr || candidate->getID() < selected->getID()) selected = candidate;
    }
    return selected;
}

} // namespace protodd::bwapi
