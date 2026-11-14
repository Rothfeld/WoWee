#pragma once
#include <cstdint>
namespace wowee::game {
class GameHandler;
enum class SpellPrerequisite { Ready, Unknown, Passive, NoPower, NoComboPoints, Stealth, Combat, Dead };
SpellPrerequisite spellPrerequisite(GameHandler& owner, uint32_t spellId, uint64_t target);
}
