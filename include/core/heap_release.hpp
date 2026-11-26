#pragma once

#include <chrono>
#include <optional>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace wowee {
namespace core {

/// When a burst of work is over: true once, after `busy` has been seen and has
/// then stayed false for `settle`.
///
/// Used to pick the moment for releaseFreedHeapPages(). Terrain streaming
/// allocates in bursts (a world load, a teleport, a tile border crossed) on
/// several threads and frees most of it again when the tiles are finalized
/// and the tiles left behind are unloaded. The moment after the burst is when
/// the allocator holds the most memory nobody uses, and when nothing is about
/// to ask for it back.
class WorkSettledTrigger {
public:
    explicit WorkSettledTrigger(std::chrono::milliseconds settle) : settle_(settle) {}

    /// Call with the current state, every frame or less often.
    [[nodiscard]] bool update(bool busy, std::chrono::steady_clock::time_point now) {
        if (busy) {
            sawWork_ = true;
            idleSince_.reset();
            return false;
        }
        if (!sawWork_) return false;
        if (!idleSince_) idleSince_ = now;
        if (now - *idleSince_ < settle_) return false;
        sawWork_ = false;
        idleSince_.reset();
        return true;
    }

private:
    std::chrono::milliseconds settle_;
    bool sawWork_ = false;
    std::optional<std::chrono::steady_clock::time_point> idleSince_;
};

#ifdef __GLIBC__
/// Give the free pages inside glibc's malloc arenas back to the system.
///
/// free() returns memory to the arena it came from and glibc only shrinks the
/// top of an arena on its own; whole free pages below the top stay resident.
/// Tiles are prepared on worker threads (one arena each) and freed from the
/// main thread, so after a burst each arena is left with hundreds of MB that
/// only its own thread could reuse. malloc_trim walks every arena and
/// MADV_DONTNEEDs those pages; they come back zero-filled when the arena
/// next uses them. Other C libraries have no such call and are not served.
inline void releaseFreedHeapPages() { malloc_trim(0); }
#endif

} // namespace core
} // namespace wowee
