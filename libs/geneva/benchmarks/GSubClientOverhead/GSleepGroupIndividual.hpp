/**
 * @file GSleepGroupIndividual.hpp
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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Geneva header files go here
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "geneva/genome/GGenomeT.hpp"

namespace Gem::Geneva::SubClientOverhead {

/******************************************************************************/
/**
 * The individual of GSubClientOverhead. Its evaluation of a nominal duration sleeps, as
 * GDelayIndividual's does, and its fitness is the sum of the squares of its parameter values.
 * evaluate() does it all in one process: the serial reference and a client without sub-clients.
 * A client with sub-clients evaluates it in its group of processes instead, with the item
 * processor of its lead (GSubClientOverhead.cpp): every process of the group gets a message with
 * an equal part of the time (or, if the evaluation is not split, the whole duration) and its share
 * of the parameter values, sleeps, and answers with the sum of the squares of its share; the
 * fitness is the sum of the answers. The individual itself knows nothing of MPI.
 */
class GSleepGroupIndividual // NOLINT(cppcoreguidelines-special-member-functions)
  : public gen::GGenomeT<GSleepGroupIndividual> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member("evaluation_time_", self.evaluation_time_),
            Gem::Common::make_member("split_", self.split_)
        );
    }

public:
    /** @brief The class name, consumed by the generated name_() and compare token */
    static constexpr std::string_view class_name = "GSleepGroupIndividual";

    /** @brief What the configuration file of the benchmark holds; the keys are those of
     *  GDelayIndividual's file, so that the scripts set both alike */
    struct Config {
        std::size_t n_variables = 100;                           ///< Ballast double parameters
        std::string delays = "(0,10), (0,100), (1,0)";           ///< The (s,ms) evaluation times
        std::string result_file = "fullResults.C";               ///< The plot script
        std::string short_result_file = "shortDelayResults.txt"; ///< The table
        std::uint32_t n_measurements = 3;                        ///< Measurements per time
        std::uint32_t inter_measurement_delay = 1;               ///< Seconds between two times
        bool split_evaluation = true; ///< The group shares the time (or each sleeps all of it)
        /// The shortest pause between two looks [us]; courtier's default for the waits of MPI
        std::uint32_t poll_pause_min =
            static_cast<std::uint32_t>(Gem::Courtier::GMPIPEERPOLLPAUSEMIN.count());
        /// The longest pause between two looks [us]; courtier's default for the waits of MPI
        std::uint32_t poll_pause_max =
            static_cast<std::uint32_t>(Gem::Courtier::GMPIPEERPOLLPAUSEMAX.count());
    };

    /** @brief The default constructor (for the archive) */
    GSleepGroupIndividual() = default;
    /** @brief The copy constructor */
    GSleepGroupIndividual(const GSleepGroupIndividual &) = default;
    /** @brief The destructor */
    ~GSleepGroupIndividual() override = default;

    /** @brief Reads the configuration file, writing it with defaults if it is not there */
    static Config readConfig(const std::filesystem::path &);
    /** @brief The evaluation times of a configuration */
    static std::vector<std::chrono::duration<double>> evaluationTimes(const Config &);
    /** @brief An individual of the given evaluation time */
    static std::shared_ptr<GSleepGroupIndividual>
    create(const Config &, const std::chrono::duration<double> &);

    /** @brief The nominal duration of an evaluation [s] */
    [[nodiscard]] double evaluationTime() const noexcept;
    /** @brief Whether the processes of a group share the evaluation time */
    [[nodiscard]] bool split() const noexcept;

protected:
    /** @brief Sleeps the evaluation time and computes the fitness, in this process alone */
    std::vector<double> evaluate() final;

private:
    double evaluation_time_ = 0.; ///< The nominal duration of an evaluation [s]
    bool split_ = true;           ///< Whether the group shares it
};

} /* namespace Gem::Geneva::SubClientOverhead */
