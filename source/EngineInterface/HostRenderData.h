#pragma once

#include <cstdint>
#include <vector>

#include "GeometryBuffers.h"

// Render geometry copied to host memory for headless consumers (network
// streaming, sonification). Holds the subset of the render buffers that has
// semantic value outside the OpenGL pipeline.
struct HostRenderData
{
    uint64_t timestep = 0;
    std::vector<ObjectVertexData> cells;
    std::vector<FluidParticleVertexData> fluidParticles;
    std::vector<unsigned int> lineIndices;  // index pairs into cells (cell-cell connections)
    std::vector<AttackEventVertexData> attackEvents;
    std::vector<DetonationEventVertexData> detonationEvents;
};
