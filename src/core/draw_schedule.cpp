#include "core/draw_schedule.hpp"

#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>

namespace wowee::core {

DrawSchedule::DrawSchedule(std::uint32_t every) : every_(every) {
    if (every_ == 0) throw std::invalid_argument("DrawSchedule: every must be at least 1");
}

std::uint32_t DrawSchedule::everyFromEnvironment(const char* value) {
    if (value == nullptr || *value == '\0') return 1;
    std::uint32_t every = 0;
    const char* end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, every);
    if (parsed.ec != std::errc{} || parsed.ptr != end || every < 1 || every > 1000) {
        throw std::invalid_argument("WOWEE_DRAW_EVERY must be a whole number from 1 to 1000, got '" +
                                    std::string(value) + "'");
    }
    return every;
}

void DrawSchedule::request(bool draw) {
    requested_ = draw;
}

bool DrawSchedule::beginTick(bool inWorld) {
    bool draw = true;
    if (inWorld) {
        draw = worldTick_ == 0;
        worldTick_ = (worldTick_ + 1) % every_;
    } else {
        worldTick_ = 0;
    }
    if (requested_) {
        draw = *requested_;
        requested_.reset();
    }
    ticks_ += 1;
    if (draw) drawnTicks_ += 1;
    return draw;
}

} // namespace wowee::core
