#include "network/tcp_socket.hpp"
#include "network/packet.hpp"
#include "network/net_platform.hpp"
#include "core/logger.hpp"

#include <chrono>
#include <thread>

namespace wowee {
namespace network {

TCPSocket::TCPSocket() {
    net::ensureInit();
}

TCPSocket::~TCPSocket() {
    TCPSocket::disconnect();  // qualified call: virtual dispatch is bypassed in destructors
}

void TCPSocket::setClientBuild(uint16_t build, uint8_t protocolVersion, uint8_t majorVersion) {
    if (build != 0) {
        clientBuild_ = build;
    }
    protocolVersion_ = protocolVersion;
    majorVersion_ = majorVersion;
}

bool TCPSocket::connect(const std::string& host, uint16_t port) {
    LOG_INFO("Connecting to ", host, ":", port);

    if (sockfd != INVALID_SOCK || connected || !receiveBuffer.empty()) {
        disconnect();
    }

    // Socket open, non-blocking, and the address resolved.
    struct sockaddr_in serverAddr;
    sockfd = net::openResolvedSocket(host, port, serverAddr);
    if (sockfd == INVALID_SOCK) return false;

    int result = ::connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    if (result < 0) {
        int err = net::lastError();
        if (!net::isInProgress(err)) {
            LOG_ERROR("Failed to connect: ", net::errorString(err));
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }

        // Non-blocking connect in progress - wait for it to complete
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sockfd, &writefds);

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int selectResult = ::select(static_cast<int>(sockfd) + 1, nullptr, &writefds, nullptr, &tv);
        if (selectResult <= 0) {
            LOG_ERROR("Connection timed out to ", host, ":", port);
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }

        // Check if the connection actually succeeded
        int sockErr = 0;
        socklen_t errLen = sizeof(sockErr);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockErr), &errLen);
        if (sockErr != 0) {
            LOG_ERROR("Connection failed: ", net::errorString(sockErr));
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }
    }

    // Disable Nagle's algorithm - send small packets immediately.
    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));

    connected = true;
    LOG_INFO("Connected to ", host, ":", port);
    return true;
}

void TCPSocket::disconnect() {
    if (sockfd != INVALID_SOCK) {
        net::closeSocket(sockfd);
        sockfd = INVALID_SOCK;
    }
    connected = false;
    receiveBuffer.clear();
}

void TCPSocket::send(const Packet& packet) {
    if (!connected || sockfd == INVALID_SOCK) return;

    const auto& data = packet.getData();
    const uint8_t wireOpcode = static_cast<uint8_t>(packet.getOpcode() & 0xFF);

    // Automatically track the client build announced in CMD_AUTH_LOGON_CHALLENGE (0x00)
    // or CMD_AUTH_RECONNECT_CHALLENGE (0x02):
    //   data[0]     = protocolVersion
    //   data[1..2]  = payloadSize (uint16 LE)
    //   data[3..6]  = game FourCC
    //   data[7..9]  = major, minor, patch
    //   data[10..11]= build (uint16 LE)
    if ((wireOpcode == 0x00 || wireOpcode == 0x02) && data.size() >= 12) {
        const uint16_t announcedBuild =
            static_cast<uint16_t>(uint16_t(data[10]) | (uint16_t(data[11]) << 8));
        if (announcedBuild != 0) {
            clientBuild_ = announcedBuild;
            protocolVersion_ = data[0];
            majorVersion_ = data[7];
        }
    }

    // Build complete packet with 1-byte opcode prefix
    std::vector<uint8_t> sendData;
    sendData.reserve(1 + data.size());
    sendData.push_back(wireOpcode);
    sendData.insert(sendData.end(), data.begin(), data.end());

    LOG_DEBUG("Sending packet: opcode=0x", std::hex, packet.getOpcode(), std::dec,
              " size=", sendData.size(), " bytes");

    size_t totalSent = 0;
    while (totalSent < sendData.size()) {
        ssize_t sent = net::portableSend(sockfd, sendData.data() + totalSent,
                                         sendData.size() - totalSent);
        if (sent < 0) {
            int err = net::lastError();
            if (net::isWouldBlock(err)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            LOG_ERROR("Send failed: ", net::errorString(err));
            break;
        }
        if (sent == 0) break;
        totalSent += static_cast<size_t>(sent);
    }
    if (totalSent != sendData.size()) {
        LOG_WARNING("Partial send: ", totalSent, " of ", sendData.size(), " bytes");
    }
}

void TCPSocket::update() {
    if (!connected) return;

    // Drain the socket. Some servers send small packets and close immediately; a single recv()
    // can return a partial packet, and the next recv() may return 0 (FIN) which would otherwise
    // make us drop the buffered bytes without parsing.
    bool sawClose = false;
    bool receivedAny = false;
    for (;;) {
        uint8_t buffer[4096];
        ssize_t received = net::portableRecv(sockfd, buffer, sizeof(buffer));

        if (received > 0) {
            receivedAny = true;
            LOG_DEBUG("Received ", received, " bytes from server");
            receiveBuffer.insert(receiveBuffer.end(), buffer, buffer + received);
            continue; // keep draining
        }

        if (received == 0) {
            sawClose = true;
            break;
        }

        int err = net::lastError();
        if (net::isWouldBlock(err)) {
            break;
        }
        if (net::isConnectionClosed(err)) {
            // Peer closed the connection - treat the same as recv() returning 0
            sawClose = true;
            break;
        }

        LOG_ERROR("Receive failed: ", net::errorString(err));
        disconnect();
        return;
    }

    if (receivedAny || (sawClose && !receiveBuffer.empty())) {
        tryParsePackets(sawClose);
    }

    if (sawClose && connected) {
        LOG_INFO("Connection closed by server");
        disconnect();
    }
}

void TCPSocket::tryParsePackets(bool peerClosed) {
    while (connected && !receiveBuffer.empty()) {
        const uint8_t opcode = receiveBuffer[0];

        bool unknownOpcode = false;
        const size_t expectedSize = getExpectedPacketSize(opcode, peerClosed, unknownOpcode);

        if (unknownOpcode) {
            LOG_ERROR("Unknown auth packet opcode: 0x", std::hex, static_cast<int>(opcode),
                      std::dec, " (buffered=", receiveBuffer.size(),
                      " bytes). Disconnecting to recover stream.");
            disconnect();
            return;
        }

        if (expectedSize == 0 || receiveBuffer.size() < expectedSize) {
            LOG_DEBUG("Waiting for more auth data: opcode=0x", std::hex, static_cast<int>(opcode),
                      std::dec, " have=", receiveBuffer.size(),
                      " expected=", expectedSize);
            break;
        }

        LOG_DEBUG("Parsing packet: opcode=0x", std::hex, static_cast<int>(opcode), std::dec,
                  " size=", expectedSize, " bytes");

        std::vector<uint8_t> packetData(receiveBuffer.begin(),
                                        receiveBuffer.begin() + expectedSize);
        Packet packet(opcode, std::move(packetData));

        receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + expectedSize);

        if (packetCallback) {
            packetCallback(packet);
        }
    }
}

size_t TCPSocket::getExpectedPacketSize(uint8_t opcode, bool peerClosed, bool& unknownOpcodeOut) {
    unknownOpcodeOut = false;

    switch (opcode) {
        case 0x00:  // CMD_AUTH_LOGON_CHALLENGE response
            if (receiveBuffer.size() >= 3) {
                const uint8_t status = receiveBuffer[2];
                if (status == 0x00) {
                    // Success: opcode(1) + unk(1) + status(1) + B(32) + gLen(1) + g(gLen) +
                    //          nLen(1) + N(nLen) + salt(32) + crcHash(16) + securityFlags(1)
                    //          + optional security flag data
                    if (receiveBuffer.size() >= 36) {
                        const uint8_t gLen = receiveBuffer[35];
                        const size_t nLenOffset = 36 + static_cast<size_t>(gLen);
                        if (receiveBuffer.size() >= nLenOffset + 1) {
                            const uint8_t nLen = receiveBuffer[nLenOffset];
                            const size_t baseSize = nLenOffset + 1 + static_cast<size_t>(nLen) + 32 + 16 + 1;
                            if (receiveBuffer.size() >= baseSize) {
                                const uint8_t secFlags = receiveBuffer[baseSize - 1];
                                size_t extra = 0;
                                if (secFlags & 0x01) extra += 20;  // PIN: seed(4) + salt(16)
                                if (secFlags & 0x02) extra += 12;  // Matrix: w(1)+h(1)+digits(1)+challenges(1)+seed(8)
                                if (secFlags & 0x04) extra += 1;   // Authenticator: required(1)
                                return baseSize + extra;
                            }
                        }
                    }
                    return 0;  // Need more data
                }
                // Failure: opcode(1) + unknown(1) + status(1)
                return 3;
            }
            return 0;

        case 0x01:  // CMD_AUTH_LOGON_PROOF response
            if (receiveBuffer.size() >= 2) {
                const uint8_t status = receiveBuffer[1];
                // AUTH_LOGON_SUCCESS (0x00) and AUTH_LOGON_SUCCESS_SURVEY (0x0E)
                // carry the full build-determined server proof frame:
                //   Build >= 8089:          cmd(1)+error(1)+M2(20)+accountFlags(4)+surveyId(4)+unkFlags(2) = 32
                //   Build 6299..8088 (2.x): cmd(1)+error(1)+M2(20)+surveyId(4)+loginFlags(2) = 28
                //   Build < 6299 (or 1.x):  cmd(1)+error(1)+M2(20)+surveyId(4) = 26
                if (status == 0x00 || status == 0x0E) {
                    return expectedLogonProofSuccessSize(clientBuild_, majorVersion_);
                }

                // Failure response:
                // - CMaNGOS sends 2 bytes {0x01, 0x09} on AUTH_LOGON_FAILED_VERSION_INVALID
                //   for all builds without closing the socket immediately.
                // - Vanilla <= 6005 sends 2 bytes {0x01, status} on wrong password
                //   (or 4 bytes {0x01, 0x04, 0x00/0x03, 0x00} on PIN failure).
                // - Builds > 6005 send 4 bytes {0x01, status, unkFlagsLo, unkFlagsHi}
                //   and keep the socket open for retry.
                const bool isVanilla112 = (clientBuild_ <= 6005 || majorVersion_ == 1);
                if (status == 0x09 || isVanilla112) {
                    if (receiveBuffer.size() >= 4 &&
                        (receiveBuffer[2] == 0x00 || receiveBuffer[2] == 0x03) &&
                        receiveBuffer[3] == 0x00) {
                        return 4;
                    }
                    return 2;
                }

                if (receiveBuffer.size() >= 4) {
                    return 4;
                }
                return peerClosed ? 2 : 0;
            }
            return 0;

        case 0x02:  // CMD_AUTH_RECONNECT_CHALLENGE response
            if (receiveBuffer.size() >= 2) {
                const uint8_t status = receiveBuffer[1];
                // Success: cmd(1) + error(1) + reconnectProof(16) + versionChallenge(16) = 34
                return (status == 0x00) ? 34 : 2;
            }
            return 0;

        case 0x03:  // CMD_AUTH_RECONNECT_PROOF response
            if (receiveBuffer.size() >= 2) {
                const uint8_t status = receiveBuffer[1];
                if (status != 0x00 || majorVersion_ == 1 || clientBuild_ < 6299) {
                    return 2;
                }
                if (receiveBuffer.size() >= 4) {
                    return 4;
                }
                return peerClosed ? 2 : 0;
            }
            return 0;

        case 0x10:  // CMD_REALM_LIST response
            // Format: opcode(1) + size(2, LE) + payload(size)
            if (receiveBuffer.size() >= 3) {
                const uint16_t size =
                    static_cast<uint16_t>(uint16_t(receiveBuffer[1]) | (uint16_t(receiveBuffer[2]) << 8));
                return 3 + static_cast<size_t>(size);
            }
            return 0;

        default:
            unknownOpcodeOut = true;
            return 0;
    }
}

} // namespace network
} // namespace wowee
