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
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>

// Boost header files go here

// Geneva header files go here
#include "weft/GArchivePolymorphic.hpp" // GArchive codec arm for toStream/fromStream (gem_serialize_pointer)
#include "weft/GBinaryArchive.hpp"      // GArchive flat binary codec
#include "common/GCommonEnums.hpp"        // For the serialization mode
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GParserBuilder.hpp"

namespace Gem::Common {
/******************************************************************************/
/**
 * This is an interface class that specifies common operations that must be
 * available for the majority of classes in the Gemfony scientific library.
 * As one example, (de-)serialization is simplified by some of the functions
 * in this class, as is the task of conversion to the derived types.
 *
 * @tparam g_class_type The most-derived public root of the hierarchy that uses this interface (CRTP self type)
 */
template <typename g_class_type>
class GCommonInterfaceT
  // This simplifies detection of classes that implement the Gemfony interface -- see GTypeTraits.hpp
  // The problem here is that GCommonInterfaceT<g_class_type> is usually the base class of g_class_type and thus an incomplete
  // type at the time type traits are applied. Hence we use another (trivial) base class that simplifies
  // detection.
  : private gemfony_common_interface_indicator {
public:
    /***************************************************************************/
    /**
     * @brief The most-derived public root of this interface hierarchy.
     *
     * This is the type through which (de-)serialization, load_() and clone_()
     * travel (the g_class_type template argument). It is exposed as a typedef so
     * that generic machinery further down the hierarchy -- notably the
     * GReflectiveInterfaceT / GReflectiveInterfaceBaseT mixins in GReflectiveInterfaceT.hpp -- can recover
     * the load_() parameter type and the default clone_() return type from any
     * derivative without re-templating on it.
     */
    using gemfony_common_root_t = g_class_type;

    /***************************************************************************/
    /**
     * Converts the class(-hierarchy) to a serial representation that is
     * then written to a stream.
     *
     * @tparam Self The deduced most-derived type of *this (an accessible g_class_type derivative)
     * @param self The (deduced) object being serialized
     * @param oarchive_stream The output stream the object should be written to
     * @param ser_mod The desired serialization mode
     */
    template <typename Self>
    void toStream(this const Self &self, std::ostream &oarchive_stream, Gem::Common::serializationMode ser_mod) {
        // With deducing this, `self` already IS the most-derived caller object, so taking its address
        // yields a g_class_type pointer by an implicit, compile-time-checked upcast -- no runtime
        // dynamic_cast (DEBUG) / unchecked static_cast (release) fork is needed. (De-)serialization
        // still travels through a g_class_type pointer, so the archive bytes are identical to before.
        const g_class_type *local = &self;

        switch(ser_mod) {
        case Gem::Common::serializationMode::GEM_BINARY: {
            // GArchive: serialize the polymorphic root pointer through a NON-owning shared_ptr so the
            // smart-ptr arm's gem_serialize_pointer runs (present flag + dynamic tag + members). The
            // deleter is a no-op -- `local` is borrowed (it is &self), the archive must not delete it;
            // the const_cast is safe because saving never mutates.
            std::shared_ptr<g_class_type> sp(const_cast<g_class_type *>(local), [](g_class_type *) {});
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("classhierarchyFromT", sp);
            oarchive_stream << oa.str();
        }

        break;

        case Gem::Common::serializationMode::GEM_JSON: {
            std::shared_ptr<g_class_type> sp(const_cast<g_class_type *>(local), [](g_class_type *) {});
            Gem::Weft::GJsonOArchive oa;
            oa &Gem::Weft::make_nvp("classhierarchyFromT", sp);
            oarchive_stream << oa.str();
        }

        break;
        }
    }

    /***************************************************************************/
    /**
     * Loads the object from a stream.
     *
     * @param istr The stream from which the object should be loaded
     * @param ser_mod The desired serialization mode
     *
     */
    void fromStream(std::istream &istr, Gem::Common::serializationMode ser_mod) {
        g_class_type *raw = nullptr;

        switch(ser_mod) {
        case Gem::Common::serializationMode::GEM_BINARY: {
            // GArchive: read the whole stream, reconstruct the dynamic type via the smart-ptr arm
            // (gem_serialize_pointer builds it from the identity registry), then hand ownership to
            // the raw pointer the common tail already adopts. NOTE: GBinaryIArchive holds a
            // string_view over its input, so the decoded buffer must outlive it (named local, not a
            // temporary).
            std::ostringstream ss;
            ss << istr.rdbuf();
            const std::string data = ss.str();
            Gem::Weft::GBinaryIArchive ia(data);
            std::unique_ptr<g_class_type> loaded;
            ia &Gem::Weft::make_nvp("classhierarchyFromT", loaded);
            raw = loaded.release();
        }

        break;

        case Gem::Common::serializationMode::GEM_JSON: {
            std::ostringstream ss;
            ss << istr.rdbuf();
            const std::string data = ss.str();
            Gem::Weft::GJsonIArchive ia(data);
            std::unique_ptr<g_class_type> loaded;
            ia &Gem::Weft::make_nvp("classhierarchyFromT", loaded);
            raw = loaded.release();
        }

        break;
        }

        std::unique_ptr<g_class_type> const local(raw);
        this->load_(local.get());
    }

    /***************************************************************************/
    /**
     * Converts the class to a text representation, using the currently set serialization mode for this
     * class. Note that you will have to take care yourself that serialization and de-serialization
     * happens in the same mode.
     *
     * @tparam Self The deduced most-derived type of *this (forwarded to toStream)
     * @param self The (deduced) object being serialized
     * @param ser_mod The desired serialization mode
     * @return A text-representation of this class (or its derivative)
     */
    template <typename Self>
    std::string toString(this const Self &self, Gem::Common::serializationMode ser_mod) {
        std::ostringstream oarchive_stream; // NOLINT(cppcoreguidelines-init-variables)
        self.toStream(oarchive_stream, ser_mod);
        return oarchive_stream.str();
    }

    /***************************************************************************/
    /**
     * Initializes the object from its string representation, using the currently set serialization mode.
     * Note that the string will likely describe a derivative of g_class_type, as g_class_type cannot usually be instantiated.
     * Note also that you will have to take care yourself that serialization and de-serialization happens
     * in the same mode.
     *
     * @param descr The string representation from which the object should be initialized
     * @param ser_mod The desired serialization mode
     */
    void fromString(const std::string &descr, Gem::Common::serializationMode ser_mod) {
        std::istringstream istr(descr);
        fromStream(istr, ser_mod);
    }

    /***************************************************************************/
    /**
     * Writes a serial representation of this object to a file. Can be used for check-pointing.
     *
     * @tparam Self The deduced most-derived type of *this (forwarded to toStream)
     * @param self The (deduced) object being serialized
     * @param p The name of the file the object should be saved to.
     * @param ser_mod The desired serialization mode
     */
    template <typename Self>
    void toFile(this const Self &self, const std::filesystem::path &p, Gem::Common::serializationMode ser_mod) {
        std::ofstream ofstr( // NOLINT(cppcoreguidelines-init-variables)
            p
            , std::ofstream::trunc
        ); // Note: will overwrite existing files

        if(not ofstr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GCommonInterfaceT::toFile():" << '\n'
                << "Problems connecting to file " << p.string() << '\n'
            );
        }

        self.toStream(ofstr, ser_mod);

#ifdef DEBUG
        if(not ofstr.good()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GCommonInterfaceT::toFile():" << '\n'
                << "Stream error after writing to " << p.string() << '\n'
            );
        }
#endif
    }

    /***************************************************************************/
    /**
     * Loads a serial representation of this object from file. Can be used for check-pointing.
     *
     * @param p The name of the file the object should be loaded from
     * @param ser_mod The desired serialization mode
     */
    void fromFile(const std::filesystem::path &p, Gem::Common::serializationMode ser_mod) {
        // Check that the file exists
        if(not std::filesystem::exists(p)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GCommonInterfaceT::fromFile(): Error!" << '\n'
                << "Requested input file " << p.string() << '\n'
                << "does not exist." << '\n'
            );
        }

        std::ifstream ifstr(p);

        if(not ifstr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GCommonInterfaceT::fromFile():" << '\n'
                << "Problem connecting to file " << p.string() << '\n'
            );
        }

        fromStream(ifstr, ser_mod);
    }

    /***************************************************************************/
    /**
     * Returns an XML description of the derivative it is called for
     *
     * @tparam Self The deduced most-derived type of *this (forwarded to toString)
     * @param self The (deduced) object being described
     * @return A JSON description of the GCommonInterfaceT-derivative the function is called for
     */
    template <typename Self>
    std::string report(this const Self &self) {
        return self.toString(Gem::Common::serializationMode::GEM_JSON);
    }

    /******************************************************************************/
    /**
     * Adds local configuration options to a GParserBuilder object.
     *
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions(Gem::Common::GParserBuilder &gpb) {
        addConfigurationOptions_(gpb);
    }

    /******************************************************************************/
    /**
     * Writes a configuration file to disk
     *
     * @param config_file The name of the configuration file to be written
     * @param header A header to be prepended to the configuration file
     */
    void writeConfigFile(std::filesystem::path const &config_file, const std::string &header) {
        // This class will handle the interaction with configuration files
        Gem::Common::GParserBuilder gpb;

        // Recursively add configuration options to gpb,
        // starting with the most derived class
        addConfigurationOptions(gpb);

        // Write out the configuration file
        gpb.writeConfigFile(config_file, header, true);
    }

    /******************************************************************************/
    /**
     * Reads a configuration file from disk
     *
     * @param config_file The name of the configuration file to be parsed
     */
    void readConfigFile(std::filesystem::path const &config_file) {
        // This class will handle the interaction with configuration files
        Gem::Common::GParserBuilder gpb;

        // Recursively add configuration options to gpb,
        // starting with the most derived class
        addConfigurationOptions(gpb);

        // Read in the configuration file
        gpb.parseConfigFile(config_file);
    }

    /***************************************************************************/
    /**
     * Emits a name for this class / object. Wrapper to avoid public virtual.
     *
     * @return The name of this class / object, as provided by the virtual name_()
     */
    [[nodiscard]] std::string name() const {
        return this->name_();
    }

    /***************************************************************************/
    /**
     * Checks for compliance with expectations with respect to another object
     * of type g_class_type. This purely virtual function ensures the well-formedness of the
     * compare hierarchy in derived classes.
     *
     * @param cp A constant reference to another object of the same type, camouflaged as a base object
     * @param e The expected outcome of the comparison
     * @param limit The maximum deviation for floating point values (important for similarity checks)
     */
    void compare(
        const g_class_type &cp // the other object
        ,
        Gem::Common::expectation e // the expectation for this object, e.g. equality
        ,
        double limit // the limit for allowed deviations of floating point types
    ) const {
        this->compare_(cp, e, limit);
    }

    /***************************************************************************/
    /**
     * Creates a deep clone of this object, wrapped into a std::unique_ptr (sole ownership).
     *
     * A clone is a freshly constructed object, so it has exactly one owner by construction: sole
     * ownership is the honest return type, and it is what the unique-pointer container storage policy
     * (UniquePtrStorage) needs in order to deep-copy without shared_ptr's atomic reference counting. A
     * caller that genuinely wants shared ownership converts at its own site -- unique_ptr converts to
     * shared_ptr implicitly, so opting in costs nothing and is visible where it happens.
     *
     * @return A deep clone of this object, as a std::unique_ptr<g_class_type>
     */
    [[nodiscard]] std::unique_ptr<g_class_type> clone() const {
        return std::unique_ptr<g_class_type>(this->clone_());
    }

    /***************************************************************************/
    /**
     * The function creates a clone of the g_class_type pointer, converts it to a pointer to a derived
     * class and emits it as a std::unique_ptr<> (sole ownership). Note that this template will only be
     * accessible to the compiler if g_class_type is a base type of clone_type.
     *
     * @tparam clone_type The derived type the clone should be converted to (must derive from g_class_type)
     * @return A converted deep clone of this object, wrapped into a std::unique_ptr<clone_type>
     */
    template <typename clone_type>
        requires std::derived_from<clone_type, g_class_type>
    [[nodiscard]] std::unique_ptr<clone_type> clone() const {
        // Take ownership immediately, so the clone is released on every exit path (Inv 21)
        std::unique_ptr<g_class_type> raw(this->clone_());
        if(auto *converted = dynamic_cast<clone_type *>(raw.get()); converted != nullptr) {
            raw.release(); // ownership passes to the converted pointer
            return std::unique_ptr<clone_type>(converted);
        }
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GCommonInterfaceT<>::clone<clone_type>():" << '\n'
            << "Invalid conversion to type " << typeid(clone_type).name() << '\n'
        );
    }

    /***************************************************************************/
    /**
     * Loads the data of another g_class_type(-derivative), wrapped in a shared pointer. Note that this
     * function is only accessible to the compiler if load_type is a derivative of g_class_type.
     *
     * @tparam load_type The (derived) type of the object to load from (must derive from g_class_type)
     * @param cp A copy of another g_class_type-derivative, wrapped into a std::shared_ptr<>
     */
    template <typename load_type>
        requires std::derived_from<load_type, g_class_type>
    void load(const std::shared_ptr<load_type> &cp) {
        load_(cp.get());
    }

    /***************************************************************************/
    /**
     * Loads the data of another g_class_type(-derivative), wrapped in a unique pointer. Symmetric with
     * the shared_ptr overload above; lets owning unique_ptr containers load element-from-element without
     * an explicit dereference. Note that this function is only accessible to the compiler if load_type
     * is a derivative of g_class_type.
     *
     * @tparam load_type The (derived) type of the object to load from (must derive from g_class_type)
     * @param cp A copy of another g_class_type-derivative, wrapped into a std::unique_ptr<>
     */
    template <typename load_type>
        requires std::derived_from<load_type, g_class_type>
    void load(const std::unique_ptr<load_type> &cp) {
        load_(cp.get());
    }

    /***************************************************************************/
    /**
     * Loads the data of another g_class_type(-derivative), presented as a constant reference. Note that this
     * function is only accessible to the compiler if load_type is a derivative of g_class_type.
     *
     * @tparam load_type The (derived) type of the object to load from (must derive from g_class_type)
     * @param cp A constant reference to another g_class_type-derivative whose data should be loaded
     */
    template <typename load_type>
        requires std::derived_from<load_type, g_class_type>
    void load(const load_type &cp) {
        load_(&cp);
    }

protected:
    /***************************************************************************/
    // Defaulted constructors -- rule of five

    GCommonInterfaceT() = default;
    GCommonInterfaceT(const GCommonInterfaceT<g_class_type> &cp) = default;
    GCommonInterfaceT(GCommonInterfaceT<g_class_type> &&cp) = default;
    // virtual destructor: the class is a polymorphic base (has virtual
    // load_/compare_/name_/clone_). `protected` already prevents
    // `delete pBase;` from outside, but any friend or sibling that
    // obtained a base pointer would still hit UB on delete-through-base
    // without virtual dispatch. Costs nothing and makes the contract
    // explicit.
    virtual ~GCommonInterfaceT() = default;

    GCommonInterfaceT<g_class_type> &operator=(GCommonInterfaceT<g_class_type> const &) = default;
    GCommonInterfaceT<g_class_type> &operator=(GCommonInterfaceT<g_class_type> &&) = default;

    /***************************************************************************/
    /**
     * @brief Loads the data of another g_class_type
     *
     * @param cp A pointer to another g_class_type-derivative whose data should be loaded into this object
     */
    virtual void load_(const g_class_type *) = 0;

    /***************************************************************************/
    /** @brief Allow access to this classes compare_ function */
    friend void compare_base_t<GCommonInterfaceT<g_class_type>>(
        GCommonInterfaceT<g_class_type> const &,
        GCommonInterfaceT<g_class_type> const &,
        GToken &
    );

    /***************************************************************************/
    /**
     * Checks for compliance with expectations with respect to another object
     * of type g_class_type. This purely virtual function ensures the well-formedness of the
     * compare hierarchy in derived classes.
     *
     * @param cp A constant reference to another object of the same type, camouflaged as a base object
     * @param e The expected outcome of the comparison
     * @param limit The maximum deviation for floating point values (important for similarity checks)
     */
    virtual void compare_(
        const g_class_type &cp // the other object
        ,
        const Gem::Common::expectation &e // the expectation for this object, e.g. equality
        ,
        const double &limit // the limit for allowed deviations of floating point types
    ) const = 0;

    /***************************************************************************/
    /**
     * Checks for compliance with expectations with respect to another object
     * of the same type. This function does the real check. Without it we would get
     * an error about "no known conversion from GCommonInterfaceT<g_class_type> to g_class_type.
     *
     * @param cp A constant reference to another object of the same type, camouflaged as a base object
     * @param e The expected outcome of the comparison
     */
    void compare_(
        const GCommonInterfaceT<g_class_type> &cp // the other object
        ,
        const Gem::Common::expectation &e // the expectation for this object, e.g. equality
        ,
        const double & /*limit*/ // the limit for allowed deviations of floating point types
    ) const {
        using namespace Gem::Common;

        // Check that cp isn't the same object as this one
        ptrDifferenceCheck(&cp, this);

        // No parent classes to check...

        // ... and no local data

        // We consider two instances of this class to be always equal, as they
        // do not have any local data and this is the base class. Hence
        // we throw an expectation violation for the expectation INEQUALITY.
        if(expectation::INEQUALITY == e) {
            throw g_expectation_violation(
                "In GCommonInterfaceT<g_class_type>: instance is empty and a base class, hence the "
                "expectation of inequality is always violated."
            );
        }
    }

    /******************************************************************************/
    /**
     * Adds local configuration options to a GParserBuilder object. This is a protected, virtual version
     * of this function that is overloaded in derived classes.
     *
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    virtual void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
        // No local data, no relevant parent classes, hence nothing to do
    }

private:
    /***************************************************************************/
    /**
     * Emits a name for this class / object
     *
     * @return The name of this class / object
     */
    [[nodiscard]] virtual std::string name_() const {
        return std::string("GCommonInterfaceT<g_class_type>");
    }

    /***************************************************************************/
    /**
     * @brief Creates a deep clone of this object
     *
     * @return A raw, owning pointer to a deep clone of this object (caller takes ownership)
     */
    [[nodiscard]] virtual g_class_type *clone_() const = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
