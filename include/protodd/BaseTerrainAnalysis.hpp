#pragma once

#include "protodd/Navigation.hpp"

#include <cmath>
#include <span>
#include <vector>

namespace protodd {

enum class BaseTerrainWork { homeDistance, defense, enemyDistance };
struct BaseTerrainUpdate {
    std::size_t site{};
    BaseTerrainWork work{};
    int groundDistance{-1};
    bool reachabilityKnown{};
    DefensivePosition defense{};
};

// Immutable terrain analysis. One owned frontier and postprocessing cursor
// survive callback boundaries; no pending prefix is published as a result.
// Work counts search operations, length segments and cross-section samples.
// Allocations and individual clearance proofs are not wall-clock guarantees.
class BaseTerrainAnalysis {
public:
    void reset() {
        sites_.clear(); jobs_.clear(); cursor_ = 0; enemyScheduled_ = false;
        search_.reset(); routeSettled_ = false; route_ = nullptr; lengthVertex_ = 1;
        length_ = 0; defense_ = {}; lastWork_ = 0;
    }
    void initialize(Position home, std::span<const Position> sites,
                    std::span<const Position> approaches) {
        reset(); sites_.assign(sites.begin(), sites.end());
        // Publish all home distances before optional defensive scans.
        for (std::size_t site = 0; site < sites_.size(); ++site)
            jobs_.push_back({site, BaseTerrainWork::homeDistance, home, sites_[site]});
        for (std::size_t site = 0; site < sites_.size(); ++site)
            for (const auto approach : approaches) {
                if (distanceSquared(sites_[site], approach) < 384 * 384) continue;
                jobs_.push_back({site, BaseTerrainWork::defense, sites_[site], approach});
            }
    }
    void scheduleEnemy(Position enemyMain) {
        if (!enemyMain.valid() || enemyScheduled_) return;
        for (std::size_t site = 0; site < sites_.size(); ++site)
            jobs_.push_back({site, BaseTerrainWork::enemyDistance, enemyMain, sites_[site]});
        enemyScheduled_ = true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return jobs_.size() - cursor_; }
    [[nodiscard]] std::size_t lastWork() const noexcept { return lastWork_; }
    [[nodiscard]] std::vector<BaseTerrainUpdate> advance(
        const NavigationGrid& terrain, const int maximumWork = 2048) {
        std::vector<BaseTerrainUpdate> updates;
        lastWork_ = 0;
        const auto allowance = static_cast<std::size_t>(std::max(0, maximumWork));
        while (cursor_ < jobs_.size() && lastWork_ < allowance) {
            const auto& job = jobs_[cursor_];
            if (!routeSettled_) {
                const auto& route = search_.advance(terrain, job.from, job.to,
                    static_cast<int>(allowance - lastWork_));
                lastWork_ += search_.diagnostics().stepWork;
                if (route.searchBudgetExhausted) break;
                routeSettled_ = true;
                route_ = &route;
            }
            // The search retains its exact completed path while the bounded
            // length/defense analysis advances; no large vector is copied.
            const auto& route = *route_;
            BaseTerrainUpdate update{job.site, job.work};
            update.reachabilityKnown = route.reached() || route.status == NavigationStatus::unreachable;
            if (route.reached() && job.work == BaseTerrainWork::defense) {
                const auto vertexCost = static_cast<std::size_t>(7 + 2 * (384 / terrain.cellSize()));
                while (defense_.nextVertex + 4 < route.points.size() &&
                       vertexCost <= allowance - lastWork_) {
                    terrain.scanDefensivePosition(job.from, route.points, defense_, 1);
                    lastWork_ += vertexCost;
                }
                if (defense_.nextVertex + 4 < route.points.size()) break;
                update.defense = defense_.best;
            } else if (route.reached()) {
                while (lengthVertex_ < route.points.size() && lastWork_ < allowance) {
                    length_ += distance(route.points[lengthVertex_-1], route.points[lengthVertex_]);
                    ++lengthVertex_; ++lastWork_;
                }
                if (lengthVertex_ < route.points.size()) break;
                update.groundDistance = static_cast<int>(std::lround(length_));
            }
            if (lastWork_ >= allowance) break;
            ++lastWork_; // Even invalid/empty jobs charge publication work.
            updates.push_back(update);
            ++cursor_; routeSettled_ = false; lengthVertex_ = 1; length_ = 0; defense_ = {};
            // Changing endpoints resets the next frontier in advance; keep
            // its bounded workspace reusable instead of reallocating it.
        }
        return updates;
    }
private:
    struct Job { std::size_t site; BaseTerrainWork work; Position from; Position to; };
    std::vector<Position> sites_;
    std::vector<Job> jobs_;
    std::size_t cursor_{}, lengthVertex_{1}, lastWork_{};
    bool enemyScheduled_{}, routeSettled_{};
    double length_{};
    NavigationSearch search_;
    const NavigationPathResult* route_{};
    DefensivePositionScan defense_;
};

} // namespace protodd
