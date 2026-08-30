#pragma once

// Minimal OSC 1.0 encoder and UDP sender: int32/float32/string arguments and
// bundles of messages. Just enough to talk to SuperCollider (sclang/scsynth).

#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

class OscMessage
{
public:
    explicit OscMessage(std::string address)
        : _address(std::move(address))
    {}

    OscMessage& addInt(int32_t value)
    {
        _typeTags += 'i';
        appendBigEndian(_args, static_cast<uint32_t>(value));
        return *this;
    }

    OscMessage& addFloat(float value)
    {
        _typeTags += 'f';
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        appendBigEndian(_args, bits);
        return *this;
    }

    OscMessage& addString(std::string const& value)
    {
        _typeTags += 's';
        appendPaddedString(_args, value);
        return *this;
    }

    std::vector<uint8_t> encode() const
    {
        std::vector<uint8_t> result;
        appendPaddedString(result, _address);
        appendPaddedString(result, _typeTags);
        result.insert(result.end(), _args.begin(), _args.end());
        return result;
    }

private:
    friend class OscBundle;

    static void appendBigEndian(std::vector<uint8_t>& buffer, uint32_t value)
    {
        buffer.push_back(static_cast<uint8_t>(value >> 24));
        buffer.push_back(static_cast<uint8_t>(value >> 16));
        buffer.push_back(static_cast<uint8_t>(value >> 8));
        buffer.push_back(static_cast<uint8_t>(value));
    }

    static void appendPaddedString(std::vector<uint8_t>& buffer, std::string const& value)
    {
        buffer.insert(buffer.end(), value.begin(), value.end());
        buffer.push_back(0);
        while (buffer.size() % 4 != 0) {
            buffer.push_back(0);
        }
    }

    std::string _address;
    std::string _typeTags = ",";
    std::vector<uint8_t> _args;
};

class OscBundle
{
public:
    void add(OscMessage const& message) { _encodedMessages.push_back(message.encode()); }

    bool empty() const { return _encodedMessages.empty(); }

    size_t size() const { return _encodedMessages.size(); }

    std::vector<uint8_t> encode() const
    {
        std::vector<uint8_t> result;
        OscMessage::appendPaddedString(result, "#bundle");
        // OSC time tag "immediately"
        OscMessage::appendBigEndian(result, 0);
        OscMessage::appendBigEndian(result, 1);
        for (auto const& encoded : _encodedMessages) {
            OscMessage::appendBigEndian(result, static_cast<uint32_t>(encoded.size()));
            result.insert(result.end(), encoded.begin(), encoded.end());
        }
        return result;
    }

private:
    std::vector<std::vector<uint8_t>> _encodedMessages;
};

// UDP channel that learns its destinations from inbound "hello" datagrams.
// Reason: on this deployment the receivers (Mac) can reach the GPU host
// through a shared tailnet node, but flows initiated by the GPU host are
// dropped. Replies on a flow the receiver opened do pass, so each receiver
// subscribes by sending any datagram to our listen port and we stream back to
// its source address. Multiple subscribers (e.g. SuperCollider + a parameter
// monitor) are kept alive by their periodic hellos and expire after 30 s.
// Inbound datagrams that carry an OSC address are also surfaced as commands.
class UdpChannel
{
public:
    UdpChannel(int listenPort, std::string const& initialHost = {}, int initialPort = 0)
    {
        _socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (_socket < 0) {
            throw std::runtime_error("Cannot create UDP socket");
        }

        if (listenPort > 0) {
            sockaddr_in bindAddr{};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_addr.s_addr = INADDR_ANY;
            bindAddr.sin_port = htons(static_cast<uint16_t>(listenPort));
            if (bind(_socket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
                close(_socket);
                throw std::runtime_error("Cannot bind UDP port " + std::to_string(listenPort));
            }
        }

        auto flags = fcntl(_socket, F_GETFL, 0);
        fcntl(_socket, F_SETFL, flags | O_NONBLOCK);

        if (!initialHost.empty() && initialPort > 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* info = nullptr;
            if (getaddrinfo(initialHost.c_str(), std::to_string(initialPort).c_str(), &hints, &info) != 0 || !info) {
                close(_socket);
                throw std::runtime_error("Cannot resolve host: " + initialHost);
            }
            Subscriber sub{};
            std::memcpy(&sub.addr, info->ai_addr, info->ai_addrlen);
            sub.addrLen = info->ai_addrlen;
            sub.permanent = true;
            _subscribers.push_back(sub);
            freeaddrinfo(info);
        }
    }

    ~UdpChannel()
    {
        if (_socket >= 0) {
            close(_socket);
        }
    }

    UdpChannel(UdpChannel const&) = delete;
    UdpChannel& operator=(UdpChannel const&) = delete;

    struct PollResult
    {
        bool subscribersChanged = false;
        std::vector<std::string> commands;  // OSC addresses of inbound datagrams (e.g. "/alien/cataclysm")
    };

    // Drain inbound datagrams: refresh/add subscribers, expire stale ones,
    // collect OSC-style command addresses.
    PollResult poll()
    {
        PollResult result;
        uint8_t buffer[512];
        sockaddr_storage from{};
        socklen_t fromLen = sizeof(from);
        auto now = std::chrono::steady_clock::now();

        ssize_t received;
        while ((received = recvfrom(_socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen)) > 0) {
            // A datagram carrying a command address is a one-shot control
            // message, not a subscription request.
            auto isCommand = false;
            if (received > 1 && buffer[0] == '/') {
                auto* end = static_cast<uint8_t const*>(std::memchr(buffer, 0, received));
                auto address = std::string(reinterpret_cast<char const*>(buffer), end ? end - buffer : received);
                if (address != "/alien/subscribe") {
                    result.commands.push_back(address);
                    isCommand = true;
                }
            }

            if (!isCommand) {
                auto known = false;
                for (auto& sub : _subscribers) {
                    if (sub.addrLen == fromLen && std::memcmp(&sub.addr, &from, fromLen) == 0) {
                        sub.lastSeen = now;
                        known = true;
                        break;
                    }
                }
                if (!known && _subscribers.size() < MaxSubscribers) {
                    Subscriber sub{};
                    std::memcpy(&sub.addr, &from, fromLen);
                    sub.addrLen = fromLen;
                    sub.lastSeen = now;
                    _subscribers.push_back(sub);
                    result.subscribersChanged = true;
                }
            }
            fromLen = sizeof(from);
        }

        auto before = _subscribers.size();
        std::erase_if(_subscribers, [&](Subscriber const& sub) { return !sub.permanent && now - sub.lastSeen > SubscriberTimeout; });
        if (_subscribers.size() != before) {
            result.subscribersChanged = true;
        }
        return result;
    }

    bool hasTarget() const { return !_subscribers.empty(); }

    std::string targetString() const
    {
        if (_subscribers.empty()) {
            return "(none)";
        }
        std::string result;
        for (auto const& sub : _subscribers) {
            auto const& addr = reinterpret_cast<sockaddr_in const&>(sub.addr);
            char host[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
            if (!result.empty()) {
                result += ", ";
            }
            result += std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
        }
        return result;
    }

    void send(std::vector<uint8_t> const& data) const
    {
        for (auto const& sub : _subscribers) {
            sendto(_socket, data.data(), data.size(), 0, reinterpret_cast<sockaddr const*>(&sub.addr), sub.addrLen);
        }
    }

private:
    static constexpr size_t MaxSubscribers = 8;
    static constexpr std::chrono::seconds SubscriberTimeout{30};

    struct Subscriber
    {
        sockaddr_storage addr{};
        socklen_t addrLen = 0;
        std::chrono::steady_clock::time_point lastSeen{};
        bool permanent = false;
    };

    int _socket = -1;
    std::vector<Subscriber> _subscribers;
};
