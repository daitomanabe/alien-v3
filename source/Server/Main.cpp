#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <thread>

#include <CLI/CLI.hpp>

#include <Base/AlienExceptions.h>
#include <Base/FileLogger.h>
#include <Base/LoggingService.h>
#include <Base/Resources.h>

#include <EngineInterface/HostRenderData.h>
#include <EngineInterface/SimulationFacade.h>

#include <EngineImpl/SimulationFacadeImpl.h>

#include <PersisterInterface/SerializerService.h>

#include "GeomStreamer.h"
#include "OscSender.h"

namespace
{
    std::atomic<bool> stopRequested = false;

    void handleSignal(int)
    {
        stopRequested = true;
    }

    // Accumulated lineage counters from the previous tick, to derive rates.
    struct LineageBaseline
    {
        double totalMuscleActivity = 0;
        double totalAttackedEnergy = 0;
        double totalMutations = 0;
    };
}

int main(int argc, char** argv)
{
    try {
        FileLogger fileLogger = std::make_shared<_FileLogger>();

        CLI::App app{"Headless ALIEN server: runs a simulation and streams statistics/events via OSC plus render geometry via UDP. v" + Const::ProgramVersion};

        std::string inputFilename;
        std::string oscHost;
        int oscPort = 0;
        int oscListen = 12000;
        std::string geomHost;
        int geomPort = 0;
        int geomListen = 12001;
        double rate = 20.0;
        int tpsCap = 0;
        int maxAttacks = 48;
        int maxLineages = 8;
        int maxGeomCells = 2000;
        int maxGeomFluid = 2000;
        int maxGeomLines = 1500;
        app.add_option("-i", inputFilename, "Simulation input file.")->required();
        app.add_option("--osc-listen", oscListen, "UDP port for OSC subscribers; any datagram subscribes its sender.");
        app.add_option("--osc-host", oscHost, "Optional fixed OSC destination host (push mode, e.g. localhost tests).");
        app.add_option("--osc-port", oscPort, "Optional fixed OSC destination port.");
        app.add_option("--geom-listen", geomListen, "UDP port for geometry subscribers.");
        app.add_option("--geom-host", geomHost, "Optional fixed geometry destination host.");
        app.add_option("--geom-port", geomPort, "Optional fixed geometry destination port.");
        app.add_option("--rate", rate, "Send rate in Hz.");
        app.add_option("--tps", tpsCap, "Cap simulation speed in timesteps per second (0 = unlimited).");
        app.add_option("--max-attacks", maxAttacks, "Max attack events sent per tick.");
        app.add_option("--max-lineages", maxLineages, "Max lineages sent per tick (largest first).");
        app.add_option("--geom-cells", maxGeomCells, "Max sampled cells per geometry frame.");
        app.add_option("--geom-fluid", maxGeomFluid, "Max sampled fluid particles per geometry frame.");
        app.add_option("--geom-lines", maxGeomLines, "Max sampled cell-connection lines per geometry frame.");
        CLI11_PARSE(app, argc, argv);

        std::cout << "Reading " << inputFilename << std::endl;
        SimulationDesc simData;
        if (!SerializerService::get().deserializeSimulationFromFiles(simData, inputFilename)) {
            std::cout << "Could not read from input files." << std::endl;
            return 1;
        }

        auto simulationFacade = std::make_shared<_SimulationFacadeImpl>();
        simulationFacade->newSimulation(simData._timestep, simData._worldSize, simData._simulationParameters);
        simulationFacade->setSimulationData(simData._mainData);
        simulationFacade->setStatisticsHistory(simData._statistics);
        simulationFacade->setRealTime(simData._realTime);
        std::cout << "Device: " << simulationFacade->getGpuName() << std::endl;

        auto worldSize = simulationFacade->getWorldSize();
        RealRect worldRect{{0.0f, 0.0f}, {static_cast<float>(worldSize.x), static_cast<float>(worldSize.y)}};

        UdpChannel oscChannel(oscListen, oscHost, oscPort);
        GeomStreamer geomStreamer(geomListen, geomHost, geomPort, maxGeomCells, maxGeomFluid, maxGeomLines);
        geomStreamer.setWorldSize(static_cast<float>(worldSize.x), static_cast<float>(worldSize.y));

        if (tpsCap > 0) {
            simulationFacade->setTpsRestriction(tpsCap);
        }

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        std::cout << "OSC subscribers on udp/" << oscListen << ", geometry subscribers on udp/" << geomListen << ", rate " << rate << " Hz. World "
                  << worldSize.x << "x" << worldSize.y << ". Ctrl-C stops." << std::endl;

        simulationFacade->runSimulation();

        HostRenderData renderData;
        std::map<uint32_t, LineageBaseline> baselines;
        auto tickDuration = std::chrono::duration<double>(1.0 / rate);
        auto nextTick = std::chrono::steady_clock::now();
        uint64_t lastReportTimestep = 0;
        auto lastReportTime = std::chrono::steady_clock::now();

        while (!stopRequested) {
            nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(tickDuration);
            std::this_thread::sleep_until(nextTick);

            simulationFacade->checkAndThrowException();

            auto oscPollResult = oscChannel.poll();
            if (oscPollResult.subscribersChanged) {
                std::cout << "OSC subscribers: " << oscChannel.targetString() << std::endl;
            }
            if (geomStreamer.channel().poll().subscribersChanged) {
                std::cout << "Geometry subscribers: " << geomStreamer.channel().targetString() << std::endl;
                geomStreamer.markStaticDirty();
            }
            for (auto const& command : oscPollResult.commands) {
                if (command == "/alien/cataclysm") {
                    std::cout << "Command: cataclysm" << std::endl;
                    simulationFacade->applyCataclysm(1);
                } else if (command == "/alien/save") {
                    auto saveTimestep = simulationFacade->getCurrentTimestep();
                    auto savePath = "saved-" + std::to_string(saveTimestep) + ".sim";
                    SimulationDesc saveData;
                    saveData.timestep(saveTimestep)
                        .worldSize(simulationFacade->getWorldSize())
                        .mainData(simulationFacade->getSimulationData())
                        .simulationParameters(simulationFacade->getSimulationParameters())
                        .statistics(simulationFacade->getStatisticsHistory().getCopiedData())
                        .realTime(simulationFacade->getRealTime());
                    if (SerializerService::get().serializeSimulationToFiles(savePath, saveData)) {
                        std::cout << "Command: saved to " << savePath << std::endl;
                    } else {
                        std::cout << "Command: save FAILED" << std::endl;
                    }
                } else {
                    std::cout << "Unknown command: " << command << std::endl;
                }
            }

            auto statistics = simulationFacade->getStatisticsEntry();
            auto haveRenderData = simulationFacade->tryExtractRenderDataToHost(renderData, worldRect);

            OscBundle bundle;

            OscMessage worldMsg("/alien/world");
            worldMsg.addInt(worldSize.x).addInt(worldSize.y);
            bundle.add(worldMsg);

            auto const& objects = statistics.objectStatistics;
            OscMessage stats("/alien/stats");
            stats.addInt(static_cast<int32_t>(objects.numCellObjects))
                .addInt(static_cast<int32_t>(objects.numFreeCellObjects))
                .addInt(static_cast<int32_t>(objects.numEnergyParticles))
                .addFloat(static_cast<float>(objects.totalInternalEnergy))
                .addFloat(simulationFacade->getTps())
                .addInt(static_cast<int32_t>(simulationFacade->getCurrentTimestep() & 0x7FFFFFFF));
            bundle.add(stats);

            // Largest lineages first
            auto lineages = statistics.lineageEntries;
            std::sort(lineages.begin(), lineages.end(), [](auto const& a, auto const& b) { return a.numCreatures > b.numCreatures; });
            auto numLineages = std::min<size_t>(lineages.size(), maxLineages);
            for (size_t i = 0; i < numLineages; ++i) {
                auto const& lineage = lineages[i];
                auto& baseline = baselines[lineage.lineageId];
                auto muscleDelta = static_cast<float>((lineage.totalMuscleActivity - baseline.totalMuscleActivity) * rate);
                auto attackDelta = static_cast<float>((lineage.totalAttackedEnergy - baseline.totalAttackedEnergy) * rate);
                auto mutationsDelta = static_cast<float>((lineage.totalMutations - baseline.totalMutations) * rate);
                baseline = {lineage.totalMuscleActivity, lineage.totalAttackedEnergy, lineage.totalMutations};

                auto avgGenerations =
                    lineage.numCreatures > 0 ? static_cast<float>(lineage.sumCreatureGenerations) / static_cast<float>(lineage.numCreatures) : 0.0f;
                OscMessage msg("/alien/lineage");
                msg.addInt(static_cast<int32_t>(lineage.lineageId))
                    .addInt(static_cast<int32_t>(lineage.numCreatures))
                    .addInt(static_cast<int32_t>(lineage.sumCreatureCells))
                    .addFloat(static_cast<float>(lineage.sumCreatureEnergy))
                    .addFloat(muscleDelta)
                    .addFloat(attackDelta)
                    .addFloat(mutationsDelta)
                    .addFloat(avgGenerations)
                    .addInt(static_cast<int32_t>(lineage.colorBitset));
                bundle.add(msg);
            }

            oscChannel.send(bundle.encode());

            if (haveRenderData) {
                // Separate bundle so each UDP datagram stays below one MTU
                OscBundle eventBundle;
                auto totalAttacks = renderData.attackEvents.size();
                OscMessage attackCount("/alien/attacks");
                attackCount.addInt(static_cast<int32_t>(totalAttacks));
                eventBundle.add(attackCount);

                auto numAttacksToSend = std::min<size_t>(totalAttacks, maxAttacks);
                if (numAttacksToSend > 0) {
                    auto strideAttacks = std::max<size_t>(1, totalAttacks / numAttacksToSend);
                    for (size_t i = 0; i < totalAttacks && eventBundle.size() <= numAttacksToSend; i += strideAttacks) {
                        auto const& attack = renderData.attackEvents[i];
                        OscMessage msg("/alien/attack");
                        msg.addFloat(attack.pos[0]).addFloat(attack.pos[1]);
                        eventBundle.add(msg);
                    }
                }

                for (auto const& detonation : renderData.detonationEvents) {
                    if (eventBundle.size() >= numAttacksToSend + 8) {
                        break;
                    }
                    OscMessage msg("/alien/detonation");
                    msg.addFloat(detonation.pos[0]).addFloat(detonation.pos[1]).addFloat(detonation.radius);
                    eventBundle.add(msg);
                }
                oscChannel.send(eventBundle.encode());
            }

            if (haveRenderData) {
                geomStreamer.sendFrame(renderData);
            }

            auto now = std::chrono::steady_clock::now();
            if (now - lastReportTime > std::chrono::seconds(5)) {
                auto timestep = simulationFacade->getCurrentTimestep();
                auto tps = simulationFacade->getTps();
                uint64_t numCreatures = 0;
                for (auto const& lineage : statistics.lineageEntries) {
                    numCreatures += lineage.numCreatures;
                }
                std::cout << "timestep " << timestep << " (" << tps << " TPS), cells " << objects.numCellObjects << ", creatures " << numCreatures
                          << " in " << statistics.lineageEntries.size() << " lineages, cellsInView " << (haveRenderData ? renderData.cells.size() : 0)
                          << " fluidInView " << (haveRenderData ? renderData.fluidParticles.size() : 0) << ", attacks "
                          << (haveRenderData ? renderData.attackEvents.size() : 0) << ", geomPts " << geomStreamer.lastPointCount() << " geomLines "
                          << geomStreamer.lastLineCount() << std::endl;
                lastReportTimestep = timestep;
                lastReportTime = now;
            }
        }

        std::cout << "Stopping." << std::endl;
        simulationFacade->pauseSimulation();
        simulationFacade->closeSimulation();
    } catch (AlienException const& e) {
        log(Priority::Important, std::string("An exception occurred: ") + e.what());
        std::cerr << LoggingService::get().getLogString();
        return 1;
    } catch (std::exception const& e) {
        std::cerr << "An exception occurred: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
