#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

class CreatureSoundIndex;
enum class CreatureSoundColumn : uint32_t;

/// The mount's vocal sounds, from its display's CreatureSoundData row
/// (CreatureDisplayInfo.SoundID, else CreatureModelData.SoundID): JumpStart
/// on a jump, JumpEnd on landing, a Fidget sound on an idle fidget.
class MountSoundManager {
public:
    MountSoundManager();
    ~MountSoundManager();

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();
    void update(float deltaTime);

    void onMount(uint32_t creatureDisplayId, bool isFlying, const std::string& modelPath = "");
    void onDismount();

    void setMoving(bool moving) { moving_ = moving; }
    void setFlying(bool flying) { flying_ = flying; }
    void setGrounded(bool grounded) { flying_ = !grounded; }

    void playRearUpSound();
    void playJumpSound();
    void playLandSound();
    void playIdleSound();

    [[nodiscard]] bool isMounted() const { return mounted_; }
    void setVolumeScale(float scale) { volumeScale_ = scale; }
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

private:
    /// Play the SoundEntries row a column of the mount's sound set names.
    void play(CreatureSoundColumn column, const char* what);
    void playSoundEntry(uint32_t soundId, const char* what);

    pipeline::AssetManager* assetManager_ = nullptr;
    std::unique_ptr<CreatureSoundIndex> sounds_;
    std::mt19937 rng_{std::random_device{}()};
    bool mounted_ = false;
    bool moving_ = false;
    bool flying_ = false;
    uint32_t soundData_ = 0;
    float volumeScale_ = 0.7f;
};

} // namespace audio
} // namespace wowee
