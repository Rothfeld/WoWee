#pragma once

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/spell_cooldown_state.hpp"
#include "game/spell_prerequisites.hpp"
#include "game/spell_classification.hpp"
#include "game/handler_types.hpp"
#include "audio/spell_sound_manager.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

/// The text a cast failure shows on the error line, from the interface's
/// GlobalStrings by result name (see cast_result_text.hpp); empty for none.
/// `result` is the normalised (WotLK-numbered) SpellCastResult, arg1/arg2 the
/// packet's trailing values (readCastResultArgs).
std::string castFailureText(GameHandler& owner, uint32_t spellId, uint8_t result,
                            uint32_t arg1, uint32_t arg2);

/// The chat line 2.4.3 shows when a spell enters the spellbook:
/// ERR_LEARN_ABILITY_S or ERR_LEARN_SPELL_S with "Name (Rank N)"; empty and
/// logged when the interface or the spell name is missing.
std::string learnedSpellText(GameHandler& owner, uint32_t spellId);

/// The first tool the spell's Spell.dbc Totem / TotemCategory entries name
/// that the player does not carry, as the cast result that reports it and the
/// item or category id; nullopt when every tool is carried or not yet known.
/// The original checks this before sending: Copper Modulator with no tools and
/// no reagents reads "Requires Blacksmith Hammer", where the server (CMaNGOS
/// Spell::CheckItems) would have answered with the missing reagent first.
std::optional<std::pair<uint8_t, uint32_t>> firstMissingTool(GameHandler& owner, uint32_t spellId);

/// The EQUIPPED_ITEM_CLASS* result the spell's Spell.dbc equipment
/// requirement fails with, in CMaNGOS Player::HasItemFitToSpellReqirements'
/// order (main hand, off hand, then any fitting slot); nullopt when met or
/// when an equipped item's data is not in yet. The original checks this
/// before sending: Backstab unarmed and facing the target reads "Must have a
/// Dagger equipped in the main hand", where the server would answer "You must
/// be behind your target" first; Cheap Shot unarmed out of stealth reads
/// "Must have a Melee Weapon equipped".
std::optional<uint8_t> missingEquipment(GameHandler& owner, uint32_t spellId);

class SpellHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit SpellHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    // --- Spell book tabs ---
    struct SpellBookTab {
        std::string name;
        std::string texture; // icon path
        std::vector<uint32_t> spellIds; // spells in this tab
    };

    // Unit cast state (aliased from handler_types.hpp)
    using UnitCastState = game::UnitCastState;

    // Equipment set info (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // --- Public API (delegated from GameHandler) ---
    /// destination: a TARGET_FLAG_DEST_LOCATION spell's floor point (canonical),
    /// from GameHandler::castGroundTarget. Without one such a spell arms the
    /// ground target instead of casting.
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0,
                   std::optional<glm::vec3> destination = std::nullopt);

    /// Spell.dbc EffectImplicitTargetA, or 0 when the spell is unknown. 21 means
    /// the spell has to be aimed at a friendly unit.
    [[nodiscard]] uint32_t getSpellImplicitTargetA(uint32_t spellId) const;
    /// Whether Spell.dbc has this spell at all.
    ///
    /// A server may cast something the client's own data has never heard of -
    /// a private core's custom item does it routinely - and the answer to
    /// "what does this aim at" is then not zero but unknown. The two are worth
    /// telling apart: zero means the spell says nothing, unknown means we do.
    [[nodiscard]] bool isSpellKnownToClient(uint32_t spellId) const;

    /// The last spell the player cast while on foot. When mounting is detected,
    /// this identifies which of the player's indefinite self-cast auras is the
    /// mount - scanning for one blindly can land on a racial or a tracking buff.
    [[nodiscard]] uint32_t getLastGroundCastSpellId() const { return lastGroundCastSpellId_; }

    /// Record a spell cast by using an item - pre-WotLK mounts are items, and
    /// their on-use spell never passes through castSpell().
    void noteGroundCastSpell(uint32_t spellId) { lastGroundCastSpellId_ = spellId; }
    void cancelCast();
    void cancelAura(uint32_t spellId);
    /// CancelShapeshiftForm and /cancelform: cancels the aura holding the
    /// current form (Cat Form, Shadowform, Stealth); a stance, whose aura has
    /// no buff icon, stays and nothing is sent, as in native 2.4.3.
    void cancelShapeshiftForm();

    // Known spells
    [[nodiscard]] const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells_; }
    [[nodiscard]] const std::unordered_map<uint32_t, float>& getSpellCooldowns() const { return cooldowns_.spellRemaining; }
    [[nodiscard]] float getSpellCooldown(uint32_t spellId) const;
    /// A spell whose cooldown starts on an event (Spell.dbc attribute
    /// 0x02000000: Stealth, Cold Blood) between its cast and the server's
    /// SMSG_COOLDOWN_EVENT. Native 2.4.3 answers IsUsableAction false and
    /// GetActionCooldown enable 0 for it meanwhile.
    [[nodiscard]] bool isCooldownEventPending(uint32_t spellId) const { return cooldownEventPending_.count(spellId) > 0; }
    [[nodiscard]] float getSpellCooldownTotal(uint32_t spellId) const;
    [[nodiscard]] float getPetSpellCooldown(uint32_t spellId) const;
    [[nodiscard]] float getPetSpellCooldownTotal(uint32_t spellId) const;

    // Cast state
    [[nodiscard]] bool isCasting() const { return casting_; }
    [[nodiscard]] bool isChanneling() const { return casting_ && castIsChannel_; }
    [[nodiscard]] bool isRestoring() const { return restorationActive_; }
    /// Auto Shot or Shoot while it repeats, otherwise 0.
    [[nodiscard]] uint32_t getAutoRepeatSpellId() const { return autoRepeatSpellId_; }
    void clearAutoRepeat() { autoRepeatSpellId_ = 0; }
    [[nodiscard]] bool isGameObjectInteractionCasting() const;
    // The cast or channel in progress, else a cast sent and not yet answered,
    // else a spell waiting on the targeting cursor, else the ability queued for
    // the next swing.
    [[nodiscard]] uint32_t getActiveActionSpellId() const;
    [[nodiscard]] uint32_t getCurrentCastSpellId() const {
        return casting_ ? currentCastSpellId_ : 0;
    }
    // The game object text the player's current cast shows on its bar: the
    // opening/closing text of the object an OPEN_LOCK cast is aimed at, which
    // can be empty. nullopt when the cast is not one (castBarText decides).
    [[nodiscard]] std::optional<std::string> getCastBarCaption() const {
        if (!casting_ || castIsChannel_ || castBarCaptionSpellId_ != currentCastSpellId_)
            return std::nullopt;
        return castBarCaption_;
    }
    [[nodiscard]] float getCastProgress() const {
        const float total = casting_ ? castTimeTotal_ : 0.0f;
        const float remaining = casting_ ? castTimeRemaining_ : 0.0f;
        return total > 0.0f ? (total - remaining) / total : 0.0f;
    }
    [[nodiscard]] float getCastTimeRemaining() const {
        return casting_ ? castTimeRemaining_ : 0.0f;
    }
    [[nodiscard]] float getCastTimeTotal() const {
        return casting_ ? castTimeTotal_ : 0.0f;
    }

    // Repeat-craft queue
    void startCraftQueue(uint32_t spellId, int count);
    void cancelCraftQueue();
    [[nodiscard]] int getCraftQueueRemaining() const { return craftQueueRemaining_; }
    [[nodiscard]] uint32_t getCraftQueueSpellId() const { return craftQueueSpellId_; }

    // Crafting window (client-side; opened by casting a profession spell
    // like Cooking or First Aid - see tradeskillOpenerSkillLine)
    [[nodiscard]] bool isCraftingWindowOpen() const { return craftingWindowOpen_; }
    [[nodiscard]] uint32_t getCraftingSkillLine() const { return craftingSkillLine_; }
    /// Opening and closing a profession announce themselves, because the
    /// interface's trade skill panel is driven entirely by these two events -
    /// it hides on TRADE_SKILL_CLOSE and fills itself on TRADE_SKILL_SHOW.
    /// Without them the panel could be complete and still never appear.
    /// `openerSpellId` is the profession spell cast; its TRADE_SKILL effect's
    /// misc value picks the window: 0 the trade skill frame, otherwise the
    /// craft frame (3 Enchanting, 1 Beast Training).
    void openCraftingWindow(uint32_t skillLine, uint32_t openerSpellId);
    void closeCraftingWindow();
    /// The craft frame (Enchanting), open beside the trade skill frame or
    /// alone; CRAFT_SHOW, CRAFT_UPDATE and CRAFT_CLOSE drive it.
    [[nodiscard]] bool isCraftWindowOpen() const { return craftWindowOpen_; }
    [[nodiscard]] uint32_t getCraftSkillLine() const { return craftSkillLine_; }
    /// The opener's TRADE_SKILL misc value: 1 Beast Training, 3 Enchanting.
    [[nodiscard]] int32_t getCraftKind() const { return craftKind_; }
    void closeCraftWindow();
    /// TRADE_SKILL_UPDATE and CRAFT_UPDATE for whichever window is open: the
    /// bags, the known spells or the skill moved under it.
    void notifyProfessionWindows();
    /// ReloadUI closes both windows and tells the interface being torn down,
    /// whether or not one was open: 2.4.3 fires TRADE_SKILL_CLOSE and
    /// CRAFT_CLOSE (then SOCKET_INFO_CLOSE) to the old interface every time.
    void closeProfessionWindowsForReload();
    // Returns the skill line id if spellId is a tradeskill-window opener
    // (e.g. Cooking → 185) with at least one known recipe, else 0.
    uint32_t tradeskillOpenerSkillLine(uint32_t spellId);

    // SpellFocusObject.dbc name ("Anvil", "Cooking Fire", ...) for
    // requires-spell-focus cast failures; empty if unknown.
    const std::string& getSpellFocusName(uint32_t focusId);

    // TotemCategory.dbc name ("Blacksmith Hammer", "Mining Pick", ...) for
    // totem-category cast failures; empty if unknown.
    const std::string& getTotemCategoryName(uint32_t categoryId);

    // Spell queue (400ms window)
    [[nodiscard]] uint32_t getQueuedSpellId() const { return queuedSpellId_; }
    void cancelQueuedSpell() { queuedSpellId_ = 0; queuedSpellTarget_ = 0; }

    // Unit cast state (tracked per GUID for target frame + boss frames)
    [[nodiscard]] const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    void clearUnitCastStates() { unitCastStates_.clear(); }
    void removeUnitCastState(uint64_t guid) { unitCastStates_.erase(guid); }

    // Aura cache mutation (formerly accessed via friend)
    void clearUnitAurasCache() { unitAurasCache_.clear(); }
    /// Copy a unit's auras into the by-guid list the party frames read.
    void mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras);
    void removeUnitAuraCache(uint64_t guid) { unitAurasCache_.erase(guid); }

    // Known spells mutation (formerly accessed via friend)
    void addKnownSpell(uint32_t spellId) { knownSpells_.insert(spellId); }
    /// Known, or replaced in the spellbook by a higher rank.
    [[nodiscard]] bool hasOrSupercededSpell(uint32_t spellId) const {
        return knownSpells_.count(spellId) != 0 || supercededSpells_.count(spellId) != 0;
    }
    [[nodiscard]] bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) > 0; }

    // Target aura mutation (formerly accessed via friend)
    void clearTargetAuras() { for (auto& slot : targetAuras_) slot = AuraSlot{}; }
    void restoreTargetAuras(uint64_t guid);

    // Player aura mutation (formerly accessed via friend)
    void resetPlayerAuras(size_t capacity) { playerAuras_.clear(); playerAuras_.resize(capacity); }
    AuraSlot& getPlayerAuraSlotRef(size_t slot) { return playerAuras_[slot]; }
    std::vector<AuraSlot>& getPlayerAurasMut() { return playerAuras_; }
    /// The last SMSG_UPDATE_AURA_DURATION for a player aura slot, kept until
    /// the update fields say which spell the slot holds. CMaNGOS sends the
    /// duration when the aura is applied and the UNIT_FIELD_AURA change only
    /// with the next object update, so when a new aura takes the slot of the
    /// one it replaces (Hydross' Mark of Hydross 38215 -> 38216 -> ...) the
    /// duration arrives while the slot still names the old spell.
    struct PendingAuraDuration { int32_t durationMs = -1; uint64_t receivedAtMs = 0; };
    std::unordered_map<uint8_t, PendingAuraDuration>& pendingPlayerAuraDurations() {
        return pendingPlayerAuraDurations_;
    }

    // Target cast helpers
    [[nodiscard]] bool isTargetCasting() const;
    [[nodiscard]] uint32_t getTargetCastSpellId() const;
    [[nodiscard]] float getTargetCastProgress() const;
    [[nodiscard]] float getTargetCastTimeRemaining() const;
    [[nodiscard]] bool isTargetCastInterruptible() const;

    // Talents
    [[nodiscard]] uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    [[nodiscard]] uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    [[nodiscard]] uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }

    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs() const { return learnedGlyphs_[activeTalentSpec_]; }
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs(uint8_t spec) const {
        static std::array<uint16_t, MAX_GLYPH_SLOTS> empty{};
        return spec < 2 ? learnedGlyphs_[spec] : empty;
    }
    [[nodiscard]] uint8_t getTalentRank(uint32_t talentId) const {
        auto it = learnedTalents_[activeTalentSpec_].find(talentId);
        return (it != learnedTalents_[activeTalentSpec_].end()) ? it->second : 0;
    }
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    void switchTalentSpec(uint8_t newSpec);

    // Talent DBC access
    [[nodiscard]] const TalentEntry* getTalentEntry(uint32_t talentId) const {
        auto it = talentCache_.find(talentId);
        return (it != talentCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const TalentTabEntry* getTalentTabEntry(uint32_t tabId) const {
        auto it = talentTabCache_.find(tabId);
        return (it != talentTabCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentEntry>& getAllTalents() const { return talentCache_; }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }

    /// Drops what was read out of Talent.dbc and TalentTab.dbc.
    ///
    /// Called when the active expansion changes, for the same reason as
    /// MovementHandler::resetTaxiDbcCache: GameHandler cleared copies of its
    /// own and the readers come here.
    void resetTalentDbcCache() {
        talentCache_.clear();
        talentTabCache_.clear();
        talentDbcLoaded_ = false;
    }
    void loadTalentDbc();
    void syncPreWotlkTalentsFromKnownSpells();
    /// PLAYER_CHARACTER_POINTS1 changed: the unspent talent point count.
    void setCharacterPointsField(uint32_t points);

    // Auras
    [[nodiscard]] const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>* getUnitAuras(uint64_t guid) const {
        auto it = unitAurasCache_.find(guid);
        return (it != unitAurasCache_.end()) ? &it->second : nullptr;
    }

    // Global Cooldown (GCD)
    [[nodiscard]] float getGCDRemaining() const {
        if (gcdTotal_ <= 0.0f) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gcdStartedAt_).count() / 1000.0f;
        float rem = gcdTotal_ - elapsed;
        return rem > 0.0f ? rem : 0.0f;
    }
    [[nodiscard]] float getGCDTotal() const { return gcdTotal_; }
    [[nodiscard]] bool isGCDActive() const { return getGCDRemaining() > 0.0f; }

    // Spell book tabs
    const std::vector<SpellBookTab>& getSpellBookTabs();

    // Talent wipe confirm dialog
    [[nodiscard]] bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    [[nodiscard]] uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    /// The trainer offering the wipe. The confirmation closes itself when the
    /// player walks away from them.
    [[nodiscard]] uint64_t getTalentWipeNpcGuid() const { return talentWipeNpcGuid_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }

    // Pet talent respec confirm
    void confirmPetUnlearn();
    void cancelPetUnlearn() { petUnlearnPending_ = false; }

    // Item use

    // Equipment sets - canonical data owned by InventoryHandler;
    // GameHandler::getEquipmentSets() delegates to inventoryHandler_.

    // Pet spells
    void sendPetAction(uint32_t action, uint64_t targetGuid = 0);
    void castPetSpell(uint32_t spellId, uint64_t targetGuid = 0);
    void petStopAttack();
    void petCancelAura(uint32_t spellId);
    void petSetAction(uint32_t pos0, uint32_t data0,
                      bool hasSecond = false, uint32_t pos1 = 0, uint32_t data1 = 0);
    void dismissPet();
    void togglePetSpellAutocast(uint32_t spellId);
    void renamePet(const std::string& newName);

    // Spell DBC accessors
    [[nodiscard]] const int32_t* getSpellEffectBasePoints(uint32_t spellId) const;
    [[nodiscard]] float getSpellDuration(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellName(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellRank(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellDescription(uint32_t spellId) const;
    [[nodiscard]] std::string getEnchantName(uint32_t enchantId) const;
    /// The gem item an enchantment came out of (SpellItemEnchantment.Src_ItemID).
    /// Zero when the enchantment is not a gem, or on a file with no such column.
    /// This is the only route from an enchantment sitting in an item's socket
    /// back to the gem that is in the socket - the item fields carry the
    /// enchantment id and nothing else.
    [[nodiscard]] uint32_t getEnchantGemItem(uint32_t enchantId) const;
    /// SpellItemEnchantmentCondition.dbc row a (meta gem) enchantment needs, 0 for none.
    uint32_t getEnchantCondition(uint32_t enchantId) const;
    [[nodiscard]] uint8_t getSpellDispelType(uint32_t spellId) const;
    [[nodiscard]] bool isSpellInterruptible(uint32_t spellId) const;
    [[nodiscard]] bool isSpellPassive(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellSchoolMask(uint32_t spellId) const;
    /// Spell.dbc Targets mask (SpellCastTargetFlags): 0x10 = TARGET_FLAG_ITEM.
    [[nodiscard]] uint32_t getSpellTargetFlags(uint32_t spellId) const;
    /// Spell.dbc RangeIndex resolved via SpellRange.dbc, in yards. Melee ("Combat
    /// Range") is 5; self-only is 0; negative means SpellRange.dbc was unavailable.
    [[nodiscard]] float getSpellMaxRange(uint32_t spellId) const;
    /// True for "Self Only" range spells (shouts, self-buffs): they always land on
    /// the caster, so they take no explicit target and skip melee range checks.
    [[nodiscard]] bool isSelfCastSpell(uint32_t spellId) const;
    /// Whether a spell aimed as `aim` can be cast at this unit. 0 and unknown
    /// guids cannot.
    [[nodiscard]] bool unitTakesAim(uint64_t guid, spellclass::UnitAim aim) const;
    /// SpellCanTargetUnit / cursor affordance for a unit-targeted spell.
    [[nodiscard]] bool canSpellTargetUnit(uint32_t spellId, uint64_t guid) const;
    /// Maps a superseded spell rank to the highest rank we actually know. Returns
    /// spellId unchanged when it is already known, or has no known same-name rank.
    [[nodiscard]] uint32_t resolveHighestKnownRank(uint32_t spellId) const;
    /// The skill's own name, by SkillLine.dbc id.
    [[nodiscard]] const std::string& getSkillLineName(uint32_t skillLineId) const;

    // Cast state
    void stopCasting();
    void resetCastState();
    void resetTalentState();
    // Full per-character reset (spells, cooldowns, auras, cast state, talents).
    // Called from GameHandler::selectCharacter so spell state doesn't bleed between characters.
    void resetAllState();
    void clearUnitCaches();

    // Aura duration
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);

    // Skill DBC
    void loadSkillLineDbc();
    void extractSkillFields(const FlatFieldMap& fields);
    void extractExploredZoneFields(const FlatFieldMap& fields);

    // Update per-frame timers (call from GameHandler::update)
    void updateTimers(float dt);
    void refreshRestorationState() { refreshRestorationFromPlayerAuras(); }

    // Packet handlers dispatched from GameHandler's opcode table
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);

    // Pet stable commands (called via GameHandler delegation)
    void requestStabledPetList();
    void stablePet();
    void unstablePet(uint32_t petNumber);
    void stableSwapPet(uint32_t petNumber);

    // DBC cache loading (called from GameHandler during login)
    void loadSpellNameCache() const;
    uint32_t getLearnedProficiency(uint32_t itemClass) const;
    void loadSkillLineAbilityDbc();

    /// Ask the server what the pet is called.
    ///
    /// The name a player gave a pet arrives only in answer to
    /// CMSG_PET_NAME_QUERY. Without it a pet wears its creature template's
    /// name - "Voidwalker" where the player wrote something else - which is
    /// what the pet frame, its nameplate and the pet bar's tooltip all show.
    ///
    /// The pet number in that request is a key the server echoes back and does
    /// not look anything up with: SendPetNameQuery finds the pet by guid. So
    /// this sends a number of its own making and uses it to match the reply to
    /// the pet it asked about, which is what the field would have been for.
    // CMSG_PET_NAME_QUERY for a unit whose UNIT_FIELD_PET_NAME_TIMESTAMP is set.
    // The server answers only when petNumber is the unit's UNIT_FIELD_PETNUMBER.
    void requestPetName(uint64_t petGuid, uint32_t petNumber);

    /// Give a hunter's pet up for good. The interface asks first - this is the
    /// other side of the ABANDON_PET dialog, whose accept called an unbound
    /// name and raised.
    /// The five values every UNIT_SPELLCAST_* event carries.
    ///
    /// unit, spell name, rank, cast id, spell id - in that order, which is what
    /// FrameXML unpacks. These were being fired as just the unit and the spell
    /// id, so the id sat where the name belongs and the cast id was absent.
    [[nodiscard]] std::vector<std::string> spellcastArgs(const std::string& unitId,
                                           uint32_t spellId) const;

    void abandonPet();

    /// Buy the next stable slot from the stable master currently open.
    void buyStableSlot();

    // A DynamicObject (persistent spell area) appearing and leaving.
    void showPersistentArea(uint64_t guid, uint32_t spellId, const glm::vec3& canonicalPosition,
                            float wireRadius = -1.0f);
    void hidePersistentArea(uint64_t guid);

    /// Whether a SpellVisual missile with this SpellMissInfo outcome plays
    /// HARDCODED Resist Spell (3207, spells\resist_immune_effect.m2) on arrival.
    [[nodiscard]] static bool spellMissShowsResistEffect(uint8_t missType);

private:
    std::unordered_map<uint32_t, uint64_t> pendingPetNameQueries_;

    // --- Packet handlers ---
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    [[nodiscard]] bool answersPress(const CastFailedData& data, uint8_t castCount) const;
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    void handleSpellUpdateChainTargets(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);

    void handleCastResult(network::Packet& packet);
    void handleSpellFailedOther(network::Packet& packet);
    void handleClearCooldown(network::Packet& packet);
    void handleModifyCooldown(network::Packet& packet);
    void handlePlaySpellVisual(network::Packet& packet);
    void handleSpellModifier(network::Packet& packet, bool isFlat);
    void handleSpellDelayed(network::Packet& packet);
    void handleSpellLogMiss(network::Packet& packet);
    void handleSpellFailure(network::Packet& packet);
    void handleItemCooldown(network::Packet& packet);
    void handleDispelFailed(network::Packet& packet);
    void handleTotemCreated(network::Packet& packet);
    void handlePeriodicAuraLog(network::Packet& packet);
    void handleSpellEnergizeLog(network::Packet& packet);
    void handleExtraAuraInfo(network::Packet& packet, bool isInit);
    void handleSpellDispelLog(network::Packet& packet);
    void handleSpellStealLog(network::Packet& packet);
    void handleSpellChanceProcLog(network::Packet& packet);
    void handleSpellInstaKillLog(network::Packet& packet);
    void handleSpellLogExecute(network::Packet& packet);
    void handleClearExtraAuraInfo(network::Packet& packet);
    void handleItemEnchantTimeUpdate(network::Packet& packet);
    void handleResumeCastBar(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);

    // --- Internal helpers ---

    // Resolve the magic school for a spell (for audio playback).
    // Returns MagicSchool from the spell name cache, defaulting to ARCANE.
    audio::SpellSoundManager::MagicSchool resolveSpellSchool(uint32_t spellId);

    // Play a spell cast or impact sound via audioCoordinator, if available.
    void playSpellCastSound(uint32_t spellId);
    void playSpellImpactSound(uint32_t spellId);

    // Resolve SpellVisualID from Spell.dbc cache for a given spellId.
    uint32_t resolveSpellVisualId(uint32_t spellId);
    float resolveSpellMissileSpeed(uint32_t spellId);
    /// Fly the spell's missile from the caster to one target. True when one
    /// launched, which then plays the impact/resist kit itself when it lands.
    bool launchSpellMissile(uint32_t spellId, uint32_t visualId, uint64_t casterGuid,
                            uint64_t targetGuid, bool impactAtEnd,
                            bool resistEffectAtEnd = false,
                            std::function<void()> onArrive = {},
                            bool skipReleaseDelay = false);
    // Resolve render-space position for a unit GUID (player or entity).
    bool resolveUnitPosition(uint64_t guid, glm::vec3& outPos);
    // Play the cast/precast visual effect at the caster's position.
    void triggerCastVisual(uint32_t spellId, uint64_t casterGuid, uint32_t castTimeMs = 0);
    // Play the impact visual effect at the target's position.
    void triggerImpactVisual(uint32_t spellId, uint64_t targetGuid);
    void launchRangedWeaponProjectile(uint32_t spellId, uint64_t targetGuid);
    /// Another unit's SMSG_SPELL_GO with CAST_FLAG_AMMO: the packet's ammo
    /// display flies from the caster's right hand to each target at the
    /// spell's Speed.
    void launchAmmoProjectile(const SpellGoData& data);
    void refreshRestorationFromPlayerAuras();
    void stopRestorationPresentation();

    // --- handleSpellLogExecute per-effect parsers (extracted to reduce nesting) ---
    void parseEffectPowerDrain(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectHealthLeech(network::Packet& packet, uint32_t effectLogCount,
                                uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectCreateItem(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectInterruptCast(network::Packet& packet, uint32_t effectLogCount,
                                  uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectFeedPet(network::Packet& packet, uint32_t effectLogCount,
                            uint64_t caster, uint32_t spellId, bool isPlayerCaster);

    // Find the on-use spell for an item (trigger=0 Use or trigger=5 NoDelay).
    // CMSG_USE_ITEM requires a valid spellId or the server silently ignores it.
    void seedCooldownFromSpellInfo(uint32_t spellId, bool isPet = false);
    void syncActionBarCooldowns(uint32_t spellId, uint32_t categoryId);
    void upgradeActionBarSpell(uint32_t oldSpellId, uint32_t newSpellId);
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);

    GameHandler& owner_;

    // --- Spell state ---
    std::unordered_set<uint32_t> knownSpells_;
    // Ranks SMSG_SUPERCEDED_SPELL replaced in the spellbook. The player still
    // has them (CMaNGOS keeps them inactive); the 2.4.3 trainer lists them as
    // used, Garrote (Rank 1) after Rank 2 is learned.
    std::unordered_set<uint32_t> supercededSpells_;
    // Player and pet cooldowns tracked separately so shared Spell.dbc category
    // IDs never collide between the player and their pet. Each tracks both
    // per-spell and per-category remaining/total seconds so GetSpellCooldown
    // can wind back the swirl start time across repeated queries.
    SpellCooldownState cooldowns_;
    SpellCooldownState petCooldowns_;
    uint8_t castCount_ = 0;
    bool casting_ = false;
    bool castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    std::optional<std::string> castBarCaption_;
    uint32_t castBarCaptionSpellId_ = 0;
    uint32_t autoRepeatSpellId_ = 0;
    uint8_t autoRepeatCastCount_ = 0;
    // An on-next-swing ability between its SMSG_SPELL_START and the swing.
    uint32_t nextSwingSpellId_ = 0;
    /// The spell of the last CMSG_CAST_SPELL until the server answers it
    /// (SMSG_SPELL_START, SMSG_SPELL_GO, a cast failure). Native 2.4.3 marks
    /// an instant Sinister Strike current from UNIT_SPELLCAST_SENT to
    /// UNIT_SPELLCAST_SUCCEEDED, about 60 ms, not for its cooldown.
    uint32_t pendingCastSpellId_ = 0;
    uint8_t nextSwingCastCount_ = 0;
    // What the action buttons were last told, so a cast or swing changing is
    // announced once whichever of the many cast paths changed it.
    uint32_t announcedCastSpellId_ = 0;
    bool announcedAutoAttack_ = false;
    /// Per action slot: whether it showed its spell's ActiveIconID when last
    /// announced. A change fires ACTIONBAR_SLOT_CHANGED <slot>, as native
    /// 2.4.3 does when Aspect of the Hawk's aura lands.
    std::vector<bool> announcedActiveIcons_;
    // IsUsableAction of every bar slot as last announced; see updateTimers.
    std::vector<SpellUsability> announcedUsability_;
    std::unordered_set<uint32_t> cooldownEventPending_;
    float castTimeRemaining_ = 0.0f;
    float castTimeTotal_ = 0.0f;
    bool restorationActive_ = false;
    uint32_t restorationSpellId_ = 0;
    bool restorationIsFood_ = false;
    float restorationTimeRemaining_ = 0.0f;
    float restorationSoundTimer_ = 0.0f; // repeats the consume sound while active

    // Repeat-craft queue
    uint32_t craftQueueSpellId_ = 0;
    int craftQueueRemaining_ = 0;
    /// Set by startCraftQueue until its first cast is sent.
    bool craftQueueAnnounce_ = false;

    // Crafting window
    bool craftingWindowOpen_ = false;
    uint32_t craftingSkillLine_ = 0;
    bool craftWindowOpen_ = false;
    uint32_t craftSkillLine_ = 0;
    int32_t craftKind_ = 0;
    /// Nesting of TRADE_SKILL_CLOSE / CRAFT_CLOSE being handled (at most the
    /// outer event and the one its OnHide causes).
    int craftingClosing_ = 0;
    int craftClosing_ = 0;

    // SpellFocusObject.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> spellFocusNames_;
    bool spellFocusDbcLoaded_ = false;

    // TotemCategory.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> totemCategoryNames_;
    bool totemCategoryDbcLoaded_ = false;

    // Spell queue (400ms window)
    uint32_t lastGroundCastSpellId_ = 0;
    /// When auto-attack was last toggled, for the Ability Toggle guard: a
    /// second press inside a short window is an accident rather than a
    /// decision. See the SPELL_ID_ATTACK branch in castSpell.
    std::chrono::steady_clock::time_point autoAttackToggledAt_{};
    uint32_t queuedSpellId_ = 0;
    uint64_t queuedSpellTarget_ = 0;

    // Per-unit cast state
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;
    uint8_t unspentTalentPoints_[2] = {0, 0};
    /// Last PLAYER_CHARACTER_POINTS1 value received (pre-WotLK unspent points).
    uint32_t characterPointsField_ = 0;
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    std::array<std::array<uint16_t, MAX_GLYPH_SLOTS>, 2> learnedGlyphs_{};
    std::unordered_map<uint32_t, TalentEntry> talentCache_;
    /// SkillLineAbility SupercededBySpell: the next rank of a spell. A talent
    /// rank whose spell a trainer rank has replaced (Mortal Strike rank 1 by
    /// rank 6) still counts as learned.
    std::unordered_map<uint32_t, uint32_t> supersededBy_;
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_;
    bool talentDbcLoaded_ = false;
    bool talentsInitialized_ = false;

    // Auras
    std::vector<AuraSlot> playerAuras_;
    std::unordered_map<uint8_t, PendingAuraDuration> pendingPlayerAuraDurations_;
    std::vector<AuraSlot> targetAuras_;
    std::unordered_map<uint64_t, std::vector<AuraSlot>> unitAurasCache_;

    // Global Cooldown
    float gcdTotal_ = 0.0f;
    std::chrono::steady_clock::time_point gcdStartedAt_{};
    // The cast that started the optimistic GCD, which a refused cast takes back.
    uint32_t optimisticGcdSpellId_ = 0;
    uint8_t optimisticGcdCastCount_ = 0;

    // Spell book tabs, rebuilt when the known spells, race or class change.
    std::vector<SpellBookTab> spellBookTabs_;
    uint64_t spellBookSignature_ = 0;
    bool spellBookBuilt_ = false;
    void buildTbcSpellBookTabs();
public:
    /// Whether a known spell has an entry in the spellbook at all.
    bool isListedInSpellbook(uint32_t spellId);
    /// 1-based spellbook tab holding a listed spell, 0 when none does.
    int spellBookTabIndexOf(uint32_t spellId);
    /// SkillRaceClassInfo.dbc flags of the skill line's row for the player's
    /// race and class; nullopt when no row covers them.
    std::optional<uint32_t> skillRaceClassFlags(uint32_t skillLine);
private:

    // SkillRaceClassInfo.dbc: skill line -> rows of (race mask, class mask, flags).
    struct SkillRaceClassRow {
        uint32_t raceMask = 0;
        uint32_t classMask = 0;
        uint32_t flags = 0;
    };
    std::unordered_map<uint32_t, std::vector<SkillRaceClassRow>> skillRaceClassInfo_;
    bool skillRaceClassInfoLoaded_ = false;
    void loadSkillRaceClassInfoDbc();

    // Talent wipe confirm dialog
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;

    // Pet talent respec confirm dialog
    bool petUnlearnPending_ = false;
    uint32_t petUnlearnCost_ = 0;
};

} // namespace game
} // namespace wowee
