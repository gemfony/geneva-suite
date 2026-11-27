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

#include "common/GChecks.hpp"
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
#include "weft/GJsonArchive.hpp"        // GJson[IO]Archive -- training-data disk persistence codec
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <geneva/individuals/GNeuralNetworkIndividual.hpp>
#include <istream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <random>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The nested value structs trainingSet / networkData are NOT wire-polymorphic (no
// gemfony_common_root_t; they travel by value in the saveToDisk training-data path,
// off the individual's wire serialize), so only the individual itself is registered
// for GArchive polymorphic dispatch.
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GNeuralNetworkIndividual) // NOLINT
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
 * @param limit The maximum allowed deviation of two floating point values under FP_SIMILARITY
 */
void trainingSet::compare(
    const trainingSet &cp,
    const Gem::Common::expectation &e,
    const double &limit
) const {
    using namespace Gem::Common;

    Gem::Common::GToken token("trainingSet", e, limit);

    // Compare our local data
    Gem::Common::compare_t(
        Gem::Common::getIdentity(nInputNodes, cp.nInputNodes, "nInputNodes", "cp.nInputNodes"),
        token
    );
    Gem::Common::compare_t(
        Gem::Common::getIdentity(nOutputNodes, cp.nOutputNodes, "nOutputNodes", "cp.nOutputNodes"),
        token
    );

    for(std::size_t i = 0; i < nInputNodes; i++) {
        Gem::Common::compare_t(
            Gem::Common::getIdentity(Input[i], cp.Input[i], "Input[i]", "cp.Input[i]"),
            token
        );
    }

    for(std::size_t i = 0; i < nOutputNodes; i++) {
        Gem::Common::compare_t(
            Gem::Common::getIdentity(Output[i], cp.Output[i], "Output[i]", "cp.Output[i]"),
            token
        );
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
 * @param limit The maximum allowed deviation of two floating point values under FP_SIMILARITY
 */
void networkData::compare(
    const networkData &cp,
    const Gem::Common::expectation &e,
    const double &limit
) const {
    Gem::Common::GToken token("networkData", e, limit);

    // Compare our local data
    Gem::Common::compare_t(
        Gem::Common::getIdentity(data_.size(), cp.data_.size(), "data_.size()", "cp.data_.size()"),
        token
    );
    Gem::Common::compare_t(
        Gem::Common::getIdentity(this->data_cnt_, cp.data_cnt_, "this->data_cnt_", "cp.data_cnt_"),
        token
    );

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
        throw gem_exception(
            g_error_streamer() << "Data file " << network_data_file
                               << " could not be opened for writing." << '\n'
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
        std::ostringstream error;
        error << "In networkData::loadFromDisk(const std::string&):" << '\n'
              << "Data file " << network_data_file << " could not be opened for reading." << '\n';

        if(not std::filesystem::exists(network_data_file.c_str())) {
            error << "File does not exist." << '\n';
        }

        throw gem_exception(g_error_streamer() << error.str() << '\n');
    }

    // Load the data via the GArchive JSON codec. A deserialization failure (a truncated file, or --
    // most commonly -- a training-data file written by an incompatible Geneva version) otherwise
    // escapes as an uncaught exception; catch it and bail out with an actionable diagnostic instead.
    networkData loaded(0);
    try {
        std::string content(
            (std::istreambuf_iterator<char>(tr_dat)),
            std::istreambuf_iterator<char>()
        );
        Gem::Weft::GJsonIArchive ia(content);
        ia &Gem::Weft::make_nvp("networkData", loaded);
    }
    catch(const std::exception &e) {
        throw gem_exception(
            g_error_streamer()
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
        throw gem_exception(
            g_error_streamer() << "pos = " << pos
                               << " exceeds end of array (size = " << data_.size() << ")" << '\n'
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
 * @return The number of input nodes (the first stored layer size)
 */
std::size_t networkData::getNInputNodes() const {
    return this->front();
}

/******************************************************************************/
/**
 * Retrieves the number of output nodes of this network
 *
 * @return The number of output nodes (the last stored layer size)
 */
std::size_t networkData::getNOutputNodes() const {
    return this->back();
}

/******************************************************************************/
/**
 * @brief The figure of the training data: the input points of all training sets in the plane of
 * the two inputs, as two series -- the sets whose desired output is below 0.5, and those at or above
 * it. A series without points is left out. The frame holds all points. Every dietrich backend can
 * draw the figure. Training data without exactly two input nodes and one output node have no such
 * figure and are refused.
 *
 * @return The figure, to be written through any dietrich backend
 */
Gem::Dietrich::GPlotDesigner networkData::trainingDataPlot() const {
    Gem::Common::checkThat(
        getNInputNodes() == 2 and getNOutputNodes() == 1,
        "The figure of the training data needs two input nodes and one output node; got ",
        getNInputNodes(),
        " and ",
        getNOutputNodes()
    );

    std::array<std::vector<std::tuple<double, double>>, 2> points; // output < 0.5, output >= 0.5
    for(const auto &t_s : data_) {
        if(t_s) { // a slot that holds no training set has nothing to show
            points.at(t_s->Output[0] < 0.5 ? 0 : 1).emplace_back(t_s->Input[0], t_s->Input[1]);
        }
    }

    const std::array<std::string, 2> labels{"desired output < 0.5", "desired output >= 0.5"};
    std::shared_ptr<Gem::Dietrich::GGraph2D> primary;
    for(std::size_t i = 0; i < points.size(); ++i) {
        if(points.at(i).empty()) {
            continue;
        }
        auto series = std::make_shared<Gem::Dietrich::GGraph2D>();
        series->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
        series->setPlotLabel(labels.at(i));
        series->setXAxisLabel("input 1");
        series->setYAxisLabel("input 2");
        (*series) & points.at(i);
        if(primary) {
            primary->registerSecondaryPlotter(series);
        }
        else {
            primary = series;
        }
    }
    Gem::Common::checkThat(primary != nullptr, "The training data hold no training set");
    primary->setPadTitle("Training data in the plane of the two inputs");
    primary->setLegendPosition(Gem::Dietrich::legendPosition::FREEST);

    Gem::Dietrich::GPlotDesigner gpd("Original training data", 1, 1);
    gpd.setCanvasDimensions(1024, 1024);
    gpd.registerPlotter(primary);
    return gpd;
}

/******************************************************************************/
/**
 * @brief Writes the figure of the training data (trainingDataPlot()) as a ROOT macro, for visual
 * inspection. Training data without two input nodes and one output node have no such figure; the
 * function then warns and writes nothing.
 *
 * @param output_file The name of the file the ROOT macro is written to
 */
void networkData::toROOT(const std::string &output_file) const {
    if(getNInputNodes() != 2 || getNOutputNodes() != 1) {
        glogger << "Got inappropriate number of input and/or output nodes: " << getNInputNodes()
                << "/" << getNOutputNodes() << '\n'
                << "We need 2/1. The function will return without further action." << '\n'
                << GWARNING;
        return;
    }

    Gem::Dietrich::GPlotDesigner gpd = this->trainingDataPlot();
    gpd.setPlotBackend(Gem::Dietrich::plotBackend::ROOT);
    gpd.writeToFile(output_file);
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
    std::ostringstream result;

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
    auto result = std::make_shared<networkData>(*this);
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
    Gem::Common::checkThat(nullptr != n_d_, "No network data appears to have been registered.");

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
 * @brief The figure of the network's output over its two input dimensions: the output of the first
 * output node at OUTPUT_PLOT_POINTS_PER_AXIS x OUTPUT_PLOT_POINTS_PER_AXIS points of a regular grid
 * over the range of the training data, drawn as a surface whose colour map is projected onto the
 * floor of the plot box. Every dietrich backend can draw it.
 *
 * The range is the one the training data report (networkData::getInitRange()), [0, 1] in both
 * dimensions if they report none. A network without two input nodes has no such figure and is
 * refused.
 *
 * @return The figure, to be written through any dietrich backend
 */
Gem::Dietrich::GPlotDesigner GNeuralNetworkIndividual::outputPlot() const {
    Gem::Common::checkThat(
        n_d_->getNInputNodes() == 2,
        "The figure of the network's output needs two input nodes; the network has ",
        n_d_->getNInputNodes()
    );

    std::tuple<double, double> x_range{0., 1.};
    std::tuple<double, double> y_range{0., 1.};
    if(const auto range = n_d_->getInitRange(); range.size() == 2) {
        x_range = range[0];
        y_range = range[1];
    }
    // The i-th of the grid's points in a range
    const auto grid_point = [](const std::tuple<double, double> &range, std::size_t i) {
        const auto [low, high] = range;
        return low + ((high - low) * static_cast<double>(i) /
                      static_cast<double>(OUTPUT_PLOT_POINTS_PER_AXIS - 1));
    };

    auto const w = this->fpParameterValues();
    auto surface = std::make_shared<Gem::Dietrich::GGraph3D>();
    for(std::size_t i = 0; i < OUTPUT_PLOT_POINTS_PER_AXIS; ++i) {
        for(std::size_t j = 0; j < OUTPUT_PLOT_POINTS_PER_AXIS; ++j) {
            const std::array<double, 2> input{grid_point(x_range, i), grid_point(y_range, j)};
            const double output = this->networkOutput(w, input).front();
            (*surface) & std::tuple<double, double, double>(input[0], input[1], output);
        }
    }
    surface->setSurfaceProjection(Gem::Dietrich::surfaceProjection::FLOOR);
    surface->setXAxisLabel("input 1");
    surface->setYAxisLabel("input 2");
    surface->setZAxisLabel("network output");
    surface->setPlotLabel("Network output over the range of the training data");

    Gem::Dietrich::GPlotDesigner gpd(
        "Output of the feed-forward network with geometry " + n_d_->getNetworkGeometryString(),
        1,
        1
    );
    gpd.setCanvasDimensions(1024, 1024);
    gpd.registerPlotter(surface);
    return gpd;
}

/******************************************************************************/
/**
 * @brief Writes the figure of the network's output over its two input dimensions (outputPlot())
 * to a file, through a dietrich backend: a ROOT macro by default, a gnuplot, matplotlib or Octave
 * script, or the plain data. A network without two input nodes has no such figure; the function
 * then warns and writes nothing.
 *
 * @param vis_file The name of the file the figure is written to
 * @param backend The dietrich backend the figure is written through
 */
void GNeuralNetworkIndividual::writeVisualizationFile(
    const std::string &vis_file,
    Gem::Dietrich::plotBackend backend
) const {
    Gem::Common::checkThat(not vis_file.empty(), "Received empty file name.");

    if(n_d_->getNInputNodes() != 2) {
        glogger << "The figure of the network's output needs two input nodes; the network has "
                << n_d_->getNInputNodes() << ". No file was written." << '\n'
                << GWARNING;
        return;
    }

    Gem::Dietrich::GPlotDesigner gpd = this->outputPlot();
    gpd.setPlotBackend(backend);
    gpd.writeToFile(vis_file);
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
    Gem::Common::checkThat(not header_file.empty(), "Received empty file name.");

    std::ofstream header(header_file);
    Gem::Common::checkThat(header.is_open(), "Could not open the output file ", header_file);

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
           << "    bool network(const std::vector<double>& in, std::vector<double>& out){" << '\n'
           << "      double node_result=0.;" << '\n'
           << '\n'
           << "      std::size_t node_counter = 0;" << '\n'
           << "      std::size_t prev_node_counter = 0;" << '\n'
           << '\n'
           << "      const std::size_t n_layers = " << n_d_->size() << ";" << '\n'
           << "      const std::size_t architecture[n_layers] = {" << '\n';

    header << (*n_d_ |
               std::views::transform([](std::size_t n) { return std::format("        {}", n); }) |
               std::views::join_with(std::string_view{",\n"}) | std::ranges::to<std::string>())
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
    auto const all_weights = this->fpParameterValues();
    header << (all_weights |
               std::views::transform([](double w) { return std::format("        {:g}", w); }) |
               std::views::join_with(std::string_view{",\n"}) | std::ranges::to<std::string>())
           << '\n';

    header << "      };" << '\n'
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
           << "        node_result=in[node_counter] * weights[2*node_counter] - "
              "weights[2*node_counter+1];"
           << '\n'
           << "        node_result=transfer(node_result);" << '\n'
           << "        prev_results.push_back(node_result);" << '\n'
           << "      }" << '\n'
           << '\n'
           << "      // All other layers" << '\n'
           << "      for(std::size_t layer_counter=1; layer_counter<n_layers; "
              "layer_counter++){"
           << '\n'
           << "        std::vector<double> current_results;" << '\n'
           << "        n_layer_nodes=architecture[layer_counter];" << '\n'
           << "        n_prev_layer_nodes=architecture[layer_counter-1];" << '\n'
           << '\n'
           << "        // For each node" << '\n'
           << "        for(node_counter=0; node_counter<n_layer_nodes; node_counter++){" << '\n'
           << "          node_result=0.;" << '\n'
           << "          // Loop over all nodes of the previous layer" << '\n'
           << "          for(prev_node_counter=0; prev_node_counter<n_prev_layer_nodes; "
              "prev_node_counter++){"
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
           << "      // At this point prev_results should contain the output values of the output "
              "layer"
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
        n_d | std::ranges::to<std::vector<std::size_t>>()
    );
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
 * or sigmoid; the forward pass through the network is networkOutput(), which
 * the figure of outputPlot() shares.
 *
 * @return The training error as a one-element vector (single criterion)
 */
std::vector<double> GNeuralNetworkIndividual::evaluate() {
    double result = 0;

    // Read all weights out of the flat genome once (genome-agnostic §2 access); networkOutput()
    // decodes their per-layer meaning through the semantic architecture.
    auto const w = this->fpParameterValues();

    // Now loop over all data sets
    std::size_t current_pos = 0;
    std::optional<std::shared_ptr<trainingSet>> o;
    while((o = n_d_->getTrainingSet(current_pos++))) {
        // Retrieve a constant reference to the training data set for faster access
        const trainingSet &t_s = **o;

        // Calculate the error made and add it to the result
        const std::vector<double> outputs = this->networkOutput(w, t_s.Input);
        for(std::size_t node_counter = 0; node_counter < outputs.size(); node_counter++) {
            result += Gem::Common::gsquared(outputs[node_counter] - t_s.Output[node_counter]);
        }
    }

    // Let the audience know
    return {result};
}

/******************************************************************************/
/**
 * @brief The forward pass of the network: the values of the output nodes for one input pattern,
 * with the weights @p w. The caller reads the weights from the genome once (fpParameterValues()) and
 * passes them for every pattern; their per-layer meaning comes from the semantic architecture.
 *
 * @param w All weights of the network, in the order of the genome
 * @param input One input pattern, a value per input node
 * @return The values of the output nodes
 */
std::vector<double> GNeuralNetworkIndividual::networkOutput(
    const std::vector<double> &w,
    std::span<const double> input
) const {
    const GNeuralNetworkArchitecture &arch = this->architecture();
    Gem::Common::checkThat(
        input.size() == arch.layerSize(0),
        "Got an input pattern of ",
        input.size(),
        " values for a network with ",
        arch.layerSize(0),
        " input nodes"
    );

    // The input layer (layer 0): two weights per input node (multiplier + bias).
    std::vector<double> prev_results;
    std::size_t n_layer_nodes = arch.layerSize(0);
    double node_result = 0;
    const std::size_t input_offset = arch.layerOffset(0);
    for(std::size_t node_counter = 0; node_counter < n_layer_nodes; node_counter++) {
        node_result = (input[node_counter] * w[input_offset + (2 * node_counter)]) -
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

    // At this point prev_results contains the output values of the output layer
    return prev_results;
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
        throw gem_exception(g_error_streamer() << "Got invalid tranfer function " << t_f_ << '\n');
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
        "ad_prob",
        c.ad_prob,
        GNN_DEF_ADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The probability for random adaptions of values in evolutionary algorithms;"
    );
    gpb.registerFileParameter<double>(
        "adapt_ad_prob",
        c.adapt_ad_prob,
        GNN_DEF_ADAPTADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        "Determines the rate of adaption of ad_prob. Set to 0, if you do not need this feature;"
    );
    gpb.registerFileParameter<double>(
        "min_ad_prob",
        c.min_ad_prob,
        GNN_DEF_MINADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The lower allowed boundary for ad_prob-variation;"
    );
    gpb.registerFileParameter<double>(
        "max_ad_prob",
        c.max_ad_prob,
        GNN_DEF_MAXADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The upper allowed boundary for ad_prob-variation;"
    );
    gpb.registerFileParameter<double>(
        "sigma",
        c.sigma,
        GNN_DEF_SIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The sigma for gauss-adaption in ES;"
    );
    gpb.registerFileParameter<double>(
        "sigma_sigma",
        c.sigma_sigma,
        GNN_DEF_SIGMASIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        "Influences the self-adaption of gauss-mutation in ES;"
    );
    gpb.registerFileParameter<double>(
        "min_sigma",
        c.min_sigma,
        GNN_DEF_MINSIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The minimum amount value of sigma;"
    );
    gpb.registerFileParameter<double>(
        "max_sigma",
        c.max_sigma,
        GNN_DEF_MAXSIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The maximum amount value of sigma;"
    );
    gpb.registerFileParameter<double>(
        "min_var",
        c.min_var,
        GNN_DEF_MINVAR,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The lower boundary of the initialization range for parameters;"
    );
    gpb.registerFileParameter<double>(
        "max_var",
        c.max_var,
        GNN_DEF_MAXVAR,
        Gem::Common::VAR_IS_ESSENTIAL,
        "The upper boundary of the initialization range for parameters;"
    );
    gpb.registerFileParameter<transferFunction>(
        "transfer_function",
        c.t_f,
        GNN_DEF_TRANSFER,
        Gem::Common::VAR_IS_ESSENTIAL,
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
        throw gem_exception(
            g_error_streamer() << "Invalid number of layers supplied (" << n_layers << ")." << '\n'
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
            throw gem_exception(
                g_error_streamer()
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
            c.sigma,
            c.sigma_sigma,
            c.min_sigma,
            c.max_sigma,
            c.ad_prob,
            c.adapt_ad_prob,
            1,
            Gem::Geneva::adaptionMode::WITHPROBABILITY,
            c.min_ad_prob,
            c.max_ad_prob
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
        return std::make_shared<Gem::Geneva::Individuals::networkData>(
            neuralNetworkOptions()->get("trainingDataFile")
        );
    }
    return std::make_shared<Gem::Geneva::Individuals::networkData>(
        Gem::Geneva::Individuals::GNN_DEF_DATAFILE
    );
}

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
