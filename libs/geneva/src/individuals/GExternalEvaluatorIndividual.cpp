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

#include "geneva/individuals/GExternalEvaluatorIndividual.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GFileExchangeRun.hpp"
#include "common/GJsonIO.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeMultiConstraint.hpp"
#include "geneva/genome/GMultiConstraintT.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GExternalEvaluatorIndividual) // NOLINT
namespace Gem::Geneva::Individuals {

/******************************************************************************/
/**
 * @brief Turns the configured custom options into arguments of the external program.
 *
 * The program is started without a shell (Gem::Common::runExternalCommand), so the options are
 * split here: at whitespace, into words, each word one argument. Quotes are not interpreted; an
 * option value that contains a space cannot be passed this way. "empty" (the default) or an empty
 * string means no options.
 *
 * @param custom_options The configured custom options
 * @return The words of @p custom_options
 */
static std::vector<std::string> customOptionArguments(const std::string &custom_options) {
    std::vector<std::string> words;
    if(custom_options == "empty") {
        return words;
    }
    std::istringstream stream(custom_options);
    for(std::string word; stream >> word;) {
        words.push_back(std::move(word));
    }
    return words;
}

/******************************************************************************/
/**
 * @brief The name of the result file of an evaluation in its run directory
 */
static constexpr std::string_view RESULT_FILE = "result.json";

/**
 * @brief The name of the setup file in the run directory of --setup
 */
static constexpr std::string_view SETUP_FILE = "setup.json";

/******************************************************************************/
/**
 * @brief An argument naming a file in the run directory, for the program
 *
 * @param option The option, e.g. "--input="
 * @param name The file's name in the run directory
 * @return The option with the file's absolute path (Gem::Common::RUN_DIRECTORY stands for the
 * directory)
 */
static std::string runFileArgument(std::string_view option, std::string_view name) {
    return std::string(option) + std::string(Gem::Common::RUN_DIRECTORY) + "/" + std::string(name);
}

/******************************************************************************/
/**
 * @brief How the program ended, for messages
 *
 * @param run How it ended
 * @param time_limit The time limit of the run
 * @return E.g. "exited with status 1" or "was ended by signal 9"
 */
static std::string
howItEnded(Gem::Common::GProgramResult const &run, std::chrono::seconds time_limit) {
    switch(run.end) {
    case Gem::Common::ProgramEnd::EXITED:
        return "exited with status " + std::to_string(run.code);
    case Gem::Common::ProgramEnd::SIGNALLED:
        return "was ended by signal " + std::to_string(run.code);
    case Gem::Common::ProgramEnd::TIME_LIMIT:
        return "was ended: the time limit of " + std::to_string(time_limit.count()) +
               " s was reached";
    case Gem::Common::ProgramEnd::STOP_REQUEST:
        return "was ended by a stop request";
    }
    return {};
}

/******************************************************************************/
/**
 * @brief Whether the program ended by itself with status 0
 */
static bool succeeded(Gem::Common::GProgramResult const &run) {
    return run.end == Gem::Common::ProgramEnd::EXITED and run.code == 0;
}

/******************************************************************************/
/**
 * @brief The message of a failed exchange: what went wrong, the command and the last lines of the
 * program's output
 *
 * @param what What went wrong
 * @param run The exchange
 * @return The message
 */
static std::string failureOf(std::string const &what, Gem::Common::GFileExchangeResult const &run) {
    std::ostringstream message;
    message << what << '\n' << "Command: " << run.program.command << '\n';
    if(not run.output_tail.empty()) {
        message << "Last lines of the program's output:" << '\n' << run.output_tail << '\n';
    }
    return message.str();
}

/******************************************************************************/
/**
 * @brief Runs the program for a step that exchanges no files (--init, --finalize); its output is
 * the caller's
 *
 * @param program_name The program
 * @param custom_options The configured custom options
 * @param step The step, e.g. "--init"
 * @return How the program ended
 */
static Gem::Common::GProgramResult runStep(
    std::string const &program_name,
    std::string const &custom_options,
    std::string const &step
) {
    std::vector<std::string> arguments = customOptionArguments(custom_options);
    arguments.push_back(step);
    return Gem::Common::runExternalCommand(
        std::filesystem::path(program_name),
        arguments,
        std::filesystem::path()
    );
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The default constructor.
 */
GExternalEvaluatorIndividual::GExternalEvaluatorIndividual()
  : program_name_(GEEI_DEF_PROGNAME)
  , custom_options_(GEEI_DEF_CUSTOMOPTIONS)
  , parameter_file_base_name_(GEEI_DEF_PARFILEBASENAME)
  , run_id_(GEEI_DEF_RUNID)
  , remove_exec_temporaries_(GEEI_DEF_REMOVETEMPORARIES) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Sets the name of the external evaluation program.
 *
 * @param program_name The path / name of the external evaluation program to invoke
 */
void GExternalEvaluatorIndividual::setProgramName(const std::string &program_name) {
    program_name_ = program_name;
}

/******************************************************************************/
/**
 * @brief Retrieves the name of the external evaluation program.
 *
 * @return The path / name of the external evaluation program
 */
std::string GExternalEvaluatorIndividual::getProgramName() const {
    return program_name_;
}

/******************************************************************************/
/**
 * @brief Sets custom command-line options for the external evaluation program.
 *
 * @param custom_options The custom options, split at whitespace into separate arguments (no shell,
 * no quoting; "empty" for none)
 */
void GExternalEvaluatorIndividual::setCustomOptions(const std::string &custom_options) {
    custom_options_ = custom_options;
}

/******************************************************************************/
/**
 * @brief Retrieves the custom command-line options passed to the external evaluation program.
 *
 * @return The custom options string forwarded to the external program
 */
std::string GExternalEvaluatorIndividual::getCustomOptions() const {
    return custom_options_;
}

/******************************************************************************/
/**
 * @brief Sets the name of the parameter file in an evaluation's run directory; ".json" is appended.
 *
 * @param parameter_file The name; must be non-empty and not the literal "empty"
 */
void GExternalEvaluatorIndividual::setExchangeBaseName(const std::string &parameter_file) {
    if(parameter_file.empty() || parameter_file == "empty") {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "Invalid file name \"" << parameter_file << "\"" << '\n'
        );
    }

    parameter_file_base_name_ = parameter_file;
}

/******************************************************************************/
/**
 * @brief Retrieves the name of the parameter file in an evaluation's run directory.
 *
 * @return The name, without ".json"
 */
std::string GExternalEvaluatorIndividual::getExchangeBaseName() const {
    return parameter_file_base_name_;
}

/******************************************************************************/
/**
 * @brief Sets the number of results to be expected from the external evaluation program; the
 * individual stores as many results.
 *
 * @param n_results The number of result values the external program is expected to return; must be greater than 0
 */
void GExternalEvaluatorIndividual::setNExpectedResults(const std::size_t &n_results) {
    if(0 == n_results) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "Got invalid number of expected results: " << n_results << '\n'
        );
    }

    this->setNStoredResults(n_results);
}

/******************************************************************************/
/**
 * @brief Retrieves the number of results to be expected from the external evaluation program.
 *
 * @return The number of result values the external program is expected to return (the number of
 * stored results)
 */
std::size_t GExternalEvaluatorIndividual::getNExpectedResults() const {
    return this->getNStoredResults();
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place in an external program.
 *
 * The parameters are written into a run directory of this evaluation (Gem::Common::runWithFileExchange),
 * the program runs within the evaluation time limit, and the result file it writes there is parsed.
 * A program that fails (exit status, signal, time limit) gives the worst case and flags the
 * individual as invalid; a missing or malformed result throws.
 *
 * @return The full per-criterion raw result vector obtained from the external evaluation program
 */
std::vector<double> GExternalEvaluatorIndividual::evaluate() {
    namespace json = boost::json;

    // Transform this object into a JSON batch document
    json::object batch_out;
    batch_out["dataType"] = "run_parameters";
    batch_out["run_id"] = this->getRunId();
    batch_out["n_individuals"] = static_cast<std::size_t>(1);
    batch_out["individuals"] = json::array{this->toJSON()};

    std::string const parameter_file = parameter_file_base_name_ + ".json";
    Gem::Common::GFileExchangeSpec spec;
    spec.program = program_name_;
    spec.arguments = customOptionArguments(custom_options_);
    spec.arguments.emplace_back("--evaluate");
    spec.arguments.push_back(runFileArgument("--input=", parameter_file));
    spec.arguments.push_back(runFileArgument("--output=", RESULT_FILE));
    spec.inputs.emplace_back(parameter_file, Gem::Common::prettyPrintJson(batch_out));
    spec.result = RESULT_FILE;
    spec.limits.time_limit = evaluation_time_limit_;
    spec.keep = not remove_exec_temporaries_;

    auto const run = Gem::Common::runWithFileExchange(spec, ".");

    if(not succeeded(run.program)) {
        // As long as failures do not happen too often, the worst case has but little influence on
        // the optimization; the flag makes the individual recognizable as invalid
        this->force_set_error(failureOf(
            "In GExternalEvaluatorIndividual::evaluate(): the external program " +
                howItEnded(run.program, evaluation_time_limit_),
            run
        ));
        return std::vector<double>(this->getNStoredResults(), this->getWorstCase());
    }
    if(not run.result) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << failureOf(
                "In GExternalEvaluatorIndividual::evaluate(): the external program exited with 0 "
                "but wrote no result file " +
                    std::string(RESULT_FILE),
                run
            )
        );
    }
    return this->parseExternalResults_(*run.result, run.program.command);
}

/******************************************************************************/
/**
 * @brief Parses the external evaluation result document into the per-criterion result vector,
 * validating the individual count / result count and honouring the individual's isValid flag
 * (worst-casing + flagging on an invalid result). Extracted from evaluate() (the result-parse branch).
 *
 * @param result_text The JSON result document written by the external evaluation program
 * @param command The command that wrote it, for messages
 * @return The per-criterion result vector
 */
std::vector<double> GExternalEvaluatorIndividual::parseExternalResults_(
    const std::string &result_text,
    const std::string &command
) {
    namespace json = boost::json;

    // Parse the results (GJsonIO throws a gem_exception naming the command on a malformed document)
    json::value const result_doc =
        Gem::Common::parseJsonString(result_text, "the result file of " + command);

    std::size_t const n_results = this->getNStoredResults();
    std::vector<double> results(n_results, 0.);
    try {
        json::object const &root = result_doc.as_object();

        // Check that only a single result individual was returned
        auto n_external_individuals = root.at("n_individuals").to_number<std::size_t>();
        if(1 != n_external_individuals) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "Number of result individuals != 1: " << n_external_individuals << '\n'
            );
        }

        json::object const &result_individual = root.at("individuals").as_array().at(0).as_object();

        // Check that the number of results provided by the result file matches the number of expected results
        auto external_n_results = result_individual.at("n_results").to_number<std::size_t>();
        if(external_n_results != n_results) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "Result file provides n_results = " << external_n_results << '\n'
                << "while we expected " << n_results << '\n'
            );
        }

        // Check whether the results represent useful values
        bool const is_valid = result_individual.at("isValid").as_bool();
        if(not is_valid) { // Assign worst-case values to all result
            std::ostringstream error_message;

            error_message << "In GExternalEvaluatorIndividual::evaluate():" << '\n'
                          << "individuals[0].isValid is \"false\"" << '\n';

            for(double &r : results) {
                r = this->getWorstCase();
            }

            // Make sure the individual can be recognized as invalid by Geneva
            this->force_set_error(error_message.str());
        }
        else { // Extract and store all result values
            json::array const &results_node = result_individual.at("results").as_array();

            for(std::size_t res = 0; res < n_results; res++) {
                results[res] = results_node.at(res).as_object().at("rawResult").to_number<double>();
            }
        }
    }
    catch(const Gem::Common::gem_exception &) {
        throw; // re-throw our own diagnostics untouched
    }
    catch(const std::exception &e) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "Error reading the result file of " << command << '\n'
            << "with message " << e.what() << '\n'
        );
    }
    return results;
}

/******************************************************************************/
/**
 * @brief Allows to assign a run-id to this individual.
 *
 * @param run_id The run identifier to assign; must be non-empty and not the literal "empty"
 */
void GExternalEvaluatorIndividual::setRunId(const std::string &run_id) {
    if(run_id.empty() || "empty" == run_id) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "Attempt to set an invalid run id: \"" << run_id << "\"" << '\n'
        );
    }

    run_id_ = run_id;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the run-id assigned to this individual.
 *
 * @return The run identifier assigned to this individual
 */
std::string GExternalEvaluatorIndividual::getRunId() const {
    return run_id_;
}

/******************************************************************************/
/**
 * @brief Allows to specify whether the run directories of the exchanges with the external program
 * are removed afterwards. Keeping them is mostly needed for debugging purposes.
 *
 * @param remove_exec_temporaries If true, the run directory of each exchange is removed after it
 */
void GExternalEvaluatorIndividual::setRemoveExecTemporaries(bool remove_exec_temporaries) {
    remove_exec_temporaries_ = remove_exec_temporaries;
}

/******************************************************************************/
/**
 * @brief Allows to check whether the run directories of the exchanges are removed.
 *
 * @return true if the run directory of each exchange is removed after it, false otherwise
 */
bool GExternalEvaluatorIndividual::getRemoveExecTemporaries() const {
    return remove_exec_temporaries_;
}

/******************************************************************************/
/**
 * @brief Sets the time after which an evaluation is ended (SIGTERM, then SIGKILL) and counts as
 * failed.
 *
 * @param evaluation_time_limit The wall-clock time limit; 0: none
 */
void GExternalEvaluatorIndividual::setEvaluationTimeLimit(
    std::chrono::seconds evaluation_time_limit
) {
    evaluation_time_limit_ = evaluation_time_limit;
}

/******************************************************************************/
/**
 * @brief Retrieves the time after which an evaluation is ended and counts as failed.
 *
 * @return The wall-clock time limit; 0: none
 */
std::chrono::seconds GExternalEvaluatorIndividual::getEvaluationTimeLimit() const {
    return evaluation_time_limit_;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Registers the config-file options, binding them to the passed Config.
 *
 * The base GGenome options are registered separately by GIndividualFactory::getObject_
 * (via addConfigurationOptions).
 *
 * @param gpb The GParserBuilder object with which the configuration file options are registered
 * @param c The Config struct whose fields are bound to the registered options (filled on parse)
 */
void GExternalEvaluatorIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    gpb.registerFileParameter<double>("ad_prob", c.ad_prob, GEEI_DEF_ADPROB)
        << "The probability for random adaption of values in evolutionary algorithms";
    gpb.registerFileParameter<double>("adapt_ad_prob", c.adapt_ad_prob, GEEI_DEF_ADAPTADPROB)
        << "Determines the rate of adaption of ad_prob. Set to 0, if you do not need this feature";
    gpb.registerFileParameter<double>("min_ad_prob", c.min_ad_prob, GEEI_DEF_MINADPROB)
        << "The lower allowed boundary for ad_prob-variation";
    gpb.registerFileParameter<double>("max_ad_prob", c.max_ad_prob, GEEI_DEF_MAXADPROB)
        << "The upper allowed boundary for ad_prob-variation";
    gpb.registerFileParameter<std::uint32_t>(
        "adaption_threshold",
        c.adaption_threshold,
        GEEI_DEF_ADAPTIONTHRESHOLD
    ) << "The number of calls to an adaptor after which adaption takes place";
    gpb.registerFileParameter<bool>("use_bi_gaussian", c.use_bi_gaussian, GEEI_DEF_USEBIGAUSSIAN)
        << "Whether to use a double gaussion for the adaption of parmeters in ES";
    gpb.registerFileParameter<double>("sigma1", c.sigma1, GEEI_DEF_SIGMA1)
        << "The sigma for gauss-adaption in ES" << '\n'
        << "(or the sigma of the left peak of a double gaussian)";
    gpb.registerFileParameter<double>("sigma_sigma1", c.sigma_sigma1, GEEI_DEF_SIGMASIGMA1)
        << "Influences the self-adaption of gauss-mutation in ES";
    gpb.registerFileParameter<double>("min_sigma1", c.min_sigma1, GEEI_DEF_MINSIGMA1)
        << "The minimum value of sigma1";
    gpb.registerFileParameter<double>("max_sigma1", c.max_sigma1, GEEI_DEF_MAXSIGMA1)
        << "The maximum value of sigma1";
    gpb.registerFileParameter<double>("sigma2", c.sigma2, GEEI_DEF_SIGMA2)
        << "The sigma of the right peak of a double gaussian (if any)";
    gpb.registerFileParameter<double>("sigma_sigma2", c.sigma_sigma2, GEEI_DEF_SIGMASIGMA2)
        << "Influences the self-adaption of gauss-mutation in ES";
    gpb.registerFileParameter<double>("min_sigma2", c.min_sigma2, GEEI_DEF_MINSIGMA2)
        << "The minimum value of sigma2";
    gpb.registerFileParameter<double>("max_sigma2", c.max_sigma2, GEEI_DEF_MAXSIGMA2)
        << "The maximum value of sigma2";
    gpb.registerFileParameter<double>("delta", c.delta, GEEI_DEF_DELTA)
        << "The start distance between both peaks used for bi-gaussian mutations in ES";
    gpb.registerFileParameter<double>("sigma_delta", c.sigma_delta, GEEI_DEF_SIGMADELTA)
        << "The width of the gaussian used for mutations of the delta parameter";
    gpb.registerFileParameter<double>("min_delta", c.min_delta, GEEI_DEF_MINDELTA)
        << "The minimum allowed value of delta";
    gpb.registerFileParameter<double>("max_delta", c.max_delta, GEEI_DEF_MAXDELTA)
        << "The maximum allowed value of delta";
    gpb.registerFileParameter<std::string>("program_name", c.program_name, GEEI_DEF_PROGNAME)
        << "The name of the external evaluation program";
    gpb.registerFileParameter<std::string>(
        "custom_options",
        c.custom_options,
        GEEI_DEF_CUSTOMOPTIONS
    ) << "Any custom options you wish to pass to the external evaluator,"
      << '\n'
      << "split at whitespace into separate arguments (no shell, no quoting)";
    gpb.registerFileParameter<std::string>(
        "parameter_file",
        c.parameter_file_base_name,
        GEEI_DEF_PARFILEBASENAME
    ) << "The name (without .json) of the parameter file"
      << '\n'
      << "in the run directory of each evaluation";
    gpb.registerFileParameter<std::string>("init_values", c.init_values, GEEI_DEF_STARTMODE)
        << "Indicates, whether individuals should be initialized randomly (random)," << '\n'
        << "with the lower (min) or upper (max) boundary of their value ranges";
    gpb.registerFileParameter<bool>(
        "remove_exec_temporaries",
        c.remove_exec_temporaries,
        GEEI_DEF_REMOVETEMPORARIES
    ) << "Indicates, whether the run directory of each exchange with the external program"
      << '\n'
      << "should be removed afterwards";
    gpb.registerFileParameter<std::uint32_t>(
        "evaluation_time_limit",
        c.evaluation_time_limit,
        GEEI_DEF_EVALUATIONTIMELIMIT
    ) << "The time in seconds after which an evaluation is ended and counts as failed;"
      << '\n'
      << "0: none";
}

/******************************************************************************/
/**
 * @brief Runs the external evaluator's --init then --setup steps and returns the parsed setup document
 * (guaranteed to be a JSON object). Extracted from buildGenome() (the external-query / IPC phase).
 *
 * @param c The Config supplying the program name / custom options / init mode
 * @return The parsed setup JSON document
 */
static boost::json::value queryExternalSetup(const GExternalEvaluatorIndividual::Config &c) {
    // Give the external program the opportunity to perform initial work
    if(auto const run = runStep(c.program_name, c.custom_options, "--init"); not succeeded(run)) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "The external program " << howItEnded(run, {}) << " in its --init step." << '\n'
            << "Command: " << run.command << '\n'
        );
    }

    // Now ask the external program for setup information
    Gem::Common::GFileExchangeSpec spec;
    spec.program = c.program_name;
    spec.arguments = customOptionArguments(c.custom_options);
    spec.arguments.emplace_back("--setup");
    spec.arguments.push_back(runFileArgument("--output=", SETUP_FILE));
    spec.arguments.push_back("--initvalues=" + c.init_values);
    spec.result = SETUP_FILE;
    spec.keep = not c.remove_exec_temporaries;

    auto const run = Gem::Common::runWithFileExchange(spec, ".");
    if(not succeeded(run.program)) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << failureOf(
                "The external program " + howItEnded(run.program, {}) + " in its --setup step.",
                run
            )
        );
    }
    if(not run.result) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << failureOf(
                "The external program exited with 0 but wrote no setup file " +
                    std::string(SETUP_FILE),
                run
            )
        );
    }

    boost::json::value setup_doc =
        Gem::Common::parseJsonString(*run.result, "the setup file of " + run.program.command);
    if(not setup_doc.is_object()) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "Setup document is empty or not a JSON object." << '\n'
        );
    }

    return setup_doc;
}

/******************************************************************************/
/**
 * @brief Builds the flat genome from the external evaluator's setup document, recording the discovered
 * run-id and expected result count back into @p c. Extracted from buildGenome() (the authoring phase).
 *
 * @param setup_doc The parsed setup document returned by queryExternalSetup()
 * @param c The Config, mutated in place with the discovered run-id and expected result count
 * @return The flat genome structure describing the discovered variables
 */
static Genome::GenomeData authorFlatGenomeFromSetup(
    const boost::json::value &setup_doc,
    GExternalEvaluatorIndividual::Config &c
) {
    namespace json = boost::json;

    // Author the flat genome: each discovered variable becomes one constrained double group. Fixed
    // variables (min == max) are built with adaptionMode::NEVER; active ones get their Gauss/bi-Gauss
    // adaptor from the OA-owned config (buildAdaptionConfig), not the layout.
    Genome::GGenomeBuilder gb;

    try {
        json::object const &root = setup_doc.as_object();

        auto n_individuals = root.at("n_individuals").to_number<std::size_t>();
        if(1 != n_individuals) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "Received invalid number of setup-individuals: " << n_individuals << '\n'
            );
        }

        c.run_id = json::value_to<std::string>(root.at("run_id"));
        json::object const &setup_individual = root.at("individuals").as_array().at(0).as_object();
        auto n_var = setup_individual.at("nVars").to_number<std::size_t>();
        c.n_results_expected = setup_individual.at("n_results").to_number<std::size_t>();

        json::array const &var_set_node = setup_individual.at("vars").as_array();
        if(var_set_node.empty()) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer() << "No variables were specified" << '\n'
            );
        }
        if(var_set_node.size() != n_var) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "The setup gives nVars " << n_var << " but lists " << var_set_node.size()
                << " variables" << '\n'
            );
        }

        // A random start: as the setup says per variable (initRandom), else as configured. All
        // variables start alike, as randomInit() draws every active one.
        std::size_t n_random = 0;
        for(json::value const &var : var_set_node) {
            json::object const &var_subtree = var.as_object();
            auto const *init_random = var_subtree.if_contains("initRandom");
            if(init_random ? init_random->as_bool() : c.init_values == "random") {
                ++n_random;
            }
        }
        if(n_random != 0 and n_random != n_var) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "The setup asks for a random start of " << n_random << " of its " << n_var
                << " variables; only all or none are supported" << '\n'
            );
        }
        c.random_start = n_random != 0;

        for(json::value const &var : var_set_node) {
            json::object const &var_subtree = var.as_object();
            std::string const var_type = json::value_to<std::string>(var_subtree.at("type"));
            if("GConstrainedDoubleObject" == var_type) {
                auto min_var = var_subtree.at("lowerBoundary").to_number<double>();
                auto max_var = var_subtree.at("upperBoundary").to_number<double>();
                auto init_value = var_subtree.at("values").as_array().at(0).to_number<double>();
                if(min_var == max_var) {
                    // Take this as a sign that the parameter should not be modified.
                    gb.addDouble(
                          init_value,
                          init_value,
                          std::max(1.0001 * init_value, init_value + 0.0001)
                    )
                        .adaptionMode(Gem::Geneva::adaptionMode::NEVER);
                }
                else {
                    gb.addDouble(init_value, min_var, max_var);
                }
            }
            else {
                throw Gem::Common::gem_exception(
                    Gem::Common::g_error_streamer()
                    << var_type << " provided as type name." << '\n'
                    << "Currently only GConstrainedDoubleObject is supported." << '\n'
                );
            }
        }
    }
    catch(const Gem::Common::gem_exception &) {
        throw; // Re-throw the original object (a copy would slice a derived type)
    }
    catch(...) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << "Caught unknown exception!" << '\n'
        );
    }

    return gb.build();
}

/******************************************************************************/
/**
 * @brief Queries the external evaluator program for the desired structure of the individuals and builds the flat genome from it.
 *
 * This runs the program with --init then --setup, parses the returned JSON, and builds the genome from
 * it. Because the generic factory builds the shared genome exactly once, the (expensive) external query
 * happens once too. The discovered
 * run-id and result count are recorded back into @p c so applyConfig() can hand them to each produced
 * individual.
 *
 * @param c The Config supplying the program name / custom options / init mode; mutated in place with the discovered run-id and expected result count
 * @return The flat genome structure (GenomeData) describing the discovered variables
 */
Genome::GenomeData GExternalEvaluatorIndividual::buildGenome(Config &c) {
    if(c.program_name.empty()) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << "Program name was empty" << '\n'
        );
    }
    if(not std::filesystem::exists(c.program_name)) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "External program " << c.program_name << " does not seem to exist" << '\n'
        );
    }

    const boost::json::value setup_doc = queryExternalSetup(c);
    return authorFlatGenomeFromSetup(setup_doc, c);
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration for a genome produced by this individual.
 *
 * Every ACTIVE double group (the constrained variables) receives the configured single-Gauss or
 * bi-Gauss adaptor; fixed groups (adaptionMode::NEVER) are left un-authored.
 *
 * @param sample A sample flat genome whose group structure the adaption config is built against
 * @param c The Config supplying the adaptor parameters (sigmas, ad_prob, bi-gaussian flag, ...)
 * @return A shared pointer to the populated OA-owned adaption configuration
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GExternalEvaluatorIndividual::buildAdaptionConfig(const Genome::GGenome &sample, const Config &c) {
    namespace OA = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = OA::makeAdaptionConfig<OA::GAdaptionConfigBase>(sample);
    const auto &groups = cfg->doubleGroups();
    for(std::size_t i = 0; i < groups.size(); i++) {
        if(not groups[i].active) {
            continue; // a fixed (adaptionMode::NEVER) parameter -- never adapted, no adaptor
        }
        if(c.use_bi_gaussian) {
            cfg->groupDouble(i).biGauss(
                c.sigma1,
                c.sigma_sigma1,
                c.min_sigma1,
                c.max_sigma1,
                c.sigma2,
                c.sigma_sigma2,
                c.min_sigma2,
                c.max_sigma2,
                c.delta,
                c.sigma_delta,
                c.min_delta,
                c.max_delta,
                c.ad_prob,
                /*use_symmetric_sigmas=*/false,
                c.adapt_ad_prob,
                c.adaption_threshold
            );
        }
        else {
            cfg->groupDouble(i).gauss(
                c.sigma1,
                c.sigma_sigma1,
                c.min_sigma1,
                c.max_sigma1,
                c.ad_prob,
                c.adapt_ad_prob,
                c.adaption_threshold,
                Gem::Geneva::adaptionMode::WITHPROBABILITY,
                c.min_ad_prob,
                c.max_ad_prob
            );
        }
    }
    return cfg;
}

/******************************************************************************/
/**
 * @brief Per-object post-config hook applying the external-program parameters and discovered metadata.
 *
 * Called by GIndividualFactory::postProcess_ after the genome is installed; applies the
 * external-program parameters and the metadata discovered by buildGenome (run-id, expected result
 * count), and draws random start values within the bounds if the setup asked for them.
 *
 * @param ind The individual to configure (mutated in place)
 * @param c The Config supplying the exchange base name, program name, custom options, expected result count, temporary-removal flag and run-id
 */
void GExternalEvaluatorIndividual::applyConfig(GExternalEvaluatorIndividual &ind, const Config &c) {
    ind.setExchangeBaseName(c.parameter_file_base_name);
    ind.setProgramName(c.program_name);
    ind.setCustomOptions(c.custom_options);
    ind.setNExpectedResults(c.n_results_expected);
    ind.setRemoveExecTemporaries(c.remove_exec_temporaries);
    ind.setEvaluationTimeLimit(std::chrono::seconds(c.evaluation_time_limit));
    ind.setRunId(c.run_id);
    if(c.random_start) {
        ind.randomInit(activityMode::ACTIVEONLY);
    }
}

/******************************************************************************/
/**
 * @brief Teardown hook giving the external evaluator program a chance to perform final work (--finalize).
 *
 * Called by GIndividualFactory's destructor once a genome has been produced. A failure is reported
 * (GEM_ERROR) and does not end the program: the caller's remaining work still runs.
 *
 * @param c The Config supplying the program name and custom options used to invoke the external program with --finalize
 */
void GExternalEvaluatorIndividual::finalize(const Config &c) {
    if(c.program_name.empty() or not std::filesystem::exists(c.program_name)) {
        GEM_LOGGER << "In GExternalEvaluatorIndividual::finalize(): the external program \""
                   << c.program_name << "\" does not exist; --finalize was not run" << '\n'
                   << GEM_ERROR;
        return;
    }

    if(auto const run = runStep(c.program_name, c.custom_options, "--finalize");
       not succeeded(run)) {
        GEM_LOGGER << "In GExternalEvaluatorIndividual::finalize(): the external program "
                   << howItEnded(run, {}) << " in its --finalize step." << '\n'
                   << "Command: " << run.command << '\n'
                   << GEM_ERROR;
    }
}

/******************************************************************************/
/**
 * @brief Submits a batch of best individuals to the external program for archiving (--archive).
 *
 * Reads the program name / custom options / exchange base name / run-id from the archived individuals
 * themselves (every produced individual carries them, courtesy of applyConfig).
 *
 * @param arch A vector of best individuals to archive; if empty the call is a no-op, otherwise the first element supplies the run metadata
 */
void GExternalEvaluatorIndividual::archive(
    const std::vector<std::unique_ptr<GExternalEvaluatorIndividual>> &arch
) {
    namespace json = boost::json;

    if(arch.empty()) {
        return;
    }

    json::object batch_out;
    batch_out["dataType"] = "archive_data";
    batch_out["run_id"] = arch.front()->getRunId();
    batch_out["n_individuals"] = arch.size();

    json::array individuals;
    for(const auto &individual : arch) {
        individuals.push_back(individual->toJSON());
    }
    batch_out["individuals"] = std::move(individuals);

    std::string const parameter_file = arch.front()->getExchangeBaseName() + ".json";
    Gem::Common::GFileExchangeSpec spec;
    spec.program = arch.front()->getProgramName();
    spec.arguments = customOptionArguments(arch.front()->getCustomOptions());
    spec.arguments.emplace_back("--archive");
    spec.arguments.push_back(runFileArgument("--input=", parameter_file));
    spec.inputs.emplace_back(parameter_file, Gem::Common::prettyPrintJson(batch_out));
    spec.keep = not arch.front()->getRemoveExecTemporaries();

    auto const run = Gem::Common::runWithFileExchange(spec, ".");
    if(not succeeded(run.program)) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << failureOf(
                "The external program " + howItEnded(run.program, {}) + " in its --archive step.",
                run
            )
        );
    }
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
