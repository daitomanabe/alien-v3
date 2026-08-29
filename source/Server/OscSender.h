#pragma once

// Minimal OSC 1.0 encoder and UDP sender: int32/float32/string arguments and
// bundles of messages. Just enough to talk to SuperCollider (sclang/scsynth).

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
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

class UdpSender
{
public:
    UdpSender(std::string const& host, int port)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* info = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &info) != 0 || !info) {
            throw std::runtime_error("Cannot resolve host: " + host);
        }
        std::memcpy(&_target, info->ai_addr, info->ai_addrlen);
        _targetLen = info->ai_addrlen;
        freeaddrinfo(info);

        _socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (_socket < 0) {
            throw std::runtime_error("Cannot create UDP socket");
        }
    }

    ~UdpSender()
    {
        if (_socket >= 0) {
            close(_socket);
        }
    }

    UdpSender(UdpSender const&) = delete;
    UdpSender& operator=(UdpSender const&) = delete;

    void send(std::vector<uint8_t> const& data) const { sendto(_socket, data.data(), data.size(), 0, reinterpret_cast<sockaddr const*>(&_target), _targetLen); }

private:
    int _socket = -1;
    sockaddr_storage _target{};
    socklen_t _targetLen = 0;
};
