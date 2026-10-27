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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "weft/GArchive.hpp"
#include "weft/GBinaryArchive.hpp"
#include "weft/GWeftError.hpp"
#include "weft/GJsonArchive.hpp"
#include "weft/GPolymorphicRegistry.hpp"
// NOTE: deliberately does NOT include GReflectiveInterfaceT.hpp. The private-ctor construction seam
// is reached through the forward-declared Weft::gem_registry_construct_reflective (declared in
// GArchive.hpp, defined in GReflectiveInterfaceT.hpp), so this header stays free of the reflection
// layer -- otherwise GArchivePolymorphic -> GReflectiveInterfaceT -> GCommonInterfaceT would be a
// cycle, blocking the codec/dispatch layer from being pulled into GCommonInterfaceT (the toStream
// codec arm). Registration sites instantiate register_archivable and include the type's header
// (hence the seam's definition).

namespace Gem::Weft {

/******************************************************************************/
/**
 * @file GArchivePolymorphic.hpp
 * @brief Polymorphic-pointer dispatch for the @c GArchive family -- the codec
 * layer that lets a @c GArchive (de)serialize a @c shared_ptr / @c unique_ptr to
 * a hierarchy root, reconstructing the exact dynamic type on load.
 *
 * @par Layering (why this is a separate header)
 * The value-codec base (@c GArchive.hpp) owns value dispatch and must stay free
 * of the registry and the codecs, or an include cycle forms
 * (@c GArchive -> registry -> codecs -> @c GArchive). Pointer dispatch instead
 * lives here, one layer up, where the registry (identity) and the concrete
 * codecs are both fully visible. The base reaches this layer only through the
 * @c gem_serialize_pointer customization point, found by ADL at instantiation --
 * so a translation unit that serializes a pointer must include this header (the
 * @c GEM_REGISTER_ARCHIVABLE sites and the serialization choke points do).
 *
 * @par Mechanism (registry-thunk model)
 * Identity is the codec-agnostic @c GPolymorphicRegistry (tag <-> factory).
 * Dispatch is @ref GArchivePointerDispatch, a per-hierarchy table mapping a tag
 * to four function-pointer thunks (one per concrete archive type) that each call
 * the concrete type's @c serialize through @c Weft::access. A single
 * mechanism -- the registry -- carries identity; this table carries the typed
 * entry points a template @c serialize cannot expose as a virtual. Both are
 * populated together by @ref GEM_REGISTER_ARCHIVABLE.
 */
/******************************************************************************/

namespace detail {

/**
 * @brief A type-erased per-hierarchy completeness checker: it appends to @p gaps a
 * human-readable line for every wire tag that is registered for identity but has no
 * archive-dispatch thunk. One is registered per @c Root the first time that Root's
 * dispatch table is touched, so @ref archiveRegistrationGaps can enumerate all
 * hierarchies without a central list of roots.
 */
using root_check_fn = void (*)(std::vector<std::string> &gaps);

/**
 * @brief Guards the process-wide checker list.
 *
 * Invariant 2 exception, recorded deliberately: this is a plain mutex over a leaked
 * process-wide registry, not a concurrency primitive that common/concurrency should
 * supply. The registry exists precisely to be reachable from static initializers and
 * from plugin-time registration, so coupling it to a facility with its own static
 * lifetime would recreate the initialization-order hazard it is built to defeat.
 */
inline std::mutex &root_check_mutex() {
    static std::mutex m;
    return m;
}

/**
 * @brief The process-wide list of per-Root completeness checkers. Leaked (like the
 * registries themselves) so cross-TU / plugin-time registration never touches a
 * torn-down container at static-destruction time.
 */
inline std::vector<root_check_fn> &root_checks() {
    static auto *const v = new std::vector<root_check_fn>();
    return *v;
}

/** @brief Appends a per-Root completeness checker (idempotence is the caller's concern). */
inline void register_root_check(root_check_fn fn) {
    const std::scoped_lock lock{root_check_mutex()};
    root_checks().push_back(fn);
}

} // namespace detail

/**
 * @brief Per-hierarchy table of typed serialize thunks: for each registered
 * concrete type (keyed by its wire tag) it stores one @c void(*)(Root&, Archive&)
 * per concrete archive type, each invoking that type's @c serialize.
 * @tparam Root The category root of the hierarchy.
 */
template <typename Root>
class GArchivePointerDispatch {
public:
    struct entry_t {
        void (*bin_o)(Root &, GBinaryOArchive &);
        void (*bin_i)(Root &, GBinaryIArchive &);
        void (*json_o)(Root &, GJsonOArchive &);
        void (*json_i)(Root &, GJsonIArchive &);
    };

    /**
     * @brief Installs the four serialize thunks for concrete type @p Derived
     * under wire tag @p tag. Idempotent for a repeated identical registration.
     * @tparam Derived The concrete type (must derive from @p Root and be reachable via @c Weft::access).
     * @param tag The wire tag (the same one used for the identity registry).
     */
    template <typename Derived>
    static void add(std::string_view tag) {
        static_assert(std::is_base_of_v<Root, Derived>,
                      "GArchivePointerDispatch::add<Derived>: Derived must derive from Root");
        entry_t e{
            +[](Root &r, GBinaryOArchive &ar) { access::serialize(ar, static_cast<Derived &>(r)); },
            +[](Root &r, GBinaryIArchive &ar) { access::serialize(ar, static_cast<Derived &>(r)); },
            +[](Root &r, GJsonOArchive &ar) { access::serialize(ar, static_cast<Derived &>(r)); },
            +[](Root &r, GJsonIArchive &ar) { access::serialize(ar, static_cast<Derived &>(r)); },
        };
        const std::scoped_lock lock{instance().mutex_};
        instance().map_.insert_or_assign(std::string{tag}, e);
    }

    /**
     * @brief Invokes the thunk for @p tag matching the concrete archive type of
     * @p ar, serializing the @p Root slice-and-below of @p r as its dynamic type.
     * @tparam Archive The concrete archive type (selects the thunk field).
     * @param tag The wire tag identifying the dynamic type.
     * @param r The object, upcast to @p Root.
     * @param ar The archive.
     * @throws weft_exception if @p tag has no dispatch entry.
     */
    template <typename Archive>
    static void invoke(std::string_view tag, Root &r, Archive &ar) {
        const entry_t &e = lookup(tag);
        if constexpr (std::is_same_v<Archive, GBinaryOArchive>) {
            e.bin_o(r, ar);
        } else if constexpr (std::is_same_v<Archive, GBinaryIArchive>) {
            e.bin_i(r, ar);
        } else if constexpr (std::is_same_v<Archive, GJsonOArchive>) {
            e.json_o(r, ar);
        } else if constexpr (std::is_same_v<Archive, GJsonIArchive>) {
            e.json_i(r, ar);
        } else {
            static_assert(sizeof(Archive) == 0, "GArchivePointerDispatch::invoke: unknown archive type");
        }
    }

    /** @brief Whether a dispatch thunk is registered under @p tag. @param tag The wire tag to probe. */
    static bool contains(std::string_view tag) {
        const std::scoped_lock lock{instance().mutex_};
        return instance().map_.contains(std::string{tag});
    }

    /**
     * @brief The completeness checker for this hierarchy: appends to @p gaps every
     * tag that the identity @c GPolymorphicRegistry<Root> holds but this dispatch
     * table lacks -- i.e. a type registered identity-only (@c GEM_REGISTER_TYPE)
     * where @c GEM_REGISTER_ARCHIVABLE was needed. Registered once per Root (see
     * @ref instance) so @ref archiveRegistrationGaps can reach it.
     * @param gaps The accumulator to which this hierarchy's gaps are appended.
     */
    static void collectGaps(std::vector<std::string> &gaps) {
        for (const auto &tag : GPolymorphicRegistry<Root>::tags()) {
            if (!contains(tag)) {
                gaps.push_back(
                    "wire tag \"" + tag +
                    "\" is in the identity registry but has no GArchive dispatch thunk "
                    "(registered identity-only via GEM_REGISTER_TYPE instead of GEM_REGISTER_ARCHIVABLE)");
            }
        }
    }

private:
    static const entry_t &lookup(std::string_view tag) {
        const std::scoped_lock lock{instance().mutex_};
        auto it = instance().map_.find(std::string{tag});
        if (it == instance().map_.end()) {
            throw weft_exception(
                weft_error_streamer()
                << "In GArchivePointerDispatch::lookup(): no serialize thunk is registered under tag \"" << tag
                << "\" (its type is in the identity registry but was not registered for archive dispatch -- "
                << "use GEM_REGISTER_ARCHIVABLE, not the identity-only GEM_REGISTER_TYPE)." << '\n'
            );
        }
        return it->second;
    }

    GArchivePointerDispatch() = default;

    static GArchivePointerDispatch &instance() {
        // Leaked by design (see GPolymorphicRegistry). The first time this Root's table is
        // created, register its completeness checker so the boot-time self-check can reach it.
        static GArchivePointerDispatch *const inst = [] {
            auto *p = new GArchivePointerDispatch();
            detail::register_root_check(&GArchivePointerDispatch::collectGaps);
            return p;
        }();
        return *inst;
    }

    /** @brief Guards @c map_. Invariant 2 exception for the same reason as
     *  @ref detail::root_check_mutex(): a leaked, static-initialization-safe registry. */
    std::mutex mutex_;
    std::unordered_map<std::string, entry_t> map_;
};

/******************************************************************************/
/**
 * @brief The pointer-dispatch customization point the value-codec base calls by
 * ADL for a @c shared_ptr / @c unique_ptr member.
 *
 * On save: writes a present flag; if non-null, writes the dynamic type's wire
 * tag then serializes its members (as a nested object). On load: reads the
 * flag; if present, reads the tag, constructs the dynamic type from the identity
 * registry, loads its members, and adopts it into @p p. A null pointer is a
 * single false flag. There is no object tracking -- Geneva serializes trees.
 *
 * @tparam Archive The concrete archive type (its @c is_saving fixes the direction).
 * @tparam SmartPtr A @c shared_ptr or @c unique_ptr whose element type exposes @c gemfony_common_root_t.
 * @param ar The archive.
 * @param p The pointer to serialize or fill.
 */
template <typename Archive, typename SmartPtr>
void gem_serialize_pointer(Archive &ar, SmartPtr &p) {
    using T = typename SmartPtr::element_type;
    static_assert(requires { typename T::gemfony_common_root_t; },
                  "gem_serialize_pointer: the pointer's element type must be a GCommonInterfaceT hierarchy type "
                  "(exposing gemfony_common_root_t) to be dispatched polymorphically");
    using Root = typename T::gemfony_common_root_t;

    // The pointer serializes as ONE nested object {present[, tag, data]} so that,
    // in a self-describing codec, the several fields occupy a single member slot
    // rather than overwriting each other. In the binary codec the object/member
    // framing is free (no-ops) and this is just: flag [tag data].
    if constexpr (Archive::is_saving) {
        ar.begin_object();
        bool present = static_cast<bool>(p);
        ar.member("present");
        ar &present;
        if (present) {
            std::string tag = GPolymorphicRegistry<Root>::tagOf(static_cast<const Root &>(*p));
            ar.member("tag");
            ar &tag;
            ar.member("data");
            ar.begin_object();
            GArchivePointerDispatch<Root>::invoke(tag, static_cast<Root &>(*p), ar);
            ar.end_object();
        }
        ar.end_object();
    } else {
        ar.enter_object();
        bool present = false;
        ar.member("present");
        ar &present;
        if (!present) {
            p.reset();
            ar.leave_object();
            return;
        }
        std::string tag;
        ar.member("tag");
        ar &tag;
        std::unique_ptr<Root> obj = GPolymorphicRegistry<Root>::create(tag);
        ar.member("data");
        ar.enter_object();
        GArchivePointerDispatch<Root>::invoke(tag, *obj, ar);
        ar.leave_object();
        ar.leave_object();
        // The constructed object is dynamically a T-or-derived; downcast Root* -> T*
        // and adopt it into the requested smart-pointer flavour.
        T *raw = static_cast<T *>(obj.release());
        p = SmartPtr(raw);
    }
}

/******************************************************************************/
/**
 * @brief Registers concrete type @p T for @b both identity (the tag<->factory
 * @c GPolymorphicRegistry) and @c GArchive dispatch (the serialize thunks), the
 * full-registration counterpart of a @c BOOST_CLASS_EXPORT_IMPLEMENT. Prefer this
 * over the identity-only @c GEM_REGISTER_TYPE for any type that travels through a
 * @c GArchive as a polymorphic pointer.
 * @tparam T The concrete type to register.
 * @param tag The wire tag (fully-qualified stringized type).
 * @return @c true (a value to seed a namespace-scope registration variable).
 */
template <typename T>
inline bool register_archivable(std::string_view tag) {
    using Root = typename T::gemfony_common_root_t;
    if constexpr (std::is_default_constructible_v<T>) {
        // Public default constructor: the registry's own std::make_unique factory reaches it.
        GPolymorphicRegistry<Root>::template reg<T>(tag);
    } else {
        // Private default constructor (constructed only on load, through a friend, exactly as
        // Boost does): route the factory through GReflectiveInterfaceAccess -- the shim every
        // mixin-managed class befriends -- so the reconstruction has ctor access the (non-friend)
        // registration site lacks.
        GPolymorphicRegistry<Root>::reg(
            tag, std::type_index(typeid(T)),
            +[]() -> std::unique_ptr<Root> { return gem_registry_construct_reflective<T>(); });
    }
    GArchivePointerDispatch<Root>::template add<T>(tag);
    return true;
}

/******************************************************************************/
/**
 * @brief Enumerates the GArchive polymorphic-registration gaps across every
 * hierarchy touched this process: a line per wire tag that is identity-registered
 * but has no archive-dispatch thunk (an empty result means every registered type
 * is fully archive-dispatchable).
 *
 * This is the enumerable counterpart of Boost.Serialization's compiler-invisible
 * @c void_cast graph: because registration is explicit (@ref GPolymorphicRegistry
 * for identity, @ref GArchivePointerDispatch for dispatch), the two sides can be
 * compared directly. It reaches every hierarchy through the per-Root checkers that
 * each @ref GArchivePointerDispatch registers on first use -- so it sees exactly
 * the roots that have at least one archivable type (a hierarchy registered
 * @e entirely identity-only would have no dispatch table and would instead fail
 * loudly at the first deserialization, per @c GArchivePointerDispatch::lookup).
 *
 * @return A (possibly empty) list of human-readable gap descriptions.
 */
inline std::vector<std::string> archiveRegistrationGaps() {
    std::vector<std::string> gaps;
    const std::scoped_lock lock{detail::root_check_mutex()};
    for (const auto fn : detail::root_checks()) {
        fn(gaps);
    }
    return gaps;
}

/**
 * @brief The number of hierarchies (category roots) that have at least one type
 * registered for GArchive dispatch -- for a boot-time diagnostic log line.
 * @return The count of registered hierarchies.
 */
inline std::size_t archiveRegisteredHierarchyCount() {
    const std::scoped_lock lock{detail::root_check_mutex()};
    return detail::root_checks().size();
}

/**
 * @brief Boot-time completeness self-check: throws if any type is identity-registered
 * but not archive-dispatchable, turning what would otherwise be a runtime "unknown
 * tag" at the first deserialization into a loud failure at startup. Cheap (a set
 * comparison per hierarchy) and idempotent, so it is safe to call once at process
 * or consumer init.
 * @throws weft_exception listing every gap, if @ref archiveRegistrationGaps is non-empty.
 */
inline void verifyArchiveRegistrations() {
    const auto gaps = archiveRegistrationGaps();
    if (gaps.empty()) {
        return;
    }
    std::string detail_msg;
    for (const auto &g : gaps) {
        detail_msg += "  - " + g + "\n";
    }
    throw weft_exception(
        weft_error_streamer()
        << "In verifyArchiveRegistrations(): the GArchive polymorphic registration is incomplete -- "
        << gaps.size() << " type(s) are identity-registered but not archive-dispatchable:\n"
        << detail_msg);
}

} // namespace Gem::Weft

/**
 * @brief Registers a concrete polymorphic type for identity + @c GArchive
 * dispatch (see @ref Gem::Weft::register_archivable). Place exactly
 * one at namespace scope in a single translation unit per type. Variadic so a
 * template instantiation with comma-separated arguments passes through intact.
 * @param ... The fully-qualified concrete type.
 */
#define GEM_REGISTER_ARCHIVABLE(...)                                                       \
    namespace {                                                                            \
    const bool GEM_REGISTRY_CAT(gem_archivable_tag_, __COUNTER__) =                        \
        ::Gem::Weft::register_archivable<__VA_ARGS__>(#__VA_ARGS__);            \
    }
