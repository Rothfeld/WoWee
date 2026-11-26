#pragma once

#include <cstddef>
#include <vector>

namespace wowee {
namespace rendering {

/// Descriptor sets of one kind from as many equal pools as the live sets need.
///
/// A descriptor pool's host memory is taken when the pool is created, for its
/// whole capacity: the terrain renderer's single pool for 163,840 material
/// sets cost 145 MB on NVIDIA 595.99 (888 bytes a set) with 20,000 to 29,000
/// sets in use. Here a pool is created when no existing one can serve.
///
/// The backend handed to allocate() supplies the two Vulkan calls, so the rule
/// is testable without a device (TerrainRenderer's is in terrain_renderer.cpp):
///     Pool createPool();        // a null Pool when creation fails
///     Set allocate(Pool pool);  // a null Set when this pool cannot serve
/// A pool that cannot serve is the pool's own answer
/// (VK_ERROR_OUT_OF_POOL_MEMORY / VK_ERROR_FRAGMENTED_POOL), not a count kept
/// here: sets go back to their pool behind the frame fences, outside this
/// class, so a count here would run ahead of the pool.
///
/// Sets are freed by the caller with the pool returned beside them. Pools are
/// not destroyed before destroyAll(): an emptied pool serves the next tiles.
template <typename Pool, typename Set>
class DescriptorPoolChain {
public:
    struct Allocation {
        Set set{};
        Pool pool{};
        explicit operator bool() const { return set != Set{}; }
    };

    /// A set from the pool that served last, else from any other, else from a
    /// new pool. Empty when a new pool could not be created or could not serve.
    template <typename B>
    Allocation allocate(B&& backend) {
        for (size_t i = 0; i < pools_.size(); ++i) {
            const size_t index = (last_ + i) % pools_.size();
            if (Set set = backend.allocate(pools_[index]); set != Set{}) {
                last_ = index;
                return Allocation{set, pools_[index]};
            }
        }
        Pool pool = backend.createPool();
        if (pool == Pool{}) return {};
        pools_.push_back(pool);
        last_ = pools_.size() - 1;
        return Allocation{backend.allocate(pool), pool};
    }

    [[nodiscard]] const std::vector<Pool>& pools() const { return pools_; }

    /// Hands every pool to `destroy` and forgets them.
    template <typename Destroy>
    void destroyAll(Destroy&& destroy) {
        for (Pool pool : pools_) destroy(pool);
        pools_.clear();
        last_ = 0;
    }

private:
    std::vector<Pool> pools_;
    size_t last_ = 0;
};

} // namespace rendering
} // namespace wowee
