#pragma once

/// Which ticks of the main loop draw a picture.
///
/// Every tick of Application::run reads input, updates the game and runs the
/// interface with its scripts. Drawing - acquiring a swapchain image,
/// recording the world and the interface into a command buffer, submitting
/// and presenting it - is the one part a tick can leave out
/// (docs/draw-schedule.md). This class answers, once per tick, whether the
/// tick that is starting draws.
///
/// Examples:
///   - a normal client: DrawSchedule(1), every tick drawn;
///   - WOWEE_DRAW_EVERY=4: one tick in four drawn while in the world, the
///     first of them the first tick in the world;
///   - a host stepping the client (an agent that reads one picture in four
///     ticks): request(false) / request(true) before each tick.

#include <cstdint>
#include <optional>

namespace wowee::core {

class DrawSchedule {
public:
    /// Draw one tick in `every` while in the world; 1 draws every tick.
    explicit DrawSchedule(std::uint32_t every = 1);

    /// The `every` named by WOWEE_DRAW_EVERY. An unset or empty variable is
    /// 1. Anything but a whole number from 1 to 1000 is a mistake in the
    /// launch and ends the client with a message, as an unparseable switch
    /// would otherwise run a different schedule from the one asked for.
    [[nodiscard]] static std::uint32_t everyFromEnvironment(const char* value);

    /// A host's decision for the next tick to begin. It wins over the count
    /// and holds for that one tick. Outside the world too.
    void request(bool draw);

    /// Called once at the start of each tick, before the update: does this
    /// tick draw? `inWorld` is whether the world is on screen.
    ///
    /// Without a request: outside the world every tick is drawn; in the
    /// world the first tick is drawn and then every `every`-th after it.
    /// Leaving the world and entering it again starts the count over. A
    /// requested tick counts like any other, so a host that speaks for some
    /// ticks only leaves the rest on the same rhythm.
    [[nodiscard]] bool beginTick(bool inWorld);

    [[nodiscard]] std::uint32_t every() const { return every_; }
    /// Ticks begun and, of those, ticks drawn.
    [[nodiscard]] std::uint64_t ticks() const { return ticks_; }
    [[nodiscard]] std::uint64_t drawnTicks() const { return drawnTicks_; }

private:
    std::uint32_t every_;
    std::uint32_t worldTick_ = 0;  // ticks in the world since entering it, modulo every_
    std::optional<bool> requested_;
    std::uint64_t ticks_ = 0;
    std::uint64_t drawnTicks_ = 0;
};

} // namespace wowee::core
