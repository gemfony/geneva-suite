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
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "weft/GWeftError.hpp"

namespace Gem::Weft {

/******************************************************************************/
/**
 * @brief A per-hierarchy "wire tag <-> concrete-type factory" registry, the
 * polymorphic-dispatch substrate for the forthcoming @c GArchive.
 *
 * @par What it replaces
 * Boost.Serialization dispatches a polymorphic pointer by writing a GUID string
 * (from @c BOOST_CLASS_EXPORT) into the archive and, on read, walking a global
 * @c void_cast edge graph to reconstruct the object. That graph is
 * compiler-invisible: a missing @c BOOST_CLASS_EXPORT_IMPLEMENT is not a
 * compile error, it is a runtime "unregistered void cast" at deserialization.
 * This registry replaces that machinery with an explicit, enumerable map:
 * a stable @b tag string identifies a concrete type, and a factory reconstructs
 * it. Writing a polymorphic pointer emits @c tagOf(obj); reading looks the tag
 * up and calls the factory. There is no hidden global graph and no
 * @c void_cast -- a missing registration fails loudly at the read (unknown tag)
 * or, better, at a boot-time completeness self-check (a later step).
 *
 * @par One registry per category root
 * The template parameter @p Root is the category root of a hierarchy (the type a
 * class passes to @c GCommonInterfaceT, recoverable from any derivative as
 * @c gemfony_common_root_t). Each @c GPolymorphicRegistry<Root> is an
 * independent map, so tags need only be unique @e within a hierarchy, and the
 * factory can hand back a @c std::unique_ptr<Root>. The @ref GEM_REGISTER_TYPE
 * macro auto-routes a concrete type @c T to @c GPolymorphicRegistry<T::gemfony_common_root_t>.
 *
 * @par The tag is decoupled from @c class_name
 * The tag is supplied by the registration site (the fully-qualified, stringized
 * type name -- see @ref GEM_REGISTER_TYPE), @b not read from the type's
 * @c class_name. @c class_name is overloaded as a config @e filename
 * (@c ./config/<class_name>.json), so it is constrained to a filename-safe
 * alphabet; a wire tag by contrast wants a globally-unique, namespace- and
 * template-argument-bearing string (@c "Gem::Geneva::GMetaOptimizerIndividualT<Gem::Geneva::GFunctionIndividual>"),
 * every one of whose @c :: @c < @c > @c , characters is filename-hostile. The
 * registry key is only ever a map key, never a path, so it is free to be fully
 * qualified -- and per-instantiation-unique for templates for free (the
 * registration site names the concrete @c T).
 *
 * @par Lifetime (important)
 * Registration happens during dynamic initialisation across many translation
 * units, and Geneva can additionally register types at runtime when a loadable
 * OA/individual plugin is @c dlopen 'd. The singleton is therefore heap-allocated
 * and @b never destroyed (leaked deliberately): a function-local @c static of
 * object type would be torn down at process exit in an order unrelated to the
 * registering TUs, and a late registration (or a late serialization on a worker
 * thread) could then touch freed memory. A leaked singleton sidesteps all
 * static-destruction-order hazards. Its maps are mutex-guarded so a plugin load
 * on one thread cannot race a lookup on another.
 *
 * @par Cross-module singleton (the pluggable-OA / pluggable-individual guarantee)
 * A loadable OA/individual plugin registers its concrete types (@ref GEM_REGISTER_TYPE /
 * @c GEM_REGISTER_ARCHIVABLE) from the plugin's own @c .so, exactly as it carries a
 * @c BOOST_CLASS_EXPORT today. For that registration to be visible to the host that
 * serializes the type, the plugin and the host must share @b one @ref instance(). Two
 * project-wide conditions make that so, and both must hold:
 *   - @c GModuleLoader @c dlopen's every plugin @c RTLD_GLOBAL @c | @c RTLD_NOW, so the
 *     plugin's references to this template's vague-linkage singleton resolve against the
 *     definition already loaded from @c libgemfony-common (RTLD_NOW forces the binding,
 *     and the registering static initializer, at plugin load);
 *   - Geneva builds with default ELF symbol visibility (no @c -fvisibility=hidden), so the
 *     singleton's guard/state symbols are interposable weak symbols rather than
 *     module-private ones.
 * This is the same interposition Boost.Serialization's own singleton relies on. If a future
 * build ever adopts hidden visibility, this singleton (and @c GArchivePointerDispatch's) must
 * be marked default-visible (as Boost marks its registry @c BOOST_SYMBOL_VISIBLE), or a
 * plugin's registrations would land in a per-module copy and fail as "unknown tag" at the read.
 *
 * @tparam Root The category root of the hierarchy this registry serves.
 */
template <typename Root>
class GPolymorphicRegistry {
public:
    /** @brief A capture-less factory reconstructing a default @p Root derivative on the heap. */
    using factory_t = std::unique_ptr<Root> (*)();

    /******************************************************************************/
    /**
     * @brief Registers concrete type @p Derived under wire tag @p tag, using
     * @c std::make_unique<Derived> as the factory.
     *
     * Requires @p Derived to be a @p Root derivative with a default constructor
     * accessible at the call site. For a type whose default constructor is
     * private (constructed on load through a friend), register via the explicit
     * @ref reg(std::string_view,std::type_index,factory_t) overload with a
     * factory that has the necessary access.
     *
     * Registration is @b idempotent for an identical (type, tag) pair -- so an
     * accidental placement of @ref GEM_REGISTER_TYPE in a header, re-run by every
     * including TU, is harmless rather than a static-init crash. It throws a
     * @c weft_exception if @p tag is already bound to a @e different type
     * (a tag collision), or if @p Derived was already registered under a
     * @e different tag (a type must have one stable tag).
     *
     * @tparam Derived The concrete type to register.
     * @param tag The stable wire tag identifying @p Derived within this hierarchy.
     * @return @c true on first registration, @c false if this exact (type, tag) pair was already present.
     */
    template <typename Derived>
    static bool reg(std::string_view tag) {
        static_assert(std::is_base_of_v<Root, Derived>,
                      "GPolymorphicRegistry::reg<Derived>: Derived must derive from Root");
        static_assert(std::is_default_constructible_v<Derived>,
                      "GPolymorphicRegistry::reg<Derived>: Derived needs an accessible default constructor; "
                      "for a private-ctor type register via the explicit (tag, type_index, factory) overload");
        return reg(tag, std::type_index(typeid(Derived)),
                   +[]() -> std::unique_ptr<Root> { return std::make_unique<Derived>(); });
    }

    /******************************************************************************/
    /**
     * @brief Registers a concrete type identified by @p ti under wire tag @p tag
     * with an explicit @p factory.
     *
     * This is the seam for a type whose default constructor is not accessible at
     * the registration site: the caller supplies a factory that does have access
     * (e.g. one routed through @c GReflectiveInterfaceAccess). Idempotency and
     * collision rules are as for the templated overload.
     *
     * @param tag The stable wire tag.
     * @param ti The @c std::type_index of the concrete type (its dynamic type on the wire).
     * @param factory A factory reconstructing a default instance on the heap.
     * @return @c true on first registration, @c false if this exact (type, tag) pair was already present.
     */
    static bool reg(std::string_view tag, std::type_index ti, factory_t factory) {
        std::string key{tag};
        const std::scoped_lock lock{instance().mutex_};
        auto &tag_to_entry = instance().tag_to_entry_;
        auto &type_to_tag = instance().type_to_tag_;

        if (auto it = tag_to_entry.find(key); it != tag_to_entry.end()) {
            if (it->second.type == ti) {
                return false; // identical (type, tag) -- idempotent
            }
            throw weft_exception(
                weft_error_streamer()
                << "In GPolymorphicRegistry::reg(): tag \"" << key << "\" is already bound to a different type "
                << "(existing type_index differs from the one being registered). Two types must not share a wire tag." << '\n'
            );
        }
        if (auto rit = type_to_tag.find(ti); rit != type_to_tag.end()) {
            throw weft_exception(
                weft_error_streamer()
                << "In GPolymorphicRegistry::reg(): the type being registered under tag \"" << key
                << "\" is already registered under a different tag \"" << rit->second
                << "\". A type must have exactly one stable wire tag." << '\n'
            );
        }
        tag_to_entry.emplace(std::move(key), entry_t{factory, ti});
        type_to_tag.emplace(ti, std::string{tag});
        return true;
    }

    /******************************************************************************/
    /**
     * @brief Reconstructs a default instance of the type registered under @p tag.
     * @param tag The wire tag read from the archive.
     * @return An owning pointer to a freshly default-constructed @p Root derivative.
     * @throws weft_exception if no type is registered under @p tag.
     */
    static std::unique_ptr<Root> create(std::string_view tag) {
        std::string key{tag};
        const std::scoped_lock lock{instance().mutex_};
        auto &tag_to_entry = instance().tag_to_entry_;
        auto it = tag_to_entry.find(key);
        if (it == tag_to_entry.end()) {
            throw weft_exception(
                weft_error_streamer()
                << "In GPolymorphicRegistry::create(): no type is registered under tag \"" << key << "\". "
                << "The archive references a type that was never registered (missing GEM_REGISTER_TYPE?)." << '\n'
            );
        }
        return it->second.factory();
    }

    /******************************************************************************/
    /**
     * @brief Returns the wire tag registered for the @e dynamic type of @p obj.
     * @param obj A live object whose most-derived type's tag is wanted.
     * @return The registered tag string (by value).
     * @throws weft_exception if the dynamic type of @p obj was never registered.
     */
    static std::string tagOf(const Root &obj) {
        std::type_index ti{typeid(obj)};
        const std::scoped_lock lock{instance().mutex_};
        auto &type_to_tag = instance().type_to_tag_;
        auto it = type_to_tag.find(ti);
        if (it == type_to_tag.end()) {
            throw weft_exception(
                weft_error_streamer()
                << "In GPolymorphicRegistry::tagOf(): the dynamic type \"" << ti.name() << "\" was never registered "
                << "(missing GEM_REGISTER_TYPE for this concrete type?)." << '\n'
            );
        }
        return it->second;
    }

    /** @brief Whether a type is registered under @p tag. @param tag The wire tag to probe. */
    static bool contains(std::string_view tag) {
        const std::scoped_lock lock{instance().mutex_};
        return instance().tag_to_entry_.contains(std::string{tag});
    }

    /** @brief Whether the concrete type @p ti has been registered. @param ti The type to probe. */
    static bool registered(std::type_index ti) {
        const std::scoped_lock lock{instance().mutex_};
        return instance().type_to_tag_.contains(ti);
    }

    /** @brief The number of registered types. */
    static std::size_t size() {
        const std::scoped_lock lock{instance().mutex_};
        return instance().tag_to_entry_.size();
    }

    /**
     * @brief All registered tags, sorted -- for diagnostics and the boot-time
     * completeness self-check.
     * @return A sorted copy of every registered tag.
     */
    static std::vector<std::string> tags() {
        const std::scoped_lock lock{instance().mutex_};
        std::vector<std::string> out;
        out.reserve(instance().tag_to_entry_.size());
        for (const auto &kv : instance().tag_to_entry_) {
            out.push_back(kv.first);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    /** @brief The value stored per tag: the factory plus the concrete type it builds. */
    struct entry_t {
        factory_t factory;
        std::type_index type;
    };

    GPolymorphicRegistry() = default;

    /**
     * @brief The process-wide, deliberately-leaked singleton for this @p Root.
     *
     * Heap-allocated and never freed (see the class-level lifetime note): this
     * defeats static-destruction-order hazards for cross-TU and plugin-time
     * registration. A function-local @c static pointer gives the standard
     * thread-safe first-use initialisation.
     */
    static GPolymorphicRegistry &instance() {
        static GPolymorphicRegistry *const inst = new GPolymorphicRegistry();
        return *inst;
    }

    /** @brief Guards the two maps below. Invariant 2 exception, recorded deliberately: a
     *  plain data guard over the leaked, never-destroyed registry described above -- a
     *  common/concurrency store would bring its own static lifetime and reintroduce exactly
     *  the initialization-order hazard this class is designed to defeat. */
    std::mutex mutex_;
    std::unordered_map<std::string, entry_t> tag_to_entry_;
    std::unordered_map<std::type_index, std::string> type_to_tag_;
};

/******************************************************************************/
// Internal token-pasting helpers for GEM_REGISTER_TYPE's unique variable name.
#define GEM_REGISTRY_CAT_INNER(a, b) a##b
#define GEM_REGISTRY_CAT(a, b) GEM_REGISTRY_CAT_INNER(a, b)

/**
 * @brief Registers a concrete polymorphic type with its hierarchy's
 * @ref Gem::Weft::GPolymorphicRegistry, keyed by its fully-qualified,
 * stringized type name.
 *
 * Place exactly one @c GEM_REGISTER_TYPE(FullyQualifiedType) at namespace scope
 * in a single translation unit per type -- the mechanical 1:1 counterpart of a
 * @c BOOST_CLASS_EXPORT_IMPLEMENT (whose default GUID is likewise the
 * fully-qualified stringized type, so the existing export sites already carry
 * the exact tags to reuse). The macro is variadic so a template instantiation
 * with comma-separated arguments -- @c GEM_REGISTER_TYPE(A<B, C>) -- is passed
 * through intact; the tag then reads @c "A<B, C>", per-instantiation unique.
 *
 * The hierarchy root is deduced from the type itself
 * (@c Type::gemfony_common_root_t), so only the concrete type is named. The
 * default constructor must be accessible here; for a private-ctor type call
 * @c GPolymorphicRegistry<Root>::reg(tag, type_index, factory) directly with an
 * access-bearing factory instead.
 *
 * @param ... The fully-qualified concrete type to register.
 */
#define GEM_REGISTER_TYPE(...)                                                                              \
    namespace {                                                                                             \
    const bool GEM_REGISTRY_CAT(gem_registry_tag_, __COUNTER__) =                                           \
        ::Gem::Weft::GPolymorphicRegistry<__VA_ARGS__::gemfony_common_root_t>::template reg<__VA_ARGS__>( \
            #__VA_ARGS__);                                                                                  \
    }

/******************************************************************************/

} // namespace Gem::Weft
