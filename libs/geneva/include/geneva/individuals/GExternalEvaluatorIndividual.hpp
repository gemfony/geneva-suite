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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"
#include "geneva/GNamespaceAliases.hpp"

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
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeMultiConstraint.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GIndividualFactory.hpp"
#include "hap/GRandomT.hpp"

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
const std::string GEEI_DEF_RUNID = "empty";
constexpr bool GEEI_DEF_REMOVETEMPORARIES = true;
constexpr std::uint32_t GEEI_DEF_EVALUATIONTIMELIMIT = 0;

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
 * --setup --initvalues=[min/max/random] --output=setupFile.json
 * --evaluate --input=paramsFile.json   --output=result_file.json
 * --archive  --input=archiveFile.json
 * --finalize
 *
 * The program is started without a shell (Gem::Common::runExternalCommand): each of these arguments
 * reaches it as one argument, file names unquoted. The configured custom options ("custom_options")
 * are placed before them, split at whitespace into separate arguments; quotes in them are not
 * interpreted. A program that needs shell syntax is called through a script.
 *
 * The parameter / result / setup files exchanged with the external program are JSON documents
 * (see the shipped evaluator.py reference implementation for the schema). Hence the external
 * program needs to read and write this JSON format. Each --setup, --evaluate and --archive
 * exchanges its files in a run directory of its own, created in the working directory and removed
 * afterwards (Gem::Common::runWithFileExchange); the file names passed to the program are absolute.
 * The program itself runs in the caller's working directory. An evaluation that takes longer than
 * its time limit is ended and counts as failed.
 *
 * A program that is ended (time limit) gets SIGTERM; if it starts further programs, it passes the
 * signal on to them or replaces itself with exec.
 */
class GExternalEvaluatorIndividual : public Genome::GGenomeT<GExternalEvaluatorIndividual> {
    ///////////////////////////////////////////////////////////////////////

    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin
    // reach the private localMembers_() below (from which serialize/load_/compare_/clone_/name_ derive).
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief Single declaration of this class'es local data members
     * @return A tuple of named member references driving the generated serialize()/load_()/compare_()
     */
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member("program_name_", self.program_name_),
            Gem::Common::make_member("custom_options_", self.custom_options_),
            Gem::Common::make_member("parameter_file_base_name_", self.parameter_file_base_name_),
            Gem::Common::make_member("run_id_", self.run_id_),
            Gem::Common::make_member("remove_exec_temporaries_", self.remove_exec_temporaries_),
            Gem::Common::make_member("evaluation_time_limit_", self.evaluation_time_limit_)
        );
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and compare token. */
    static constexpr std::string_view class_name = "GExternalEvaluatorIndividual";

    /***************************************************************************/
    using FACTORYTYPE = Gem::Geneva::Genome::GIndividualFactory<GExternalEvaluatorIndividual>;

    /** @brief The default constructor */
    GExternalEvaluatorIndividual();

    /**
     * @brief Sets the name of the external evaluation program
     */
    void setProgramName(const std::string &program_name);
    /**
     * @brief Retrieves the name of the external evaluation program
     */
    std::string getProgramName() const;

    /**
     * @brief Sets any custom options that need to be passed to the external evaluation program
     */
    void setCustomOptions(const std::string &custom_options);
    /**
     * @brief Retrieves any custom options that need to be passed to the external evaluation program
     */
    std::string getCustomOptions() const;

    /**
     * @brief Sets the name (without ".json") of the parameter file in an evaluation's run directory
     */
    void setExchangeBaseName(const std::string &parameter_file);
    /**
     * @brief Retrieves the name (without ".json") of the parameter file in an evaluation's run directory
     */
    std::string getExchangeBaseName() const;

    /**
     * @brief Sets the number of results to be expected from the external evaluation program
     */
    void setNExpectedResults(const std::size_t &n_results);
    /**
     * @brief Retrieves the number of results to be expected from the external evaluation program
     */
    std::size_t getNExpectedResults() const;

    /**
     * @brief Allows to assign a run-id to this individual
     */
    void setRunId(const std::string &run_id);
    /**
     * @brief Allows to retrieve the run-id assigned to this individual
     */
    std::string getRunId() const;

    /**
     * @brief Allows to specify whether the run directories of the exchanges should be removed
     */
    void setRemoveExecTemporaries(bool remove_exec_temporaries);
    /**
     * @brief Allows to check whether the run directories of the exchanges are removed
     */
    bool getRemoveExecTemporaries() const;

    /**
     * @brief Sets the time after which an evaluation is ended as failed; 0: none
     */
    void setEvaluationTimeLimit(std::chrono::seconds evaluation_time_limit);
    /**
     * @brief Retrieves the time after which an evaluation is ended as failed; 0: none
     */
    std::chrono::seconds getEvaluationTimeLimit() const;

    /***************************************************************************/
    /**
     * The configuration read from the config file by GIndividualFactory<GExternalEvaluatorIndividual>.
     * Besides the Gauss / bi-Gauss adaptor settings and the external-program parameters, three fields
     * (run_id, n_results_expected, random_start) are not parsed from the file but DISCOVERED by
     * buildGenome() when it queries the external evaluator; buildGenome writes them back so
     * applyConfig() can hand them to each produced individual.
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
        std::uint32_t evaluation_time_limit = GEEI_DEF_EVALUATIONTIMELIMIT;
        // Discovered by buildGenome() from the external evaluator's setup output (not parsed from file):
        std::string run_id = GEEI_DEF_RUNID;
        std::size_t n_results_expected = GEEI_DEF_NRESULTS;
        bool random_start = false; ///< whether the setup asked for random start values
    };

    /**
     * @brief Registers the config-file options, binding them to the passed Config
     */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c);
    /**
     * @brief Queries the external evaluator for the individual structure and builds the flat genome;
     *  also records the discovered run_id / n_results_expected back into @p c.
     */
    static Genome::GenomeData buildGenome(Config &c);
    /**
     * @brief The OA-owned adaption config: the configured Gauss/bi-Gauss adaptor on every ACTIVE group
     */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const Genome::GGenome &sample, const Config &c);
    /**
     * @brief Per-object post-config hook: applies the external-program parameters + discovered metadata
     */
    static void applyConfig(GExternalEvaluatorIndividual &ind, const Config &c);
    /**
     * @brief Teardown hook (called when the factory is destroyed): runs the external program --finalize
     */
    static void finalize(const Config &c);
    /**
     * @brief Submits a batch of best individuals to the external program for archiving (--archive).
     *  Reads the program name / custom options / exchange base name / run-id from the individuals.
     */
    static void archive(const std::vector<std::unique_ptr<GExternalEvaluatorIndividual>> &arch);

protected:
    /***************************************************************************/

    /***************************************************************************/
    /**
     * @brief The evaluation hook: runs the external evaluation program and returns its per-criterion results.
     */
    std::vector<double> evaluate() final;

private:
    /***************************************************************************/
    /** @brief Parses+validates the external result document into the per-criterion result vector,
     *  honouring the isValid flag (the result-parse branch of evaluate()). */
    std::vector<double>
    parseExternalResults_(const std::string &result_text, const std::string &command);

    /***************************************************************************/

    std::string program_name_; ///< The name of the external program to be executed
    std::string
        custom_options_; ///< Any custom options that need to be provided to the external program
    std::string parameter_file_base_name_; ///< The name of the parameter file in the run directory
    std::string run_id_;                   ///< Identifies this run with a unique id
    bool remove_exec_temporaries_; ///< Indicates whether the run directories should be removed
    std::chrono::seconds evaluation_time_limit_{GEEI_DEF_EVALUATIONTIMELIMIT}; ///< 0: none
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
