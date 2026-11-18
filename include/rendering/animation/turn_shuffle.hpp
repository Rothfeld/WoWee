#pragma once

#include "rendering/animation/animation_ids.hpp"
#include <cstdint>

namespace wowee {
namespace rendering {

// Feet of a standing unit turning in place: ShuffleLeft (11) toward a left
// turn, ShuffleRight (12) toward a right one. A shuffle cycle in which the
// unit turned is followed by another; a cycle without a turn ends the
// shuffle, so the feet keep stepping for up to a cycle after the turn stops
// and a one-frame turn still steps once. Build 8606 does this for its own
// character (keyboard and mouse-look) and for other players it sees turn.
// Pure logic: the caller supplies the facing change and the playing animation.
class TurnShuffle {
public:
    // turnDirection: the unit's facing changed this frame, +1 left
    // (counter-clockwise from above), -1 right, 0 not.
    void start(int turnDirection) {
        active_ = true;
        left_ = turnDirection > 0;
        turnedThisCycle_ = true;
        seen_ = false;
        time_ = 0.0f;
    }

    // Advances a started shuffle by one frame. A reversed turn restarts it
    // toward the new side. Returns false when a cycle passed without a turn:
    // the shuffle is over.
    bool update(int turnDirection, bool haveAnimState, uint32_t currentAnimId, float currentAnimTime) {
        if (!active_) return false;
        if (turnDirection != 0 && (turnDirection > 0) != left_) {
            start(turnDirection);
            return true;
        }
        if (turnDirection != 0) turnedThisCycle_ = true;
        // Each cycle ends by asking whether the unit turned during it;
        // mouse-look turns arrive in steps, not every frame.
        if (cycleDone(haveAnimState, currentAnimId, currentAnimTime)) {
            if (!turnedThisCycle_) {
                active_ = false;
                return false;
            }
            turnedThisCycle_ = false;
        }
        return true;
    }

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] uint32_t animation() const { return left_ ? anim::SHUFFLE_LEFT : anim::SHUFFLE_RIGHT; }
    void reset() { *this = TurnShuffle{}; }

private:
    bool active_ = false;
    bool left_ = false;
    bool turnedThisCycle_ = false;
    bool seen_ = false;
    float time_ = 0.0f;

    // True when the looping shuffle has wrapped back to its start since it
    // was last asked about: a cycle has finished.
    bool cycleDone(bool haveAnimState, uint32_t currentAnimId, float currentAnimTime) {
        if (!haveAnimState) return true;
        if (currentAnimId != animation()) return seen_;
        const bool wrapped = seen_ && currentAnimTime < time_;
        seen_ = true;
        time_ = currentAnimTime;
        return wrapped;
    }
};

} // namespace rendering
} // namespace wowee
