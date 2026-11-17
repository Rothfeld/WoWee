#pragma once
#include <cstdint>

namespace wowee::game {
enum class TaxiNodeStatus : uint8_t { Unqueried, Pending, Undiscovered, Discovered };

inline bool taxiDiscoveryMarkerVisible(TaxiNodeStatus status, bool questMarkerVisible) {
    return status == TaxiNodeStatus::Undiscovered && !questMarkerVisible;
}
}
