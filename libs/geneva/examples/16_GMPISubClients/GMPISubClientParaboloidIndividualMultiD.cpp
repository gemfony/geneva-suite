/**
 * @file GMPISubClientParaboloidIndividualMultiD.cpp
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

#include "GMPISubClientParaboloidIndividualMultiD.hpp"

#include "geneva/oa/GAdaption.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "geneva/oa/GAdaptionConfig.hpp"

#include <algorithm>
#include <functional>
#include <ranges>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GMPISubClientParaboloidIndividualMultiD) // NOLINT
namespace Gem::Geneva {

// The MPI helper utilities (mpiSize / mpiErrorString / MPIStatusCode / MPICompletionStatus, …) now
// live in the Gem::Courtier namespace.
using namespace Gem::Courtier;

/********************************************************************************************/
/**
 * The default constructor. This function will add a specified number of double parameters to this individual,
 * each of which has a constrained value range [-10:10].
 */
GMPISubClientParaboloidIndividualMultiD::GMPISubClientParaboloidIndividualMultiD()
  : Gem::Common::GReflectiveInterfaceT<GMPISubClientParaboloidIndividualMultiD, GMPISubClientIndividual>()
  , M_PAR_MIN(-10.)
  , M_PAR_MAX(10.) {
    // Build a flat genome of nParameters_ constrained doubles in [M_PAR_MIN, M_PAR_MAX[, each its own
    // Gauss group adapted with the default GDoubleGaussAdaptor configuration.
    gen::GGenomeBuilder b;
    for(std::size_t npar = 0; npar < nParameters_; npar++) {
        b.addDouble(M_PAR_MIN, M_PAR_MIN, M_PAR_MAX); // structure only; the adaptor lives on the OA config
    }
    this->setGenome(b.build());

    // Per-parameter random initialization within bounds.
    this->randomInit(activityMode::ALLPARAMETERS);
}

/********************************************************************************************/
/**
 * A standard copy constructor. All real work is done by the parent class.
 *
 * @param cp A copy of another GMPISubClientParaboloidIndividualMultiD
 */
GMPISubClientParaboloidIndividualMultiD::GMPISubClientParaboloidIndividualMultiD(
    const GMPISubClientParaboloidIndividualMultiD &cp
)
  : Gem::Common::GReflectiveInterfaceT<GMPISubClientParaboloidIndividualMultiD, GMPISubClientIndividual>(cp)
  , M_PAR_MIN(-10.)
  , M_PAR_MAX(10) { /* nothing */
}

/********************************************************************************************/
/**
 * Builds the OA-owned adaption configuration: every parameter is its own Gauss group, configured with the
 * default GDoubleGaussAdaptor settings. The adaptor settings live solely on the returned (OA-owned)
 * config -- none reside on the individual.
 *
 * @param sample A sample flat genome whose group structure the config mirrors
 * @return A shared pointer to the populated OA-owned adaption config
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GMPISubClientParaboloidIndividualMultiD::buildAdaptionConfig(const gen::GGenome &sample) {
    auto cfg = OptimizationAlgorithms::makeAdaptionConfig<OptimizationAlgorithms::GAdaptionConfigBase>(sample);
    for(std::size_t npar = 0; npar < cfg->doubleGroups().size(); npar++) {
        cfg->groupDouble(npar).gauss(DEFAULTSIGMA, DEFAULTSIGMASIGMA, DEFAULTMINSIGMA, DEFAULTMAXSIGMA, DEFAULTADPROB);
    }
    return cfg;
}

/********************************************************************************************/
/**
 * The actual fitness calculation takes place here.
 *
 * @return The value of this object
 */
std::vector<double> GMPISubClientParaboloidIndividualMultiD::evaluate() {
    const uint32_t size{mpiSize(getCommunicator())}; // number of processes in the communicator
    double result{0.0};                              // Will hold the result
    std::vector<double> parVec;                      // Will hold the parameters

    // Retrieve the parameters
    this->streamline(parVec);

    // must create lValue binding
    std::optional<std::vector<double>> recvVecOpt{parVec};

    // distributed calculation of squares of individual parameters together with sub-clients
    MPICompletionStatus const status = distributedSolveWhile(
        parVec,
        recvVecOpt,
        nParameters_ / size,
        // clients stop themselves, and always run
        []() { return true; }
    );

    // Qualify the case labels below: MPIStatusCode and the (unscoped) ClientStatus both export an
    // ERROR enumerator, so a bare `case ERROR:` binds to ClientStatus::ERROR and mismatches the
    // MPIStatusCode switch condition (clang -Wenum-compare-switch).
    switch(status.statusCode) {
    case MPIStatusCode::ERROR:
        std::cerr << "MPI error occurred: " << '\n'
                  << mpiErrorString(status.mpiStatus.MPI_ERROR) << '\n';
        break;
    case MPIStatusCode::STOPPED:
        std::cerr
            << "Client executed evaluate() while being stopped. This is an internal error. "
               "Client should only be stopped after evaluate() has been finished."
            << '\n';
        break;
    case MPIStatusCode::SUCCESS: {
        // Calculate the sum of all individual results as the fitness value
        result = std::ranges::fold_left(recvVecOpt.value(), 0., std::plus{});
    } break;
    }

    return {result};
}

int GMPISubClientParaboloidIndividualMultiD::subClientJob(MPI_Comm _communicator) {
    const uint32_t size{mpiSize(getCommunicator())};
    std::optional<std::vector<double>> dummyRecvVec{};

    while(true) {
        MPICompletionStatus const status =
            distributedSolveWhile({}, dummyRecvVec, nParameters_ / size, []() {
                return GMPISubClientIndividual::getClientStatus() == ClientStatus::RUNNING;
            });

        if(status.statusCode == MPIStatusCode::STOPPED) {
            std::cout << "Sub-client will exit because the client sent a shutdown signal"
                      << '\n';
            return 0;
        }
        else if(status.statusCode ==
                MPIStatusCode::ERROR) { // operation was not successful and has not timed out
            std::cout << "Error occurred: " << '\n'
                      << mpiErrorString(status.mpiStatus.MPI_ERROR);
            return -1;
        }

        // client ist still running -> continue with next iteration
    }
}

MPICompletionStatus GMPISubClientParaboloidIndividualMultiD::distributedSolveWhile(
    const std::optional<std::vector<double>> &sendVec,
    std::optional<std::vector<double>> &resultVec,
    const std::uint32_t &parsPerProc,
    const std::function<bool()> &runWhile
) {
    // vector to store the received subset of parameters in
    std::vector<double> parameterSubset{};
    parameterSubset.resize(parsPerProc);

    // completion status to return
    MPICompletionStatus completionStatus{};

    // only the root process must pass a valid send address for scattering
    const double *scatterSendVecAddress{sendVec.has_value() ? sendVec.value().data() : nullptr};
    // only the root process must pass a valid send address for gathering
    double *gatherRecvVecAddress{resultVec.has_value() ? resultVec.value().data() : nullptr};

    // scatter data to all processes

    completionStatus = mpiScatterWhile(
        scatterSendVecAddress,
        parsPerProc,
        parameterSubset.data(),
        MPI_DOUBLE,
        runWhile,
        0,
        getCommunicator(),
        pollIntervalMSec_
    );

    // return early with the error status if not completed successfully
    if(completionStatus.statusCode == MPIStatusCode::ERROR) {
        return completionStatus;
    }

    // actual calculation on the range of parameters assigned to this process
    for(double &par : parameterSubset) {
        // simulate longer time for the calculation of one parameter
        std::this_thread::sleep_for(std::chrono::milliseconds(delayPerParameterMSec_));

        // square each parameter
        par = par * par;
    }

    // gather results from all processes
    completionStatus = mpiGatherWhile(
        parameterSubset.data(),
        parsPerProc,
        gatherRecvVecAddress,
        MPI_DOUBLE,
        runWhile,
        0,
        getCommunicator(),
        pollIntervalMSec_
    );

    return completionStatus;
}

} // namespace Gem::Geneva
