#pragma once

#include <cstddef>
#include <cstdint>

namespace wowee {
namespace core {

/**
 * Monitors system memory and provides dynamic cache sizing
 */
class MemoryMonitor {
public:
    static MemoryMonitor& getInstance();

    /**
     * Initialize memory monitoring
     */
    void initialize();

    /**
     * Get total system RAM in bytes
     */
    [[nodiscard]] size_t getTotalRAM() const { return totalRAM_; }

    /**
     * Get currently available RAM in bytes
     */
    [[nodiscard]] size_t getAvailableRAM() const;

    /**
     * Get recommended cache budget (80% of available RAM, capped at 90% of total RAM)
     */
    [[nodiscard]] size_t getRecommendedCacheBudget() const;

    /**
     * Check if system is under memory pressure (< 15% RAM available).
     * Terrain streaming narrows to one tile at a time.
     */
    [[nodiscard]] bool isMemoryPressure() const;

    /**
     * Check if system is under severe memory pressure (< 10% RAM available),
     * a subset of memory pressure. Terrain streaming waits longer between
     * tiles; continuing to allocate risks OOM-killing other applications.
     */
    [[nodiscard]] bool isSevereMemoryPressure() const;

    /// The two levels as pure functions of a reading. Severe is the stricter:
    /// every severe reading is also a pressure reading.
    [[nodiscard]] static constexpr bool memoryPressureAt(size_t available, size_t total) {
        return available < total * 15 / 100;
    }
    [[nodiscard]] static constexpr bool severeMemoryPressureAt(size_t available, size_t total) {
        return available < total * 10 / 100;
    }

private:
    MemoryMonitor() = default;
    size_t totalRAM_ = 0;
};

} // namespace core
} // namespace wowee
