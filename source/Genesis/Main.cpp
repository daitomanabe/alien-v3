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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
    };

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

        // --- seeds: creatures from the library, one lineage and color per plant ---
        if (!seedFiles.empty() && garden.numSeeds > 0) {
            for (int i = 0; i < garden.numSeeds; ++i) {
                ContentDesc seed;
                auto const& seedFile = seedFiles[i % seedFiles.size()];
                if (!SerializerService::get().deserializeContentFromFile(seed, seedFile)) {
                    std::cerr << "Could not read seed " << seedFile << std::endl;
                    return 1;
                }
                auto color = i % 7;
                DescEditService::get().randomizeCellColors(seed, {color});
                DescEditService::get().randomizeGenomeColors(seed, {color});
                for (auto& creature : seed._creatures) {
                    creature.lineageId(i + 1);
                }

                // place between shelves, horizontally spread
                auto x = marginX + (garden.worldW - 2.0f * marginX) * (i + 0.5f) / garden.numSeeds;
                auto shelfIdx = i % std::max(1, garden.shelves);
                auto yBase = marginY + (garden.shelves > 1 ? shelfSpan * shelfIdx / (garden.shelves - 1) : shelfSpan * 0.5f);
                auto y = yBase - garden.tendrilLength * 0.8f - 15.0f;
                for (auto& object : seed._objects) {
                    object._pos += RealVector2D{x, y};
                }
                content.add(std::move(seed), true);
            }
        }

        // --- fluid sea: thin haze everywhere, denser towards the bottom third ---
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
