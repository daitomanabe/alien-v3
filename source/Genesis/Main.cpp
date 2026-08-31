// alien_genesis — build and dissect garden scenes from the command line.
//
//   alien_genesis dump -i scene.sim -o outdir
//       Writes parameters.json, summary.txt, and for the most populous
//       genomes a seed library: creature-<k>.content + genome-<k>.genome.
//
//   alien_genesis new -o garden.sim --params parameters.json --world 4000x1200 \
//       --seed outdir/creature-0.content [--seed ...] --seeds 8 \
//       --shelves 4 --amplitude 50 --wavelength 900 --tendrils 14 --energy 3000
//       Procedural hanging-garden skeleton: sine-wave static shelves with
//       hanging tendrils, seeded creatures (one lineage and color per plant),
//       scattered energy particles.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <unordered_set>

#include <CLI/CLI.hpp>

#include <Base/AlienExceptions.h>
#include <Base/FileLogger.h>
#include <Base/LoggingService.h>
#include <Base/Resources.h>

#include <EngineInterface/DescEditService.h>
#include <EngineInterface/Descs.h>
#include <EngineInterface/GenomeDesc.h>
#include <EngineInterface/ParametersValidationService.h>

#include <PersisterInterface/SerializerService.h>

namespace
{
    std::pair<int, int> parseWorld(std::string const& text)
    {
        auto sep = text.find('x');
        if (sep == std::string::npos) {
            throw std::runtime_error("--world expects WxH, e.g. 4000x1200");
        }
        return {std::stoi(text.substr(0, sep)), std::stoi(text.substr(sep + 1))};
    }

    // --- self-contained fBm value noise for organic terrain ---
    float hashToUnit(int x, int y, unsigned seed)
    {
        auto h = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u + seed * 2246822519u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
    }

    float valueNoise(float x, float y, unsigned seed)
    {
        auto xi = static_cast<int>(std::floor(x));
        auto yi = static_cast<int>(std::floor(y));
        auto fx = x - xi;
        auto fy = y - yi;
        auto sx = fx * fx * (3.0f - 2.0f * fx);
        auto sy = fy * fy * (3.0f - 2.0f * fy);
        auto v00 = hashToUnit(xi, yi, seed);
        auto v10 = hashToUnit(xi + 1, yi, seed);
        auto v01 = hashToUnit(xi, yi + 1, seed);
        auto v11 = hashToUnit(xi + 1, yi + 1, seed);
        auto a = v00 + (v10 - v00) * sx;
        auto b = v01 + (v11 - v01) * sx;
        return a + (b - a) * sy;
    }

    float fbm(float x, float y, unsigned seed, int octaves = 4)
    {
        auto amplitude = 0.5f;
        auto frequency = 1.0f;
        auto sum = 0.0f;
        auto norm = 0.0f;
        for (int i = 0; i < octaves; ++i) {
            sum += amplitude * valueNoise(x * frequency, y * frequency, seed + i * 101);
            norm += amplitude;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        return sum / norm;
    }

    RealVector2D centerOfCells(std::vector<ObjectDesc> const& objects)
    {
        RealVector2D sum;
        for (auto const& object : objects) {
            sum += object._pos;
        }
        if (!objects.empty()) {
            sum *= 1.0f / static_cast<float>(objects.size());
        }
        return sum;
    }

    int dumpScene(std::string const& inputFilename, std::filesystem::path const& outDir, int topK)
    {
        SimulationDesc simData;
        if (!SerializerService::get().deserializeSimulationFromFiles(simData, inputFilename)) {
            std::cerr << "Could not read " << inputFilename << std::endl;
            return 1;
        }
        std::filesystem::create_directories(outDir);

        SerializerService::get().serializeSimulationParametersToFile(outDir / "parameters.json", simData._simulationParameters);

        auto const& content = simData._mainData;

        // classify objects
        size_t numSolid = 0, numFluid = 0, numFreeCells = 0, numCells = 0;
        std::map<int, size_t> cellColorCounts;
        for (auto const& object : content._objects) {
            switch (object.getObjectType()) {
            case ObjectType_Solid:
                ++numSolid;
                break;
            case ObjectType_Fluid:
                ++numFluid;
                break;
            case ObjectType_FreeCell:
                ++numFreeCells;
                break;
            default:
                ++numCells;
                ++cellColorCounts[object._color];
                break;
            }
        }

        // creatures per genome
        std::map<uint64_t, std::vector<size_t>> genomeToCreatureIndices;
        for (size_t i = 0; i < content._creatures.size(); ++i) {
            genomeToCreatureIndices[content._creatures[i]._genomeId].push_back(i);
        }
        std::vector<std::pair<uint64_t, size_t>> genomesByPopulation;
        for (auto const& [genomeId, indices] : genomeToCreatureIndices) {
            genomesByPopulation.emplace_back(genomeId, indices.size());
        }
        std::sort(genomesByPopulation.begin(), genomesByPopulation.end(), [](auto const& a, auto const& b) { return a.second > b.second; });

        // cells per creature (for picking a grown representative)
        std::map<uint64_t, std::vector<size_t>> creatureToCellIndices;
        for (size_t i = 0; i < content._objects.size(); ++i) {
            auto const& object = content._objects[i];
            if (object.getObjectType() == ObjectType_Cell) {
                creatureToCellIndices[object.getCellRef()._creatureId].push_back(i);
            }
        }

        std::ofstream summary(outDir / "summary.txt");
        summary << "scene: " << inputFilename << "\n";
        summary << "world: " << simData._worldSize.x << " x " << simData._worldSize.y << ", timestep " << simData._timestep << "\n\n";
        summary << "objects: " << content._objects.size() << " (solid " << numSolid << ", fluid " << numFluid << ", freeCell " << numFreeCells << ", cell "
                << numCells << ")\n";
        summary << "energy particles: " << content._energies.size() << "\n";
        summary << "creatures: " << content._creatures.size() << ", genomes: " << content._genomes.size() << "\n\n";
        summary << "cell colors:";
        for (auto const& [color, count] : cellColorCounts) {
            summary << "  c" << color << "=" << count;
        }
        summary << "\n\ngenomes by population (top " << topK << "):\n";

        auto findGenome = [&](uint64_t genomeId) -> GenomeDesc const* {
            for (auto const& genome : content._genomes) {
                if (genome._id == genomeId) {
                    return &genome;
                }
            }
            return nullptr;
        };

        auto exported = 0;
        for (auto const& [genomeId, population] : genomesByPopulation) {
            if (exported >= topK) {
                break;
            }
            auto const* genome = findGenome(genomeId);
            if (!genome) {
                continue;
            }
            size_t numNodes = 0;
            for (auto const& gene : genome->_genes) {
                numNodes += gene._nodes.size();
            }

            // grown representative: creature with the most cells
            size_t bestCreatureIdx = SIZE_MAX;
            size_t bestCellCount = 0;
            for (auto creatureIdx : genomeToCreatureIndices[genomeId]) {
                auto const& creature = content._creatures[creatureIdx];
                auto cellCount = creatureToCellIndices[creature._id].size();
                if (cellCount >= bestCellCount) {
                    bestCellCount = cellCount;
                    bestCreatureIdx = creatureIdx;
                }
            }
            summary << "  #" << exported << " genomeId " << genomeId << ": creatures " << population << ", genes " << genome->_genes.size() << ", nodes "
                    << numNodes << ", representative cells " << bestCellCount << "\n";

            // decode the genome: per gene shape and node cell types with replication wiring
            static char const* cellTypeNames[] =
                {"Base", "Depot", "Sensor", "Generator", "Attacker", "Injector", "Muscle", "Defender", "Reconnector", "Detonator", "Digestor", "Memory", "Communicator", "Void"};
            static char const* shapeNames[] = {"Segment", "Triangle", "Rectangle", "Hexagon", "Tube", "LargeLolli", "SmallLolli", "Zigzag"};
            for (size_t geneIdx = 0; geneIdx < genome->_genes.size(); ++geneIdx) {
                auto const& gene = genome->_genes[geneIdx];
                auto shapeIdx = static_cast<size_t>(gene._shape);
                summary << "      gene[" << geneIdx << "] shape=" << (shapeIdx < 8 ? shapeNames[shapeIdx] : "?") << " connDist=" << gene._connectionDistance
                        << " stiffness=" << gene._stiffness << " nodes[";
                for (auto const& node : gene._nodes) {
                    auto cellType = static_cast<size_t>(node.getCellType());
                    summary << " " << (cellType < 14 ? cellTypeNames[cellType] : "?") << "(c" << node._color;
                    if (node._constructor.has_value()) {
                        auto const& ctor = node._constructor.value();
                        summary << ",ctor->gene" << ctor._geneIndex << ",branches=" << ctor._numBranches << ",concat="
                                << (ctor._numConcatenations == std::numeric_limits<int>::max() ? -1 : ctor._numConcatenations)
                                << (ctor._separation ? ",sep" : "");
                    }
                    summary << ")";
                }
                summary << " ]\n";
            }

            SerializerService::get().serializeGenomeToFile(outDir / ("genome-" + std::to_string(exported) + ".genome"), *genome);

            if (bestCreatureIdx != SIZE_MAX && bestCellCount > 0) {
                auto const& creature = content._creatures[bestCreatureIdx];
                std::vector<ObjectDesc> cells;
                for (auto cellIdx : creatureToCellIndices[creature._id]) {
                    cells.push_back(content._objects[cellIdx]);
                }
                auto center = centerOfCells(cells);
                std::unordered_set<uint64_t> cellIds;
                for (auto const& cell : cells) {
                    cellIds.insert(cell._id);
                }
                for (auto& cell : cells) {
                    cell._pos -= center;
                    // the creature may be anchored to shelf cells in the source
                    // scene; connections leaving the creature would dangle
                    std::erase_if(cell._connections, [&](ConnectionDesc const& connection) { return !cellIds.contains(connection._objectId); });
                    if (cell.getObjectType() == ObjectType_Cell && cell.getCellRef()._constructor.has_value()) {
                        cell.getCellRef()._constructor->lastConstructedCellId(std::nullopt);
                    }
                }
                ContentDesc seed;
                seed.addCreature(cells, creature, *genome);
                SerializerService::get().serializeContentToFile(outDir / ("creature-" + std::to_string(exported) + ".content"), seed);
            }
            ++exported;
        }
        summary.close();

        std::cout << "dumped to " << outDir.string() << " (" << exported << " seed(s) exported)" << std::endl;
        std::ifstream showSummary(outDir / "summary.txt");
        std::cout << showSummary.rdbuf();
        return 0;
    }

    struct GardenParams
    {
        int worldW = 4000;
        int worldH = 1200;
        std::string layout = "shelves";  // shelves | spiral | islands | wild | membrane
        float spiralTurns = 2.6f;
        float terrainScale = 380.0f;  // wild: noise feature size in world units
        int rockPoints = 26000;       // wild: approximate solid point budget
        int shelves = 4;
        float amplitude = 50.0f;
        float wavelength = 900.0f;
        int tendrilsPerShelf = 14;
        float tendrilLength = 60.0f;
        int numSeeds = 8;
        int energyParticles = 3000;
        float energyValue = 100.0f;
        int fluidParticles = 80000;
        unsigned rngSeed = 42;
        int shelfColor = 6;
        std::string bodyShape;  // empty = keep the seed's genome; comma list cycles per plant: segment,hexagon,...
        std::string bodyNodes;  // empty/0 = keep node count; comma list cycles per plant, e.g. 0,5,4
        std::string recolor = "1";  // comma list cycles per plant: 1 = recolor to i%7 (pitch class variety), 0 = keep species color (color IS ecology: energy inflow and food chain are per-color)
        std::string arm = "0";      // comma list cycles per plant: 1 = insert an Attacker node (AttackCreature mode) as the second node of gene[0]
    };

    std::vector<std::string> splitList(std::string const& text)
    {
        std::vector<std::string> result;
        size_t start = 0;
        while (start <= text.size()) {
            auto end = text.find(',', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            result.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return result;
    }

    std::optional<ConstructorShape> shapeFromName(std::string const& name)
    {
        static std::map<std::string, ConstructorShape> const shapes = {
            {"segment", ConstructorShape_Segment},
            {"triangle", ConstructorShape_Triangle},
            {"rectangle", ConstructorShape_Rectangle},
            {"hexagon", ConstructorShape_Hexagon},
            {"tube", ConstructorShape_Tube},
            {"largelolli", ConstructorShape_LargeLolli},
            {"smalllolli", ConstructorShape_SmallLolli},
            {"zigzag", ConstructorShape_Zigzag},
        };
        auto lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        auto findResult = shapes.find(lower);
        return findResult != shapes.end() ? std::optional(findResult->second) : std::nullopt;
    }

    void addSolidPoint(ContentDesc& content, float x, float y, int color)
    {
        auto object = ObjectDesc();
        object.pos({x, y});
        object.stiffness(1.0f);
        object.color(color);
        object.isStatic(true);
        object.type(SolidDesc());
        content._objects.push_back(object);
    }

    int createGarden(std::string const& outputFilename, std::string const& paramsFile, std::vector<std::string> const& seedFiles, GardenParams const& garden)
    {
        SimulationParameters parameters;
        if (!paramsFile.empty()) {
            if (!SerializerService::get().deserializeSimulationParametersFromFile(parameters, paramsFile)) {
                std::cerr << "Could not read parameters from " << paramsFile << std::endl;
                return 1;
            }
        }
        IntVector2D worldSize{garden.worldW, garden.worldH};
        ParametersValidationService::get().validateAndCorrect({worldSize}, parameters);

        std::mt19937 rng(garden.rngSeed);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        ContentDesc content;

        // seed placement targets, filled by the chosen layout
        std::vector<RealVector2D> seedPositions;

        if (garden.layout == "spiral") {
            // --- Archimedean spiral wall with inward tendrils ---
            auto cx = garden.worldW * 0.5f;
            auto cy = garden.worldH * 0.5f;
            auto rOut = std::min(garden.worldW, garden.worldH) * 0.45f;
            auto rIn = rOut * 0.15f;
            auto thetaMax = 6.2831853f * garden.spiralTurns;
            auto slope = (rOut - rIn) / thetaMax;

            auto arcSinceTendril = 0.0f;
            auto tendrilSpacing = 140.0f;
            for (float theta = 0.0f; theta < thetaMax;) {
                auto r = rIn + slope * theta;
                auto x = cx + r * std::cos(theta);
                auto y = cy + r * std::sin(theta);
                addSolidPoint(content, x, y, garden.shelfColor);

                arcSinceTendril += 1.0f;
                if (arcSinceTendril >= tendrilSpacing && garden.tendrilsPerShelf > 0) {
                    arcSinceTendril = 0.0f;
                    auto length = garden.tendrilLength * (0.4f + 0.6f * unit(rng));
                    auto dirX = (cx - x) / std::max(r, 1.0f);
                    auto dirY = (cy - y) / std::max(r, 1.0f);
                    for (float d = 2.0f; d <= length; d += 1.0f) {
                        auto sway = 5.0f * std::sin(d * 0.17f + theta);
                        addSolidPoint(content, x + dirX * d - dirY * sway, y + dirY * d + dirX * sway, garden.shelfColor);
                    }
                }
                theta += 1.0f / std::sqrt(r * r + slope * slope);
            }

            for (int i = 0; i < garden.numSeeds; ++i) {
                auto t = (i + 0.5f) / garden.numSeeds;
                auto theta = thetaMax * t;
                auto r = rIn + slope * theta - 14.0f;  // just inside the wall
                seedPositions.push_back({cx + r * std::cos(theta), cy + r * std::sin(theta)});
            }

            // fluid: dense mist near the core, thinning outwards, plus a faint global haze
            for (int i = 0; i < garden.fluidParticles; ++i) {
                auto object = ObjectDesc();
                if (unit(rng) < 0.75f) {
                    auto r = rOut * std::pow(unit(rng), 1.6f);
                    auto a = unit(rng) * 6.2831853f;
                    object.pos({cx + r * std::cos(a), cy + r * std::sin(a)});
                } else {
                    object.pos({unit(rng) * garden.worldW, unit(rng) * garden.worldH});
                }
                object.vel({(unit(rng) - 0.5f) * 0.2f, (unit(rng) - 0.5f) * 0.2f});
                object.color(garden.shelfColor);
                object.type(FluidDesc());
                content._objects.push_back(object);
            }
        } else if (garden.layout == "wild") {
            // --- organic terrain: no drawn geometry, only a noise field ---
            // rocks scatter where the field is high (cloudy reefs with no readable
            // outline), fluid pools in the lowlands, life is seeded on the shores.
            auto height = [&](float x, float y) { return fbm(x / garden.terrainScale, y / garden.terrainScale, garden.rngSeed, 4); };

            auto rockThreshold = 0.60f;
            size_t placed = 0;
            size_t attempts = 0;
            auto maxAttempts = static_cast<size_t>(garden.rockPoints) * 60;
            while (placed < static_cast<size_t>(garden.rockPoints) && attempts++ < maxAttempts) {
                auto x = unit(rng) * garden.worldW;
                auto y = unit(rng) * garden.worldH;
                auto v = height(x, y);
                if (v <= rockThreshold || unit(rng) > (v - rockThreshold) * 4.0f) {
                    continue;
                }
                // one accepted site becomes a small pebble cluster
                auto clusterSize = 2 + static_cast<int>(unit(rng) * 5);
                for (int p = 0; p < clusterSize && placed < static_cast<size_t>(garden.rockPoints); ++p) {
                    auto a = unit(rng) * 6.2831853f;
                    auto r = unit(rng) * 3.2f;
                    addSolidPoint(content, x + r * std::cos(a), y + r * std::sin(a), garden.shelfColor);
                    ++placed;
                }
                // occasionally a whisker grows off the rock
                if (unit(rng) < 0.06f) {
                    auto wa = unit(rng) * 6.2831853f;
                    auto length = garden.tendrilLength * (0.3f + 0.5f * unit(rng));
                    for (float d = 2.0f; d <= length; d += 1.0f) {
                        auto sway = 3.5f * std::sin(d * 0.2f + x);
                        addSolidPoint(content, x + std::cos(wa) * d - std::sin(wa) * sway, y + std::sin(wa) * d + std::cos(wa) * sway, garden.shelfColor);
                    }
                }
            }

            // seeds on the shores: the mid-band between rock and open water
            attempts = 0;
            while (static_cast<int>(seedPositions.size()) < garden.numSeeds && attempts++ < 200000) {
                auto x = garden.worldW * (0.06f + 0.88f * unit(rng));
                auto y = garden.worldH * (0.06f + 0.88f * unit(rng));
                auto v = height(x, y);
                if (v > 0.46f && v < 0.58f) {
                    seedPositions.push_back({x, y});
                }
            }

            // fluid pools in the lowlands
            attempts = 0;
            int placedFluid = 0;
            while (placedFluid < garden.fluidParticles && attempts++ < static_cast<size_t>(garden.fluidParticles) * 40) {
                auto x = unit(rng) * garden.worldW;
                auto y = unit(rng) * garden.worldH;
                auto v = height(x, y);
                if (v < 0.47f && unit(rng) < (0.47f - v) * 3.0f) {
                    auto object = ObjectDesc();
                    object.pos({x, y});
                    object.vel({(unit(rng) - 0.5f) * 0.2f, (unit(rng) - 0.5f) * 0.2f});
                    object.color(garden.shelfColor);
                    object.type(FluidDesc());
                    content._objects.push_back(object);
                    ++placedFluid;
                }
            }
        } else if (garden.layout == "membrane") {
            // --- contour membranes: walls follow iso-bands of the noise field,
            // forming an organic labyrinth of curved passages and chambers ---
            auto height = [&](float x, float y) { return fbm(x / garden.terrainScale, y / garden.terrainScale, garden.rngSeed, 4); };
            auto bandLo = 0.545f;
            auto bandHi = 0.575f;
            auto step = 1.6f;

            // pass 1: count candidates so the wall budget thins evenly
            size_t candidates = 0;
            for (float y = 0; y < garden.worldH; y += step) {
                for (float x = 0; x < garden.worldW; x += step) {
                    auto v = height(x, y);
                    if (v > bandLo && v < bandHi) {
                        ++candidates;
                    }
                }
            }
            auto keepProbability = candidates > 0 ? std::min(1.0f, static_cast<float>(garden.rockPoints) / candidates) : 0.0f;

            for (float y = 0; y < garden.worldH; y += step) {
                for (float x = 0; x < garden.worldW; x += step) {
                    auto v = height(x, y);
                    if (v <= bandLo || v >= bandHi || unit(rng) > keepProbability) {
                        continue;
                    }
                    addSolidPoint(content, x + (unit(rng) - 0.5f), y + (unit(rng) - 0.5f), garden.shelfColor);
                    if (unit(rng) < 0.004f) {
                        auto wa = unit(rng) * 6.2831853f;
                        auto length = garden.tendrilLength * (0.3f + 0.5f * unit(rng));
                        for (float d = 2.0f; d <= length; d += 1.0f) {
                            auto sway = 3.5f * std::sin(d * 0.2f + x);
                            addSolidPoint(content, x + std::cos(wa) * d - std::sin(wa) * sway, y + std::sin(wa) * d + std::cos(wa) * sway, garden.shelfColor);
                        }
                    }
                }
            }

            // seeds in the open chambers
            size_t attempts = 0;
            while (static_cast<int>(seedPositions.size()) < garden.numSeeds && attempts++ < 200000) {
                auto x = garden.worldW * (0.06f + 0.88f * unit(rng));
                auto y = garden.worldH * (0.06f + 0.88f * unit(rng));
                auto v = height(x, y);
                if (v > 0.42f && v < 0.52f) {
                    seedPositions.push_back({x, y});
                }
            }

            // fluid pools in the deep chambers
            attempts = 0;
            int placedFluid = 0;
            while (placedFluid < garden.fluidParticles && attempts++ < static_cast<size_t>(garden.fluidParticles) * 40) {
                auto x = unit(rng) * garden.worldW;
                auto y = unit(rng) * garden.worldH;
                auto v = height(x, y);
                if (v < 0.46f && unit(rng) < (0.46f - v) * 3.0f) {
                    auto object = ObjectDesc();
                    object.pos({x, y});
                    object.vel({(unit(rng) - 0.5f) * 0.2f, (unit(rng) - 0.5f) * 0.2f});
                    object.color(garden.shelfColor);
                    object.type(FluidDesc());
                    content._objects.push_back(object);
                    ++placedFluid;
                }
            }
        } else if (garden.layout == "islands") {
            // --- archipelago: ring walls with an opening facing the inner sea ---
            constexpr int NumIslands = 8;
            auto cx = garden.worldW * 0.5f;
            auto cy = garden.worldH * 0.5f;
            auto ringR = std::min(garden.worldW, garden.worldH) * 0.33f;
            auto islandR = std::min(garden.worldW, garden.worldH) * 0.115f;

            std::vector<RealVector2D> islandCenters;
            for (int island = 0; island < NumIslands; ++island) {
                auto a = 6.2831853f * island / NumIslands;
                auto ix = cx + ringR * std::cos(a);
                auto iy = cy + ringR * std::sin(a);
                islandCenters.push_back({ix, iy});

                auto gapCenter = a + 3.1415926f;  // opening faces the inner sea
                for (float t = 0.0f; t < 6.2831853f; t += 1.0f / islandR) {
                    auto d = std::fmod(t - gapCenter + 9.42477f, 6.2831853f) - 3.1415926f;
                    if (std::abs(d) < 0.55f) {
                        continue;
                    }
                    addSolidPoint(content, ix + islandR * std::cos(t), iy + islandR * std::sin(t), garden.shelfColor);
                }
                // a few inward tendrils per island
                for (int tendril = 0; tendril < 3; ++tendril) {
                    auto wallAngle = gapCenter + 3.1415926f + (unit(rng) - 0.5f) * 3.6f;
                    auto wx = ix + islandR * std::cos(wallAngle);
                    auto wy = iy + islandR * std::sin(wallAngle);
                    auto length = garden.tendrilLength * (0.4f + 0.6f * unit(rng));
                    auto dirX = (ix - wx) / islandR;
                    auto dirY = (iy - wy) / islandR;
                    for (float d = 2.0f; d <= length; d += 1.0f) {
                        auto sway = 4.0f * std::sin(d * 0.18f + tendril * 2.1f);
                        addSolidPoint(content, wx + dirX * d - dirY * sway, wy + dirY * d + dirX * sway, garden.shelfColor);
                    }
                }
            }

            for (int i = 0; i < garden.numSeeds; ++i) {
                auto const& center = islandCenters[i % NumIslands];
                auto jitterAngle = unit(rng) * 6.2831853f;
                auto jitterR = islandR * 0.35f * unit(rng);
                seedPositions.push_back({center.x + jitterR * std::cos(jitterAngle), center.y + jitterR * std::sin(jitterAngle)});
            }

            // fluid: lagoons inside the islands plus a thin open sea
            for (int i = 0; i < garden.fluidParticles; ++i) {
                auto object = ObjectDesc();
                if (unit(rng) < 0.65f) {
                    auto const& center = islandCenters[static_cast<int>(unit(rng) * NumIslands) % NumIslands];
                    auto a = unit(rng) * 6.2831853f;
                    auto r = islandR * 0.85f * std::sqrt(unit(rng));
                    object.pos({center.x + r * std::cos(a), center.y + r * std::sin(a)});
                } else {
                    object.pos({unit(rng) * garden.worldW, unit(rng) * garden.worldH});
                }
                object.vel({(unit(rng) - 0.5f) * 0.2f, (unit(rng) - 0.5f) * 0.2f});
                object.color(garden.shelfColor);
                object.type(FluidDesc());
                content._objects.push_back(object);
            }
        } else {
            // --- static shelves: sine waves spanning the world, with hanging tendrils ---
            auto marginY = garden.worldH * 0.18f;
            auto marginX = garden.worldW * 0.04f;
            auto shelfSpan = garden.worldH - 2.0f * marginY;
            for (int shelf = 0; shelf < garden.shelves; ++shelf) {
                auto yBase = marginY + (garden.shelves > 1 ? shelfSpan * shelf / (garden.shelves - 1) : shelfSpan * 0.5f);
                auto phase = unit(rng) * 6.2831853f;
                for (float x = marginX; x <= garden.worldW - marginX; x += 1.0f) {
                    auto y = yBase + garden.amplitude * std::sin(6.2831853f * x / garden.wavelength + phase);
                    addSolidPoint(content, x, y, garden.shelfColor);
                }
                for (int tendril = 0; tendril < garden.tendrilsPerShelf; ++tendril) {
                    auto tx = marginX + unit(rng) * (garden.worldW - 2.0f * marginX);
                    auto ty = yBase + garden.amplitude * std::sin(6.2831853f * tx / garden.wavelength + phase);
                    auto length = garden.tendrilLength * (0.4f + 0.6f * unit(rng));
                    for (float d = 1.0f; d <= length; d += 1.0f) {
                        auto sway = 6.0f * std::sin(d * 0.15f + tendril);
                        addSolidPoint(content, tx + sway, ty + d, garden.shelfColor);
                    }
                }
            }

            for (int i = 0; i < garden.numSeeds; ++i) {
                auto x = marginX + (garden.worldW - 2.0f * marginX) * (i + 0.5f) / garden.numSeeds;
                auto shelfIdx = i % std::max(1, garden.shelves);
                auto yBase = marginY + (garden.shelves > 1 ? shelfSpan * shelfIdx / (garden.shelves - 1) : shelfSpan * 0.5f);
                seedPositions.push_back({x, yBase - garden.tendrilLength * 0.8f - 15.0f});
            }

            // fluid sea: thin haze everywhere, denser towards the bottom third
            for (int i = 0; i < garden.fluidParticles; ++i) {
                auto object = ObjectDesc();
                auto inLowerSea = unit(rng) < 0.7f;
                auto y = inLowerSea ? garden.worldH * (0.66f + 0.34f * unit(rng)) : garden.worldH * unit(rng);
                object.pos({unit(rng) * garden.worldW, y});
                object.vel({(unit(rng) - 0.5f) * 0.2f, (unit(rng) - 0.5f) * 0.2f});
                object.color(garden.shelfColor);
                object.type(FluidDesc());
                content._objects.push_back(object);
            }
        }

        // --- seeds: creatures from the library, one lineage and color per plant ---
        if (!seedFiles.empty() && garden.numSeeds > 0) {
            for (int i = 0; i < garden.numSeeds && i < static_cast<int>(seedPositions.size()); ++i) {
                ContentDesc seed;
                auto const& seedFile = seedFiles[i % seedFiles.size()];
                if (!SerializerService::get().deserializeContentFromFile(seed, seedFile)) {
                    std::cerr << "Could not read seed " << seedFile << std::endl;
                    return 1;
                }
                auto recolorList = splitList(garden.recolor);
                if (recolorList[i % recolorList.size()] == "1") {
                    auto color = i % 7;
                    DescEditService::get().randomizeCellColors(seed, {color});
                    DescEditService::get().randomizeGenomeColors(seed, {color});
                }
                for (auto& creature : seed._creatures) {
                    creature.lineageId(i + 1);
                }

                // weapons: insert an Attacker node after the head node (the head cell
                // is already built, so only unbuilt genome positions are touched)
                {
                    auto armList = splitList(garden.arm);
                    if (armList[i % armList.size()] == "1") {
                        for (auto& genome : seed._genomes) {
                            if (!genome._genes.empty() && !genome._genes[0]._nodes.empty()) {
                                auto& nodes = genome._genes[0]._nodes;
                                auto attackNode = nodes.front();
                                attackNode.cellType(AttackerGenomeDesc());
                                attackNode.constructor(std::nullopt);
                                nodes.insert(nodes.begin() + 1, attackNode);
                            }
                        }
                    }
                }

                // custom morphology: rewrite the body plan of the inherited genome
                // (comma lists cycle per plant, so several morphotypes share one garden)
                if (!garden.bodyShape.empty()) {
                    auto shapeNames = splitList(garden.bodyShape);
                    auto nodeCounts = garden.bodyNodes.empty() ? std::vector<std::string>{"0"} : splitList(garden.bodyNodes);
                    auto const& shapeName = shapeNames[i % shapeNames.size()];
                    auto numNodes = std::stoi(nodeCounts[i % nodeCounts.size()]);
                    if (!shapeName.empty() && shapeName != "keep") {
                        auto shape = shapeFromName(shapeName);
                        if (!shape.has_value()) {
                            std::cerr << "Unknown --body-shape entry: " << shapeName << std::endl;
                            return 1;
                        }
                        for (auto& genome : seed._genomes) {
                            for (auto& gene : genome._genes) {
                                gene.shape(*shape);
                                if (numNodes > 0 && !gene._nodes.empty()) {
                                    auto prototype = gene._nodes.front();
                                    gene._nodes.assign(numNodes, prototype);
                                }
                            }
                        }
                    }
                }
                for (auto& object : seed._objects) {
                    object._pos += seedPositions[i];
                }
                content.add(std::move(seed), true);
            }
        }

        // --- energy particles everywhere ---
        for (int i = 0; i < garden.energyParticles; ++i) {
            auto energy = EnergyDesc();
            energy.pos({unit(rng) * garden.worldW, unit(rng) * garden.worldH});
            energy.vel({(unit(rng) - 0.5f) * 0.4f, (unit(rng) - 0.5f) * 0.4f});
            energy.energy(garden.energyValue);
            energy.color(static_cast<int>(unit(rng) * 7) % 7);
            content._energies.push_back(energy);
        }

        // referential integrity check (mirrors the lookups in DescConverterService)
        {
            std::unordered_set<uint64_t> creatureIds, genomeIds;
            for (auto const& creature : content._creatures) {
                creatureIds.insert(creature._id);
            }
            for (auto const& genome : content._genomes) {
                genomeIds.insert(genome._id);
            }
            size_t danglingCellRefs = 0, danglingGenomeRefs = 0, cellsWithZeroCreature = 0;
            for (auto const& object : content._objects) {
                if (object.getObjectType() == ObjectType_Cell) {
                    auto creatureId = object.getCellRef()._creatureId;
                    if (creatureId == 0) {
                        ++cellsWithZeroCreature;
                    } else if (!creatureIds.contains(creatureId)) {
                        ++danglingCellRefs;
                    }
                }
            }
            for (auto const& creature : content._creatures) {
                if (!genomeIds.contains(creature._genomeId)) {
                    ++danglingGenomeRefs;
                }
            }
            std::cout << "integrity: creatures " << content._creatures.size() << ", genomes " << content._genomes.size() << ", cell->creature dangling "
                      << danglingCellRefs << " (zero " << cellsWithZeroCreature << "), creature->genome dangling " << danglingGenomeRefs << std::endl;
        }

        SimulationDesc simData;
        simData.timestep(0)
            .worldSize(worldSize)
            .simulationParameters(parameters)
            .mainData(content)
            .statistics(StatisticsHistoryData())
            .realTime(std::chrono::milliseconds(0));

        if (!SerializerService::get().serializeSimulationToFiles(outputFilename, simData)) {
            std::cerr << "Could not write " << outputFilename << std::endl;
            return 1;
        }
        std::cout << "garden written: " << outputFilename << "\n"
                  << "  world " << garden.worldW << "x" << garden.worldH << ", shelves " << garden.shelves << ", solid points " << content._objects.size()
                  << ", seeds " << (seedFiles.empty() ? 0 : garden.numSeeds) << ", energies " << content._energies.size() << std::endl;
        return 0;
    }
}

int main(int argc, char** argv)
{
    try {
        FileLogger fileLogger = std::make_shared<_FileLogger>();

        CLI::App app{"Garden construction kit for ALIEN v" + Const::ProgramVersion};
        app.require_subcommand(1);

        auto* dump = app.add_subcommand("dump", "Dissect a scene: parameters.json, summary, seed library.");
        std::string dumpInput;
        std::string dumpOutDir = "garden-dump";
        int topK = 4;
        dump->add_option("-i", dumpInput, "Input .sim file")->required();
        dump->add_option("-o", dumpOutDir, "Output directory");
        dump->add_option("--top", topK, "Number of genomes to export as seeds");

        auto* create = app.add_subcommand("new", "Generate a procedural garden .sim");
        std::string outputFilename = "garden.sim";
        std::string paramsFile;
        std::vector<std::string> seedFiles;
        std::string worldText = "4000x1200";
        GardenParams garden;
        create->add_option("-o", outputFilename, "Output .sim file");
        create->add_option("--params", paramsFile, "SimulationParameters JSON (from dump)");
        create->add_option("--seed", seedFiles, "Seed .content file(s) from dump (repeatable)");
        create->add_option("--world", worldText, "World size WxH");
        create->add_option("--layout", garden.layout, "Structure layout: shelves | spiral | islands | wild | membrane");
        create->add_option("--terrain-scale", garden.terrainScale, "wild: noise feature size in world units");
        create->add_option("--rock-points", garden.rockPoints, "wild: solid point budget");
        create->add_option("--turns", garden.spiralTurns, "Spiral turns (layout=spiral)");
        create->add_option("--shelves", garden.shelves, "Number of shelves");
        create->add_option("--amplitude", garden.amplitude, "Shelf wave amplitude");
        create->add_option("--wavelength", garden.wavelength, "Shelf wave length");
        create->add_option("--tendrils", garden.tendrilsPerShelf, "Hanging tendrils per shelf");
        create->add_option("--tendril-length", garden.tendrilLength, "Max tendril length");
        create->add_option("--seeds", garden.numSeeds, "Number of creatures to plant");
        create->add_option("--energy", garden.energyParticles, "Number of energy particles");
        create->add_option("--energy-value", garden.energyValue, "Energy per particle");
        create->add_option("--fluid", garden.fluidParticles, "Number of fluid particles (the sea)");
        create->add_option("--rng", garden.rngSeed, "Random seed");
        create->add_option("--shelf-color", garden.shelfColor, "Color index of shelf cells (0-6)");
        create->add_option(
            "--body-shape", garden.bodyShape, "Rewrite seed genome gene shapes; comma list cycles per plant (keep|segment|triangle|rectangle|hexagon|tube|largelolli|smalllolli|zigzag)");
        create->add_option("--body-nodes", garden.bodyNodes, "Resize each gene to N nodes; comma list cycles per plant (0 = keep)");
        create->add_option("--recolor", garden.recolor, "Comma list cycling per plant: 1 = recolor to i%7, 0 = keep species color (color drives energy inflow and food chain)");
        create->add_option("--arm", garden.arm, "Comma list cycling per plant: 1 = insert an Attacker node (hunts creatures) into gene[0]");

        CLI11_PARSE(app, argc, argv);

        if (dump->parsed()) {
            return dumpScene(dumpInput, dumpOutDir, topK);
        }
        auto [w, h] = parseWorld(worldText);
        garden.worldW = w;
        garden.worldH = h;
        return createGarden(outputFilename, paramsFile, seedFiles, garden);
    } catch (std::exception const& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
