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

// Standard header files go here
#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonHelperFunctionsT.hpp" // g_convert_and_compare
#include "common/GCommonInterfaceT.hpp"       // GCommonInterfaceT, gemfony_common_root_t
#include "common/GExpectationChecksT.hpp" // GToken, expectation, compare_base_t, g_compare_members
#include "common/GMemberReflectionT.hpp"  // g_load_members, serialize_members

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Access shim that lets the GReflectiveInterface mixins reach a managed class's
 * private localMembers_() declaration.
 *
 * localMembers_() is the single, per-class declaration of a class's own data
 * members (a tuple of named references). It is kept private in every class,
 * exactly as the hand-written serialize()/load_()/compare_() were. The mixins
 * below live in the base class and therefore cannot see a derived class's
 * private members directly, so each managed class grants friendship to this one
 * empty struct (a single `friend struct Gem::Common::GReflectiveInterfaceAccess;` line
 * the folded serialize() needs). Routing every access through this single shim keeps
 * the friendship declaration short and identical across all managed classes.
 */
struct GReflectiveInterfaceAccess {
    /**
     * @brief Returns a managed object's localMembers_() tuple.
     *
     * If T declares its own localMembers_() this resolves to it. If T declares
     * none, it resolves to the *deleted* localMembers_() inherited from
     * GReflectiveInterfaceBaseT (see below), which makes this call ill-formed -- so a
     * class that forgets to declare its own member list fails to compile rather
     * than silently double-serializing its parent's members (amendment: reject a
     * missing/inherited member list, do not accept it).
     *
     * @tparam T The (possibly const-qualified) managed class type
     * @param self The object whose local members are retrieved
     * @return The tuple of named local-member references declared by T::localMembers_()
     */
    template <typename T>
    static auto members(T &self) {
        return self.localMembers_();
    }

    /**
     * @brief Invokes a managed object's post-load hook.
     *
     * The mixin's generated load_() calls this once, after all members have been
     * loaded, to let a class repair cached or derived state that is not itself a
     * serialized member (e.g. a content-addressed cache keyed on a just-loaded
     * member). If T declares no postLoad_() of its own this resolves to the no-op
     * default in GReflectiveInterfaceBaseT, so the hook costs nothing for the classes that
     * do not need it.
     *
     * @tparam T The (non-const) managed class type
     * @param self The object whose post-load hook is invoked
     */
    template <typename T>
    static void postLoad(T &self) {
        self.postLoad_();
    }

    /**
     * @brief SFINAE-safe test of whether T resolves a usable localMembers_().
     *
     * Unlike members() -- whose deduced (auto) return type forces the callee's
     * body to be instantiated, turning a class with NO viable localMembers_() into
     * a hard error rather than a substitution failure -- this checks only call
     * *resolvability*. A constrained localMembers_() whose constraint is
     * unsatisfied (e.g. GGenomeT's marker-gated default without the opt-in marker)
     * is discarded during overload resolution, and the =deleted GReflectiveInterfaceBaseT
     * fallback is non-viable, so no candidate is selected and this simply yields
     * false instead of failing to compile. It is routed through this friend shim
     * so the private localMembers_() is reachable, and it never instantiates a
     * viable candidate's body beyond what overload resolution already requires.
     *
     * @tparam T The (possibly const-qualified) managed class type
     */
    template <typename T>
    static constexpr bool has_members = requires(T &t) { t.localMembers_(); };

    /**
     * @brief Reconstructs a default-constructed managed object on the heap,
     * reaching a *private* default constructor through this friend shim.
     *
     * A managed class keeps its default constructor private (only de-serialization
     * needs it) and befriends this struct. The polymorphic-pointer registry factory
     * therefore reconstructs the type *here* -- where the friendship grants ctor
     * access -- rather than at the (non-friend) @c GEM_REGISTER_ARCHIVABLE site.
     * @c register_archivable routes through this whenever the concrete type is not
     * publicly default-constructible.
     *
     * @tparam T The managed concrete class to construct
     * @return An owning pointer to a freshly default-constructed T
     */
    template <typename T>
    static std::unique_ptr<T> construct() {
        return std::unique_ptr<T>(new T());
    }
};

/******************************************************************************/
/**
 * @brief A class is managed by the GReflectiveInterface mixins iff it exposes both a
 * reachable localMembers_() and a static class_name.
 *
 * This is the concept the mixins assert on (amendment: the mixins deliberately
 * supply *no* default localMembers_() -- a class that fails to declare its own
 * must fail to compile rather than silently serialize/compare an empty member
 * set). class_name backs name_() and the compare token label.
 *
 * @tparam T The managed class type
 */
template <typename T>
concept ReflectiveInterfaceManaged =
    requires(T &t) { GReflectiveInterfaceAccess::members(t); } && requires {
        { T::class_name } -> std::convertible_to<std::string_view>;
    };

/******************************************************************************/
/**
 * @brief Generates the name_()/load_()/compare_() members (and the folded
 * serialize()) shared by every class in a GCommonInterfaceT hierarchy.
 *
 * This is the base half of the reflective-interface mixin, used directly by *abstract*
 * classes (those whose clone_() stays pure) and inherited by GReflectiveInterfaceT for
 * *concrete* classes. It derives from the class's real parent_type and inserts no
 * data of its own, so it is transparent to serialization: the folded
 * serialize() writes the parent_type base slice (skipping this stateless layer) plus
 * the class's own members, exactly as the hand-written code did.
 *
 * The three overriders and the folded serialize() are single-sourced here from
 * the managed class's localMembers_() declaration, so a class can no longer
 * silently drop a member from one of the four (the failure mode Inv 4 exists to
 * prevent). Everything that varies is a template parameter or is recovered from
 * the hierarchy:
 *  - the load_() parameter type / the interface base is Root, recovered from
 *    parent_type::gemfony_common_root_t;
 *  - the class name comes from derived_type::class_name;
 *  - the member list comes from derived_type::localMembers_().
 *
 * The root of a hierarchy (parent_type == GCommonInterfaceT<Root>) is handled by the
 * two `if constexpr` guards: it has no serializable/loadable parent slice, but
 * its compare_() still compares the GCommonInterfaceT base (the non-pure
 * same-type overload), matching the hand-written root behaviour.
 *
 * All member bodies are instantiated lazily (when the vtable is emitted / the
 * archive is used), by which point derived_type is complete -- the same reason the
 * pre-existing name_/clone_ generators (GGenomeT, GOptimizationAlgorithmT, ...)
 * can name derived_type's constructor from a CRTP base.
 *
 * @tparam derived_type The managed (CRTP self) class
 * @tparam parent_type  The class's real base (a GCommonInterfaceT derivative, or
 *                 GCommonInterfaceT<Root> itself for a hierarchy root)
 */
template <typename derived_type, typename parent_type>
class GReflectiveInterfaceBaseT : public parent_type {
protected:
    /** @brief The most-derived public root of the hierarchy (load_() parameter / clone_() base). */
    using Root = typename parent_type::gemfony_common_root_t;

    /** @brief Inherit the parent's constructors so a managed class keeps its initialization surface. */
    using parent_type::parent_type;

    // Inherited constructors never include the default / copy / move members, so
    // declare them explicitly (the mixin adds no state, hence all defaulted).
    GReflectiveInterfaceBaseT() = default;
    GReflectiveInterfaceBaseT(GReflectiveInterfaceBaseT const &) = default;
    GReflectiveInterfaceBaseT(GReflectiveInterfaceBaseT &&) = default;
    ~GReflectiveInterfaceBaseT() override = default;
    GReflectiveInterfaceBaseT &operator=(GReflectiveInterfaceBaseT const &) = default;
    GReflectiveInterfaceBaseT &operator=(GReflectiveInterfaceBaseT &&) = default;

    /**
     * @brief Loads the data of another object of the same hierarchy into this one.
     *
     * Protected (not private) because a subclass's generated load_() invokes its
     * own parent_type::load_() by qualified-id to load the parent slice.
     *
     * @param cp The other object, passed as a hierarchy-root pointer
     */
    void load_(Root const *cp) override {
        static_assert(
            ReflectiveInterfaceManaged<derived_type>,
            "GReflectiveInterfaceBaseT: derived_type must declare its own localMembers_() and a "
            "static "
            "class_name"
        );
        auto const *p_load =
            Gem::Common::g_convert_and_compare(cp, static_cast<derived_type const *>(this));

        // Load the parent slice, unless this is the hierarchy root (whose parent is the pure interface).
        if constexpr(!std::same_as<parent_type, Gem::Common::GCommonInterfaceT<Root>>) {
            parent_type::load_(cp);
        }

        // ... and then this class's own members, from the single localMembers_() declaration.
        Gem::Common::g_load_members(
            GReflectiveInterfaceAccess::members(static_cast<derived_type &>(*this)),
            GReflectiveInterfaceAccess::members(static_cast<derived_type const &>(*p_load))
        );

        // Finally, let the class repair any cached/derived state keyed on the members
        // just loaded (no-op unless the class declares its own postLoad_()).
        GReflectiveInterfaceAccess::postLoad(static_cast<derived_type &>(*this));
    }

    /**
     * @brief Default post-load hook: does nothing.
     *
     * A managed class that carries cached or derived state not covered by its
     * serialized members (for instance a content-addressed cache keyed on a
     * just-loaded member) declares its own private `void postLoad_()` to rebuild
     * that state; it hides this default by ordinary name lookup. Classes without
     * such state inherit this no-op, so the hook is free for them. It is invoked
     * only through GReflectiveInterfaceAccess (a friend of every managed class), the same
     * routing localMembers_() uses.
     */
    void postLoad_() {
    }

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    // Lets a GArchive codec reach this generated serialize().
    friend struct Gem::Weft::access;

    /**
     * @brief Deleted fallback member list.
     *
     * A managed class must declare its own localMembers_() (an empty
     * `return std::make_tuple();` for a stateless class). If it does not, name
     * lookup from the class finds this deleted declaration -- hidden by any real
     * one -- so GReflectiveInterfaceAccess::members() becomes ill-formed and the
     * ReflectiveInterfaceManaged concept turns false, failing the static_assert below.
     * This is what makes "a stateless class needs an *explicit* empty member
     * list" a compile-time guarantee rather than a review convention: without it,
     * an inherited localMembers_() would silently serialize the parent's members
     * a second time.
     */
    template <typename self_type>
    auto localMembers_(this self_type &self) = delete;

    /**
     * @brief Serializes the parent slice (unless this is the hierarchy root) and
     * this class's own members, derived from the single localMembers_() declaration.
     * Codec-generic: the same body serves every @c GArchive codec (the codec-specific
     * spellings live once in @c archive_named_base / @c serialize_members), so adding a
     * codec never touches a serialize() body.
     * @tparam archive_type The GArchive codec type
     * @param ar The archive being read from or written to
     * @param version The (unused) serialization format version
     */
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] unsigned int const version) {
        static_assert(
            ReflectiveInterfaceManaged<derived_type>,
            "GReflectiveInterfaceBaseT: derived_type must declare its own localMembers_() and a "
            "static "
            "class_name"
        );
        if constexpr(!std::same_as<parent_type, Gem::Common::GCommonInterfaceT<Root>>) {
            // The parent_type slice is taken from the *concrete* derived_type reference, so what crosses the
            // archive is exactly what a hand-written archive_named_base<parent_type>(ar, "gemfonyParent",
            // *this) inside derived_type would write -- the codec narrows to the parent_type sub-object itself.
            Gem::Common::archive_named_base<parent_type>(
                ar,
                "gemfonyParent",
                static_cast<derived_type &>(*this)
            );
        }
        Gem::Common::serialize_members(
            ar,
            GReflectiveInterfaceAccess::members(static_cast<derived_type &>(*this))
        );
    }
    ///////////////////////////////////////////////////////////////////////

    /** @brief Grant this class's compare_ to a subclass comparing this slice (friendship is not inherited). */
    friend void
    Gem::Common::compare_base_t<derived_type>(derived_type const &, derived_type const &, GToken &);

    /**
     * @brief Compares this object to another of the same hierarchy against an expectation.
     * @param cp The other object, passed as a hierarchy-root reference
     * @param e The expectation (equality / inequality) the comparison should fulfil
     * @param limit The tolerance for floating-point comparisons (unused: per-member
     *              tolerance is applied inside g_compare_members / compare_base_t)
     */
    void compare_(
        Root const &cp,
        Gem::Common::expectation const &e,
        [[maybe_unused]] double const &limit
    ) const override {
        static_assert(
            ReflectiveInterfaceManaged<derived_type>,
            "GReflectiveInterfaceBaseT: derived_type must declare its own localMembers_() and a "
            "static "
            "class_name"
        );
        // Convert cp to derived_type, guarding against self-comparison.
        auto const *p_load =
            Gem::Common::g_convert_and_compare(cp, static_cast<derived_type const *>(this));

        GToken token(std::string(derived_type::class_name), e);

        // Compare the parent slice (for the root this is the GCommonInterfaceT base's same-type overload) ...
        Gem::Common::compare_base_t<parent_type>(*this, *p_load, token);

        // ... and then this class's own members, from the single localMembers_() declaration.
        Gem::Common::g_compare_members(
            GReflectiveInterfaceAccess::members(static_cast<derived_type const &>(*this)),
            GReflectiveInterfaceAccess::members(static_cast<derived_type const &>(*p_load)),
            token
        );

        // React on deviations from the expectation.
        token.evaluate();
    }

    /**
     * @brief Emits this class's name.
     * @return derived_type::class_name as a std::string
     */
    [[nodiscard]] std::string name_() const override {
        return std::string(derived_type::class_name);
    }
};

/******************************************************************************/
/**
 * @brief The full boilerplate mixin for a *concrete* class: GReflectiveInterfaceBaseT
 * (name_/load_/compare_/serialize) plus a generated clone_().
 *
 * clone_() cannot live in the shared base because its body (`new derived_type(...)`)
 * would be instantiated into the vtable of an abstract class too, so it is added
 * only here, where a class opting in is asserting it is concrete.
 *
 * clone_() returns the hierarchy root, recovered from parent_type. There was once a third
 * template parameter for a narrower, covariant return type: GGenomeT declared
 * GGenome* while the root was GOptimizableEntity. Folding those two classes together
 * left the parameter with no client, so it is gone -- one fewer knob, and one fewer way
 * for a derivative to disagree with its own hierarchy about what a clone is.
 *
 * @tparam derived_type The managed (CRTP self) concrete class
 * @tparam parent_type  The class's real base
 */
template <typename derived_type, typename parent_type>
class GReflectiveInterfaceT : public GReflectiveInterfaceBaseT<derived_type, parent_type> {
protected:
    /** @brief Inherit the base mixin's (and thereby the parent's) constructors. */
    using GReflectiveInterfaceBaseT<derived_type, parent_type>::GReflectiveInterfaceBaseT;

private:
    /**
     * @brief Creates a deep clone of this object via the derived_type copy constructor.
     * @return A heap-allocated deep copy of this object (caller takes ownership)
     */
    [[nodiscard]] typename parent_type::gemfony_common_root_t *clone_() const override {
        return new derived_type(*static_cast<derived_type const *>(this));
    }
};

/******************************************************************************/

} /* namespace Gem::Common */

/******************************************************************************/

namespace Gem::Weft {

/**
 * @brief Definition of the Weft codec base's polymorphic-registry construction seam
 * (declared in @c GArchive.hpp): reconstructs a private-ctor reflection-managed type
 * through the @c GReflectiveInterfaceAccess friend shim and hands it back as its
 * hierarchy root. Defined here, in the reflection layer, so @c GArchivePolymorphic.hpp
 * can call the seam without a reflection-layer include (see the declaration's note on
 * the broken include cycle). It lives in a @c Gem::Weft block outside @c Gem::Common
 * because @c Gem::Weft is its own library and does not nest inside @c Gem::Common.
 * @tparam T The concrete reflection-managed type to reconstruct.
 */
template <typename T>
std::unique_ptr<typename T::gemfony_common_root_t> gem_registry_construct_reflective() {
    return Gem::Common::GReflectiveInterfaceAccess::template construct<T>();
}

} // namespace Gem::Weft
