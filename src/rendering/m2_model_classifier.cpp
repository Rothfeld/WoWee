#include "rendering/m2_model_classifier.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace wowee {
namespace rendering {

namespace {

// Returns true if `lower` contains `token` as a substring.
// Caller must provide an already-lowercased string.
inline bool has(const std::string& lower, std::string_view token) noexcept {
    return lower.find(token) != std::string::npos;
}

/// `has`, but the token may not be the tail of a longer word.
///
/// Written for `fire`, which is in `hellfire`. Outland's sky model is
/// HellfireSkyBox, so it classified as a brazier - and the renderer gives an
/// additive batch of a brazier a lamp flicker keyed on the instance position.
/// A sky dome's position is the camera's, rewritten every frame, so the flicker
/// re-rolled its phase whenever the camera crossed a one-unit cell and the sky
/// strobed. lampFlicker's own comment says a drifting seed does exactly that.
///
/// The same shape as the `forge` rule below, which was added when Ironforge
/// made all 64 doodads of the city into forges.
inline bool hasWord(const std::string& lower, std::string_view token) noexcept {
    for (std::size_t at = lower.find(token); at != std::string::npos;
         at = lower.find(token, at + 1)) {
        if (at == 0 || !std::isalpha(static_cast<unsigned char>(lower[at - 1])))
            return true;
    }
    return false;
}

/// Whether the token stands alone: no letter on either side of it.
///
/// hasWord only guards the front, so a bare "fire" matched the head of
/// FirewoodPile and FireFlies01 and a stack of logs was classified as a
/// burning one - with a flame's colour floor and an ambient fire loop.
inline bool hasStandaloneWord(const std::string& lower, std::string_view token) noexcept {
    for (std::size_t at = lower.find(token); at != std::string::npos;
         at = lower.find(token, at + 1)) {
        const bool frontOk =
            at == 0 || !std::isalpha(static_cast<unsigned char>(lower[at - 1]));
        const std::size_t after = at + token.size();
        const bool backOk =
            after >= lower.size() || !std::isalpha(static_cast<unsigned char>(lower[after]));
        if (frontOk && backOk) return true;
    }
    return false;
}

/// Whether the name ends on this token, allowing a trailing number or
/// separator after it: Torch_Out, OrcBonFireOff, Brazier_Off01.
///
/// Nothing is required in front of it, because the names being matched run
/// their words together - the "off" in OrcBonFireOff follows a letter. The
/// token is only ever looked for alongside a fire or torch token, so a word
/// that merely ends in one of these ("lookout") cannot reach here.
inline bool endsWithToken(const std::string& lower, std::string_view token) noexcept {
    for (std::size_t at = lower.find(token); at != std::string::npos;
         at = lower.find(token, at + 1)) {
        bool onlyTrailers = true;
        for (std::size_t k = at + token.size(); k < lower.size(); ++k) {
            const char c = lower[k];
            if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '_' ||
                  c == '-' || c == '.' || c == ' ')) {
                onlyTrailers = false;
                break;
            }
        }
        if (onlyTrailers) return true;
    }
    return false;
}

/// Whether this name is a lit fire: a campfire, a bonfire, a fire pit, or
/// something named "fire" outright.
///
/// Shared by classifyM2Model and classifyAmbientEmitter, which spelled it out
/// separately and had already drifted - one is what the renderer treats as
/// flame, the other is what the sound system plays a fire loop over, and a
/// model can be neither but not one without the other.
bool isLitFireName(const std::string& lower) noexcept {
    if (endsWithToken(lower, "off") || endsWithToken(lower, "out")) return false;
    return hasStandaloneWord(lower, "fire") || has(lower, "campfire")
        || has(lower, "bonfire") || has(lower, "firepit");
}

// Returns true if any token in the compile-time array is a substring of `lower`.
template <std::size_t N>
bool hasAny(const std::string& lower,
            const std::array<std::string_view, N>& tokens) noexcept {
    for (auto tok : tokens)
        if (lower.find(tok) != std::string::npos) return true;
    return false;
}

/// hasAny, refusing a token that is only the tail of a longer word. Used for
/// the flame families, where "fire" is in "hellfire".
template <std::size_t N>
bool hasAnyWord(const std::string& lower,
                const std::array<std::string_view, N>& tokens) noexcept {
    for (auto tok : tokens)
        if (hasWord(lower, tok)) return true;
    return false;
}

} // namespace

std::string assetTokenName(const std::string& path) {
    std::string name = path;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t lastSep = name.find_last_of("\\/");
    if (lastSep != std::string::npos) name = name.substr(lastSep + 1);
    for (std::string_view ext : {".m2", ".mdx", ".blp"}) {
        if (name.size() > ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            name.resize(name.size() - ext.size());
            break;
        }
    }
    return name;
}

bool assetNameHasToken(const std::string& path, std::string_view token) {
    return assetTokenName(path).find(token) != std::string::npos;
}

bool assetNameHasWordToken(const std::string& path, std::string_view token) {
    const std::string name = assetTokenName(path);
    for (std::size_t at = name.find(token); at != std::string::npos;
         at = name.find(token, at + 1)) {
        // Preceded by a letter means this is the tail of a longer word:
        // `fire` inside `hellfire`. A digit or a separator is a boundary,
        // so `fire2` and `stone_fire` still match.
        if (at == 0 || !std::isalpha(static_cast<unsigned char>(name[at - 1]))) {
            return true;
        }
    }
    return false;
}

M2ClassificationResult classifyM2Model(
    const std::string& name,
    const glm::vec3&   /*boundsMin*/,
    const glm::vec3&   /*boundsMax*/,
    std::size_t        vertexCount,
    std::size_t        emitterCount,
    bool               groundEffect)
{
    // Lowercased full name for explicitly path-based checks.
    std::string fullPath = name;
    std::transform(fullPath.begin(), fullPath.end(), fullPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Token checks run on the file name only. See assetTokenName().
    const std::string n = assetTokenName(name);

    M2ClassificationResult r;

    // ---------------------------------------------------------------
    // Simple single-token flags
    // ---------------------------------------------------------------
    r.isInvisibleTrap = has(n, "invisibletrap");
    // Ground clutter is a placement, not a folder. 61 models under a
    // ...\Detail\ folder are ADT and WMO doodads (1184 MMDX references:
    // StranglePlant08/10, StranglethornFern05, ElwynnRock1/2, ElwynnSeaweed01)
    // and 36 World\NoDXT\Detail clutter models are placed by ADTs too. 2.4.3
    // draws those with their authored materials: StranglePlant10's alpha-key
    // fronds stay hard and opaque beside the big trees of Stranglethorn, where
    // the clutter path's soft blended edges painted the sky through them.
    r.isGroundDetail  = groundEffect;
    // A model under PARTICLEEMITTERS\ is pure VFX - the 61 models there are
    // auras, wisps and splashes, and not one of them is a physical object.
    // kEffectTokens already carries "particleemitter", but tokens are matched
    // on the BASENAME, and for these the word is only ever in the directory:
    // the basename is "AuraPurple". So the token never fired and the aura fell
    // through to the generic-solid shape rule instead.
    //
    // That is the Rut'theran portal's second invisible wall. AuraPurple.m2 is
    // the glow inside the arch, placed at scale 10.69, and its unscaled 1.96 x
    // 1.96 x 3.08 bounds land squarely in genericSolid's window - so it became
    // a small solid prop, and at that scale a ~21 x 21 x 33 unit block of solid
    // nothing centred on the portal. The player was stopped exactly where the
    // glow begins.
    const bool particleEmitterVfx = has(fullPath, "\\particleemitters\\")
                                 || has(fullPath, "/particleemitters/");

    r.isInstancePortal  = has(n, "instanceportal") || has(n, "instancenewportal")
                        || has(n, "portalfx")       || has(n, "spellportal")
                        || has(fullPath, "\\instanceportal\\") || has(fullPath, "/instanceportal/");

    r.isWaterfall       = has(n, "waterfall");

    r.isElvenLike   = has(n, "elf")     || has(n, "elven") || has(n, "quel");
    // Directional volumetric light effects (lighthouse beam, light rays/shafts) match
    // the broad "light" token but are NOT point lanterns. They are animated additive
    // meshes - e.g. the Stormwind lighthouse beam rotates via a looped single-bone
    // animation. Classifying them as lanterns routed their batch through the glow-card
    // path, which skips the mesh and draws a static billboard instead, freezing the
    // sweep. Exclude beams/rays/shafts so their animated mesh renders normally.
    const bool volumetricLightBeam =
        has(n, "beam")     || has(n, "lighthouse") || has(n, "lightray") ||
        has(n, "lightshaft") || has(n, "godray")   || has(n, "sunray");
    r.isLanternLike = (has(n, "lantern") || has(n, "lamp")  || has(n, "light") ||
                       has(n, "sconce")  || has(n, "candle") ||
                       has(n, "candelabra") || has(n, "chandelier"))
                      && !volumetricLightBeam;
    r.isKoboldFlame = has(n, "kobold")
                    && (has(n, "candle") || has(n, "torch") || has(n, "mine"));

    // Fire / brazier / torch model detection (for ambient emitter + rendering)
    //
    // "bonfire" and "campfire" match as plain substrings, not as delimited
    // words. A word match wants a non-letter in front of the token, and WoW's
    // model names run their words together: OrcBonFire, OrcPvPBonFireLarge,
    // BlackrockOrcCampfire, KarazahnBonfire01 - 19 of the 37 fires in the data
    // were failing this test and getting none of the treatment below, which is
    // why the Grom'gol bonfire had no ambient fire sound and none of the flame
    // colour and scale floors. Nothing in the game contains either compound
    // without being a fire, so the delimiter buys nothing here; bare "fire"
    // still needs it, or FireFlies, Fireworks and FirewoodPile all become
    // fires.
    //
    // An explicitly unlit variant is not a fire. These carry the same name
    // with a suffix, and treating one as burning gives a cold fire pit a
    // flame's colour floor and an ambient fire loop.
    const bool unlitVariant = endsWithToken(n, "off") || endsWithToken(n, "out");
    const bool fireName    = isLitFireName(n);
    const bool brazierName = has(n, "brazier") || has(n, "cauldronfire");
    // A forge is a forge only when "forge" is what the name ends on. Matched as
    // a bare substring it also caught Ironforge, so all 64 doodads of the city
    // - benches, statues, cliffs, elevators, lanterns - were treated as forge
    // fire and drawn additive, which is to say translucent. Anything after the
    // token names something else: IronforgeBench is a bench, ForgeArms are
    // arms, CrystalForgeController is a control panel.
    const bool forgeName = [&] {
        if (has(n, "forgelava")) return false;
        const std::size_t i = n.rfind("forge");
        if (i == std::string::npos) return false;
        for (std::size_t k = i + 5; k < n.size(); ++k) {
            const char c = n[k];
            const bool qualifier = (c >= '0' && c <= '9') || c == '_' || c == '-'
                                || c == '.' || c == ' ';
            if (!qualifier) return false;
        }
        return true;
    }();
    const bool torchName   = has(n, "torch") && !r.isKoboldFlame && !unlitVariant;
    r.isBrazierOrFire = fireName || brazierName;
    // TaurenLampPost is the small ground-level path fire used around Camp
    // Narache, despite its misleading model name. Its decorative halo must
    // follow the lowest flame emitter rather than the mesh/card center.
    r.isGroundFire    = (fireName && !brazierName) || has(n, "taurenlamppost");
    r.isTorch         = torchName;
    r.isForge         = forgeName;

    const bool chestName    = has(n, "chest");
    const bool fountainName = has(n, "fountain");

    // ---------------------------------------------------------------
    // Sky birds / bats: animated flying doodads that look frozen beyond bone range
    // ---------------------------------------------------------------
    static constexpr auto kSkyBirdTokens = std::to_array<std::string_view>({
        "albatross", "carrionbird", "crane", "crow",
        "eagle",     "gull",        "hawk",  "osprey",
        "owl",       "parrot",      "pelican",
        "raven",     "seagull",     "vulture",
    });
    r.isSkyBird = hasAny(n, kSkyBirdTokens) || has(n, "\\bird")
                || has(n, "\\bat\\") || has(n, "\\bat.");
    // These meshes are visible from far beyond the normal skeletal LOD radius,
    // and their sweep is entirely bone-driven. Freezing distant bone matrices
    // therefore freezes the whole lighthouse beam in its bind pose.
    r.isLightBeam = has(n, "lighthousebeam") || has(n, "lightbeam")
                 || has(n, "lightray");
    // Not maintenancelight: that is the zeppelin's deck lanterns, which are
    // lamps in glass and not cones of light. They were added here while
    // hunting for the searchlight and the debug colour showed them lighting
    // up instead of it.
    r.isTransportDoodad = has(n, "transportship_sails")
                       || has(n, "icebreaker_paddlewheel");

    // ---------------------------------------------------------------
    // Animation
    // ---------------------------------------------------------------
    // Every placed model plays its own authored Stand, whatever its name, and
    // nothing is added to it. 2.4.3 moves a tree's canopy by its keyed bones
    // (StranglethornTree02: 18 rotation tracks over a 23.3 s Stand, trunk bone
    // unkeyed) and leaves a model without keys still (StranglethornRoot01-03,
    // ElwynnBush01, the static banners of the Temple of Telhamat: dr_banner01
    // has one unkeyed bone and hangs still beside WoWee's old cloth sway).
    r.disableAnimation = chestName;

    // ---------------------------------------------------------------
    // Spell effects (named tokens + particle-dominated geometry heuristic)
    // ---------------------------------------------------------------
    static constexpr auto kEffectTokens = std::to_array<std::string_view>({
        "bubbles",        "dustcloud",        "hazardlight",
        "instancenewportal", "instanceportal",
        "lavabubble",     "lavasplash",        "lavasteam",         "levelup",
        "lightshaft",     "mageportal",        "particleemitter",
        "smokepuff",      "sparkle",           "spotlight",
        "volumetriclight", "wisps",            "worldtreeportal",
    });
    // A bare "steam" substring collides with solid models - the Steam Tonk /
    // Steam Tank vehicles, Steamwheedle doodads - turning them additive/unlit
    // (glowing translucent). The low-poly gate below alone is not enough: the
    // TBC/Turtle SteamTonk overlay models are tiny proxies that slip under the
    // vertex threshold, so also exclude vehicle names outright - a "tonk"/"tank"
    // is never a steam VFX (real ones are steam/steamgeyser/lavasteam/etc.).
    const bool steamVehicle = has(n, "tonk") || has(n, "tank");
    const bool steamVfx = has(n, "steam") && !steamVehicle
                        && emitterCount >= 1 && vertexCount <= 200;
    // Emitters do not make solid geometry disposable. BubblingBowl01 has
    // 42 vertices and several emitters, but its bowl must remain opaque.
    // Only geometry-free emitter models can be identified by counts alone.
    const bool emitterOnly = emitterCount > 0 && vertexCount == 0;
    r.isSpellEffect = hasAny(n, kEffectTokens) || steamVfx || particleEmitterVfx || emitterOnly;
    // Instance portals are spell effects too.
    if (r.isInstancePortal) r.isSpellEffect = true;

    // ---------------------------------------------------------------
    // Ambient emitter type (for sound system integration)
    // ---------------------------------------------------------------
    if (r.isBrazierOrFire) {
        const bool isSmallFire = has(n, "small") || has(n, "campfire");
        r.ambientEmitterType = isSmallFire ? AmbientEmitterType::FireplaceSmall
                                           : AmbientEmitterType::FireplaceLarge;
    } else if (r.isTorch) {
        r.ambientEmitterType = AmbientEmitterType::Torch;
    } else if (forgeName) {
        r.ambientEmitterType = AmbientEmitterType::Forge;
    } else if (fountainName) {
        r.ambientEmitterType = AmbientEmitterType::Fountain;
    } else if (r.isWaterfall) {
        r.ambientEmitterType = AmbientEmitterType::Waterfall;
    }

    return r;
}

// ---------------------------------------------------------------------------
// classifyBatchTexture
// ---------------------------------------------------------------------------

M2BatchTexClassification classifyBatchTexture(const std::string& lowerTexKey)
{
    M2BatchTexClassification r;

    // The sky models keep their star points in one texture apiece, alongside
    // the cloud, nebula and planet layers of the same dome. Only the star
    // layers are named for stars - except STARSANDCLOUDS, which is both and
    // must stay, and STARBRIGHTENER, which is a brightening card rather than
    // the field itself. The directory is checked as well as the name: a floor
    // doodad called UL_SKY_FLOOR_STARS is not a sky.
    if (lowerTexKey.find("environment\\stars\\") != std::string::npos ||
        lowerTexKey.find("environments\\stars\\") != std::string::npos) {
        // The directory is called stars too, so the name has to be read on its
        // own: matching the whole path made every cloud and planet in the
        // folder a star layer, which would have deleted the sky.
        const std::size_t slash = lowerTexKey.find_last_of("\\/");
        const std::string base = (slash == std::string::npos)
            ? lowerTexKey : lowerTexKey.substr(slash + 1);
        if (base.find("stars") != std::string::npos &&
            base.find("starsandclouds") == std::string::npos &&
            base.find("starbrightener") == std::string::npos) {
            r.starPointLayer = true;
        }
    }

    // Exact paths for well-known lantern / lamp glow-card textures.
    static constexpr auto kExactGlowTextures = std::to_array<std::string_view>({
        "world\\azeroth\\karazahn\\passivedoodads\\bonfire\\flamelicksmallblue.blp",
        "world\\expansion06\\doodads\\nightelf\\7ne_druid_streetlamp01_light.blp",
        "world\\generic\\human\\passive doodads\\stormwind\\t_vfx_glow01_64.blp",
        "world\\generic\\nightelf\\passive doodads\\lamps\\glowblue32.blp",
        "world\\generic\\nightelf\\passive doodads\\magicalimplements\\glow.blp",
    });
    for (auto s : kExactGlowTextures)
        if (lowerTexKey == s) { r.exactLanternGlowTex = true; break; }

    static constexpr auto kGlowTokens = std::to_array<std::string_view>({
        "flare", "glow", "halo", "light",
    });
    static constexpr auto kFlameTokens = std::to_array<std::string_view>({
        "ember", "fire", "flame", "flamelick",
    });
    static constexpr auto kGlowCardTokens = std::to_array<std::string_view>({
        "flamelick", "genericglow", "glow", "glowball",
        "lensflare", "lightbeam",   "t_vfx",
    });
    static constexpr auto kLikelyFlameTokens = std::to_array<std::string_view>({
        "fire", "flame", "torch",
    });
    static constexpr auto kLanternFamilyTokens = std::to_array<std::string_view>({
        "elf", "lamp", "lantern", "quel", "silvermoon", "thalas",
    });
    static constexpr auto kCoolTintTokens = std::to_array<std::string_view>({
        "arcane", "blue", "nightelf",
    });
    static constexpr auto kRedTintTokens = std::to_array<std::string_view>({
        "red", "ruby", "scarlet",
    });

    r.hasGlowToken     = hasAny(lowerTexKey, kGlowTokens);
    r.hasFlameToken    = hasAnyWord(lowerTexKey, kFlameTokens);
    r.hasGlowCardToken = hasAny(lowerTexKey, kGlowCardTokens);
    if (r.exactLanternGlowTex) r.hasGlowCardToken = true;
    r.likelyFlame      = hasAnyWord(lowerTexKey, kLikelyFlameTokens);
    r.lanternFamily    = hasAny(lowerTexKey, kLanternFamilyTokens);
    // Stormwind street lamps use an opaque unlit glass texture rather than a
    // named glow card or particle emitter. Preserve that glass mesh and layer
    // a soft halo over it in the renderer.
    r.softGlowSurface  = lowerTexKey ==
        "dungeons\\textures\\doodads\\stormwindlampglass.blp";
    r.glowTint         = hasAny(lowerTexKey, kCoolTintTokens) ? 1
                       : hasAny(lowerTexKey, kRedTintTokens)  ? 2
                       : 0;

    return r;
}

// ---------------------------------------------------------------------------
// classifyAmbientEmitter - lightweight name-only emitter type detection
// ---------------------------------------------------------------------------

AmbientEmitterType classifyAmbientEmitter(const std::string& lowerName)
{
    const bool fireName    = isLitFireName(lowerName);
    const bool brazierName = has(lowerName, "brazier") || has(lowerName, "cauldronfire");
    const bool forgeName   = has(lowerName, "forge") && !has(lowerName, "forgelava");

    if (fireName || brazierName) {
        const bool isSmall = has(lowerName, "small") || has(lowerName, "campfire");
        return isSmall ? AmbientEmitterType::FireplaceSmall
                       : AmbientEmitterType::FireplaceLarge;
    }
    if (has(lowerName, "torch"))     return AmbientEmitterType::Torch;
    if (forgeName)                   return AmbientEmitterType::Forge;
    if (has(lowerName, "fountain"))  return AmbientEmitterType::Fountain;
    if (has(lowerName, "waterfall")) return AmbientEmitterType::Waterfall;
    return AmbientEmitterType::None;
}

} // namespace rendering
} // namespace wowee
