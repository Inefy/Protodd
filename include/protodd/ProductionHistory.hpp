#pragma once
#include "protodd/GameState.hpp"
#include <array>
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace protodd {
// BWAPI Build commands carry the top-left tile; PlaceBuilding orders target
// the footprint center in pixels (also for odd-sized Gateway footprints).
inline Position productionBuildCenter(int tileX,int tileY,int width,int height) {
    if(tileX<0||tileY<0||width<1||height<1)throw std::invalid_argument("invalid build footprint");
    return {tileX*32+width*16,tileY*32+height*16};
}
// Events are recorded when confirmed, never backdated after a sample. A sample
// excludes same-frame events, exactly as production_demands.demand_rows does.
class ProductionHistory {
public:
    void reset() { events_.clear(); lastEvent_ = lastSample_ = -1; }
    void accepted(Frame frame, int action) {
        if (frame < 0 || action < 0 || action >= 8 || frame < lastEvent_ || frame < lastSample_)
            throw std::invalid_argument("invalid production event chronology");
        if (events_.size() >= 8192) throw std::runtime_error("production history capacity");
        events_.push_back({frame, action}); lastEvent_ = frame;
    }
    std::array<float,16> sample(Frame frame) {
        if (frame < 0 || frame < lastSample_) throw std::invalid_argument("backwards production sample");
        lastSample_ = frame;
        while (!events_.empty() && events_.front().frame < frame - 2400) events_.pop_front();
        std::array<float,16> result{};
        for (int i=8;i<16;++i) result[i]=1;
        for (const auto& event : events_) {
            if (event.frame >= frame) break;
            if (event.frame >= frame-240) result[event.action] += .25F;
            result[8+event.action] = static_cast<float>(frame-event.frame)/2400.F;
        }
        for (int i=0;i<8;++i) result[i]=std::min(1.F,result[i]);
        return result;
    }
private:
    struct Event { Frame frame; int action; };
    std::deque<Event> events_;
    Frame lastEvent_{-1}, lastSample_{-1};
};
}
