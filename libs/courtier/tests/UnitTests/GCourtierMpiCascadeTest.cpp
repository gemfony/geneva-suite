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

/*
 * The test program of the MPI cascade (courtier/mpi/GMPICascade.hpp). It runs under mpirun with
 * real MPI, one scenario per run (--scenario=<name>), and links courtier's MPI library and MPI only:
 * nothing of Geneva. Every rank prints "OK: <scenario>: rank <r> ..." or "FAIL: <scenario>: rank
 * <r>: ..." lines and ends with 1 after a FAIL. The watch (GMPICallWatch.hpp) adds one line per
 * rank at MPI_Finalize. The ctest entries (CMakeLists.txt) run it through CheckJobEnds.cmake,
 * which checks the exit code, the texts and that the job ends within a bound.
 *
 * Unless a scenario says otherwise the cascade initializes MPI. Every member prints "member <r>
 * served <k> rounds", every lead "lead led <k> rounds".
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include <mpi.h>

#include "common/GConfigEmission.hpp"
#include "common/GExceptions.hpp"
#include "common/GParserBuilder.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/mpi/GMPICascade.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"
#include "courtier/mpi/GMPIPolledCalls.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

#include "GMPICallWatch.hpp"

namespace Courtier = Gem::Courtier;
namespace CallWatch = Gem::Tests::CallWatch;
using Courtier::GMPICascadeLayout;
using Courtier::processRole;

namespace {

/******************************************************************************/
/** @brief The rank the launcher gave this process (MPI need not be initialized; -1 without one)
 *  @return The rank */
int launcherRank() {
    for(const char *name : {"OMPI_COMM_WORLD_RANK", "PMIX_RANK", "PMI_RANK"}) {
        if(const char *value =
               std::getenv(name)) { // NOLINT(concurrency-mt-unsafe): before any thread
            return std::stoi(value);
        }
    }
    return -1;
}

/** @brief The name of a role, for messages @param role The role @return Its name */
std::string_view roleText(processRole role) {
    switch(role) {
    case processRole::SUBMITTER:
        return "SUBMITTER";
    case processRole::CLIENT:
        return "CLIENT";
    case processRole::BYSTANDER:
        return "BYSTANDER";
    }
    return "UNKNOWN";
}

/******************************************************************************/
/**
 * The OK and FAIL lines of one rank, and the count of its failures. Lines may come from several
 * threads.
 */
class Report {
public:
    /** @brief A report of @p scenario @param scenario The scenario's name */
    explicit Report(std::string scenario)
      : scenario_(std::move(scenario))
      , rank_(launcherRank()) { /* nothing */
    }

    /** @brief Prints a FAIL line and counts it */
    template <typename... args_type>
    void fail(std::format_string<args_type...> what, args_type &&...args) {
        ++failures_;
        std::println(
            "FAIL: {}: rank {}: {}",
            scenario_,
            rank_,
            std::format(what, std::forward<args_type>(args)...)
        );
        std::ignore = std::fflush(stdout);
    }

    /** @brief Prints an OK line */
    template <typename... args_type>
    void ok(std::format_string<args_type...> what, args_type &&...args) {
        std::println(
            "OK: {}: rank {} {}",
            scenario_,
            rank_,
            std::format(what, std::forward<args_type>(args)...)
        );
        std::ignore = std::fflush(stdout);
    }

    /** @brief Checks @p condition, a FAIL line naming @p what if it does not hold */
    void check(bool condition, std::string_view what) {
        if(not condition) {
            this->fail("{}", what);
        }
    }

    /** @brief Checks that @p call throws a std::exception whose text contains @p text */
    template <typename F>
    void expectThrow(std::string_view what, std::string_view text, F &&call) {
        try {
            std::forward<F>(call)();
        }
        catch(const std::exception &e) {
            if(not std::string_view(e.what()).contains(text)) {
                this->fail(
                    "{} threw, but its text does not contain '{}': {}",
                    what,
                    text,
                    e.what()
                );
            }
            return;
        }
        this->fail("{} did not throw", what);
    }

    /** @brief The rank of the lines */
    [[nodiscard]] int rank() const {
        return rank_;
    }
    /** @brief The number of FAIL lines so far */
    [[nodiscard]] int failures() const {
        return failures_.load();
    }

private:
    std::string scenario_;         ///< The scenario's name
    int rank_;                     ///< The rank in MPI_COMM_WORLD (from the launcher)
    std::atomic<int> failures_{0}; ///< The number of FAIL lines
};

/******************************************************************************/
/** @brief A configuration of the cascade with the default pauses
 *  @param group_size The group size @param layout The layout @return The configuration */
Courtier::GMPICascadeConfig configOf(std::uint16_t group_size, GMPICascadeLayout layout) {
    return Courtier::GMPICascadeConfig{.groupSize = group_size, .layout = layout};
}

/** @brief The role a base rank has @param base_rank The rank in the base communicator
 *  @param layout The layout @param g The group size @return The role */
processRole expectedRole(int base_rank, GMPICascadeLayout layout, int g) {
    const int first = layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS ? 1 : 0;
    if(base_rank < first) {
        return processRole::SUBMITTER;
    }
    return (base_rank - first) % g == 0 ? processRole::CLIENT : processRole::BYSTANDER;
}

/** @brief The size of the group a base rank belongs to @param base_rank The rank
 *  @param base_size The size of the base communicator @param layout The layout @param g The group
 *  size @return The group's size; 0 for the submitter */
int expectedGroupSize(int base_rank, int base_size, GMPICascadeLayout layout, int g) {
    const int first = layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS ? 1 : 0;
    if(base_rank < first) {
        return 0;
    }
    const int lead = first + (((base_rank - first) / g) * g);
    return std::min(g, base_size - lead);
}

/** @brief The size of a communicator @param comm It @return Its size */
int sizeOf(MPI_Comm comm) {
    int size = 0;
    MPI_Comm_size(comm, &size);
    return size;
}

/** @brief The rank in a communicator @param comm It @return The rank */
int rankIn(MPI_Comm comm) {
    int rank = 0;
    MPI_Comm_rank(comm, &rank);
    return rank;
}

/******************************************************************************/
/**
 * The lead's half of a round of squares: every process of the group gets one number (scatter),
 * squares it, and the lead gathers the squares and adds them up.
 *
 * @param group The group
 * @param cascade The cascade (for its pauses)
 * @param base A number that differs from round to round
 * @param between How long the lead waits between the scatter and the gather
 * @return true if the sum is right
 */
bool squareRound(
    MPI_Comm group,
    const Courtier::GMPICascade &cascade,
    double base,
    std::chrono::milliseconds between = std::chrono::milliseconds(0)
) {
    const auto &config = cascade.config();
    const int size = sizeOf(group);
    std::vector<double> numbers(static_cast<std::size_t>(size));
    double expected = 0.;
    for(std::size_t i = 0; i < numbers.size(); ++i) {
        numbers[i] = base + static_cast<double>(i) + 1.;
        expected += numbers[i] * numbers[i];
    }
    double own = 0.;
    if(Courtier::mpiScatter(
           numbers.data(),
           1,
           &own,
           MPI_DOUBLE,
           0,
           group,
           config.pauseMin,
           config.pauseMax
       ) != MPI_SUCCESS) {
        return false;
    }
    std::this_thread::sleep_for(between);
    const double square = own * own;
    std::vector<double> squares(numbers.size());
    if(Courtier::mpiGather(
           &square,
           1,
           squares.data(),
           MPI_DOUBLE,
           0,
           group,
           config.pauseMin,
           config.pauseMax
       ) != MPI_SUCCESS) {
        return false;
    }
    double sum = 0.;
    for(const double s : squares) {
        sum += s;
    }
    return sum == expected;
}

/******************************************************************************/
/** @brief An MPI_Ibarrier on @p group, completed with waitPolled() @param group The group */
void pollBarrier(MPI_Comm group) {
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitPolled() completes the request; the
    // checker does not know it
    MPI_Request request = MPI_REQUEST_NULL;
    MPI_Ibarrier(group, &request);
    std::ignore = Courtier::waitPolled(
        std::span<MPI_Request>(&request, 1),
        Courtier::GMPIPEERPOLLPAUSEMIN,
        Courtier::GMPIPEERPOLLPAUSEMAX
    );
    // NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)
}

/******************************************************************************/
/**
 * A member that squares its number of each round (the member half of squareRound()). With
 * collective set to true, setUp() and tearDown() are collective on the group (an MPI_Ibarrier
 * completed with waitPolled()), and every round checks that setUp() has run and tearDown() has not.
 */
class SquareMember final : public Courtier::GMPIGroupMemberI {
public:
    /** @brief A member @param report Where failures go @param collective Whether the lifecycle is collective */
    explicit SquareMember(Report &report, bool collective = false)
      : report_(report)
      , collective_(collective) { /* nothing */
    }

    /** @brief Counts, and with collective a barrier */
    void setUp(MPI_Comm group) override {
        ++set_ups_;
        this->barrier_(group);
    }
    /** @brief Counts, and with collective a barrier */
    void tearDown(MPI_Comm group) override {
        ++tear_downs_;
        this->barrier_(group);
    }
    /** @brief Receives a number, squares it, sends the square back */
    void serveRound(MPI_Comm group) override {
        if(collective_ && (set_ups_ != 1 || tear_downs_ != 0)) {
            report_.fail(
                "a round was served with {} setUp() and {} tearDown() before it",
                set_ups_,
                tear_downs_
            );
        }
        double number = 0.;
        std::ignore = Courtier::mpiScatter(
            nullptr,
            1,
            &number,
            MPI_DOUBLE,
            0,
            group,
            Courtier::GMPIPEERPOLLPAUSEMIN,
            Courtier::GMPIPEERPOLLPAUSEMAX
        );
        const double square = number * number;
        std::ignore = Courtier::mpiGather(
            &square,
            1,
            nullptr,
            MPI_DOUBLE,
            0,
            group,
            Courtier::GMPIPEERPOLLPAUSEMIN,
            Courtier::GMPIPEERPOLLPAUSEMAX
        );
        ++rounds_;
    }

    [[nodiscard]] int rounds() const {
        return rounds_;
    }
    [[nodiscard]] int setUps() const {
        return set_ups_;
    }
    [[nodiscard]] int tearDowns() const {
        return tear_downs_;
    }

private:
    /** @brief A barrier on @p group (pollBarrier()) if the lifecycle is collective */
    void barrier_(MPI_Comm group) const {
        if(collective_) {
            pollBarrier(group);
        }
    }

    Report &report_;
    bool collective_;
    int rounds_ = 0;
    int set_ups_ = 0;
    int tear_downs_ = 0;
};

/******************************************************************************/
/**
 * The lead's lifecycle: counts, with an optional barrier as SquareMember's, and an optional check
 * in tearDown().
 */
class LeadLifecycle final : public Courtier::GMPIGroupLifecycleI {
public:
    /** @brief A lifecycle @param collective Whether setUp()/tearDown() are collective
     *  @param on_tear_down Called in tearDown() (may be empty)
     *  @param on_set_up Called in setUp() (may be empty) */
    explicit LeadLifecycle(
        bool collective,
        std::function<void()> on_tear_down = {},
        std::function<void()> on_set_up = {}
    )
      : collective_(collective)
      , on_tear_down_(std::move(on_tear_down))
      , on_set_up_(std::move(on_set_up)) { /* nothing */
    }

    /** @brief Counts, calls the hook, and with collective a barrier */
    void setUp(MPI_Comm group) override {
        ++set_ups_;
        if(on_set_up_) {
            on_set_up_();
        }
        this->barrier_(group);
    }
    void tearDown(MPI_Comm group) override {
        ++tear_downs_;
        if(on_tear_down_) {
            on_tear_down_();
        }
        this->barrier_(group);
    }

    [[nodiscard]] int setUps() const {
        return set_ups_.load();
    }
    [[nodiscard]] int tearDowns() const {
        return tear_downs_.load();
    }

private:
    void barrier_(MPI_Comm group) const {
        if(collective_) {
            pollBarrier(group);
        }
    }

    bool collective_;
    std::function<void()> on_tear_down_;
    std::function<void()> on_set_up_;
    std::atomic<int> set_ups_{0};
    std::atomic<int> tear_downs_{0};
};

/******************************************************************************/
/** A member whose rounds do nothing; it counts them. */
class EmptyMember final : public Courtier::GMPIGroupMemberI {
public:
    void serveRound([[maybe_unused]] MPI_Comm group) override {
        ++rounds_;
    }
    [[nodiscard]] int rounds() const {
        return rounds_;
    }

private:
    int rounds_ = 0;
};

/******************************************************************************/
/** @brief The tag of the item number a lead sends to its members in the websocket scenario */
constexpr int NUMBERTAG = 7;
/** @brief The tag of a member's answer */
constexpr int ANSWERTAG = 8;

/**
 * A member that waits for its lead's message with probePolled(), receives it and answers with
 * a point-to-point message: the number times (its group rank + 1).
 */
class ProbingMember final : public Courtier::GMPIGroupMemberI {
public:
    /** @brief Waits for the lead's number, answers with its multiple */
    void serveRound(MPI_Comm group) override {
        const MPI_Status status = Courtier::probePolled(
            0,
            NUMBERTAG,
            group,
            Courtier::GMPIPEERPOLLPAUSEMIN,
            Courtier::GMPIPEERPOLLPAUSEMAX
        );
        std::int64_t number = 0;
        MPI_Recv(&number, 1, MPI_INT64_T, status.MPI_SOURCE, NUMBERTAG, group, MPI_STATUS_IGNORE);
        std::int64_t answer = number * (rankIn(group) + 1);
        // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitPolled() completes the request;
        // the checker does not know it
        MPI_Request request = MPI_REQUEST_NULL;
        MPI_Isend(&answer, 1, MPI_INT64_T, 0, ANSWERTAG, group, &request);
        std::ignore = Courtier::waitPolled(
            std::span<MPI_Request>(&request, 1),
            Courtier::GMPIPEERPOLLPAUSEMIN,
            Courtier::GMPIPEERPOLLPAUSEMAX
        );
        // NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)
        ++rounds_;
    }
    [[nodiscard]] int rounds() const {
        return rounds_;
    }

private:
    int rounds_ = 0;
};

/** @brief The lead's half for ProbingMember: sends @p number to every member and adds up its own
 *  share and their answers @param group The group @param number The number @return The sum */
std::int64_t probingRound(MPI_Comm group, std::int64_t number) {
    const int size = sizeOf(group);
    std::vector<std::int64_t> answers(static_cast<std::size_t>(size), 0);
    std::vector<MPI_Request> requests;
    for(int member = 1; member < size; ++member) {
        requests.push_back(MPI_REQUEST_NULL);
        MPI_Isend(&number, 1, MPI_INT64_T, member, NUMBERTAG, group, &requests.back());
        requests.push_back(MPI_REQUEST_NULL);
        MPI_Irecv(
            &answers[static_cast<std::size_t>(member)],
            1,
            MPI_INT64_T,
            member,
            ANSWERTAG,
            group,
            &requests.back()
        );
    }
    std::ignore = Courtier::waitPolled(
        requests,
        Courtier::GMPIPEERPOLLPAUSEMIN,
        Courtier::GMPIPEERPOLLPAUSEMAX
    );
    std::int64_t sum = number; // the lead's own share: the number times 1
    for(const auto answer : answers) {
        sum += answer;
    }
    return sum;
}

/******************************************************************************/
/**
 * The common part of most scenarios: a member serves (and, if asked, serves a second time, which
 * must return at once without a further setUp()); a lead confirms its role and leads @p rounds
 * rounds of squares, then releases; the submitter confirms its role. Prints the member's and the
 * lead's lines.
 *
 * @param report Where the lines go
 * @param cascade The cascade
 * @param member The member half
 * @param rounds The rounds of each lead
 * @param serve_twice Whether a member serves a second time
 */
void runGroups(
    Report &report,
    Courtier::GMPICascade &cascade,
    SquareMember &member,
    int rounds,
    bool serve_twice = false
) {
    switch(cascade.role()) {
    case processRole::BYSTANDER: {
        report.check(cascade.serve(member) == 0, "serve() did not return 0");
        if(serve_twice) {
            const auto start = std::chrono::steady_clock::now();
            const int again = cascade.serve(member);
            const auto took = std::chrono::steady_clock::now() - start;
            report.check(again == 0, "the second serve() did not return 0");
            report.check(took < std::chrono::seconds(1), "the second serve() took 1 s or more");
            report.check(member.setUps() == 1, "the second serve() set the member up again");
        }
        report.check(
            member.rounds() == rounds,
            std::format("the member served {} rounds, not {}", member.rounds(), rounds)
        );
        std::println("member {} served {} rounds", report.rank(), member.rounds());
    } break;
    case processRole::CLIENT: {
        cascade.requireRole(processRole::CLIENT);
        int led = 0;
        for(int r = 0; r < rounds; ++r) {
            const bool right =
                cascade.round([&](MPI_Comm group) { return squareRound(group, cascade, 10. * r); });
            report.check(right, std::format("the sum of round {} is wrong", r));
            ++led;
        }
        cascade.release();
        std::println("lead led {} rounds", led);
    } break;
    case processRole::SUBMITTER:
        cascade.requireRole(processRole::SUBMITTER);
        break;
    }
    std::ignore = std::fflush(stdout);
}

/******************************************************************************/
/**
 * Checks the roles, the groups and the configuration of a cascade on a base communicator of
 * @p base_size processes.
 */
void checkLayout(
    Report &report,
    const Courtier::GMPICascade &cascade,
    int base_rank,
    int base_size,
    GMPICascadeLayout layout,
    int g,
    bool roles_as_environment
) {
    const processRole expected = expectedRole(base_rank, layout, g);
    if(cascade.role() != expected) {
        report.fail(
            "world rank {} (base rank {}) has role {}, expected {}",
            report.rank(),
            base_rank,
            roleText(cascade.role()),
            roleText(expected)
        );
    }
    const processRole environment = Courtier::mpiProcessRole();
    if(roles_as_environment && environment != cascade.role()) {
        report.fail(
            "role() is {}, mpiProcessRole() {}",
            roleText(cascade.role()),
            roleText(environment)
        );
    }
    if(not roles_as_environment && environment != processRole::BYSTANDER) {
        report.fail("mpiProcessRole() is {}, not BYSTANDER", roleText(environment));
    }
    const int group_size = expectedGroupSize(base_rank, base_size, layout, g);
    if(group_size == 0) {
        report.check(cascade.group() == MPI_COMM_NULL, "the submitter has a group");
    }
    else if(cascade.group() == MPI_COMM_NULL) {
        report.fail("no group, expected one of {}", group_size);
    }
    else {
        report.check(
            sizeOf(cascade.group()) == group_size,
            std::format(
                "the group has {} processes, expected {}",
                sizeOf(cascade.group()),
                group_size
            )
        );
        const bool is_lead = rankIn(cascade.group()) == 0;
        report.check(
            is_lead == (cascade.role() == processRole::CLIENT),
            "the lead is not rank 0 of its group"
        );
    }
    const auto &config = cascade.config();
    report.check(
        std::cmp_equal(config.groupSize, g),
        std::format("config().groupSize is {}", config.groupSize)
    );
    report.check(config.layout == layout, "config().layout differs");
    report.check(
        config.pauseMin == Courtier::GMPIPEERPOLLPAUSEMIN &&
            config.pauseMax == Courtier::GMPIPEERPOLLPAUSEMAX,
        "config()'s pauses differ"
    );
}

/** @brief Calls of the wrong role must throw, and announce nothing */
void checkWrongRoleCalls(Report &report, Courtier::GMPICascade &cascade) {
    SquareMember unused(report);
    const auto half = [](MPI_Comm) { return 0; };
    switch(cascade.role()) {
    case processRole::SUBMITTER:
        report.expectThrow("round() on the submitter", "is called on a lead", [&] {
            std::ignore = cascade.round(half);
        });
        report.expectThrow("serve() on the submitter", "is called on a member", [&] {
            std::ignore = cascade.serve(unused);
        });
        break;
    case processRole::BYSTANDER:
        report.expectThrow("round() on a member", "is called on a lead", [&] {
            std::ignore = cascade.round(half);
        });
        break;
    case processRole::CLIENT:
        report.expectThrow("serve() on a lead", "is called on a member", [&] {
            std::ignore = cascade.serve(unused);
        });
        break;
    }
}

/******************************************************************************/
// The scenarios. Each returns the number of its failures.

/** @brief The half of a round that does nothing */
constexpr auto NOTHING = [](MPI_Comm) { return 0; };

/******************************************************************************/
/**
 * The layouts (entries GCourtierMpiCascadeLayoutA, ...LayoutB, ...LayoutBTwoGroups): the roles by
 * base rank, the groups, the configuration on every rank, the calls of a wrong role, 50 rounds per
 * lead, and a second serve() of every member that returns at once.
 */
int layoutScenario(Report &report, GMPICascadeLayout layout) {
    constexpr int G = 3;
    constexpr int ROUNDS = 50;
    SquareMember member(report);
    Courtier::GMPICascade cascade([layout] { return configOf(G, layout); });
    checkLayout(
        report,
        cascade,
        rankIn(MPI_COMM_WORLD),
        sizeOf(MPI_COMM_WORLD),
        layout,
        G,
        layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS
    );
    // The watch sees the calls made inside courtier's MPI library: the environment's initialization
    // and the cascade's two splits (else the counts of the other scenarios would mean nothing)
    report.check(
        CallWatch::watchedInitThreads() == 1,
        std::format(
            "the watch saw MPI_Init_thread {} time(s), not once",
            CallWatch::watchedInitThreads()
        )
    );
    report.check(
        CallWatch::watchedSplits() == 2,
        std::format(
            "the watch saw MPI_Comm_split {} time(s), not twice",
            CallWatch::watchedSplits()
        )
    );
    checkWrongRoleCalls(report, cascade);
    runGroups(report, cascade, member, ROUNDS, true);
    return report.failures();
}

/**
 * A group of one is a group (entries ...OneA, ...OneB): its lead announces every round, so the
 * lead makes the broadcast of the configuration, 20 "go" and one "stop"; the submitter only the
 * first.
 */
int oneScenario(Report &report, GMPICascadeLayout layout) {
    constexpr int ROUNDS = 20;
    SquareMember member(report);
    Courtier::GMPICascade cascade([layout] { return configOf(1, layout); });
    runGroups(report, cascade, member, ROUNDS);
    const std::size_t expected = cascade.role() == processRole::CLIENT ? ROUNDS + 2 : 1;
    report.check(
        CallWatch::watchedIbcasts() == expected,
        std::format("{} MPI_Ibcast calls, expected {}", CallWatch::watchedIbcasts(), expected)
    );
    return report.failures();
}

/** Only rank 0's configuration counts (entry ...RankZeroSize): the others are handed another one. */
int rankZeroSizeScenario(Report &report) {
    constexpr int G = 3;
    int calls = 0;
    SquareMember member(report);
    Courtier::GMPICascade cascade([&calls] {
        ++calls;
        return configOf(
            rankIn(MPI_COMM_WORLD) == 0 ? G : 2,
            GMPICascadeLayout::SUBMITTER_AND_GROUPS
        );
    });
    const int world_rank = rankIn(MPI_COMM_WORLD);
    report.check(
        calls == (world_rank == 0 ? 1 : 0),
        std::format("the configuration callable ran {} time(s) on world rank {}", calls, world_rank)
    );
    checkLayout(
        report,
        cascade,
        world_rank,
        sizeOf(MPI_COMM_WORLD),
        GMPICascadeLayout::SUBMITTER_AND_GROUPS,
        G,
        true
    );
    runGroups(report, cascade, member, 10);
    return report.failures();
}

/**
 * The lifecycle pair (entry ...Lifecycle): lead() sets the lead up, serve() the members, both
 * collectively; release() and the end of serve() tear them down; every round lies in between.
 */
int lifecycleScenario(Report &report) {
    constexpr int ROUNDS = 10;
    SquareMember member(report, true);
    LeadLifecycle lifecycle(true);
    Courtier::GMPICascade cascade([] {
        return configOf(3, GMPICascadeLayout::SUBMITTER_AND_GROUPS);
    });
    switch(cascade.role()) {
    case processRole::BYSTANDER:
        std::ignore = cascade.serve(member);
        report.check(
            member.setUps() == 1 && member.tearDowns() == 1,
            "the member's setUp()/tearDown() did not run once each"
        );
        report.check(
            member.rounds() == ROUNDS,
            std::format("the member served {} rounds", member.rounds())
        );
        std::println("member {} served {} rounds", report.rank(), member.rounds());
        break;
    case processRole::CLIENT: {
        cascade.requireRole(processRole::CLIENT);
        cascade.lead(lifecycle);
        report.check(lifecycle.setUps() == 1, "lead() did not set the lead up");
        for(int r = 0; r < ROUNDS; ++r) {
            const bool right = cascade.round([&](MPI_Comm group) {
                report.check(
                    lifecycle.setUps() == 1 && lifecycle.tearDowns() == 0,
                    "a round ran outside the lifecycle"
                );
                return squareRound(group, cascade, r);
            });
            report.check(right, "a wrong sum");
        }
        cascade.release();
        report.check(lifecycle.tearDowns() == 1, "release() did not tear the lead down");
        std::println("lead led {} rounds", ROUNDS);
    } break;
    case processRole::SUBMITTER:
        cascade.requireRole(processRole::SUBMITTER);
        break;
    }
    return report.failures();
}

/** A member that leaves without serve() ends the job (entry ...MemberNeverServes). */
int memberNeverServesScenario(Report &report) {
    SquareMember member(report);
    Courtier::GMPICascade cascade([] {
        return configOf(3, GMPICascadeLayout::SUBMITTER_AND_GROUPS);
    });
    if(cascade.role() == processRole::BYSTANDER && rankIn(cascade.group()) == 1) {
        std::println("member {} leaves without serve()", report.rank());
        std::ignore = std::fflush(stdout);
        return report.failures(); // the cascade's destructor ends the job
    }
    runGroups(report, cascade, member, 0);
    return report.failures();
}

/**
 * A base communicator that is not MPI_COMM_WORLD (entry ...BaseComm): the program initializes MPI
 * and reverses the ranks; the roles follow the base rank; MPI stays the program's to finalize.
 */
int baseCommScenario(Report &report) {
    constexpr int G = 2;
    int provided = 0;
    MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
    const int world_rank = rankIn(MPI_COMM_WORLD);
    const int world_size = sizeOf(MPI_COMM_WORLD);
    MPI_Comm base = MPI_COMM_NULL;
    MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - 1 - world_rank, &base);
    const int base_rank = rankIn(base);
    {
        SquareMember member(report);
        Courtier::GMPICascade cascade(
            [] { return configOf(G, GMPICascadeLayout::SUBMITTER_AND_GROUPS); },
            base
        );
        checkLayout(
            report,
            cascade,
            base_rank,
            world_size,
            GMPICascadeLayout::SUBMITTER_AND_GROUPS,
            G,
            true
        );
        runGroups(report, cascade, member, 10);
    }
    int finalized = 0;
    MPI_Finalized(&finalized);
    report.check(finalized == 0, "the cascade finalized MPI, which the program had initialized");
    MPI_Comm_free(&base);
    MPI_Finalize();
    return report.failures();
}

/** @brief The rounds one thread of twoThreadsScenario() leads @param report Where failures go
 *  @param cascade The cascade @param led Counts the rounds led @param t The thread's number
 *  @param rounds How many rounds */
void leadRoundsOfThread(
    Report &report,
    Courtier::GMPICascade &cascade,
    std::atomic<int> &led,
    int t,
    int rounds
) {
    for(int r = 0; r < rounds; ++r) {
        // The first thread waits between its scatter and its gather: without the cascade's lock
        // the other thread's round would overtake it, and the gathers would be matched to the
        // wrong scatters
        const bool right = cascade.round([&](MPI_Comm group) {
            return squareRound(
                group,
                cascade,
                (1000. * t) + r,
                std::chrono::milliseconds(t == 0 ? 3 : 0)
            );
        });
        report.check(right, std::format("thread {}: the sum of round {} is wrong", t, r));
        ++led;
    }
}

/** Two threads call round() (entry ...TwoThreads): the rounds run one after the other. */
int twoThreadsScenario(Report &report) {
    constexpr int PER_THREAD = 25;
    SquareMember member(report);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() != processRole::CLIENT) {
        runGroups(report, cascade, member, 2 * PER_THREAD);
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    std::atomic<int> led{0};
    {
        Gem::Common::Concurrency::GThreadGroup threads;
        for(int t = 0; t < 2; ++t) {
            threads.create_thread([&report, &cascade, &led, t] {
                leadRoundsOfThread(report, cascade, led, t, PER_THREAD);
            });
        }
        threads.join_all();
    }
    cascade.release();
    report.check(led.load() == 2 * PER_THREAD, "not every round was led");
    std::println("lead led {} rounds", led.load());
    return report.failures();
}

/**
 * release() during a round (entry ...ReleaseDuringRound): it waits for the round, the lead's
 * tearDown() comes after it, and a round() afterwards throws.
 */
int releaseDuringRoundScenario(Report &report) {
    SquareMember member(report);
    std::atomic<bool> in_round{false};
    LeadLifecycle lifecycle(false, [&report, &in_round] {
        if(in_round.load()) {
            report.fail("tearDown() ran while a round was running");
        }
    });
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() != processRole::CLIENT) {
        runGroups(report, cascade, member, 1);
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    cascade.lead(lifecycle);
    auto entered = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    std::atomic<bool> right{false};
    {
        Gem::Common::Concurrency::GThreadGroup threads;
        threads.create_thread([&cascade, &in_round, &right, entered] {
            right = cascade.round([&](MPI_Comm group) {
                entered->count_down();
                in_round = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const bool sum_right = squareRound(group, cascade, 5.);
                in_round = false;
                return sum_right;
            });
        });
        entered->wait();
        cascade.release();
        threads.join_all();
    }
    report.check(right.load(), "the round that ran during release() has a wrong sum");
    report.check(lifecycle.tearDowns() == 1, "release() did not tear the lead down once");
    report.expectThrow("round() after release()", "release()", [&] {
        std::ignore = cascade.round(NOTHING);
    });
    std::println("lead led 1 rounds");
    return report.failures();
}

/**
 * --update-configs (entry ...Refresh, no launcher): the cascade makes no MPI call, the callable
 * writes the configuration file with its default, and the functions of the rounds throw.
 */
int refreshScenario(Report &report, int argc, char **argv) {
    if(not Gem::Common::configEmissionRequested(argc, argv)) {
        report.fail("the scenario must be started with --update-configs");
        return report.failures();
    }
    const std::filesystem::path file{std::string(Courtier::GMPICASCADECONFIGFILE)};
    std::filesystem::remove(file); // so that its existence afterwards says something
    Gem::Common::beginConfigEmission();
    Courtier::GMPICascade cascade([] {
        Courtier::GMPICascadeConfig config;
        config.load(std::string(Courtier::GMPICASCADECONFIGFILE));
        return config;
    });
    report.check(
        CallWatch::watchedInitThreads() == 0,
        std::format("MPI_Init_thread called {} time(s)", CallWatch::watchedInitThreads())
    );
    int initialized = 0;
    MPI_Initialized(&initialized);
    report.check(initialized == 0, "MPI is initialized");
    report.check(cascade.role() == processRole::SUBMITTER, "the role is not SUBMITTER");
    report.check(cascade.group() == MPI_COMM_NULL, "there is a group");
    SquareMember member(report);
    LeadLifecycle lifecycle(false);
    report.expectThrow("serve()", "--update-configs", [&] { std::ignore = cascade.serve(member); });
    report.expectThrow("lead()", "--update-configs", [&] { cascade.lead(lifecycle); });
    report.expectThrow("round()", "--update-configs", [&] {
        std::ignore = cascade.round(NOTHING);
    });
    cascade.release(); // nothing
    report.check(std::filesystem::exists(file), std::format("{} was not written", file.string()));
    Courtier::GMPICascadeConfig reread{
        .groupSize = 0,
        .pauseMin = std::chrono::microseconds(1),
        .pauseMax = std::chrono::microseconds(1)
    };
    reread.load(file.string());
    report.check(
        reread.groupSize == 4,
        std::format("the file has the group size {}, not 4", reread.groupSize)
    );
    report.check(
        reread.pauseMin == Courtier::GMPIPEERPOLLPAUSEMIN &&
            reread.pauseMax == Courtier::GMPIPEERPOLLPAUSEMAX,
        "the file does not have the default poll pauses"
    );
    return report.failures();
}

/** requireRole() (entries ...RequireRoleA, ...RequireRoleB): a wrong role is refused with the layout and the remedy. */
int requireRoleScenario(Report &report, GMPICascadeLayout layout) {
    SquareMember member(report);
    Courtier::GMPICascade cascade([layout] { return configOf(3, layout); });
    const bool a = layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS;
    const std::string_view layout_name = a ? "SUBMITTER_AND_GROUPS" : "GROUPS_ONLY";
    const std::string_view remedy = a ? "--consumer mpi" : "--client";
    if(cascade.role() == processRole::BYSTANDER) {
        report.expectThrow("requireRole() on a member", "serve()", [&] {
            cascade.requireRole(processRole::BYSTANDER);
        });
    }
    else {
        for(const processRole wrong :
            {processRole::SUBMITTER, processRole::CLIENT, processRole::BYSTANDER}) {
            if(wrong == cascade.role()) {
                continue;
            }
            const std::string what = std::format(
                "requireRole({}) on {}",
                roleText(wrong),
                cascade.role() == processRole::CLIENT ? "a lead" : "the submitter"
            );
            report.expectThrow(what, layout_name, [&] { cascade.requireRole(wrong); });
            report.expectThrow(what, remedy, [&] { cascade.requireRole(wrong); });
        }
    }
    runGroups(report, cascade, member, 3); // confirms role() on the lead and the submitter
    return report.failures();
}

/** An idle member waits without waiting busily (entry ...IdleMember). */
int idleMemberScenario(Report &report) {
    SquareMember member(report);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() == processRole::CLIENT) {
        cascade.requireRole(processRole::CLIENT);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        cascade.release();
        std::println("lead led 0 rounds");
        return report.failures();
    }
    const auto cpu_seconds = [] {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
               (1e-6 * static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec));
    };
    const double cpu_before = cpu_seconds();
    const auto wall_before = std::chrono::steady_clock::now();
    std::ignore = cascade.serve(member);
    const double cpu = cpu_seconds() - cpu_before;
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_before).count();
    const double share = 100. * cpu / wall;
    std::println(
        "member {} used {:.1f} % of a core while it waited {:.2f} s",
        report.rank(),
        share,
        wall
    );
    report.check(share < 10., std::format("an idle member used {:.1f} % of a core", share));
    std::println("member {} served {} rounds", report.rank(), member.rounds());
    return report.failures();
}

/** What is wrong with rank 0's configuration in refusedConfigScenario() */
enum class Refusal {
    GROUP_SIZE,  ///< a group size of 0
    PAUSES,      ///< pauses out of order
    RANK0_THROWS ///< rank 0's callable throws
};

/**
 * A configuration that is refused or missing (entries ...GroupSizeZero, ...PausesRefused,
 * ...ConfigThrows): every rank throws, none waits for ever, and MPI is finalized again, as the
 * cascade had initialized it.
 *
 * @param refusal What is wrong
 */
int refusedConfigScenario(Report &report, Refusal refusal) {
    std::string text;
    try {
        const Courtier::GMPICascade cascade([refusal]() -> Courtier::GMPICascadeConfig {
            switch(refusal) {
            case Refusal::RANK0_THROWS:
                throw std::runtime_error("the configuration of rank 0 failed on purpose");
            case Refusal::PAUSES:
                return Courtier::GMPICascadeConfig{
                    .groupSize = 3,
                    .layout = GMPICascadeLayout::GROUPS_ONLY,
                    .pauseMin = std::chrono::microseconds(500),
                    .pauseMax = std::chrono::microseconds(100)
                };
            case Refusal::GROUP_SIZE:
                break;
            }
            return configOf(0, GMPICascadeLayout::GROUPS_ONLY);
        });
        report.fail("the constructor did not throw");
    }
    catch(const std::exception &e) {
        text = e.what();
    }
    std::string_view expected = "The group size of the MPI cascade (sub_client_group_size) is 0";
    if(refusal == Refusal::RANK0_THROWS) {
        expected = report.rank() == 0 ? "the configuration of rank 0 failed on purpose"
                                      : "could not provide the cascade's configuration";
    }
    if(refusal == Refusal::PAUSES) {
        expected = "pause_min is 500 us and pause_max is 100 us";
    }
    report.check(
        text.contains(expected),
        std::format("the text '{}' does not contain '{}'", text, expected)
    );
    int finalized = 0;
    MPI_Finalized(&finalized);
    report.check(finalized == 1, "MPI is not finalized");
    return report.failures();
}

/**
 * A lead that has not confirmed its role ends the job at its first use (entries ...RoleNotConfirmed
 * for round(), ...RoleNotConfirmedLead for lead()): the group is released first, so every
 * member's serve() returns, and the message names requireRole().
 *
 * @param by_lead Whether the first use is lead() (with a collective lifecycle) rather than round()
 */
int roleNotConfirmedScenario(Report &report, bool by_lead) {
    SquareMember member(report, by_lead);
    LeadLifecycle lifecycle(by_lead);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() == processRole::BYSTANDER) {
        report.check(cascade.serve(member) == 0, "serve() did not return 0");
        std::println("member {} served {} rounds", report.rank(), member.rounds());
        return report.failures();
    }
    if(by_lead) {
        cascade.lead(lifecycle);
        report.fail("lead() before requireRole() did not end the job");
    }
    else {
        std::ignore =
            cascade.round([&](MPI_Comm group) { return squareRound(group, cascade, 1.); });
        report.fail("round() before requireRole() did not end the job");
    }
    return report.failures();
}

/** Where failureScenario() injects an exception */
enum class Injected {
    SERVE_ROUND, ///< a member's serveRound(), in its second round
    LEAD_HALF,   ///< the lead's half of the second round, before any collective
    SET_UP,      ///< a member's setUp()
    TEAR_DOWN    ///< the lead's tearDown(), called by release()
};

/** A member that throws where it is told to, and squares its number otherwise. */
class InjectingMember final : public Courtier::GMPIGroupMemberI {
public:
    /** @brief A member @param report Where failures go @param injected Where it throws */
    InjectingMember(Report &report, Injected injected)
      : square_(report)
      , injected_(injected) { /* nothing */
    }
    /** @brief Throws if told to, in setUp() */
    void setUp(MPI_Comm group) override {
        if(injected_ == Injected::SET_UP) {
            throw std::runtime_error("injected exception in setUp()");
        }
        square_.setUp(group);
    }
    /** @brief Throws in the second round if told to, squares otherwise */
    void serveRound(MPI_Comm group) override {
        if(injected_ == Injected::SERVE_ROUND && square_.rounds() == 1) {
            throw std::runtime_error("injected exception in serveRound()");
        }
        square_.serveRound(group);
    }
    /** @brief The rounds served */
    [[nodiscard]] int rounds() const {
        return square_.rounds();
    }

private:
    SquareMember square_; ///< What it does when it does not throw
    Injected injected_;   ///< Where it throws
};

/**
 * An exception out of a half or a lifecycle function ends the job (entries ...ServeRoundThrows,
 * ...LeadHalfThrows, ...SetUpThrows, ...TearDownThrows): the cascade logs where and what, and
 * calls MPI_Abort; no rank is left waiting. Every rank that comes back from its part prints a FAIL.
 *
 * @param injected Where the exception comes from
 */
int failureScenario(Report &report, Injected injected) {
    constexpr int ROUNDS = 5;
    InjectingMember member(report, injected);
    LeadLifecycle lifecycle(false, [injected] {
        if(injected == Injected::TEAR_DOWN) {
            throw std::runtime_error("injected exception in tearDown()");
        }
    });
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() == processRole::BYSTANDER) {
        std::ignore = cascade.serve(member);
        report.fail(
            "member {} came back from serve() after {} rounds",
            report.rank(),
            member.rounds()
        );
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    cascade.lead(lifecycle);
    for(int r = 0; r < ROUNDS; ++r) {
        std::ignore = cascade.round([&](MPI_Comm group) {
            if(injected == Injected::LEAD_HALF && r == 1) {
                throw std::runtime_error("injected exception in the lead's half");
            }
            return squareRound(group, cascade, r);
        });
    }
    cascade.release();
    report.fail("the lead came back from its rounds and release()");
    return report.failures();
}

/**
 * An MPI error in the cascade ends the job (entries ...IbcastStartFails, ...TestallFails): the
 * watch makes the start of rank 0's broadcast of the configuration fail, or rank 1's wait for it.
 * Neither the rank with the error nor the others may be left waiting.
 *
 * @param at_start Whether the start fails (rank 0) rather than the wait (rank 1)
 */
int mpiErrorScenario(Report &report, bool at_start) {
    if(at_start && report.rank() == 0) {
        CallWatch::failNextIbcastStart();
    }
    if(not at_start && report.rank() == 1) {
        CallWatch::failNextTestall();
    }
    SquareMember member(report);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    runGroups(report, cascade, member, 1);
    report.fail("the MPI error did not end the job");
    return report.failures();
}

/**
 * lead() in the wrong order is refused (entries ...LeadAfterRound, ...LeadTwice): after a round
 * and after release(), or a second time. The refusals throw before anything is announced, so the
 * group stays in step and ends in order.
 *
 * @param twice Whether lead() is called twice rather than after a round and after release()
 */
int leadOrderScenario(Report &report, bool twice) {
    SquareMember member(report);
    LeadLifecycle lifecycle(false);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() != processRole::CLIENT) {
        runGroups(report, cascade, member, 1);
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    if(twice) {
        cascade.lead(lifecycle);
        report.expectThrow("a second lead()", "a group is set up once", [&] {
            cascade.lead(lifecycle);
        });
    }
    report.check(
        cascade.round([&](MPI_Comm group) { return squareRound(group, cascade, 2.); }),
        "a wrong sum"
    );
    if(not twice) {
        report.expectThrow("lead() after a round", "A round was announced already", [&] {
            cascade.lead(lifecycle);
        });
    }
    cascade.release();
    if(not twice) {
        report.expectThrow("lead() after release()", "The group was released (release())", [&] {
            cascade.lead(lifecycle);
        });
    }
    report.check(
        lifecycle.setUps() == (twice ? 1 : 0),
        "the lead was set up a wrong number of times"
    );
    std::println("lead led 1 rounds");
    return report.failures();
}

/**
 * A call from inside a lead's half that would wait for its own round ends the job with a message
 * (entries ...ReleaseInsideRound, ...RoundInsideRound) instead of a deadlock.
 *
 * @param release Whether the half calls release() rather than round()
 */
int insideRoundScenario(Report &report, bool release) {
    SquareMember member(report);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() != processRole::CLIENT) {
        runGroups(report, cascade, member, 1);
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    std::ignore = cascade.round([&](MPI_Comm group) {
        if(release) {
            cascade.release();
        }
        else {
            std::ignore = cascade.round(NOTHING);
        }
        return squareRound(group, cascade, 3.);
    });
    report.fail("the call from inside the round did not end the job");
    return report.failures();
}

/**
 * A rank on which an MPI environment exists already (entry ...EnvironmentExists): rank 1 builds a
 * bystander's environment of its own before the cascade. It refuses ("exists already"), and the
 * other ranks refuse too instead of waiting for it; every rank ends with MPI finalized.
 */
int environmentExistsScenario(Report &report) {
    std::shared_ptr<Courtier::Consumers::GMPIEnvironment> own;
    if(report.rank() == 1) {
        own = Courtier::mpiEnvironmentOn([] { return MPI_COMM_NULL; });
    }
    std::string text;
    try {
        const Courtier::GMPICascade cascade([] {
            return configOf(3, GMPICascadeLayout::GROUPS_ONLY);
        });
        report.fail("the constructor did not throw");
    }
    catch(const std::exception &e) {
        text = e.what();
    }
    const std::string_view expected = report.rank() == 1 ? "An MPI environment exists already"
                                                         : "could not take part in the cascade";
    report.check(
        text.contains(expected),
        std::format("the text '{}' does not contain '{}'", text, expected)
    );
    own.reset(); // rank 1: finalizes MPI, which its environment initialized
    int finalized = 0;
    MPI_Finalized(&finalized);
    report.check(finalized == 1, "MPI is not finalized");
    return report.failures();
}

/**
 * The submitter's safeguard (entry ...SubmitterNeverConfirms): in SUBMITTER_AND_GROUPS rank 0
 * never confirms its role; the leads and members serve as usual. The submitter's destructor ends
 * the job.
 */
int submitterNeverConfirmsScenario(Report &report) {
    SquareMember member(report);
    Courtier::GMPICascade cascade([] {
        return configOf(3, GMPICascadeLayout::SUBMITTER_AND_GROUPS);
    });
    if(cascade.role() == processRole::SUBMITTER) {
        std::println("the submitter leaves without confirming its role");
        std::ignore = std::fflush(stdout);
        return report.failures(); // the cascade's destructor ends the job
    }
    runGroups(report, cascade, member, 2);
    return report.failures();
}

/** A member whose serveRound() calls a hook (set after the cascade exists). */
class HookMember final : public Courtier::GMPIGroupMemberI {
public:
    /** @brief Calls the hook */
    void serveRound([[maybe_unused]] MPI_Comm group) override {
        if(hook) {
            hook();
        }
    }
    std::function<void()> hook; ///< What serveRound() does
};

/** Where insideCallbackScenario() calls into the cascade */
enum class Reentry {
    ROUND_IN_SET_UP,      ///< the lead's setUp() calls round()
    RELEASE_IN_TEAR_DOWN, ///< the lead's tearDown() calls release()
    SERVE_IN_SERVE_ROUND  ///< a member's serveRound() calls serve()
};

/**
 * A call into the cascade from inside a lifecycle function or a member's half ends the job with a
 * message (entries ...RoundInsideSetUp, ...ReleaseInsideTearDown, ...ServeInsideServeRound)
 * instead of a deadlock or a group out of step.
 *
 * @param reentry Where the call is made
 */
int insideCallbackScenario(Report &report, Reentry reentry) {
    HookMember member;
    Courtier::GMPICascade *cascade_of_hooks = nullptr; // set below; the hooks run after that
    LeadLifecycle lifecycle(
        false,
        [&] {
            if(reentry == Reentry::RELEASE_IN_TEAR_DOWN) {
                cascade_of_hooks->release();
            }
        },
        [&] {
            if(reentry == Reentry::ROUND_IN_SET_UP) {
                std::ignore = cascade_of_hooks->round(NOTHING);
            }
        }
    );
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    cascade_of_hooks = &cascade;
    if(cascade.role() == processRole::BYSTANDER) {
        if(reentry == Reentry::SERVE_IN_SERVE_ROUND) {
            member.hook = [&] { std::ignore = cascade.serve(member); };
        }
        std::ignore = cascade.serve(member);
        std::println("member {} came back from serve()", report.rank());
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    cascade.lead(lifecycle);
    std::ignore = cascade.round(NOTHING);
    cascade.release();
    report.fail("the call into the cascade did not end the job");
    return report.failures();
}

/**
 * A lead that never confirms its role and never uses the cascade (entry ...NeverUsed): the
 * destructor releases the group and ends the job, the last safeguard.
 */
int neverUsedScenario(Report &report) {
    SquareMember member(report);
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() != processRole::CLIENT) {
        runGroups(report, cascade, member, 0);
        return report.failures();
    }
    std::println("lead leaves without confirming its role and without a round");
    std::ignore = std::fflush(stdout);
    return report.failures(); // the cascade's destructor ends the job
}

/**
 * The cascade's configuration file (entry ...ConfigFile): rank 0 reads the group size and the poll
 * pauses from it, and every rank uses them.
 */
int configFileScenario(Report &report) {
    Courtier::GMPICascade cascade([] {
        const std::filesystem::path file{std::string(Courtier::GMPICASCADECONFIGFILE)};
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << R"({ "sub_client_group_size": { "value": 3 },
  "sub_client_poll_pause_min_us": { "value": 20 },
  "sub_client_poll_pause_max_us": { "value": 400 } })";
        Courtier::GMPICascadeConfig config{.layout = GMPICascadeLayout::GROUPS_ONLY};
        config.load(file.string());
        return config;
    });
    const auto &config = cascade.config();
    report.check(
        config.groupSize == 3,
        std::format("the group size is {}, not 3", config.groupSize)
    );
    report.check(
        config.pauseMin == std::chrono::microseconds(20) &&
            config.pauseMax == std::chrono::microseconds(400),
        std::format(
            "the pauses are {} and {} us, not 20 and 400",
            config.pauseMin.count(),
            config.pauseMax.count()
        )
    );
    SquareMember member(report);
    runGroups(report, cascade, member, 5);
    return report.failures();
}

/** @brief What runOverWebsocket() saw */
struct WebsocketRun {
    std::ptrdiff_t processed = 0; ///< The items that came back processed
    bool clientThrew = false;     ///< Whether the client's run() threw
};

/**
 * @brief Rank 0 of websocketScenario(): a websocket server and a client of it in this process,
 * whose item processor evaluates every item in a round of the group; then the client ends and the
 * lead releases its members.
 * @param cascade The lead's cascade @param n_items How many items @param factor What the group
 * multiplies an item's number with @param led Counts the rounds @param wrong Counts wrong values
 * @return What happened
 */
WebsocketRun runOverWebsocket(
    Courtier::GMPICascade &cascade,
    std::size_t n_items,
    std::int64_t factor,
    std::atomic<int> &led,
    std::atomic<int> &wrong
) {
    Courtier::GWebsocketConsumerConfig config{
        .ip = "127.0.0.1",
        .serializationMode = Gem::Common::serializationMode::GEM_BINARY,
        .nThreads = 2,
        .clientPrefetchDepth = 2,
        .clientParallelEvaluations = 2,
    };
    auto consumer =
        std::make_shared<Courtier::GWebsocketConsumerT<Courtier::GFaultyContainer>>(config);
    // Every item is evaluated once: none is given up on time and evaluated again
    consumer->setTimeoutTreatment(Courtier::timeoutTreatment::wait_indefinitely);
    consumer->startServer();
    config.port = consumer->getPort();
    auto client =
        std::make_shared<Courtier::Consumers::GWebsocketClientT<Courtier::GFaultyContainer>>(
            config
        );
    client->setItemProcessor([&cascade, &led, &wrong, factor](Courtier::GFaultyContainer &item) {
        const auto number = static_cast<std::int64_t>(item.get_stored_number()) + 1;
        const std::int64_t value =
            cascade.round([number](MPI_Comm group) { return probingRound(group, number); });
        ++led;
        if(value != number * factor) {
            ++wrong;
        }
        item.markAsProcessedWith({true});
    });

    std::vector<std::unique_ptr<Courtier::GFaultyContainer>> items;
    items.reserve(n_items);
    for(std::size_t i = 0; i < n_items; ++i) {
        items.push_back(std::make_unique<Courtier::GFaultyContainer>(i));
    }
    std::atomic<bool> client_threw{false};
    {
        Gem::Common::Concurrency::GThreadGroup threads;
        threads.create_thread([&client, &client_threw] {
            try {
                client->run();
            }
            catch(...) {
                client_threw = true;
            }
        });
        consumer->processBatch(
            std::span<std::unique_ptr<Courtier::GFaultyContainer>>(items.data(), items.size()),
            Courtier::GSubmissionPolicy::full_success_or_fatal()
        );
        client->flagCloseRequested();
        threads.join_all();
    }
    cascade.release();
    consumer->stopServer();
    const auto processed =
        std::ranges::count_if(items, [](const auto &item) { return item && item->is_processed(); });
    return WebsocketRun{.processed = processed, .clientThrew = client_threw.load()};
}

/**
 * The cascade behind a websocket client (entry ...Websocket): rank 0 runs a websocket server and a
 * client of it with two evaluations at a time, whose item processor evaluates every item in a
 * round of the group; afterwards the client ends and the lead releases its members. Without
 * @p confirm (entry ...WebsocketNotConfirmed) the lead never calls requireRole(): its first round,
 * inside the client's item processor, must end the job instead of flagging every item.
 *
 * @param confirm Whether the lead confirms its role
 */
int websocketScenario(Report &report, bool confirm) {
    constexpr std::size_t ITEMS = 30;
    ProbingMember member;
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() == processRole::BYSTANDER) {
        std::ignore = cascade.serve(member);
        if(not confirm) {
            std::println("member {} served {} rounds", report.rank(), member.rounds());
            return report.failures();
        }
        report.check(
            member.rounds() == static_cast<int>(ITEMS),
            std::format("the member served {} rounds", member.rounds())
        );
        std::println("member {} served {} rounds", report.rank(), member.rounds());
        return report.failures();
    }
    if(confirm) {
        cascade.requireRole(processRole::CLIENT);
    }
    const std::int64_t size = sizeOf(cascade.group());
    const std::int64_t factor = size * (size + 1) / 2; // every process adds its share
    std::atomic<int> led{0};
    std::atomic<int> wrong{0};

    const WebsocketRun run = runOverWebsocket(cascade, ITEMS, factor, led, wrong);
    if(not confirm) {
        report.fail(
            "the client ran to its end ({} of {} items processed) although the lead never "
            "confirmed its role",
            run.processed,
            ITEMS
        );
        return report.failures();
    }

    report.check(
        std::cmp_equal(run.processed, ITEMS),
        std::format("{} of {} items processed", run.processed, ITEMS)
    );
    report.check(wrong.load() == 0, std::format("{} wrong values", wrong.load()));
    report.check(
        led.load() == static_cast<int>(ITEMS),
        std::format("the lead led {} rounds", led.load())
    );
    const int led_at_release = led.load();
    report.expectThrow("round() after release()", "release()", [&] {
        std::ignore = cascade.round(NOTHING);
    });
    report.check(led.load() == led_at_release, "a round ran after release()");
    report.check(not run.clientThrew, "the client's run threw");
    std::println("lead led {} rounds", led.load());
    return report.failures();
}

/** The time of an empty round (entry ...EmptyRound): a number for a caption, not a gate. */
int emptyRoundScenario(Report &report) {
    constexpr std::size_t ROUNDS = 1'000;
    EmptyMember member;
    Courtier::GMPICascade cascade([] { return configOf(3, GMPICascadeLayout::GROUPS_ONLY); });
    if(cascade.role() == processRole::BYSTANDER) {
        std::ignore = cascade.serve(member);
        report.check(
            member.rounds() == static_cast<int>(ROUNDS),
            std::format("the member served {} rounds", member.rounds())
        );
        std::println("member {} served {} rounds", report.rank(), member.rounds());
        return report.failures();
    }
    cascade.requireRole(processRole::CLIENT);
    std::vector<double> times;
    times.reserve(ROUNDS);
    for(std::size_t r = 0; r < ROUNDS; ++r) {
        const auto start = std::chrono::steady_clock::now();
        cascade.round([](MPI_Comm) { /* nothing */ });
        times.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count()
        );
    }
    cascade.release();
    std::ranges::sort(times);
    std::println(
        "empty round: median {:.1f} us, 99th percentile {:.1f} us (group of {})",
        times[ROUNDS / 2],
        times[(ROUNDS * 99) / 100],
        sizeOf(cascade.group())
    );
    std::println("lead led {} rounds", ROUNDS);
    return report.failures();
}

/** @brief Runs the scenario @p name @return The number of failures */
int runScenario(Report &report, std::string_view name, int argc, char **argv) {
    using enum GMPICascadeLayout;
    using scenario_type = std::function<int(Report &)>;
    const std::vector<std::pair<std::string_view, scenario_type>> scenarios{
        {"layout-a", [](Report &r) { return layoutScenario(r, SUBMITTER_AND_GROUPS); }},
        {"layout-b", [](Report &r) { return layoutScenario(r, GROUPS_ONLY); }},
        {"one-a", [](Report &r) { return oneScenario(r, SUBMITTER_AND_GROUPS); }},
        {"one-b", [](Report &r) { return oneScenario(r, GROUPS_ONLY); }},
        {"rank-zero-size", rankZeroSizeScenario},
        {"lifecycle", lifecycleScenario},
        {"member-never-serves", memberNeverServesScenario},
        {"base-comm", baseCommScenario},
        {"two-threads", twoThreadsScenario},
        {"release-during-round", releaseDuringRoundScenario},
        {"refresh", [argc, argv](Report &r) { return refreshScenario(r, argc, argv); }},
        {"require-role-a", [](Report &r) { return requireRoleScenario(r, SUBMITTER_AND_GROUPS); }},
        {"require-role-b", [](Report &r) { return requireRoleScenario(r, GROUPS_ONLY); }},
        {"idle-member", idleMemberScenario},
        {"group-size-zero",
         [](Report &r) { return refusedConfigScenario(r, Refusal::GROUP_SIZE); }},
        {"pauses-refused", [](Report &r) { return refusedConfigScenario(r, Refusal::PAUSES); }},
        {"config-throws",
         [](Report &r) { return refusedConfigScenario(r, Refusal::RANK0_THROWS); }},
        {"config-file", configFileScenario},
        {"role-not-confirmed", [](Report &r) { return roleNotConfirmedScenario(r, false); }},
        {"role-not-confirmed-lead", [](Report &r) { return roleNotConfirmedScenario(r, true); }},
        {"never-used", neverUsedScenario},
        {"round-inside-set-up",
         [](Report &r) { return insideCallbackScenario(r, Reentry::ROUND_IN_SET_UP); }},
        {"release-inside-tear-down",
         [](Report &r) { return insideCallbackScenario(r, Reentry::RELEASE_IN_TEAR_DOWN); }},
        {"serve-inside-serve-round",
         [](Report &r) { return insideCallbackScenario(r, Reentry::SERVE_IN_SERVE_ROUND); }},
        {"submitter-never-confirms", submitterNeverConfirmsScenario},
        {"environment-exists", environmentExistsScenario},
        {"release-inside-round", [](Report &r) { return insideRoundScenario(r, true); }},
        {"round-inside-round", [](Report &r) { return insideRoundScenario(r, false); }},
        {"lead-after-round", [](Report &r) { return leadOrderScenario(r, false); }},
        {"lead-twice", [](Report &r) { return leadOrderScenario(r, true); }},
        {"ibcast-start-fails", [](Report &r) { return mpiErrorScenario(r, true); }},
        {"testall-fails", [](Report &r) { return mpiErrorScenario(r, false); }},
        {"serve-round-throws", [](Report &r) { return failureScenario(r, Injected::SERVE_ROUND); }},
        {"lead-half-throws", [](Report &r) { return failureScenario(r, Injected::LEAD_HALF); }},
        {"set-up-throws", [](Report &r) { return failureScenario(r, Injected::SET_UP); }},
        {"tear-down-throws", [](Report &r) { return failureScenario(r, Injected::TEAR_DOWN); }},
        {"websocket", [](Report &r) { return websocketScenario(r, true); }},
        {"websocket-not-confirmed", [](Report &r) { return websocketScenario(r, false); }},
        {"empty-round", emptyRoundScenario},
    };
    for(const auto &[scenario_name, scenario] : scenarios) {
        if(scenario_name == name) {
            return scenario(report);
        }
    }
    report.fail("unknown scenario '{}'", name);
    return report.failures();
}

} // namespace

/******************************************************************************/

int main(int argc, char **argv) {
    try {
        std::string scenario;
        constexpr std::string_view OPTION = "--scenario=";
        const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));
        for(const std::string_view argument : arguments.subspan(1)) {
            if(argument.starts_with(OPTION)) {
                scenario = argument.substr(OPTION.size());
            }
        }
        Report report(scenario);
        try {
            std::ignore = runScenario(report, scenario, argc, argv);
        }
        catch(const std::exception &e) {
            report.fail("an exception: {}", e.what());
        }
        catch(...) {
            report.fail("an exception that is not a std::exception");
        }
        if(report.failures() == 0) {
            report.ok("passed");
        }
        return report.failures() == 0 ? 0 : 1;
    }
    catch(...) {
        return 1; // the report itself failed: nothing can be said any more
    }
}
