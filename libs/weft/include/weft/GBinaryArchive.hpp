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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

// Boost headers go here

// Geneva headers go here
#include "weft/GArchive.hpp"
#include "weft/GWeftError.hpp"

namespace Gem::Weft {

/******************************************************************************/
/**
 * @file GBinaryArchive.hpp
 * @brief The compact binary codec of the @c GArchive family (the default wire
 * format): a flat, self-contained little-endian byte stream with no field names.
 *
 * @par Format
 * Scalars are stored at their natural width, little-endian (byte-swapped on a
 * big-endian host so the stream is canonical LE). @c bool is one byte;
 * @c float / @c double are the little-endian IEEE-754 binary32 / binary64 bit
 * patterns; @c long @c double is a little-endian IEEE-754 binary128 (16 bytes,
 * see the portability note). A string and every container are length-prefixed by
 * a 64-bit count. Object/element framing carries no bytes -- structure is implicit
 * in the fixed member walk, which is identical on save and load.
 *
 * @par Portability
 * All floating-point and integer encodings are endian-canonical (LE) and
 * architecture-portable. @c long @c double is widened to and stored as IEEE-754
 * binary128, so x86-64's 80-bit extended format and AArch64's native binary128
 * encode identically on the wire (the 80->128 widening is lossless), and the
 * encoding carries none of the indeterminate padding an 80-bit @c long @c double
 * holds in its 16-byte slot. On a toolchain with no 128-bit float type
 * (@c __SIZEOF_FLOAT128__ undefined) the codec falls back to raw platform bytes,
 * which round-trip bit-exactly only within one architecture (Inv 4).
 */
/******************************************************************************/

namespace detail {

/** @brief Byte-swaps @p u to little-endian storage order on a big-endian host; identity on little-endian. */
template <typename UInt>
constexpr UInt to_le(UInt u) noexcept {
    static_assert(std::is_unsigned_v<UInt>);
    if constexpr (std::endian::native == std::endian::big) {
        return std::byteswap(u);
    } else {
        return u;
    }
}

#if defined(__SIZEOF_FLOAT128__)
static_assert(sizeof(__float128) == 16, "binary128 must be 16 bytes");

/** @brief Reverses a 16-byte block in place (used to canonicalise binary128 to LE on a BE host). */
inline void reverse16(std::array<std::byte, 16> &b) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        std::byte t = b[i];
        b[i] = b[15 - i];
        b[15 - i] = t;
    }
}

/**
 * @brief Encodes a @c long @c double as a little-endian IEEE-754 binary128 (16 bytes).
 *
 * The value is widened to @c __float128 (IEEE binary128) and its bytes canonicalised to
 * little-endian order. This makes the @c long @c double encoding portable across
 * architectures (x86-64's 80-bit extended, AArch64's native binary128) AND free of the
 * indeterminate padding bytes an 80-bit @c long @c double carries in its 16-byte storage
 * slot. The 80->128 widening is lossless (binary128's 113-bit significand holds the 80-bit
 * format's 64-bit significand exactly).
 */
inline std::array<std::byte, 16> long_double_to_le128(long double v) noexcept {
    auto raw = std::bit_cast<std::array<std::byte, 16>>(static_cast<__float128>(v));
    if constexpr (std::endian::native == std::endian::big) {
        reverse16(raw);
    }
    return raw;
}

/** @brief The inverse of @ref long_double_to_le128: decodes a LE binary128 back to @c long @c double. */
inline long double le128_to_long_double(std::array<std::byte, 16> raw) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        reverse16(raw);
    }
    return static_cast<long double>(std::bit_cast<__float128>(raw));
}
#endif

} // namespace detail

/******************************************************************************/
/**
 * @brief The saving (output) binary codec. Accumulates the encoded stream in a
 * @c std::string, retrievable via @ref str().
 */
class GBinaryOArchive : public GOArchiveT<GBinaryOArchive> {
public:
    GBinaryOArchive() = default;

    /** @brief The accumulated encoded byte stream. */
    [[nodiscard]] const std::string &str() const { return buffer_; }
    /** @brief Moves the accumulated byte stream out (leaving this archive empty). */
    [[nodiscard]] std::string take() { return std::move(buffer_); }

    // --- primitive hooks (called by GOArchiveT) ---------------------------

    /** @brief Writes a boolean as a single 0/1 byte. @param b The value. */
    void put_bool(bool b) { buffer_.push_back(b ? char{1} : char{0}); }

    /** @brief Writes an integer at its natural width, little-endian. @param v The value. */
    template <typename Int>
    void put_scalar(Int v) {
        static_assert(std::is_integral_v<Int>);
        using UInt = std::make_unsigned_t<Int>;
        UInt le = detail::to_le(static_cast<UInt>(v));
        append_raw(&le, sizeof(le));
    }

    /** @brief Writes a floating-point value as a canonical LE IEEE-754 bit pattern (binary32/64, or
     *  binary128 for long double). @param v The value. */
    template <typename Float>
    void put_fp(Float v) {
        static_assert(std::is_floating_point_v<Float>);
        if constexpr (std::is_same_v<Float, float>) {
            std::uint32_t le = detail::to_le(std::bit_cast<std::uint32_t>(v));
            append_raw(&le, sizeof(le));
        } else if constexpr (std::is_same_v<Float, double>) {
            std::uint64_t le = detail::to_le(std::bit_cast<std::uint64_t>(v));
            append_raw(&le, sizeof(le));
        } else {
#if defined(__SIZEOF_FLOAT128__)
            // long double: canonicalised to a little-endian IEEE-754 binary128 (16 bytes), so it is
            // portable across architectures (x86-64's 80-bit extended and AArch64's native binary128
            // both encode identically) and carries none of the indeterminate padding bytes an 80-bit
            // long double has in its 16-byte slot. See detail::long_double_to_le128.
            auto raw = detail::long_double_to_le128(v);
            append_raw(raw.data(), raw.size());
#else
            // No 128-bit float type on this toolchain: fall back to raw platform bytes. Round-trips
            // bit-exactly within one binary on one architecture (Inv 4), but is not cross-arch portable.
            append_raw(&v, sizeof(v));
#endif
        }
    }

    /** @brief Writes a string as a 64-bit length prefix followed by its bytes. @param s The string. */
    void put_string(std::string_view s) {
        put_scalar(static_cast<std::uint64_t>(s.size()));
        buffer_.append(s.data(), s.size());
    }

    // --- framing hooks (structure is implicit in binary -> mostly no-ops) --

    void begin_object() {}
    void end_object() {}
    void member(const char * /*name*/) {}
    /** @brief Writes a container's element count as a 64-bit prefix. @param n The element count. */
    void begin_seq(std::size_t n) { put_scalar(static_cast<std::uint64_t>(n)); }
    void end_seq() {}
    void begin_elem() {}
    void end_elem() {}

    // Raw caller-managed range framing (make_array / make_binary): the count is
    // caller-managed and NOT written, so these carry no bytes in binary.
    void begin_raw(std::size_t /*n*/) {}
    void end_raw() {}
    /** @brief Writes @p n opaque bytes verbatim. @param p The block address. @param n The byte count. */
    void put_bytes(const void *p, std::size_t n) { append_raw(p, n); }

private:
    void append_raw(const void *p, std::size_t n) {
        const char *bytes = static_cast<const char *>(p);
        buffer_.append(bytes, n);
    }

    std::string buffer_;
};

/******************************************************************************/
/**
 * @brief The loading (input) binary codec. Reads from a caller-owned byte view
 * with a monotonically advancing cursor; underflow throws.
 */
class GBinaryIArchive : public GIArchiveT<GBinaryIArchive> {
public:
    /** @brief Constructs a reader over @p data (which must outlive this archive). @param data The encoded stream. */
    explicit GBinaryIArchive(std::string_view data) : data_(data) {}

    /** @brief Whether every byte has been consumed (a well-formed full read ends here). */
    [[nodiscard]] bool exhausted() const { return pos_ == data_.size(); }

    // --- primitive hooks (called by GIArchiveT) ---------------------------

    /** @brief Reads a single 0/1 byte into @p b. @param b The value to fill. */
    void get_bool(bool &b) {
        char c = 0;
        read_raw(&c, 1);
        b = (c != 0);
    }

    /** @brief Reads a natural-width little-endian integer into @p v. @param v The value to fill. */
    template <typename Int>
    void get_scalar(Int &v) {
        static_assert(std::is_integral_v<Int>);
        using UInt = std::make_unsigned_t<Int>;
        UInt le{};
        read_raw(&le, sizeof(le));
        v = static_cast<Int>(detail::to_le(le)); // to_le is its own inverse (identity or byteswap)
    }

    /** @brief Reads a floating-point value into @p v (LE IEEE bits for float/double; raw bytes for long double). */
    template <typename Float>
    void get_fp(Float &v) {
        static_assert(std::is_floating_point_v<Float>);
        if constexpr (std::is_same_v<Float, float>) {
            std::uint32_t le{};
            read_raw(&le, sizeof(le));
            v = std::bit_cast<float>(detail::to_le(le));
        } else if constexpr (std::is_same_v<Float, double>) {
            std::uint64_t le{};
            read_raw(&le, sizeof(le));
            v = std::bit_cast<double>(detail::to_le(le));
        } else {
#if defined(__SIZEOF_FLOAT128__)
            // long double: decode the little-endian binary128 written by put_fp (see
            // detail::le128_to_long_double). The 128->native narrowing is exact for the value ranges an
            // 80-bit or binary128 long double can represent.
            std::array<std::byte, 16> raw{};
            read_raw(raw.data(), raw.size());
            v = detail::le128_to_long_double(raw);
#else
            read_raw(&v, sizeof(v));
#endif
        }
    }

    /** @brief Reads a length-prefixed string into @p s. @param s The string to fill. */
    void get_string(std::string &s) {
        std::uint64_t n = 0;
        get_scalar(n);
        require(n);
        s.assign(data_.data() + pos_, static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
    }

    // --- framing hooks ----------------------------------------------------

    void enter_object() {}
    void leave_object() {}
    void member(const char * /*name*/) {}
    /** @brief Reads a container's 64-bit element count. @return The element count. */
    std::size_t begin_seq() {
        std::uint64_t n = 0;
        get_scalar(n);
        return static_cast<std::size_t>(n);
    }
    void end_seq() {}
    void begin_elem() {}
    void end_elem() {}

    // Raw caller-managed range framing (make_array / make_binary): no count read.
    void begin_raw() {}
    void end_raw() {}
    /** @brief Reads @p n opaque bytes verbatim into @p p. @param p The pre-sized destination. @param n The byte count. */
    void get_bytes(void *p, std::size_t n) { read_raw(p, n); }

private:
    void require(std::size_t n) const {
        if (pos_ + n > data_.size()) {
            throw weft_exception(
                weft_error_streamer()
                << "In GBinaryIArchive: attempt to read " << n << " byte(s) past the end of a "
                << data_.size() << "-byte stream (cursor at " << pos_ << "). Truncated or malformed archive." << '\n'
            );
        }
    }
    void read_raw(void *p, std::size_t n) {
        require(n);
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
    }

    std::string_view data_;
    std::size_t pos_ = 0;
};

/******************************************************************************/

} // namespace Gem::Weft
