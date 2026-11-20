// evaluateMacroConditionals - convenience free function.
// Thin wrapper over MacroEvaluator with concrete adapters.
// Separate TU to avoid pulling Application/Renderer into macro_evaluator unit tests.
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat/game_state_adapter.hpp"
#include "ui/chat/input_modifier_adapter.hpp"
#include "ui/chat/i_game_state.hpp"
#include "game/game_handler.hpp"
#include "core/application.hpp"
#include "rendering/renderer.hpp"

namespace wowee { namespace ui {
namespace {
class EmptyGameState final : public IGameState {
public:
    uint64_t getPlayerGuid() const override { return 0; }
    uint64_t getTargetGuid() const override { return 0; }
    uint64_t getFocusGuid() const override { return 0; }
    uint64_t getPetGuid() const override { return 0; }
    uint64_t getMouseoverGuid() const override { return 0; }
    bool isInCombat() const override { return false; }
    bool isMounted() const override { return false; }
    bool isSwimming() const override { return false; }
    bool isFlying() const override { return false; }
    bool isCasting() const override { return false; }
    bool isChanneling() const override { return false; }
    bool isStealthed() const override { return false; }
    bool hasPet() const override { return false; }
    bool isInGroup() const override { return false; }
    bool isInRaid() const override { return false; }
    bool isIndoors() const override { return false; }
    uint8_t getActiveTalentSpec() const override { return 0; }
    uint32_t getVehicleId() const override { return 0; }
    uint32_t getCurrentCastSpellId() const override { return 0; }
    std::string getSpellName(uint32_t) const override { return {}; }
    bool hasAuraByName(uint64_t, const std::string&, bool) const override { return false; }
    bool hasFormAura() const override { return false; }
    bool entityExists(uint64_t) const override { return false; }
    bool entityIsDead(uint64_t) const override { return false; }
    bool entityIsHostile(uint64_t) const override { return false; }
};
} // namespace

std::string evaluateMacroConditionals(const std::string& rawArg,
                                      game::GameHandler& gameHandler,
                                      uint64_t& targetOverride) {
    auto* renderer = core::Application::getInstance().getRenderer();
    GameStateAdapter gs(gameHandler, renderer);
    InputModifierAdapter im;
    MacroEvaluator eval(gs, im);
    return eval.evaluate(rawArg, targetOverride);
}

MacroParseResult evaluateMacroDetailed(const std::string& rawArg,
                                       game::GameHandler* gameHandler) {
    InputModifierAdapter im;
    if (gameHandler) {
        auto* renderer = core::Application::getInstance().getRenderer();
        GameStateAdapter gs(*gameHandler, renderer);
        return MacroEvaluator(gs, im).evaluateDetailed(rawArg);
    }
    EmptyGameState empty;
    return MacroEvaluator(empty, im).evaluateDetailed(rawArg);
}

} // namespace ui
} // namespace wowee
