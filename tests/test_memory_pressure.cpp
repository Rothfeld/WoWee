// Memory pressure levels used by terrain streaming back-off.
#include <catch_amalgamated.hpp>

#include "core/memory_monitor.hpp"

using wowee::core::MemoryMonitor;

TEST_CASE("severe memory pressure is the stricter level", "[memory]") {
    constexpr size_t MB = 1024 * 1024;
    constexpr size_t total = 91860 * MB;
    // The 2026-11-17 13:43 reading: 13.4% available.
    CHECK(MemoryMonitor::memoryPressureAt(12294 * MB, total));
    CHECK_FALSE(MemoryMonitor::severeMemoryPressureAt(12294 * MB, total));
    for (size_t pct = 0; pct <= 100; ++pct) {
        CAPTURE(pct);
        const size_t available = total / 100 * pct;
        if (MemoryMonitor::severeMemoryPressureAt(available, total))
            CHECK(MemoryMonitor::memoryPressureAt(available, total));
    }
    CHECK_FALSE(MemoryMonitor::memoryPressureAt(total / 5, total));
}
