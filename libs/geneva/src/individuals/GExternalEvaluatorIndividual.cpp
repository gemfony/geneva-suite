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
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GJsonIO.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GMultiConstraintT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/genome/GGenomeMultiConstraint.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <sstream>
#include <tuple>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GExternalEvaluatorIndividual) // NOLINT
namespace Gem::Geneva::Individuals {

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
  , n_results_(GEEI_DEF_NRESULTS)
  , run_id_(GEEI_DEF_RUNID)
  , remove_exec_temporaries_(GEEI_DEF_REMOVETEMPORARIES) { /* nothing */
}

/******************************************************************************/
/**
 * @brief A standard copy constructor.
 *
 * @param cp A constant reference to another GExternalEvaluatorIndividual to be copied
 */
GExternalEvaluatorIndividual::GExternalEvaluatorIndividual(const GExternalEvaluatorIndividual &cp)
  : gen::GGenomeT<GExternalEvaluatorIndividual>(cp) // copies the base genome (value channels + layout)
  , program_name_(cp.program_name_)
  , custom_options_(cp.custom_options_)
  , parameter_file_base_name_(cp.parameter_file_base_name_)
  , n_results_(cp.n_results_)
  , run_id_(cp.run_id_)
  , remove_exec_temporaries_(cp.remove_exec_temporaries_) { /* nothing */
}

/******************************************************************************/
/**
 * The standard destructor
 */
GExternalEvaluatorIndividual::~GExternalEvaluatorIndividual() { /* nothing */
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
 * @brief Sets custom command-line options passed verbatim to the external evaluation program.
 *
 * @param custom_options The custom options string forwarded to the external program
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
 * @brief Sets the base name of the data exchange file.
 *
 * Note that the individual might add additional characters in order to distinguish between the
 * exchange files of different individuals.
 *
 * @param parameter_file The desired new base name of the exchange file; must be non-empty and not the literal "empty"
 */
void GExternalEvaluatorIndividual::setExchangeBaseName(const std::string &parameter_file) {
    if(parameter_file.empty() || parameter_file == "empty") {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::setExchangeBaseName(): Error!" << '\n'
            << "Invalid file name \"" << parameter_file << "\"" << '\n'
        );
    }

    parameter_file_base_name_ = parameter_file;
}

/******************************************************************************/
/**
 * @brief Retrieves the current value of the parameter_file_base_name_ variable.
 *
 * @return The current base name of the exchange file
 */
std::string GExternalEvaluatorIndividual::getExchangeBaseName() const {
    return parameter_file_base_name_;
}

/******************************************************************************/
/**
 * @brief Sets the number of results to be expected from the external evaluation program.
 *
 * @param n_results The number of result values the external program is expected to return; must be greater than 0
 */
void GExternalEvaluatorIndividual::setNExpectedResults(const std::size_t &n_results) {
    if(0 == n_results) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::setNExpectedResults(): Error!" << '\n'
            << "Got invalid number of expected results: " << n_results << '\n'
        );
    }

    n_results_ = n_results;
}

/******************************************************************************/
/**
 * @brief Retrieves the number of results to be expected from the external evaluation program.
 *
 * @return The number of result values the external program is expected to return
 */
std::size_t GExternalEvaluatorIndividual::getNExpectedResults() const {
    return n_results_;
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place in an external program.
 *
 * Here we just write a file with the required parameters to disk and execute the program, then
 * parse the result file it produces. On failure the worst-case value is assigned and the individual
 * is flagged as invalid.
 *
 * @return The primary (first) result value returned by the external program
 */
std::vector<double> GExternalEvaluatorIndividual::evaluate() {
    namespace json = boost::json;

    // Transform this object into a JSON batch document
    json::object batch_out;
    batch_out["dataType"] = "run_parameters";
    batch_out["run_id"] = this->getRunId();
    batch_out["n_individuals"] = static_cast<std::size_t>(1);
    batch_out["individuals"] = json::array{this->toJSON()};

    // Create a suitable extension and exchange file names for this object
    std::string const extension = std::string("-") +
                            Gem::Common::to_string(this->getAssignedIteration()) + "-" +
                            Gem::Common::to_string(this);
    std::string const parameterfile_name = parameter_file_base_name_ + extension + ".json";
    std::string const result_file_name = std::string("result") + extension + ".json";
    std::string const command_output_file_name = std::string("commandOutput") + extension + ".txt";

    // RAII guard: remove the three IPC temp files on scope exit, whether normal or via exception.
    // remove() is a no-op for non-existent files (e.g. result_file_name when the external
    // program never ran), so all three can be listed unconditionally.
    struct TempFileGuard {
        const std::string &param, &result, &cmdOut;
        bool active;
        ~TempFileGuard() {
            if (!active) return;
            std::filesystem::remove(param);
            std::filesystem::remove(result);
            std::filesystem::remove(cmdOut);
        }
    } temp_guard{
        .param=parameterfile_name,
        .result=result_file_name,
        .cmdOut=command_output_file_name,
        .active=remove_exec_temporaries_
    };

    // Save the parameters to a file for the external evaluation
    Gem::Common::writeJsonFile(parameterfile_name, batch_out);

    // Collect all command-line arguments
    std::vector<std::string> arguments;
    if(custom_options_ != "empty" && not custom_options_.empty()) {
        arguments.push_back(custom_options_);
    }
    arguments.emplace_back("--evaluate");
    arguments.push_back(std::string("--input=\"") + parameterfile_name + "\"");
    arguments.push_back(std::string("--output=\"") + result_file_name + "\"");

    // Perform the external evaluation. The full per-criterion result vector is returned (the caller writes
    // each criterion); an error / invalid path fills it with the worst case and flags the individual.
    std::vector<double> results(n_results_, 0.);
    std::string command;
    int const error_code = Gem::Common::runExternalCommand(
        std::filesystem::path(program_name_),
        arguments,
        std::filesystem::path(command_output_file_name),
        command
    );

    if(error_code) { // Something went wrong -- worst-case the results and flag the individual
        this->appendWorstCaseOnCommandError_(command, error_code, command_output_file_name, results);
    }
    else { // Everything is o.k., lets retrieve the evaluation from the result file
        this->parseExternalResults_(result_file_name, results);
    }

    // Return the full per-criterion result vector (the caller writes each criterion)
    return results;
}

/******************************************************************************/
/**
 * @brief Fills the result vector with the worst case and flags the individual as invalid after the
 * external evaluation command failed. Extracted from evaluate() (the command-error branch).
 *
 * @param command The command line that was executed
 * @param error_code The non-zero error code returned by the external command
 * @param command_output_file_name The file capturing the external program's output (for diagnostics)
 * @param results The per-criterion result vector to fill with the worst case (modified in place)
 */
void GExternalEvaluatorIndividual::appendWorstCaseOnCommandError_(
    const std::string &command,
    int error_code,
    const std::string &command_output_file_name,
    std::vector<double> &results
) {
    std::ostringstream error_message; // NOLINT(cppcoreguidelines-init-variables)

    error_message << "In GExternalEvaluatorIndividual::evaluate():" << '\n'
                  << "Execution of external command failed." << '\n'
                  << "Command: " << command << '\n'
                  << "Error code: " << error_code << '\n'
                  << "Program output:" << '\n'
                  << Gem::Common::loadTextDataFromFile(command_output_file_name) << '\n';

#ifdef DEBUG
    glogger << error_message.str() << GWARNING;
#endif

    // O.k., so the external application crashed or returned an error.
    // All we can do here is to return the worst case. As long as crashes
    // do not happen too often, this will have but little influence on
    // the optimization.
    for(double &r : results) {
        r = this->getWorstCase();
    }

    // Make sure the individual can be recognized as invalid by Geneva
    this->force_set_error(error_message.str());
}

/******************************************************************************/
/**
 * @brief Parses the external evaluation result file into the per-criterion result vector, validating the
 * individual count / result count and honouring the individual's isValid flag (worst-casing + flagging on
 * an invalid result). Extracted from evaluate() (the result-parse branch).
 *
 * @param result_file_name The JSON result file written by the external evaluation program
 * @param results The per-criterion result vector to fill (modified in place)
 */
void GExternalEvaluatorIndividual::parseExternalResults_(
    const std::string &result_file_name,
    std::vector<double> &results
) {
    namespace json = boost::json;

    // Check that the result file exists
    if(not std::filesystem::exists(result_file_name)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::evaluate(): Error!" << '\n'
            << "Result file " << result_file_name << " does not seem to exist." << '\n'
        );
    }

    // Parse the results (GJsonIO throws a geneva_exception with the file name on a malformed file)
    json::value result_doc = Gem::Common::parseJsonFile(result_file_name);

    try {
        json::object const &root = result_doc.as_object();

        // Check that only a single result individual was returned
        auto n_external_individuals = root.at("n_individuals").to_number<std::size_t>();
        if(1 != n_external_individuals) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GExternalEvaluatorIndividual::evaluate(): Error!" << '\n'
                << "Number of result individuals != 1: " << n_external_individuals << '\n'
            );
        }

        json::object const &result_individual = root.at("individuals").as_array().at(0).as_object();

        // Check that the number of results provided by the result file matches the number of expected results
        auto external_n_results = result_individual.at("n_results").to_number<std::size_t>();
        if(external_n_results != n_results_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GExternalEvaluatorIndividual::evaluate(): Error!" << '\n'
                << "Result file provides n_results = " << external_n_results << '\n'
                << "while we expected " << n_results_ << '\n'
            );
        }

        // Check whether the results represent useful values
        bool const is_valid = result_individual.at("isValid").as_bool();
        if(not is_valid) {                    // Assign worst-case values to all result
            std::ostringstream error_message; // NOLINT(cppcoreguidelines-init-variables)

            error_message << "In GExternalEvaluatorIndividual::evaluate():" << '\n'
                          << "individuals[0].isValid is \"false\"" << '\n';

#ifdef DEBUG
            glogger << error_message.str() << GWARNING;
#endif

            for(double &r : results) {
                r = this->getWorstCase();
            }

            // Make sure the individual can be recognized as invalid by Geneva
            this->force_set_error(error_message.str());
        }
        else { // Extract and store all result values
            json::array const &results_node = result_individual.at("results").as_array();

            for(std::size_t res = 0; res < n_results_; res++) {
                results[res] = results_node.at(res).as_object().at("rawResult").to_number<double>();
            }
        }
    }
    catch(const geneva_exception &) {
        throw; // re-throw our own diagnostics untouched
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::evaluate(): Error reading "
            << result_file_name << '\n'
            << "with message " << e.what() << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Allows to assign a run-id to this individual.
 *
 * @param run_id The run identifier to assign; must be non-empty and not the literal "empty"
 */
void GExternalEvaluatorIndividual::setRunId(const std::string& run_id) {
    if(run_id.empty() || "empty" == run_id) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::setRunId(): Error!" << '\n'
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
 * @brief Allows to specify whether temporary files should be removed. This is mostly needed for debugging purposes.
 *
 * @param remove_exec_temporaries If true, the IPC temporary files are removed after each external evaluation
 */
void GExternalEvaluatorIndividual::setRemoveExecTemporaries(bool remove_exec_temporaries) {
    remove_exec_temporaries_ = remove_exec_temporaries;
}

/******************************************************************************/
/**
 * @brief Allows to check whether temporaries should be removed.
 *
 * @return true if IPC temporary files are removed after each external evaluation, false otherwise
 */
bool GExternalEvaluatorIndividual::getRemoveExecTemporaries() const {
    return remove_exec_temporaries_;
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
    gpb.registerFileParameter<std::uint32_t>("adaption_threshold", c.adaption_threshold, GEEI_DEF_ADAPTIONTHRESHOLD)
        << "The number of calls to an adaptor after which adaption takes place";
    gpb.registerFileParameter<bool>("use_bi_gaussian", c.use_bi_gaussian, GEEI_DEF_USEBIGAUSSIAN)
        << "Whether to use a double gaussion for the adaption of parmeters in ES";
    gpb.registerFileParameter<double>("sigma1", c.sigma1, GEEI_DEF_SIGMA1)
        << "The sigma for gauss-adaption in ES" << '\n'
        << "(or the sigma of the left peak of a double gaussian)";
    gpb.registerFileParameter<double>("sigma_sigma1", c.sigma_sigma1, GEEI_DEF_SIGMASIGMA1)
        << "Influences the self-adaption of gauss-mutation in ES";
    gpb.registerFileParameter<double>("min_sigma1", c.min_sigma1, GEEI_DEF_MINSIGMA1) << "The minimum value of sigma1";
    gpb.registerFileParameter<double>("max_sigma1", c.max_sigma1, GEEI_DEF_MAXSIGMA1) << "The maximum value of sigma1";
    gpb.registerFileParameter<double>("sigma2", c.sigma2, GEEI_DEF_SIGMA2)
        << "The sigma of the right peak of a double gaussian (if any)";
    gpb.registerFileParameter<double>("sigma_sigma2", c.sigma_sigma2, GEEI_DEF_SIGMASIGMA2)
        << "Influences the self-adaption of gauss-mutation in ES";
    gpb.registerFileParameter<double>("min_sigma2", c.min_sigma2, GEEI_DEF_MINSIGMA2) << "The minimum value of sigma2";
    gpb.registerFileParameter<double>("max_sigma2", c.max_sigma2, GEEI_DEF_MAXSIGMA2) << "The maximum value of sigma2";
    gpb.registerFileParameter<double>("delta", c.delta, GEEI_DEF_DELTA)
        << "The start distance between both peaks used for bi-gaussian mutations in ES";
    gpb.registerFileParameter<double>("sigma_delta", c.sigma_delta, GEEI_DEF_SIGMADELTA)
        << "The width of the gaussian used for mutations of the delta parameter";
    gpb.registerFileParameter<double>("min_delta", c.min_delta, GEEI_DEF_MINDELTA) << "The minimum allowed value of delta";
    gpb.registerFileParameter<double>("max_delta", c.max_delta, GEEI_DEF_MAXDELTA) << "The maximum allowed value of delta";
    gpb.registerFileParameter<std::string>("program_name", c.program_name, GEEI_DEF_PROGNAME)
        << "The name of the external evaluation program";
    gpb.registerFileParameter<std::string>("custom_options", c.custom_options, GEEI_DEF_CUSTOMOPTIONS)
        << "Any custom options you wish to pass to the external evaluator";
    gpb.registerFileParameter<std::string>("parameter_file", c.parameter_file_base_name, GEEI_DEF_PARFILEBASENAME)
        << "The base name assigned to parameter files" << '\n'
        << "in addition to data identifying this specific evaluation";
    gpb.registerFileParameter<std::string>("init_values", c.init_values, GEEI_DEF_STARTMODE)
        << "Indicates, whether individuals should be initialized randomly (random)," << '\n'
        << "with the lower (min) or upper (max) boundary of their value ranges";
    gpb.registerFileParameter<bool>("remove_exec_temporaries", c.remove_exec_temporaries, GEEI_DEF_REMOVETEMPORARIES)
        << "Indicates, whether files created during external execution should be removed";
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
    namespace json = boost::json;

    json::value setup_doc;

    { // Give the external program the opportunity to perform initial work
        std::vector<std::string> arguments;
        if(c.custom_options != "empty" && not c.custom_options.empty()) {
            arguments.push_back(c.custom_options);
        }
        arguments.emplace_back("--init");
        std::string command;
        int const error_code = Gem::Common::runExternalCommand(
            std::filesystem::path(c.program_name), arguments, std::filesystem::path(), command
        );
        if(error_code) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GExternalEvaluatorIndividual::buildGenome(//1//): Error" << '\n'
                << "Execution of external command failed." << '\n'
                << "Command: " << command << '\n'
                << "Error code: " << error_code << '\n'
            );
        }
    }

    { // Now ask the external program for setup information
        std::vector<std::string> arguments;
        if(c.custom_options != "empty" && not c.custom_options.empty()) {
            arguments.push_back(c.custom_options);
        }
        std::string const setup_file_name =
            std::string("./setup-") + Gem::Common::generate_uuid_v4() + std::string(".json");
        arguments.push_back("--setup");
        arguments.push_back("--output=\"" + setup_file_name + "\"");
        arguments.push_back("--initvalues=\"" + c.init_values + "\"");
        std::string command;
        int const error_code = Gem::Common::runExternalCommand(
            std::filesystem::path(c.program_name), arguments, std::filesystem::path(), command
        );
        if(error_code) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GExternalEvaluatorIndividual::buildGenome(//2//): Error" << '\n'
                << "Execution of external command failed." << '\n'
                << "Command: " << command << '\n'
                << "Error code: " << error_code << '\n'
            );
        }
        setup_doc = Gem::Common::parseJsonFile(setup_file_name);
        std::filesystem::remove(std::filesystem::path(setup_file_name));
    }

    if(not setup_doc.is_object()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::buildGenome(): Error!" << '\n'
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
static gen::GenomeData authorFlatGenomeFromSetup(
    const boost::json::value &setup_doc,
    GExternalEvaluatorIndividual::Config &c
) {
    namespace json = boost::json;

    // Author the flat genome: each discovered variable becomes one constrained double group. Fixed
    // variables (min == max) are built with adaptionMode::NEVER; active ones get their Gauss/bi-Gauss
    // adaptor from the OA-owned config (buildAdaptionConfig), not the layout.
    gen::GGenomeBuilder gb;

    try {
        json::object const &root = setup_doc.as_object();

        auto n_individuals = root.at("n_individuals").to_number<std::size_t>();
        if(1 != n_individuals) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GExternalEvaluatorIndividual::buildGenome(): Error!" << '\n'
                << "Received invalid number of setup-individuals: " << n_individuals << '\n'
            );
        }

        c.run_id = json::value_to<std::string>(root.at("run_id"));
        json::object const &setup_individual = root.at("individuals").as_array().at(0).as_object();
        auto n_var = setup_individual.at("nVars").to_number<std::size_t>();
        c.n_results_expected = setup_individual.at("n_results").to_number<std::size_t>();

        json::array const &var_set_node = setup_individual.at("vars").as_array();
        if(var_set_node.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GExternalEvaluatorIndividual::buildGenome(): Error!" << '\n'
                << "No variables were specified" << '\n'
            );
        }

        for(std::size_t var_counter = 0; var_counter < n_var && var_counter < var_set_node.size();
            ++var_counter) {
            json::object const &var_subtree = var_set_node.at(var_counter).as_object();
            std::string const var_type = json::value_to<std::string>(var_subtree.at("type"));
            if("GConstrainedDoubleObject" == var_type) {
                auto min_var = var_subtree.at("lowerBoundary").to_number<double>();
                auto max_var = var_subtree.at("upperBoundary").to_number<double>();
                auto init_value = var_subtree.at("values").as_array().at(0).to_number<double>();
                if(min_var == max_var) {
                    // Take this as a sign that the parameter should not be modified.
                    gb.addDouble(
                          init_value, init_value, std::max(1.0001 * init_value, init_value + 0.0001)
                      )
                        .adaptionMode(Gem::Geneva::adaptionMode::NEVER);
                }
                else {
                    gb.addDouble(init_value, min_var, max_var);
                }
            }
            else {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GExternalEvaluatorIndividual::buildGenome(): Error!" << '\n'
                    << var_type << " provided as type name." << '\n'
                    << "Currently only GConstrainedDoubleObject is supported." << '\n'
                );
            }
        }
    }
    catch(const geneva_exception &gec) {
        throw gec; // Re-throw
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::buildGenome(): Caught unknown exception!" << '\n'
        );
    }

    return gb.build();
}

/******************************************************************************/
/**
 * @brief Queries the external evaluator program for the desired structure of the individuals and builds the flat genome from it.
 *
 * This runs the program with --init then --setup, parses the returned XML, and builds the genome from
 * it. Because the generic factory builds the shared genome exactly once, the (expensive) external query
 * happens once too. The discovered
 * run-id and result count are recorded back into @p c so applyConfig() can hand them to each produced
 * individual.
 *
 * @param c The Config supplying the program name / custom options / init mode; mutated in place with the discovered run-id and expected result count
 * @return The flat genome structure (GenomeData) describing the discovered variables
 */
gen::GenomeData GExternalEvaluatorIndividual::buildGenome(Config &c) {
    if(c.program_name.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::buildGenome(): Error!" << '\n'
            << "Program name was empty" << '\n'
        );
    }
    if(not std::filesystem::exists(c.program_name)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::buildGenome(): Error!" << '\n'
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
GExternalEvaluatorIndividual::buildAdaptionConfig(const gen::GGenome &sample, const Config &c) {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
    const auto &groups = cfg->doubleGroups();
    for(std::size_t i = 0; i < groups.size(); i++) {
        if(not groups[i].active) {
            continue; // a fixed (adaptionMode::NEVER) parameter -- never adapted, no adaptor
        }
        if(c.use_bi_gaussian) {
            cfg->groupDouble(i).biGauss(
                c.sigma1, c.sigma_sigma1, c.min_sigma1, c.max_sigma1, c.sigma2, c.sigma_sigma2,
                c.min_sigma2, c.max_sigma2, c.delta, c.sigma_delta, c.min_delta, c.max_delta, c.ad_prob,
                /*use_symmetric_sigmas=*/false, c.adapt_ad_prob, c.adaption_threshold
            );
        }
        else {
            cfg->groupDouble(i).gauss(
                c.sigma1, c.sigma_sigma1, c.min_sigma1, c.max_sigma1, c.ad_prob, c.adapt_ad_prob,
                c.adaption_threshold, Gem::Geneva::adaptionMode::WITHPROBABILITY, c.min_ad_prob, c.max_ad_prob
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
 * count).
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
    ind.setRunId(c.run_id);
}

/******************************************************************************/
/**
 * @brief Teardown hook giving the external evaluator program a chance to perform final work (--finalize).
 *
 * Called by GIndividualFactory's destructor once a genome has been produced. Errors here are fatal.
 *
 * @param c The Config supplying the program name and custom options used to invoke the external program with --finalize
 */
void GExternalEvaluatorIndividual::finalize(const Config &c) {
    if(c.program_name.empty()) {
        glogger << "In GExternalEvaluatorIndividual::finalize(): Error!" << '\n'
                << "Program name was empty" << '\n'
                << LOGEXIT(EXIT_FAILURE);
    }
    if(not std::filesystem::exists(c.program_name)) {
        glogger << "In GExternalEvaluatorIndividual::finalize(): Error!" << '\n'
                << "External program " << c.program_name << " does not seem to exist" << '\n'
                << LOGEXIT(EXIT_FAILURE);
    }

    std::vector<std::string> arguments;
    if(c.custom_options != "empty" && not c.custom_options.empty()) {
        arguments.push_back(c.custom_options);
    }
    arguments.emplace_back("--finalize");

    std::string command;
    int const error_code = Gem::Common::runExternalCommand(
        std::filesystem::path(c.program_name), arguments, std::filesystem::path(), command
    );
    if(error_code) {
        glogger << "In GExternalEvaluatorIndividual::finalize(): Error" << '\n'
                << "Execution of external command failed." << '\n'
                << "Command: " << command << '\n'
                << "Error code: " << error_code << '\n'
                << LOGEXIT(EXIT_FAILURE);
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

    std::string const parameterfile_name =
        arch.front()->getExchangeBaseName() + "-" + Gem::Common::generate_uuid_v4() + ".json";

    Gem::Common::writeJsonFile(parameterfile_name, batch_out);

    std::vector<std::string> arguments;
    const std::string custom_options = arch.front()->getCustomOptions();
    if(custom_options != "empty" && not custom_options.empty()) {
        arguments.push_back(custom_options);
    }
    arguments.emplace_back("--archive");
    arguments.push_back(std::string("--input=\"" + parameterfile_name + "\""));

    std::string command;
    int const error_code = Gem::Common::runExternalCommand(
        std::filesystem::path(arch.front()->getProgramName()), arguments, std::filesystem::path(), command
    );
    if(error_code) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GExternalEvaluatorIndividual::archive(): Error" << '\n'
            << "Execution of external command failed." << '\n'
            << "Command: " << command << '\n'
            << "Error code: " << error_code << '\n'
        );
    }

    std::filesystem::remove(parameterfile_name);
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
