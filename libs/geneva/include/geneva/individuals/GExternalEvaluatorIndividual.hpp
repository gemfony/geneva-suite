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

#pragma once

// Standard header files go here
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

// Boost header files go here
#include <boost/json.hpp>

// Geneva header files go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeMultiConstraint.hpp"
#include "hap/GRandomT.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {
class GAdaptionConfigBase;
} // namespace Gem::Geneva::OptimizationAlgorithms

namespace Gem::Geneva::Individuals {

/******************************************************************************/
// A number of default settings for the factory
constexpr double GEEI_DEF_ADPROB = 1.0;
constexpr double GEEI_DEF_ADAPTADPROB = 0.1;
constexpr double GEEI_DEF_MINADPROB = 0.05;

constexpr double GEEI_DEF_MAXADPROB = 1.;
constexpr std::uint32_t GEEI_DEF_ADAPTIONTHRESHOLD = 1;
constexpr bool GEEI_DEF_USEBIGAUSSIAN = false;
constexpr double GEEI_DEF_SIGMA1 = 0.025;
constexpr double GEEI_DEF_SIGMASIGMA1 = 0.2;
constexpr double GEEI_DEF_MINSIGMA1 = 0.001;
constexpr double GEEI_DEF_MAXSIGMA1 = 1;
constexpr double GEEI_DEF_SIGMA2 = 0.025;
constexpr double GEEI_DEF_SIGMASIGMA2 = 0.2;
constexpr double GEEI_DEF_MINSIGMA2 = 0.001;
constexpr double GEEI_DEF_MAXSIGMA2 = 1;
constexpr double GEEI_DEF_DELTA = 0.2;
constexpr double GEEI_DEF_SIGMADELTA = 0.2;
constexpr double GEEI_DEF_MINDELTA = 0.001;
constexpr double GEEI_DEF_MAXDELTA = 1.;
constexpr std::size_t GEEI_DEF_PARDIM = 2;
constexpr double GEEI_DEF_MINVAR = -10.;
constexpr double GEEI_DEF_MAXVAR = 10.;
constexpr bool GEEI_DEF_USECONSTRAINEDDOUBLECOLLECTION = false;
const std::string GEEI_DEF_PROGNAME = "./evaluator/evaluator.py";
const std::string GEEI_DEF_CUSTOMOPTIONS = "empty";
const std::string GEEI_DEF_PARFILEBASENAME = "parameter_file";
constexpr std::size_t GEEI_DEF_NRESULTS = 1;
const std::string GEEI_DEF_STARTMODE = "random";
const std::string GEEI_DEF_DATATYPE = "setup_data";
const std::string GEEI_DEF_RUNID = "empty";
constexpr bool GEEI_DEF_REMOVETEMPORARIES = true;


/******************************************************************************/
/**
 * This individual calls an external program to evaluate a given set of parameters.
 * Data exchange happens partially through the GNumericParameterT class. The
 * structure of the individual is determined from information given by the external
 * program. Currently double-, bool- and std::int32_t values are supported.
 *
 * External programs should understand at least the following command line
 * arguments with obvious meanings
 *
 * --init
 * --setup --init_values=[min/max/random] --output="setupFile.json"
 * --evaluate --input="paramsFile.json"   --output="result_file.json"
 * --archive  --input="archiveFile.json"
 * --finalize
 *
 * The parameter / result / setup files exchanged with the external program are JSON documents
 * (see the shipped evaluator.py reference implementation for the schema). Hence the external
 * program needs to read and write this JSON format.
 */
class GExternalEvaluatorIndividual
  : public gen::GGenomeT<GExternalEvaluatorIndividual>
  , public Gem::Common::GSelfTestable { // NOLINT(cppcoreguidelines-special-member-functions)
    ///////////////////////////////////////////////////////////////////////

    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin
    // reach the private localMembers_() below (from which serialize/load_/compare_/clone_/name_ derive).
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief Single declaration of this class'es local data members
     * @return A tuple of named member references driving the generated serialize()/load_()/compare_()
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("program_name_", self.program_name_),
            Gem::Common::make_member("custom_options_", self.custom_options_),
            Gem::Common::make_member("parameter_file_base_name_", self.parameter_file_base_name_),
            Gem::Common::make_member("n_results_", self.n_results_),
            Gem::Common::make_member("run_id_", self.run_id_),
            Gem::Common::make_member("remove_exec_temporaries_", self.remove_exec_temporaries_)
        );
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and compare token. */
    static constexpr std::string_view class_name = "GExternalEvaluatorIndividual";

    /***************************************************************************/
    using FACTORYTYPE =
        Gem::Geneva::Genome::GIndividualFactory<GExternalEvaluatorIndividual>;

    /** @brief The default constructor */
    GExternalEvaluatorIndividual();
    /**
     * @brief A standard copy constructor
     * @param cp The other GExternalEvaluatorIndividual object whose data is copied
     */
    GExternalEvaluatorIndividual(const GExternalEvaluatorIndividual &cp);

    /** @brief The standard destructor */
    ~GExternalEvaluatorIndividual() override;

    /**
     * @brief Sets the name of the external evaluation program
     * @param program_name The path / name of the external program to be executed
     */
    void setProgramName(const std::string &program_name);
    /**
     * @brief Retrieves the name of the external evaluation program
     * @return The name of the external evaluation program
     */
    std::string getProgramName() const;

    /**
     * @brief Sets any custom options that need to be passed to the external evaluation program
     * @param custom_options The custom command-line options forwarded to the external program
     */
    void setCustomOptions(const std::string &custom_options);
    /**
     * @brief Retrieves any custom options that need to be passed to the external evaluation program
     * @return The custom options string
     */
    std::string getCustomOptions() const;

    /**
     * @brief Sets the base name of the data exchange file
     * @param parameter_file The base name used for the XML parameter exchange files
     */
    void setExchangeBaseName(const std::string &parameter_file);
    /**
     * @brief Retrieves the current value of the parameter_file_base_name_ variable
     * @return The base name of the data exchange file
     */
    std::string getExchangeBaseName() const;

    /**
     * @brief Sets the number of results to be expected from the external evaluation program
     * @param n_results The number of fitness results expected from each evaluation
     */
    void setNExpectedResults(const std::size_t &n_results);
    /**
     * @brief Retrieves the number of results to be expected from the external evaluation program
     * @return The number of expected results
     */
    std::size_t getNExpectedResults() const;

    /**
     * @brief Allows to set the data type of this individual
     * @param dataType The data-type tag forwarded to the external program in the exchange file
     */
    void setDataType(std::string);
    /**
     * @brief Allows to retrieve the data type of this individual
     * @return The data-type tag of this individual
     */
    std::string getDataType() const;

    /**
     * @brief Allows to assign a run-id to this individual
     * @param run_id The unique identifier for this optimization run
     */
    void setRunId(const std::string& run_id);
    /**
     * @brief Allows to retrieve the run-id assigned to this individual
     * @return The run-id assigned to this individual
     */
    std::string getRunId() const;

    /**
     * @brief Allows to specify whether temporary files should be removed
     * @param remove_exec_temporaries Whether the temporary exchange files should be deleted after use
     */
    void setRemoveExecTemporaries(bool remove_exec_temporaries);
    /**
     * @brief Allows to check whether temporaries should be removed
     * @return true if temporary files are removed, false otherwise
     */
    bool getRemoveExecTemporaries() const;

    /***************************************************************************/
    /**
     * The configuration read from the config file by GIndividualFactory<GExternalEvaluatorIndividual>.
     * Besides the Gauss / bi-Gauss adaptor settings and the external-program parameters, two fields
     * (run_id, n_results_expected) are not parsed from the file but DISCOVERED by buildGenome() when it
     * queries the external evaluator; buildGenome writes them back so applyConfig() can hand them to each
     * produced individual.
     */
    struct Config {
        double ad_prob = GEEI_DEF_ADPROB;
        double adapt_ad_prob = GEEI_DEF_ADAPTADPROB;
        double min_ad_prob = GEEI_DEF_MINADPROB;
        double max_ad_prob = GEEI_DEF_MAXADPROB;
        std::uint32_t adaption_threshold = GEEI_DEF_ADAPTIONTHRESHOLD;
        bool use_bi_gaussian = GEEI_DEF_USEBIGAUSSIAN;
        double sigma1 = GEEI_DEF_SIGMA1;
        double sigma_sigma1 = GEEI_DEF_SIGMASIGMA1;
        double min_sigma1 = GEEI_DEF_MINSIGMA1;
        double max_sigma1 = GEEI_DEF_MAXSIGMA1;
        double sigma2 = GEEI_DEF_SIGMA2;
        double sigma_sigma2 = GEEI_DEF_SIGMASIGMA2;
        double min_sigma2 = GEEI_DEF_MINSIGMA2;
        double max_sigma2 = GEEI_DEF_MAXSIGMA2;
        double delta = GEEI_DEF_DELTA;
        double sigma_delta = GEEI_DEF_SIGMADELTA;
        double min_delta = GEEI_DEF_MINDELTA;
        double max_delta = GEEI_DEF_MAXDELTA;
        std::string program_name = GEEI_DEF_PROGNAME;
        std::string custom_options = GEEI_DEF_CUSTOMOPTIONS;
        std::string parameter_file_base_name = GEEI_DEF_PARFILEBASENAME;
        std::string init_values = GEEI_DEF_STARTMODE;
        bool remove_exec_temporaries = GEEI_DEF_REMOVETEMPORARIES;
        // Discovered by buildGenome() from the external evaluator's setup output (not parsed from file):
        std::string run_id = GEEI_DEF_RUNID;
        std::size_t n_results_expected = GEEI_DEF_NRESULTS;
    };

    /**
     * @brief Registers the config-file options, binding them to the passed Config
     * @param gpb The parser builder the configuration options are registered with
     * @param c The Config object whose members the options are bound to
     */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c);
    /**
     * @brief Queries the external evaluator for the individual structure and builds the flat genome;
     *  also records the discovered run_id / n_results_expected back into @p c.
     * @param c The Config supplying the external-program settings; updated in place with the
     *          discovered run_id and n_results_expected
     * @return The flat genome data describing the individual's parameter structure
     */
    static gen::GenomeData buildGenome(Config &c);
    /**
     * @brief The OA-owned adaption config: the configured Gauss/bi-Gauss adaptor on every ACTIVE group
     * @param sample A sample flat genome whose group structure the adaption config is built for
     * @param c The Config supplying the Gauss / bi-Gauss adaptor settings
     * @return A shared pointer to the constructed adaption config
     */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const gen::GGenome &sample, const Config &c);
    /**
     * @brief Per-object post-config hook: applies the external-program parameters + discovered metadata
     * @param ind The individual that the configuration is applied to
     * @param c The Config supplying the external-program parameters and discovered metadata
     */
    static void applyConfig(GExternalEvaluatorIndividual &ind, const Config &c);
    /**
     * @brief Teardown hook (called when the factory is destroyed): runs the external program --finalize
     * @param c The Config supplying the external-program name and options used for the --finalize call
     */
    static void finalize(const Config &c);
    /**
     * @brief Submits a batch of best individuals to the external program for archiving (--archive).
     *  Reads the program name / custom options / exchange base name / run-id from the individuals.
     * @param arch The batch of best individuals to be archived
     */
    static void archive(const std::vector<std::unique_ptr<GExternalEvaluatorIndividual>> &arch);

protected:
    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * This class contributes no checks of its own; it opts into the self-test facet so that the
     * category root's modify_GUnitTests_() keeps running for it under the standard tests.
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING
        return gen::GGenome::modify_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GExternalEvaluatorIndividual::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    /***************************************************************************/
    /**
     * @brief Performs self tests that are expected to succeed. This is needed for testing purposes
     *
     * This class contributes no checks of its own; it opts into the self-test facet so that the
     * category root's specificTestsNoFailureExpected_GUnitTests_() keeps running for it under the standard tests.
     */
    void specificTestsNoFailureExpected_GUnitTests_() override {
#ifdef GEM_TESTING
        gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GExternalEvaluatorIndividual::specificTestsNoFailureExpected_GUnitTests", "GEM_TESTING");
#endif /* GEM_TESTING */
    }

    /***************************************************************************/
    /**
     * @brief The evaluation hook: runs the external evaluation program and returns its per-criterion results.
     * @return The full per-criterion raw result vector obtained from the external evaluation program
     */
    std::vector<double> evaluate() final;

private:
    /***************************************************************************/
    /** @brief Worst-cases the results and flags the individual after the external command failed
     *  (the command-error branch of evaluate()). */
    void appendWorstCaseOnCommandError_(
        const std::string &command,
        int error_code,
        const std::string &command_output_file_name,
        std::vector<double> &results
    );
    /** @brief Parses+validates the external result file into the per-criterion result vector, honouring
     *  the isValid flag (the result-parse branch of evaluate()). */
    void parseExternalResults_(const std::string &result_file_name, std::vector<double> &results);

    /***************************************************************************/

    std::string program_name_; ///< The name of the external program to be executed
    std::string
        custom_options_; ///< Any custom options that need to be provided to the external program
    std::string parameter_file_base_name_; ///< The base name to be assigned to the parameter_file
    std::size_t n_results_; ///< The number of results to be expected from the evaluation function
    std::string run_id_;      ///< Identifies this run with a unique id
    bool remove_exec_temporaries_; ///< Indicates whether temporary files should be removed
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A factory for GExternalEvaluatorIndividual objects: an alias for the generic, config-driven
 * GIndividualFactory, for which GExternalEvaluatorIndividual supplies the static describeConfig /
 * buildGenome / buildAdaptionConfig / applyConfig / finalize hooks (plus the static archive() helper).
 * Call sites use ctor(path), get_as<>(), getAdaptionConfig() and registerContentCreator().
 */
using GExternalEvaluatorIndividualFactory =
    Gem::Geneva::Genome::GIndividualFactory<GExternalEvaluatorIndividual>;

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */

/*************************************************************************************************/
///////////////////////////////////////////////////////////////////////////////////////////////////
/*************************************************************************************************/
