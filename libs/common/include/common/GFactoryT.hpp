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
#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// Boost header files go here
#include <boost/json.hpp>

// Geneva header files go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GMemberReflectionT.hpp" // make_member / serialize_members / g_load_members
#include "common/GParserBuilder.hpp"
#include "common/GSerializationHelperFunctionsT.hpp" // serialization of std::atomic<T>
#include "common/concurrency/GLoadOnceCellT.hpp"     // the transient config-parse cache

namespace Gem::Common {

/******************************************************************************/
/**
 * A factory class that returns objects of type prod_type . The class comprises a framework
 * for reading additional configuration options from a configuration file. The actual setup
 * work needs to be done in functions that are implemented in derived classes for each target
 * object individually, or in specializations of this class.
 *
 * @tparam prod_type The type of object produced by this factory
 */
template <typename prod_type>
class GFactoryT {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief The single declaration of the factory's persistent members, from which serialize(), the
     * copy operations and load() are derived. The mutex and the parse cache are not members of the
     * list: a copy starts with fresh ones.
     * @return A tuple of named member references
     */
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member("configFile", self.config_path_),
            Gem::Common::make_member("initialized_", self.initialized_)
        );
    }

    /**
     * @brief (De)serializes the factory's persistent members
     * @tparam archive_type The GArchive codec type
     * @param ar The archive to read from / write to
     * @param version The (unused) class version
     */
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::serialize_members(ar, this->localMembers_());
        if constexpr(not archive_type::is_saving) {
            // The path was replaced wholesale: a cache filled from the previous one must not survive
            // (loading into an already-used factory is the case a default member initialiser cannot cover).
            config_document_cell_.reset();
        }
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /**
	  * The standard constructor
	  *
	  * @param configFile The path of a configuration file holding information about objects of type prod_type
	  */
    explicit GFactoryT(std::filesystem::path configFile)
      : config_path_(std::move(configFile)) { /* nothing */
    }

    /***************************************************************************/
    // The init_mutex_ and the parse cache are neither copyable nor movable, so the implicit copy and
    // move operations are deleted. The copy operations below copy the member list (localMembers_())
    // and leave both fresh; a move copies as well (the two members are cheap).

    /**
     * @brief Copy constructor; copies the member list, and leaves the new object with a fresh
     * synchronisation mutex and a cold parse cache
     * @param cp The factory to copy from
     */
    GFactoryT(const GFactoryT<prod_type> &cp) {
        Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());
    }

    /** @brief The (virtual, defaulted) destructor */
    virtual ~GFactoryT() = default;

    /**
     * @brief Copy-assignment operator; copies the member list and invalidates the transient parse
     * cache (the mutex is deliberately not copied). The class is abstract, so it cannot copy-construct
     * a temporary.
     * @param cp The factory to copy from
     * @return A reference to this object
     */
    GFactoryT<prod_type> &operator=(GFactoryT<prod_type> const &cp) {
        if(this != &cp) {
            Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());
            config_document_cell_.reset(); // config_path_ may have changed
        }
        return *this;
    }

    /***************************************************************************/
    /**
	  * Triggers the creation of objects of the desired type
	  *
	  * @brief Function-call operator; a convenience alias for get()
	  * @return A newly produced object of type prod_type
	  */
    std::shared_ptr<prod_type> operator()() {
        return this->get();
    }

    /***************************************************************************/
    /**
	  * Allows to retrieve the name of the config file, including its path
	  *
	  * @return The name of the config-file
	  */
    std::string getConfigFileName() const {
        return config_path_.string();
    }

    /***************************************************************************/
    /**
	  * Allows to retrieve the std::filesystem::path object referring to the config file
	  *
	  * @return The std::filesystem::path object referring to the config file
	  */
    std::filesystem::path getConfigFilePath() const {
        return config_path_;
    }

    /***************************************************************************/
    /**
	  * Sets a new name for the configuration file. Will only have an effect for
	  * the next produced object.
	  *
	  * Re-keying the factory drops the transient parse cache, so the next get() really does read the
	  * new file -- otherwise the document parsed from the previous path would keep being re-applied
	  * and this call would have no effect at all.
	  *
	  * @param configFile The new configuration-file name (interpreted as a filesystem path)
	  */
    void setConfigFile(const std::string &configFile) {
        config_path_ = std::filesystem::path(configFile);
        config_document_cell_.reset();
    }

    /***************************************************************************/
    /**
	  * Retrieves an object of the desired type and converts it to a target type,
	  * if possible.
	  *
	  * @tparam target_type The type the produced object should be converted to
	  * @return The produced object converted to target_type, or an empty pointer if production yielded nothing
	  */
    template <typename target_type>
    std::shared_ptr<target_type> get_as() {
        // An empty product passes through as an empty pointer
        return Gem::Common::g_ptr_conversion<prod_type, target_type>(this->get());
    }

    /***************************************************************************/
    /**
	  * Writes a configuration file to disk
	  *
	  * @param header A header comment to be prepended to the configuration file
	  */
    void writeConfigFile(std::string const &header) {
        // Make sure the initialization code has been executed.
        // This function will do nothing when called more than once
        this->globalInit();

        // Create a parser builder object. It will be destroyed at
        // the end of this function and thus cannot cause trouble
        // due to registered call-backs and references
        Gem::Common::GParserBuilder gpb;

        // Add the user-defined configuration specifications, local to the factory
        this->describeLocalOptions_(gpb);

        // Retrieve an object (will be discarded at the end of this function)
        // Here, further options may be added to the parser builder.
        std::shared_ptr<prod_type> p = this->getObject_(gpb);

        // Allow the factory to act on configuration options received
        // in the parsing process.
        this->postProcess_(p);

        // Write out the configuration file, if options have been registered
        if(gpb.numberOfFileOptions() > 0) {
            gpb.writeConfigFile(config_path_, header, true);
        }
        else {
            std::cout << "Warning: An attempt was made to write out configuration file "
                      << config_path_.string() << '\n'
                      << "even though no configuration options were registered. Doing nothing."
                      << '\n';
        }
    }

    /***************************************************************************/
    /**
	  * Loads the data of another GFactoryT<> object
	  *
	  * @param cp A pointer to the factory whose data (config path and initialized flag) should be loaded
	  */
    virtual void load(std::shared_ptr<GFactoryT<prod_type>> cp) {
        Gem::Common::g_load_members(this->localMembers_(), cp->localMembers_());
    }

    /***************************************************************************/
    /**
	  * Creates a deep clone of this object. This function is a trap. Factories
	  * wishing to use this functionality need to overload this function.
	  * Others don't have to due to this "pseudo-implementation".
	  *
	  * @return A deep clone of this factory (overloaded in derived classes; throws here)
	  */
    virtual std::shared_ptr<GFactoryT<prod_type>> clone() const {
        throw gem_exception(
            g_error_streamer() << "Function was called when it shouldn't be." << '\n'
                               << "This function is a trap." << '\n'
        );
    }

    /***************************************************************************/
    /**
	 * Allows the creation of objects of the desired type.
	 *
	 * @return A newly produced object of type prod_type
	 */
    std::shared_ptr<prod_type> get() {
        return this->get_();
    }

protected:
    /***************************************************************************/
    /** @brief The default constructor. Only needed for (de-)serialization purposes, hence protected */
    GFactoryT() = default;

    /***************************************************************************/
    /** @brief Performs necessary initialization work */
    virtual void init_() { /* nothing */
    }

    /**
     * @brief Allows to describe local configuration options in derived classes
     * @param gpb The parser builder to which derived classes may register their configuration options
     */
    virtual void describeLocalOptions_(Gem::Common::GParserBuilder &gpb) { /* nothing */ };

    /**
     * @brief Allows to act on the configuration options received from the configuration file
     *
     * The argument is the freshly produced object on which derived classes may act.
     */
    virtual void postProcess_(std::shared_ptr<prod_type> &) = 0;

    /***************************************************************************/
    /**
     * Allows the creation of objects of the desired type.
     *
     * @return A newly produced object of type prod_type, configured from the (cached) config file
     */
    virtual std::shared_ptr<prod_type> get_() {
        // Make sure the initialization code has been executed.
        // This function will do nothing when called more than once
        this->globalInit();

        // Create a parser builder object. It will be destroyed at
        // the end of this function and thus cannot cause trouble
        // due to registered call-backs and references
        Gem::Common::GParserBuilder gpb;

        // Add specific configuration options for the derived factory.
        // These may correspond to local variables
        this->describeLocalOptions_(gpb);

        // Retrieve the actual object. It may, in the process of its
        // creation, add further configuration options and call-backs to
        // the parser
        std::shared_ptr<prod_type> p = this->getObject_(gpb);

        // Read + parse the configuration file only ONCE: the first call captures the
        // parsed document in the load-once cell, every subsequent call re-applies that
        // document to the freshly created object (no disk I/O / JSON re-parse). The file
        // does not change between produce() calls, so this is purely an efficiency win.
        // A failing parse throws out of the producer, which leaves the cell unfilled, so
        // the next call retries rather than caching a failure.
        bool this_call_parsed = false;
        boost::json::value const &config_document =
            config_document_cell_.getOrCompute([this, &gpb, &this_call_parsed]() {
                boost::json::value parsed;
                gpb.parseConfigFile(config_path_, &parsed);
                this_call_parsed = true;
                return parsed;
            });
        if(not this_call_parsed) {
            // Somebody else did the real parse (this call or an earlier one), so this gpb still
            // needs the values. The unknown-key diagnostic already ran on the first (real) parse,
            // so skip it here -- otherwise it would re-run per produced object.
            gpb.loadFromDocument(
                config_document,
                config_path_,
                /* run_unknown_key_check = */ false
            );
        }

        // Allow the factory to act on configuration options received
        // in the parsing process.
        this->postProcess_(p);

        // Let the audience know
        return p;
    }

private:
    /***************************************************************************/
    /**
	  * Performs necessary global initialization work. This function is meant for
	  * initialization work performed just prior to the creation of the first
	  * item. It will do nothing when called more than once — thread-safely
	  * gated via init_mutex_. All real work is done in the "init_()" function,
	  * which may be overloaded by the user.
	  *
	  * Note on serialisation: @c initialized_ is part of the serialised state,
	  * so a deserialised factory may already be flagged as initialised. The
	  * mutex-guarded check below correctly skips a second init in that case.
	  * (A std::once_flag would not work here because it cannot be serialised
	  * and a fresh post-load flag would re-run init_().)
	  */
    void globalInit() {
        std::scoped_lock const lk(init_mutex_);
        if(not initialized_) {
            this->init_();
            initialized_ = true;
        }
    }

    /***************************************************************************/
    /**
     * @brief Creates objects of the desired type (implemented in derived classes)
     *
     * The argument is the parser builder to which the object may add further configuration
     * options.
     *
     * @return A newly created object of type prod_type
     */
    virtual std::shared_ptr<prod_type> getObject_(Gem::Common::GParserBuilder &) = 0;

    /***************************************************************************/

    std::filesystem::path config_path_; ///< The name and path of the configuration file
    bool initialized_ = false; ///< Indicates whether the initialization work has already been done
    /// transient: a lock that orders concurrent first calls of globalInit()
    mutable std::mutex init_mutex_;

    /** @brief transient: a parse cache -- the configuration file is read and parsed on the first
     *  get_(), and every later call re-applies this document. It is the sanctioned load-once
     *  cell rather than a hand-rolled mutex+value+flag triple -- unlike the neighbouring initialized_
     *  flag, which IS serialized and therefore genuinely cannot be one (see globalInit()). A copy or
     *  move starts with a fresh, cold cell; assignment and deserialize-into re-key the factory and
     *  therefore reset() it explicitly. */
    Concurrency::GLoadOnceCellT<boost::json::value> config_document_cell_;
};

/******************************************************************************/

} /* namespace Gem::Common */
