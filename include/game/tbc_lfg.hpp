#pragma once
#include "network/packet.hpp"
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace wowee::game {
struct TbcLfgMember { uint64_t guid = 0; uint32_t level = 0; };
struct TbcLfgResult {
    uint64_t guid = 0;
    uint32_t level = 0, zone = 0;
    bool lfm = false;
    std::array<uint32_t, 3> slots{};
    std::string comment;
    std::vector<TbcLfgMember> members;
};
struct TbcLfgState {
    std::array<uint32_t, 3> slots{};
    std::array<int, 3> types{1, 1, 1};
    uint32_t more = 0;
    int moreType = 1;
    std::string comment;
    bool queued = false, lfg = false, lfm = false, autofill = false;
    uint32_t queryType = 0, queryEntry = 0, total = 0;
    std::vector<TbcLfgResult> results;
    std::chrono::steady_clock::time_point nextQuery{};
};
inline void requireLfgBytes(const network::Packet& packet, size_t size) {
    if (!packet.hasRemaining(size)) throw std::runtime_error("Truncated TBC LFG packet");
}
inline uint64_t readLfgGuid(network::Packet& packet) {
    requireLfgBytes(packet, 1);
    const auto mask = packet.readUInt8();
    uint64_t guid = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (!(mask & (1u << bit))) continue;
        requireLfgBytes(packet, 1);
        guid |= uint64_t(packet.readUInt8()) << (bit * 8);
    }
    return guid;
}
inline void readTbcLfgResults(network::Packet& packet, TbcLfgState& state) {
    requireLfgBytes(packet, 16);
    const auto type = packet.readUInt32(), entry = packet.readUInt32();
    const auto count = packet.readUInt32(), total = packet.readUInt32();
    if (count > 50 || total < count) throw std::runtime_error("Invalid TBC LFG result count");
    std::vector<TbcLfgResult> results;
    for (uint32_t i = 0; i < count; ++i) {
        TbcLfgResult row;
        row.guid = readLfgGuid(packet);
        requireLfgBytes(packet, 21);
        row.level = packet.readUInt32(); row.zone = packet.readUInt32();
        row.lfm = packet.readUInt8() != 0;
        for (auto& slot : row.slots) slot = packet.readUInt32();
        for (;;) {
            requireLfgBytes(packet, 1);
            const char value = static_cast<char>(packet.readUInt8());
            if (!value) break;
            row.comment += value;
        }
        requireLfgBytes(packet, 4);
        const auto members = packet.readUInt32();
        if (members > 39) throw std::runtime_error("Invalid TBC LFG party size");
        for (uint32_t j = 0; j < members; ++j) {
            const auto guid = readLfgGuid(packet);
            requireLfgBytes(packet, 4);
            row.members.push_back({guid, packet.readUInt32()});
        }
        results.push_back(std::move(row));
    }
    state.queryType = type; state.queryEntry = entry; state.total = total;
    state.results = std::move(results);
}
}
