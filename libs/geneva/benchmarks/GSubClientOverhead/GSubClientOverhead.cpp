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
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Boost header files go here
#include <boost/program_options.hpp>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "common/GFatalReport.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GConsumerSpec.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/mpi/GMPICascade.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/Go2.hpp"
#include "hap/GRandomFactory.hpp"

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
 * cascade before any Geneva object, and a sub-client constructs none of them. Go2 parses the same
 * command line again.
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @return The role
 */
Role readRole(int argc, char **argv) {
    using boost::program_options::bool_switch;
    using boost::program_options::value;
    Role role;
    boost::program_options::options_description options;
    auto add = options.add_options();
    add("consumer,c", value<std::string>(&role.consumer));
    add("client", bool_switch(&role.client));
    add("update-configs", bool_switch(&role.updateConfigs));
    add("group", value<unsigned int>(&role.group));
    boost::program_options::variables_map vm;
    boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv)
            .options(options)
            .allow_unregistered()
            .run(),
        vm
    );
    boost::program_options::notify(vm);
    if(role.group == 0) {
        throw std::runtime_error("--group must be at least 1");
    }
    return role;
}

/******************************************************************************/
/** What the options of the benchmark set (Go2's parse) */
struct Settings {
    bool referenceRun = false;                       ///< --reference
    std::string referenceFile = "referenceLine.txt"; ///< --referenceFile
    SpeedupMeasurement::ClientGate gate;             ///< --waitForClients, --clientWaitLimit
};

/**
 * The options of the benchmark, for Go2's parse.
 *
 * @param settings Where the values go
 * @return The option descriptions
 */
boost::program_options::options_description benchmarkOptions(Settings &settings) {
    boost::program_options::options_description options;
    options.add_options()(
        "reference",
        boost::program_options::value<bool>(&settings.referenceRun)
            ->implicit_value(true)
            ->default_value(false),
        "Measure the serial reference (on one local worker, in a group of one) and store its line "
        "in --referenceFile, instead of measuring the parallel times"
    )(
        "referenceFile",
        boost::program_options::value<std::string>(&settings.referenceFile)
            ->default_value(settings.referenceFile),
        "The file that holds the line of the serial reference"
    )(
        // Read by readRole() (Role::group); registered here for Go2's parse and help
        "group",
        boost::program_options::value<unsigned int>()->default_value(Role{}.group),
        "The processes of a client's group, the client included (1: no sub-clients, no MPI "
        "cascade). With mpi the job has 1 + clients x group processes; a networked client is an "
        "MPI job of group processes"
    );
    SpeedupMeasurement::addClientGateOptions(options, settings.gate);
    return options;
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
    if(go.updateConfigsMode()) {
        go.optimize(); // refreshes Go2's owned configs, then exits
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
/** @brief What the consumer of the command line gives a lead: its role and, for a CLIENT, its
 *  client. */
struct LeadClient {
    processRole role; ///< The consumer's role of this process
    /// The client; null unless the role is CLIENT
    std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>> client;
};

/**
 * The one place that builds a lead's client, from the consumer options of the command line,
 * through Geneva's consumer setup (as Go2 does for a client, without Go2). Called on a lead only,
 * after the cascade exists: on a member it would load the consumers and parse their options, and
 * a member runs no Geneva code.
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @param role What readRole() found
 * @return The consumer's role of this process and, for a CLIENT, its client
 */
LeadClient buildLeadClient(int argc, char **argv, const Role &role) {
    Gem::Hap::setGpuRandomNumbersAllowed(false); // as Go2 for a client: no CUDA context per lead
    boost::program_options::options_description options;
    boost::program_options::options_description hidden;
    addConsumerOptions(options, hidden);
    boost::program_options::variables_map vm;
    boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv)
            .options(options.add(hidden))
            .allow_unregistered()
            .run(),
        vm
    );
    const Gem::Courtier::ConsumerSpec spec = specFromCommandLine(role.consumer, vm);
    const processRole consumer_role = consumerRole(spec, role.client);
    return {
        .role = consumer_role,
        .client = consumer_role == processRole::CLIENT ? buildConsumerClient(spec) : nullptr
    };
}

/******************************************************************************/
/**
 * A process of a job that is laid out in groups (GMPICascade): a member serves its lead; the
 * submitter (only with -c mpi) measures through Go2; a lead runs its client and evaluates every
 * item in a round of its group. With -c mpi the job is everything (the layout
 * SUBMITTER_AND_GROUPS); a networked client is a job of its own (GROUPS_ONLY).
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @param role What readRole() found
 * @param settings Where Go2's parse puts the benchmark's options (the submitter)
 * @param layout The layout of the job
 * @return The exit code
 */
int runGroupProcess(
    int argc,
    char **argv,
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
    if(cascade.role() == processRole::SUBMITTER) {
        // An error here (an option only Go2 knows, refused here alone; a missing serial
        // reference) is caught by nobody: the fatal report prints it and ends this process, and
        // with it the job, whose leads wait for this process's greeting.
        cascade.requireRole(consumerRole(Gem::Courtier::ConsumerSpec{.mnemonic = role.consumer}));
        Go2 go(argc, argv, "./config/Go2.json", benchmarkOptions(settings));
        return measure(go, settings);
    }

    // A lead. The client is declared after the cascade: it goes before the cascade's communicators.
    const auto [consumer_role, client] = buildLeadClient(argc, argv, role);
    cascade.requireRole(consumer_role);
    lead_log.setPauses(pause_min, pause_max);
    cascade.lead(lead_log);
    // Every item is evaluated in a round of the group; process() keeps the item's lifecycle
    // (status, timing, an exception flags the item).
    client->setItemProcessor([&cascade, &lead_log, pause_min, pause_max](gen::GGenome &item) {
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
    client->run();
    cascade.release(); // prints the lead's line (lead_log's tearDown())
    return 0;
}

} /* namespace */

/******************************************************************************/

int main(int argc, char **argv) {
    // An error nobody handles ends the program with a message and the exit code 1
    Gem::Common::installFatalReport();
    const Role role = readRole(argc, argv);
    Settings settings;
    if(not role.updateConfigs and role.group > 1) {
        if(role.consumer == "mpi") {
            return runGroupProcess(
                argc,
                argv,
                role,
                settings,
                GMPICascadeLayout::SUBMITTER_AND_GROUPS
            );
        }
        if(role.client) {
            return runGroupProcess(argc, argv, role, settings, GMPICascadeLayout::GROUPS_ONLY);
        }
    }
    // Everything without a cascade: the serial reference, a websocket server, a group of one,
    // --update-configs
    Go2 go(argc, argv, "./config/Go2.json", benchmarkOptions(settings));
    if(not role.updateConfigs and go.clientMode()) {
        return go.clientRun();
    }
    return measure(go, settings);
}
