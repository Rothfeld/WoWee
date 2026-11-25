#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::rendering {

/// A byte-budgeted cache of shared values whose users pin what they hold.
///
/// Written for the terrain renderer's tileset textures
/// (tileset\elwynn\elwynngrassbase.blp is drawn by thousands of chunks, and
/// each chunk's descriptor set names the image), but it knows nothing of
/// Vulkan: the value is whatever the owner stores.
///
/// The rules:
///   - an entry with pins is never evicted, whatever the budget says;
///   - among unpinned entries the least recently used go first, and only as
///     many as it takes for the requested bytes to fit;
///   - evicted values are handed back to the caller, who decides when they
///     can actually be destroyed (the terrain renderer waits for the frame
///     fences);
///   - only what is inserted is counted. Values that belong to one user alone
///     (a chunk's alpha maps) do not belong here: nothing looks them up
///     again, so the cache could only hold them dead.
///
/// The cost of making room depends on the number of entries, not on the
/// number of users: pins are counted where they are taken and released
/// instead of being found by walking every user.
template <class Key, class Value>
class PinnedLruCache {
public:
    struct Entry {
        Value value{};
        size_t bytes = 0;
        uint64_t lastUse = 0;
        uint32_t pins = 0;
    };

    void setBudgetBytes(size_t bytes) { budgetBytes_ = bytes; }
    [[nodiscard]] size_t budgetBytes() const { return budgetBytes_; }
    [[nodiscard]] size_t bytes() const { return bytes_; }
    [[nodiscard]] size_t size() const { return entries_.size(); }
    [[nodiscard]] size_t pinnedEntries() const { return pinnedEntries_; }
    [[nodiscard]] bool contains(const Key& key) const { return entries_.find(key) != entries_.end(); }

    /// The entry under `key`, marked as just used, or nullptr.
    ///
    /// The pointer stays valid until the entry is evicted, which cannot
    /// happen while it is pinned.
    [[nodiscard]] Entry* find(const Key& key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return nullptr;
        it->second.lastUse = ++counter_;
        return &it->second;
    }

    /// Add a value the budget has room for (see makeRoom). The key must be new.
    Entry& insert(Key key, Value value, size_t bytes) {
        auto [it, inserted] = entries_.try_emplace(std::move(key));
        assert(inserted && "PinnedLruCache::insert: key already present");
        it->second.value = std::move(value);
        it->second.bytes = bytes;
        it->second.lastUse = ++counter_;
        bytes_ += bytes;
        return it->second;
    }

    void pin(Entry& entry) {
        if (entry.pins++ == 0) ++pinnedEntries_;
    }

    void unpin(Entry& entry) {
        assert(entry.pins > 0 && "PinnedLruCache::unpin without a pin");
        if (--entry.pins == 0) --pinnedEntries_;
    }

    /// Evict least recently used unpinned entries until `needBytes` more fit.
    ///
    /// Returns whether they now fit. The evicted values are appended to
    /// `evicted`; nothing is evicted when the bytes already fit.
    bool makeRoom(size_t needBytes, std::vector<Value>& evicted) {
        if (fits(needBytes)) return true;

        std::vector<std::pair<uint64_t, const Key*>> candidates;
        candidates.reserve(entries_.size() - pinnedEntries_);
        for (const auto& [key, entry] : entries_) {
            if (entry.pins == 0) candidates.emplace_back(entry.lastUse, &key);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& [lastUse, key] : candidates) {
            if (fits(needBytes)) break;
            auto it = entries_.find(*key);
            bytes_ -= it->second.bytes;
            evicted.push_back(std::move(it->second.value));
            entries_.erase(it);
        }
        return fits(needBytes);
    }

    /// Hand every value to `fn` and empty the cache. For shutdown, when no
    /// user is left; pins are not consulted.
    template <class Fn>
    void drain(Fn&& fn) {
        for (auto& [key, entry] : entries_) fn(entry.value);
        entries_.clear();
        bytes_ = 0;
        pinnedEntries_ = 0;
        counter_ = 0;
    }

private:
    [[nodiscard]] bool fits(size_t needBytes) const { return bytes_ + needBytes <= budgetBytes_; }

    // Node-based, so Entry pointers survive rehashing.
    std::unordered_map<Key, Entry> entries_;
    size_t bytes_ = 0;
    size_t budgetBytes_ = 0;
    size_t pinnedEntries_ = 0;
    uint64_t counter_ = 0;
};

} // namespace wowee::rendering
