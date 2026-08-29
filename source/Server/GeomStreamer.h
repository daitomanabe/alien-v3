#pragma once

// Streams a downsampled snapshot of the render geometry as UDP packets for a
// remote renderer. Wire format (little endian, packed):
//
//   packet header:  magic  u32 = 0x414C4E33 ("ALN3")
//                   frameId u32
//                   chunkIdx u16
//                   numChunks u16
//                   payloadBytes u16
//                   pad u16
//   payload:        repeated 12-byte points:
//                   x f32, y f32, r u8, g u8, b u8, flags u8
//                   flags bit0..2 = cell type bits (from ObjectVertexData state)
//                   flags bit6    = isolated
//                   flags bit7    = fluid particle
//
// Chunks stay under the usual 1500-byte MTU so the WAN path does not have to
// reassemble IP fragments.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <EngineInterface/HostRenderData.h>

#include "OscSender.h"

class GeomStreamer
{
public:
    GeomStreamer(int listenPort, std::string const& initialHost, int initialPort, int maxCells, int maxFluid)
        : _sender(listenPort, initialHost, initialPort)
        , _maxCells(maxCells)
        , _maxFluid(maxFluid)
    {}

    UdpChannel& channel() { return _sender; }

    void sendFrame(HostRenderData const& renderData)
    {
        if (!_sender.hasTarget()) {
            return;
        }
        _points.clear();
        samplePoints(renderData);

        auto pointBytes = _points.size() * PointSize;
        auto numChunks = std::max<size_t>(1, (pointBytes + MaxPayload - 1) / MaxPayload);

        std::vector<uint8_t> packet;
        size_t offset = 0;
        for (size_t chunk = 0; chunk < numChunks; ++chunk) {
            auto remaining = pointBytes - offset;
            auto payload = std::min<size_t>(remaining, MaxPayload);

            packet.clear();
            appendU32(packet, Magic);
            appendU32(packet, _frameId);
            appendU16(packet, static_cast<uint16_t>(chunk));
            appendU16(packet, static_cast<uint16_t>(numChunks));
            appendU16(packet, static_cast<uint16_t>(payload));
            appendU16(packet, 0);
            packet.insert(packet.end(), _points.data() + offset, _points.data() + offset + payload);
            _sender.send(packet);
            offset += payload;
        }
        ++_frameId;
    }

private:
    static constexpr uint32_t Magic = 0x414C4E33;
    static constexpr size_t PointSize = 12;
    static constexpr size_t MaxPayload = 1416;  // 118 points per packet, header included stays < 1440 bytes

    void samplePoints(HostRenderData const& renderData)
    {
        auto sampleObjects = [this](auto const& items, size_t maxCount, bool fluid) {
            if (items.empty() || maxCount == 0) {
                return;
            }
            auto stride = std::max<size_t>(1, items.size() / maxCount);
            for (size_t i = _frameId % stride; i < items.size(); i += stride) {
                auto const& item = items[i];
                uint8_t flags = fluid ? 0x80 : 0;
                if constexpr (requires { item.state; }) {
                    flags |= static_cast<uint8_t>(item.state & 0x07);
                    if (item.state & (1 << 16)) {
                        flags |= 0x40;
                    }
                }
                appendPoint(item.pos[0], item.pos[1], item.color, flags);
            }
        };
        sampleObjects(renderData.cells, _maxCells, false);
        sampleObjects(renderData.fluidParticles, _maxFluid, true);
    }

    void appendPoint(float x, float y, float const color[3], uint8_t flags)
    {
        auto toByte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        uint8_t buffer[PointSize];
        std::memcpy(buffer, &x, 4);
        std::memcpy(buffer + 4, &y, 4);
        buffer[8] = toByte(color[0]);
        buffer[9] = toByte(color[1]);
        buffer[10] = toByte(color[2]);
        buffer[11] = flags;
        _points.insert(_points.end(), buffer, buffer + PointSize);
    }

    static void appendU32(std::vector<uint8_t>& buffer, uint32_t value)
    {
        auto* bytes = reinterpret_cast<uint8_t const*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + 4);
    }

    static void appendU16(std::vector<uint8_t>& buffer, uint16_t value)
    {
        auto* bytes = reinterpret_cast<uint8_t const*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + 2);
    }

    UdpChannel _sender;
    int _maxCells = 0;
    int _maxFluid = 0;
    uint32_t _frameId = 0;
    std::vector<uint8_t> _points;
};
