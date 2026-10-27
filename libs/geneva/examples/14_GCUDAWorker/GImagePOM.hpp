/**
 * @file GImagePOM.hpp
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

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Geneva headers go here
#include "common/GArchiveNamed.hpp"
#include "common/GSelfTestable.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "weft/GArchivePolymorphic.hpp"

// Example-local headers
#include "GImageHelperFunctions.hpp"
#include "GImageScalar.hpp"
#include "GMonaLisaProblem.hpp"

namespace Gem::Geneva {
/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A small off-thread task sink for image output. The candidate rasterise (renderToRGB) and the PNG
 * encode/write are CPU-bound and, in example 14, cost ~40% of an iteration -- yet they only need a
 * by-value copy of the best genome, so they need not block the optimization loop. submit() launches
 * each task on a background thread (std::async) and keeps the loop running; a small in-flight cap
 * applies back-pressure if writing ever falls behind, and waitAll()/the destructor drain outstanding
 * writes so nothing is lost at shutdown. The host has spare cores here (the GPU path leaves it ~60%
 * idle). The mechanism is task-generic and could be lifted into a reusable sink for other
 * file-writing pluggable monitors.
 *
 * @par Invariant 2, recorded exception
 * This is a mutex plus a vector of futures, i.e. a small bounded work queue, and Invariant 2
 * asks for such a primitive to come from common/concurrency. It deliberately does not: the
 * back-pressure this needs -- block on the OLDEST outstanding task once a cap is reached -- is
 * not what GThreadPool offers, and extending the shared facility for a single example would be
 * out of proportion. Revisit if a second caller ever wants the same shape; the note is here so
 * the exception is visible rather than assumed.
 */
class AsyncImageWriter {
public:
    AsyncImageWriter() = default;
    AsyncImageWriter(const AsyncImageWriter &) = delete;
    AsyncImageWriter &operator=(const AsyncImageWriter &) = delete;
    ~AsyncImageWriter() {
        waitAll();
    }

    /** @brief Launches @p task on a background thread; reaps finished ones and caps the in-flight set. */
    template <typename Fn>
    void submit(Fn &&task) {
        std::lock_guard<std::mutex> lock(mutex_);
        reapDone();
        if(pending_.size() >= MAX_IN_FLIGHT) {
            // Back-pressure: writing has fallen behind the optimizer -- wait for the oldest to finish.
            pending_.front().wait();
            pending_.erase(pending_.begin());
        }
        pending_.push_back(std::async(std::launch::async, std::forward<Fn>(task)));
    }

    /** @brief Blocks until every outstanding write has completed (called at finalization). */
    void waitAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto &f : pending_) {
            if(f.valid()) {
                f.wait();
            }
        }
        pending_.clear();
    }

private:
    void reapDone() {
        std::erase_if(pending_, [](const std::future<void> &f) {
            return f.valid() &&
                   f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
    }

    std::mutex mutex_;
    std::vector<std::future<void>> pending_;
    static constexpr std::size_t MAX_IN_FLIGHT = 4;
};

/******************************************************************************/
/**
 * A pluggable optimization monitor that saves the iteration's best candidate image to disk. The best
 * candidate's flat genome is rasterised with the shared CPU renderer (the same alpha-blend math the
 * GPU kernel uses, see GMonaLisaProblem.hpp) and written as a PNG -- so the evolving superimposition
 * of triangles can be watched as it converges towards the target. No GPU read-back is needed: the
 * picture is reconstructed purely from the candidate's parameters.
 *
 * It reads the best candidate through the flat-genome base (Genome::GGenome): only streamline() and
 * raw_fitness() are needed, so the monitor is problem-type-agnostic and can be registered by a launcher
 * that loads the concrete image individual from a runtime module (it never names GImageIndividual).
 */
class GImagePOM final
  : public oa::GBasePluggableOM
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////

    /**
     * @brief Single declaration of this class'es local data members
     * @return A tuple of named member references driving serialize(), load_() and compare_()
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("resultImageDirectory_", self.resultImageDirectory_),
            Gem::Common::make_member("emitBestOnly_", self.emitBestOnly_)
        );
    }

    friend struct Gem::Weft::access;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        // The member list is derived from the single localMembers() declaration
        // so serialize()/load_()/compare_() stay in sync (no silently-dropped member).
        Gem::Common::archive_named_base<oa::GBasePluggableOM>(ar, "GBasePluggableOM", *this);
        Gem::Common::serialize_members(ar, this->localMembers_());
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /**
     * The standard constructor.
     *
     * @param resultDirectory The directory to which result images should be written
     * @param emitBestOnly Whether only images for improved iterations should be emitted
     */
    GImagePOM(const std::string &resultDirectory, bool emitBestOnly)
      : resultImageDirectory_(GImagePOM::trailingSlash(resultDirectory))
      , emitBestOnly_(emitBestOnly) {
        /* nothing */
    }

    /***************************************************************************/
    /** @brief The copy constructor. Copies the configuration but NOT the async writer: the
     *  in-flight output queue is per-instance runtime state, so a clone starts with its own
     *  (lazily created) writer rather than sharing the original's queue. */
    GImagePOM(const GImagePOM &cp)
      : oa::GBasePluggableOM(cp)
      , resultImageDirectory_(cp.resultImageDirectory_)
      , emitBestOnly_(cp.emitBestOnly_) {
        /* writer_ intentionally left null */
    }

    /***************************************************************************/
    /** @brief The destructor */
    ~GImagePOM() override = default;

    /***************************************************************************/
    /** @brief Allows to specify whether only images for improved iterations should be emitted */
    [[maybe_unused]] void setEmitBestOnly(const bool &emitBestOnly) {
        emitBestOnly_ = emitBestOnly;
    }

    /***************************************************************************/
    /** @brief Allows to check whether only images for improved iterations should be emitted */
    [[maybe_unused]] [[nodiscard]] bool getEmitBestOnly() const {
        return emitBestOnly_;
    }

protected:
    /************************************************************************/
    /** @brief Loads the data of another object */
    void load_(const oa::GBasePluggableOM *cp) override {
        // Check that we are dealing with a GImagePOM reference independent of this object
        const GImagePOM *p_load = Gem::Common::g_convert_and_compare(cp, this);

        // Load the parent classes' data ...
        oa::GBasePluggableOM::load_(cp);

        // ... and then our local data
        Gem::Common::g_load_members(this->localMembers_(), p_load->localMembers_());
    }

    /** @brief Allow access to this classes compare_ function */
    friend void Gem::Common::compare_base_t<GImagePOM>(
        GImagePOM const &,
        GImagePOM const &,
        Gem::Common::GToken &
    );

    /** @brief Searches for compliance with expectations with respect to another object of the same type */
    void compare_(
        const oa::GBasePluggableOM &cp,
        const Gem::Common::expectation &e,
        const double &limit
    ) const final {
        using namespace Gem::Common;

        // Check that we are dealing with a GImagePOM reference independent of this object
        const GImagePOM *p_load = Gem::Common::g_convert_and_compare(cp, this);

        GToken token("GImagePOM", e);

        // Compare our parent data ...
        Gem::Common::compare_base_t<oa::GBasePluggableOM>(*this, *p_load, token);

        // ... and then our local data
        g_compare_members(this->localMembers_(), p_load->localMembers_(), token);

        // React on deviations from the expectation
        token.evaluate();
    }

    /***************************************************************************/
    /** @brief Applies modifications to this object. This is needed for testing purposes */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING
        bool result = false;
        if(oa::GBasePluggableOM::modify_GUnitTests_()) {
            result = true;
        }
        return result;
#else /* GEM_TESTING */
        condnotset("GImagePOM::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

private:
    /***************************************************************************/
    /** @brief Emits a name for this class / object */
    [[nodiscard]] std::string name_() const override {
        return {"GImagePOM"};
    }

    /************************************************************************/
    /** @brief Creates a deep clone of this object */
    [[nodiscard]] oa::GBasePluggableOM *clone_() const override {
        return new GImagePOM(*this);
    }

    /***************************************************************************/
    /** @brief The default constructor. Intentionally private; only needed for (de-)serialization. */
    GImagePOM() = default;

    /***************************************************************************/
    /**
     * Emits information at the various stages of the information cycle (initialization, during each
     * iteration, and during finalization).
     */
    void informationFunction_(infoMode im, oa::GOptimizationAlgorithmBase const *const goa) override {
        switch(im) {
        case Gem::Geneva::infoMode::INFOINIT: {
            // Make sure the target directory for result images exists.
            const std::filesystem::path dir(resultImageDirectory_);
            if(not std::filesystem::exists(dir)) {
                if(not std::filesystem::create_directories(dir)) {
                    throw geneva_exception(
                        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                        << "GImagePOM: could not create directory " << resultImageDirectory_ << '\n'
                    );
                }
            }
            else if(not std::filesystem::is_directory(dir)) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "GImagePOM: " << resultImageDirectory_ << " is not a directory" << '\n'
                );
            }
        } break;

        case Gem::Geneva::infoMode::INFOPROCESSING: {
            // Emit an image only for improved iterations, unless every iteration was requested.
            if(emitBestOnly_ && not goa->progress()) {
                break;
            }

            auto best_ptr =
                goa->Interface::GOptimizerIT<oa::GOptimizationAlgorithmBase>::getBestIterationIndividual<Genome::GGenome>();

            // Snapshot everything the output needs into by-value data (cheap), so the actual work --
            // the CPU rasterise (renderToRGB) and the PNG encode/write -- can run off the optimization
            // thread. The genome scalar type is selected at compile time (gimage_fp_t), so streamline
            // into a matching buffer -- streamline<float> on a double genome collects nothing.
            std::vector<gimage_fp_t> parVec;
            best_ptr->streamline(parVec);
            const MonaLisa::Target &tgt = MonaLisa::target();
            const int width = tgt.width;
            const int height = tgt.height;
            const std::uint32_t iteration =
                goa->Interface::GOptimizerIT<oa::GOptimizationAlgorithmBase>::getIteration();
            const double fitness = best_ptr->raw_fitness(0);
            const std::string resultFileName = resultImageDirectory_ + std::to_string(iteration) +
                                               "_" + std::to_string(fitness) + "_bestIndividual.png";

            if(not writer_) {
                writer_ = std::make_unique<AsyncImageWriter>();
            }
            // Hand the rasterise + PNG write to a background thread; the optimizer continues at once.
            // The task is self-contained: parVec is moved in, the rest are copied by value, and it
            // writes to a unique per-iteration filename, so concurrent writes never collide.
            writer_->submit(
                [parVec = std::move(parVec), width, height, resultFileName]() {
                    std::vector<unsigned char> rgb;
                    MonaLisa::renderToRGB(parVec.data(), static_cast<int>(parVec.size()), width,
                                          height, rgb);
                    Gem::Common::writeRGBtoPNG(resultFileName, rgb, width, height);
                }
            );
        } break;

        case Gem::Geneva::infoMode::INFOEND: {
            // Make sure every queued image has been written before the run returns.
            if(writer_) {
                writer_->waitAll();
            }
        } break;
        };
    }

    /***************************************************************************/
    /** @brief Adds a slash to the end of the path if necessary */
    static std::string trailingSlash(const std::string &path) {
        if(path.empty() || path[path.size() - 1] != '/') {
            return path + '/';
        }
        return path;
    }

    /***************************************************************************/
    // Class data

    std::string resultImageDirectory_ = "./results/"; ///< The target directory for results
    bool emitBestOnly_{true}; ///< Whether images are written only for improved iterations

    /// Off-thread writer for the rasterise + PNG output. Transient runtime state: it is created
    /// lazily on first use and deliberately excluded from serialize/load_/compare_ (a std::future
    /// is neither copyable nor comparable, and there is nothing meaningful to persist). Sole
    /// ownership: the copy constructor above deliberately does not carry the writer over, so no
    /// second owner of one writer ever exists.
    std::unique_ptr<AsyncImageWriter> writer_; // NOLINT(misc-non-private-member-variables-in-classes)
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
} /* namespace Gem::Geneva */
