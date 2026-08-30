#pragma once

// Streams a downsampled snapshot of the render geometry as UDP packets for a
// remote renderer. Wire format (little endian, packed):
//
//   packet header:  magic  u32 = 0x414C4E33 ("ALN3")
//                   frameId u32
//                   chunkIdx u16
//                   numChunks u16
//                   payloadBytes u16
//                   type u16          0 = points, 1 = lines
//
//   type 0 payload: repeated 12-byte points:
//                   x f32, y f32, r u8, g u8, b u8, flags u8
//                   flags bit0..2 = cell type bits (from ObjectVertexData state)
//                   flags bit6    = attack event marker
//                   flags bit7    = fluid particle
//
//   type 1 payload: repeated 20-byte line segments (cell-cell connections):
//                   x1 f32, y1 f32, x2 f32, y2 f32, r u8, g u8, b u8, pad u8
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
    GeomStreamer(int listenPort, std::string const& initialHost, int initialPort, int maxCells, int maxFluid, int maxLines)
        : _sender(listenPort, initialHost, initialPort)
        , _maxCells(maxCells)
        , _maxFluid(maxFluid)
        , _maxLines(maxLines)
    {}

    UdpChannel& channel() { return _sender; }

    size_t lastPointCount() const { return _lastPointCount; }
    size_t lastLineCount() const { return _lastLineCount; }

    void sendFrame(HostRenderData const& renderData)
    {
        if (!_sender.hasTarget()) {
            return;
        }

        _points.clear();
        samplePoints(renderData);
        _lastPointCount = _points.size() / PointSize;
        sendChunked(TypePoints, _points);

        _lines.clear();
        sampleLines(renderData);
        _lastLineCount = _lines.size() / LineSize;
        if (!_lines.empty()) {
            sendChunked(TypeLines, _lines);
        }

        ++_frameId;
    }

private:
    static constexpr uint32_t Magic = 0x414C4E33;
    static constexpr uint16_t TypePoints = 0;
    static constexpr uint16_t TypeLines = 1;
    static constexpr size_t PointSize = 12;
    static constexpr size_t LineSize = 20;
    static constexpr size_t MaxPayload = 1400 - (1400 % (PointSize * LineSize / 4));  // 1380: divisible by both 12 and 20

    void sendChunked(uint16_t type, std::vector<uint8_t> const& payload)
    {
        auto totalBytes = payload.size();
        auto numChunks = std::max<size_t>(1, (totalBytes + MaxPayload - 1) / MaxPayload);

        std::vector<uint8_t> packet;
        size_t offset = 0;
        for (size_t chunk = 0; chunk < numChunks; ++chunk) {
            auto remaining = totalBytes - offset;
            auto chunkBytes = std::min<size_t>(remaining, MaxPayload);

            packet.clear();
            appendU32(packet, Magic);
            appendU32(packet, _frameId);
            appendU16(packet, static_cast<uint16_t>(chunk));
            appendU16(packet, static_cast<uint16_t>(numChunks));
            appendU16(packet, static_cast<uint16_t>(chunkBytes));
            appendU16(packet, type);
            packet.insert(packet.end(), payload.data() + offset, payload.data() + offset + chunkBytes);
            _sender.send(packet);
            offset += chunkBytes;
        }
    }

    void samplePoints(HostRenderData const& renderData)
    {
        auto sampleObjects = [this](auto const& items, size_t maxCount, uint8_t baseFlags) {
            if (items.empty() || maxCount == 0) {
                return;
            }
            auto stride = std::max<size_t>(1, items.size() / maxCount);
            for (size_t i = _frameId % stride; i < items.size(); i += stride) {
                auto const& item = items[i];
                uint8_t flags = baseFlags;
                if constexpr (requires { item.state; }) {
                    flags |= static_cast<uint8_t>(item.state & 0x07);
                }
                appendPoint(item.pos[0], item.pos[1], item.color, flags);
            }
        };
        sampleObjects(renderData.cells, _maxCells, 0);
        sampleObjects(renderData.fluidParticles, _maxFluid, 0x80);

        // attack events are few and sonically/visually important: send them all
        float const attackColor[3] = {1.0f, 0.25f, 0.15f};
        for (auto const& attack : renderData.attackEvents) {
            appendPoint(attack.pos[0], attack.pos[1], attackColor, 0x40);
        }
    }

    void sampleLines(HostRenderData const& renderData)
    {
        auto numSegments = renderData.lineIndices.size() / 2;
        if (numSegments == 0 || _maxLines <= 0) {
            return;
        }
        auto stride = std::max<size_t>(1, numSegments / _maxLines);
        auto numCells = renderData.cells.size();
        for (size_t seg = _frameId % stride; seg < numSegments; seg += stride) {
            auto ia = renderData.lineIndices[seg * 2];
            auto ib = renderData.lineIndices[seg * 2 + 1];
            if (ia >= numCells || ib >= numCells) {
                continue;
            }
            auto const& a = renderData.cells[ia];
            auto const& b = renderData.cells[ib];

            auto toByte = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
            uint8_t buffer[LineSize];
            std::memcpy(buffer, &a.pos[0], 4);
            std::memcpy(buffer + 4, &a.pos[1], 4);
            std::memcpy(buffer + 8, &b.pos[0], 4);
            std::memcpy(buffer + 12, &b.pos[1], 4);
            buffer[16] = toByte(a.color[0]);
            buffer[17] = toByte(a.color[1]);
            buffer[18] = toByte(a.color[2]);
            buffer[19] = 0;
            _lines.insert(_lines.end(), buffer, buffer + LineSize);
        }
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
    int _maxLines = 0;
    uint32_t _frameId = 0;
    size_t _lastPointCount = 0;
    size_t _lastLineCount = 0;
    std::vector<uint8_t> _points;
    std::vector<uint8_t> _lines;
};
