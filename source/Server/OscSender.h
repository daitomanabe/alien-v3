#pragma once

// Minimal OSC 1.0 encoder and UDP sender: int32/float32/string arguments and
// bundles of messages. Just enough to talk to SuperCollider (sclang/scsynth).

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

// UDP channel that learns its destination from inbound "hello" datagrams.
// Reason: on this deployment the receiver (Mac) can reach the GPU host through
// a shared tailnet node, but flows initiated by the GPU host are dropped.
// Replies on a flow the receiver opened do pass, so the receiver subscribes by
// sending any datagram to our listen port and we stream back to its source
// address. An optional initial host:port supports plain push (e.g. localhost).
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
            std::memcpy(&_target, info->ai_addr, info->ai_addrlen);
            _targetLen = info->ai_addrlen;
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

    // Drain inbound datagrams; the most recent sender becomes the target.
    // Returns true if a new subscriber address was learned.
    bool poll()
    {
        auto newSubscriber = false;
        uint8_t buffer[512];
        sockaddr_storage from{};
        socklen_t fromLen = sizeof(from);
        while (recvfrom(_socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen) > 0) {
            if (fromLen != _targetLen || std::memcmp(&from, &_target, fromLen) != 0) {
                std::memcpy(&_target, &from, fromLen);
                _targetLen = fromLen;
                newSubscriber = true;
            }
            fromLen = sizeof(from);
        }
        return newSubscriber;
    }

    bool hasTarget() const { return _targetLen > 0; }

    std::string targetString() const
    {
        if (!hasTarget()) {
            return "(none)";
        }
        auto const& addr = reinterpret_cast<sockaddr_in const&>(_target);
        char host[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
        return std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    void send(std::vector<uint8_t> const& data) const
    {
        if (hasTarget()) {
            sendto(_socket, data.data(), data.size(), 0, reinterpret_cast<sockaddr const*>(&_target), _targetLen);
        }
    }

private:
    int _socket = -1;
    sockaddr_storage _target{};
    socklen_t _targetLen = 0;
};
