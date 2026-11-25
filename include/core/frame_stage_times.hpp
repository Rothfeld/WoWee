#pragma once

// Time spent in parts of a frame that the application's own stage timers
// cannot see into, noted where the work happens and collected once a frame.
//
// Application::runRenderStage times a lambda in application.cpp. The widget
// tree's passes, the script handlers and the engine's per-frame sweeps run
// inside those lambdas or outside every one of them, several times a frame,
// from files that know nothing of the application. They note their time here
// under a stage name; Application drains the notes at the end of the frame
// into the same "frame budget over 10s" report as its own stages.
//
// Examples of names noted: "scripts/OnUpdate", "scripts/OnEvent",
// "addonWidgets/layout/sizing", "addonWidgets/engine/visibility",
// "frameXml->draw/tooltips".
//
// A stage noted here overlaps the application stage that contains it
// ("scripts/OnEvent" runs inside "gameHandler->update"), so the lines of the
// report do not add up to the frame.
//
// Main thread only: the notes are thread-local and only the thread that runs
// Application::render drains them.

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace wowee::core {

struct FrameStageNote {
    double ms = 0.0;        ///< summed over the calls of this frame
    uint64_t calls = 0;     ///< how many times it was noted this frame
};

struct FrameStageNotes {
    /// Stage name to this frame's time. Entries stay once made, zeroed by the
    /// drain, so a steady frame allocates nothing.
    std::map<std::string, FrameStageNote, std::less<>> stages;
    /// Plain counts ("layout passes with nothing moved"), summed over the
    /// report window by the application.
    std::map<std::string, uint64_t, std::less<>> counts;
    /// Per handler owner, kept over the whole report window and only while
    /// detail is on (WOWEE_FRAME_PROFILE): "OnUpdate WorldFrame",
    /// "OnEvent UNIT_AURA".
    std::map<std::string, FrameStageNote, std::less<>> detail;
    bool detailEnabled = false;
};

inline FrameStageNotes& frameStageNotes() {
    thread_local FrameStageNotes notes;
    return notes;
}

inline void noteFrameStage(std::string_view stage, double ms, uint64_t calls = 1) {
    auto& stages = frameStageNotes().stages;
    auto it = stages.find(stage);
    if (it == stages.end()) it = stages.emplace(std::string(stage), FrameStageNote{}).first;
    it->second.ms += ms;
    it->second.calls += calls;
}

inline void noteFrameCount(std::string_view name, uint64_t n) {
    auto& counts = frameStageNotes().counts;
    auto it = counts.find(name);
    if (it == counts.end()) it = counts.emplace(std::string(name), 0).first;
    it->second += n;
}

inline bool frameStageDetailEnabled() { return frameStageNotes().detailEnabled; }

inline void noteFrameStageDetail(std::string_view key, double ms) {
    auto& detail = frameStageNotes().detail;
    auto it = detail.find(key);
    if (it == detail.end()) it = detail.emplace(std::string(key), FrameStageNote{}).first;
    it->second.ms += ms;
    it->second.calls += 1;
}

/// Notes the time between its construction and destruction under `stage`.
class ScopedFrameStage {
public:
    explicit ScopedFrameStage(const char* stage)
        : stage_(stage), start_(std::chrono::steady_clock::now()) {}
    ScopedFrameStage(const ScopedFrameStage&) = delete;
    ScopedFrameStage& operator=(const ScopedFrameStage&) = delete;
    ~ScopedFrameStage() {
        noteFrameStage(stage_, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_).count());
    }

private:
    const char* stage_;
    std::chrono::steady_clock::time_point start_;
};

/// Cuts a stretch of code into consecutive phases and notes each one's time.
///
/// For a loop body with many early exits (`continue`): `phase("a/b")` ends the
/// phase before it and starts "a/b"; leaving the scope ends the last one, so
/// an exit anywhere is charged to the phase it happened in. Does nothing
/// unless detail is on (WOWEE_FRAME_PROFILE), so it can stay in a loop that
/// runs for every unit around the player.
///
/// Example, from the nameplate pass: "uiManager->render/nameplates/model
/// position", ".../label visibility", ".../name text".
class FrameStagePhases {
public:
    FrameStagePhases() : enabled_(frameStageDetailEnabled()) {}
    FrameStagePhases(const FrameStagePhases&) = delete;
    FrameStagePhases& operator=(const FrameStagePhases&) = delete;
    ~FrameStagePhases() { phase(nullptr); }

    void phase(const char* next) {
        if (!enabled_) return;
        const auto now = std::chrono::steady_clock::now();
        if (current_) {
            noteFrameStage(current_, std::chrono::duration<double, std::milli>(now - start_).count());
        }
        current_ = next;
        start_ = now;
    }

private:
    bool enabled_;
    const char* current_ = nullptr;
    std::chrono::steady_clock::time_point start_{};
};

} // namespace wowee::core
