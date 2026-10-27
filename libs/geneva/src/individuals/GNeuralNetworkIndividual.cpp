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

#include <geneva/individuals/GNeuralNetworkIndividual.hpp>
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GContainerT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GSingletonT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "weft/GJsonArchive.hpp"         // GJson[IO]Archive -- training-data disk persistence codec
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The nested value structs trainingSet / networkData are NOT wire-polymorphic (no
// gemfony_common_root_t; they travel by value in the saveToDisk training-data path,
// off the individual's wire serialize), so only the individual itself is registered
// for GArchive polymorphic dispatch.
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GNeuralNetworkIndividual)      // NOLINT
namespace Gem::Geneva::Individuals {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Initialization with the number of nodes
 *
 * Sizes the Input / Output vectors for the given dimensions and zero-initializes them.
 * All other special member functions are compiler-generated (see the class definition).
 *
 * @param n_input The number of input nodes (size of the Input vector)
 * @param n_output The number of output nodes (size of the Output vector)
 */
trainingSet::trainingSet(const std::size_t &n_input, const std::size_t &n_output)
  : nInputNodes(n_input)
  , nOutputNodes(n_output)
  , Input(n_input, 0.)
  , Output(n_output, 0.) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Searches for compliance with expectations with respect to another object
 * of the same type
 *
 * @param cp A constant reference to another trainingSet object to compare against
 * @param e The expected outcome of the comparison
 * @param limit The acceptable deviation limit (unused here; data are compared for identity)
 */
void trainingSet::compare(
    const trainingSet &cp,
    const Gem::Common::expectation &e,
    [[maybe_unused]] const double & limit
) const {
    using namespace Gem::Common;

    Gem::Common::GToken token("trainingSet", e);

    // Compare our local data
    Gem::Common::compare_t(Gem::Common::getIdentity(nInputNodes, cp.nInputNodes, "nInputNodes", "cp.nInputNodes"), token);
    Gem::Common::compare_t(Gem::Common::getIdentity(nOutputNodes, cp.nOutputNodes, "nOutputNodes", "cp.nOutputNodes"), token);

    for(std::size_t i = 0; i < nInputNodes; i++) {
        Gem::Common::compare_t(Gem::Common::getIdentity(Input[i], cp.Input[i], "Input[i]", "cp.Input[i]"), token);
    }

    for(std::size_t i = 0; i < nOutputNodes; i++) {
        Gem::Common::compare_t(Gem::Common::getIdentity(Output[i], cp.Output[i], "Output[i]", "cp.Output[i]"), token);
    }

    // React on deviations from the expectation
    token.evaluate();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The default constructor. Private, as it is only needed for (de-)serialization
 * purposes.
 */
networkData::networkData() { /* nothing */
}

/******************************************************************************/
/**
 * @brief Initialization with the amount of entries
 *
 * @param array_size The desired number of training-set slots (initially empty pointers)
 */
networkData::networkData(const std::size_t &array_size)
  : data_(array_size) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Initializes the object with data from a file
 *
 * @param network_data_file The name of a file holding the (serialized) training data to load
 */
networkData::networkData(const std::string &network_data_file) {
    this->loadFromDisk(network_data_file);
}

/******************************************************************************/
/**
 * @brief Deep-copies the training sets of another networkData object (an empty slot stays empty).
 *
 * @param cp The source object
 * @return The deep copy of cp's training-set slots
 */
std::vector<std::shared_ptr<trainingSet>>
networkData::deepCopyOfTrainingSets(const networkData &cp) {
    std::vector<std::shared_ptr<trainingSet>> copy;
    copy.reserve(cp.data_.size());
    for(const auto &t_s : cp.data_) {
        copy.push_back(t_s ? std::make_shared<trainingSet>(*t_s) : std::shared_ptr<trainingSet>{});
    }
    return copy;
}

/******************************************************************************/
/**
 * @brief Initializes with data from another networkData object
 *
 * @param cp A constant reference to another networkData object whose data is deep-copied
 */
networkData::networkData(const networkData &cp)
  : Gem::Common::GPodContainerT<std::size_t>(cp)
  , data_(deepCopyOfTrainingSets(cp))
  , init_range_(cp.init_range_) { /* nothing */
}

/******************************************************************************/
/**
 * A standard destructor.
 */
networkData::~networkData() = default;

/******************************************************************************/
/**
 * @brief Copies the data of another networkData object into this object.
 *
 * The former hand-written assignment silently dropped init_range_ (which IS serialized and copied by
 * the copy constructor) -- a copy-path drift now healed by assigning every member.
 *
 * @param cp A constant reference to another networkData object whose data is deep-copied
 * @return A reference to this object
 */
networkData &networkData::operator=(const networkData &cp) {
    if(this != &cp) {
        Gem::Common::GPodContainerT<std::size_t>::operator=(cp);
        data_ = deepCopyOfTrainingSets(cp);
        init_range_ = cp.init_range_;
    }
    return *this;
}

/******************************************************************************/
/**
 * @brief Searches for compliance with expectations with respect to another object
 * of the same type
 *
 * @param cp A constant reference to another networkData object to compare against
 * @param e The expected outcome of the comparison
 * @param limit The acceptable deviation limit (unused here; sizes are compared for identity)
 */
void networkData::compare(
    const networkData &cp,
    const Gem::Common::expectation &e,
    [[maybe_unused]] const double & limit
) const {
    Gem::Common::GToken token("networkData", e);

    // Compare our local data
    Gem::Common::compare_t(Gem::Common::getIdentity(data_.size(), cp.data_.size(), "data_.size()", "cp.data_.size()"), token);
    Gem::Common::compare_t(Gem::Common::getIdentity(this->data_cnt_, cp.data_cnt_, "this->data_cnt_", "cp.data_cnt_"), token);

    // React on deviations from the expectation
    token.evaluate();
}

/******************************************************************************/
/**
 * @brief Saves the data of this struct to disc
 *
 * @param network_data_file The name of the file the data should be saved to (GArchive JSON)
 */
void networkData::saveToDisk(const std::string &network_data_file) const {
    std::ofstream tr_dat(network_data_file);

    if(not tr_dat) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In networkData::saveToDisk(const std::string&) : Error!" << '\n'
            << "Data file " << network_data_file << " could not be opened for writing." << '\n'
        );
    }

    // Save the data via the GArchive JSON codec (self-describing, human-readable on disk).
    {
        Gem::Weft::GJsonOArchive oa;
        oa &Gem::Weft::make_nvp("networkData", const_cast<networkData &>(*this));
        tr_dat << oa.str();
    }

    tr_dat.close();
}

/******************************************************************************/
/**
 * @brief Loads training data from the disc
 *
 * @param network_data_file The name of the file from which the data should be loaded (GArchive JSON)
 */
void networkData::loadFromDisk(const std::string &network_data_file) {
    std::ifstream tr_dat(network_data_file.c_str());

    if(not tr_dat) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "In networkData::loadFromDisk(const std::string&):" << '\n'
              << "Data file " << network_data_file << " could not be opened for reading." << '\n';

        if(not std::filesystem::exists(network_data_file.c_str())) {
            error << "File does not exist." << '\n';
        }

        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace()) << error.str() << '\n'
        );
    }

    // Load the data via the GArchive JSON codec. A deserialization failure (a truncated file, or --
    // most commonly -- a training-data file written by an incompatible Geneva version) otherwise
    // escapes as an uncaught exception; catch it and bail out with an actionable diagnostic instead.
    networkData loaded(0);
    try {
        std::string content(
            (std::istreambuf_iterator<char>(tr_dat)), std::istreambuf_iterator<char>());
        Gem::Weft::GJsonIArchive ia(content);
        ia &Gem::Weft::make_nvp("networkData", loaded);
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In networkData::loadFromDisk(const std::string&):" << '\n'
            << "Failed to deserialise the training-data file" << '\n'
            << "  " << network_data_file << '\n'
            << "as a GArchive JSON archive. Reason:" << '\n'
            << "  " << e.what() << '\n'
            << "The file is most likely stale or was written by an incompatible version." << '\n'
            << "Regenerate it with:" << '\n'
            << "  GNeuralNetwork --trainingDataFile " << network_data_file
            << " --trainingDataType <1-4> --nDataSets <N>" << '\n'
        );
    }

    // Copy the data over, using our own operator=()
    *this = loaded;
}

/******************************************************************************/
/**
 * @brief Adds a new training set to the collection. Note that the training set isn't
 * cloned, simply a copy of the smart pointer is stored in the internal array.
 *
 * @param t_s A std::shared_ptr<trainingSet> object, pointing to the training set to store (shared, not cloned)
 * @param pos The position in the internal array in which the data set should be stored (must be < array size)
 */
void networkData::addTrainingSet(std::shared_ptr<trainingSet> t_s, const std::size_t &pos) {
    if(pos >= data_.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In networkData::addTrainingSet(): Error!" << '\n'
            << "pos = " << pos << " exceeds end of array (size = " << data_.size() << ")" << '\n'
        );
    }
    data_[pos] = std::move(t_s);
}

/******************************************************************************/
/**
 * @brief Retrieves a training set at a given position. If the position exceeds the size of the array,
 * a std::nullopt is returned which evaluates to "false".
 *
 * @param pos The position from which an item should be retrieved
 * @return The training set at the requested position, or std::nullopt if pos is out of range
 */
std::optional<std::shared_ptr<trainingSet>>
networkData::getTrainingSet(const std::size_t &pos) const {
    if(pos >= data_.size()) {
        return std::nullopt;
    }
            return data_[pos];
   
}

/******************************************************************************/
/**
 * Retrieves the number of input nodes of this network
 *
 * @return The number of input nodes of this network
 */
std::size_t networkData::getNInputNodes() const {
    return this->front();
}

/******************************************************************************/
/**
 * Retrieves the number of output nodes of this network
 *
 * @return The number of output nodes of this network
 */
std::size_t networkData::getNOutputNodes() const {
    return this->back();
}

/******************************************************************************/
/**
 * @brief Saves this data set in ROOT format for visual inspection. It assumes that the input dimension
 * is 2 and the output dimension is 1. It will generate two distributions that will be coloured
 * differently -- one with output < 0.5, the other with output >= 0.5.
 *
 * @param output_file The name of the file the ROOT visualization program should be written to
 * @param min The minimum axis value of the distribution to be displayed
 * @param max The maximum axis value of the distribution to be displayed
 */
void networkData::toROOT(const std::string &output_file, const double &min, const double &max) {
    // Check that we have a matching number of input nodes
    if(getNInputNodes() != 2 || getNOutputNodes() != 1) {
        glogger << "In networkData::toRoot(): Warning!" << '\n'
                << "Got inappropriate number of input and/or output nodes: " << getNInputNodes()
                << "/" << getNOutputNodes() << '\n'
                << "We need 2/1. The function will return without further action." << '\n'
                << GWARNING;
        return;
    }

    const std::size_t n_sets = data_.size();
    std::size_t entries1 = 0;
    std::size_t entries2 = 0;
    std::ofstream of(output_file);

    of << "{" << '\n'
       << "  gROOT->Reset();" << '\n'
       << "  gStyle->SetCanvasColor(0);" << '\n'
       << "  gStyle->SetStatBorderSize(1);" << '\n'
       << "  gStyle->SetOptStat(0);" << '\n'
       << '\n'
       << R"(  TCanvas *cc = new TCanvas("cc", "cc",0,0,1024,1024);)" << '\n'
       << '\n'
       << "  TPaveLabel* canvasTitle = new TPaveLabel(0.1,0.95,0.9,0.99, \"Original training "
          "data\");"
       << '\n'
       << "  canvasTitle->Draw();" << '\n'
       << '\n'
       << R"(  TPad* graphPad = new TPad("Graphs", "Graphs", 0.01, 0.01, 0.99, 0.94);)"
       << '\n'
       << "  graphPad->Draw();" << '\n'
       << "  graphPad->Divide(1,1);" << '\n'
       << '\n'
       << "  double xarr1[" << n_sets << "], yarr1[" << n_sets << "], xarr2[" << n_sets
       << "], yarr2[" << n_sets << "];" << '\n'
       << '\n'
       << "  // Filling the data sets" << '\n';

    for(std::size_t i = 0; i < n_sets; i++) {
        if(data_[i]->Output[0] < 0.5) {
            of << "  xarr1[" << entries1 << "] = " << data_[i]->Input[0] << ";" << '\n'
               << "  yarr1[" << entries1 << "] = " << data_[i]->Input[1] << ";" << '\n';
            entries1++;
        }
        else {
            of << "  xarr2[" << entries2 << "] = " << data_[i]->Input[0] << ";" << '\n'
               << "  yarr2[" << entries2 << "] = " << data_[i]->Input[1] << ";" << '\n';
            entries2++;
        }
    }

    of << '\n'
       << "  // Setting remaining entries to 0" << '\n'
       << "  for(std::size_t i=" << entries1 << "; i<" << n_sets << "; i++) {" << '\n'
       << "    xarr1[i] = 0.;" << '\n'
       << "    yarr1[i] = 0.;" << '\n'
       << "  }" << '\n'
       << "  for(std::size_t i=" << entries2 << "; i<" << n_sets << "; i++) {" << '\n'
       << "    xarr2[i] = 0.;" << '\n'
       << "    yarr2[i] = 0.;" << '\n'
       << "  }" << '\n'
       << '\n'
       << "  // Creation of suitable TGraph objects" << '\n'
       << "  TGraph *gr1 = new TGraph(" << entries1 << ", xarr1, yarr1);" << '\n'
       << "  TGraph *gr2 = new TGraph(" << entries2 << ", xarr2, yarr2);" << '\n'
       << '\n'
       << "  gr1->SetMarkerColor(17);" << '\n'
       << "  gr2->SetMarkerColor(14);" << '\n'
       << '\n'
       << "  gr1->SetMarkerStyle(21);" << '\n'
       << "  gr2->SetMarkerStyle(21);" << '\n'
       << '\n'
       << "  gr1->SetMarkerSize(0.35);" << '\n'
       << "  gr2->SetMarkerSize(0.35);" << '\n'
       << '\n'
       << "  gr2->GetXaxis()->SetLimits(" << min << ", " << max << ");" << '\n'
       << "  gr2->GetYaxis()->SetRangeUser(" << min << ", " << max << ");" << '\n'
       << '\n'
       << "  // Do the drawing" << '\n'
       << "  graphPad->cd(1);" << '\n'
       << "  gr2->Draw(\"AP\");" << '\n'
       << "  gr1->Draw(\"P,same\");" << '\n'
       << "}" << '\n';

    of.close();
}

/******************************************************************************/
/**
 * @brief Allows to check whether an initialization range has been set
 *
 * @return true if an initialization range has been registered, false otherwise
 */
bool networkData::initRangeSet() const {
    return not init_range_.empty();
}

/******************************************************************************/
/**
 * @brief Allows to set the initialization range
 *
 * @param init_range A vector of (lower, upper) tuples, one per input dimension, defining the init range
 */
void networkData::setInitRange(const std::vector<std::tuple<double, double>> &init_range) {
    init_range_ = init_range;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the initialization range
 *
 * @return A vector of (lower, upper) tuples, one per input dimension (empty if none was set)
 */
std::vector<std::tuple<double, double>> networkData::getInitRange() const {
    return init_range_;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve a string that describes the network geometry
 *
 * @return A dash-separated string of the per-layer node counts (e.g. "2-4-4-1")
 */
std::string networkData::getNetworkGeometryString() const {
    std::ostringstream result; // NOLINT(cppcoreguidelines-init-variables)

    for(std::size_t i = 0; i < this->size() - 1; i++) {
        result << this->at(i) << "-";
    }
    result << this->at(this->size() - 1);
    return result.str();
}

/******************************************************************************/
/**
 * @brief Creates a deep clone of this object
 *
 * @return A std::shared_ptr to a newly allocated deep copy of this networkData object
 */
std::shared_ptr<networkData> networkData::clone() const {
    // Lock access to this function
    std::scoped_lock<std::mutex> const lock(m_);
    std::shared_ptr<networkData> result(new networkData(*this));
    return result;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The default constructor
 */
GNeuralNetworkIndividual::GNeuralNetworkIndividual()
  : t_f_(GNN_DEF_TRANSFER)
  , n_d_(nnTrainingDataStore()) { /* nothing */
}

/******************************************************************************/
/**
 * @brief A constructor which initializes the individual with a suitable set of network layers. It
 * also loads the training data from file.
 *
 * The arguments are forwarded to init(), which builds the flat weight genome for the network
 * geometry held in the global training-data store with the given init range and Gauss settings.
 *
 * @param min The lower boundary of the initialization range for the weight parameters
 * @param max The upper boundary of the initialization range for the weight parameters
 * @param sigma The sigma (step width) for the Gauss adaptor
 * @param sigma_sigma Influences the self-adaption of sigma
 * @param min_sigma The lower allowed boundary for sigma
 * @param max_sigma The upper allowed boundary for sigma
 * @param ad_prob The probability for random adaptions of weight values
 * @param adapt_ad_prob The rate of adaption of ad_prob (0 disables ad_prob self-adaption)
 * @param min_ad_prob The lower allowed boundary for ad_prob variation
 * @param max_ad_prob The upper allowed boundary for ad_prob variation
 */
// NOLINTNEXTLINE(readability-function-size) -- forwarding constructor whose 10 parameters mirror GNeuralNetworkIndividual::Config's fields and delegate verbatim to init(); reducing the parameter count would require an API change, not a body split
GNeuralNetworkIndividual::GNeuralNetworkIndividual(
    const double &min,
    const double &max,
    const double &sigma,
    const double &sigma_sigma,
    const double &min_sigma,
    const double &max_sigma,
    const double &ad_prob,
    const double &adapt_ad_prob,
    const double &min_ad_prob,
    const double &max_ad_prob
)
  : t_f_(GNN_DEF_TRANSFER)
  , n_d_(nnTrainingDataStore()) {
    this->init(
        min,
        max,
        sigma,
        sigma_sigma,
        min_sigma,
        max_sigma,
        ad_prob,
        adapt_ad_prob,
        min_ad_prob,
        max_ad_prob
    );
}

/******************************************************************************/
/**
 * A standard copy constructor
 *
 * @param cp A copy of another GNeuralNetworkIndividual object
 */
GNeuralNetworkIndividual::GNeuralNetworkIndividual(const GNeuralNetworkIndividual &cp)
  : gen::GGenomeT<GNeuralNetworkIndividual>(cp)
  , t_f_(cp.t_f_)
  , n_d_(nnTrainingDataStore()) // We want a single source for the training data
{                             /* nothing */
}

/******************************************************************************/
/**
 * The standard destructor
 */
GNeuralNetworkIndividual::~GNeuralNetworkIndividual() { /* nothing */
}

/******************************************************************************/
/**
 * @brief A function which initializes the individual with a suitable set of network
 * layers, according to user-specifications.
 *
 * Builds the flat weight genome (via the static buildGenome() hook) for the network geometry
 * held in the global data store and (re)builds the cached semantic architecture.
 *
 * @param min The lower boundary of the initialization range for the weight parameters
 * @param max The upper boundary of the initialization range for the weight parameters
 * @param sigma The sigma (step width) for the Gauss adaptor
 * @param sigma_sigma Influences the self-adaption of sigma
 * @param min_sigma The lower allowed boundary for sigma
 * @param max_sigma The upper allowed boundary for sigma
 * @param ad_prob The probability for random adaptions of weight values
 * @param adapt_ad_prob The rate of adaption of ad_prob (0 disables ad_prob self-adaption)
 * @param min_ad_prob The lower allowed boundary for ad_prob variation
 * @param max_ad_prob The upper allowed boundary for ad_prob variation
 */
// NOLINTNEXTLINE(readability-function-size) -- fills the Config from all 10 constructor parameters and calls buildGenome(); the parameter list matches the Config struct it populates, so splitting the body would not reduce the parameter count
void GNeuralNetworkIndividual::init(
    const double &min,
    const double &max,
    const double &sigma,
    const double &sigma_sigma,
    const double &min_sigma,
    const double &max_sigma,
    const double &ad_prob,
    const double &adapt_ad_prob,
    const double &min_ad_prob,
    const double &max_ad_prob
) {
#ifdef DEBUG
    if(not n_d_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::init([...]): Error!" << '\n'
            << "No network data appears to have been registered." << '\n'
        );
    }
#endif /* DEBUG */

    // Build the flat weight genome for the network geometry held in the global data store, with the
    // supplied init range / Gauss settings. The genome construction is shared with the factory path
    // through the static buildGenome() hook; here we additionally (re)build the cached architecture.
    Config c;
    c.min_var = min;
    c.max_var = max;
    c.sigma = sigma;
    c.sigma_sigma = sigma_sigma;
    c.min_sigma = min_sigma;
    c.max_sigma = max_sigma;
    c.ad_prob = ad_prob;
    c.adapt_ad_prob = adapt_ad_prob;
    c.min_ad_prob = min_ad_prob;
    c.max_ad_prob = max_ad_prob;

    this->setGenome(buildGenome(c));

    // (Re)build the cached semantic architecture for this geometry.
    nn_arch_ = makeArchitecture(*n_d_);
}

/******************************************************************************/
/**
 * @brief Sets the type of the transfer function
 *
 * @param t_f The transfer function to use (SIGMOID or RBF)
 */
void GNeuralNetworkIndividual::setTransferFunction(transferFunction t_f) {
    t_f_ = t_f;
}

/******************************************************************************/
/**
 * @brief Retrieves the type of the transfer function
 *
 * @return The currently configured transfer function (SIGMOID or RBF)
 */
transferFunction GNeuralNetworkIndividual::getTransferFunction() const {
    return t_f_;
}

/******************************************************************************/
/**
 * @brief Creates a program which in turn creates a program suitable for visualization of optimization
 * results with the ROOT analysis framework (see http://root.cern.ch for further information).
 *
 * @param vis_file The name of the file the visualization program should be saved to
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent ROOT visualization-script code generator; the stream-emission of the literal generated program text is one unit and splitting it would scatter the generated source across functions
void GNeuralNetworkIndividual::writeVisualizationFile(const std::string &vis_file) {
    if(vis_file.empty() || vis_file.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::writeVisualizationFile(const std::string&) : Error"
            << '\n'
            << "Received empty file name." << '\n'
        );
    }

    std::ofstream vis_program(vis_file);
    if(not vis_program) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::writeVisualizationFile(const std::string&) :" << '\n'
            << "Attempt to open output file " << vis_file << " for writing failed." << '\n'
        );
    }

    // The following only makes sense if the input dimension is 2
    if(n_d_->getNInputNodes() == 2) {
        double x_low = 0.;
        double x_high = 1.;
        double y_low = 0.;
        double y_high = 1.;

        // Retrieve information about the initialization range
        // We only act if initialization ranges have been registered.
        // If not, than the above default values will be used.
        std::vector<std::tuple<double, double>> init_range = n_d_->getInitRange();
        if(init_range.size() == 2) {
            x_low = std::get<0>(init_range.at(0));
            x_high = std::get<1>(init_range.at(0));
            y_low = std::get<0>(init_range.at(1));
            y_high = std::get<1>(init_range.at(1));
        }

        // Write the header
        vis_program
            << "/**" << '\n'
            << " * @file visualization.C" << '\n'
            << " *" << '\n'
            << " * This program allows to visualize the output of the training example." << '\n'
            << " * It has been auto-generated by the GNeuralNetworkIndividual class of" << '\n'
            << " * the Geneva library" << '\n'
            << " *" << '\n'
            << " * Can be compiled with a command similar to" << '\n'
            << " * g++ -o visualization -I/opt/boost155/include/ visualization.C" << '\n'
            << " * e.g. on Ubuntu 14.04 (assuming that Boost is installed under /opt/boost155"
            << '\n'
            << " * in your system). The code should work with virtually any other" << '\n'
            << " * Linux distribution that supports Boost." << '\n'
            << " */" << '\n'
            << '\n'
            << "/*" << '\n'
            << " * This file is part of the Geneva library collection." << '\n'
            << " * The same license applies to this generated file." << '\n'
            << " */" << '\n'
            << '\n'
            << '\n'
            << "#include <iostream>" << '\n'
            << "#include <sstream>" << '\n'
            << "#include <fstream>" << '\n'
            << "#include <vector>" << '\n'
            << "#include <random>" << '\n'
            << '\n'
            << "#include <cstdint>" << '\n'
            << '\n'
            << "#include \"trainedNetwork.hpp\"" << '\n'
            << '\n'
            << "const std::uint32_t MAXPOINTS=20000;" << '\n'
            << '\n'
            << "using namespace Gem::NeuralNetwork;" << '\n'
            << '\n'
            << "int main(int argc, char**argv){" << '\n'
            << "  std::string geometry = \"" << n_d_->getNetworkGeometryString() << "\";" << '\n'
            << "  double x_low = " << x_low << ", x_high = " << x_high << ";" << '\n'
            << "  double y_low = " << y_low << ", y_high = " << y_high << ";" << '\n'
            << '\n'
            << "  boost::lagged_fibonacci607 lf(123);" << '\n'
            << '\n'
            << "  double x=0., y=0., result=0;" << '\n'
            << "  std::vector<double> in;" << '\n'
            << "  std::vector<double> out;" << '\n'
            << '\n'
            << "  std::vector<double> x01, y01;" << '\n'
            << "  std::vector<double> x02, y02;" << '\n'
            << "  std::vector<double> x03, y03;" << '\n'
            << "  std::vector<double> x04, y04;" << '\n'
            << "  std::vector<double> x05, y05;" << '\n'
            << "  std::vector<double> x06, y06;" << '\n'
            << "  std::vector<double> x07, y07;" << '\n'
            << "  std::vector<double> x08, y08;" << '\n'
            << "  std::vector<double> x09, y09;" << '\n'
            << "  std::vector<double> x10, y10;" << '\n'
            << '\n'
            << "  // Create random numbers and check the output" << '\n'
            << "  for(std::uint32_t i=0; i<MAXPOINTS; i++){" << '\n'
            << "    x=x_low + (x_high - x_low)*lf();" << '\n'
            << "    y=x_low + (y_high - y_low)*lf();" << '\n'
            << '\n'
            << "    in.clear();" << '\n'
            << "    out.clear();" << '\n'
            << '\n'
            << "    in.push_back(x);" << '\n'
            << "    in.push_back(y);" << '\n'
            << '\n'
            << "    if(!network(in,out) || out.size()==0){" << '\n'
            << "      std::cout << \"Error in calculation of network output\" << std::endl;" << '\n'
            << "      exit(1);" << '\n'
            << "    }" << '\n'
            << '\n'
            << "    double output = out[0];" << '\n'
            << '\n'
            << "    if(output < 0.1) {" << '\n'
            << "      x01.push_back(x);" << '\n'
            << "      y01.push_back(y);" << '\n'
            << "    } else if(output < 0.2) {" << '\n'
            << "      x02.push_back(x);" << '\n'
            << "      y02.push_back(y);" << '\n'
            << "    } else if(output < 0.3) {" << '\n'
            << "      x03.push_back(x);" << '\n'
            << "      y03.push_back(y);" << '\n'
            << "    } else if(output < 0.4) {" << '\n'
            << "      x04.push_back(x);" << '\n'
            << "      y04.push_back(y);" << '\n'
            << "    } else if(output < 0.5) {" << '\n'
            << "      x05.push_back(x);" << '\n'
            << "      y05.push_back(y);" << '\n'
            << "    } else if(output < 0.6) {" << '\n'
            << "      x06.push_back(x);" << '\n'
            << "      y06.push_back(y);" << '\n'
            << "    } else if(output < 0.7) {" << '\n'
            << "      x07.push_back(x);" << '\n'
            << "      y07.push_back(y);" << '\n'
            << "    } else if(output < 0.8) {" << '\n'
            << "      x08.push_back(x);" << '\n'
            << "      y08.push_back(y);" << '\n'
            << "    } else if(output < 0.9) {" << '\n'
            << "      x09.push_back(x);" << '\n'
            << "      y09.push_back(y);" << '\n'
            << "    } else {" << '\n'
            << "      x10.push_back(x);" << '\n'
            << "      y10.push_back(y);" << '\n'
            << "    }" << '\n'
            << "  }" << '\n'
            << '\n'
            << "  // Write test results" << '\n'
            << "  std::ostringstream results;" << '\n'
            << "  results" << '\n'
            << "  << \"{\" << std::endl" << '\n'
            << "  << \"  gROOT->Reset();\" << std::endl" << '\n'
            << "  << \"  gStyle->SetCanvasColor(0);\" << std::endl" << '\n'
            << "  << \"  gStyle->SetStatBorderSize(1);\" << std::endl" << '\n'
            << "  << \"  gStyle->SetOptStat(0);\" << std::endl" << '\n'
            << "  << std::endl" << '\n'
            << "  << \"  TCanvas *cc = new TCanvas(\\\"cc\\\", \\\"cc\\\",0,0,1024,1024);\" << "
               "std::endl"
            << '\n'
            << "  << std::endl" << '\n'
            << "  << \"  TPaveLabel* canvasTitle = new TPaveLabel(0.1,0.95,0.9,0.99, \\\"Output of "
               "Feedforward Neural Network with geometry \" << geometry << \"\\\");\" << std::endl"
            << '\n'
            << "  << \"  canvasTitle->Draw();\" << std::endl" << '\n'
            << "  << std::endl" << '\n'
            << "  << \"  TPad* graphPad = new TPad(\\\"Graphs\\\", \\\"Graphs\\\", 0.01, 0.01, "
               "0.99, 0.94);\" << std::endl"
            << '\n'
            << "  << \"  graphPad->Draw();\" << std::endl" << '\n'
            << "  << \"  graphPad->Divide(1,1);\" << std::endl" << '\n'
            << "  << std::endl" << '\n'
            << R"(  << "  double x01[" << x01.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y01[" << y01.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x02[" << x02.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y02[" << y02.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x03[" << x03.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y03[" << y03.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x04[" << x04.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y04[" << y04.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x05[" << x05.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y05[" << y05.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x06[" << x06.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y06[" << y06.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x07[" << x07.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y07[" << y07.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x08[" << x08.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y08[" << y08.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x09[" << x09.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y09[" << y09.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double x10[" << x10.size() << "];" << std::endl)" << '\n'
            << R"(  << "  double y10[" << y10.size() << "];" << std::endl)" << '\n'
            << "  << std::endl;" << '\n'
            << '\n'
            << "  for(std::size_t i=0; i<x01.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x01[" << i << "] = " << x01[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y01[" << i << "] = " << y01[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x02.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x02[" << i << "] = " << x02[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y02[" << i << "] = " << y02[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x03.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x03[" << i << "] = " << x03[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y03[" << i << "] = " << y03[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x04.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x04[" << i << "] = " << x04[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y04[" << i << "] = " << y04[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x05.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x05[" << i << "] = " << x05[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y05[" << i << "] = " << y05[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x06.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x06[" << i << "] = " << x06[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y06[" << i << "] = " << y06[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x07.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x07[" << i << "] = " << x07[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y07[" << i << "] = " << y07[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x08.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x08[" << i << "] = " << x08[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y08[" << i << "] = " << y08[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x09.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x09[" << i << "] = " << x09[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y09[" << i << "] = " << y09[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << "  for(std::size_t i=0; i<x10.size(); i++){" << '\n'
            << "    results" << '\n'
            << R"(    << "  x10[" << i << "] = " << x10[i] << ";" << std::endl)" << '\n'
            << R"(    << "  y10[" << i << "] = " << y10[i] << ";" << std::endl;)" << '\n'
            << "  }" << '\n'
            << '\n'
            << "   results" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  TGraph *inside01 = new TGraph(\" << x01.size() << \", x01, y01);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside02 = new TGraph(\" << x02.size() << \", x02, y02);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside03 = new TGraph(\" << x03.size() << \", x03, y03);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside04 = new TGraph(\" << x04.size() << \", x04, y04);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside05 = new TGraph(\" << x05.size() << \", x05, y05);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside06 = new TGraph(\" << x06.size() << \", x06, y06);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside07 = new TGraph(\" << x07.size() << \", x07, y07);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside08 = new TGraph(\" << x08.size() << \", x08, y08);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside09 = new TGraph(\" << x09.size() << \", x09, y09);\" << "
               "std::endl"
            << '\n'
            << "   << \"  TGraph *inside10 = new TGraph(\" << x10.size() << \", x10, y10);\" << "
               "std::endl"
            << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  graphPad->cd(1);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside01->GetHistogram()->SetTitle(\\\"Network outputs in the ranges "
               "[0:0.1], ... ,[0.9:1.0]\\\");\" << std::endl"
            << '\n'
            << "   << \"  inside01->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside01->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside01->SetMarkerColor(17);\" << std::endl" << '\n'
            << "   << \"  inside01->GetXaxis()->SetLimits(" << x_low << ", " << x_high
            << ");\" << std::endl" << '\n'
            << "   << \"  inside01->GetYaxis()->SetRangeUser(" << y_low << ", " << y_high
            << ");\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside02->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside02->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside02->SetMarkerColor(14);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside03->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside03->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside03->SetMarkerColor(17);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside04->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside04->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside04->SetMarkerColor(14);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside05->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside05->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside05->SetMarkerColor(17);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside06->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside06->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside06->SetMarkerColor(14);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside07->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside07->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside07->SetMarkerColor(17);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside08->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside08->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside08->SetMarkerColor(14);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside09->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside09->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside09->SetMarkerColor(17);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << "   << \"  inside10->SetMarkerStyle(21);\" << std::endl" << '\n'
            << "   << \"  inside10->SetMarkerSize(0.35);\" << std::endl" << '\n'
            << "   << \"  inside10->SetMarkerColor(14);\" << std::endl" << '\n'
            << "   << std::endl" << '\n'
            << R"( << "  inside01->Draw(\"AP\");" << std::endl)" << '\n'
            << R"( << "  inside02->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside03->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside04->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside05->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside06->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside07->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside08->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside09->Draw(\"P\");"  << std::endl)" << '\n'
            << R"( << "  inside10->Draw(\"P\");"  << std::endl)" << '\n'
            << " << \"}\" << std::endl;" << '\n'
            << '\n'
            << "  std::cout " << '\n'
            << "  << \"Writing test results into file testResults.C\" << std::endl" << '\n'
            << R"(  << "Test with the command \"root -l testResults.C\"" << std::endl;)" << '\n'
            << "  std::ofstream fstr(\"testResults.C\");" << '\n'
            << "  fstr << results.str();" << '\n'
            << "  fstr.close();" << '\n'
            << "}" << '\n';
    }
    else {
        glogger
            << "In GNeuralNetworkIndividual::writeVisualizationFile(const std::string&) : Warning!"
            << '\n'
            << "Request to create visualization program for more than two input dimensions!"
            << '\n'
            << "No action taken." << '\n'
            << GWARNING;
    }

    // Clean up
    vis_program.close();
}

/******************************************************************************/
/**
 * @brief Creates a C++ output file for the trained network, suitable for usage in
 * other projects.
 *
 * @param header_file The name of the header file the network should be saved in
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent C++ header code generator for the trained network; weight-offset computation and literal source emission are tightly interleaved with the generated output's layout, so splitting would scatter it
void GNeuralNetworkIndividual::writeTrainedNetwork(const std::string &header_file) {
    if(header_file.empty() || header_file.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::writeTrainedNetwork(const std::string&) : Error"
            << '\n'
            << "Received empty file name." << '\n'
        );
    }

    std::ofstream header(header_file);
    if(not header) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::writeTrainedNetwork(const std::string&) :" << '\n'
            << "Error writing output file " << header_file << '\n'
        );
    }

    header
        << "/**" << '\n'
        << " * @file " << header_file << '\n'
        << " *" << '\n'
        << " * This file represents the results of a feedforward neural network trained" << '\n'
        << " * using the Geneva library. It has been auto-generated by the GNeuralNetworkIndividual"
        << '\n'
        << " * class." << '\n'
        << " */" << '\n'
        << '\n'
        << "/*" << '\n'
        << " * This file is part of the Geneva library collection." << '\n'
        << " * The same license applies to this generated file." << '\n'
        << " */" << '\n'
        << '\n'
        << "#include <cmath>" << '\n'
        << "#include <vector>" << '\n'
        << '\n'
        << "#ifndef GENEVANEURALNETHEADER_HPP_" << '\n'
        << "#define GENEVANEURALNETHEADER_HPP_" << '\n'
        << '\n'
        << "namespace Gem" << '\n'
        << "{" << '\n'
        << "  namespace NeuralNetwork" << '\n'
        << "  {" << '\n'
        << "    double transfer(double value) {" << '\n';

    switch(t_f_) {
    case transferFunction::SIGMOID:
        header << "      return 1./(1.+exp(-value));" << '\n';
        break;
    case transferFunction::RBF:
        header << "      return exp(-value*value);" << '\n';
        break;
    }

    header << "    }" << '\n'
           << '\n'
           << "    bool network(const std::vector<double>& in, std::vector<double>& out){"
           << '\n'
           << "      double node_result=0.;" << '\n'
           << '\n'
           << "      register std::size_t node_counter = 0;" << '\n'
           << "      register std::size_t prev_node_counter = 0;" << '\n'
           << '\n'
           << "      const std::size_t n_layers = " << n_d_->size() << ";" << '\n'
           << "      const std::size_t architecture[n_layers] = {" << '\n';

    header << (*n_d_
               | std::views::transform([](std::size_t n) { return std::format("        {}", n); })
               | std::views::join_with(std::string_view{",\n"})
               | std::ranges::to<std::string>())
           << '\n';

    std::size_t weight_offset = 0;

    header << "      };" << '\n'
           << "      const std::size_t weight_offset[n_layers] = {" << '\n'
           << "        " << weight_offset << "," << '\n';

    weight_offset += 2 * (*n_d_)[0];
    header << "       " << weight_offset << "," << '\n';

    for(std::size_t i = 1; i < n_d_->size() - 1; i++) {
        weight_offset += (*n_d_)[i] * ((*n_d_)[i - 1] + 1);
        header << "        " << weight_offset;

        if(i == n_d_->size() - 1) {
            header << '\n';
        }
        else {
            header << "," << '\n';
        }
    }

    header << "      };" << '\n';

    std::size_t n_weights = 2 * (*n_d_)[0]; // NOLINT(cppcoreguidelines-init-variables)
    for(std::size_t i = 1; i < n_d_->size(); i++) {
        n_weights += (*n_d_)[i] * ((*n_d_)[i - 1] + 1);
    }

    header << "      const std::size_t n_weights = " << n_weights << ";" << '\n'
           << "      const double weights[n_weights] = {" << '\n';

    // The flat genome already stores the weights in layer-concatenated order (the same order the
    // architecture decodes), so dump them straight from the genome-agnostic §2 view.
    std::vector<double> all_weights;
    this->streamlineFP(all_weights);
    header << (all_weights
               | std::views::transform([](double w) { return std::format("        {:g}", w); })
               | std::views::join_with(std::string_view{",\n"})
               | std::ranges::to<std::string>())
           << '\n';

    header
        << "      };" << '\n'
        << '\n'
        << "      // Rudimentary error check" << '\n'
        << "      if(in.size() != architecture[0]) return false;" << '\n'
        << '\n'
        << "      // Clear the result vector" << '\n'
        << "      out.clear();" << '\n'
        << '\n'
        << "      // The input layer" << '\n'
        << "      std::vector<double> prev_results;" << '\n'
        << "      std::size_t n_layer_nodes = architecture[0];" << '\n'
        << "      std::size_t n_prev_layer_nodes = 0;" << '\n'
        << '\n'
        << "      for(node_counter=0; node_counter<n_layer_nodes; node_counter++){" << '\n'
        << "        node_result=in[node_counter] * weights[2*node_counter] - weights[2*node_counter+1];"
        << '\n'
        << "        node_result=transfer(node_result);" << '\n'
        << "        prev_results.push_back(node_result);" << '\n'
        << "      }" << '\n'
        << '\n'
        << "      // All other layers" << '\n'
        << "      for(register std::size_t layer_counter=1; layer_counter<n_layers; layer_counter++){"
        << '\n'
        << "        std::vector<double> current_results;" << '\n'
        << "        n_layer_nodes=architecture[layer_counter];" << '\n'
        << "        n_prev_layer_nodes=architecture[layer_counter-1];" << '\n'
        << '\n'
        << "        // For each node" << '\n'
        << "        for(node_counter=0; node_counter<n_layer_nodes; node_counter++){" << '\n'
        << "          node_result=0.;" << '\n'
        << "          // Loop over all nodes of the previous layer" << '\n'
        << "          for(prev_node_counter=0; prev_node_counter<n_prev_layer_nodes; prev_node_counter++){"
        << '\n'
        << "            node_result += "
           "prev_results[prev_node_counter]*weights[weight_offset[layer_counter] + "
           "node_counter*(n_prev_layer_nodes+1)+prev_node_counter];"
        << '\n'
        << "          }" << '\n'
        << "          node_result -= weights[weight_offset[layer_counter] + "
           "node_counter*(n_prev_layer_nodes+1)+n_prev_layer_nodes];"
        << '\n'
        << "          node_result = transfer(node_result);" << '\n'
        << "          current_results.push_back(node_result);" << '\n'
        << "        }" << '\n'
        << '\n'
        << "        prev_results=current_results;" << '\n'
        << "      }" << '\n'
        << '\n'
        << "      // At this point prev_results should contain the output values of the output layer"
        << '\n'
        << "      out=prev_results;" << '\n'
        << '\n'
        << "      return true;" << '\n'
        << "    }" << '\n'
        << '\n'
        << "  } /* namespace NeuralNetwork */" << '\n'
        << "} /* namespace Gem */" << '\n'
        << '\n'
        << "#endif /* GENEVANEURALNETHEADER_HPP_ */" << '\n';

    // Clean up
    header.close();
}

/******************************************************************************/
/**
 * @brief Builds the shared, immutable semantic architecture for a given network geometry (DM §4). The layer
 * sizes are read from the networkData; the architecture then exposes per-layer weight offsets into the
 * flat genome, layout-agnostically.
 *
 * @param n_d The network data describing the per-layer node counts (the network geometry)
 * @return A std::shared_ptr to a newly built, immutable GNeuralNetworkArchitecture for that geometry
 */
std::shared_ptr<const GNeuralNetworkArchitecture>
GNeuralNetworkIndividual::makeArchitecture(const networkData &n_d) {
    return std::make_shared<const GNeuralNetworkArchitecture>(
        n_d | std::ranges::to<std::vector<std::size_t>>());
}

/******************************************************************************/
/**
 * @brief Lazily (re)builds and returns the cached semantic architecture. The cache is transient (not
 * serialised, not copied), so it is rebuilt on first use after construction, copy or deserialisation;
 * the network geometry always comes from the (singleton-backed) networkData.
 *
 * @return A constant reference to the cached semantic architecture for this individual's geometry
 */
const GNeuralNetworkArchitecture &GNeuralNetworkIndividual::architecture() const {
    if(not nn_arch_) {
        nn_arch_ = makeArchitecture(*n_d_);
    }
    return *nn_arch_;
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation (i.e. the error calculation) takes place here. In the
 * case of a feed-forward network this fitness is equivalent to the error a network makes
 * for a given weight-set when trying to categorize a training set with known network output.
 * Minimizing this error means training the network.
 *
 * The error is implemented using the formula
 *
 * \f[
 * E(weights)=\sum_{\nu=1}^{p}\sum_{k}(y_{k}^{\nu}-s_{k}(x^{\nu}))^{2}
 * \f]
 *
 * where \f$p\f$ is the number of training patters (pairs of input/output
 * values), \f$k\f$ is the number of output nodes, \f$y_{k}^{\nu}\f$ is
 * the desired output value of output node \f$k\f$ for input pattern
 * \f$x^{\nu}\f$ and \f$s_{k}(x^{\nu})\f$ is the real output of output
 * node \f$k\f$ for input pattern \f$x^{\nu}\f$.
 *
 * The function "transfer()" used in this function can be either radial basis
 * or sigmoid.
 *
 * A small demonstration of the technique of storing a reference to a vector
 * in another vector is shown in the file refWrapper.cpp in the Geneva test cases.
 *
 * @return The fitness of this object
 */
std::vector<double> GNeuralNetworkIndividual::evaluate() {
    double result = 0;

    // Read all weights out of the flat genome once (genome-agnostic §2 access), then decode their
    // per-layer meaning through the semantic architecture.
    std::vector<double> w;
    this->streamlineFP(w);
    const GNeuralNetworkArchitecture &arch = this->architecture();

    // Now loop over all data sets
    std::size_t current_pos = 0;
    std::optional<std::shared_ptr<trainingSet>> o;
    while((o = n_d_->getTrainingSet(current_pos++))) {
        // Retrieve a constant reference to the training data set for faster access
        const trainingSet &t_s = **o;

        // The input layer (layer 0): two weights per input node (multiplier + bias).
        std::vector<double> prev_results;
        std::size_t n_layer_nodes = arch.layerSize(0);
        double node_result = 0;
        const std::size_t input_offset = arch.layerOffset(0);
        for(std::size_t node_counter = 0; node_counter < n_layer_nodes; node_counter++) {
            node_result = (t_s.Input[node_counter] * w[input_offset + (2 * node_counter)]) -
                          w[input_offset + (2 * node_counter) + 1];
            node_result = transfer(node_result);
            prev_results.push_back(node_result);
        }

        // All other layers: one weight per previous-layer node plus a bias, per node.
        std::size_t const n_layers = arch.nLayers();
        for(std::size_t layer_counter = 1; layer_counter < n_layers; layer_counter++) {
            std::vector<double> current_results;
            n_layer_nodes = arch.layerSize(layer_counter);
            std::size_t const n_prev_layer_nodes = arch.layerSize(layer_counter - 1);
            const std::size_t layer_offset = arch.layerOffset(layer_counter);

            for(std::size_t node_counter = 0; node_counter < n_layer_nodes; node_counter++) {
                // Loop over all nodes of the previous layer
                node_result = 0.;
                for(std::size_t prev_node_counter = 0; prev_node_counter < n_prev_layer_nodes;
                    prev_node_counter++) {
                    node_result +=
                        prev_results.at(prev_node_counter) *
                        w[layer_offset + (node_counter * (n_prev_layer_nodes + 1)) + prev_node_counter];
                }
                node_result -=
                    w[layer_offset + (node_counter * (n_prev_layer_nodes + 1)) + n_prev_layer_nodes];
                node_result = transfer(node_result);
                current_results.push_back(node_result);
            }

            prev_results = current_results;
        }

        // At this point prev_results should contain the output values of the output layer

        // Calculate the error made and add it to the result
        std::size_t const pref_results_size = prev_results.size();
        for(std::size_t node_counter = 0; node_counter < pref_results_size; node_counter++) {
            result += Gem::Common::gsquared(prev_results.at(node_counter) - t_s.Output[node_counter]);
        }
    }

    // Let the audience know
    return {result};
}

/******************************************************************************/
/**
 * @brief The transfer function, used to switch between radial basis and
 * sigmoid networks
 *
 * @param value The activation input to the node (weighted sum minus bias)
 * @return The node output after applying the configured transfer function (SIGMOID or RBF)
 */
double GNeuralNetworkIndividual::transfer(const double &value) const {
    switch(t_f_) {
    case transferFunction::SIGMOID: {
        return 1. / (1. + exp(-value));
    } break;

    case transferFunction::RBF: {
        return exp(-Gem::Common::gsquared(value));
    } break;

    default: {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::transfer(): Error!" << '\n'
            << "Got invalid tranfer function " << t_f_ << '\n'
        );
    } break;
    }

    // Make the compiler happy
    return 0.;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Registers the config-file options, binding them to the passed Config. The base
 * GGenome options are registered separately by GIndividualFactory::getObject_ (via
 * addConfigurationOptions).
 *
 * @param gpb The parser builder the file-parameter options are registered with
 * @param c The Config object whose members the registered options are bound to (filled on parse)
 */
void GNeuralNetworkIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    gpb.registerFileParameter<double>(
        "ad_prob", c.ad_prob, GNN_DEF_ADPROB, Gem::Common::VAR_IS_ESSENTIAL,
        "The probability for random adaptions of values in evolutionary algorithms;"
    );
    gpb.registerFileParameter<double>(
        "adapt_ad_prob", c.adapt_ad_prob, GNN_DEF_ADAPTADPROB, Gem::Common::VAR_IS_ESSENTIAL,
        "Determines the rate of adaption of ad_prob. Set to 0, if you do not need this feature;"
    );
    gpb.registerFileParameter<double>(
        "min_ad_prob", c.min_ad_prob, GNN_DEF_MINADPROB, Gem::Common::VAR_IS_ESSENTIAL,
        "The lower allowed boundary for ad_prob-variation;"
    );
    gpb.registerFileParameter<double>(
        "max_ad_prob", c.max_ad_prob, GNN_DEF_MAXADPROB, Gem::Common::VAR_IS_ESSENTIAL,
        "The upper allowed boundary for ad_prob-variation;"
    );
    gpb.registerFileParameter<double>(
        "sigma", c.sigma, GNN_DEF_SIGMA, Gem::Common::VAR_IS_ESSENTIAL,
        "The sigma for gauss-adaption in ES;"
    );
    gpb.registerFileParameter<double>(
        "sigma_sigma", c.sigma_sigma, GNN_DEF_SIGMASIGMA, Gem::Common::VAR_IS_ESSENTIAL,
        "Influences the self-adaption of gauss-mutation in ES;"
    );
    gpb.registerFileParameter<double>(
        "min_sigma", c.min_sigma, GNN_DEF_MINSIGMA, Gem::Common::VAR_IS_ESSENTIAL,
        "The minimum amount value of sigma;"
    );
    gpb.registerFileParameter<double>(
        "max_sigma", c.max_sigma, GNN_DEF_MAXSIGMA, Gem::Common::VAR_IS_ESSENTIAL,
        "The maximum amount value of sigma;"
    );
    gpb.registerFileParameter<double>(
        "min_var", c.min_var, GNN_DEF_MINVAR, Gem::Common::VAR_IS_ESSENTIAL,
        "The lower boundary of the initialization range for parameters;"
    );
    gpb.registerFileParameter<double>(
        "max_var", c.max_var, GNN_DEF_MAXVAR, Gem::Common::VAR_IS_ESSENTIAL,
        "The upper boundary of the initialization range for parameters;"
    );
    gpb.registerFileParameter<transferFunction>(
        "transfer_function", c.t_f, GNN_DEF_TRANSFER, Gem::Common::VAR_IS_ESSENTIAL,
        "The transferFunction: SIGMOID (0) or RBF/Radial Basis (1);"
    );
}

/******************************************************************************/
/**
 * @brief Builds the flat weight genome (structure only) for the network geometry held in the global training-
 * data store. Each weight is an unbounded double, random-initialised in [min_var, max_var) with that
 * perimeter (the OA re-randomises every population member within it). The per-layer / per-weight meaning
 * is provided by GNeuralNetworkArchitecture, not by the genome layout; the Gauss adaptor lives on the
 * OA-owned config (buildAdaptionConfig), authored from the same sigma / ad_prob parameters.
 *
 * @param c The Config providing the [min_var, max_var) init range / perimeter for each weight
 * @return The built flat genome (GenomeData) holding one double per network weight, layer-concatenated
 */
gen::GenomeData GNeuralNetworkIndividual::buildGenome(const Config &c) {
    using namespace Gem::Hap;

    auto n_d = nnTrainingDataStore();
    const std::size_t n_layers = n_d->size();
    if(n_layers < 2) { // Two layers are required at the minimum (3 and 4 layers are useful)
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::buildGenome(): Error!" << '\n'
            << "Invalid number of layers supplied (" << n_layers << ")." << '\n'
            << "Did you set up the network architecture ?" << '\n'
        );
    }

    GRandom gr_l;
    std::uniform_real_distribution<double> uniform_real_distribution(c.min_var, c.max_var);

    gen::GGenomeBuilder gb;
    std::size_t layer_number = 0;
    std::size_t n_nodes_previous = 0;
    for(const auto &layer_n_nodes : *n_d) {
        if(layer_n_nodes == 0) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GNeuralNetworkIndividual::buildGenome(): Error!" << '\n'
                << "Found invalid number of nodes in layer: " << layer_n_nodes << '\n'
                << "Did you set up the network architecture ?" << '\n'
            );
        }

        const std::size_t n_nodes = layer_n_nodes;
        const std::size_t n_weights =
            (layer_number == 0) ? (2 * n_nodes) : (n_nodes * (n_nodes_previous + 1));

        for(std::size_t i = 0; i < n_weights; i++) {
            gb.addDouble(uniform_real_distribution(gr_l)).perimeter(c.min_var, c.max_var);
        }

        n_nodes_previous = n_nodes;
        layer_number++;
    }

    return gb.build();
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration for a network genome: every weight (one double group each)
 * gets a Gauss adaptor with the configured parameters.
 *
 * @param sample A sample genome whose double-group structure the config is derived from
 * @param c The Config providing the Gauss adaptor parameters (sigma, ad_prob and their bounds)
 * @return A std::shared_ptr to the populated OA-owned adaption config
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GNeuralNetworkIndividual::buildAdaptionConfig(const gen::GGenome &sample, const Config &c) {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(
            c.sigma, c.sigma_sigma, c.min_sigma, c.max_sigma, c.ad_prob, c.adapt_ad_prob, 1,
            Gem::Geneva::adaptionMode::WITHPROBABILITY, c.min_ad_prob, c.max_ad_prob
        );
    }
    return cfg;
}

/******************************************************************************/
/**
 * @brief Per-object post-config hook (called by GIndividualFactory::postProcess_ after the genome is
 * installed): applies the non-genome transfer function to a produced individual.
 *
 * @param ind The individual to configure (its transfer function is set)
 * @param c The Config providing the transfer function to apply
 */
void GNeuralNetworkIndividual::applyConfig(GNeuralNetworkIndividual &ind, const Config &c) {
    ind.setTransferFunction(c.t_f);
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */

namespace Gem::Common {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A factory function for networkData objects, used by GSingletonT. It
 * queries a global options store for the name of the network data file
 *
 * @return A std::shared_ptr to a newly created networkData object (loaded from the configured
 *         training-data file, or from the default data file if none is configured)
 */
template <>
std::shared_ptr<Gem::Geneva::Individuals::networkData> TFactory_GSingletonT() {
    if(neuralNetworkOptions()->exists("trainingDataFile")) {
        return std::make_shared<Gem::Geneva::Individuals::networkData>(neuralNetworkOptions()->get("trainingDataFile"));
    }
            return std::make_shared<Gem::Geneva::Individuals::networkData>(Gem::Geneva::Individuals::GNN_DEF_DATAFILE);
   
}

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
