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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/json.hpp>

#include "common/GExceptions.hpp"
#include "common/GJsonIO.hpp"
#include "common/GLogger.hpp"
#include "common/record/GCsvSink.hpp"
#include "common/record/GJsonLinesSink.hpp"
#include "common/record/GRecorder.hpp"
#include "common/record/GTextSink.hpp"

using namespace Gem::Common::Record;

namespace {

/******************************************************************************/
// A fresh directory per test case, set as the recorder's directory, and removed at its end
class ScratchDirectory {
public:
    explicit ScratchDirectory(std::string const &tag)
      : dir_(std::filesystem::temp_directory_path() / "gem_record_tests" /
             (tag + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        recorder().setDirectory(dir_);
    }
    ScratchDirectory(ScratchDirectory const &) = delete;
    ScratchDirectory &operator=(ScratchDirectory const &) = delete;
    ~ScratchDirectory() {
        recorder().closeAll();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    operator std::filesystem::path const &() const { return dir_; }
    std::filesystem::path operator/(std::string const &name) const { return dir_ / name; }

private:
    std::filesystem::path dir_;
};

// The lines of a file
std::vector<std::string> linesOf(std::filesystem::path const &file) {
    std::ifstream in(file);
    std::vector<std::string> lines;
    for(std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

// The fields of one CSV line (RFC 4180, without line breaks inside a field)
std::vector<std::string> csvFields(std::string const &line) {
    std::vector<std::string> fields(1);
    bool quoted = false;
    for(std::size_t i = 0; i < line.size(); ++i) {
        char const c = line[i];
        if(quoted) {
            if(c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                fields.back() += '"';
                ++i;
            }
            else if(c == '"') {
                quoted = false;
            }
            else {
                fields.back() += c;
            }
        }
        else if(c == '"') {
            quoted = true;
        }
        else if(c == ',') {
            fields.emplace_back();
        }
        else {
            fields.back() += c;
        }
    }
    return fields;
}

// The number of backups of a file in its directory
std::size_t backupsOf(std::filesystem::path const &file) {
    std::size_t n = 0;
    for(auto const &entry : std::filesystem::directory_iterator(file.parent_path())) {
        if(entry.path().filename().string().starts_with(file.filename().string() + ".bak_")) {
            ++n;
        }
    }
    return n;
}

// A sink that notes the calls it gets, and throws where it is told to: a std::exception, or
// something that is none
class ProbeSink final : public GSink {
public:
    enum class FailIn { NOWHERE, OPEN, CLOSE };
    explicit ProbeSink(FailIn fail = FailIn::NOWHERE, bool foreign = false)
      : fail_(fail)
      , foreign_(foreign) {}
    void open(GStreamInfo const &) override { this->note_("open", FailIn::OPEN); }
    void write(std::span<GRow const> rows) override {
        calls.push_back("write " + std::to_string(rows.size()));
    }
    void close() override { this->note_("close", FailIn::CLOSE); }

    std::vector<std::string> calls; ///< The calls, in their order

private:
    void note_(std::string const &call, FailIn here) {
        calls.push_back(call);
        if(fail_ == here) {
            if(foreign_) {
                throw 42; // NOLINT(hicpp-exception-baseclass): what the test is about
            }
            throw std::runtime_error("the probe fails in " + call);
        }
    }
    FailIn fail_;
    bool foreign_;
};

GTimeStamp parseTimeStamp(std::string const &text) {
    std::istringstream in(text);
    std::chrono::sys_time<std::chrono::nanoseconds> tp;
    in >> std::chrono::parse("%FT%TZ", tp);
    REQUIRE(not in.fail());
    return std::chrono::time_point_cast<GTimeStamp::duration>(tp);
}

} // namespace

/******************************************************************************/

TEST_CASE("every column type goes through both sinks and reads back unchanged", "[common][record]") {
    ScratchDirectory const dir("roundtrip");
    const GTimeStamp stamp = std::chrono::system_clock::now();
    const std::string text = "a \"quoted\", text\twith tab";
    const double value = 0.1 + 0.2; // no short decimal form
    {
        auto csv = recorder().stream<std::int32_t, std::uint64_t, double, bool, std::string, GTimeStamp>(
            "roundtrip", {"i", "u", "d", "b", "s", "t"}
        );
        auto json = recorder().stream<std::int32_t, std::uint64_t, double, bool, std::string, GTimeStamp>(
            "roundtrip_json",
            {"i", "u", "d", "b", "s", "t"},
            withTime::none,
            GStreamOptions{.sinks = {std::make_shared<GJsonLinesSink>()}}
        );
        csv.append(-7, std::numeric_limits<std::uint64_t>::max(), value, true, text, stamp);
        json.append(-7, std::numeric_limits<std::uint64_t>::max(), value, true, text, stamp);
        csv.append(0, 0, std::numeric_limits<double>::infinity(), false, "", stamp);
    }

    auto const csv_lines = linesOf(dir / "roundtrip.csv");
    REQUIRE(csv_lines.size() == 3);
    CHECK(csv_lines[0] == "i,u,d,b,s,t");
    auto const fields = csvFields(csv_lines[1]);
    REQUIRE(fields.size() == 6);
    CHECK(std::stoll(fields[0]) == -7);
    CHECK(std::stoull(fields[1]) == std::numeric_limits<std::uint64_t>::max());
    CHECK(std::stod(fields[2]) == value);
    CHECK(fields[3] == "true");
    CHECK(fields[4] == text);
    CHECK(parseTimeStamp(fields[5]) == stamp);
    auto const second = csvFields(csv_lines[2]);
    CHECK(second[2] == "inf");
    CHECK(second[3] == "false");
    CHECK(second[4].empty());

    auto const json_lines = linesOf(dir / "roundtrip_json.jsonl");
    REQUIRE(json_lines.size() == 1);
    auto const jv = Gem::Common::parseJsonString(json_lines[0]);
    auto const &object = jv.as_object();
    CHECK(object.at("i").as_int64() == -7);
    CHECK(object.at("u").as_uint64() == std::numeric_limits<std::uint64_t>::max());
    CHECK(object.at("d").as_double() == value);
    CHECK(object.at("b").as_bool());
    CHECK(object.at("s").as_string() == text);
    CHECK(parseTimeStamp(std::string(object.at("t").as_string())) == stamp);
    // The members come in column order
    std::vector<std::string> keys;
    for(auto const &member : object) {
        keys.emplace_back(member.key());
    }
    CHECK(keys == std::vector<std::string>{"i", "u", "d", "b", "s", "t"});
}

TEST_CASE("a value that is not finite is null in JSON Lines", "[common][record]") {
    ScratchDirectory const dir("nan");
    {
        auto s = recorder().stream<double>(
            "nan", {"d"}, withTime::none, GStreamOptions{.sinks = {std::make_shared<GJsonLinesSink>()}}
        );
        s.append(std::nan(""));
    }
    CHECK(linesOf(dir / "nan.jsonl") == std::vector<std::string>{R"({"d":null})"});
}

TEST_CASE("a stream with columns given at run time checks each row", "[common][record]") {
    ScratchDirectory const dir("runtime");
    auto s = recorder().stream(
        "runtime",
        {GColumn{.name = "x", .type = columnType::FLOATING}, GColumn{.name = "n", .type = columnType::UNSIGNED}}
    );
    s.append(GRow{1.5, std::uint64_t{3}});
    CHECK_THROWS_AS(s.append(GRow{1.5}), gem_exception);
    CHECK_THROWS_AS(s.append(GRow{std::uint64_t{3}, 1.5}), gem_exception);
    s.close();
    CHECK(linesOf(dir / "runtime.csv") == std::vector<std::string>{"x,n", "1.5,3"});
    CHECK_THROWS_AS(s.append(GRow{1.5, std::uint64_t{3}}), gem_exception);
}

TEST_CASE("the time column comes first: of the wall or the steady clock", "[common][record]") {
    ScratchDirectory const dir("time");
    {
        auto wall = recorder().stream<std::string, double>("quotes", {"symbol", "price"}, withTime::wall);
        auto steady = recorder().stream<int>("ticks", {"n"}, withTime::steady);
        CHECK(wall.isOpen());
        const auto before = std::chrono::system_clock::now();
        wall.append("ACME", 17.25);
        steady.append(1);
        steady.append(2);
        auto const wall_lines = linesOf(dir / "quotes.csv");
        REQUIRE(wall_lines.size() == 2);
        CHECK(wall_lines[0] == "time,symbol,price");
        auto const fields = csvFields(wall_lines[1]);
        CHECK(parseTimeStamp(fields[0]) >= before);
        CHECK(fields[1] == "ACME");
    }
    auto const lines = linesOf(dir / "ticks.csv");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "time,n");
    CHECK(std::stod(csvFields(lines[1])[0]) <= std::stod(csvFields(lines[2])[0]));
    CHECK_THROWS_AS(
        recorder().stream<double>("clash", {"time"}, withTime::steady), gem_exception
    );
}

TEST_CASE("a second open stream of a name is refused and both declarers are named", "[common][record]") {
    ScratchDirectory const dir("duplicate");
    auto first = recorder().stream<int>("twice", {"n"});
    try {
        auto second = recorder().stream<int>("twice", {"n"});
        FAIL("the second declaration was taken");
    }
    catch(gem_exception const &e) {
        const std::string what = e.what();
        CHECK_THAT(what, Catch::Matchers::ContainsSubstring("GRecorderTests.cpp", Catch::CaseSensitive::No));
        CHECK(what.find("GRecorderTests.cpp") != what.rfind("GRecorderTests.cpp")); // named twice
        CHECK_THAT(what, Catch::Matchers::ContainsSubstring("two objects of one class"));
    }
    // Free again once closed
    first.close();
    auto again = recorder().stream<int>("twice", {"n"});
    CHECK(again.isOpen());
    CHECK_THROWS_AS(recorder().stream<int>("a/b", {"n"}), gem_exception);
    CHECK_THROWS_AS((recorder().stream<int, int>("dup", {"n", "n"})), gem_exception);
}

TEST_CASE("an existing file is backed up at the first write and not before", "[common][record]") {
    ScratchDirectory const dir("backup");
    std::filesystem::create_directories(dir / "");
    {
        std::ofstream(dir / "kept.csv") << "older run\n";
    }
    auto s = recorder().stream<int>("kept", {"n"});
    CHECK(backupsOf(dir / "kept.csv") == 0);
    CHECK(linesOf(dir / "kept.csv") == std::vector<std::string>{"older run"});
    s.append(1);
    CHECK(backupsOf(dir / "kept.csv") == 1);
    CHECK(linesOf(dir / "kept.csv") == std::vector<std::string>{"n", "1"});
}

TEST_CASE("a resuming stream appends when its columns match and else starts anew", "[common][record]") {
    ScratchDirectory const dir("resume");
    for(bool const json : {false, true}) {
        auto const sinks = [json]() {
            return json ? std::vector<std::shared_ptr<GSink>>{std::make_shared<GJsonLinesSink>()}
                        : std::vector<std::shared_ptr<GSink>>{std::make_shared<GCsvSink>()};
        };
        auto const file = dir / (json ? "run.jsonl" : "run.csv");
        {
            auto s = recorder().stream<int>("run", {"n"}, withTime::none, GStreamOptions{.sinks = sinks()});
            s.append(1);
        }
        {
            auto s = recorder().stream<int>(
                "run", {"n"}, withTime::none, GStreamOptions{.sinks = sinks(), .resume = true}
            );
            s.append(2);
        }
        CHECK(backupsOf(file) == 0);
        CHECK(linesOf(file).size() == (json ? 2 : 3));
        {
            auto s = recorder().stream<int, int>(
                "run", {"n", "m"}, withTime::none, GStreamOptions{.sinks = sinks(), .resume = true}
            );
            s.append(3, 4);
        }
        CHECK(backupsOf(file) == 1);
        CHECK(linesOf(file).size() == (json ? 1 : 2));
    }
}

TEST_CASE("a resuming stream first cuts its file back to the checkpoint", "[common][record]") {
    // The earlier run wrote the rows of n = 0..4 and was cut in the middle of a sixth; the resumed
    // run starts after the checkpoint of n = 2. Its stream drops the rows after it, and the broken
    // line, before it appends -- in a CSV and in a JSON Lines file.
    ScratchDirectory const dir("cut");
    for(bool const json : {false, true}) {
        auto const sinks = [json]() {
            return json ? std::vector<std::shared_ptr<GSink>>{std::make_shared<GJsonLinesSink>()}
                        : std::vector<std::shared_ptr<GSink>>{std::make_shared<GCsvSink>()};
        };
        auto const file = dir / (json ? "cut.jsonl" : "cut.csv");
        {
            auto s = recorder().stream<std::uint64_t, std::string>(
                "cut", {"n", "note"}, withTime::none, GStreamOptions{.sinks = sinks()}
            );
            for(std::uint64_t n = 0; n < 5; ++n) {
                s.append(n, "earlier");
            }
        }
        {
            std::ofstream(file, std::ios::app) << (json ? R"({"n":5,"no)" : "5,earl");
        }
        {
            auto s = recorder().stream<std::uint64_t, std::string>(
                "cut",
                {"n", "note"},
                withTime::none,
                GStreamOptions{
                    .sinks = sinks(), .resume = true, .resumeColumn = "n", .resumeAfter = 2.
                }
            );
            s.append(3, "resumed");
        }
        CHECK(backupsOf(file) == 0);
        const auto lines = linesOf(file);
        if(json) {
            CHECK(
                lines ==
                std::vector<std::string>{
                    R"({"n":0,"note":"earlier"})",
                    R"({"n":1,"note":"earlier"})",
                    R"({"n":2,"note":"earlier"})",
                    R"({"n":3,"note":"resumed"})"
                }
            );
        }
        else {
            CHECK(
                lines ==
                std::vector<std::string>{"n,note", "0,earlier", "1,earlier", "2,earlier", "3,resumed"}
            );
        }
    }
    // The column by which a file is cut back must be a numeric column of the stream
    CHECK_THROWS_AS(
        (recorder().stream<std::string>(
            "cut_text", {"note"}, withTime::none, GStreamOptions{.resume = true, .resumeColumn = "note"}
        )),
        gem_exception
    );
    CHECK_THROWS_AS(
        (recorder().stream<int>(
            "cut_none", {"n"}, withTime::none, GStreamOptions{.resume = true, .resumeColumn = "m"}
        )),
        gem_exception
    );
}

TEST_CASE("a resuming stream without a resumeColumn removes a last row cut by a crash", "[common][record]") {
    // A row without its final line break is never data: the resumed stream drops it before it
    // appends, also where the cut lies in a CSV text of several lines, and keeps every whole row
    ScratchDirectory const dir("torn");
    struct Case {
        std::string name;
        bool json;
        std::string torn;
    };
    for(Case const &c :
        {Case{"torn_csv", false, "2,earl"},
         Case{"torn_text", false, "2,\"one line\n"},
         Case{"torn_json", true, R"({"n":2,"no)"}}) {
        auto const sinks = [&c]() {
            return c.json ? std::vector<std::shared_ptr<GSink>>{std::make_shared<GJsonLinesSink>()}
                          : std::vector<std::shared_ptr<GSink>>{std::make_shared<GCsvSink>()};
        };
        auto const file = dir / (c.name + (c.json ? ".jsonl" : ".csv"));
        {
            auto s = recorder().stream<std::uint64_t, std::string>(
                c.name, {"n", "note"}, withTime::none, GStreamOptions{.sinks = sinks()}
            );
            s.append(0, "earlier");
            s.append(1, "two\nlines");
        }
        {
            std::ofstream(file, std::ios::app) << c.torn;
        }
        {
            auto s = recorder().stream<std::uint64_t, std::string>(
                c.name, {"n", "note"}, withTime::none, GStreamOptions{.sinks = sinks(), .resume = true}
            );
            s.append(3, "resumed");
        }
        CHECK(backupsOf(file) == 0);
        if(c.json) {
            CHECK(
                linesOf(file) ==
                std::vector<std::string>{
                    R"({"n":0,"note":"earlier"})", R"({"n":1,"note":"two\nlines"})", R"({"n":3,"note":"resumed"})"
                }
            );
        }
        else {
            CHECK(
                GCsvSink::readRows(
                    file,
                    {GColumn{.name = "n", .type = columnType::UNSIGNED},
                     GColumn{.name = "note", .type = columnType::TEXT}}
                ) == std::vector<std::vector<std::string>>{{"0", "earlier"}, {"1", "two\nlines"}, {"3", "resumed"}}
            );
        }
    }
}

TEST_CASE("a CSV row whose text spans several lines survives a resume and reads back whole", "[common][record]") {
    // A text with a line break is written in quotes over several lines of the file (RFC 4180).
    // Cutting the file back and readRows() read such a row as one record, not line by line.
    ScratchDirectory const dir("multiline");
    const std::vector<GColumn> columns{
        GColumn{.name = "n", .type = columnType::UNSIGNED},
        GColumn{.name = "note, \"quoted\"", .type = columnType::TEXT}
    };
    const std::string tricky = "he said \"x, y\"\r\nand left\n";
    auto const options = [](bool resume) {
        return GStreamOptions{.resume = resume, .resumeColumn = resume ? "n" : "", .resumeAfter = 2.};
    };
    {
        auto s = recorder().stream("multiline", columns, withTime::none, options(false));
        s.append(GRow{std::uint64_t{0}, std::string("a\nb")});
        s.append(GRow{std::uint64_t{1}, tricky});
        s.append(GRow{std::uint64_t{2}, std::string("plain")});
        s.append(GRow{std::uint64_t{3}, std::string("after\nthe checkpoint")});
    }
    {
        auto s = recorder().stream("multiline", columns, withTime::none, options(true));
        s.append(GRow{std::uint64_t{3}, std::string("resumed")});
    }
    CHECK(backupsOf(dir / "multiline.csv") == 0);
    auto const rows = GCsvSink::readRows(dir / "multiline.csv", columns);
    REQUIRE(rows);
    CHECK(
        *rows == std::vector<std::vector<std::string>>{
                     {"0", "a\nb"}, {"1", tricky}, {"2", "plain"}, {"3", "resumed"}
                 }
    );

    // A column name with a line break makes a header of several lines: still the stream's own
    const std::vector<GColumn> named{GColumn{.name = "two\nlines", .type = columnType::INTEGER}};
    for(bool const resume : {false, true}) {
        auto s = recorder().stream("header", named, withTime::none, GStreamOptions{.resume = resume});
        s.append(GRow{std::int64_t{resume ? 2 : 1}});
    }
    CHECK(backupsOf(dir / "header.csv") == 0);
    CHECK(
        GCsvSink::readRows(dir / "header.csv", named) ==
        std::vector<std::vector<std::string>>{{"1"}, {"2"}}
    );
}

TEST_CASE("GCsvSink::readRows() returns the complete rows of a file written for its columns", "[common][record]") {
    ScratchDirectory const dir("readrows");
    std::filesystem::create_directories(dir / "");
    const std::vector<GColumn> columns{
        GColumn{.name = "n", .type = columnType::INTEGER}, GColumn{.name = "note", .type = columnType::TEXT}
    };
    {
        // A row with a missing field and a last row cut by a crash -- with the right number of
        // fields, but without its line break -- are left out
        std::ofstream(dir / "rows.csv") << "n,note\n1,one\n2\n3,\"a, b\"\"c\"\"\nd\"\n4,cu";
    }
    auto const rows = GCsvSink::readRows(dir / "rows.csv", columns);
    REQUIRE(rows);
    CHECK(*rows == std::vector<std::vector<std::string>>{{"1", "one"}, {"3", "a, b\"c\"\nd"}});

    // No such file, or one written for other columns: none
    CHECK_FALSE(GCsvSink::readRows(dir / "missing.csv", columns));
    {
        std::ofstream(dir / "other.csv") << "n,other\n1,one\n";
    }
    CHECK_FALSE(GCsvSink::readRows(dir / "other.csv", columns));
    // A file of the header alone has no rows
    {
        std::ofstream(dir / "empty.csv") << "n,note\n";
    }
    auto const none = GCsvSink::readRows(dir / "empty.csv", columns);
    REQUIRE(none);
    CHECK(none->empty());
}

TEST_CASE("a stream whose sink fails to open takes no further row and closes only the sinks it opened", "[common][record]") {
    // The GSink contract: write() and close() only after open(); close() also when open() threw
    ScratchDirectory const dir("failopen");
    auto const first = std::make_shared<ProbeSink>();
    auto const failing = std::make_shared<ProbeSink>(ProbeSink::FailIn::OPEN);
    auto const third = std::make_shared<ProbeSink>();
    auto s = recorder().stream(
        "failopen",
        {GColumn{.name = "n", .type = columnType::INTEGER}},
        withTime::none,
        GStreamOptions{.sinks = {first, failing, third}}
    );
    CHECK_THROWS_AS(s.append(GRow{std::int64_t{1}}), std::runtime_error);
    try {
        s.append(GRow{std::int64_t{2}});
        FAIL("a stream whose sink failed to open took a row");
    }
    catch(gem_exception const &e) {
        CHECK_THAT(e.what(), Catch::Matchers::ContainsSubstring("failed to open"));
    }
    CHECK_THROWS_AS(s.close(), gem_exception); // the rows held back reach no sink
    CHECK(first->calls == std::vector<std::string>{"open", "close"});
    CHECK(failing->calls == std::vector<std::string>{"open", "close"});
    CHECK(third->calls.empty());
}

TEST_CASE("a sink that throws no std::exception is reported by the handle and by closeAll()", "[common][record]") {
    // The handle's end and its move assignment do not throw, and closeAll() reports through the log
    ScratchDirectory const dir("foreign_exception");
    std::vector<GColumn> const columns{GColumn{.name = "n", .type = columnType::INTEGER}};
    auto const declare = [&columns](std::string const &name) {
        auto s = recorder().stream(
            name,
            columns,
            withTime::none,
            GStreamOptions{.sinks = {std::make_shared<ProbeSink>(ProbeSink::FailIn::CLOSE, true)}}
        );
        s.append(GRow{std::int64_t{1}});
        return s;
    };
    auto by_close_all = declare("by_close_all");
    CHECK_NOTHROW(recorder().closeAll());
    CHECK_FALSE(by_close_all.isOpen());
    auto by_close = declare("by_close");
    CHECK_THROWS_AS(by_close.close(), int); // close() itself passes it on
    auto moved_onto = declare("by_move");
    moved_onto = GStream();
    CHECK_FALSE(moved_onto.isOpen());
    {
        auto s = declare("by_destructor");
    }
}

TEST_CASE("a stream whose close fails ends no program when its report fails as well", "[common][record]") {
    // Regression: the report of a failed close went to the log from a function that does not throw
    // (the handle's move assignment and destructor); a log target that threw ended the program
    // through std::terminate()
    struct ThrowingTarget final : Gem::Common::GBaseLogTarget {
        void log(std::string const &) const override { throw std::runtime_error("the log fails"); }
        void logWithSource(std::string const &, std::string const &) const override {
            throw std::runtime_error("the log fails");
        }
    };
    ScratchDirectory const dir("failing_log");
    std::vector<GColumn> const columns{GColumn{.name = "n", .type = columnType::INTEGER}};
    auto const declare = [&columns](std::string const &name) {
        auto s = recorder().stream(
            name,
            columns,
            withTime::none,
            GStreamOptions{.sinks = {std::make_shared<ProbeSink>(ProbeSink::FailIn::CLOSE)}}
        );
        s.append(GRow{std::int64_t{1}});
        return s;
    };
    auto moved_onto = declare("failing_log_move");
    auto destroyed = std::make_unique<GStream>(declare("failing_log_destructor"));
    glogger.addLogTarget(std::make_shared<ThrowingTarget>());
    moved_onto = GStream();
    destroyed.reset();
    glogger.resetLogTargets();
    CHECK_FALSE(moved_onto.isOpen());
}

TEST_CASE("a handle moved onto an open stream closes that stream and takes over the other", "[common][record]") {
    ScratchDirectory const dir("move");
    auto target = recorder().stream<int>("target", {"n"});
    auto source = recorder().stream<int>("source", {"n"});
    target.append(1);
    source.append(2);
    target = std::move(source);
    CHECK(target.name() == "source");
    CHECK(target.isOpen());
    CHECK_FALSE(source.isOpen()); // NOLINT(bugprone-use-after-move): a moved-from handle is empty
    CHECK(source.name().empty());  // NOLINT(bugprone-use-after-move)
    CHECK(linesOf(dir / "target.csv") == std::vector<std::string>{"n", "1"});
    auto again = recorder().stream<int>("target", {"n"}); // its name is free again
    CHECK(again.isOpen());
    target.append(3);
    target.close();
    CHECK(linesOf(dir / "source.csv") == std::vector<std::string>{"n", "2", "3"});
}

TEST_CASE("a sink added by name joins the next declaration of that stream", "[common][record]") {
    // A program adds a sink to a stream that another part of it declares later: the sink joins the
    // declarer's sinks (here the default CSV file), for that declaration only.
    ScratchDirectory const dir("added");
    recorder().addSink("foreign", std::make_shared<GJsonLinesSink>());
    {
        auto s = recorder().stream<int>("foreign", {"n"});
        s.append(1);
    }
    CHECK(linesOf(dir / "foreign.csv") == std::vector<std::string>{"n", "1"});
    CHECK(linesOf(dir / "foreign.jsonl") == std::vector<std::string>{R"({"n":1})"});
    {
        auto s = recorder().stream<int>("foreign", {"n"});
        s.append(2);
    }
    CHECK(linesOf(dir / "foreign.jsonl") == std::vector<std::string>{R"({"n":1})"}); // not again

    // A stream that is open already is refused: the sink would miss its earlier rows
    auto open = recorder().stream<int>("open_already", {"n"});
    try {
        recorder().addSink("open_already", std::make_shared<GJsonLinesSink>());
        FAIL("a sink was added to an open stream");
    }
    catch(gem_exception const &e) {
        CHECK_THAT(e.what(), Catch::Matchers::ContainsSubstring("is open already"));
    }
    CHECK_THROWS_AS(recorder().addSink("nothing", nullptr), gem_exception);
}

TEST_CASE("rows held back reach the file when the stream closes", "[common][record]") {
    ScratchDirectory const dir("flush");
    auto s = recorder().stream<int>("held", {"n"}, withTime::none, GStreamOptions{.flushEvery = 100});
    s.append(1);
    s.append(2);
    CHECK(not std::filesystem::exists(dir / "held.csv"));
    recorder().closeAll();
    CHECK(not s.isOpen());
    CHECK(linesOf(dir / "held.csv") == std::vector<std::string>{"n", "1", "2"});
}

TEST_CASE("rows appended from several threads are whole and keep each thread's order", "[common][record]") {
    ScratchDirectory const dir("threads");
    constexpr int N_THREADS = 8;
    constexpr int N_ROWS = 500;
    {
        auto s = recorder().stream<int, int, std::string>(
            "threads", {"thread", "row", "text"}, withTime::none, GStreamOptions{.flushEvery = 7}
        );
        std::vector<std::jthread> threads;
        for(int t = 0; t < N_THREADS; ++t) {
            threads.emplace_back([&s, t] {
                for(int r = 0; r < N_ROWS; ++r) {
                    s.append(t, r, std::string(static_cast<std::size_t>(t + 1) * 10, 'x'));
                }
            });
        }
    }
    auto const lines = linesOf(dir / "threads.csv");
    REQUIRE(lines.size() == 1 + N_THREADS * N_ROWS);
    std::map<int, int> next;
    for(std::size_t i = 1; i < lines.size(); ++i) {
        auto const fields = csvFields(lines[i]);
        REQUIRE(fields.size() == 3);
        int const t = std::stoi(fields[0]);
        CHECK(std::stoi(fields[1]) == next[t]++);
        CHECK(fields[2] == std::string(static_cast<std::size_t>(t + 1) * 10, 'x'));
    }
}

TEST_CASE("the directory is ./records until it is set", "[common][record]") {
    // Other test cases have set it; this checks the documented default of a fresh recorder
    GRecorder fresh;
    CHECK(fresh.directory() == std::filesystem::path(".") / "records");
}

/******************************************************************************/
TEST_CASE("GTextSink: one row per line with the values separated by a space", "[common][record]") {
    ScratchDirectory const dir("text");
    {
        auto s = recorder().stream<std::uint64_t, double, bool, std::string>(
            "plain", {"n", "x", "ok", "note"}, withTime::none,
            GStreamOptions{.sinks = {std::make_shared<GTextSink>()}});
        s.append(7, 2.5, true, "some text\n");
        s.append(8, 0.5, false, "");
    }
    CHECK(linesOf(dir / "plain.log") == std::vector<std::string>{"7 2.5 true some text", "8 0.5 false"});
    CHECK_FALSE(std::filesystem::exists(dir / "plain.csv")); // the sinks given replace the default
}
TEST_CASE("a handle tells its name and columns and an empty one has none", "[common][record]") {
    ScratchDirectory const dir("handle");
    auto s = recorder().stream(
        "described", {GColumn{.name = "x", .type = columnType::FLOATING}}, withTime::wall
    );
    CHECK(s.name() == "described");
    CHECK(
        s.columns() ==
        std::vector<GColumn>{
            GColumn{.name = "time", .type = columnType::TIMESTAMP},
            GColumn{.name = "x", .type = columnType::FLOATING}
        }
    );
    auto typed = recorder().stream<int>("typed", {"n"});
    CHECK(typed.name() == "typed");

    GStream empty;
    CHECK_FALSE(empty.isOpen());
    CHECK(empty.name().empty());
    CHECK(empty.columns().empty());
    CHECK_THROWS_AS(empty.append(GRow{1.}), gem_exception);
    CHECK_NOTHROW(empty.close());
    GStreamT<int> empty_typed;
    CHECK_FALSE(empty_typed.isOpen());
    CHECK(empty_typed.name().empty());
    CHECK_THROWS_AS(empty_typed.append(1), gem_exception);
}

TEST_CASE("a file sink names its file by the output rule and knows it once open", "[common][record]") {
    ScratchDirectory const dir("paths");
    auto const csv = std::make_shared<GCsvSink>();
    GStreamInfo const info{.name = "where", .directory = dir};
    CHECK(csv->fileFor(info) == dir / "where.csv");
    CHECK(GJsonLinesSink().fileFor(info) == dir / "where.jsonl");
    CHECK(GTextSink().fileFor(info) == dir / "where.log");
    {
        auto s = recorder().stream<int>("where", {"n"}, withTime::none, GStreamOptions{.sinks = {csv}});
        CHECK(csv->path().empty()); // opened at the first write
        s.append(1);
        CHECK(csv->path() == dir / "where.csv");
    }
}

TEST_CASE("the types of the values: typeOf() columnTypeOf() and toValue()", "[common][record]") {
    static_assert(columnTypeOf<bool>() == columnType::BOOLEAN);
    static_assert(columnTypeOf<std::int8_t>() == columnType::INTEGER);
    static_assert(columnTypeOf<unsigned short>() == columnType::UNSIGNED);
    static_assert(columnTypeOf<float>() == columnType::FLOATING);
    static_assert(columnTypeOf<char const *>() == columnType::TEXT);
    static_assert(columnTypeOf<std::string const &>() == columnType::TEXT);
    static_assert(columnTypeOf<GTimeStamp>() == columnType::TIMESTAMP);

    CHECK(toValue(std::int16_t{-3}) == GValue(std::int64_t{-3}));
    CHECK(toValue(std::uint8_t{200}) == GValue(std::uint64_t{200}));
    CHECK(toValue(0.5F) == GValue(0.5));
    CHECK(toValue(true) == GValue(true));
    CHECK(toValue(std::string_view("text")) == GValue(std::string("text")));
    const GTimeStamp stamp = std::chrono::system_clock::now();
    CHECK(toValue(stamp) == GValue(stamp));
    CHECK(typeOf(toValue(std::int16_t{-3})) == columnType::INTEGER);
    CHECK(typeOf(GValue(std::uint64_t{1})) == columnType::UNSIGNED);
    CHECK(typeOf(GValue(std::string())) == columnType::TEXT);
    CHECK(typeOf(GValue(stamp)) == columnType::TIMESTAMP);

    CHECK(GColumn{.name = "a", .type = columnType::TEXT} == GColumn{.name = "a", .type = columnType::TEXT});
    CHECK_FALSE(GColumn{.name = "a", .type = columnType::TEXT} == GColumn{.name = "a", .type = columnType::BOOLEAN});
    CHECK_FALSE(GColumn{.name = "a", .type = columnType::TEXT} == GColumn{.name = "b", .type = columnType::TEXT});
    CHECK(Gem::Common::enumName(columnType::TIMESTAMP) == "timestamp");
}
