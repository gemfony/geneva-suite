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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
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
 * shortest round-trip-exact decimal (@c %.9g / @c %.17g, read back with
 * @c strtof / @c strtod), and @c long @c double as an exact hexadecimal float
 * (@c %La). Strings are used because boost::json's number serializer is not
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
    [[nodiscard]] const boost::json::value &value() const { return root_; }
    /** @brief The assembled tree serialized to compact JSON text. */
    [[nodiscard]] std::string str() const { return boost::json::serialize(root_); }

    // --- primitive hooks --------------------------------------------------

    /** @brief Stores a boolean. @param b The value. */
    void put_bool(bool b) { place(boost::json::value(b)); }

    /** @brief Stores an integer as a signed or unsigned JSON number by its signedness. @param v The value. */
    template <typename Int>
    void put_scalar(Int v) {
        static_assert(std::is_integral_v<Int>);
        if constexpr (std::is_signed_v<Int>) {
            place(boost::json::value(static_cast<std::int64_t>(v)));
        } else {
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
     * use the shortest round-trip-exact decimal (@c %.9g / @c %.17g), which reads back
     * bit-exactly via @c strtof / @c strtod and stays human-readable; @c long @c double
     * uses an exact hexadecimal float (@c %La). @param v The value.
     */
    template <typename Float>
    void put_fp(Float v) {
        static_assert(std::is_floating_point_v<Float>);
        place(boost::json::value(fp_to_string(v)));
    }

    /** @brief Stores a string. @param s The string. */
    void put_string(std::string_view s) { place(boost::json::value(s)); }

    // --- framing hooks ----------------------------------------------------

    void begin_object() { open(boost::json::value(boost::json::object{})); }
    void end_object() { close(); }
    /** @brief Records the key the next attached value/node uses inside an object. @param name The member name. */
    void member(const char *name) { pending_key_ = name; }
    void begin_seq(std::size_t /*n*/) { open(boost::json::value(boost::json::array{})); }
    void end_seq() { close(); }
    void begin_elem() {}
    void end_elem() {}

    // Raw caller-managed range framing: an array of elements (make_array) or a
    // hex string (make_binary). The count is implicit in the JSON structure.
    void begin_raw(std::size_t /*n*/) { open(boost::json::value(boost::json::array{})); }
    void end_raw() { close(); }
    /** @brief Stores @p n opaque bytes as a lowercase hex string. @param p The block. @param n The byte count. */
    void put_bytes(const void *p, std::size_t n) { place(boost::json::value(to_hex(p, n))); }

private:
    static std::string to_hex(const void *p, std::size_t n) {
        static constexpr char digits[] = "0123456789abcdef";
        const auto *b = static_cast<const unsigned char *>(p);
        std::string out;
        out.resize(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
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
        if (stack_.empty()) {
            root_ = std::move(v);
        } else {
            attach(stack_.back().node, std::move(v), pending_key_);
        }
    }

    void open(boost::json::value container) {
        stack_.push_back(Frame{std::move(container), pending_key_});
    }

    void close() {
        Frame f = std::move(stack_.back());
        stack_.pop_back();
        if (stack_.empty()) {
            root_ = std::move(f.node);
        } else {
            attach(stack_.back().node, std::move(f.node), f.key);
        }
    }

    static void attach(boost::json::value &parent, boost::json::value v, const std::string &key) {
        if (parent.is_object()) {
            parent.as_object()[key] = std::move(v);
        } else {
            parent.as_array().push_back(std::move(v));
        }
    }

    // Formats a floating-point value as an exact, re-parseable decimal (float/double)
    // or hex-float (long double) string. The precisions %.9g / %.17g are the
    // round-trip-minimal digit counts for IEEE single / double.
    template <typename Float>
    static std::string fp_to_string(Float v) {
        char buf[64];
        if constexpr (std::is_same_v<Float, long double>) {
            std::snprintf(buf, sizeof(buf), "%La", v); // hex float: exact and re-parseable
        } else if constexpr (std::is_same_v<Float, float>) {
            std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        } else {
            std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(v));
        }
        return std::string{buf};
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
    /** @brief Parses @p text as JSON to read from. @param text The JSON document. */
    explicit GJsonIArchive(std::string_view text) : root_(boost::json::parse(text)) {}
    /** @brief Reads from an existing JSON tree. @param v The JSON tree (copied). */
    explicit GJsonIArchive(boost::json::value v) : root_(std::move(v)) {}

    // --- primitive hooks --------------------------------------------------

    /** @brief Reads a boolean. @param b The value to fill. */
    void get_bool(bool &b) { b = slot().as_bool(); }

    /** @brief Reads an integer with a checked numeric conversion. @param v The value to fill. */
    template <typename Int>
    void get_scalar(Int &v) {
        static_assert(std::is_integral_v<Int>);
        v = slot().to_number<Int>();
    }

    /**
     * @brief Reads a floating-point value from its exact string form (see
     * GJsonOArchive::put_fp): @c strtof / @c strtod / @c strtold reproduce the
     * written value bit-exactly. @param v The value to fill.
     */
    template <typename Float>
    void get_fp(Float &v) {
        static_assert(std::is_floating_point_v<Float>);
        const boost::json::string &s = slot().as_string();
        if constexpr (std::is_same_v<Float, long double>) {
            v = std::strtold(s.c_str(), nullptr);
        } else if constexpr (std::is_same_v<Float, float>) {
            v = std::strtof(s.c_str(), nullptr);
        } else {
            v = std::strtod(s.c_str(), nullptr);
        }
    }

    /** @brief Reads a string. @param s The string to fill. */
    void get_string(std::string &s) {
        const boost::json::string &js = slot().as_string();
        s.assign(js.begin(), js.end());
    }

    // --- framing hooks ----------------------------------------------------

    void enter_object() {
        const boost::json::value &s = slot();
        stack_.push_back(Cursor{&s, 0});
    }
    void leave_object() { stack_.pop_back(); }
    /** @brief Reads a container's element count. @return The number of elements. */
    std::size_t begin_seq() {
        const boost::json::value &s = slot();
        std::size_t n = s.as_array().size();
        stack_.push_back(Cursor{&s, 0});
        return n;
    }
    void end_seq() { stack_.pop_back(); }
    /** @brief Selects the object member the next read targets. @param name The member name. */
    void member(const char *name) { pending_key_ = name; }
    void begin_elem() {}
    void end_elem() {}

    // Raw caller-managed range framing (make_array / make_binary).
    void begin_raw() {
        const boost::json::value &s = slot();
        stack_.push_back(Cursor{&s, 0});
    }
    void end_raw() { stack_.pop_back(); }
    /** @brief Reads a hex string of @p n bytes into @p p. @param p The pre-sized destination. @param n The byte count. */
    void get_bytes(void *p, std::size_t n) {
        const boost::json::string &s = slot().as_string();
        from_hex(std::string_view{s.data(), s.size()}, p, n);
    }

private:
    static void from_hex(std::string_view hex, void *p, std::size_t n) {
        if (hex.size() != 2 * n) {
            throw weft_exception(
                weft_error_streamer()
                << "In GJsonIArchive::get_bytes(): hex string of length " << hex.size()
                << " does not match the expected " << (2 * n) << " (for " << n << " bytes)." << '\n'
            );
        }
        auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
            return 0;
        };
        auto *b = static_cast<unsigned char *>(p);
        for (std::size_t i = 0; i < n; ++i) {
            b[i] = static_cast<unsigned char>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
        }
    }

    struct Cursor {
        const boost::json::value *node;
        std::size_t idx; // next array index to read (unused for objects)
    };

    // The next value to read at the current level: an object member by key, an
    // array element by advancing cursor, or the whole tree at top level.
    const boost::json::value &slot() {
        if (stack_.empty()) {
            return root_;
        }
        Cursor &c = stack_.back();
        if (c.node->is_object()) {
            return c.node->as_object().at(pending_key_);
        }
        return c.node->as_array().at(c.idx++);
    }

    boost::json::value root_;
    std::string pending_key_;
    std::vector<Cursor> stack_;
};

/******************************************************************************/

} // namespace Gem::Weft
