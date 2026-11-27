#include <catch_amalgamated.hpp>

#include "auth/auth_handler.hpp"
#include "auth/auth_opcodes.hpp"
#include "auth/auth_packets.hpp"
#include "auth/big_num.hpp"
#include "auth/crypto.hpp"
#include "auth/srp.hpp"
#include "core/logger.hpp"
#include "network/net_platform.hpp"
#include "network/packet.hpp"
#include "network/tcp_socket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using wowee::auth::AuthHandler;
using wowee::auth::AuthOpcode;
using wowee::auth::AuthResult;
using wowee::auth::AuthState;
using wowee::auth::BigNum;
using wowee::auth::ClientInfo;
using wowee::auth::Crypto;
using wowee::auth::LogonChallengePacket;
using wowee::auth::LogonProofResponse;
using wowee::auth::LogonProofResponseParser;
using wowee::auth::RealmListResponse;
using wowee::auth::RealmListResponseParser;
using wowee::auth::SRP;
using wowee::network::Packet;
using wowee::network::TCPSocket;

// Standard 32-byte little-endian WoW SRP6 safe prime N (0x894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7).
const std::vector<uint8_t> kWowPrimeLe = {
    0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB,
    0x8F, 0x5E, 0xBF, 0xBF, 0x8E, 0xB1, 0x01, 0x08,
    0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B, 0xAD, 0xBD,
    0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89
};

struct LoopbackListener {
    socket_t listenFd = INVALID_SOCK;
    uint16_t port = 0;

    LoopbackListener() {
        wowee::net::ensureInit();
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listenFd != INVALID_SOCK);

        int reuse = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listenFd, 4) == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        REQUIRE(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        port = ntohs(bound.sin_port);
        REQUIRE(port != 0);
    }

    ~LoopbackListener() {
        if (listenFd != INVALID_SOCK) {
            wowee::net::closeSocket(listenFd);
        }
    }

    socket_t acceptClient() const {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        socket_t clientFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (clientFd != INVALID_SOCK) {
            int one = 1;
            ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&one), sizeof(one));
        }
        return clientFd;
    }
};

bool sendAllBlocking(socket_t fd, const uint8_t* data, size_t len) {
    size_t sentTotal = 0;
    while (sentTotal < len) {
        ssize_t n = wowee::net::portableSend(fd, data + sentTotal, len - sentTotal);
        if (n <= 0) {
            int err = wowee::net::lastError();
            if (n < 0 && wowee::net::isWouldBlock(err)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            return false;
        }
        sentTotal += static_cast<size_t>(n);
    }
    return true;
}

bool recvExactBlocking(socket_t fd, uint8_t* out, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = wowee::net::portableRecv(fd, out + got, len - got);
        if (n <= 0) {
            int err = wowee::net::lastError();
            if (n < 0 && wowee::net::isWouldBlock(err)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

void appendU16Le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void appendU32Le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void appendCString(std::vector<uint8_t>& out, const std::string& s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

// Build wire CMD_AUTH_LOGON_PROOF (0x01) success packet matching CMaNGOS / WoW build layout.
std::vector<uint8_t> buildServerLogonProofSuccess(const std::vector<uint8_t>& m2,
                                                  uint16_t build,
                                                  uint8_t majorVersion = 0,
                                                  uint32_t accountFlags = 0x00800000u,
                                                  uint32_t surveyId = 0u,
                                                  uint16_t loginFlags = 0u) {
    REQUIRE(m2.size() == 20);
    const size_t expectedLen = TCPSocket::expectedLogonProofSuccessSize(build, majorVersion);
    std::vector<uint8_t> wire;
    wire.reserve(expectedLen);
    wire.push_back(0x01); // CMD_AUTH_LOGON_PROOF
    wire.push_back(0x00); // AUTH_LOGON_SUCCESS
    wire.insert(wire.end(), m2.begin(), m2.end());
    if (expectedLen == 32) {
        appendU32Le(wire, accountFlags);
        appendU32Le(wire, surveyId);
        appendU16Le(wire, loginFlags);
    } else if (expectedLen == 28) {
        appendU32Le(wire, surveyId);
        appendU16Le(wire, loginFlags);
    } else {
        appendU32Le(wire, surveyId);
    }
    REQUIRE(wire.size() == expectedLen);
    return wire;
}

// Build wire CMD_REALM_LIST (0x10) response matching CMaNGOS AuthSocket::LoadRealmlist.
std::vector<uint8_t> buildServerRealmList(const std::string& realmName,
                                          const std::string& address,
                                          uint8_t realmId,
                                          bool legacyVanillaLayout,
                                          uint16_t build) {
    std::vector<uint8_t> body;
    appendU32Le(body, 0u);
    if (legacyVanillaLayout) {
        body.push_back(1u);          // uint8 realmCount
        appendU32Le(body, 1u);       // uint32 icon
        body.push_back(0x00u);       // realmFlags
        appendCString(body, realmName);
        appendCString(body, address);
        uint32_t popBits = 0;
        float pop = 1.0f;
        std::memcpy(&popBits, &pop, sizeof(float));
        appendU32Le(body, popBits);
        body.push_back(2u);          // numChars
        body.push_back(1u);          // category
        body.push_back(realmId);     // realm id
        appendU16Le(body, 0x0002u);  // trailing footer
    } else {
        appendU16Le(body, 1u);       // uint16 realmCount
        body.push_back(1u);          // uint8 icon
        body.push_back(0u);          // uint8 lock
        body.push_back(0x04u);       // REALM_FLAG_SPECIFYBUILD
        appendCString(body, realmName);
        appendCString(body, address);
        uint32_t popBits = 0;
        float pop = 1.25f;
        std::memcpy(&popBits, &pop, sizeof(float));
        appendU32Le(body, popBits);
        body.push_back(4u);          // numChars
        body.push_back(1u);          // category
        body.push_back(realmId);     // realm id
        body.push_back((build >= 10000) ? 3u : 2u);
        body.push_back((build >= 10000) ? 3u : 4u);
        body.push_back((build >= 10000) ? 5u : 3u);
        appendU16Le(body, build);
        appendU16Le(body, 0x0010u);  // trailing footer
    }

    std::vector<uint8_t> wire;
    wire.reserve(3 + body.size());
    wire.push_back(0x10u);
    appendU16Le(wire, static_cast<uint16_t>(body.size()));
    wire.insert(wire.end(), body.begin(), body.end());
    return wire;
}

template <typename Pred>
bool pumpSocketUntil(TCPSocket& sock, Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        sock.update();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sock.update();
    return pred();
}

template <typename Pred>
bool pumpAuthUntil(AuthHandler& auth, Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auth.update(0.016f);
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auth.update(0.016f);
    return pred();
}

} // namespace

TEST_CASE("TCPSocket::expectedLogonProofSuccessSize covers all client build tiers",
          "[network][auth][framing]") {
    // Vanilla 1.x (< 6299) -> 26 bytes
    CHECK(TCPSocket::expectedLogonProofSuccessSize(4544) == 26);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(5875) == 26);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(6005) == 26);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(6141) == 26);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(6298) == 26);

    // Custom Vanilla 1.x announcing high build (e.g. Turtle WoW 1.18.1 build 7272) -> 26 bytes
    CHECK(TCPSocket::expectedLogonProofSuccessSize(7272, /*majorVersion=*/1) == 26);

    // TBC 2.0.3..2.3.3 (6299..8088) -> 28 bytes
    CHECK(TCPSocket::expectedLogonProofSuccessSize(6299, /*majorVersion=*/2) == 28);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(6546, /*majorVersion=*/2) == 28);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(6898, /*majorVersion=*/2) == 28);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(7799) == 28);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(8088) == 28);

    // TBC 2.4.0+ and WotLK 3.x (>= 8089) -> 32 bytes
    CHECK(TCPSocket::expectedLogonProofSuccessSize(8089, /*majorVersion=*/2) == 32);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(8606, /*majorVersion=*/2) == 32);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(10505, /*majorVersion=*/3) == 32);
    CHECK(TCPSocket::expectedLogonProofSuccessSize(12340, /*majorVersion=*/3) == 32);
}

TEST_CASE("TCPSocket frames fragmented and coalesced AUTH_LOGON_PROOF + REALM_LIST per build",
          "[network][auth][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    const std::vector<uint8_t> expectedM2 = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9,
        0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F, 0x1E, 0x2D, 0x3C, 0x4B
    };

    SECTION("TBC 8606 (32B) split at 26B and 28B does not truncate or corrupt REALM_LIST") {
        for (size_t splitPoint : {size_t(26), size_t(28), size_t(31)}) {
            LoopbackListener listener;
            const auto proofWire = buildServerLogonProofSuccess(
                expectedM2, 8606, 2, /*accountFlags=*/0x00800000u, /*surveyId=*/0u, /*loginFlags=*/0u);
            const auto realmWire = buildServerRealmList(
                "Outland TBC", "127.0.0.1:8085", 9, /*legacyVanillaLayout=*/false, 8606);

            std::thread serverThread([&]() {
                socket_t clientFd = listener.acceptClient();
                if (clientFd == INVALID_SOCK) return;

                // Read client LOGON_CHALLENGE so TCPSocket::send auto-detects build=8606.
                uint8_t challengeBuf[64];
                recvExactBlocking(clientFd, challengeBuf, 12);

                // Send first `splitPoint` bytes (e.g. 26B or 28B) of the 32B LOGON_PROOF.
                sendAllBlocking(clientFd, proofWire.data(), splitPoint);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));

                // Send remaining LOGON_PROOF bytes + REALM_LIST split mid-header.
                sendAllBlocking(clientFd, proofWire.data() + splitPoint, proofWire.size() - splitPoint);
                sendAllBlocking(clientFd, realmWire.data(), 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                sendAllBlocking(clientFd, realmWire.data() + 2, realmWire.size() - 2);

                uint8_t drain[16];
                while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
                wowee::net::closeSocket(clientFd);
            });

            TCPSocket sock;
            std::vector<Packet> packets;
            sock.setPacketCallback([&](const Packet& pkt) { packets.push_back(pkt); });

            REQUIRE(sock.connect("127.0.0.1", listener.port));

            ClientInfo tbcInfo;
            tbcInfo.majorVersion = 2;
            tbcInfo.minorVersion = 4;
            tbcInfo.patchVersion = 3;
            tbcInfo.build = 8606;
            tbcInfo.protocolVersion = 8;
            sock.send(LogonChallengePacket::build("PLAYTESTER9", tbcInfo));
            CHECK(sock.getClientBuild() == 8606);

            REQUIRE(pumpSocketUntil(sock, [&]() { return packets.size() >= 2; },
                                    std::chrono::milliseconds(1000)));
            REQUIRE(packets.size() == 2);

            // Packet 0: complete 32-byte LOGON_PROOF
            CHECK(packets[0].getOpcode() == 0x01);
            REQUIRE(packets[0].getSize() == 32);
            Packet p0 = packets[0];
            CHECK(p0.readUInt8() == 0x01);
            LogonProofResponse proofResp;
            REQUIRE(LogonProofResponseParser::parse(p0, proofResp));
            CHECK(proofResp.isSuccess());
            CHECK(proofResp.M2 == expectedM2);
            CHECK(proofResp.accountFlags == 0x00800000u);

            // Packet 1: uncorrupted REALM_LIST
            CHECK(packets[1].getOpcode() == 0x10);
            Packet p1 = packets[1];
            CHECK(p1.readUInt8() == 0x10);
            RealmListResponse realmResp;
            REQUIRE(RealmListResponseParser::parse(p1, realmResp, /*legacyVanillaLayout=*/false));
            REQUIRE(realmResp.realms.size() == 1);
            CHECK(realmResp.realms[0].name == "Outland TBC");
            CHECK(realmResp.realms[0].address == "127.0.0.1:8085");
            CHECK(realmResp.realms[0].id == 9);
            CHECK(realmResp.realms[0].build == 8606);

            sock.disconnect();
            serverThread.join();
        }
    }

    SECTION("Early TBC 6898 (28B) neither truncates at 26B nor over-reads 32B into REALM_LIST") {
        // Test both: (a) split at 26B + 2B, and (b) coalesced 28B + REALM_LIST in one write (>32B).
        for (bool coalesced : {false, true}) {
            LoopbackListener listener;
            const auto proofWire = buildServerLogonProofSuccess(
                expectedM2, 6898, 2, 0u, /*surveyId=*/0x11223344u, /*loginFlags=*/0x0001u);
            const auto realmWire = buildServerRealmList(
                "Karazhan 2.1", "127.0.0.1:8086", 4, /*legacyVanillaLayout=*/false, 6898);

            std::thread serverThread([&]() {
                socket_t clientFd = listener.acceptClient();
                if (clientFd == INVALID_SOCK) return;

                if (!coalesced) {
                    sendAllBlocking(clientFd, proofWire.data(), 26);
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                    sendAllBlocking(clientFd, proofWire.data() + 26, 2);
                    sendAllBlocking(clientFd, realmWire.data(), realmWire.size());
                } else {
                    std::vector<uint8_t> combined = proofWire;
                    combined.insert(combined.end(), realmWire.begin(), realmWire.end());
                    sendAllBlocking(clientFd, combined.data(), combined.size());
                }

                uint8_t drain[16];
                while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
                wowee::net::closeSocket(clientFd);
            });

            TCPSocket sock;
            sock.setClientBuild(6898, 8, 2);
            std::vector<Packet> packets;
            sock.setPacketCallback([&](const Packet& pkt) { packets.push_back(pkt); });

            REQUIRE(sock.connect("127.0.0.1", listener.port));
            REQUIRE(pumpSocketUntil(sock, [&]() { return packets.size() >= 2; },
                                    std::chrono::milliseconds(1000)));
            REQUIRE(packets.size() == 2);
            REQUIRE(packets[0].getSize() == 28);

            Packet p0 = packets[0];
            CHECK(p0.readUInt8() == 0x01);
            LogonProofResponse proofResp;
            REQUIRE(LogonProofResponseParser::parse(p0, proofResp));
            CHECK(proofResp.M2 == expectedM2);
            CHECK(proofResp.surveyId == 0x11223344u);
            CHECK(proofResp.loginFlags == 0x0001u);

            Packet p1 = packets[1];
            CHECK(p1.readUInt8() == 0x10);
            RealmListResponse realmResp;
            REQUIRE(RealmListResponseParser::parse(p1, realmResp, /*legacyVanillaLayout=*/false));
            REQUIRE(realmResp.realms.size() == 1);
            CHECK(realmResp.realms[0].name == "Karazhan 2.1");
            CHECK(realmResp.realms[0].id == 4);

            sock.disconnect();
            serverThread.join();
        }
    }

    SECTION("Vanilla 5875 (26B) coalesced with REALM_LIST does not over-read 28B or 32B") {
        LoopbackListener listener;
        const auto proofWire = buildServerLogonProofSuccess(
            expectedM2, 5875, 1, 0u, /*surveyId=*/0xAABBCCDDu, 0u);
        const auto realmWire = buildServerRealmList(
            "Nostalrius", "127.0.0.1:8087", 1, /*legacyVanillaLayout=*/true, 5875);

        std::thread serverThread([&]() {
            socket_t clientFd = listener.acceptClient();
            if (clientFd == INVALID_SOCK) return;

            // Coalesce 26B proof + REALM_LIST (>= 32B total) into one TCP send!
            std::vector<uint8_t> combined = proofWire;
            combined.insert(combined.end(), realmWire.begin(), realmWire.end());
            sendAllBlocking(clientFd, combined.data(), combined.size());

            uint8_t drain[16];
            while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
            wowee::net::closeSocket(clientFd);
        });

        TCPSocket sock;
        sock.setClientBuild(5875, 3, 1);
        std::vector<Packet> packets;
        sock.setPacketCallback([&](const Packet& pkt) { packets.push_back(pkt); });

        REQUIRE(sock.connect("127.0.0.1", listener.port));
        REQUIRE(pumpSocketUntil(sock, [&]() { return packets.size() >= 2; },
                                std::chrono::milliseconds(1000)));
        REQUIRE(packets.size() == 2);
        REQUIRE(packets[0].getSize() == 26);

        Packet p0 = packets[0];
        CHECK(p0.readUInt8() == 0x01);
        LogonProofResponse proofResp;
        REQUIRE(LogonProofResponseParser::parse(p0, proofResp));
        CHECK(proofResp.M2 == expectedM2);
        CHECK(proofResp.surveyId == 0xAABBCCDDu);

        Packet p1 = packets[1];
        CHECK(p1.readUInt8() == 0x10);
        RealmListResponse realmResp;
        REQUIRE(RealmListResponseParser::parse(p1, realmResp, /*legacyVanillaLayout=*/true));
        REQUIRE(realmResp.realms.size() == 1);
        CHECK(realmResp.realms[0].name == "Nostalrius");
        CHECK(realmResp.realms[0].address == "127.0.0.1:8087");

        sock.disconnect();
        serverThread.join();
    }
}

TEST_CASE("Full AuthHandler SRP6 handshake + fragmented AUTH_LOGON_PROOF + REALM_LIST across 5875, 6898, 8606, 12340",
          "[network][auth][framing][srp]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    struct BuildCase {
        uint8_t major;
        uint8_t minor;
        uint8_t patch;
        uint16_t build;
        uint8_t protocol;
        bool vanillaRealms;
    };

    const std::array<BuildCase, 4> cases = {{
        {1, 12, 1, 5875, 3, true},
        {2, 1, 3, 6898, 8, false},
        {2, 4, 3, 8606, 5, false},
        {3, 3, 5, 12340, 8, false},
    }};

    const std::string username = "TESTACCT";
    const std::string password = "SECRET";
    const std::vector<uint8_t> authHash = Crypto::sha1(username + ":" + password);

    std::vector<uint8_t> salt(32);
    for (size_t i = 0; i < 32; ++i) {
        salt[i] = static_cast<uint8_t>(0x31u + i * 5u);
    }
    salt[31] |= 0x80u;
    salt[0] |= 0x01u;

    const BigNum N(kWowPrimeLe, true);
    const BigNum g(7);

    std::vector<uint8_t> xIn = salt;
    xIn.insert(xIn.end(), authHash.begin(), authHash.end());
    const BigNum x(Crypto::sha1(xIn), true);
    const BigNum v = g.modPow(x, N);
    const BigNum b = BigNum::fromRandom(19);
    const BigNum B = (BigNum(3).multiply(v).add(g.modPow(b, N))).mod(N);
    const std::vector<uint8_t> BWire = B.toArray(true, 32);

    for (const auto& bc : cases) {
        DYNAMIC_SECTION("End-to-end SRP6 + fragmented framing for build " << bc.build) {
            LoopbackListener listener;
            std::vector<uint8_t> serverSessionKey;

            std::thread serverThread([&]() {
                socket_t clientFd = listener.acceptClient();
                if (clientFd == INVALID_SOCK) return;

                // 1. Read client CMD_AUTH_LOGON_CHALLENGE (1 + 3 + payloadSize bytes).
                uint8_t chHdr[4] = {};
                if (!recvExactBlocking(clientFd, chHdr, 4)) {
                    wowee::net::closeSocket(clientFd);
                    return;
                }
                REQUIRE(chHdr[0] == 0x00);
                const uint16_t chBodyLen =
                    static_cast<uint16_t>(uint16_t(chHdr[2]) | (uint16_t(chHdr[3]) << 8));
                std::vector<uint8_t> chBody(chBodyLen);
                REQUIRE(recvExactBlocking(clientFd, chBody.data(), chBodyLen));
                const uint16_t clientAnnouncedBuild =
                    static_cast<uint16_t>(uint16_t(chBody[7]) | (uint16_t(chBody[8]) << 8));
                CHECK(clientAnnouncedBuild == bc.build);

                // 2. Send fragmented CMD_AUTH_LOGON_CHALLENGE success (119 bytes).
                std::vector<uint8_t> chResp;
                chResp.reserve(119);
                chResp.push_back(0x00); // cmd
                chResp.push_back(0x00); // unk
                chResp.push_back(0x00); // AUTH_LOGON_SUCCESS
                chResp.insert(chResp.end(), BWire.begin(), BWire.end());
                chResp.push_back(1);
                chResp.push_back(7);
                chResp.push_back(32);
                chResp.insert(chResp.end(), kWowPrimeLe.begin(), kWowPrimeLe.end());
                chResp.insert(chResp.end(), salt.begin(), salt.end());
                for (int i = 0; i < 16; ++i) chResp.push_back(static_cast<uint8_t>(0xA0 + i));
                chResp.push_back(0x00); // securityFlags = 0

                sendAllBlocking(clientFd, chResp.data(), 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                sendAllBlocking(clientFd, chResp.data() + 2, 48);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                sendAllBlocking(clientFd, chResp.data() + 50, chResp.size() - 50);

                // 3. Read client CMD_AUTH_LOGON_PROOF (1 + 74 = 75 bytes).
                std::vector<uint8_t> proofReq(75);
                REQUIRE(recvExactBlocking(clientFd, proofReq.data(), proofReq.size()));
                CHECK(proofReq[0] == 0x01);

                std::vector<uint8_t> AWire(proofReq.begin() + 1, proofReq.begin() + 33);
                std::vector<uint8_t> clientM1(proofReq.begin() + 33, proofReq.begin() + 53);

                std::vector<uint8_t> ab = AWire;
                ab.insert(ab.end(), BWire.begin(), BWire.end());
                const BigNum u(Crypto::sha1(ab), true);
                const BigNum A(AWire, true);
                const BigNum S = A.multiply(v.modPow(u, N)).mod(N).modPow(b, N);
                serverSessionKey = SRP::interleaveSessionKey(S.toArray(true, 32));

                std::vector<uint8_t> m2In = AWire;
                m2In.insert(m2In.end(), clientM1.begin(), clientM1.end());
                m2In.insert(m2In.end(), serverSessionKey.begin(), serverSessionKey.end());
                const std::vector<uint8_t> serverM2 = Crypto::sha1(m2In);

                // 4. Send CMD_AUTH_LOGON_PROOF fragmented right at 25B / 26B!
                const auto proofResp = buildServerLogonProofSuccess(
                    serverM2, bc.build, bc.major, 0x00800000u, 0u, 0u);
                const size_t firstChunk = (proofResp.size() > 26) ? 26 : 18;
                sendAllBlocking(clientFd, proofResp.data(), firstChunk);
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
                sendAllBlocking(clientFd, proofResp.data() + firstChunk,
                                proofResp.size() - firstChunk);

                // 5. Read client CMD_REALM_LIST (1 + 4 = 5 bytes) and send fragmented realm list.
                uint8_t rlReq[5] = {};
                REQUIRE(recvExactBlocking(clientFd, rlReq, sizeof(rlReq)));
                CHECK(rlReq[0] == 0x10);

                const auto rlResp = buildServerRealmList(
                    "BuildRealm", "127.0.0.1:8085", 3, bc.vanillaRealms, bc.build);
                sendAllBlocking(clientFd, rlResp.data(), 2);
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                sendAllBlocking(clientFd, rlResp.data() + 2, rlResp.size() - 2);

                uint8_t drain[16];
                while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
                wowee::net::closeSocket(clientFd);
            });

            AuthHandler auth;
            ClientInfo info;
            info.majorVersion = bc.major;
            info.minorVersion = bc.minor;
            info.patchVersion = bc.patch;
            info.build = bc.build;
            info.protocolVersion = bc.protocol;
            info.legacyVanillaRealmList = bc.vanillaRealms;
            auth.setClientInfo(info);

            bool authSucceeded = false;
            auth.setOnSuccess([&](const std::vector<uint8_t>&) {
                authSucceeded = true;
                auth.requestRealmList();
            });

            REQUIRE(auth.connect("127.0.0.1", listener.port));
            auth.authenticate(username, password);

            REQUIRE(pumpAuthUntil(auth, [&]() {
                return auth.getState() == AuthState::REALM_LIST_RECEIVED ||
                       auth.getState() == AuthState::FAILED;
            }, std::chrono::milliseconds(2000)));

            CHECK(authSucceeded);
            REQUIRE(auth.getState() == AuthState::REALM_LIST_RECEIVED);
            CHECK(auth.getSessionKey() == serverSessionKey);
            REQUIRE(auth.getRealms().size() == 1);
            CHECK(auth.getRealms()[0].name == "BuildRealm");
            CHECK(auth.getRealms()[0].address == "127.0.0.1:8085");
            CHECK(auth.getRealms()[0].id == 3);

            auth.disconnect();
            serverThread.join();
        }
    }
}

TEST_CASE("TCPSocket frames 2-byte and split 4-byte AUTH_LOGON_PROOF failure packets cleanly",
          "[network][auth][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    SECTION("TBC 8606 split 2+2 wrong-password failure consumes all 4 bytes without residue") {
        LoopbackListener listener;

        std::thread serverThread([&]() {
            socket_t clientFd = listener.acceptClient();
            if (clientFd == INVALID_SOCK) return;

            // CMaNGOS logonProofUnknownAccount = {0x01, 0x04, 0x00, 0x00} on build > 6005,
            // keeping socket open via ProcessIncomingData().
            const uint8_t failWire[4] = {0x01, 0x04, 0x00, 0x00};
            sendAllBlocking(clientFd, failWire, 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            sendAllBlocking(clientFd, failWire + 2, 2);

            // Follow with a 3-byte challenge failure to prove zero residual bytes remained.
            const uint8_t nextWire[3] = {0x00, 0x00, 0x03};
            sendAllBlocking(clientFd, nextWire, sizeof(nextWire));

            uint8_t drain[16];
            while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
            wowee::net::closeSocket(clientFd);
        });

        TCPSocket sock;
        sock.setClientBuild(8606, 5, 2);
        std::vector<Packet> packets;
        sock.setPacketCallback([&](const Packet& pkt) { packets.push_back(pkt); });

        REQUIRE(sock.connect("127.0.0.1", listener.port));
        REQUIRE(pumpSocketUntil(sock, [&]() { return packets.size() >= 2; },
                                std::chrono::milliseconds(1000)));
        REQUIRE(packets.size() == 2);
        CHECK(packets[0].getOpcode() == 0x01);
        CHECK(packets[0].getSize() == 4);
        CHECK(packets[1].getOpcode() == 0x00);
        CHECK(packets[1].getSize() == 3);

        sock.disconnect();
        serverThread.join();
    }

    SECTION("CMaNGOS 2-byte BUILD_INVALID (0x09) on TBC 8606 dispatches without waiting for 4 bytes") {
        LoopbackListener listener;

        std::thread serverThread([&]() {
            socket_t clientFd = listener.acceptClient();
            if (clientFd == INVALID_SOCK) return;

            const uint8_t verInvalid[2] = {0x01, 0x09};
            sendAllBlocking(clientFd, verInvalid, sizeof(verInvalid));

            // Keep socket open (matching CMaNGOS 30s ArmTimeoutTimer).
            uint8_t drain[16];
            while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
            wowee::net::closeSocket(clientFd);
        });

        TCPSocket sock;
        sock.setClientBuild(8606, 5, 2);
        std::vector<Packet> packets;
        sock.setPacketCallback([&](const Packet& pkt) { packets.push_back(pkt); });

        REQUIRE(sock.connect("127.0.0.1", listener.port));
        REQUIRE(pumpSocketUntil(sock, [&]() { return !packets.empty(); },
                                std::chrono::milliseconds(500)));
        REQUIRE(packets.size() == 1);
        CHECK(packets[0].getOpcode() == 0x01);
        CHECK(packets[0].getSize() == 2);
        CHECK(packets[0].getData()[1] == 0x09);

        sock.disconnect();
        serverThread.join();
    }
}
