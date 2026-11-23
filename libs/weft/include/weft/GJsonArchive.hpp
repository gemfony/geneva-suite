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

// Standard headers go here
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

// Boost headers go here
#include <boost/json.hpp>

// Geneva headers go here
#include "weft/GArchive.hpp"
#include "weft/GWeftError.hpp"

namespace Gem::Weft {

/******************************************************************************/
/**
 * @file GJsonArchive.hpp
 * @brief The human-readable JSON codec of the @c GArchive family (the
 * inspectable checkpoint format), built on @c boost::json (already a
 * dependency).
 *
 * @par Format
 * A serializable class becomes a JSON object keyed by member name; a container
 * becomes a JSON array; @c pair and @c map entries become @c {"first","second"}
 * / @c {"key","value"} objects. Floating-point values are stored as exact,
 * re-parseable strings rather than JSON numbers: @c float / @c double as their
 * shortest round-trip-exact decimal, and @c long @c double as an exact hexadecimal
 * float (e.g. @c "a.aaaaaaaaaaaaaabp-5", without a @c 0x prefix). Both are written
 * with @c std::to_chars and read with @c std::from_chars, which do not follow the
 * C locale: an archive written after a program set a decimal-comma @c LC_NUMERIC
 * reads anywhere. Strings are used because boost::json's number serializer is not
 * round-trip-exact for every double, and because a JSON number can hold neither a
 * @c long @c double nor a NaN/Inf; the decimal form stays human-readable.
 *
 * @par Tree assembly (save)
 * Values are built bottom-up on an owned-node stack: @c begin_object /
 * @c begin_seq push an empty node, members/elements are attached into the top
 * node, and @c end_object / @c end_seq pop the completed node and move it into
 * its parent. Nothing holds a pointer into a @c boost::json container across an
 * insertion, so container reallocation cannot dangle. Each stack frame captures
 * the member key in force when it was opened, so a nested node is attached under
 * the right key even though inner @c member() calls overwrite the pending key.
 *
 * @par Navigation (load)
 * The loader walks the parsed tree in the same fixed member order: object
 * members are looked up by name (order-tolerant), array elements by an advancing
 * per-level cursor.
 *
 * @par A malformed archive
 * The loading codec reports every flaw of the document as @c weft_exception, with
 * the path of the value in the tree (e.g. @c /point/x, @c /values/3) and the reason:
 * text that is no JSON, a value of the wrong type, a missing key, an array of the
 * wrong length (a fixed-size array, a tuple or a raw range must have exactly its
 * elements), a number out of the range of its type, a floating-point string that is
 * no number, a malformed hex string. No exception of Boost.JSON or of the standard
 * library leaves the codec.
 */
/******************************************************************************/

/**
 * @brief The saving (output) JSON codec. Assembles a @c boost::json::value,
 * retrievable as a tree (@ref value) or serialized text (@ref str).
 */
class GJsonOArchive : public GOArchiveT<GJsonOArchive> {
public:
    GJsonOArchive() = default;

    /** @brief The assembled JSON tree. */
    [[nodiscard]] const boost::json::value &value() const {
        return root_;
    }
    /** @brief The assembled tree serialized to compact JSON text. */
    [[nodiscard]] std::string str() const {
        return boost::json::serialize(root_);
    }

    // --- primitive hooks --------------------------------------------------

    /** @brief Stores a boolean. @param b The value. */
    void put_bool(bool b) {
        place(boost::json::value(b));
    }

    /** @brief Stores an integer as a signed or unsigned JSON number by its signedness. @param v The value. */
    template <typename int_type>
    void put_scalar(int_type v) {
        static_assert(std::is_integral_v<int_type>);
        if constexpr(std::is_signed_v<int_type>) {
            place(boost::json::value(static_cast<std::int64_t>(v)));
        }
        else {
            place(boost::json::value(static_cast<std::uint64_t>(v)));
        }
    }

    /**
     * @brief Stores a floating-point value as an exact, re-parseable string.
     *
     * All three widths are stored as strings rather than JSON numbers: boost::json's
     * number serializer is not round-trip-exact for every @c double (it can emit a
     * shortest form that re-parses to an adjacent value, losing a ULP), and a JSON
     * number cannot hold a @c long @c double or a NaN/Inf at all. @c float / @c double
     * use the shortest round-trip-exact decimal, which stays human-readable;
     * @c long @c double uses an exact hexadecimal float. Both are locale-independent
     * (@c std::to_chars). @param v The value.
     */
    template <typename float_type>
    void put_fp(float_type v) {
        static_assert(std::is_floating_point_v<float_type>);
        place(boost::json::value(fp_to_string(v)));
    }

    /** @brief Stores a string. @param s The string. */
    void put_string(std::string_view s) {
        place(boost::json::value(s));
    }

    // --- framing hooks ----------------------------------------------------

    void begin_object() {
        open(boost::json::value(boost::json::object{}));
    }
    void end_object() {
        close();
    }
    /** @brief Records the key the next attached value/node uses inside an object. @param name The member name. */
    void member(const char *name) {
        pending_key_ = name;
    }
    void begin_seq(std::size_t /*n*/) {
        open(boost::json::value(boost::json::array{}));
    }
    void end_seq() {
        close();
    }
    void begin_elem() {
    }
    void end_elem() {
    }

    // Raw caller-managed range framing: an array of elements (make_array) or a
    // hex string (make_binary). The count is implicit in the JSON structure.
    void begin_raw(std::size_t /*n*/) {
        open(boost::json::value(boost::json::array{}));
    }
    void end_raw() {
        close();
    }
    /** @brief Stores @p n opaque bytes as a lowercase hex string. @param p The block. @param n The byte count. */
    void put_bytes(const void *p, std::size_t n) {
        place(boost::json::value(to_hex(p, n)));
    }

private:
    static std::string to_hex(const void *p, std::size_t n) {
        static constexpr char digits[] = "0123456789abcdef";
        const auto *b = static_cast<const unsigned char *>(p);
        std::string out;
        out.resize(2 * n);
        for(std::size_t i = 0; i < n; ++i) {
            out[2 * i] = digits[b[i] >> 4];
            out[2 * i + 1] = digits[b[i] & 0x0F];
        }
        return out;
    }

    struct Frame {
        boost::json::value node;
        std::string key; // the key this node occupies in its parent (if the parent is an object)
    };

    void place(boost::json::value v) {
        if(stack_.empty()) {
            root_ = std::move(v);
        }
        else {
            attach(stack_.back().node, std::move(v), pending_key_);
        }
    }

    void open(boost::json::value container) {
        stack_.push_back(Frame{std::move(container), pending_key_});
    }

    void close() {
        Frame f = std::move(stack_.back());
        stack_.pop_back();
        if(stack_.empty()) {
            root_ = std::move(f.node);
        }
        else {
            attach(stack_.back().node, std::move(f.node), f.key);
        }
    }

    static void attach(boost::json::value &parent, boost::json::value v, const std::string &key) {
        if(parent.is_object()) {
            parent.as_object()[key] = std::move(v);
        }
        else {
            parent.as_array().push_back(std::move(v));
        }
    }

    // Formats a floating-point value as an exact, re-parseable decimal (float/double:
    // the shortest form that reads back to the same value) or hex-float (long double)
    // string, independent of the locale. 64 characters hold every value of the three types.
    template <typename float_type>
    static std::string fp_to_string(float_type v) {
        std::array<char, 64> buf{};
        std::to_chars_result r{};
        if constexpr(std::is_same_v<float_type, long double>) {
            r = std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::hex);
        }
        else {
            r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
        }
        return std::string(buf.data(), r.ptr);
    }

    boost::json::value root_;
    std::string pending_key_;
    std::vector<Frame> stack_;
};

/******************************************************************************/
/**
 * @brief The loading (input) JSON codec. Reads from parsed JSON text or an
 * existing @c boost::json::value.
 */
class GJsonIArchive : public GIArchiveT<GJsonIArchive> {
public:
    /** @brief Parses @p text as JSON to read from; text that is no JSON throws weft_exception.
     *  @param text The JSON document. */
    /** @brief This archive reports an array of the wrong length itself, with its path (slot() and
     *  leave_array()), so GIArchiveT does not check the count of a tuple or std::array. */
    static constexpr bool reports_sequence_length = true;

    explicit GJsonIArchive(std::string_view text)
      : root_(parse(text)) {
    }
    /** @brief Reads from an existing JSON tree. @param v The JSON tree (copied). */
    explicit GJsonIArchive(boost::json::value v)
      : root_(std::move(v)) {
    }

    // --- primitive hooks --------------------------------------------------

    /** @brief Reads a boolean. @param b The value to fill. */
    void get_bool(bool &b) {
        const boost::json::value &v = slot();
        if(not v.is_bool()) {
            fail(expected("a bool", v));
        }
        b = v.get_bool();
    }

    /** @brief Reads an integer with a checked numeric conversion. @param v The value to fill. */
    template <typename int_type>
    void get_scalar(int_type &v) {
        static_assert(std::is_integral_v<int_type>);
        const boost::json::value &n = slot();
        if(not n.is_number()) {
            fail(expected("a number", n));
        }
        boost::system::error_code ec;
        const auto value = n.to_number<int_type>(ec);
        if(ec) {
            fail(
                "the number " + boost::json::serialize(n) + " does not fit its type (" + ec.message() +
                ")"
            );
        }
        v = value;
    }

    /**
     * @brief Reads a floating-point value from its exact string form (see
     * GJsonOArchive::put_fp): @c std::from_chars reproduces the written value
     * bit-exactly, independent of the locale. @param v The value to fill.
     */
    template <typename float_type>
    void get_fp(float_type &v) {
        static_assert(std::is_floating_point_v<float_type>);
        const boost::json::string &s = text(slot(), "a floating-point number as a string");
        const char *const last = s.data() + s.size();
        std::from_chars_result r{};
        if constexpr(std::is_same_v<float_type, long double>) {
            r = std::from_chars(s.data(), last, v, std::chars_format::hex);
        }
        else {
            r = std::from_chars(s.data(), last, v);
        }
        if(s.empty() || r.ec != std::errc{} || r.ptr != last) {
            fail("\"" + std::string(s.c_str()) + "\" is no floating-point number");
        }
    }

    /** @brief Reads a string. @param s The string to fill. */
    void get_string(std::string &s) {
        const boost::json::string &js = text(slot(), "a string");
        s.assign(js.begin(), js.end());
    }

    // --- framing hooks ----------------------------------------------------

    void enter_object() {
        const boost::json::value &s = slot();
        if(not s.is_object()) {
            fail(expected("an object", s));
        }
        stack_.push_back(Cursor{&s, 0, where_});
    }
    void leave_object() {
        stack_.pop_back();
    }
    /** @brief Reads a container's element count. @return The number of elements. */
    std::size_t begin_seq() {
        const boost::json::value &s = slot();
        if(not s.is_array()) {
            fail(expected("an array", s));
        }
        std::size_t n = s.get_array().size();
        stack_.push_back(Cursor{&s, 0, where_});
        return n;
    }
    void end_seq() {
        leave_array();
    }
    /** @brief Selects the object member the next read targets. @param name The member name. */
    void member(const char *name) {
        pending_key_ = name;
    }
    void begin_elem() {
    }
    void end_elem() {
    }

    // Raw caller-managed range framing (make_array / make_binary).
    void begin_raw() {
        const boost::json::value &s = slot();
        if(not s.is_array()) {
            fail(expected("an array", s));
        }
        stack_.push_back(Cursor{&s, 0, where_});
    }
    void end_raw() {
        leave_array();
    }
    /** @brief Reads a hex string of @p n bytes into @p p. @param p The pre-sized destination. @param n The byte count. */
    void get_bytes(void *p, std::size_t n) {
        const boost::json::string &s = text(slot(), "a hex string");
        from_hex(std::string_view{s.data(), s.size()}, p, n);
    }

private:
    void from_hex(std::string_view hex, void *p, std::size_t n) const {
        if(hex.size() != 2 * n) {
            fail(
                "the hex string has " + std::to_string(hex.size()) + " digits, " +
                std::to_string(2 * n) + " were expected (for " + std::to_string(n) + " bytes)"
            );
        }
        auto nibble = [this](char c) -> unsigned {
            if(c >= '0' && c <= '9') {
                return static_cast<unsigned>(c - '0');
            }
            if(c >= 'a' && c <= 'f') {
                return static_cast<unsigned>(c - 'a' + 10);
            }
            if(c >= 'A' && c <= 'F') {
                return static_cast<unsigned>(c - 'A' + 10);
            }
            fail(std::string("'") + c + "' is no hex digit");
        };
        auto *b = static_cast<unsigned char *>(p);
        for(std::size_t i = 0; i < n; ++i) {
            b[i] = static_cast<unsigned char>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
        }
    }

    struct Cursor {
        const boost::json::value *node;
        std::size_t idx;   // next array index to read (unused for objects)
        std::string where; // the path of this object or array in the tree
    };

    // Parses the document; text that is no JSON is a malformed archive
    static boost::json::value parse(std::string_view text) {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if(ec) {
            throw weft_exception(
                weft_error_streamer() << "In GJsonIArchive: the archive is no JSON: " << ec.message()
                                      << '\n'
            );
        }
        return v;
    }

    // Throws the weft_exception of a malformed archive, naming the path of the value last taken
    [[noreturn]] void fail(const std::string &reason) const {
        throw weft_exception(
            weft_error_streamer() << "In GJsonIArchive: the archive is malformed at "
                                  << (where_.empty() ? std::string("/") : where_) << ": " << reason
                                  << '\n'
        );
    }

    // "<what> was expected, found <kind of the value>"
    static std::string expected(const std::string &what, const boost::json::value &found) {
        return what + " was expected, found " + std::string(boost::json::to_string(found.kind()));
    }

    // The string a value must be
    const boost::json::string &text(const boost::json::value &v, const std::string &what) const {
        if(not v.is_string()) {
            fail(expected(what, v));
        }
        return v.get_string();
    }

    // Leaves an array, which must have been read to its end: a fixed-size container (std::array,
    // tuple) or a raw range reads as many elements as it has, so a longer array is malformed
    void leave_array() {
        const Cursor &c = stack_.back();
        const std::size_t size = c.node->get_array().size();
        if(c.idx != size) {
            where_ = c.where;
            fail(
                "the array holds " + std::to_string(size) + " elements, " + std::to_string(c.idx) +
                " were expected"
            );
        }
        stack_.pop_back();
    }

    // The next value to read at the current level: an object member by key, an
    // array element by advancing cursor, or the whole tree at top level. Records
    // its path (where_) for the message of a malformed archive.
    const boost::json::value &slot() {
        if(stack_.empty()) {
            where_.clear();
            return root_;
        }
        Cursor &c = stack_.back();
        if(c.node->is_object()) {
            where_ = c.where + "/" + pending_key_;
            const boost::json::value *v = c.node->get_object().if_contains(pending_key_);
            if(v == nullptr) {
                fail("the key \"" + pending_key_ + "\" is missing");
            }
            return *v;
        }
        const boost::json::array &a = c.node->get_array();
        where_ = c.where + "/" + std::to_string(c.idx);
        if(c.idx >= a.size()) {
            where_ = c.where;
            fail(
                "the array holds " + std::to_string(a.size()) + " elements, more were expected"
            );
        }
        return a[c.idx++];
    }

    boost::json::value root_;
    std::string pending_key_;
    std::string where_; // the path of the value last taken (slot())
    std::vector<Cursor> stack_;
};

/******************************************************************************/

} // namespace Gem::Weft
