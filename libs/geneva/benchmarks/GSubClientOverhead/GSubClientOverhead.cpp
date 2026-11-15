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
// (GSpeedupMeasurement.hpp).
//
// How it is started:
//  - the serial reference:           GSubClientOverhead --reference
//  - MPI between server and clients: mpirun -np <1 + clients x group> GSubClientOverhead -c mpi
//    (the group size is sub_client_group_size in config/GMPISubClientOptimizer.json; rank 0 is
//    the server, the communicators are split by GMPISubClientOptimizer)
//  - websockets:                     GSubClientOverhead -c beast            (the server)
//    and per client                  mpirun -np <group> GSubClientOverhead -c beast --client
//                                        --group <group>
//    (rank 0 of each small job is the client and lead, the others its sub-clients; a group of
//    one is started without mpirun)

// Standard header files go here
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

// Boost header files go here
#include <boost/program_options.hpp>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"
#include "geneva/GMPISubClientIndividual.hpp"
#include "geneva/GMPISubClientOptimizer.hpp"
#include "geneva/Go2.hpp"

#include "GSleepGroup.hpp"
#include "GSleepGroupIndividual.hpp"
#include "GSpeedupMeasurement.hpp"

using namespace Gem::Geneva;
using Gem::Geneva::SubClientOverhead::GSleepGroupIndividual;

namespace {

/******************************************************************************/
/** What this process is, as far as it must be known before any Geneva object exists */
struct Role {
    std::string consumer;       ///< --consumer
    bool client = false;        ///< --client
    bool updateConfigs = false; ///< --update-configs
    unsigned int group = 1;     ///< --group: processes per group of a networked client
};

/**
 * Reads the few options that decide what this process is: an MPI job and a websocket client set
 * up MPI and their groups before Geneva's own objects, and a sub-client constructs none of them.
 * Go2 parses the same command line again.
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
    options
        .add_options()("consumer,c", value<std::string>(&role.consumer))("client", bool_switch(&role.client))("update-configs", bool_switch(&role.updateConfigs))(
            "group",
            value<unsigned int>(&role.group)
        );
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
/**
 * The options of the benchmark, for Go2's parse.
 *
 * @param reference_run Set by --reference
 * @param reference_file Set by --referenceFile
 * @param group Set by --group
 * @return The option descriptions
 */
boost::program_options::options_description
benchmarkOptions(bool &reference_run, std::string &reference_file, unsigned int &group) {
    boost::program_options::options_description options;
    options.add_options()(
        "reference",
        boost::program_options::value<bool>(&reference_run)->implicit_value(true)->default_value(false),
        "Measure the serial reference (on one local worker, in a group of one) and store its line "
        "in --referenceFile, instead of measuring the parallel times"
    )(
        "referenceFile",
        boost::program_options::value<std::string>(&reference_file)->default_value(reference_file),
        "The file that holds the line of the serial reference"
    )(
        "group",
        boost::program_options::value<unsigned int>(&group)->default_value(group),
        "A networked client: the processes of its group, started as one MPI job (1: no sub-clients, "
        "no MPI)"
    );
    return options;
}

/******************************************************************************/
/**
 * The submitting side: the serial reference, or the parallel measurement through the consumer of
 * @p go.
 *
 * @param go The optimization wrapper
 * @param reference_run Whether this is the reference run
 * @param reference_file The file of the reference line
 * @return The exit code
 */
int measure(Go2 &go, bool reference_run, const std::string &reference_file) {
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
    const auto &config = reference_run ? config_ref : config_par;
    SpeedupMeasurement::run(
        go,
        reference_run,
        reference_file,
        to_schedule(config_ref),
        to_schedule(config_par),
        [&config](const std::chrono::duration<double> &delay) -> std::shared_ptr<gen::GGenome> {
            return GSleepGroupIndividual::create(config, delay);
        },
        config_par.result_file,
        config_par.short_result_file
    );
    return 0;
}

/******************************************************************************/
/**
 * One MPI job for everything: rank 0 is the server, the others are clients (leads) and their
 * sub-clients, split by GMPISubClientOptimizer.
 */
int runMPIJob(
    int argc,
    char **argv,
    bool &reference_run,
    std::string &reference_file,
    unsigned int &group
) {
    GMPISubClientOptimizer go(
        argc,
        argv,
        "./config/GMPISubClientOptimizer.json",
        benchmarkOptions(reference_run, reference_file, group)
    );
    go.registerSubClientJob([](MPI_Comm sub_group) {
        return SubClientOverhead::serveGroup(sub_group);
    });
    if(not go.clientMode()) {
        return measure(go, reference_run, reference_file);
    }
    if(go.isSubClient()) {
        return go.clientRun(); // runs serveGroup() until the lead releases the group
    }
    SubClientOverhead::setProcessGroup(GMPISubClientIndividual::getCommunicator());
    const int status = go.clientRun();
    SubClientOverhead::releaseGroup(SubClientOverhead::processGroup());
    return status;
}

/******************************************************************************/
/**
 * A client of a networked consumer. In a group of more than one it is an MPI job of its own:
 * rank 0 is the client and lead, the other ranks serve it and construct no Geneva object.
 */
int runNetworkedClient(
    int argc,
    char **argv,
    const Role &role,
    bool &reference_run,
    std::string &reference_file,
    unsigned int &group
) {
    std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> environment;
    if(role.group > 1) {
        environment = Gem::Courtier::mpiEnvironment(); // initializes MPI
        int size = 0;
        int rank = 0;
        MPI_Comm_size(MPI_COMM_WORLD, &size);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        if(std::cmp_not_equal(size, role.group)) {
            throw std::runtime_error(
                "--group " + std::to_string(role.group) + " but the MPI job has " +
                std::to_string(size) + " processes"
            );
        }
        if(rank != 0) {
            return SubClientOverhead::serveGroup(MPI_COMM_WORLD);
        }
        SubClientOverhead::setProcessGroup(MPI_COMM_WORLD);
    }
    int status = 0;
    {
        Go2 go(
            argc,
            argv,
            "./config/Go2.json",
            benchmarkOptions(reference_run, reference_file, group)
        );
        status = go.clientRun();
    }
    SubClientOverhead::releaseGroup(SubClientOverhead::processGroup());
    return status;
}

} /* namespace */

/******************************************************************************/

// A failure ends the program with its message and a non-zero status instead of an abort.
int main(int argc, char **argv) try {
    const Role role = readRole(argc, argv);
    // Every process of a run reads the same file in the same directory: the pauses of a run are
    // the same in all of them, whatever the mode.
    const auto config = GSleepGroupIndividual::readConfig("./config/GSleepGroupIndividual.json");
    SubClientOverhead::setPollPauses(
        {.min = std::chrono::microseconds(config.poll_pause_min),
         .max = std::chrono::microseconds(config.poll_pause_max)}
    );
    bool reference_run = false;
    std::string reference_file = "referenceLine.txt";
    unsigned int group = 1;

    if(role.updateConfigs) {
        // Both configuration files of Go2 that the benchmark reads: the MPI job's (which adds the
        // group size) and the one of every other process. Go2's optimize() exits.
        {
            const GMPISubClientOptimizer mpi_job(
                argc,
                argv,
                "./config/GMPISubClientOptimizer.json",
                benchmarkOptions(reference_run, reference_file, group)
            );
        }
        Go2 go(
            argc,
            argv,
            "./config/Go2.json",
            benchmarkOptions(reference_run, reference_file, group)
        );
        return measure(go, reference_run, reference_file);
    }
    if(role.consumer == "mpi") {
        return runMPIJob(argc, argv, reference_run, reference_file, group);
    }
    if(role.client) {
        return runNetworkedClient(argc, argv, role, reference_run, reference_file, group);
    }
    Go2 go(argc, argv, "./config/Go2.json", benchmarkOptions(reference_run, reference_file, group));
    return measure(go, reference_run, reference_file);
}
catch(const std::exception &e) {
    std::cerr << "GSubClientOverhead: " << e.what() << '\n';
    return 1;
}
