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

// Standard headers
#include <cstdint>
#include <string>

// Geneva headers
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief How a networked consumer decides that an outstanding work item has "timed out".
 *
 * The treatment is the user's answer to a genuinely workload-dependent question: WHEN is a work item that
 * has not come back assumed lost? Its cost is asymmetric -- under a need-all submission policy
 * (GSubmissionPolicy::full_success_or_fatal, e.g. gradient descent) a false-positive timeout triggers a
 * full re-evaluation, while under a tolerant policy a missing item is cheaply cloned. And because a single
 * Geneva evaluation may run anywhere from microseconds to days -- a duration not known a priori -- no fixed
 * ABSOLUTE timeout is a sensible general default. The default is therefore scale-free (@c adaptive).
 *
 * The set is deliberately open (an enum class, not the plain-C manifest ABI), so further treatments (e.g. a
 * percentile of the observed return-time distribution) can be added over time.
 */
enum class timeoutTreatment {
    adaptive = 0,         ///< scale-free: give-up window / reclaim lease are multiples of the running mean
                          ///< return time (clamped), self-calibrating to the true evaluation scale
    fixed = 1,            ///< user-set fixed give-up window / reclaim lease (no adaptation); opt-in, for a
                          ///< user who genuinely knows their evaluation's time bound
    wait_indefinitely = 2 ///< never declare an item MISSING on time: wait for every item (for reliable
                          ///< clusters + need-all policies, so a full re-evaluation is never paid on a
                          ///< false positive). Liveness-driven reclaim (websocket disconnect) still applies.
};

/******************************************************************************/
/**
 * @brief Maps a config string to a timeoutTreatment; an unrecognised value warns and falls back to adaptive.
 * @param s The treatment name ("adaptive" | "fixed" | "wait_indefinitely")
 * @return The matching treatment, or timeoutTreatment::adaptive if @p s is unrecognised
 */
inline timeoutTreatment timeoutTreatmentFromString(const std::string &s) {
    if(s == "adaptive") { return timeoutTreatment::adaptive; }
    if(s == "fixed") { return timeoutTreatment::fixed; }
    if(s == "wait_indefinitely") { return timeoutTreatment::wait_indefinitely; }
    glogger << "In Gem::Courtier::timeoutTreatmentFromString(): Warning!" << '\n'
            << "Unrecognised timeout treatment \"" << s << "\"; falling back to \"adaptive\"." << '\n'
            << "Known treatments: adaptive, fixed, wait_indefinitely." << '\n'
            << GWARNING;
    return timeoutTreatment::adaptive;
}

/**
 * @brief The config-string name of a timeoutTreatment.
 * @param t The treatment to name
 * @return Its config string ("adaptive" | "fixed" | "wait_indefinitely")
 */
inline std::string to_string(timeoutTreatment t) {
    switch(t) {
        case timeoutTreatment::fixed: return "fixed";
        case timeoutTreatment::wait_indefinitely: return "wait_indefinitely";
        case timeoutTreatment::adaptive:
        default: return "adaptive";
    }
}

/******************************************************************************/
/**
 * @brief The networked consumers' timeout / death-detection configuration, read from a Geneva-style JSON
 * config file (GParserBuilder), created with scale-free defaults if absent.
 *
 * This is the SERVER-side transport concern of "when is an unreturned item assumed lost" -- distinct from
 * (and composed with) the algorithm-driven GSubmissionPolicy, which decides what to DO about a missing item
 * (fatal vs clone). It is applied to every networked consumer (ASIO / websocket / MPI, all derived from
 * GNetworkedConsumerT) via GNetworkedConsumerT::applyTimeoutConfig(). Durations are whole milliseconds.
 *
 * Defaults reproduce GNetworkedConsumerT's built-in adaptive behaviour exactly, so an absent config file
 * changes nothing; a user edits the self-created file to select a treatment or tune it.
 */
struct GNetworkedTimeoutConfig {
    //--- the treatment selector (see timeoutTreatment) ---
    std::string treatment = "adaptive"; ///< "adaptive" | "fixed" | "wait_indefinitely"

    //--- adaptive-treatment knobs (multiples of / clamps on the running mean return time) ---
    double ema_alpha = 0.25;    ///< EMA weight for a new return-time sample
    double lease_factor = 4.0;  ///< reclaim lease = lease_factor * mean return time (clamped)
    double stall_factor = 8.0;  ///< give-up window = stall_factor * mean return time (clamped)
    std::int64_t lease_bootstrap_ms = 10'000; ///< reclaim lease before the first return is observed
    std::int64_t min_lease_ms = 200;          ///< lower clamp on the adaptive reclaim lease
    std::int64_t max_lease_ms = 300'000;      ///< upper clamp on the adaptive reclaim lease
    std::int64_t min_stall_ms = 2'000;        ///< lower clamp on the adaptive give-up window
    std::int64_t max_stall_ms = 600'000;      ///< upper clamp on the adaptive give-up window
    std::int64_t sweep_tick_ms = 50;          ///< dispatch_ re-evaluation poll interval

    //--- fixed-treatment knobs (used only when treatment == "fixed") ---
    std::int64_t fixed_stall_window_ms = 300'000; ///< give-up window when treatment == fixed
    std::int64_t fixed_lease_ms = 300'000;        ///< reclaim lease when treatment == fixed

    //--- connection-reclaim (ASIO only): the per-EXCHANGE session deadline that closes a client that
    //    connects but never completes its request. NOT the evaluation timeout (the ASIO client does discrete
    //    fetch/return exchanges and does not hold the connection while it computes). 0 disables it. ---
    std::int64_t session_timeout_ms = 300'000; ///< [asio] per-exchange session deadline (0 == disabled)

    /**
     * @brief Reads the configuration from @p configFile, writing scale-free defaults if it is absent.
     * @param configFile Path to the Geneva-style JSON config file (created with defaults if missing)
     */
    void load(const std::string &configFile) {
        Gem::Common::GParserBuilder gpb;
        gpb.registerFileParameter<std::string>(
            "treatment", treatment, treatment, Gem::Common::VAR_IS_ESSENTIAL,
            "Timeout treatment: adaptive (scale-free, the default) | fixed | wait_indefinitely");
        gpb.registerFileParameter<double>(
            "ema_alpha", ema_alpha, ema_alpha, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] EMA weight for a new return-time sample");
        gpb.registerFileParameter<double>(
            "lease_factor", lease_factor, lease_factor, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] reclaim lease = lease_factor * running mean return time (clamped)");
        gpb.registerFileParameter<double>(
            "stall_factor", stall_factor, stall_factor, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] give-up window = stall_factor * running mean return time (clamped)");
        gpb.registerFileParameter<std::int64_t>(
            "lease_bootstrap_ms", lease_bootstrap_ms, lease_bootstrap_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] reclaim lease (ms) used before the first return is observed");
        gpb.registerFileParameter<std::int64_t>(
            "min_lease_ms", min_lease_ms, min_lease_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] lower clamp (ms) on the reclaim lease");
        gpb.registerFileParameter<std::int64_t>(
            "max_lease_ms", max_lease_ms, max_lease_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] upper clamp (ms) on the reclaim lease");
        gpb.registerFileParameter<std::int64_t>(
            "min_stall_ms", min_stall_ms, min_stall_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] lower clamp (ms) on the give-up window");
        gpb.registerFileParameter<std::int64_t>(
            "max_stall_ms", max_stall_ms, max_stall_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[adaptive] upper clamp (ms) on the give-up window");
        gpb.registerFileParameter<std::int64_t>(
            "sweep_tick_ms", sweep_tick_ms, sweep_tick_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "dispatch_ re-evaluation poll interval (ms)");
        gpb.registerFileParameter<std::int64_t>(
            "fixed_stall_window_ms", fixed_stall_window_ms, fixed_stall_window_ms,
            Gem::Common::VAR_IS_ESSENTIAL, "[fixed] give-up window (ms) when treatment == fixed");
        gpb.registerFileParameter<std::int64_t>(
            "fixed_lease_ms", fixed_lease_ms, fixed_lease_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[fixed] reclaim lease (ms) when treatment == fixed");
        gpb.registerFileParameter<std::int64_t>(
            "session_timeout_ms", session_timeout_ms, session_timeout_ms, Gem::Common::VAR_IS_ESSENTIAL,
            "[asio] per-exchange connection deadline (ms); 0 disables it. NOT the evaluation timeout.");
        gpb.parseConfigFile(configFile);
    }

    /** @brief @return The treatment as its enum value (adaptive on an unrecognised string). */
    [[nodiscard]] timeoutTreatment treatmentEnum() const { return timeoutTreatmentFromString(treatment); }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
