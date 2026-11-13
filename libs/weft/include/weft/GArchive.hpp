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
#include <atomic>
#include <chrono>
#include <bitset>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <forward_list>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// Boost headers go here

// Geneva headers go here

namespace Gem::Weft {

/******************************************************************************/
/**
 * @file GArchive.hpp
 * @brief The structural core of the @c GArchive serialization family: the
 * @c nvp / @c base_object wrappers, the container/scalar type traits, and the
 * two CRTP dispatch bases (@ref GOArchiveT saving, @ref GIArchiveT loading)
 * that walk an object's members and hand primitives to a concrete codec.
 *
 * @par Design
 * A concrete codec (e.g. @c GBinaryOArchive, @c GJsonOArchive) derives from
 * @ref GOArchiveT / @ref GIArchiveT and implements a small, fixed set of
 * primitive and framing hooks (scalars, strings, object/sequence/element
 * delimiters, member keys). The CRTP base owns @b all type analysis -- it is
 * the single place that knows how a @c std::vector differs from a @c std::map
 * differs from a nested serializable class -- and expresses each as calls to
 * those hooks. This keeps the two codecs free of structural logic: the binary
 * codec makes the framing hooks no-ops and writes a flat byte stream; the JSON
 * codec makes them build a @c boost::json tree keyed by member name.
 *
 * @par Interface parity with Boost.Serialization
 * The reflection layer (@c GMemberReflectionT.hpp) reaches an archive through
 * exactly two spellings: @c "ar & make_nvp(name, ref)" and
 * @c "ar & make_nvp(name, base_object<Base>(derived))". This family mirrors
 * both -- @ref make_nvp and @ref base_object here have the same shape -- so
 * retargeting a class's generated @c serialize from a Boost archive to a
 * @c GArchive is a change of the archive @e type only, not of the call sites.
 *
 * @par Trees, not graphs
 * Geneva serializes trees without shared-object tracking (see the
 * Boost-replacement design doc): a cloneable member is deep-cloned on load, not
 * reconstructed as a shared node. This family therefore carries @b no
 * object-identity / de-duplication machinery. Polymorphic-pointer dispatch
 * (through @c GPolymorphicRegistry, requiring a small set of virtual serialize
 * hooks on the interface) is intentionally @b not part of this value-codec
 * layer; it arrives with the mixin integration step.
 */
/******************************************************************************/

/**
 * @brief A name/value pair: binds a member reference to the name a self-describing
 * codec (JSON) uses as its key; a nameless codec (binary) ignores the name.
 * @tparam T The (possibly const) referent type.
 */
template <typename T>
struct nvp {
    const char *name;
    T &value;
};

/** @brief Builds an @ref nvp. @param name The member name. @param value The member reference. */
template <typename T>
nvp<T> make_nvp(const char *name, T &value) {
    return nvp<T>{name, value};
}

/**
 * @brief Wraps a derived object so that only its @p Base slice is serialized in
 * place (the archive analogue of @c boost::serialization::base_object).
 * @tparam Base The base slice to serialize.
 * @tparam Derived The concrete object providing that slice.
 */
template <typename Base, typename Derived>
struct base_object_t {
    Derived &ref;
};

/** @brief Builds a @ref base_object_t. @param derived The derived object whose Base slice to serialize. */
template <typename Base, typename Derived>
base_object_t<Base, Derived> base_object(Derived &derived) {
    static_assert(std::is_base_of_v<Base, Derived>, "base_object<Base>(d): Base must be a base of decltype(d)");
    return base_object_t<Base, Derived>{derived};
}

/**
 * @brief Like @ref base_object_t but serializes the @p Base slice as a @b named,
 * @b nested member (a sub-object under @c name) rather than inline. This mirrors
 * Boost's @c make_nvp(name, base_object<Base>(d)) nesting -- used for a
 * hierarchy's parent slice -- so base and derived members cannot collide on a
 * shared name in the self-describing (JSON) codec.
 * @tparam Base The base slice to serialize.
 * @tparam Derived The concrete object providing that slice.
 */
template <typename Base, typename Derived>
struct named_base_object_t {
    const char *name;
    Derived &ref;
};

/** @brief Builds a @ref named_base_object_t. @param name The member name for the nested base slice. @param derived The derived object. */
template <typename Base, typename Derived>
named_base_object_t<Base, Derived> named_base(const char *name, Derived &derived) {
    static_assert(std::is_base_of_v<Base, Derived>, "named_base<Base>(name, d): Base must be a base of decltype(d)");
    return named_base_object_t<Base, Derived>{name, derived};
}

/**
 * @brief Empty marker base of every @c GArchive codec, so generic serialization
 * code can branch on "is this a GArchive?" (vs a Boost archive) at compile time
 * via @ref is_gem_archive_v.
 */
struct gem_archive_tag {};

/** @brief Whether @p Archive is a @c GArchive codec (derives from @ref gem_archive_tag). */
template <typename Archive>
inline constexpr bool is_gem_archive_v = std::is_base_of_v<gem_archive_tag, std::remove_cvref_t<Archive>>;

/******************************************************************************/
/**
 * @brief A caller-managed raw range of @p count contiguous @c T elements (the
 * @c GArchive analogue of @c boost::serialization::make_array). The count is
 * @b not stored -- the caller serializes/recovers it separately and the buffer
 * is pre-sized on load -- so the same value count must be supplied both ways.
 * @tparam T The element type (possibly const on save).
 */
template <typename T>
struct array_wrapper_t {
    T *data;
    std::size_t count;
};

/** @brief Builds an @ref array_wrapper_t. @param data Pointer to the first element. @param count The element count. */
template <typename T>
array_wrapper_t<T> make_array(T *data, std::size_t count) {
    return array_wrapper_t<T>{data, count};
}

/**
 * @brief A caller-managed raw block of @p bytes opaque bytes (the @c GArchive
 * analogue of @c boost::serialization::make_binary_object). Like @ref make_array
 * the length is caller-managed; the block is stored verbatim (binary) or as a
 * hex string (JSON).
 */
struct binary_wrapper_t {
    void *data;
    std::size_t bytes;
};

/** @brief Builds a @ref binary_wrapper_t. @param data The block address. @param bytes The block length. */
inline binary_wrapper_t make_binary(void *data, std::size_t bytes) {
    return binary_wrapper_t{data, bytes};
}

/******************************************************************************/
/**
 * @par Type coverage vs Boost.Serialization's out-of-the-box catalogue
 * The @c GArchive family aims for parity with the std types Boost.Serialization
 * ships support for. Status of every Boost-native serializer:
 *
 *  Supported (value dispatch in @ref GOArchiveT / @ref GIArchiveT):
 *    - all arithmetic types, @c bool, @c enum, @c float / @c double / @c long double
 *    - @c std::string, @c std::filesystem::path
 *    - @c std::atomic<T>
 *    - @c std::pair, @c std::tuple, @c std::array, @c std::optional, @c std::variant,
 *      @c std::complex<T>, @c std::bitset<N>
 *    - @c std::vector, @c std::deque, @c std::list, @c std::forward_list
 *    - @c std::set / @c std::multiset, @c std::unordered_set / @c std::unordered_multiset
 *    - @c std::map / @c std::multimap, @c std::unordered_map / @c std::unordered_multimap
 *    - @c std::unique_ptr / @c std::shared_ptr (polymorphic; see GArchivePolymorphic.hpp)
 *    - @c make_array / @c make_binary (caller-managed raw ranges; the two above)
 *
 *  Deliberately NOT pre-built (add on demand -- each is a trait + a process()
 *  arm of a few lines -- rather than ship untested code no consumer exercises):
 *    - @c std::wstring / @c std::u16string / @c std::u32string -- no serialized
 *      consumer; needs a fixed code-unit wire encoding first.
 *    - @c std::valarray -- semantically a @c std::vector; use that.
 *    - C-style arrays @c T[N] -- use @c std::array or @ref make_array.
 *
 *  Intentionally unsupported (incompatible with the design, not an omission):
 *    - @c std::weak_ptr and Boost's shared-pointer @e tracking -- these express a
 *      shared object graph. Geneva serializes @b trees (deep-clone on load, zero
 *      object tracking); a weak_ptr has no owning tree edge to reconstruct.
 *    - @c boost::scoped_ptr and other Boost-only wrappers -- deprecated / use std.
 */
/******************************************************************************/

/**
 * @brief Access shim invoking a class's (often private) @c serialize member.
 * A serializable class grants access with
 * @c "friend struct Gem::Weft::access;".
 */
struct access {
    /** @brief Invokes @c obj.serialize(ar, 0). @param ar The archive. @param obj The object to (de)serialize. */
    template <typename Archive, typename T>
    static void serialize(Archive &ar, T &obj) {
        obj.serialize(ar, 0u);
    }

    // Immediate-context detection of a reachable serialize MEMBER. Because these
    // overloads are members of access -- the type serializable classes befriend --
    // the decltype probe can see a PRIVATE serialize + friend access; a plain
    // external requires-expression could not. A type without the member (or that
    // does not grant access friendship) drops to the varargs overload.
    template <typename Archive, typename T>
    static auto has_member_probe(int)
        -> decltype(std::declval<T &>().serialize(std::declval<Archive &>(), 0u), std::true_type{});
    template <typename Archive, typename T>
    static std::false_type has_member_probe(...);

    /**
     * @brief Default-constructs a @p T on the heap through this friend shim,
     * reaching a *private* default constructor (the analogue of Boost's
     * @c access::construct). A concrete pointee kept default-constructible only for
     * de-serialization grants access with @c "friend struct Gem::Weft::access;";
     * the non-polymorphic owned-pointer load path calls this when the pointee is
     * not publicly default-constructible. (Hierarchy types instead reconstruct
     * through @c GReflectiveInterfaceAccess::construct via the registry.)
     * @tparam T The concrete pointee to construct.
     * @return A raw owning pointer, adopted by the caller's smart pointer.
     */
    template <typename T>
    static T *construct_raw() {
        return new T();
    }
};

/******************************************************************************/
/**
 * @brief Construction seam for the polymorphic-registry factory of a
 * reflection-managed type whose default constructor is private.
 *
 * @b Declared here (in the reflection-free codec base) but @b defined in
 * @c GReflectiveInterfaceT.hpp, which owns the @c GReflectiveInterfaceAccess friend
 * shim that can reach the private constructor. This indirection is what lets
 * @c GArchivePolymorphic.hpp call the seam @e without including the reflection
 * layer -- breaking the otherwise-cyclic
 * @c GArchivePolymorphic -> @c GReflectiveInterfaceT -> @c GCommonInterfaceT
 * include chain, so the codec/dispatch layer can be pulled into @c GCommonInterfaceT
 * (the @c toStream / @c fromStream codec arm). It is only ever instantiated at a
 * @c GEM_REGISTER_ARCHIVABLE site, which includes the concrete type's header and
 * hence the definition.
 *
 * @tparam T The concrete reflection-managed type to reconstruct.
 * @return An owning pointer to a freshly default-constructed @p T, as its hierarchy root.
 */
template <typename T>
std::unique_ptr<typename T::gemfony_common_root_t> gem_registry_construct_reflective();

/******************************************************************************/
// Type traits identifying the finite set of value shapes the codecs handle.

namespace detail {

template <typename T>
struct is_pair : std::false_type {};
template <typename A, typename B>
struct is_pair<std::pair<A, B>> : std::true_type {};

template <typename T>
struct is_tuple : std::false_type {};
template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type {};

template <typename T>
struct is_std_array : std::false_type {};
template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

// Key/value associative containers, including the multi-key flavours (whose
// emplace always inserts, so duplicate keys survive a round trip).
template <typename T>
struct is_map : std::false_type {};
template <typename K, typename V, typename... R>
struct is_map<std::map<K, V, R...>> : std::true_type {};
template <typename K, typename V, typename... R>
struct is_map<std::multimap<K, V, R...>> : std::true_type {};
template <typename K, typename V, typename... R>
struct is_map<std::unordered_map<K, V, R...>> : std::true_type {};
template <typename K, typename V, typename... R>
struct is_map<std::unordered_multimap<K, V, R...>> : std::true_type {};

// Sequence and set-like containers: a size, a value_type, iterable, and
// (for load) clearable + able to grow one element at a time. std::forward_list
// is handled on its own arm (no size(), front insertion) -- not here.
template <typename T>
struct is_sequence : std::false_type {};
template <typename T, typename... R>
struct is_sequence<std::vector<T, R...>> : std::true_type {};
template <typename T, typename... R>
struct is_sequence<std::deque<T, R...>> : std::true_type {};
template <typename T, typename... R>
struct is_sequence<std::list<T, R...>> : std::true_type {};
template <typename T, typename... R>
struct is_sequence<std::set<T, R...>> : std::true_type {};
template <typename T, typename... R>
struct is_sequence<std::multiset<T, R...>> : std::true_type {};
template <typename T, typename... R>
struct is_sequence<std::unordered_set<T, R...>> : std::true_type {};
template <typename T, typename... R>
struct is_sequence<std::unordered_multiset<T, R...>> : std::true_type {};

// std::vector<bool> is a proxy-reference specialization: its elements are not
// real bool lvalues, so the generic is_sequence arm (which binds each element by
// reference) cannot handle it. It gets its own arm, serializing bit by bit.
template <typename T>
inline constexpr bool is_vector_bool_v = std::is_same_v<T, std::vector<bool>>;

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
struct is_variant : std::false_type {};
template <typename... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};

template <typename T>
struct is_complex : std::false_type {};
template <typename T>
struct is_complex<std::complex<T>> : std::true_type {};

template <typename T>
struct is_bitset : std::false_type {};
template <std::size_t N>
struct is_bitset<std::bitset<N>> : std::true_type {};

template <typename T>
struct is_forward_list : std::false_type {};
template <typename T, typename... R>
struct is_forward_list<std::forward_list<T, R...>> : std::true_type {};

template <typename T>
inline constexpr bool is_string_v = std::is_same_v<T, std::string>;

template <typename T>
inline constexpr bool is_path_v = std::is_same_v<T, std::filesystem::path>;

template <typename T>
struct is_atomic : std::false_type {};
template <typename T>
struct is_atomic<std::atomic<T>> : std::true_type {};

// A std::chrono::duration is serialized by its raw count (its Rep), matching the
// Boost split-free handling in GSerializationHelperFunctionsT.hpp. std types cannot
// be reached by an ADL free gem_archive_serialize, so this is a built-in codec arm.
template <typename T>
struct is_chrono_duration : std::false_type {};
template <typename Rep, typename Period>
struct is_chrono_duration<std::chrono::duration<Rep, Period>> : std::true_type {};

// Owning smart pointers. Only the shapes Geneva serializes are matched.
template <typename T>
struct is_smart_ptr : std::false_type {};
template <typename T, typename D>
struct is_smart_ptr<std::unique_ptr<T, D>> : std::true_type {};
template <typename T>
struct is_smart_ptr<std::shared_ptr<T>> : std::true_type {};

// Whether a pointee type belongs to a GCommonInterfaceT hierarchy (exposes
// gemfony_common_root_t). Such a pointer is dispatched POLYMORPHICALLY through the
// registry (GArchivePolymorphic.hpp); a pointer to a concrete non-hierarchy type is
// instead serialized inline by value (present flag + pointee), needing no registry.
template <typename Pointee>
concept has_common_root = requires { typename Pointee::gemfony_common_root_t; };

// A "value class" is anything not covered above that is (de)serializable in one
// of two ways, mirroring Boost's intrusive/non-intrusive split:
//   - INTRUSIVE: a serialize member reachable via the access shim (checked
//     structurally, so a private serialize + friend access still qualifies) --
//     the reflective-mixin classes;
//   - NON-INTRUSIVE: a free gem_archive_serialize(Archive&, T&) found by ADL in
//     the type's own namespace -- the analogue of a Boost non-intrusive free
//     serialize(), for the POD-clean structs kept free of a serialize member.
// The class dispatch below prefers the member form and otherwise takes the free
// form; a type offering neither trips the static_assert.
template <typename Archive, typename T>
concept has_member_serialize = decltype(access::has_member_probe<Archive, T>(0))::value;

template <typename Archive, typename T>
concept has_free_serialize = requires(Archive &ar, T &t) { gem_archive_serialize(ar, t); };

template <typename Archive, typename T>
concept serializable_class = has_member_serialize<Archive, T> || has_free_serialize<Archive, T>;

// set-family containers grow via insert(), std::vector/deque/list via
// push_back(); distinguish so the load base picks the right insertion.
template <typename T>
inline constexpr bool is_set_like_v = false;
template <typename T, typename... R>
inline constexpr bool is_set_like_v<std::set<T, R...>> = true;
template <typename T, typename... R>
inline constexpr bool is_set_like_v<std::multiset<T, R...>> = true;
template <typename T, typename... R>
inline constexpr bool is_set_like_v<std::unordered_set<T, R...>> = true;
template <typename T, typename... R>
inline constexpr bool is_set_like_v<std::unordered_multiset<T, R...>> = true;

} // namespace detail

/******************************************************************************/
/**
 * @brief CRTP dispatch base for a @b saving (output) archive.
 *
 * Implements @c operator& for @ref nvp, @ref base_object_t, and arbitrary
 * values, decomposing each value into calls on the concrete codec @p Derived.
 * @p Derived must provide the saving hooks:
 *   - @c put_bool(bool) / @c put_scalar(Integral) / @c put_fp(FloatingPoint)
 *   - @c put_string(std::string_view)
 *   - @c begin_object() / @c end_object() / @c member(const char*)
 *   - @c begin_seq(std::size_t) / @c end_seq() / @c begin_elem() / @c end_elem()
 *
 * @tparam Derived The concrete saving codec (CRTP).
 */
template <typename Derived>
class GOArchiveT : public gem_archive_tag {
public:
    static constexpr bool is_saving = true;
    static constexpr bool is_loading = false;

    /** @brief Serializes a named member. @param n The name/value pair. */
    template <typename T>
    Derived &operator&(const nvp<T> &n) {
        d().member(n.name);
        return process(n.value);
    }

    /** @brief Serializes only the @p Base slice of a derived object, inline. @param b The base-object wrapper. */
    template <typename Base, typename Der>
    Derived &operator&(const base_object_t<Base, Der> &b) {
        access::serialize(d(), base_slice<Base>(b.ref));
        return d();
    }

    /** @brief Serializes the @p Base slice as a named, nested sub-object. @param b The named base-object wrapper. */
    template <typename Base, typename Der>
    Derived &operator&(const named_base_object_t<Base, Der> &b) {
        d().member(b.name);
        d().begin_object();
        access::serialize(d(), base_slice<Base>(b.ref));
        d().end_object();
        return d();
    }

    /** @brief Serializes a caller-managed raw element range (no stored count). @param a The array wrapper. */
    template <typename T>
    Derived &operator&(const array_wrapper_t<T> &a) {
        d().begin_raw(a.count);
        for (std::size_t i = 0; i < a.count; ++i) {
            d().begin_elem();
            process(a.data[i]);
            d().end_elem();
        }
        d().end_raw();
        return d();
    }

    /** @brief Serializes a caller-managed raw byte block. @param b The binary wrapper. */
    Derived &operator&(const binary_wrapper_t &b) {
        d().put_bytes(b.data, b.bytes);
        return d();
    }

    /** @brief Serializes an unnamed value. @param v The value to save. */
    template <typename T>
    Derived &operator&(const T &v) {
        return process(const_cast<T &>(v)); // save never mutates; const_cast lets one serialize() serve both directions
    }

    /** @brief Boost-style save alias. @param v The value to save. */
    template <typename T>
    Derived &operator<<(const T &v) {
        return *this & v;
    }

private:
    Derived &d() { return static_cast<Derived &>(*this); }

    // Yields the @p Base slice as a non-const reference regardless of the derived
    // object's constness. A saving archive never mutates what it visits, but a
    // genuinely const derived (e.g. a Boost-style split save(), which is const)
    // must still route its base slice through the one serialize() that serves both
    // directions -- so const is cast away here on the save side only.
    template <typename Base, typename Der>
    static Base &base_slice(Der &derived) {
        return const_cast<Base &>(static_cast<const Base &>(derived));
    }

    template <typename T>
    Derived &process(T &v_in) {
        using U = std::remove_cv_t<T>;
        // A saving archive never mutates what it visits; normalize away any const
        // on the referent so every arm (and the free/member serializers it calls)
        // sees one non-const U&, whether the value arrived through a const
        // container/member (e.g. a by-value save of a const layout) or not.
        U &v = const_cast<U &>(v_in);
        if constexpr (std::is_same_v<U, bool>) {
            d().put_bool(v);
        } else if constexpr (std::is_enum_v<U>) {
            auto raw = static_cast<std::underlying_type_t<U>>(v);
            d().put_scalar(raw);
        } else if constexpr (std::is_integral_v<U>) {
            d().put_scalar(v);
        } else if constexpr (std::is_floating_point_v<U>) {
            d().put_fp(v);
        } else if constexpr (detail::is_string_v<U>) {
            d().put_string(std::string_view{v});
        } else if constexpr (detail::is_path_v<U>) {
            d().put_string(v.string());
        } else if constexpr (detail::is_atomic<U>::value) {
            typename U::value_type held = v.load();
            process(held);
        } else if constexpr (detail::is_chrono_duration<U>::value) {
            typename U::rep count = v.count();
            process(count);
        } else if constexpr (detail::is_optional<U>::value) {
            d().begin_object();
            bool present = v.has_value();
            d().member("present");
            process(present);
            if (present) {
                d().member("value");
                process(*v);
            }
            d().end_object();
        } else if constexpr (detail::is_complex<U>::value) {
            typename U::value_type re = v.real();
            typename U::value_type im = v.imag();
            d().begin_object();
            d().member("re");
            process(re);
            d().member("im");
            process(im);
            d().end_object();
        } else if constexpr (detail::is_bitset<U>::value) {
            std::string bits = v.to_string();
            process(bits);
        } else if constexpr (std::is_same_v<U, std::monostate>) {
            // The canonical EMPTY variant alternative. It carries no state, but the codecs are
            // structural (the JSON driver needs a value for every member it opened), so it is written
            // as an empty object -- the same shape a stateless serializable class produces.
            d().begin_object();
            d().end_object();
        } else if constexpr (detail::is_variant<U>::value) {
            std::size_t idx = v.index();
            d().begin_object();
            d().member("index");
            process(idx);
            d().member("value");
            std::visit([this](auto &alt) { process(alt); }, v);
            d().end_object();
        } else if constexpr (detail::is_forward_list<U>::value) {
            std::size_t n = static_cast<std::size_t>(std::distance(v.begin(), v.end()));
            d().begin_seq(n);
            for (auto &e : v) {
                d().begin_elem();
                process(e);
                d().end_elem();
            }
            d().end_seq();
        } else if constexpr (detail::is_pair<U>::value) {
            d().begin_object();
            d().member("first");
            process(v.first);
            d().member("second");
            process(v.second);
            d().end_object();
        } else if constexpr (detail::is_tuple<U>::value) {
            d().begin_seq(std::tuple_size_v<U>);
            std::apply(
                [&](auto &...elems) {
                    ((d().begin_elem(), process(elems), d().end_elem()), ...);
                },
                v);
            d().end_seq();
        } else if constexpr (detail::is_std_array<U>::value) {
            d().begin_seq(v.size());
            for (auto &e : v) {
                d().begin_elem();
                process(e);
                d().end_elem();
            }
            d().end_seq();
        } else if constexpr (detail::is_map<U>::value) {
            d().begin_seq(v.size());
            for (auto &kv : v) {
                d().begin_elem();
                d().begin_object();
                d().member("key");
                process(kv.first);
                d().member("value");
                process(kv.second);
                d().end_object();
                d().end_elem();
            }
            d().end_seq();
        } else if constexpr (detail::is_vector_bool_v<U>) {
            d().begin_seq(v.size());
            for (bool bit : v) {
                d().begin_elem();
                process(bit);
                d().end_elem();
            }
            d().end_seq();
        } else if constexpr (detail::is_sequence<U>::value) {
            d().begin_seq(v.size());
            for (auto &e : v) {
                d().begin_elem();
                process(e);
                d().end_elem();
            }
            d().end_seq();
        } else if constexpr (detail::is_smart_ptr<U>::value) {
            if constexpr (detail::has_common_root<typename U::element_type>) {
                // POLYMORPHIC owning pointer (element type is a GCommonInterfaceT
                // hierarchy root): dispatched through the registry by
                // gem_serialize_pointer, an ADL customization point defined in
                // GArchivePolymorphic.hpp. Kept out of this base so the base never
                // depends on the registry/codecs -- the include-cycle break of the
                // "codec layer owns pointer dispatch" layering. Found at instantiation
                // via ADL on the archive type; a TU serializing such a pointer must
                // include GArchivePolymorphic.hpp (registration sites / choke points do).
                gem_serialize_pointer(d(), v);
            } else {
                // NON-polymorphic owning pointer to a concrete, final type (no
                // hierarchy / no derived types): the analogue of Boost serializing a
                // shared_ptr<Concrete> by value. No tag/registry -- just a present
                // flag plus the pointee serialized inline. Reconstructed on load with
                // the pointee's accessible default constructor.
                d().begin_object();
                bool present = static_cast<bool>(v);
                d().member("present");
                process(present);
                if (present) {
                    d().member("value");
                    process(*v);
                }
                d().end_object();
            }
        } else {
            static_assert(detail::serializable_class<Derived, U>,
                          "GOArchiveT: type is neither a supported primitive/container/pointer nor a serializable "
                          "class (needs a serialize member reachable via Weft::access, or a non-intrusive free "
                          "gem_archive_serialize(Archive&, T&) in the type's namespace)");
            d().begin_object();
            if constexpr (detail::has_member_serialize<Derived, U>) {
                access::serialize(d(), v);
            } else {
                gem_archive_serialize(d(), v); // ADL: non-intrusive free serializer in U's namespace
            }
            d().end_object();
        }
        return d();
    }
};

/******************************************************************************/
/**
 * @brief CRTP dispatch base for a @b loading (input) archive.
 *
 * The mirror of @ref GOArchiveT. @p Derived must provide the loading hooks:
 *   - @c get_bool(bool&) / @c get_scalar(Integral&) / @c get_fp(FloatingPoint&)
 *   - @c get_string(std::string&)
 *   - @c enter_object() / @c leave_object() / @c member(const char*)
 *   - @c begin_seq() -> std::size_t / @c end_seq() / @c begin_elem() / @c end_elem()
 *
 * @tparam Derived The concrete loading codec (CRTP).
 */
template <typename Derived>
class GIArchiveT : public gem_archive_tag {
public:
    static constexpr bool is_saving = false;
    static constexpr bool is_loading = true;

    /** @brief Loads a named member. @param n The name/value pair to fill. */
    template <typename T>
    Derived &operator&(const nvp<T> &n) {
        d().member(n.name);
        return process(n.value);
    }

    /** @brief Loads only the @p Base slice of a derived object, inline. @param b The base-object wrapper. */
    template <typename Base, typename Der>
    Derived &operator&(const base_object_t<Base, Der> &b) {
        access::serialize(d(), static_cast<Base &>(b.ref));
        return d();
    }

    /** @brief Loads the @p Base slice from a named, nested sub-object. @param b The named base-object wrapper. */
    template <typename Base, typename Der>
    Derived &operator&(const named_base_object_t<Base, Der> &b) {
        d().member(b.name);
        d().enter_object();
        access::serialize(d(), static_cast<Base &>(b.ref));
        d().leave_object();
        return d();
    }

    /** @brief Loads a caller-managed raw element range into a pre-sized buffer. @param a The array wrapper. */
    template <typename T>
    Derived &operator&(const array_wrapper_t<T> &a) {
        d().begin_raw();
        for (std::size_t i = 0; i < a.count; ++i) {
            d().begin_elem();
            process(a.data[i]);
            d().end_elem();
        }
        d().end_raw();
        return d();
    }

    /** @brief Loads a caller-managed raw byte block into a pre-sized buffer. @param b The binary wrapper. */
    Derived &operator&(const binary_wrapper_t &b) {
        d().get_bytes(b.data, b.bytes);
        return d();
    }

    /** @brief Loads an unnamed value. @param v The value to fill. */
    template <typename T>
    Derived &operator&(T &v) {
        return process(v);
    }

    /** @brief Boost-style load alias. @param v The value to fill. */
    template <typename T>
    Derived &operator>>(T &v) {
        return *this & v;
    }

private:
    Derived &d() { return static_cast<Derived &>(*this); }

    template <typename T>
    Derived &process(T &v) {
        using U = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<U, bool>) {
            d().get_bool(v);
        } else if constexpr (std::is_enum_v<U>) {
            std::underlying_type_t<U> raw{};
            d().get_scalar(raw);
            v = static_cast<U>(raw);
        } else if constexpr (std::is_integral_v<U>) {
            d().get_scalar(v);
        } else if constexpr (std::is_floating_point_v<U>) {
            d().get_fp(v);
        } else if constexpr (detail::is_string_v<U>) {
            d().get_string(v);
        } else if constexpr (detail::is_path_v<U>) {
            std::string s;
            d().get_string(s);
            v = std::filesystem::path{s};
        } else if constexpr (detail::is_atomic<U>::value) {
            typename U::value_type held{};
            process(held);
            v.store(held);
        } else if constexpr (detail::is_chrono_duration<U>::value) {
            typename U::rep count{};
            process(count);
            v = U(count);
        } else if constexpr (detail::is_optional<U>::value) {
            d().enter_object();
            bool present = false;
            d().member("present");
            process(present);
            if (present) {
                typename U::value_type tmp{};
                d().member("value");
                process(tmp);
                v = std::move(tmp);
            } else {
                v.reset();
            }
            d().leave_object();
        } else if constexpr (detail::is_complex<U>::value) {
            typename U::value_type re{};
            typename U::value_type im{};
            d().enter_object();
            d().member("re");
            process(re);
            d().member("im");
            process(im);
            d().leave_object();
            v = U{re, im};
        } else if constexpr (detail::is_bitset<U>::value) {
            std::string bits;
            process(bits);
            v = U{bits};
        } else if constexpr (std::is_same_v<U, std::monostate>) {
            // The canonical EMPTY variant alternative -- written as an empty object (see the saving
            // side); consume that object so the stream position stays in step.
            d().enter_object();
            d().leave_object();
        } else if constexpr (detail::is_variant<U>::value) {
            d().enter_object();
            std::size_t idx = 0;
            d().member("index");
            process(idx);
            d().member("value");
            // Runtime index -> compile-time alternative: read the alternative that
            // matches the stored index and assign it into the variant.
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (((Is == idx) ? (void)([&] {
                     std::variant_alternative_t<Is, U> alt{};
                     process(alt);
                     v = std::move(alt);
                 }()) : (void)0), ...);
            }(std::make_index_sequence<std::variant_size_v<U>>{});
            d().leave_object();
        } else if constexpr (detail::is_forward_list<U>::value) {
            std::size_t n = d().begin_seq();
            v.clear();
            auto it = v.before_begin();
            for (std::size_t i = 0; i < n; ++i) {
                d().begin_elem();
                typename U::value_type e{};
                process(e);
                d().end_elem();
                it = v.insert_after(it, std::move(e));
            }
            d().end_seq();
        } else if constexpr (detail::is_pair<U>::value) {
            d().enter_object();
            d().member("first");
            process(const_cast<std::remove_const_t<typename U::first_type> &>(v.first));
            d().member("second");
            process(v.second);
            d().leave_object();
        } else if constexpr (detail::is_tuple<U>::value) {
            d().begin_seq();
            std::apply(
                [&](auto &...elems) {
                    ((d().begin_elem(), process(elems), d().end_elem()), ...);
                },
                v);
            d().end_seq();
        } else if constexpr (detail::is_std_array<U>::value) {
            d().begin_seq();
            for (auto &e : v) {
                d().begin_elem();
                process(e);
                d().end_elem();
            }
            d().end_seq();
        } else if constexpr (detail::is_map<U>::value) {
            std::size_t n = d().begin_seq();
            v.clear();
            for (std::size_t i = 0; i < n; ++i) {
                d().begin_elem();
                d().enter_object();
                typename U::key_type key{};
                typename U::mapped_type mapped{};
                d().member("key");
                process(key);
                d().member("value");
                process(mapped);
                d().leave_object();
                d().end_elem();
                v.emplace(std::move(key), std::move(mapped));
            }
            d().end_seq();
        } else if constexpr (detail::is_vector_bool_v<U>) {
            std::size_t n = d().begin_seq();
            v.clear();
            v.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                d().begin_elem();
                bool bit = false;
                process(bit);
                d().end_elem();
                v.push_back(bit);
            }
            d().end_seq();
        } else if constexpr (detail::is_sequence<U>::value) {
            std::size_t n = d().begin_seq();
            v.clear();
            for (std::size_t i = 0; i < n; ++i) {
                d().begin_elem();
                typename U::value_type elem{};
                process(elem);
                d().end_elem();
                if constexpr (detail::is_set_like_v<U>) {
                    v.insert(std::move(elem));
                } else {
                    v.push_back(std::move(elem));
                }
            }
            d().end_seq();
        } else if constexpr (detail::is_smart_ptr<U>::value) {
            if constexpr (detail::has_common_root<typename U::element_type>) {
                // POLYMORPHIC owning pointer -- see the matching note in GOArchiveT.
                gem_serialize_pointer(d(), v);
            } else {
                // NON-polymorphic owning pointer to a concrete type -- see GOArchiveT.
                d().enter_object();
                bool present = false;
                d().member("present");
                process(present);
                if (present) {
                    using Pointee = typename U::element_type;
                    // reset(ptr) serves unique_ptr and shared_ptr alike. A publicly
                    // default-constructible pointee is built inline; one whose default ctor is
                    // private (kept only for de-serialization) is built through the access shim
                    // it befriends -- the concrete-pointer analogue of the polymorphic factory seam.
                    if constexpr (std::is_default_constructible_v<Pointee>) {
                        v.reset(new Pointee());
                    } else {
                        v.reset(access::construct_raw<Pointee>());
                    }
                    d().member("value");
                    process(*v);
                } else {
                    v.reset();
                }
                d().leave_object();
            }
        } else {
            static_assert(detail::serializable_class<Derived, U>,
                          "GIArchiveT: type is neither a supported primitive/container/pointer nor a serializable "
                          "class (needs a serialize member reachable via Weft::access, or a non-intrusive free "
                          "gem_archive_serialize(Archive&, T&) in the type's namespace)");
            d().enter_object();
            if constexpr (detail::has_member_serialize<Derived, U>) {
                access::serialize(d(), v);
            } else {
                gem_archive_serialize(d(), v); // ADL: non-intrusive free serializer in U's namespace
            }
            d().leave_object();
        }
        return d();
    }
};

/******************************************************************************/

} // namespace Gem::Weft

/**
 * @brief Serializes a named member, deducing the name from the identifier.
 * The @c GArchive analogue of @c "ar & BOOST_SERIALIZATION_NVP(x)".
 */
#define GEM_NVP(member) ::Gem::Weft::make_nvp(#member, member)
