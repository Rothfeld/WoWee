#pragma once

// The CVars kept between runs: the rows of cvars.cfg, held in memory.
//
// Example rows: `nameplateshowenemies=1`, `unitnamenpc=0`, `uiscale=1.28`.
// Keys are lower case; a key the player and the interface never set has no
// row, and its reader supplies the default.
//
// The rule this type exists for is in value(): once load() has run, the rows
// in memory answer and the file is not opened. The file is written whole from
// the rows on every change, so after load() it holds nothing the rows do not.
// Before load() - a reader that runs ahead of the interface, which is what
// reading the file was written for - the file is read.
//
// It was read for every key without a row at any time. The nameplate pass
// asks for about eight such keys per unit, every frame: with 93 units around
// that was 650 opens of cvars.cfg and 4.2 ms a frame
// (_experiments/20261125T192530Z.present_nameplate_cost).

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wowee::addons {

class StoredCVars {
public:
    using Rows = std::unordered_map<std::string, std::string>;

    [[nodiscard]] Rows& rows() { return rows_; }
    [[nodiscard]] const Rows& rows() const { return rows_; }
    [[nodiscard]] bool loaded() const { return loaded_; }

    /// Reads `path` into rows() and returns how many rows it held. A missing
    /// file holds none; either way the rows are what answers from now on.
    size_t load(const std::string& path) {
        size_t count = 0;
        if (std::ifstream in(path); in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                rows_[lowered(std::string_view(line).substr(0, eq))] = line.substr(eq + 1);
                ++count;
            }
        }
        loaded_ = true;
        return count;
    }

    /// The stored value of `key` (any case), or `fallback` when it has none.
    /// `path` is read only before load() has run.
    [[nodiscard]] std::string value(std::string_view key, std::string_view fallback,
                                    const std::string& path) const {
        // One buffer per thread for the lower-cased key: this runs several
        // hundred times a frame.
        thread_local std::string wanted;
        wanted.assign(key);
        for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (const auto it = rows_.find(wanted); it != rows_.end()) return it->second;
        if (loaded_) return std::string(fallback);

        std::ifstream in(path);
        if (!in.is_open()) return std::string(fallback);
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            if (lowered(std::string_view(line).substr(0, eq)) == wanted) return line.substr(eq + 1);
        }
        return std::string(fallback);
    }

private:
    static std::string lowered(std::string_view text) {
        std::string out(text);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    Rows rows_;
    bool loaded_ = false;
};

} // namespace wowee::addons
