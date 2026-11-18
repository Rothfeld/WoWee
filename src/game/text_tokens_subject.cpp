#include "game/text_tokens.hpp"

#include "game/character.hpp"
#include "game/game_handler.hpp"

namespace wowee {
namespace game {

// The half that needs a client. Kept out of text_tokens.cpp so the substitution
// itself links against nothing and can be exercised on its own.

TextSubject textSubjectFor(const GameHandler& gameHandler) {
    TextSubject subject;
    if (const auto* character = gameHandler.getActiveCharacter()) {
        subject.name = character->name;
        subject.className = getClassName(character->characterClass);
        subject.raceName = getRaceName(character->race);
        subject.female = character->gender == Gender::FEMALE;
    }
    subject.worldState = [&gameHandler](uint32_t id) {
        return gameHandler.getWorldState(id).value_or(0u);
    };
    return subject;
}

}  // namespace game
}  // namespace wowee
