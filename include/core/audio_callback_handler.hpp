#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <optional>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }
namespace audio { class AudioCoordinator; class CreatureSoundIndex; }
namespace pipeline { class AssetManager; }
namespace ui { class UIManager; }

namespace core {

/// Handles audio-related callbacks: music, sound effects, level-up, achievement, LFG.
class AudioCallbackHandler {
public:
    AudioCallbackHandler(pipeline::AssetManager& assetManager,
                         audio::AudioCoordinator* audioCoordinator,
                         rendering::Renderer* renderer,
                         ui::UIManager* uiManager,
                         game::GameHandler& gameHandler);
    ~AudioCallbackHandler();

    void setupCallbacks();

private:
    pipeline::AssetManager& assetManager_;
    audio::AudioCoordinator* audioCoordinator_;
    rendering::Renderer* renderer_;
    ui::UIManager* uiManager_;
    game::GameHandler& gameHandler_;
    std::unique_ptr<audio::CreatureSoundIndex> creatureSounds_;
    bool creatureSoundsLoaded_ = false;
};

} // namespace core
} // namespace wowee
