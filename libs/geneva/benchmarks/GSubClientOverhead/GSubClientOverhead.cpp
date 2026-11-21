/**
 * @file GSubClientOverhead.cpp
 */

/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

// The speed-up of an optimization whose clients share the evaluation of an individual with
// sub-clients, which they address through MPI; between server and clients either MPI or a
// networked consumer such as the websocket one. With a group of one (no sub-clients) it measures
// what GParallelisationOverhead measures. Its individual, GSleepGroupIndividual, sleeps; the
// measurement, the serial reference and the results are those of GParallelisationOverhead
// (GSpeedupMeasurement.hpp). The groups are those of courtier's MPI cascade (GMPICascade): a
// client with sub-clients is the lead of its group and evaluates every item in a round of it.
//
// How it is started (n: the processes of a group, the lead included):
//  - the serial reference:           GSubClientOverhead --reference
//  - MPI between server and clients: mpirun -np <1 + clients x n> GSubClientOverhead -c mpi
//                                        --group n
//    (rank 0 is the server, then come the groups, each a lead and its sub-clients)
//  - websockets:                     GSubClientOverhead -c beast            (the server)
//    and per client                  mpirun -np n GSubClientOverhead -c beast --client --group n
//    (rank 0 of each small job is the client and lead, the others its sub-clients)
// A group of one is started without --group (or with --group 1) and has no cascade: the client
// evaluates alone, as GParallelisationOverhead's does; with websockets it is started without
// mpirun.

// Standard header files go here
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GLauncherT.hpp"
#include "courtier/mpi/GMPICascade.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "geneva/Go2.hpp"

#include "GSleepGroup.hpp"
#include "GSleepGroupIndividual.hpp"
#include "GSpeedupMeasurement.hpp"

using namespace Gem::Geneva;
using Gem::Courtier::GMPICascadeLayout;
using Gem::Courtier::processRole;
using Gem::Geneva::SubClientOverhead::GSleepGroupIndividual;

namespace {

/******************************************************************************/
/** What this process is, as far as it must be known before any Geneva object exists */
struct Role {
    std::string consumer;       ///< --consumer
    bool client = false;        ///< --client
    bool updateConfigs = false; ///< --update-configs
    unsigned int group = 1;     ///< --group: the processes of a client's group
};

/**
 * Reads the few options that decide what this process is: a process of a group constructs the
 * cascade before it runs the launcher, and a sub-client runs none. The launcher parses the same
 * command line again.
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @return The role
 */
Role readRole(int argc, char **argv) {
    Role role;
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter<std::string>("consumer,c", role.consumer, std::string());
    gpb.registerCLFlag("client", role.client);
    gpb.registerCLFlag("update-configs", role.updateConfigs);
    gpb.registerCLParameter<unsigned int>("group", role.group, role.group);
    gpb.parseKnownOptions(argc, argv);
    if(role.group == 0) {
        throw std::runtime_error("--group must be at least 1");
    }
    return role;
}

/******************************************************************************/
/** What the options of the benchmark set (the launcher's parse) */
struct Settings {
    bool referenceRun = false;                       ///< --reference
    std::string referenceFile = "referenceLine.txt"; ///< --referenceFile
    unsigned int group = Role{}.group;   ///< --group (read by readRole(); here for the help text)
    SpeedupMeasurement::ClientGate gate; ///< --waitForClients, --clientWaitLimit
};

/**
 * The options of the benchmark, in the launcher's parse.
 *
 * @param gpb The launcher's parser
 * @param settings Where the values go
 */
void benchmarkOptions(Gem::Common::GParserBuilder &gpb, Settings &settings) {
    gpb.registerCLParameter<bool>(
        "reference",
        settings.referenceRun,
        false,
        "Measure the serial reference (on one local worker, in a group of one) and store its line "
        "in --referenceFile, instead of measuring the parallel times",
        Gem::Common::GCL_IMPLICIT_ALLOWED,
        true
    );
    gpb.registerCLParameter<std::string>(
        "referenceFile",
        settings.referenceFile,
        settings.referenceFile,
        "The file that holds the line of the serial reference"
    );
    gpb.registerCLParameter<unsigned int>(
        "group",
        settings.group,
        settings.group,
        "The processes of a client's group, the client included (1: no sub-clients, no MPI "
        "cascade). With mpi the job has 1 + clients x group processes; a networked client is an "
        "MPI job of group processes"
    );
    SpeedupMeasurement::addClientGateOptions(gpb, settings.gate);
}

/******************************************************************************/
/**
 * The submitting side: the serial reference, or the parallel measurement through the consumer of
 * @p go.
 *
 * @param go The optimization wrapper
 * @param settings What the options set
 * @return The exit code
 */
int measure(Go2 &go, const Settings &settings) {
    const auto config_ref =
        GSleepGroupIndividual::readConfig("./config/GSleepGroupIndividual-reference.json");
    const auto config_par =
        GSleepGroupIndividual::readConfig("./config/GSleepGroupIndividual.json");
    go.registerDefaultAlgorithm("ea");
    if(Gem::Common::GParserBuilder::updateInPlace()) {
        go.optimize(); // refreshes Go2's owned configs, then ends the process
        return 0;
    }
    std::cout << "GSubClientOverhead: poll pauses " << config_par.poll_pause_min << " to "
              << config_par.poll_pause_max << " us, evaluations "
              << (config_par.split_evaluation ? "split over" : "repeated by") << " the group"
              << '\n';
    const auto to_schedule = [](const GSleepGroupIndividual::Config &c) {
        return SpeedupMeasurement::Schedule{
            .delays = GSleepGroupIndividual::evaluationTimes(c),
            .n_measurements = c.n_measurements,
            .inter_measurement_delay = c.inter_measurement_delay
        };
    };
    const auto &config = settings.referenceRun ? config_ref : config_par;
    SpeedupMeasurement::run(
        go,
        settings.referenceRun,
        settings.referenceFile,
        to_schedule(config_ref),
        to_schedule(config_par),
        [&config](const std::chrono::duration<double> &delay) -> std::shared_ptr<gen::GGenome> {
            return GSleepGroupIndividual::create(config, delay);
        },
        config_par.result_file,
        config_par.short_result_file,
        settings.gate
    );
    return 0;
}

/******************************************************************************/
/**
 * The configuration of the cascade, on base rank 0 only: first refuses a job that does not form
 * whole groups of @p group processes -- the cascade then refuses on every rank -- then reads the
 * poll pauses from the benchmark's own configuration file. The group size is --group, next to the
 * -np it must agree with; the cascade's configuration file is not read.
 *
 * @param group The processes of a group (--group)
 * @param layout The layout of the job
 * @return The configuration
 */
Gem::Courtier::GMPICascadeConfig cascadeConfig(unsigned int group, GMPICascadeLayout layout) {
    int size = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if(group > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(
            "--group " + std::to_string(group) + " is more than a group of the MPI cascade can have"
        );
    }
    if(layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS and
       (size < 2 or (static_cast<unsigned int>(size) - 1) % group != 0)) {
        throw std::runtime_error(
            "the MPI job has " + std::to_string(size) + " processes, but with -c mpi and --group " +
            std::to_string(group) + " it needs 1 + clients x " + std::to_string(group)
        );
    }
    if(layout == GMPICascadeLayout::GROUPS_ONLY and std::cmp_not_equal(size, group)) {
        throw std::runtime_error(
            "the MPI job has " + std::to_string(size) + " processes, but a client with --group " +
            std::to_string(group) + " is one job of " + std::to_string(group)
        );
    }
    const auto config = GSleepGroupIndividual::readConfig("./config/GSleepGroupIndividual.json");
    return Gem::Courtier::GMPICascadeConfig{
        .groupSize = static_cast<std::uint16_t>(group),
        .layout = layout,
        .pauseMin = std::chrono::microseconds(config.poll_pause_min),
        .pauseMax = std::chrono::microseconds(config.poll_pause_max)
    };
}

/******************************************************************************/
/**
 * Runs the benchmark on courtier's launcher, with Geneva's description: the submitter measures,
 * a client evaluates. A lead of a cascade gives the launcher its item processor before.
 *
 * @param launcher The launcher
 * @param geneva Geneva's description
 * @param settings Where the launcher's parse puts the benchmark's options
 * @param prepare What every process does once its role is known (nothing if empty)
 * @return The exit code
 */
int runOnLauncher(
    Gem::Courtier::GLauncherT<gen::GGenome> &launcher,
    GOptimizationDescription &geneva,
    Settings &settings,
    std::move_only_function<std::optional<int>(processRole)> prepare = {}
) {
    return launcher.run(
        geneva,
        {.userOptions =
             [&settings](Gem::Common::GParserBuilder &gpb) { benchmarkOptions(gpb, settings); },
         .prepare = std::move(prepare),
         .produce =
             [&geneva, &settings] {
                 Go2 go(geneva);
                 return measure(go, settings);
             }}
    );
}

/******************************************************************************/
/**
 * A process of a job that is laid out in groups (GMPICascade): a member serves its lead; the
 * submitter (only with -c mpi) measures through Go2; a lead runs its client and evaluates every
 * item in a round of its group. With -c mpi the job is everything (the layout
 * SUBMITTER_AND_GROUPS); a networked client is a job of its own (GROUPS_ONLY).
 *
 * @param launcher The launcher, not run yet
 * @param geneva Geneva's description
 * @param role What readRole() found
 * @param settings Where the launcher's parse puts the benchmark's options (the submitter)
 * @param layout The layout of the job
 * @return The exit code
 */
int runGroupProcess(
    Gem::Courtier::GLauncherT<gen::GGenome> &launcher,
    GOptimizationDescription &geneva,
    const Role &role,
    Settings &settings,
    GMPICascadeLayout layout
) {
    // Declared before the cascade, so that they outlive it
    SubClientOverhead::GSleepGroupMember member;
    SubClientOverhead::GSleepGroupLeadLog lead_log;
    Gem::Courtier::GMPICascade cascade([&role, layout] {
        return cascadeConfig(role.group, layout);
    });
    const auto pause_min = cascade.config().pauseMin;
    const auto pause_max = cascade.config().pauseMax;

    if(cascade.role() == processRole::BYSTANDER) {
        member.setPauses(pause_min, pause_max);
        return cascade.serve(member);
    }

    // The submitter and the leads. An error on the submitter (an option only it knows, refused
    // there alone; a missing serial reference) is caught by nobody: the fatal report prints it and
    // ends this process, and with it the job, whose leads wait for this process's greeting.
    // A lead's client evaluates every item in a round of the group; process() keeps the item's
    // lifecycle (status, timing, an exception flags the item).
    launcher.registerItemProcessor([&cascade, &lead_log, pause_min, pause_max](gen::GGenome &item) {
        std::ignore = item.process([&](const gen::GGenome &candidate) -> std::vector<double> {
            const auto &individual = dynamic_cast<const GSleepGroupIndividual &>(candidate);
            std::vector<double> parameters;
            individual.streamline(parameters);
            const double fitness = cascade.round([&](MPI_Comm group) {
                return SubClientOverhead::leadHalf(
                    group,
                    std::chrono::duration<double>(individual.evaluationTime()),
                    individual.split(),
                    parameters,
                    pause_min,
                    pause_max
                );
            });
            lead_log.countRound();
            return {fitness};
        });
    });
    const int rc = runOnLauncher(launcher, geneva, settings, [&](processRole consumer_role) {
        cascade.requireRole(consumer_role); // the consumer's role must be the cascade's
        if(consumer_role == processRole::CLIENT) {
            lead_log.setPauses(pause_min, pause_max);
            cascade.lead(lead_log);
        }
        return std::optional<int>{};
    });
    if(cascade.role() == processRole::CLIENT) { // also if the launcher ended before its role
        cascade.release();                      // prints the lead's line (lead_log's tearDown())
    }
    return rc;
}

} /* namespace */

/******************************************************************************/

int main(int argc, char **argv) {
    const Role role = readRole(argc, argv);
    Settings settings;
    // Built before a cascade, so that a request for the help text is answered before the cascade
    // would start MPI
    GOptimizationDescription geneva;
    Gem::Courtier::GLauncherT<gen::GGenome> launcher(argc, argv, "./config/Go2.json");
    if(not role.updateConfigs and not launcher.helpRequested() and role.group > 1) {
        if(role.consumer == "mpi") {
            return runGroupProcess(
                launcher,
                geneva,
                role,
                settings,
                GMPICascadeLayout::SUBMITTER_AND_GROUPS
            );
        }
        if(role.client) {
            return runGroupProcess(
                launcher,
                geneva,
                role,
                settings,
                GMPICascadeLayout::GROUPS_ONLY
            );
        }
    }
    // Everything without a cascade: the serial reference, a websocket server, a group of one,
    // --update-configs, a help request
    return runOnLauncher(launcher, geneva, settings);
}
