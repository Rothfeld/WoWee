// world_map_facade.hpp - Public API for the world map system.
// Drop-in replacement for the monolithic WorldMap class (Phase 10 of refactoring plan).
// Facade pattern - hides internal complexity behind the same public interface.
#pragma once

#include "rendering/world_map/world_map_types.hpp"
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <vulkan/vulkan.h>

namespace wowee {
namespace rendering {
class VkContext;
}
namespace pipeline { class AssetManager; }
namespace rendering {
namespace world_map {

class WorldMapFacade {
public:
    /// Backward-compatible alias for old WorldMap::QuestPoi usage.
    using QuestPoi = QuestPOI;

    WorldMapFacade();
    ~WorldMapFacade();

    bool initialize(VkContext* ctx, pipeline::AssetManager* am);
    void shutdown();

    /// Off-screen composite pass - call BEFORE the main render pass begins.
    void compositePass(VkCommandBuffer cmd);

    /// ImGui overlay - call INSIDE the main render pass (during ImGui frame).
    void render(const glm::vec3& playerRenderPos,
                int screenWidth, int screenHeight,
                float playerYawDeg = 0.0f);

    void setMapName(const std::string& name);
    void setServerExplorationMask(const std::vector<uint32_t>& masks, bool hasData);
    /// The zone the server says the player is in. Used in preference to working
    /// it out from map geometry, which can only guess between overlapping zones.
    void setPlayerZoneId(uint32_t zoneId);
    void setPartyDots(std::vector<PartyDot> dots);
    void setTaxiNodes(std::vector<TaxiNode> nodes);
    void setQuestPois(std::vector<QuestPOI> pois);
    void setCorpsePos(bool hasCorpse, glm::vec3 renderPos);
    /// The spirit healer a release would send the player to.
    void setGraveyardPos(bool hasGraveyard, glm::vec3 renderPos);

    /// Draw the map into this rect instead of a window of its own.
    ///
    /// In pixels from the top-left. The map is an ImGui window that centres
    /// and sizes itself, which is right when this client's interface owns it
    /// and wrong when FrameXML does: there the map belongs inside the frame
    /// FrameXML drew for it, and the title bar belongs to that frame rather
    /// than to a window underneath it. Unset, nothing changes.
    void setFrameRect(float x, float y, float w, float h, bool frameXml = false);
    void clearFrameRect();
    /// Where FrameXML's map API puts the player, [0,1] across the hosted map,
    /// or nothing when the player is not on the map shown. The arrow is drawn
    /// there rather than from this class's own view.
    void setHostedPlayerMarker(std::optional<glm::vec2> uv);
    /// Whether a frame rect is set - which, when FrameXML owns the map, is how
    /// the client knows FrameXML has its map panel on screen and wants the map
    /// drawn into it.
    [[nodiscard]] bool hasFrameRect() const;
    /// Nearby rare/rare-elite creatures currently spawned near the player.
    void setRares(std::vector<RareMark> rares);

    [[nodiscard]] bool isOpen() const;
    void close();

    /// A map that is always open, in a window of its own: Escape does not
    /// close it. Escape is read from the global keyboard state, not from the
    /// window the map is in, so closing the game's panels with it would
    /// otherwise close this map too.
    void setPersistent(bool persistent);

    /// Flight-map (taxi selection) mode - opens the map locked to the player's
    /// continent with interactive flight nodes (see TaxiNodeLayer flight-map
    /// rendering). routeProvider maps a destination node id to the hop chain
    /// used to draw the route; onSelect fires with the chosen destination;
    /// onClose fires when the player dismisses the map (Escape / X).
    void openTaxiMap(std::function<std::vector<uint32_t>(uint32_t)> routeProvider,
                     std::function<void(uint32_t)> onSelect,
                     std::function<void()> onClose);
    /// Silent close (no onClose callback) - used when the game state already
    /// closed the flight master window.
    void closeTaxiMap();
    [[nodiscard]] bool isTaxiMapOpen() const;

    /// The zone under a point on the map being shown, in [0,1] map space, or
    /// -1. Continent view only: a zone's own map has no child zones to name.
    ///
    /// The same ZMP lookup the hover highlight does, and pulled out of it so
    /// the interface can ask about a point rather than about the mouse.
    [[nodiscard]] int zoneAtMapPoint(float u, float v) const;

    /// Drill into the zone under that point, as clicking it would.
    bool clickMapPoint(float u, float v);

    /// Where a world position falls on the map now showing, as 0..1 across the
    /// image. False when it is off this map, which is how the interface's own
    /// markers ask to be hidden.
    ///
    /// The inverse of zoneAtMapPoint, and the thing GetPlayerMapPosition needs:
    /// FrameXML multiplies what that answers by the map frame's width, so a
    /// world coordinate handed over unconverted puts the player arrow thousands
    /// of pixels off the parchment.
    bool mapUVForCanonical(float wowX, float wowY, float wowZ,
                           float& u, float& v, std::optional<uint32_t> sourceMapId = std::nullopt) const;

    /// Whether the map moved itself since this was last asked, and clears it.
    ///
    /// The interface rebuilds its two dropdowns on WORLD_MAP_UPDATE, and the
    /// only move it cannot predict is the one this class makes on its own -
    /// settling on the player's zone a frame after being asked to. Polled
    /// where the frame rect is read, and answered with the event.
    [[nodiscard]] bool takeViewChanged();

    [[nodiscard]] std::vector<std::string> continentNames() const;

    /// The zones on that continent by their display names, alphabetically.
    /// One-based continent index; empty for anything else.
    [[nodiscard]] std::vector<std::string> zoneNames(int continentIndex) const;

    /// Show the zone the player is standing in. The interface asks for this
    /// every time the map is shown, and used to reach nothing.
    void showPlayerZone();

    /// Resolve selection immediately, including when the map is hidden.
    void showPlayerZone(const glm::vec3& playerRenderPos);

    /// Show the zone with that AreaTable id - what a quest names as its zone.
    /// False when it is not a zone of the map now loaded.
    bool showAreaZone(uint32_t areaTableId);

private:
    void preparePlayerZone(const glm::vec3& playerRenderPos);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace world_map
} // namespace rendering
} // namespace wowee

// Backward-compatible alias for gradual migration
namespace wowee {
namespace rendering {
using WorldMap = world_map::WorldMapFacade;
} // namespace rendering
} // namespace wowee
