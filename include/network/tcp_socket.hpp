#pragma once

#include "network/socket.hpp"
#include "network/net_platform.hpp"

namespace wowee {
namespace network {

class TCPSocket : public Socket {
public:
    TCPSocket();
    ~TCPSocket() override;

    bool connect(const std::string& host, uint16_t port) override;
    void disconnect() override;
    [[nodiscard]] bool isConnected() const override { return connected; }

    void send(const Packet& packet) override;
    void update() override;

    /**
     * Configure client build/version metadata announced in AUTH_LOGON_CHALLENGE
     * so variable-layout auth server responses (such as CMD_AUTH_LOGON_PROOF
     * 26/28/32-byte success frames and 2/4-byte failure frames) are framed to
     * the exact protocol length regardless of TCP segmentation.
     */
    void setClientBuild(uint16_t build, uint8_t protocolVersion = 8, uint8_t majorVersion = 0);
    [[nodiscard]] uint16_t getClientBuild() const noexcept { return clientBuild_; }
    [[nodiscard]] uint8_t getProtocolVersion() const noexcept { return protocolVersion_; }
    [[nodiscard]] uint8_t getMajorVersion() const noexcept { return majorVersion_; }

    /**
     * Wire byte size of a successful CMD_AUTH_LOGON_PROOF (0x01) response:
     * - Vanilla 1.x (build < 6299, or custom 1.x with majorVersion == 1): 26 bytes
     *   cmd(1) + error(1) + M2(20) + surveyId/loginFlags(4)
     * - TBC 2.0.3..2.3.3 (6299 <= build < 8089): 28 bytes
     *   cmd(1) + error(1) + M2(20) + surveyId(4) + loginFlags(2)
     * - TBC 2.4.0+ / WotLK 3.x (build >= 8089): 32 bytes
     *   cmd(1) + error(1) + M2(20) + accountFlags(4) + surveyId(4) + unkFlags(2)
     */
    [[nodiscard]] static constexpr size_t expectedLogonProofSuccessSize(
        uint16_t build, uint8_t majorVersion = 0) noexcept {
        if (majorVersion == 1 || build < 6299) {
            return 26;
        }
        if (build < 8089) {
            return 28;
        }
        return 32;
    }

private:
    void tryParsePackets(bool peerClosed = false);
    size_t getExpectedPacketSize(uint8_t opcode, bool peerClosed, bool& unknownOpcodeOut);

    socket_t sockfd = INVALID_SOCK;
    bool connected = false;
    uint16_t clientBuild_ = 12340;
    uint8_t protocolVersion_ = 8;
    uint8_t majorVersion_ = 0;
    std::vector<uint8_t> receiveBuffer;
};

} // namespace network
} // namespace wowee
