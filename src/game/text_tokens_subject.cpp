#include "game/text_tokens.hpp"

#include "game/character.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "core/logger.hpp"

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
    }
    // The gender the server gives the player's unit (UNIT_FIELD_BYTES_0 byte
    // 2), as UnitSex reads it. The character record can carry a gender kept in
    // this client's own per-character config, which native does not have.
    if (const auto player = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid())) {
        const uint32_t bytes0 = player->getField(fieldIndex(UF::UNIT_FIELD_BYTES_0));
        subject.female = ((bytes0 >> 16) & 0xFF) == 1;
    } else {
        LOG_WARNING("textSubjectFor: no player unit - $g reads as male");
    }
    subject.worldState = [&gameHandler](uint32_t id) {
        return gameHandler.getWorldState(id).value_or(0u);
    };
    return subject;
}

}  // namespace game
}  // namespace wowee
