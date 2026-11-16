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

#include "geneva/GMPISubClientOptimizer.hpp"

#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"
#include "geneva/GMPISubClientIndividual.hpp"
#include "geneva/Go2.hpp"
#include "geneva/genome/GGenome.hpp"
#include <boost/program_options.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace Gem::Geneva {
/**
 * @brief Constructs the optimizer, sets up the MPI sub-client topology and splits communicators.
 *
 * Verifies that the MPI consumer is in use, re-parses the configuration file to pick up this
 * class's own options, and then obtains the process's MPI environment
 * (Gem::Courtier::mpiEnvironmentOn()), which initializes MPI unless the program did. Once MPI is
 * initialized, the environment lets this constructor partition the processes into the geneva
 * server, geneva clients and sub-clients; the communicator of the server and the clients becomes
 * the environment's base (a sub-client has none and is a bystander), the sub-client communicator
 * is handed to GMPISubClientIndividual.
 *
 * @param argc Argument count forwarded to the Go2 base class for command-line parsing.
 * @param argv Argument vector forwarded to the Go2 base class for command-line parsing.
 * @param configFilePath Path to the JSON configuration file, parsed both by Go2 and again here for this class's options.
 * @param userDescriptions Additional user-defined command-line option descriptions forwarded to Go2.
 * @param baseCommunicator The outermost MPI communicator to partition. MPI is initialized here
 *        unless the program has done so; a communicator other than MPI_COMM_WORLD implies that
 *        it has.
 */
// One coherent MPI topology setup: rank/color computation and the communicator splits are tightly
// sequential and depend on each other's results (is_server, isSubClient_, sub_comm_color);
// splitting would scatter interdependent MPI bootstrap steps.
// NOLINTNEXTLINE(readability-function-size)
GMPISubClientOptimizer::GMPISubClientOptimizer(
    int argc,
    char **argv,
    std::string const &configFilePath,
    boost::program_options::options_description const &userDescriptions,
    MPI_Comm baseCommunicator
)
  : Go2{argc, argv, configFilePath, userDescriptions} {
    // A pure --update-configs run only refreshes configuration files and exits without optimizing, and
    // Go2 forces the local thread-pool consumer for it -- so the MPI-consumer requirement below does not
    // apply, and none of the MPI setup further down must run (there is no MPI environment).
    if(not updateConfigsMode() && Go2::getConsumerName() != "mpi") { // only allow using MPI
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "GMPISubClientOptimizer constructor Error!" << '\n'
            << "GMPISubClientOptimizer may only be used with the GMPIConsumerT, but the consumer "
               "is `"
            << Go2::getConsumerName() << "`" << '\n'
            << "Set the consumer to GMPIConsumerT by using the command line argument `--consumer "
               "mpi`"
            << '\n'
        );
    }

    // Go2 already parses the config file but does not know the options needed by this class.
    // Therefore, we parse it again here and override the results of parsing the first time
    // When parsing here, the addConfigurationOptions_ of this class is used and options of this class are added.
    // TODO: Pass a callback to Go2-constructor which adds additional config-file options, in order to not parse twice and remove this call here
    parseConfigFile(configFilePath);

    // --update-configs: this class's own configuration has now been refreshed (create-if-absent /
    // rewrite via the parse above). Skip all MPI initialization -- Go2::optimize() will refresh the
    // remaining owned configs and exit without running.
    if(updateConfigsMode()) {
        return;
    }

    // The environment initializes MPI (unless the program did) before it calls the callable, which
    // the rank arithmetic and the splits below need. The communicator of the server and the geneva
    // clients becomes courtier's base; a sub-client gets MPI_COMM_NULL from its split and is a
    // bystander.
    environment_ = Gem::Courtier::mpiEnvironmentOn([&]() -> MPI_Comm {
        // initialize position in MPI world e.g. in the outermost communicator
        MPI_Comm_rank(baseCommunicator, &baseCommRank_);
        MPI_Comm_size(baseCommunicator, &baseCommSize_);

        // server has rank 0
        const bool is_server = baseCommRank_ == 0;

        // All processes but the geneva server and the geneva clients are sub-clients.
        // As an example: In case of 17 processes with one server, 4 clients and 4 sub-clients the
        // ranks [0, 1, 5, 7, 13] are server and geneva clients. All other processes will be
        // sub-clients.
        isSubClient_ = !is_server && ((baseCommRank_ - 1) % subClientGroupSize_ != 0);
        const int sub_comm_color =
            ((baseCommRank_ - 1) / (subClientGroupSize_)) + M_MPI_GENEVA_COLOR;

        // emit output about this instance
        if(!is_server) { // the server is in no sub-client group
            glogger << "baseRank=" << baseCommRank_
                    << " with mode=" << (isSubClient() ? "`sub-client`" : "`client`")
                    << " is in subgroup " << sub_comm_color << '\n'
                    << GLOGGING;
        }
        else {
            glogger << "baseRank=" << baseCommRank_ << " is the server and in no sub-group" << '\n'
                    << GLOGGING;
        }

        // create new communicators based on the current position in the baseRank
        if(isSubClient_) { // process is a sub-client

            // create the genevaCommunicator, because the call is collective.
            // But as this process is a sub-client we do not want to communicate inside that
            // subgroup and therefore pass MPI_UNDEFINED as color
            MPI_Comm_split(baseCommunicator, MPI_UNDEFINED, baseCommRank_, &genevaComm_);

            // putting nSubClients in one communicator by using down-rounding integer division
            MPI_Comm_split(baseCommunicator, sub_comm_color, baseCommRank_, &subClientComm_);
        }
        else { // process is a geneva client or the geneva server

            // Create a communicator to be used by GMPIConsumerT
            MPI_Comm_split(baseCommunicator, M_MPI_GENEVA_COLOR, baseCommRank_, &genevaComm_);

            if(is_server) { // process is the geneva server (master node)
                // The geneva server has rank 0 and does not need to be in any sub-client
                // communicator. It only communicates with the geneva clients and does not
                // communicate inside of subgroups.
                MPI_Comm_split(baseCommunicator, MPI_UNDEFINED, baseCommRank_, &subClientComm_);
            }
            else {
                // all geneva clients are in separate communicators by using down-rounding integer
                // division. This process (geneva client) receives rank 0 inside the subgroup. All
                // other processes in this subgroup will definitely pass numbers greater 0 as key
                // and therefore get the ranks 1 - (subClientGroupSize_ - 1)
                MPI_Comm_split(baseCommunicator, sub_comm_color, 0, &subClientComm_);
            }
        }

        return genevaComm_;
    });

    // Notify the individual to use this inter-communicator
    GMPISubClientIndividual::setCommunicator(subClientComm_);

    // set sub-client group status communicator
    if(baseCommRank_ != 0) { // the server is in no sub-client group
        // create the status communicator as a copy of the local group communicator
        MPI_Comm_dup(subClientComm_, &subClientStatusComm_);
    }
}

/**
 * @brief The destructor: releases the consumer, then the MPI environment.
 *
 * Ordering matters twice over. The consumer must be torn down BEFORE MPI is finalized (the master
 * node's shutdown still sends the stop signal to the worker ranks over MPI), but the base-class
 * destructor would only release it after this body has run -- hence the explicit releaseConsumer_()
 * here (the base destructor's own call then finds nothing left to do). Releasing the environment
 * then finalizes MPI if the environment initialized it and this was its last holder. And every rank
 * must actually reach MPI_Finalize: a rank that simply exits makes mpirun/hydra declare the job
 * abnormally terminated and SIGKILL the remaining ranks -- which used to hit the sub-clients, still
 * waiting for their group's shutdown barrier, on every run of the sub-client example.
 */
GMPISubClientOptimizer::~GMPISubClientOptimizer() {
    this->releaseConsumer_();
    environment_.reset();
}

/**
 * @brief Adds this class's configuration-file options on top of those of the Go2 base class.
 *
 * Registers the "sub_client_group_size" file parameter that controls how many processes cooperate
 * on a single individual.
 *
 * @param gpb The parser builder to which the configuration options are registered.
 */
void GMPISubClientOptimizer::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // let parent class `Go2` add its options first
    Go2::addConfigurationOptions_(gpb);

    // add additional options specific to this class

    gpb.registerFileParameter<std::uint16_t>(
        "sub_client_group_size",
        subClientGroupSize_,
        subClientGroupSize_
    ) << "The amount of processes in each sub-group. Each sub group works together on one "
         "individual.";
}

/**
 * @brief Registers the callback that each sub-client executes for its share of an individual's work.
 *
 * @param callback The job to run on a sub-client; it receives the sub-client group communicator and returns an exit code.
 * @return A reference to this optimizer, to allow call chaining.
 */
GMPISubClientOptimizer &
GMPISubClientOptimizer::registerSubClientJob(std::move_only_function<int(MPI_Comm)> callback) {
    subClientJob_ = std::move(callback);

    // return reference to self for chaining calls
    return *this;
}

/**
 * @brief Starts a non-blocking barrier on the sub-client status communicator.
 *
 * The returned request is used to signal across the sub-client group when the geneva client has
 * finished optimization.
 *
 * @return The MPI request handle for the freshly started non-blocking barrier.
 */
MPI_Request GMPISubClientOptimizer::startAsyncBarrier() const {
    MPI_Request request{};
    MPI_Ibarrier(subClientStatusComm_, &request);

    return request;
}

/**
 * @brief Runs the per-process client loop, dispatching by sub-client versus geneva-client role.
 *
 * A sub-client marks itself as such, arms the asynchronous barrier used to detect completion and
 * executes the registered sub-client job. A geneva client runs the normal Go2 client loop until
 * optimization finishes and then trips the barrier to notify its sub-clients.
 *
 * @return The exit code: the sub-client job's return value for sub-clients, or the Go2 client run's return value for geneva clients.
 */
int GMPISubClientOptimizer::clientRun_() {
    if(isSubClient_) {
        GMPISubClientIndividual::setClientMode(ClientMode::SUB_CLIENT);
        GMPISubClientIndividual::setClientStatusRequest(startAsyncBarrier());
        // start the asynchronous request waiting for the client to finish its job
        // execute the sub-client job
        return subClientJob_(subClientComm_);
    }
            GMPISubClientIndividual::setClientMode(ClientMode::CLIENT);
        // run the client until optimization finished
        int const returnValue{Go2::clientRun_()};
        // Tell the sub-clients that the optimization has finished, and wait for the barrier to
        // complete (every sub-client entered it at startup, so this returns promptly) -- the wait
        // both frees the request and guarantees no communication is pending on this rank when the
        // destructor finalizes MPI.
        MPI_Request stopRequest{startAsyncBarrier()};
        MPI_Wait(&stopRequest, MPI_STATUS_IGNORE);
        // return value
        return returnValue;

}

} /* namespace Gem::Geneva */
