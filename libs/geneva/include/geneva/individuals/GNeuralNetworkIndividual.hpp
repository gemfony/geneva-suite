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

// Standard header files go here
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GGlobalOptionsT.hpp"
#include "common/GLogger.hpp"
#include "common/GContainerT.hpp"
#include "common/GSingletonT.hpp"
#include "common/GUnitTestFrameworkT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GGenomeArchitecture.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {
class GAdaptionConfigBase;
} // namespace Gem::Geneva::OptimizationAlgorithms

namespace Gem::Geneva::Individuals {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A single data set holding the training data of a single training iteration
 */
struct trainingSet {
    /////////////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    // A single, archive-generic serialize(): the Input / Output std::vectors are
    // serialized directly, so the container itself carries the element count and
    // load resizes automatically -- no make_array, no separate count field, no
    // pre-sizing, no split. (This is a local training-data file, never a wire item,
    // and archives may change under the clean-break policy, so the historical raw-
    // array layout no longer needs preserving.)
    //! [weft-archive-named]
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named(ar, "nInputNodes", nInputNodes);
        Gem::Common::archive_named(ar, "nOutputNodes", nOutputNodes);
        Gem::Common::archive_named(ar, "Input", Input);
        Gem::Common::archive_named(ar, "Output", Output);
    }
    //! [weft-archive-named]

    /////////////////////////////////////////////////////////////////////////////

    /**
     * @brief The constructor.
     * @param nInputNodes The number of input nodes (size of the Input vector)
     * @param nOutputNodes The number of output nodes (size of the Output vector)
     */
    trainingSet(const std::size_t & n_input, const std::size_t & n_output);

    // Rule of zero: with std::vector data members the compiler-generated
    // copy/move/assignment operations are deep, self-assignment-safe and
    // exception-safe (the former hand-written array management was neither).
    trainingSet(const trainingSet &) = default;
    trainingSet(trainingSet &&) = default;
    trainingSet &operator=(const trainingSet &) = default;
    trainingSet &operator=(trainingSet &&) = default;

    /** @brief The destructor */
    virtual ~trainingSet() = default;

    /**
     * @brief Searches for compliance with expectations with respect to another object of the same type.
     * @param cp The other object to compare against
     * @param e The expectation for this object, e.g. equality
     * @param limit The limit for allowed deviations of floating point types
     */
    virtual void compare(
        const trainingSet & cp
        ,
        const Gem::Common::expectation & e
        ,
        const double & limit
    ) const;

    /***************************************************************************/
    // Local data

    std::size_t nInputNodes = 0;  ///< The number of input nodes (== Input.size(); kept for the archive layout)
    std::size_t nOutputNodes = 0; ///< The number of output nodes (== Output.size(); kept for the archive layout)

    std::vector<double> Input;  ///< Holds the input data
    std::vector<double> Output; ///< Holds the output data

private:
    /** @brief The default constructor -- intentionally private, needed only for (de-)serialization */
    trainingSet() = default;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class holds all necessary information for the training of the neural network individual,
 * including the network's geometry. For intermediate storage on disk, we can serialize the
 * entire object using the GArchive serialization. networkData objects can themselves be
 * treated as std::vector. The idea is that the architecture is appended to the object, with
 * the first attached number being the input layer and the last one the output layer. In-between
 * numbers are hidden layers.
 */
class networkData : public Gem::Common::GPodContainerT<std::size_t> {
    /////////////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    // A single, archive-generic serialize(): the data_ vector of trainingSet
    // pointers is serialized directly, so the container carries its own element
    // count and each pointer is (de)serialized element-wise -- no make_array, no
    // separate array_size_ field, no manual overflow guard (this is a local, trusted
    // training-data file that never crosses the wire), and no split.
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Common::GPodContainerT<std::size_t>>(
            ar, "GStdSimpleVectorInterfaceT_size_t", *this);
        Gem::Common::archive_named(ar, "init_range_", init_range_);
        Gem::Common::archive_named(ar, "data_", data_);
    }

    /////////////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /**
     * @brief Initialization with the amount of entries.
     * @param nData The number of training sets this object will hold
     */
    explicit networkData(const std::size_t & array_size);

    /**
     * @brief Initialization with data from file.
     * @param networkDataFile The path of the file the training data is loaded from
     */
    explicit networkData(const std::string & network_data_file);
    /**
     * @brief The copy constructor.
     * @param cp A constant reference to another networkData object
     */
    networkData(const networkData & cp);

    /** @brief A standard destructor. */
    ~networkData() override;

    /**
     * @brief Copies the data of another networkData object.
     * @param cp A constant reference to the networkData to copy from
     * @return A reference to this object
     */
    networkData &operator=(const networkData & cp);

    /**
     * @brief Searches for compliance with expectations with respect to another object of the same type.
     * @param cp The other object to compare against
     * @param e The expectation for this object, e.g. equality
     * @param limit The limit for allowed deviations of floating point types
     */
    virtual void compare(
        const networkData & cp
        ,
        const Gem::Common::expectation & e
        ,
        const double & limit
    ) const;

    /**
     * @brief Saves the data of this struct to disc.
     * @param networkDataFile The path of the file the data is written to
     */
    void saveToDisk(const std::string & network_data_file) const;
    /**
     * @brief Loads training data from the disc.
     * @param networkDataFile The path of the file the data is read from
     */
    void loadFromDisk(const std::string & network_data_file);

    /**
     * @brief Adds a new training set to the collection. Requires the network architecture to be defined already.
     * @param tS The training set to store
     * @param pos The position at which the training set is stored
     */
    void addTrainingSet(std::shared_ptr<trainingSet> t_s, const std::size_t & pos);
    /**
     * @brief Retrieves the training set at a given position.
     * @param pos The position of the training set to retrieve
     * @return The training set at that position, or an empty optional if none is stored there
     */
    std::optional<std::shared_ptr<trainingSet>>

    getTrainingSet(const std::size_t & pos) const;

    /**
     * @brief Retrieves the number of input nodes of this network.
     * @return The number of input nodes (the first stored layer size)
     */
    std::size_t getNInputNodes() const;
    /**
     * @brief Retrieves the number of output nodes of this network.
     * @return The number of output nodes (the last stored layer size)
     */
    std::size_t getNOutputNodes() const;

    /**
     * @brief Saves this data set in ROOT format for visual inspection.
     * @param outputFile The path of the ROOT script to write
     * @param min The lower boundary of the plotted value range
     * @param max The upper boundary of the plotted value range
     */
    void toROOT(const std::string & output_file, const double & min, const double & max);

    /**
     * @brief Allows to check whether an initialization range has been set.
     * @return true if an initialization range has been set, false otherwise
     */
    bool initRangeSet() const;
    /**
     * @brief Allows to set the initialization range.
     * @param init_range The per-dimension (lower, upper) initialization ranges
     */
    void setInitRange(const std::vector<std::tuple<double, double>> &init_range);
    /**
     * @brief Allows to retrieve the initialization range.
     * @return The per-dimension (lower, upper) initialization ranges
     */
    std::vector<std::tuple<double, double>> getInitRange() const;

    /**
     * @brief Allows to retrieve a string that describes the network geometry.
     * @return A string describing the network geometry (layer sizes)
     */
    std::string getNetworkGeometryString() const;

    /**
     * @brief Creates a deep clone of this object.
     * @return A deep clone of this object, wrapped in a shared_ptr
     */
    std::shared_ptr<networkData> clone() const;

private:
    /***************************************************************************/
    /** @brief Default constructor, intentionally private */
    networkData();

    /**
     * @brief Deep-copies the training sets of another networkData object (empty slots stay empty).
     * @param cp The source object
     * @return The deep copy of cp's training-set slots
     */
    static std::vector<std::shared_ptr<trainingSet>> deepCopyOfTrainingSets(const networkData &cp);

    /***************************************************************************/
    /** @brief Holds the individual data items */
    std::vector<std::shared_ptr<trainingSet>> data_;

    /** @brief Holds the initialization range in each direction */
    std::vector<std::tuple<double, double>> init_range_;

    /** @brief Locks access to the clone function */
    mutable std::mutex m_; ///< Lock get/set operations
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/** @brief This enum is used to specify the type of training data that should be generated */
enum class trainingDataType : Gem::Common::ENUMBASETYPE {
    TDTNONE = 0,
    HYPERCUBE = 1,
    HYPERSPHERE = 2,
    AXISCENTRIC = 3,
    SINUS = 4
};
/** @brief Allows to specify whether we want to use a sigmoidal transfer function or a radial basis function */
enum class transferFunction : Gem::Common::ENUMBASETYPE {
    SIGMOID = 0,
    RBF = 1
};

/******************************************************************************/
// trainingDataType and transferFunction stream as their underlying numeric
// values through the shared machinery in GCommonEnums.hpp. The marker
// specializations must precede the first in-header streaming use, hence the
// namespace round-trip right here; the using-declarations re-export the
// operators so ADL finds them in this namespace.

} /* namespace Gem::Geneva::Individuals */
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::Individuals::trainingDataType> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::Individuals::transferFunction> = true;
} /* namespace Gem::Common */
namespace Gem::Geneva::Individuals {

using Gem::Common::operator<<;
using Gem::Common::operator>>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// A global singleton giving access to the training data.
// See also the definition of TFactory_GSingletonT<Gem::Geneva::Individuals::networkData>
using GDatStore = Gem::Common::GSingletonT<Gem::Geneva::Individuals::networkData>;
/**
 * @brief Accessor for the global training-data singleton.
 * @return A shared pointer to the global training-data singleton
 */
[[nodiscard]] inline std::shared_ptr<GDatStore::STYPE> nnTrainingDataStore() {
    return GDatStore::instance();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// A number of default settings for the factory
const std::string GNN_DEF_DATAFILE = "./Datasets/hyper_cube.dat";
constexpr double GNN_DEF_ADPROB = 0.05;
constexpr double GNN_DEF_ADAPTADPROB = 0.1;
constexpr double GNN_DEF_MINADPROB = 0.05;
constexpr double GNN_DEF_MAXADPROB = 1.;
constexpr double GNN_DEF_SIGMA = 0.1;
constexpr double GNN_DEF_SIGMASIGMA = 0.8;
constexpr double GNN_DEF_MINSIGMA = 0.01;
constexpr double GNN_DEF_MAXSIGMA = 0.2;
constexpr double GNN_DEF_MINVAR = -10.;
constexpr double GNN_DEF_MAXVAR = 10.;
constexpr transferFunction GNN_DEF_TRANSFER = transferFunction::SIGMOID;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The semantic architecture (DM §4) of a feed-forward neural network: a layout-agnostic decoder that
 * maps the individual's flat floating-point genome (the concatenated layer weight vectors) to the
 * network's per-layer weight offsets. It is the first real GGenomeArchitecture user. The weight layout
 * (matching the historical tree genome's GDoubleObjectCollection-per-layer ordering) is:
 *  - input layer (layer 0): 2 * nodes[0] weights (per input node: one multiplier + one bias);
 *  - layer L > 0: nodes[L] * (nodes[L-1] + 1) weights (per node: one weight per previous node + bias).
 * The architecture computes only the structure (offsets / sizes); the forward pass + transfer function
 * live in GNeuralNetworkIndividual::evaluate(), which reads the weights through this view.
 */
class GNeuralNetworkArchitecture : public gen::GGenomeArchitecture {
public:
    /**
     * @brief Initialization from the per-layer node counts (input ... output).
     * @param layer_sizes The number of nodes in each layer, from input layer to output layer
     */
    explicit GNeuralNetworkArchitecture(std::vector<std::size_t> layer_sizes)
      : layer_sizes_(std::move(layer_sizes)) {
        offsets_.resize(layer_sizes_.size());
        std::size_t off = 0;
        for(std::size_t l = 0; l < layer_sizes_.size(); ++l) {
            offsets_[l] = off;
            off += weightCount(l);
        }
        total_weights_ = off;
    }

    /**
     * @brief Emits a name for this architecture.
     * @return The name of this architecture class
     */
    [[nodiscard]] std::string name() const override { return "GNeuralNetworkArchitecture"; }
    /**
     * @brief The number of floating-point genome entries this architecture expects.
     * @return The total number of weights across all layers
     */
    [[nodiscard]] std::size_t expectedFPSize() const override { return total_weights_; }

    /**
     * @brief The number of layers (input + hidden + output).
     * @return The number of layers
     */
    [[nodiscard]] std::size_t nLayers() const { return layer_sizes_.size(); }
    /**
     * @brief The number of nodes in layer l.
     * @param l The layer index (0 = input layer)
     * @return The number of nodes in layer l
     */
    [[nodiscard]] std::size_t layerSize(std::size_t l) const { return layer_sizes_.at(l); }
    /**
     * @brief The offset of layer l's weights into the flat genome.
     * @param l The layer index (0 = input layer)
     * @return The offset of layer l's first weight within the flat weight genome
     */
    [[nodiscard]] std::size_t layerOffset(std::size_t l) const { return offsets_.at(l); }
    /**
     * @brief The number of weights owned by layer l.
     * @param l The layer index (0 = input layer)
     * @return The number of weights belonging to layer l
     */
    [[nodiscard]] std::size_t weightCount(std::size_t l) const {
        return (l == 0) ? (2 * layer_sizes_[0]) : (layer_sizes_[l] * (layer_sizes_[l - 1] + 1));
    }

private:
    std::vector<std::size_t> layer_sizes_; ///< nodes per layer
    std::vector<std::size_t> offsets_;     ///< per-layer weight offset into the flat genome
    std::size_t total_weights_ = 0;        ///< total number of weights
};


namespace detail {
/******************************************************************************/
/**
 * The shared driver of the four public dataset generators (hyper-cube, hyper-sphere,
 * axis-centric, sin): validates the architecture, builds the networkData skeleton, then
 * fills each training set through the shape-specific @p fillSet callback. Only the
 * per-set data generation (and any extra input-dimension requirement) differs between
 * the generators.
 *
 * @param architecture The desired architecture of the network
 * @param n_data_sets The number of training sets to create
 * @param where The generator name used in error messages (e.g. "createSinNetworkData")
 * @param required_input_nodes If non-zero, the exact number of input nodes required
 * @param fillSet Fills one training set (and may register an init range on the data object)
 * @param init_range The initialization range to register; empty if fillSet registers it itself
 * @return The created networkData object, wrapped in a shared_ptr
 */
inline std::shared_ptr<networkData> createNetworkData(
    const std::vector<std::size_t> &architecture,
    std::size_t n_data_sets,
    std::string_view where,
    std::size_t required_input_nodes,
    const std::function<void(Gem::Hap::GRandom &, networkData &, trainingSet &)> &fillSet,
    const std::vector<std::tuple<double, double>> &init_range
) {
    // Check the number of supplied layers
    if(architecture.size() < 2) { // We need at least an input- and an output-layer
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::" << where << "(): Error!" << '\n'
            << "Got invalid number of layers: " << architecture.size() << '\n'
        );
    }

    // Check that the output layer has exactly one node
    if(architecture.back() != 1) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::" << where << "(): Error!" << '\n'
            << "The output layer must have exactly one node for this training data." << '\n'
            << "Got " << architecture.back() << " instead." << '\n'
        );
    }

    // Some generators require a fixed input dimension (e.g. the sin data needs 2)
    if(required_input_nodes != 0 && architecture.front() != required_input_nodes) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNeuralNetworkIndividual::" << where << "(): Error!" << '\n'
            << "The input layer must have exactly " << required_input_nodes
            << " node(s) for this example." << '\n'
            << "Got " << architecture.front() << " instead." << '\n'
        );
    }

    // Create a local random number generator.
    Gem::Hap::GRandom gr_l;

    // Create the actual networkData object and attach the architecture,
    // checking the layer sizes on the way
    std::shared_ptr<networkData> n_d(new networkData(n_data_sets));
    std::size_t layer_counter = 0;
    for(auto layer_size : architecture) {
        if(layer_size == 0) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GNeuralNetworkIndividual::" << where << "(): Error!" << '\n'
                << "Layer " << layer_counter << "has invalid size " << layer_size << '\n'
            );
        }
        n_d->push_back(layer_size);
        ++layer_counter;
    }

    // Create the required data through the shape-specific fill.
    const std::size_t n_input_nodes = architecture.front();
    const std::size_t n_output_nodes = architecture.back();
    for(std::size_t dat_counter = 0; dat_counter < n_data_sets; dat_counter++) {
        std::shared_ptr<trainingSet> const t_s(new trainingSet(n_input_nodes, n_output_nodes));
        fillSet(gr_l, *n_d, *t_s);
        n_d->addTrainingSet(t_s, dat_counter);
    }

    // Make the initialization range known to nD_ (a generator that derives the range from
    // its drawn data -- the hyper-sphere -- registers it inside fillSet instead).
    if(not init_range.empty()) {
        n_d->setInitRange(init_range);
    }

    return n_d;
}

/******************************************************************************/
} // namespace detail

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * With this individual you can use Genevas optimization algorithms instead of the
 * standard back-propagation algorithm to train feed-forward neural networks.
 */
class GNeuralNetworkIndividual // NOLINT(cppcoreguidelines-special-member-functions)
  : public gen::GGenomeT<GNeuralNetworkIndividual> {
    /////////////////////////////////////////////////////////////////////////////

    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin reach
    // the private localMembers_() below (serialize/load_/compare_/clone_/name_ are all generated).
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief The single declaration of this class'es serialised local data members. n_d_ (the training
     *  data) is intentionally NOT listed: every constructor -- including the one Boost invokes before it
     *  deserialises -- sets it from the global nnTrainingDataStore() singleton, so it never needs to travel
     *  in the archive. (The former split save()/load() reassigned n_d_ in load() redundantly, which is why
     *  it needed to be asymmetric; a single generated serialize() suffices.) nn_arch_ is a transient cache
     *  rebuilt from n_d_ on demand. */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(Gem::Common::make_member("t_f_", self.t_f_));
    }

    /////////////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and compare token. */
    static constexpr std::string_view class_name = "GNeuralNetworkIndividual";
    /***************************************************************************/
    /** @brief The default constructor */
    GNeuralNetworkIndividual();
    /**
     * @brief A constructor which initializes the individual with a suitable set of network layers.
     * @param min The lower boundary of the weight initialization range
     * @param max The upper boundary of the weight initialization range
     * @param sigma The initial sigma (step width) of the weight Gauss adaptor
     * @param sigma_sigma The self-adaption strength of sigma
     * @param min_sigma The lower boundary for sigma
     * @param max_sigma The upper boundary for sigma
     * @param ad_prob The adaption probability
     * @param adapt_ad_prob The self-adaption strength of the adaption probability
     * @param min_ad_prob The lower boundary for the adaption probability
     * @param max_ad_prob The upper boundary for the adaption probability
     */
    GNeuralNetworkIndividual(
        [[maybe_unused]] const double & min,
        [[maybe_unused]] const double & max
        ,
        [[maybe_unused]] const double & sigma,
        [[maybe_unused]] const double & sigma_sigma
        ,
        [[maybe_unused]] const double & min_sigma,
        [[maybe_unused]] const double & max_sigma
        ,
        [[maybe_unused]] const double & ad_prob,
        [[maybe_unused]] const double & adapt_ad_prob
        ,
        [[maybe_unused]] const double & min_ad_prob,
        [[maybe_unused]] const double & max_ad_prob
    );
    /**
     * @brief A standard copy constructor.
     * @param cp A constant reference to another GNeuralNetworkIndividual object
     */
    GNeuralNetworkIndividual(const GNeuralNetworkIndividual &cp);

    /** @brief The standard destructor */
    ~GNeuralNetworkIndividual() override;

    /**
     * @brief Initialization according to user-specifications.
     * @param min The lower boundary of the weight initialization range
     * @param max The upper boundary of the weight initialization range
     * @param sigma The initial sigma (step width) of the weight Gauss adaptor
     * @param sigma_sigma The self-adaption strength of sigma
     * @param min_sigma The lower boundary for sigma
     * @param max_sigma The upper boundary for sigma
     * @param ad_prob The adaption probability
     * @param adapt_ad_prob The self-adaption strength of the adaption probability
     * @param min_ad_prob The lower boundary for the adaption probability
     * @param max_ad_prob The upper boundary for the adaption probability
     */
    void init(
        [[maybe_unused]] const double & min,
        [[maybe_unused]] const double & max
        ,
        [[maybe_unused]] const double & sigma,
        [[maybe_unused]] const double & sigma_sigma
        ,
        [[maybe_unused]] const double & min_sigma,
        [[maybe_unused]] const double & max_sigma
        ,
        [[maybe_unused]] const double & ad_prob,
        [[maybe_unused]] const double & adapt_ad_prob
        ,
        [[maybe_unused]] const double & min_ad_prob,
        [[maybe_unused]] const double & max_ad_prob
    );

    /**
     * @brief Sets the type of the transfer function.
     * @param t_f The transfer function (sigmoid or radial basis function) to use
     */
    void setTransferFunction(transferFunction t_f);
    /**
     * @brief Retrieves the type of the transfer function.
     * @return The transfer function currently in use
     */
    transferFunction getTransferFunction() const;

    /***************************************************************************/
    /**
	  * This static function can be called in main() in order to create a suitable set of training
	  * data for this class. It is added here as a means of testing the neural network individual.
	  * We use a simple hyper-cube, ranging from [-edgelength/2,edgelength/2[ in each dimension.
	  * Areas outside of the cube get an output value of 0.99, areas inside of the cube get an output
	  * value of 0.01. The training data is initialized in the range [-edgelength:edgelength[.
	  *
	  * @param architecture The desired architecture of the network
	  * @param n_data_sets The number of training sets to create
	  * @param edgelength The desired edge length of the cube
	  * @return A copy of the networkData struct that has been created, wrapped in a shared_ptr
	  */
    static std::shared_ptr<networkData> createHyperCubeNetworkData(
        const std::vector<std::size_t> &architecture,
        const std::size_t &n_data_sets,
        const double &edgelength
    ) {
        std::uniform_real_distribution<double> uniform_real_distribution;
        return detail::createNetworkData(
            architecture,
            n_data_sets,
            "createHyperCubeNetworkData",
            0,
            [&](Gem::Hap::GRandom &gr_l, networkData &, trainingSet &t_s) {
                bool outside = false;
                for(std::size_t i = 0; i < t_s.Input.size(); i++) {
                    double const one_dim_rnd = uniform_real_distribution(
                        gr_l,
                        std::uniform_real_distribution<double>::param_type(-edgelength, edgelength)
                    );

                    // Need to find at least one dimension outside of the perimeter
                    // in order to set the outside flag to true.
                    if(one_dim_rnd < -edgelength / 2. || one_dim_rnd > edgelength / 2.) {
                        outside = true;
                    }

                    t_s.Input[i] = one_dim_rnd;
                }

                t_s.Output[0] = outside ? 0.99 : 0.01;
            },
            {{-edgelength, edgelength}, {-edgelength, edgelength}} // x, y
        );
    }

    /***************************************************************************/
    /**
	  * This static function can be called in main() in order to create a suitable input file for
	  * this class. It is added here as a means of testing this neural network individual. We create
	  * a sphere of radius "radius". See http://en.wikipedia.org/wiki/Hypersphere for a description of
	  * the formulae used.  Areas outside of the sphere get an output value of 0.99, areas inside of the
	  * sphere get an output value of 0.01. The training data is initialized with a radius of 2*radius.
	  *
	  * @param architecture The desired architecture of the network
	  * @param n_data_sets The number of training sets to create
	  * @param radius The desired radius of the sphere
	  * @return A copy of the networkData struct that has been created, wrapped in a shared_ptr
	  */
    // NOLINTNEXTLINE(readability-function-size) -- one coherent numeric kernel: the hypersphere-to-Cartesian coordinate transform (Wikipedia's n-sphere formula), whose dimension-dispatch switch (1D/2D/general nD) is one mathematical derivation; splitting would scatter tightly coupled coordinate math
    static std::shared_ptr<networkData> createHyperSphereNetworkData(
        const std::vector<std::size_t> &architecture,
        const std::size_t &n_data_sets,
        const double &radius
    ) {
        std::uniform_real_distribution<double> uniform_real_distribution;
        return detail::createNetworkData(
            architecture,
            n_data_sets,
            "createHyperSphereNetworkData",
            0,
            [&](Gem::Hap::GRandom &gr_l, networkData &n_d, trainingSet &t_s) {
                const std::size_t n_dim = t_s.Input.size();

                // Declared at first assignment (the former leading `= 1.` initializer was a dead store).
                double const local_radius = uniform_real_distribution(
                    gr_l,
                    std::uniform_real_distribution<double>::param_type(0., 3 * radius)
                );
                t_s.Output[0] = (local_radius > radius) ? 0.99 : 0.01;

                //////////////////////////////////////////////////////////////////
                // Calculate random Cartesian coordinates for hyper sphere

                // Special cases
                switch(n_dim) {
                case 1:
                    t_s.Input[0] = local_radius;
                    break;

                case 2: {
                    double const phi = uniform_real_distribution(
                        gr_l,
                        std::uniform_real_distribution<double>::param_type(0., 2 * std::numbers::pi)
                    );
                    t_s.Input[0] = local_radius * sin(phi); // x
                    t_s.Input[1] = local_radius * cos(phi); // y

                    // Make the initialization range known to nD_ . We only do this for 2D-data
                    std::vector<std::tuple<double, double>> init_range;
                    init_range.emplace_back(-local_radius, local_radius); // x
                    init_range.emplace_back(-local_radius, local_radius); // y
                    n_d.setInitRange(init_range);
                } break;

                default: // dimensions 3 ... inf
                {
                    //////////////////////////////////////////////////////////////////
                    // Create the required random numbers in spherical coordinates.
                    // n_dim will be at least 3 here.
                    std::size_t const n_angles = n_dim - 1;
                    std::vector<double> angle_collection(n_angles);
                    for(std::size_t i = 0; i < (n_angles - 1); i++) { // Angles in range [0,Pi[
                        angle_collection[i] = uniform_real_distribution(
                            gr_l,
                            std::uniform_real_distribution<double>::param_type(0., std::numbers::pi)
                        );
                    }
                    angle_collection[n_angles - 1] = uniform_real_distribution(
                        gr_l,
                        std::uniform_real_distribution<double>::param_type(0., 2 * std::numbers::pi)
                    ); // Range of last angle is [0, 2.*Pi[

                    //////////////////////////////////////////////////////////////////
                    // Now we can fill the source-vector itself
                    std::vector<double> cart_coord(n_dim);

                    for(std::size_t i = 0; i < n_dim; i++) {
                        cart_coord[i] = local_radius; // They all have that
                    }

                    cart_coord[0] *= cos(angle_collection[0]); // x_1 / cart_coord[0]

                    for(std::size_t i = 1; i < n_dim - 1;
                        i++) { // x_2 ... x_(n-1) / cart_coord[1] .... cart_coord[n-2]
                        for(std::size_t j = 0; j < i; j++) {
                            cart_coord[i] *= sin(angle_collection[j]);
                        }
                        cart_coord[i] *= cos(angle_collection[i]);
                    }

                    for(std::size_t j = 0; j < n_angles; j++) { // x_n / cart_coord[n-1]
                        cart_coord[n_dim - 1] *= sin(angle_collection[j]);
                    }

                    // Transfer the results
                    for(std::size_t i = 0; i < n_dim; i++) {
                        t_s.Input[i] = cart_coord[i];
                    }

                } break;
                }
            },
            {} // the 2-D case registers its init range from the drawn radius above
        );
    }

    /***************************************************************************/
    /**
	  * Creates training data where one data set is evenly distributed in the range of [0,1.] in
	  * each dimension, the other centers along the different coordinate axes. It is added here as
	  * a means of testing this neural network individual. The even distribution gets an output
	  * value of 0.01, the "axis-centric" data distribution gets an output value of 0.99. Note that
	  * the creation of training data might take a long time for large dimensions values
	  *
	  * @param architecture The desired architecture of the network
	  * @param n_data_sets The number of training sets to create
	  * @return A copy of the networkData struct that has been created, wrapped in a shared_ptr
	  */
    static std::shared_ptr<networkData> createAxisCentricNetworkData(
        const std::vector<std::size_t> &architecture,
        const std::size_t &n_data_sets
    ) {
        std::uniform_real_distribution<double> uniform_real_distribution;
        std::size_t data_counter = 0;
        return detail::createNetworkData(
            architecture,
            n_data_sets,
            "createAxisCentricNetworkData",
            0,
            [&](Gem::Hap::GRandom &gr_l, networkData &, trainingSet &t_s) {
                const std::size_t n_dim = t_s.Input.size();

                // Create even distribution across all dimensions
                if(data_counter % 2 == 0) {
                    for(std::size_t dim_counter = 0; dim_counter < n_dim; dim_counter++) {
                        t_s.Input[dim_counter] = uniform_real_distribution(gr_l);
                    }
                    t_s.Output[0] = 0.01;
                }
                // Create entries in a half-cylindrical "cloud" around one axis. The density of
                // this cloud is decreasing with increasing distance from the axis.
                else {
                    // Create a test value
                    double probe_value = 0.;
                    for(std::size_t dim_counter = 0; dim_counter < n_dim; dim_counter++) {
                        probe_value += exp(-5. * uniform_real_distribution(gr_l));
                    }

                    double function_value = 0.;
                    std::vector<double> input_vector(n_dim);
                    do {
                        function_value = 0.;

                        // Create the input vector
                        for(std::size_t dim_counter = 0; dim_counter < n_dim; dim_counter++) {
                            input_vector[dim_counter] = uniform_real_distribution(gr_l);
                            function_value += exp(-5 * input_vector[dim_counter]);
                        }
                        function_value = pow(function_value, 4.);
                    }
                    while(function_value < probe_value);

                    for(std::size_t i = 0; i < n_dim; i++) {
                        t_s.Input[i] = input_vector[i];
                    }
                    t_s.Output[0] = 0.99;
                }

                ++data_counter;
            },
            {{0, 1}, {0, 1}} // x, y
        );
    }

    /***************************************************************************/
    /**
	  * Creates training data where one data set is evenly distributed above a sin(x) curve, the
	  * other evenly below it. This example only accepts two input nodes.
	  *
	  * @param architecture The desired architecture of the network
	  * @param n_data_sets The number of training sets to create
	  * @return A copy of the networkData struct that has been created, wrapped in a shared_ptr
	  */
    static std::shared_ptr<networkData> createSinNetworkData(
        const std::vector<std::size_t> &architecture,
        const std::size_t &n_data_sets
    ) {
        std::uniform_real_distribution<double> uniform_real_distribution;
        return detail::createNetworkData(
            architecture,
            n_data_sets,
            "createSinNetworkData",
            2, // this example only accepts two input nodes
            [&](Gem::Hap::GRandom &gr_l, networkData &, trainingSet &t_s) {
                // create the two test values
                t_s.Input[0] = uniform_real_distribution(
                    gr_l,
                    std::uniform_real_distribution<double>::param_type(-6., 6.)
                ); // x
                t_s.Input[1] = uniform_real_distribution(
                    gr_l,
                    std::uniform_real_distribution<double>::param_type(-6., 6.)
                ); // y

                // Check whether we are below or above the sin function and assign the output value accordingly
                t_s.Output[0] = (t_s.Input[1] > 4. * sin(t_s.Input[0])) ? 0.99 : 0.01;
            },
            {{-6, 6}, {-6, 6}} // x, y
        );
    }

    /***************************************************************************/
    /**
	  * Creates a data set of the desired type or throws, if that type is not available
	  *
	  * @param t The type of network data to be created
	  * @param output_file The name of the output training data file
	  * @param architecture_string The desired architecture of the network in std::string format
	  * @param n_data_sets The number of data sets to be produced
	  */
    static void createNetworkData(
        const Gem::Geneva::Individuals::trainingDataType &t,
        const std::string &output_file,
        const std::string &architecture_string,
        const std::size_t &n_data_sets
    ) {
        // Split the architecture_string as needed. I
        std::vector<std::size_t> const architecture =
            Gem::Common::splitStringT<std::size_t>(architecture_string, "-");
        std::shared_ptr<networkData> n_d_ptr;

        switch(t) {
        case Gem::Geneva::Individuals::trainingDataType::HYPERCUBE:
            n_d_ptr = GNeuralNetworkIndividual::createHyperCubeNetworkData(
                architecture,
                n_data_sets,
                0.5 // edge-length
            );

            // Emit a visualization file, suitable for viewing with ROOT (see http://root.cern.ch)
            n_d_ptr->toROOT(output_file + ".C", -0.5, 0.5);

            break;

        case Gem::Geneva::Individuals::trainingDataType::HYPERSPHERE:
            n_d_ptr = GNeuralNetworkIndividual::createHyperSphereNetworkData(
                architecture,
                n_data_sets,
                0.5 // radius
            );

            // Emit a visualization file, suitable for viewing with ROOT (see http://root.cern.ch)
            n_d_ptr->toROOT(output_file + ".C", -1., 1.);

            break;

        case Gem::Geneva::Individuals::trainingDataType::AXISCENTRIC:
            n_d_ptr =
                GNeuralNetworkIndividual::createAxisCentricNetworkData(architecture, n_data_sets);

            // Emit a visualization file, suitable for viewing with ROOT (see http://root.cern.ch)
            n_d_ptr->toROOT(output_file + ".C", 0., 1.);

            break;

        case Gem::Geneva::Individuals::trainingDataType::SINUS:
            n_d_ptr = GNeuralNetworkIndividual::createSinNetworkData(architecture, n_data_sets);

            // Emit a visualization file, suitable for viewing with ROOT (see http://root.cern.ch)
            n_d_ptr->toROOT(output_file + ".C", -6., 6.);

            break;

        default: {                    // Error
            std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
            error << "In createDataset(): Error!" << '\n'
                  << "Received invalid data type " << t << '\n';
            throw(geneva_exception(error.str()));
        } // no break: the default case always throws (the trailing break was unreachable)
        }

        // Write distribution to file
        n_d_ptr->saveToDisk(output_file);
    }

    /***************************************************************************/
    /**
     * @brief Creates a program used for the visualization of optimization results.
     * @param vis_file The path of the visualization file to write
     */
    void writeVisualizationFile(const std::string &vis_file);
    /**
     * @brief Creates a C++ output file for the trained network.
     * @param header_file The path of the C++ header file to write
     */
    void writeTrainedNetwork(const std::string &header_file);

    /***************************************************************************/
    /**
     * The configuration read from the config file by GIndividualFactory<GNeuralNetworkIndividual>.
     * The genome geometry itself comes from the global training-data store (see buildGenome); these are
     * the per-weight Gauss-adaptor settings plus the parameter init range and transfer function.
     */
    struct Config {
        double ad_prob = GNN_DEF_ADPROB;
        double adapt_ad_prob = GNN_DEF_ADAPTADPROB;
        double min_ad_prob = GNN_DEF_MINADPROB;
        double max_ad_prob = GNN_DEF_MAXADPROB;
        double sigma = GNN_DEF_SIGMA;
        double sigma_sigma = GNN_DEF_SIGMASIGMA;
        double min_sigma = GNN_DEF_MINSIGMA;
        double max_sigma = GNN_DEF_MAXSIGMA;
        double min_var = GNN_DEF_MINVAR;
        double max_var = GNN_DEF_MAXVAR;
        transferFunction t_f = GNN_DEF_TRANSFER;
    };

    /**
     * @brief Registers the config-file options, binding them to the passed Config.
     * @param gpb The GParserBuilder object to which the configurable values are added
     * @param c The Config instance whose members are bound to the parser (written on parse)
     */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c);
    /**
     * @brief Builds the flat weight genome (structure only) for the geometry in the global data store.
     * @param c The configuration providing the weight init range
     * @return The structure-only weight genome data
     */
    static gen::GenomeData buildGenome(const Config &c);
    /**
     * @brief The OA-owned Gauss adaption config: every weight group gets the configured Gauss adaptor.
     * @param sample A sample flat genome whose group structure the config mirrors
     * @param c The configuration providing the Gauss adaptor settings
     * @return A shared pointer to the populated adaption config
     */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const gen::GGenome &sample, const Config &c);
    /**
     * @brief Per-object post-config hook: applies the (non-genome) transfer function to a produced individual.
     * @param ind The individual to configure (modified in place)
     * @param c The configuration providing the transfer function to apply
     */
    static void applyConfig(GNeuralNetworkIndividual &ind, const Config &c);

protected:
    /***************************************************************************/
    /**
     * @brief The evaluation hook: the training error of the network encoded by this individual's weights.
     * @return The training error as a one-element vector (single criterion)
     */
    std::vector<double> evaluate() final;

public:
    /**
     * @brief Builds the (shared, immutable) semantic architecture for a given network geometry.
     * @param n_d The network data describing the network geometry (layer sizes)
     * @return A shared pointer to the immutable architecture for that geometry
     */
    static std::shared_ptr<const GNeuralNetworkArchitecture>
    makeArchitecture(const networkData &n_d);

private:
    /***************************************************************************/
    /**
     * @brief The transfer function.
     * @param value The pre-activation input value
     * @return The transfer function (sigmoid or RBF) applied to the input value
     */
    double transfer(const double &value) const;

    /**
     * @brief The semantic architecture (lazily built from n_d_; not serialised -- recoverable).
     * @return A reference to the cached semantic architecture
     */
    const GNeuralNetworkArchitecture &architecture() const;

    /***************************************************************************/
    // Local variables
    transferFunction t_f_;             ///< The transfer function to be used for the training
    std::shared_ptr<networkData> n_d_; ///< Holds the training data
    mutable std::shared_ptr<const GNeuralNetworkArchitecture>
        nn_arch_; ///< Cached semantic architecture (transient; rebuilt from n_d_ on demand)
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A factory for GNeuralNetworkIndividual objects: an alias for the generic, config-driven
 * GIndividualFactory, for which GNeuralNetworkIndividual supplies the static describeConfig /
 * buildGenome / buildAdaptionConfig / applyConfig hooks. Call sites use ctor(path),
 * get_as<>(), getAdaptionConfig() and registerContentCreator().
 */
using GNeuralNetworkIndividualFactory =
    Gem::Geneva::Genome::GIndividualFactory<GNeuralNetworkIndividual>;

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

namespace Gem::Common {

// A global store for network configuration data
using GNNOptStore = GSingletonT<GGlobalOptionsT<std::string>>;
/**
 * @brief Accessor for the global neural-network options singleton.
 * @return A shared pointer to the global neural-network options singleton
 */
[[nodiscard]] inline std::shared_ptr<GNNOptStore::STYPE> neuralNetworkOptions() {
    return GNNOptStore::instance();
}

/**
 * @brief A factory function for networkData objects, used by GSingletonT.
 * @return A default-initialized networkData object, wrapped in a shared_ptr
 */
template <>
std::shared_ptr<Gem::Geneva::Individuals::networkData> TFactory_GSingletonT();

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// For testing purposes

#ifdef GEM_TESTING

/**
 * @brief Factory specialization that creates GNeuralNetworkIndividual objects for the unit tests.
 *
 * As Gem::Geneva::Individuals::GNeuralNetworkIndividual has a private default constructor, we
 * provide a specialization of the factory function that constructs it via the parameterized
 * constructor using the default settings.
 *
 * @return A newly created GNeuralNetworkIndividual, wrapped in a shared_ptr
 */
template <>
inline std::shared_ptr<Gem::Geneva::Individuals::GNeuralNetworkIndividual>
TFactory_GUnitTests<Gem::Geneva::Individuals::GNeuralNetworkIndividual>() {
    return std::make_shared<Gem::Geneva::Individuals::GNeuralNetworkIndividual>(
        Gem::Geneva::Individuals::GNN_DEF_MINVAR,
        Gem::Geneva::Individuals::GNN_DEF_MAXVAR,
        Gem::Geneva::Individuals::GNN_DEF_SIGMA,
        Gem::Geneva::Individuals::GNN_DEF_SIGMASIGMA,
        Gem::Geneva::Individuals::GNN_DEF_MINSIGMA,
        Gem::Geneva::Individuals::GNN_DEF_MAXSIGMA,
        Gem::Geneva::Individuals::GNN_DEF_ADPROB,
        Gem::Geneva::Individuals::GNN_DEF_ADAPTADPROB,
        Gem::Geneva::Individuals::GNN_DEF_MINADPROB,
        Gem::Geneva::Individuals::GNN_DEF_MAXADPROB
    );
}

#endif /* GEM_TESTING */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/******************************************************************************/
