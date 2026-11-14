#pragma once

#include <array>
#include <cstdint>

namespace wowee::game {

// TBC service tracking is client-side. NPC masks match UNIT_NPC_FLAGS;
// mailboxes instead use GameObject type 19.
struct TrackingService {
    const char* label;
    const char* texture;
    uint32_t npcMask;
};

inline constexpr std::array<TrackingService, 13> tbcTrackingServices{{
    {"MINIMAP_TRACKING_REPAIR", "Interface\\Minimap\\Tracking\\Repair", 0x1000},
    {"MINIMAP_TRACKING_VENDOR_FOOD", "Interface\\Minimap\\Tracking\\Food", 0x200},
    {"MINIMAP_TRACKING_VENDOR_POISON", "Interface\\Minimap\\Tracking\\Poisons", 0x400},
    {"MINIMAP_TRACKING_VENDOR_AMMO", "Interface\\Minimap\\Tracking\\Ammunition", 0x100},
    {"MINIMAP_TRACKING_VENDOR_REAGENT", "Interface\\Minimap\\Tracking\\Reagents", 0x800},
    {"MINIMAP_TRACKING_INNKEEPER", "Interface\\Minimap\\Tracking\\Innkeeper", 0x10000},
    {"MINIMAP_TRACKING_FLIGHTMASTER", "Interface\\Minimap\\Tracking\\FlightMaster", 0x2000},
    {"MINIMAP_TRACKING_BATTLEMASTER", "Interface\\Minimap\\Tracking\\BattleMaster", 0x100000},
    {"MINIMAP_TRACKING_TRAINER_CLASS", "Interface\\Minimap\\Tracking\\Class", 0x20},
    {"MINIMAP_TRACKING_TRAINER_PROFESSION", "Interface\\Minimap\\Tracking\\Profession", 0x40},
    {"MINIMAP_TRACKING_AUCTIONEER", "Interface\\Minimap\\Tracking\\Auctioneer", 0x200000},
    {"MINIMAP_TRACKING_BANKER", "Interface\\Minimap\\Tracking\\Banker", 0x20000},
    {"MINIMAP_TRACKING_MAILBOX", "Interface\\Minimap\\Tracking\\Mailbox", 0},
}};

} // namespace wowee::game
