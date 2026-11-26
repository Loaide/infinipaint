#pragma once
#include <cstdint>

namespace engine::document {
// Conservative admission envelope, not a measurement of process RSS or driver
// memory. R is min(physical RAM / 16, 384 MiB) from ActivityManager.MemoryInfo.
// Fail closed on insufficient headroom and keep existing records unchanged.
struct WorkingSet {
    uint64_t limit=0, reserved=0;
    static WorkingSet forSlice(uint64_t r) noexcept {
        if(!r || r>(384ull<<20)) return {};
        // Both history/save may be fully pinned at the same time. Allow 32 MiB
        // for the serialized Java writer, bounded JNI chunks, SQLite/windows
        // and edit planning. One delayed live upload snapshot can overlap the
        // next builder; its capacity cannot exceed the scene limit (+tail).
        return {3*r,(160ull<<20)+r/3+(2ull<<20)};
    }
    bool admits(uint64_t canonicalPayload,uint64_t completedMeshes,uint64_t nodes,
                uint64_t activeMesh,uint64_t activeSamples,uint64_t decoding=0) const noexcept {
        if(!limit || reserved>limit) return false;
        uint64_t room=limit-reserved;
        auto charge=[&](uint64_t n,uint64_t unit) {
            if(n>room/unit) return false;
            room-=n*unit;return true;
        };
        // Count unique canonical payloads even when a history/root keeps them
        // alive. Mesh handles also include the old GL scene until GL retires it.
        // Completed VBO adoption permits at most 1.5x slack; round up to 2x.
        // Active reserve covers CPU, simultaneous old/new growth, upload copy,
        // live GPU growth and index/filter/spline staging.
        return charge(canonicalPayload,1) && charge(completedMeshes,3) &&
               charge(nodes,1024) && charge(activeMesh,5) && charge(activeSamples,640) &&
               charge(decoding,1);
    }
};
}
