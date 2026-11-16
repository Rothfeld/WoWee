#pragma once

#include "game/corpse_harvesting.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include <cassert>
#include <optional>

namespace wowee::ui {

struct CorpseHarvestPresentation {
    const char* tooltipGlobal;
    game::HarvestDifficulty difficulty;
    bool knowsProfession;
};

inline std::optional<CorpseHarvestPresentation> corpseHarvestPresentation(
    game::GameHandler& handler, const game::Unit& unit) {
    if (!(unit.getUnitFlags() & game::UNIT_FLAG_SKINNABLE) ||
        !game::isUnitCorpseState(unit.getHealth(), unit.getMaxHealth(), unit.getDynamicFlags()))
        return std::nullopt;
    const auto& creatures = handler.getCreatureInfoCache();
    const auto info = creatures.find(unit.getEntry());
    if (info == creatures.end()) return std::nullopt; // Query response pending.
    const auto requirements = game::corpseHarvestRequirements(info->second.typeFlags);
    const auto& skills = handler.getPlayerSkills();
    const auto skill = skills.find(requirements.skill);
    const uint32_t rank = skill == skills.end() ? 0 : skill->second.effectiveValue();
    bool hasTool = requirements.toolCategory == 0;
    if (rank >= game::corpseHarvestRequiredRank(unit.getLevel()) && rank && !hasTool) {
        auto* assets = handler.services().assetManager;
        assert(assets);
        if (game::isActiveExpansion("classic") || game::isActiveExpansion("turtle")) {
            // Vanilla spells name tool items directly. The two Totem entries
            // precede the first reagent in Spell.dbc; categories arrived in TBC.
            const auto spells = assets->loadDBC("Spell.dbc");
            const auto* layouts = pipeline::getActiveDBCLayout();
            const auto* layout = layouts ? layouts->getLayout("Spell") : nullptr;
            assert(spells && layout && layout->tryField("Reagent0") != 0xffffffffu);
            const auto record = spells->findRecordById(requirements.skill == 393 ? 8613 : 2575);
            assert(record >= 0);
            hasTool = true;
            for (int index = 0; index < 2; ++index) {
                const uint32_t item = spells->getUInt32(record, (*layout)["Reagent0"] - 2 + index);
                if (!item) continue;
                bool present = handler.getInventory().countItem(item) != 0;
                for (int slot = 0; slot < game::Inventory::NUM_EQUIP_SLOTS && !present; ++slot)
                    present = handler.getInventory().getEquipSlot(static_cast<game::EquipSlot>(slot)).item.itemId == item;
                hasTool = hasTool && present;
            }
            return CorpseHarvestPresentation{requirements.tooltipGlobal,
                game::corpseHarvestDifficulty(unit.getLevel(), rank, hasTool)};
        }
        const auto categories = assets->loadDBC("TotemCategory.dbc");
        assert(categories && categories->isLoaded() && categories->getFieldCount() >= 20);
        const auto required = categories->findRecordById(requirements.toolCategory);
        assert(required >= 0);
        const uint32_t requiredType = categories->getUInt32(required, 18);
        const uint32_t requiredMask = categories->getUInt32(required, 19);
        const auto matches = [&](const game::ItemSlot& slot) {
            if (slot.empty()) return false;
            const auto* item = handler.getItemInfo(slot.item.itemId);
            if (!item) { handler.ensureItemInfo(slot.item.itemId); return false; }
            if (!item->totemCategory) return false;
            const auto record = categories->findRecordById(item->totemCategory);
            assert(record >= 0);
            return game::compatibleGatheringTool(categories->getUInt32(record, 18),
                categories->getUInt32(record, 19), requiredType, requiredMask);
        };
        const auto& inventory = handler.getInventory();
        for (int slot = 0; slot < game::Inventory::NUM_EQUIP_SLOTS && !hasTool; ++slot)
            hasTool = matches(inventory.getEquipSlot(static_cast<game::EquipSlot>(slot)));
        for (int slot = 0; slot < inventory.getBackpackSize() && !hasTool; ++slot)
            hasTool = matches(inventory.getBackpackSlot(slot));
        for (int bag = 0; bag < game::Inventory::NUM_BAG_SLOTS && !hasTool; ++bag)
            for (int slot = 0; slot < inventory.getBagSize(bag) && !hasTool; ++slot)
                hasTool = matches(inventory.getBagSlot(bag, slot));
        for (int slot = 0; slot < game::Inventory::KEYRING_SLOTS && !hasTool; ++slot)
            hasTool = matches(inventory.getKeyringSlot(slot));
    }
    return CorpseHarvestPresentation{requirements.tooltipGlobal,
        game::corpseHarvestDifficulty(unit.getLevel(), rank, hasTool), rank != 0};
}

} // namespace wowee::ui
