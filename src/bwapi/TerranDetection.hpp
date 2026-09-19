#pragma once
namespace protodd::bwapi {
// Base insurance against Protoss tech, after initial economy/army only.
inline bool wantsTerranDetection(bool enemyProtoss, int frame, int workers, int marines) {
    return enemyProtoss && frame >= 24 * 240 && workers >= 14 && marines >= 8;
}
}
