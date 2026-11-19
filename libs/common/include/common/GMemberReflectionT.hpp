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

// Standard headers go here
#include <atomic>
#include <tuple>
#include <type_traits>
#include <utility>

// Boost headers go here

// Gemfony headers go here
#include "common/GArchiveNamed.hpp" // archive_named / archive_named_base (the named member / base emitters)
#include "common/GCommonHelperFunctionsT.hpp"
#include "weft/GArchive.hpp"

namespace Gem::Common {

/******************************************************************************/
// archive_named / archive_named_base -- the named-member and named-base emitters for
// a plain member / a base slice -- live in the low-dependency GArchiveNamed.hpp
// (re-exported here via the include above) so hand-written serialize sites can pull
// them in without this whole reflection header.

/******************************************************************************/
/**
 * Single-source-of-truth machinery for a GCommonInterfaceT subclass's local data members.
 *
 * A class declares its local members exactly once through a localMembers() pair
 * (const + non-const) that returns a tuple of descriptors (a name plus a reference
 * to the member). serialize(), load_() and compare_() then all derive their
 * behaviour from that single declaration -- via serialize_members(),
 * g_load_members() and g_compare_members() -- instead of each function enumerating
 * the members separately. This removes the "added a member but forgot to update
 * load_()/compare_()" class of bugs.
 *
 * How a member participates is factored into THREE INDEPENDENT AXES, each chosen by
 * a small policy:
 *
 *  - serialize : ser_emit (through the archive by NVP) | ser_skip (omit);
 *  - load      : load_assign (plain =) | load_clone_ptr | load_clone_container
 *                (deep clone) | load_atomic (.store(.load()));
 *  - compare   : cmp_value (compare_t / getIdentity) | cmp_atomic (compare the
 *                loaded value) | cmp_skip (not part of comparable identity).
 *
 * A descriptor (member_desc) composes one policy per axis; the make_* factories
 * name the combinations actually in use (member_t, cloneable_member_t, atomic_member_t,
 * load_only_member_t, transient_member_t). Because the axes are orthogonal, a new
 * participation pattern is a new factory line -- NOT a new descriptor type carrying
 * a fresh set of g_serialize_one / g_load_one / g_compare_one overloads. The three
 * g_*_one dispatchers are each written once and forward to the relevant policy.
 *
 * This header holds the descriptors, the serialize and load policies (whose
 * dependencies live here) and the serialize/load derivations. The compare
 * derivation lives in GExpectationChecksT.hpp (it depends on the comparison DSL);
 * the compare axis is represented here by opaque tag types only, on which
 * g_compare_one() there dispatches.
 */

/******************************************************************************/
// --- serialize axis -------------------------------------------------------

/** @brief Serialize policy: emit the member through the archive under its NVP name. */
struct ser_emit {
    /**
     * @brief Serializes the member by NVP.
     * @tparam archive_type The GArchive codec type
     * @tparam T The referenced member type
     * @param ar The archive to serialize through
     * @param name The NVP tag (a static string literal with static storage duration)
     * @param ref The member reference serialized under @p name
     */
    template <typename archive_type, typename T>
    static void serialize(archive_type &ar, const char *name, T &ref) {
        Gem::Common::archive_named(ar, name, ref);
    }
};

/** @brief Serialize policy: skip the member (not part of the on-wire / on-disk form). */
struct ser_skip {
    /**
     * @brief Emits nothing: the member is excluded from the folded serialize().
     * @tparam archive_type The GArchive codec type
     * @tparam T The referenced member type
     */
    template <typename archive_type, typename T>
    static void serialize(archive_type & /*ar*/, const char * /*name*/, T & /*ref*/) { /* skipped */
    }
};

/**
 * @brief Serialize policy for a *base subobject*: emit it as boost base_object<base_type>.
 *
 * Unlike the other serialize policies, the descriptor's referenced value is the *derived* object itself
 * (not a data member), so base_object<base_type>(derived) both serialises the base_type slice and registers the
 * derived_type<->base_type void-cast Boost needs to (de)serialise the derived type through a base_type pointer. Use it
 * for a stateful, non-container base that cannot be tied in through a single data member.
 *
 * @tparam base_type The base class whose slice is serialised
 */
template <typename base_type>
struct ser_base_object {
    /**
     * @brief Serialises the base_type slice of @p derived under @p name.
     * @tparam archive_type The GArchive codec type
     * @tparam derived_type The (deduced) derived type carrying the base_type subobject
     * @param ar The archive to serialize through
     * @param name The NVP tag
     * @param derived The derived object whose base_type slice is emitted
     */
    template <typename archive_type, typename derived_type>
    static void serialize(archive_type &ar, const char *name, derived_type &derived) {
        Gem::Common::archive_named_base<base_type>(ar, name, derived);
    }
};

/******************************************************************************/
// --- load axis ------------------------------------------------------------

/** @brief Load policy: plain assignment (copies a scalar; SHARES a std::shared_ptr, not deep-cloned). */
struct load_assign {
    /**
     * @brief Assigns @p src to @p dst.
     * @tparam dst_type The destination member type
     * @tparam src_type The source member type
     * @param dst The destination member (written to)
     * @param src The source member (read from)
     */
    template <typename dst_type, typename src_type>
    static void load(dst_type &dst, const src_type &src) {
        dst = src;
    }
};

/** @brief Load policy: deep clone of a single std::shared_ptr<Cloneable> (no pointer aliasing). */
struct load_clone_ptr {
    /**
     * @brief Deep-clones the pointee of @p src into @p dst.
     * @tparam dst_type The destination std::shared_ptr<Cloneable> type
     * @tparam src_type The source std::shared_ptr<Cloneable> type
     * @param dst The destination pointer (written to)
     * @param src The source pointer (read from)
     */
    template <typename dst_type, typename src_type>
    static void load(dst_type &dst, const src_type &src) {
        Gem::Common::copyCloneableSmartPointer(src, dst);
    }
};

/** @brief Load policy: deep clone of every element of a container of std::shared_ptr<Cloneable>. */
struct load_clone_container {
    /**
     * @brief Deep-clones each element of @p src into @p dst.
     * @tparam dst_type The destination pointer-container type
     * @tparam src_type The source pointer-container type
     * @param dst The destination container (written to)
     * @param src The source container (read from)
     */
    template <typename dst_type, typename src_type>
    static void load(dst_type &dst, const src_type &src) {
        Gem::Common::copyCloneableSmartPointerContainer(src, dst);
    }
};

/** @brief Load policy: atomic load/store (a std::atomic is not copy-assignable). */
struct load_atomic {
    /**
     * @brief Loads the source atomic's value and stores it into the destination atomic.
     * @tparam dst_type The destination std::atomic<...> type
     * @tparam src_type The source std::atomic<...> type
     * @param dst The destination atomic (written to)
     * @param src The source atomic (read from)
     */
    template <typename dst_type, typename src_type>
    static void load(dst_type &dst, const src_type &src) {
        dst.store(src.load());
    }
};

/** @brief Load policy: deep copy of a smart-pointer pointee via its COPY CONSTRUCTOR (not clone()). */
struct load_copy_ptr {
    /**
     * @brief Copy-constructs a fresh pointee from @p src into @p dst (empty source -> empty destination).
     *
     * For a pointer to a plain value type that is copy-constructible but does NOT carry the Gemfony
     * clone()/load() interface (so load_clone_ptr does not apply). Works for unique_ptr and shared_ptr.
     *
     * @tparam dst_type The destination smart-pointer type
     * @tparam src_type The source smart-pointer type
     * @param dst The destination pointer (written to)
     * @param src The source pointer (read from)
     */
    template <typename dst_type, typename src_type>
    static void load(dst_type &dst, const src_type &src) {
        using element_t = typename dst_type::element_type;
        if(src) {
            dst = std::make_unique<element_t>(*src);
        }
        else {
            dst.reset();
        }
    }
};

/** @brief Load policy for a *base subobject*: copy-assign the base_type slice (its own operator=). */
template <typename base_type>
struct load_base_slice {
    /**
     * @brief Copy-assigns the base_type slice of @p src onto @p dst (base_type::operator=).
     *
     * The descriptor's referenced value is the derived object; this policy copies exactly the base_type slice,
     * mirroring the hand-written `base_type::operator=(other)` a folded load_() would otherwise perform. The
     * derived members are loaded by their own descriptors.
     *
     * @tparam derived_type The (deduced) derived type carrying the base_type subobject
     * @param dst The destination derived object (its base_type slice is written)
     * @param src The source derived object (its base_type slice is read)
     */
    template <typename derived_type>
    static void load(derived_type &dst, const derived_type &src) {
        static_cast<base_type &>(dst) = static_cast<const base_type &>(src);
    }
};

/******************************************************************************/
// --- compare axis (behaviour lives in GExpectationChecksT.hpp) -------------
// The compare operation depends on the comparison DSL (compare_t / getIdentity /
// GToken), declared in GExpectationChecksT.hpp -- a header that includes THIS one,
// not the reverse. So the compare axis is represented here by opaque tag types
// only; g_compare_one() there dispatches on the tag via if constexpr.

/** @brief Compare policy tag: compare the member's value via compare_t / getIdentity. */
struct cmp_value {};
/** @brief Compare policy tag: compare the LOADED value of a std::atomic member. */
struct cmp_atomic {};
/** @brief Compare policy tag: skip the member (not part of comparable identity). */
struct cmp_skip {};

/******************************************************************************/
/**
 * @brief A single named data member described for the single-source machinery.
 *
 * Carries the member's name (used as the serialization NVP tag) and a reference to
 * it, plus one policy per axis fixing how it is serialized, loaded and compared.
 * The name is a compile-time string literal (const char*) with static storage
 * duration, so the pointer handed to make_nvp is always valid.
 *
 * @tparam T    The type of the referenced member (const-qualified in a const context)
 * @tparam ser_type  The serialize policy (ser_emit / ser_skip)
 * @tparam load_type The load policy (load_assign / load_clone_ptr / load_clone_container / load_atomic)
 * @tparam cmp_type  The compare policy tag (cmp_value / cmp_atomic / cmp_skip)
 */
template <typename T, typename ser_type, typename load_type, typename cmp_type>
struct member_desc {
    const char *name;
    T &ref; // T& in a non-const context, T const& in a const context
};

/******************************************************************************/
// Named descriptor kinds, each a fixed policy composition. These aliases are the
// vocabulary the factories and call sites speak; member_desc keeps the three-axis
// behaviour explicit and total behind them.

/** @brief A plain member: serialized, plain-assigned on load, value-compared. */
template <typename T>
using member_t = member_desc<T, ser_emit, load_assign, cmp_value>;
/** @brief A single std::shared_ptr<Cloneable>: serialized, deep-cloned on load, value-compared. */
template <typename T>
using cloneable_member_t = member_desc<T, ser_emit, load_clone_ptr, cmp_value>;
/** @brief A container of std::shared_ptr<Cloneable>: serialized, element-wise deep-cloned on load, value-compared. */
template <typename T>
using cloneable_container_member_t = member_desc<T, ser_emit, load_clone_container, cmp_value>;
/** @brief A std::atomic member: serialized via the free atomic serialization, load/store on load, compared by its loaded value. */
template <typename T>
using atomic_member_t = member_desc<T, ser_emit, load_atomic, cmp_atomic>;
/** @brief A load-only member: skipped by BOTH serialize and compare, plain-assigned on load only. */
template <typename T>
using load_only_member_t = member_desc<T, ser_skip, load_assign, cmp_skip>;
/** @brief A transient member: skipped by serialize, plain-assigned on load, yet STILL value-compared. */
template <typename T>
using transient_member_t = member_desc<T, ser_skip, load_assign, cmp_value>;

/******************************************************************************/
// Factories. Each names one policy composition; a class lists ALL of its members
// -- of whatever kind -- through these in one localMembers() declaration, so
// serialize()/load_()/compare_() all derive from the same single source.

/**
 * @brief Builds one named plain member for a localMembers() tuple.
 * @tparam T The (deduced) type of the referenced member
 * @param name The member's name, used as the serialization NVP tag (a static string literal)
 * @param ref A reference to the data member being described
 * @return A member_t pairing @p name with @p ref
 */
template <typename T>
member_t<T> make_member(const char *name, T &ref) {
    return member_t<T>{name, ref};
}

/**
 * @brief Builds one named, deep-cloned single-pointer member for a localMembers() tuple.
 *
 * On load the pointee is deep-cloned via copyCloneableSmartPointer() rather than the shared_ptr
 * being shared, so no aliasing of shared state occurs.
 *
 * @tparam T The (deduced) type of the referenced std::shared_ptr<Cloneable> member
 * @param name The member's name, used as the serialization NVP tag (a static string literal)
 * @param ref A reference to the std::shared_ptr<Cloneable> data member
 * @return A cloneable_member_t pairing @p name with @p ref
 */
template <typename T>
cloneable_member_t<T> make_cloneable_member(const char *name, T &ref) {
    return cloneable_member_t<T>{name, ref};
}

/**
 * @brief Builds one named, deep-cloned pointer-container member for a localMembers() tuple.
 *
 * On load each element is deep-cloned via copyCloneableSmartPointerContainer().
 *
 * @tparam T The (deduced) type of the referenced pointer-container member
 * @param name The member's name, used as the serialization NVP tag (a static string literal)
 * @param ref A reference to the container of std::shared_ptr<Cloneable>
 * @return A cloneable_container_member_t pairing @p name with @p ref
 */
template <typename T>
cloneable_container_member_t<T> make_cloneable_container_member(const char *name, T &ref) {
    return cloneable_container_member_t<T>{name, ref};
}

/**
 * @brief Builds one named atomic member for a localMembers() tuple.
 *
 * Boost serialises std::atomic<bool> via a free serialization, so it is emitted like any other member;
 * but an atomic is neither copy-assignable nor directly comparable, so it is loaded via .store(.load())
 * and compared by its loaded value.
 *
 * @tparam T The (deduced) type of the referenced std::atomic<...> member
 * @param name The member's name, used as the serialization NVP tag (a static string literal)
 * @param ref A reference to the std::atomic<...> data member
 * @return An atomic_member_t pairing @p name with @p ref
 */
template <typename T>
atomic_member_t<T> make_atomic_member(const char *name, T &ref) {
    return atomic_member_t<T>{name, ref};
}

/**
 * @brief Builds one named load-only member for a localMembers() tuple.
 *
 * The member is plain-assigned on load (sharing a std::shared_ptr, copying a scalar) but skipped by
 * BOTH serialize_members() and g_compare_members(). Use it for a member that is copied when one object
 * is loaded from another, yet is neither part of the object's comparable identity nor emitted through
 * the folded serialize():
 *
 *  - a shared, immutable piece of metadata that several objects reference (identity lives in the
 *    per-object values, not in the shared handle), and whose wire representation, if any, the owning
 *    class emits through its OWN hand-written serialize() (e.g. GGenome's structural layout, which
 *    travels via a bespoke send-once-by-content-id protocol);
 *  - a transient marker set during (de)serialization that a load_()-based copy must propagate but
 *    that must not affect equality (e.g. GGenome's shared structural layout).
 *
 * @tparam T The (deduced) type of the referenced member
 * @param name The member's name (kept for symmetry / diagnostics; not emitted, as the member is not serialized here)
 * @param ref A reference to the data member being described
 * @return A load_only_member_t pairing @p name with @p ref
 */
template <typename T>
load_only_member_t<T> make_load_only_member(const char *name, T &ref) {
    return load_only_member_t<T>{name, ref};
}

/**
 * @brief Builds one named transient member for a localMembers() tuple.
 *
 * A transient member is copied when one object is loaded from another and IS part of the object's
 * comparable identity, yet is deliberately NOT serialized -- it is per-run / per-iteration state that
 * must never be persisted to a checkpoint or sent over the wire, but that two live objects are only
 * equal if they agree on (e.g. an optimization algorithm's transient best-of-this-iteration priority
 * queue). It differs from a load-only member only on the compare axis: load-only skips comparison,
 * transient participates in it.
 *
 * The member is plain-assigned on load, relying on its own type's operator= (which, for a value-type
 * holding cloneable pointers, performs the appropriate deep copy).
 *
 * @tparam T The (deduced) type of the referenced member
 * @param name The member's name (used as the comparison label; not emitted, as the member is not serialized)
 * @param ref A reference to the data member being described
 * @return A transient_member_t pairing @p name with @p ref
 */
template <typename T>
transient_member_t<T> make_transient_member(const char *name, T &ref) {
    return transient_member_t<T>{name, ref};
}

/**
 * @brief Builds a serialized-but-not-compared member (plain assignment on load).
 *
 * For state that travels on the wire / to disk and is copied when one object is loaded from another, yet
 * is deliberately NOT part of the object's comparable identity (e.g. a shared 1:N configuration pointer,
 * or a result store). It is serialized (ser_emit), plain-assigned on load (sharing a shared_ptr), and
 * skipped by compare.
 *
 * @tparam T The (deduced) type of the referenced member
 * @param name The member's serialization NVP tag
 * @param ref A reference to the data member
 * @return A member_desc composing ser_emit / load_assign / cmp_skip
 */
template <typename T>
member_desc<T, ser_emit, load_assign, cmp_skip> make_uncompared_member(const char *name, T &ref) {
    return member_desc<T, ser_emit, load_assign, cmp_skip>{name, ref};
}

/**
 * @brief Builds a serialized-but-not-compared, deep-cloned single-pointer member.
 *
 * As make_uncompared_member, but the std::shared_ptr<Cloneable> pointee is deep-cloned on load (via
 * copyCloneableSmartPointer) rather than shared.
 *
 * @tparam T The (deduced) type of the referenced std::shared_ptr<Cloneable> member
 * @param name The member's serialization NVP tag
 * @param ref A reference to the data member
 * @return A member_desc composing ser_emit / load_clone_ptr / cmp_skip
 */
template <typename T>
member_desc<T, ser_emit, load_clone_ptr, cmp_skip>
make_uncompared_cloneable_member(const char *name, T &ref) {
    return member_desc<T, ser_emit, load_clone_ptr, cmp_skip>{name, ref};
}

/**
 * @brief Builds a descriptor for a stateful, non-container BASE subobject.
 *
 * The base is serialised as boost base_object<base_type> (which also registers the derived_type<->base_type void-cast),
 * copy-assigned on load via base_type::operator= (its own value semantics), and excluded from comparison
 * (compare the base's state separately if it is part of identity -- for the cases folded so far it is
 * not). The referenced value is the derived object itself, so pass `self`. This is the general tool for
 * a stateful base that cannot be tied in through a single data member (unlike a container base, whose
 * lone data_cnt_ is tied in with make_cloneable_container_member instead).
 *
 * @tparam base_type The base class whose slice is carried
 * @tparam self_type The (deduced) derived (const or non-const) type of *this
 * @param name The base's serialization NVP tag
 * @param self A reference to the derived object (*this)
 * @return A member_desc composing ser_base_object<base_type> / load_base_slice<base_type> / cmp_skip
 */
template <typename base_type, typename self_type>
member_desc<self_type, ser_base_object<base_type>, load_base_slice<base_type>, cmp_skip>
make_base_object_member(const char *name, self_type &self) {
    return member_desc<self_type, ser_base_object<base_type>, load_base_slice<base_type>, cmp_skip>{
        name,
        self
    };
}

/******************************************************************************/
/**
 * @brief Loads a single member by applying its descriptor's load policy.
 *
 * The destination and source descriptors carry the same three policies (they come from the same
 * localMembers() declaration), but the referenced type differs -- the destination tuple is built in a
 * non-const context (dst_type) and the source tuple in a const one (src_type) -- so the two are independent
 * template parameters.
 *
 * @tparam dst_type The destination member type
 * @tparam src_type The source member type
 * @tparam ser_type The (shared) serialize policy
 * @tparam load_type The (shared) load policy, whose load_type::load() performs the copy
 * @tparam cmp_type The (shared) compare policy tag
 * @param dst The destination member descriptor (written to)
 * @param src The source member descriptor (read from)
 */
template <
    typename dst_type,
    typename src_type,
    typename ser_type,
    typename load_type,
    typename cmp_type>
void g_load_one(
    member_desc<dst_type, ser_type, load_type, cmp_type> &dst,
    const member_desc<src_type, ser_type, load_type, cmp_type> &src
) {
    load_type::load(dst.ref, src.ref);
}

/**
 * @brief Loads each member of the two tuples pairwise (implementation helper).
 * @tparam dst_tuple_type The destination localMembers() tuple type
 * @tparam src_tuple_type The source localMembers() tuple type
 * @tparam I The compile-time member indices expanded by the fold
 * @param dst The destination member tuple (written to)
 * @param src The source member tuple (read from)
 * @param seq Index sequence used to expand the pack; its value is unused
 */
template <typename dst_tuple_type, typename src_tuple_type, std::size_t... I>
void g_load_members_impl(
    dst_tuple_type &dst,
    const src_tuple_type &src,
    [[maybe_unused]] std::index_sequence<I...> seq
) {
    (g_load_one(std::get<I>(dst), std::get<I>(src)), ...);
}

/**
 * @brief Copies each local member from src to dst, member by member.
 * @tparam dst_tuple_type The destination localMembers() tuple type
 * @tparam src_tuple_type The source localMembers() tuple type
 * @param dst The destination member tuple (taken by value; its references write through)
 * @param src The source member tuple (taken by value; its references read through)
 */
template <typename dst_tuple_type, typename src_tuple_type>
void g_load_members(dst_tuple_type dst, src_tuple_type src) {
    static_assert(
        std::tuple_size_v<dst_tuple_type> == std::tuple_size_v<src_tuple_type>,
        "g_load_members: localMembers() arity mismatch"
    );
    g_load_members_impl(dst, src, std::make_index_sequence<std::tuple_size_v<dst_tuple_type>>{});
}

/******************************************************************************/
/**
 * @brief Serializes one member descriptor by applying its serialize policy.
 *
 * ser_emit puts the member through the archive under its name; ser_skip emits nothing. This lets a
 * class's serialize() derive its member list from the same single localMembers() declaration that
 * load_() and compare_() use.
 *
 * @tparam archive_type The GArchive codec type
 * @tparam T The referenced member type
 * @tparam ser_type The serialize policy applied
 * @tparam load_type The (unused here) load policy
 * @tparam cmp_type The (unused here) compare policy tag
 * @param ar The archive to serialize through
 * @param m The member descriptor whose .ref is (or is not) emitted under its .name
 */
template <
    typename archive_type,
    typename T,
    typename ser_type,
    typename load_type,
    typename cmp_type>
void g_serialize_one(archive_type &ar, member_desc<T, ser_type, load_type, cmp_type> &m) {
    ser_type::serialize(ar, m.name, m.ref);
}

/**
 * @brief Serializes each entry of a localMembers() tuple (implementation helper).
 * @tparam archive_type The GArchive codec type
 * @tparam tuple_type The localMembers() tuple type
 * @tparam I The compile-time member indices expanded by the fold
 * @param ar The archive to serialize through
 * @param members The localMembers() tuple whose entries are serialized
 * @param seq Index sequence used to expand the pack; its value is unused
 */
template <typename archive_type, typename tuple_type, std::size_t... I>
void serialize_members_impl(
    archive_type &ar,
    tuple_type &members,
    [[maybe_unused]] std::index_sequence<I...> seq
) {
    (g_serialize_one(ar, std::get<I>(members)), ...);
}

/**
 * @brief Serializes every entry of a localMembers() tuple through the archive, name by name.
 * @tparam archive_type The GArchive codec type
 * @tparam tuple_type The localMembers() tuple type
 * @param ar The archive to serialize through
 * @param members The localMembers() tuple whose entries are serialized (taken by value)
 */
template <typename archive_type, typename tuple_type>
void serialize_members(archive_type &ar, tuple_type members) {
    serialize_members_impl(ar, members, std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}

/******************************************************************************/

} /* namespace Gem::Common */
