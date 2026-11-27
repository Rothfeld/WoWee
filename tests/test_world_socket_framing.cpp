#include <catch_amalgamated.hpp>

#include "auth/crypto.hpp"
#include "auth/rc4.hpp"
#include "auth/vanilla_crypt.hpp"
#include "core/logger.hpp"
#include "game/opcode_table.hpp"
#include "network/net_platform.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using wowee::auth::Crypto;
using wowee::auth::RC4;
using wowee::auth::VanillaCrypt;
using wowee::network::Packet;
using wowee::network::WorldSocket;

constexpr uint16_t kSmsgAuthChallenge = 0x01EC;
constexpr uint16_t kCmsgAuthSession   = 0x01ED;
constexpr uint16_t kSmsgAuthResponse  = 0x01EE;
constexpr uint16_t kSmsgAddonInfo     = 0x02EF;
constexpr uint16_t kSmsgCompressedUpdateObject = 0x01F6;

constexpr uint8_t kCmangosTbcSeed[16] = {
    0x38, 0xA7, 0x83, 0x15, 0xF8, 0x92, 0x25, 0x30,
    0x71, 0x98, 0x67, 0xB1, 0x8C, 0x04, 0xE2, 0xAA
};

constexpr uint8_t kWotlkServerEncryptSeed[16] = {
    0xCC, 0x98, 0xAE, 0x04, 0xE8, 0x97, 0xEA, 0xCA,
    0x12, 0xDD, 0xC0, 0x93, 0x42, 0x91, 0x53, 0x57
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

std::vector<uint8_t> buildPlaintextSmsg(uint16_t opcode, const std::vector<uint8_t>& payload) {
    const uint16_t sizeField = static_cast<uint16_t>(payload.size() + 2);
    std::vector<uint8_t> wire;
    wire.reserve(4 + payload.size());
    wire.push_back(static_cast<uint8_t>((sizeField >> 8) & 0xFF));
    wire.push_back(static_cast<uint8_t>(sizeField & 0xFF));
    wire.push_back(static_cast<uint8_t>(opcode & 0xFF));
    wire.push_back(static_cast<uint8_t>((opcode >> 8) & 0xFF));
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}

std::vector<uint8_t> buildTbcEncryptedSmsg(VanillaCrypt& serverCrypt,
                                           uint16_t opcode,
                                           const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> wire = buildPlaintextSmsg(opcode, payload);
    serverCrypt.encrypt(wire.data(), 4);
    return wire;
}

std::vector<uint8_t> buildWotlkEncryptedSmsg(RC4& serverSendCipher,
                                             uint16_t opcode,
                                             const std::vector<uint8_t>& payload) {
    const uint32_t sizeField = static_cast<uint32_t>(payload.size() + 2);
    std::vector<uint8_t> wire;
    if (sizeField > 0x7FFFu) {
        wire.reserve(5 + payload.size());
        wire.push_back(static_cast<uint8_t>(0x80u | ((sizeField >> 16) & 0x7Fu)));
        wire.push_back(static_cast<uint8_t>((sizeField >> 8) & 0xFFu));
        wire.push_back(static_cast<uint8_t>(sizeField & 0xFFu));
        wire.push_back(static_cast<uint8_t>(opcode & 0xFFu));
        wire.push_back(static_cast<uint8_t>((opcode >> 8) & 0xFFu));
        serverSendCipher.process(wire.data(), 5);
    } else {
        wire.reserve(4 + payload.size());
        wire.push_back(static_cast<uint8_t>((sizeField >> 8) & 0xFFu));
        wire.push_back(static_cast<uint8_t>(sizeField & 0xFFu));
        wire.push_back(static_cast<uint8_t>(opcode & 0xFFu));
        wire.push_back(static_cast<uint8_t>((opcode >> 8) & 0xFFu));
        serverSendCipher.process(wire.data(), 4);
    }
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}

VanillaCrypt makeTbcServerCrypt(const std::vector<uint8_t>& sessionKey) {
    std::vector<uint8_t> seed(kCmangosTbcSeed, kCmangosTbcSeed + sizeof(kCmangosTbcSeed));
    std::vector<uint8_t> headerKey = Crypto::hmacSHA1(seed, sessionKey);
    VanillaCrypt crypt;
    crypt.init(headerKey);
    return crypt;
}

RC4 makeWotlkServerSendCipher(const std::vector<uint8_t>& sessionKey) {
    std::vector<uint8_t> seed(kWotlkServerEncryptSeed,
                              kWotlkServerEncryptSeed + sizeof(kWotlkServerEncryptSeed));
    std::vector<uint8_t> hash = Crypto::hmacSHA1(seed, sessionKey);
    RC4 cipher;
    cipher.init(hash);
    cipher.drop(1024);
    return cipher;
}

// Find a 40-byte session key whose first TBC encrypted SMSG header byte has
// bit 0x80 set (so raw big-endian size > 32768 when mistakenly parsed with enc=0,
// matching the exact 'oversized packet size=58029 rawHdr=e2 ad 23 86 enc=0' symptom).
std::vector<uint8_t> makeHighBitTbcSessionKey() {
    for (uint32_t salt = 1; salt < 10000; ++salt) {
        std::vector<uint8_t> key(40, 0);
        for (size_t i = 0; i < 40; ++i) {
            key[i] = static_cast<uint8_t>((salt * 37u + i * 13u) & 0xFFu);
        }
        VanillaCrypt probe = makeTbcServerCrypt(key);
        std::vector<uint8_t> sample = buildTbcEncryptedSmsg(probe, kSmsgAddonInfo, {1, 2, 3, 4});
        const uint16_t rawSize = static_cast<uint16_t>((uint16_t(sample[0]) << 8) | sample[1]);
        if (rawSize > 0x8000u) {
            return key;
        }
    }
    FAIL("Failed to synthesize session key with rawSize > 0x8000");
    return std::vector<uint8_t>(40, 0x42);
}

bool readUnencryptedClientPacket(socket_t fd, uint16_t& opcodeOut, std::vector<uint8_t>& payloadOut) {
    uint8_t hdr[6] = {};
    if (!recvExactBlocking(fd, hdr, sizeof(hdr))) return false;
    const uint16_t sizeField = static_cast<uint16_t>((uint16_t(hdr[0]) << 8) | hdr[1]);
    if (sizeField < 4) return false;
    opcodeOut = static_cast<uint16_t>(hdr[2] | (uint16_t(hdr[3]) << 8));
    const size_t payloadLen = static_cast<size_t>(sizeField - 4);
    payloadOut.resize(payloadLen);
    if (payloadLen > 0 && !recvExactBlocking(fd, payloadOut.data(), payloadLen)) {
        return false;
    }
    return true;
}

template <typename Pred>
bool pumpClientUntil(WorldSocket& ws, Pred&& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ws.update();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ws.update();
    return pred();
}

} // namespace

TEST_CASE("WorldSocket survives async recv race between CMSG_AUTH_SESSION and initEncryption",
          "[network][world_socket][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    const std::vector<uint8_t> sessionKey = makeHighBitTbcSessionKey();
    LoopbackListener listener;

    const std::vector<uint8_t> expectedAddonPayload = {0x02, 0x01, 0x00, 0x00, 0x00, 0x00};
    const std::vector<uint8_t> expectedAuthRespPayload = {
        0x0C, // AUTH_OK
        0x00, 0x00, 0x00, 0x00,
        0x00,
        0x00, 0x00, 0x00, 0x00,
        0x01  // expansion = TBC
    };

    std::atomic<uint16_t> observedFirstRawCipherSize{0};

    std::thread serverThread([&]() {
        socket_t clientFd = listener.acceptClient();
        if (clientFd == INVALID_SOCK) return;

        // 1. Send unencrypted SMSG_AUTH_CHALLENGE (4-byte seed).
        std::vector<uint8_t> challenge = buildPlaintextSmsg(kSmsgAuthChallenge, {0xDE, 0xAD, 0xBE, 0xEF});
        sendAllBlocking(clientFd, challenge.data(), challenge.size());

        // 2. Read unencrypted CMSG_AUTH_SESSION and immediately fire encrypted
        //    SMSG_ADDON_INFO + SMSG_AUTH_RESPONSE before client calls initEncryption().
        uint16_t cmsgOp = 0;
        std::vector<uint8_t> cmsgBody;
        if (readUnencryptedClientPacket(clientFd, cmsgOp, cmsgBody) && cmsgOp == kCmsgAuthSession) {
            VanillaCrypt serverCrypt = makeTbcServerCrypt(sessionKey);
            std::vector<uint8_t> addonWire = buildTbcEncryptedSmsg(
                serverCrypt, kSmsgAddonInfo, expectedAddonPayload);
            std::vector<uint8_t> authRespWire = buildTbcEncryptedSmsg(
                serverCrypt, kSmsgAuthResponse, expectedAuthRespPayload);

            observedFirstRawCipherSize.store(
                static_cast<uint16_t>((uint16_t(addonWire[0]) << 8) | addonWire[1]),
                std::memory_order_release);

            std::vector<uint8_t> burst;
            burst.reserve(addonWire.size() + authRespWire.size());
            burst.insert(burst.end(), addonWire.begin(), addonWire.end());
            burst.insert(burst.end(), authRespWire.begin(), authRespWire.end());
            sendAllBlocking(clientFd, burst.data(), burst.size());
        }

        // Keep connection open until client finishes assertions.
        uint8_t drain[16];
        while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
        wowee::net::closeSocket(clientFd);
    });

    WorldSocket ws;
    std::mutex rxMutex;
    std::vector<Packet> received;
    ws.setPacketCallback([&](const Packet& pkt) {
        std::lock_guard<std::mutex> lock(rxMutex);
        received.push_back(pkt);
    });

    REQUIRE(ws.connect("127.0.0.1", listener.port));

    // Wait for SMSG_AUTH_CHALLENGE.
    REQUIRE(pumpClientUntil(ws, [&]() {
        std::lock_guard<std::mutex> lock(rxMutex);
        return !received.empty();
    }, std::chrono::milliseconds(1000)));
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        REQUIRE(received.size() == 1);
        CHECK(received[0].getOpcode() == kSmsgAuthChallenge);
        received.clear();
    }

    // Send CMSG_AUTH_SESSION, then deliberately sleep 30ms BEFORE calling
    // initEncryption() so asyncPumpThread_ is guaranteed to recv() the server's
    // encrypted response burst while encryptionEnabled is still false.
    Packet authSession(kCmsgAuthSession, {0x9E, 0x21, 0x00, 0x00, 'T', 'E', 'S', 'T', 0x00});
    ws.send(authSession);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Verify our test vector actually had rawSize > 0x8000 (the exact desync trigger).
    CHECK(observedFirstRawCipherSize.load(std::memory_order_acquire) > 0x8000u);
    REQUIRE(ws.isConnected());

    ws.initEncryption(sessionKey, 8606);
    REQUIRE(ws.isConnected());

    REQUIRE(pumpClientUntil(ws, [&]() {
        std::lock_guard<std::mutex> lock(rxMutex);
        return received.size() >= 2;
    }, std::chrono::milliseconds(1000)));

    {
        std::lock_guard<std::mutex> lock(rxMutex);
        REQUIRE(received.size() == 2);
        CHECK(received[0].getOpcode() == kSmsgAddonInfo);
        CHECK(received[0].getData() == expectedAddonPayload);
        CHECK(received[1].getOpcode() == kSmsgAuthResponse);
        CHECK(received[1].getData() == expectedAuthRespPayload);
    }

    ws.disconnect();
    serverThread.join();
}

TEST_CASE("WorldSocket handles split encrypted header across initEncryption boundary",
          "[network][world_socket][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    const std::vector<uint8_t> sessionKey = makeHighBitTbcSessionKey();
    LoopbackListener listener;
    const std::vector<uint8_t> expectedPayload = {0x0C, 0x11, 0x22, 0x33, 0x44, 0x00, 0x01};
    std::atomic<bool> clientInitDone{false};

    std::thread serverThread([&]() {
        socket_t clientFd = listener.acceptClient();
        if (clientFd == INVALID_SOCK) return;

        std::vector<uint8_t> challenge = buildPlaintextSmsg(kSmsgAuthChallenge, {1, 2, 3, 4});
        sendAllBlocking(clientFd, challenge.data(), challenge.size());

        uint16_t cmsgOp = 0;
        std::vector<uint8_t> cmsgBody;
        if (readUnencryptedClientPacket(clientFd, cmsgOp, cmsgBody) && cmsgOp == kCmsgAuthSession) {
            VanillaCrypt serverCrypt = makeTbcServerCrypt(sessionKey);
            std::vector<uint8_t> authRespWire = buildTbcEncryptedSmsg(
                serverCrypt, kSmsgAuthResponse, expectedPayload);

            // Send only first 2 bytes of encrypted header BEFORE initEncryption().
            sendAllBlocking(clientFd, authRespWire.data(), 2);
            while (!clientInitDone.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Send remaining 2 header bytes + payload AFTER initEncryption().
            sendAllBlocking(clientFd, authRespWire.data() + 2, authRespWire.size() - 2);
        }

        uint8_t drain[16];
        while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
        wowee::net::closeSocket(clientFd);
    });

    WorldSocket ws;
    std::mutex rxMutex;
    std::vector<Packet> received;
    ws.setPacketCallback([&](const Packet& pkt) {
        std::lock_guard<std::mutex> lock(rxMutex);
        received.push_back(pkt);
    });

    REQUIRE(ws.connect("127.0.0.1", listener.port));
    REQUIRE(pumpClientUntil(ws, [&]() {
        std::lock_guard<std::mutex> lock(rxMutex);
        return !received.empty();
    }, std::chrono::milliseconds(1000)));
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        received.clear();
    }

    ws.send(Packet(kCmsgAuthSession, {1, 2, 3, 4}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ws.initEncryption(sessionKey, 8606);
    clientInitDone.store(true, std::memory_order_release);

    REQUIRE(pumpClientUntil(ws, [&]() {
        std::lock_guard<std::mutex> lock(rxMutex);
        return !received.empty();
    }, std::chrono::milliseconds(1000)));

    {
        std::lock_guard<std::mutex> lock(rxMutex);
        REQUIRE(received.size() == 1);
        CHECK(received[0].getOpcode() == kSmsgAuthResponse);
        CHECK(received[0].getData() == expectedPayload);
    }

    ws.disconnect();
    serverThread.join();
}

TEST_CASE("WorldSocket delivers pre-crypt plaintext SMSG_AUTH_RESPONSE rejection on server close",
          "[network][world_socket][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    const std::vector<uint8_t> sessionKey = makeHighBitTbcSessionKey();
    LoopbackListener listener;

    std::thread serverThread([&]() {
        socket_t clientFd = listener.acceptClient();
        if (clientFd == INVALID_SOCK) return;

        std::vector<uint8_t> challenge = buildPlaintextSmsg(kSmsgAuthChallenge, {9, 8, 7, 6});
        sendAllBlocking(clientFd, challenge.data(), challenge.size());

        uint16_t cmsgOp = 0;
        std::vector<uint8_t> cmsgBody;
        if (readUnencryptedClientPacket(clientFd, cmsgOp, cmsgBody) && cmsgOp == kCmsgAuthSession) {
            // CMaNGOS sends 5-byte unencrypted SMSG_AUTH_RESPONSE before AuthCrypt::Init
            // when rejecting unknown/banned/version-mismatched accounts and closes socket.
            std::vector<uint8_t> rejectWire = buildPlaintextSmsg(kSmsgAuthResponse, {0x15}); // AUTH_UNKNOWN_ACCOUNT
            sendAllBlocking(clientFd, rejectWire.data(), rejectWire.size());
        }
        wowee::net::closeSocket(clientFd);
    });

    WorldSocket ws;
    std::mutex rxMutex;
    std::vector<Packet> received;
    ws.setPacketCallback([&](const Packet& pkt) {
        std::lock_guard<std::mutex> lock(rxMutex);
        received.push_back(pkt);
    });

    REQUIRE(ws.connect("127.0.0.1", listener.port));
    REQUIRE(pumpClientUntil(ws, [&]() {
        std::lock_guard<std::mutex> lock(rxMutex);
        return !received.empty();
    }, std::chrono::milliseconds(1000)));
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        received.clear();
    }

    ws.send(Packet(kCmsgAuthSession, {1, 2, 3, 4}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ws.initEncryption(sessionKey, 8606);

    REQUIRE(pumpClientUntil(ws, [&]() {
        std::lock_guard<std::mutex> lock(rxMutex);
        return !received.empty();
    }, std::chrono::milliseconds(1000)));

    {
        std::lock_guard<std::mutex> lock(rxMutex);
        REQUIRE(received.size() == 1);
        CHECK(received[0].getOpcode() == kSmsgAuthResponse);
        REQUIRE(received[0].getData().size() == 1);
        CHECK(received[0].getData()[0] == 0x15);
    }

    ws.disconnect();
    serverThread.join();
}

TEST_CASE("WorldSocket parses >32KB TBC 16-bit headers and WotLK 5-byte 0x80 large headers",
          "[network][world_socket][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    SECTION("TBC 40KB SMSG_COMPRESSED_UPDATE_OBJECT (uint16 size > 0x8000)") {
        const std::vector<uint8_t> sessionKey = makeHighBitTbcSessionKey();
        LoopbackListener listener;

        std::vector<uint8_t> largePayload(40000);
        for (size_t i = 0; i < largePayload.size(); ++i) {
            largePayload[i] = static_cast<uint8_t>((i * 73u + 19u) & 0xFFu);
        }

        std::thread serverThread([&]() {
            socket_t clientFd = listener.acceptClient();
            if (clientFd == INVALID_SOCK) return;

            std::vector<uint8_t> challenge = buildPlaintextSmsg(kSmsgAuthChallenge, {1, 1, 1, 1});
            sendAllBlocking(clientFd, challenge.data(), challenge.size());

            uint16_t cmsgOp = 0;
            std::vector<uint8_t> cmsgBody;
            if (readUnencryptedClientPacket(clientFd, cmsgOp, cmsgBody) && cmsgOp == kCmsgAuthSession) {
                VanillaCrypt serverCrypt = makeTbcServerCrypt(sessionKey);
                std::vector<uint8_t> authOk = buildTbcEncryptedSmsg(
                    serverCrypt, kSmsgAuthResponse, {0x0C});
                std::vector<uint8_t> bigPkt = buildTbcEncryptedSmsg(
                    serverCrypt, kSmsgCompressedUpdateObject, largePayload);
                sendAllBlocking(clientFd, authOk.data(), authOk.size());
                sendAllBlocking(clientFd, bigPkt.data(), bigPkt.size());
            }

            uint8_t drain[16];
            while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
            wowee::net::closeSocket(clientFd);
        });

        WorldSocket ws;
        std::mutex rxMutex;
        std::vector<Packet> received;
        ws.setPacketCallback([&](const Packet& pkt) {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.push_back(pkt);
        });

        REQUIRE(ws.connect("127.0.0.1", listener.port));
        REQUIRE(pumpClientUntil(ws, [&]() {
            std::lock_guard<std::mutex> lock(rxMutex);
            return !received.empty();
        }, std::chrono::milliseconds(1000)));
        {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.clear();
        }

        ws.send(Packet(kCmsgAuthSession, {1, 2, 3, 4}));
        ws.initEncryption(sessionKey, 8606);

        REQUIRE(pumpClientUntil(ws, [&]() {
            std::lock_guard<std::mutex> lock(rxMutex);
            return received.size() >= 2;
        }, std::chrono::milliseconds(2000)));

        {
            std::lock_guard<std::mutex> lock(rxMutex);
            REQUIRE(received.size() == 2);
            CHECK(received[0].getOpcode() == kSmsgAuthResponse);
            CHECK(received[1].getOpcode() == kSmsgCompressedUpdateObject);
            CHECK(received[1].getData() == largePayload);
        }
        CHECK(ws.isConnected());
        ws.disconnect();
        serverThread.join();
    }

    SECTION("WotLK 45KB 5-byte (0x80) RC4 header split 4-then-1 across TCP segments") {
        std::vector<uint8_t> sessionKey(40);
        for (size_t i = 0; i < sessionKey.size(); ++i) {
            sessionKey[i] = static_cast<uint8_t>(0xA0u + i);
        }
        LoopbackListener listener;

        std::vector<uint8_t> largePayload(45000);
        for (size_t i = 0; i < largePayload.size(); ++i) {
            largePayload[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
        }

        std::thread serverThread([&]() {
            socket_t clientFd = listener.acceptClient();
            if (clientFd == INVALID_SOCK) return;

            std::vector<uint8_t> challenge = buildPlaintextSmsg(kSmsgAuthChallenge, {2, 2, 2, 2});
            sendAllBlocking(clientFd, challenge.data(), challenge.size());

            uint16_t cmsgOp = 0;
            std::vector<uint8_t> cmsgBody;
            if (readUnencryptedClientPacket(clientFd, cmsgOp, cmsgBody) && cmsgOp == kCmsgAuthSession) {
                RC4 serverCipher = makeWotlkServerSendCipher(sessionKey);
                std::vector<uint8_t> authOk = buildWotlkEncryptedSmsg(
                    serverCipher, kSmsgAuthResponse, {0x0C});
                std::vector<uint8_t> bigPkt = buildWotlkEncryptedSmsg(
                    serverCipher, kSmsgCompressedUpdateObject, largePayload);

                sendAllBlocking(clientFd, authOk.data(), authOk.size());
                // Deliberately send only first 4 bytes of the 5-byte WotLK header,
                // pause so client decrypts bytes 0..3 and sees bit 0x80, then send
                // byte 4 + payload + a trailing small packet to verify cipher alignment!
                sendAllBlocking(clientFd, bigPkt.data(), 4);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                sendAllBlocking(clientFd, bigPkt.data() + 4, bigPkt.size() - 4);

                std::vector<uint8_t> tailPkt = buildWotlkEncryptedSmsg(
                    serverCipher, kSmsgAddonInfo, {0x55, 0xAA});
                sendAllBlocking(clientFd, tailPkt.data(), tailPkt.size());
            }

            uint8_t drain[16];
            while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
            wowee::net::closeSocket(clientFd);
        });

        WorldSocket ws;
        std::mutex rxMutex;
        std::vector<Packet> received;
        ws.setPacketCallback([&](const Packet& pkt) {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.push_back(pkt);
        });

        REQUIRE(ws.connect("127.0.0.1", listener.port));
        REQUIRE(pumpClientUntil(ws, [&]() {
            std::lock_guard<std::mutex> lock(rxMutex);
            return !received.empty();
        }, std::chrono::milliseconds(1000)));
        {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.clear();
        }

        ws.send(Packet(kCmsgAuthSession, {1, 2, 3, 4}));
        ws.initEncryption(sessionKey, 12340);

        REQUIRE(pumpClientUntil(ws, [&]() {
            std::lock_guard<std::mutex> lock(rxMutex);
            return received.size() >= 3;
        }, std::chrono::milliseconds(2000)));

        {
            std::lock_guard<std::mutex> lock(rxMutex);
            REQUIRE(received.size() == 3);
            CHECK(received[0].getOpcode() == kSmsgAuthResponse);
            CHECK(received[1].getOpcode() == kSmsgCompressedUpdateObject);
            CHECK(received[1].getData() == largePayload);
            CHECK(received[2].getOpcode() == kSmsgAddonInfo);
            CHECK(received[2].getData() == std::vector<uint8_t>{0x55, 0xAA});
        }
        CHECK(ws.isConnected());
        ws.disconnect();
        serverThread.join();
    }
}

TEST_CASE("WorldSocket 50-connect loopback stress across TBC and WotLK handshakes",
          "[network][world_socket][framing]") {
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::WARNING);

    const std::vector<uint8_t> sessionKey = makeHighBitTbcSessionKey();
    LoopbackListener listener;
    constexpr int kIterations = 50;

    std::thread serverThread([&]() {
        for (int iter = 0; iter < kIterations; ++iter) {
            socket_t clientFd = listener.acceptClient();
            if (clientFd == INVALID_SOCK) return;

            std::vector<uint8_t> challenge = buildPlaintextSmsg(
                kSmsgAuthChallenge, {static_cast<uint8_t>(iter), 1, 2, 3});
            sendAllBlocking(clientFd, challenge.data(), challenge.size());

            uint16_t cmsgOp = 0;
            std::vector<uint8_t> cmsgBody;
            if (readUnencryptedClientPacket(clientFd, cmsgOp, cmsgBody) && cmsgOp == kCmsgAuthSession) {
                const bool wotlk = (iter % 2 == 1);
                if (!wotlk) {
                    VanillaCrypt serverCrypt = makeTbcServerCrypt(sessionKey);
                    auto p1 = buildTbcEncryptedSmsg(serverCrypt, kSmsgAddonInfo, {1, 2, 3});
                    auto p2 = buildTbcEncryptedSmsg(serverCrypt, kSmsgAuthResponse, {0x0C, static_cast<uint8_t>(iter)});
                    sendAllBlocking(clientFd, p1.data(), p1.size());
                    sendAllBlocking(clientFd, p2.data(), p2.size());
                } else {
                    RC4 serverCipher = makeWotlkServerSendCipher(sessionKey);
                    auto p1 = buildWotlkEncryptedSmsg(serverCipher, kSmsgAddonInfo, {4, 5, 6});
                    auto p2 = buildWotlkEncryptedSmsg(serverCipher, kSmsgAuthResponse, {0x0C, static_cast<uint8_t>(iter)});
                    sendAllBlocking(clientFd, p1.data(), p1.size());
                    sendAllBlocking(clientFd, p2.data(), p2.size());
                }
            }

            uint8_t drain[16];
            while (wowee::net::portableRecv(clientFd, drain, sizeof(drain)) > 0) {}
            wowee::net::closeSocket(clientFd);
        }
    });

    // Reuse a single WorldSocket instance across all 50 connects to also verify
    // connect()/disconnect() state reset hygiene.
    WorldSocket ws;
    std::mutex rxMutex;
    std::vector<Packet> received;
    ws.setPacketCallback([&](const Packet& pkt) {
        std::lock_guard<std::mutex> lock(rxMutex);
        received.push_back(pkt);
    });

    for (int iter = 0; iter < kIterations; ++iter) {
        {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.clear();
        }
        REQUIRE(ws.connect("127.0.0.1", listener.port));
        REQUIRE(pumpClientUntil(ws, [&]() {
            std::lock_guard<std::mutex> lock(rxMutex);
            return !received.empty();
        }, std::chrono::milliseconds(1000)));
        {
            std::lock_guard<std::mutex> lock(rxMutex);
            REQUIRE(received.size() == 1);
            CHECK(received[0].getOpcode() == kSmsgAuthChallenge);
            received.clear();
        }

        ws.send(Packet(kCmsgAuthSession, {1, 2, 3, 4}));
        if (iter % 3 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        ws.initEncryption(sessionKey, (iter % 2 == 1) ? 12340u : 8606u);

        REQUIRE(pumpClientUntil(ws, [&]() {
            std::lock_guard<std::mutex> lock(rxMutex);
            return received.size() >= 2;
        }, std::chrono::milliseconds(1000)));

        {
            std::lock_guard<std::mutex> lock(rxMutex);
            REQUIRE(received.size() == 2);
            CHECK(received[0].getOpcode() == kSmsgAddonInfo);
            CHECK(received[1].getOpcode() == kSmsgAuthResponse);
            REQUIRE(received[1].getData().size() == 2);
            CHECK(received[1].getData()[1] == static_cast<uint8_t>(iter));
        }
        CHECK(ws.isConnected());
        ws.disconnect();
    }

    serverThread.join();
}
