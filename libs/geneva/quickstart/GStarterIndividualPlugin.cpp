/**
 * @file
 * @brief Module "glue" that makes GStarterIndividual a runtime-loadable Geneva individual.
 *
 * This is the entire author-facing surface for making an existing individual loadable at run time: the fixed
 * extern "C" entry point geneva_module_manifest(), delegating to the typed helper
 * individualManifest<Factory, Config, Name>() (from the INSTALLED Geneva header geneva/genome/GIndividualPlugin.hpp).
 * The individual's archive tag (GEM_REGISTER_ARCHIVABLE) lives in GStarterIndividual.cpp, which is
 * the DECLARE's SOURCES (compiled into both the compiled-in object library and the loadable module), so this
 * translation unit carries ONLY the manifest -- no archive registration here, or it would be a double
 * registration.
 *
 * Compiled into the module .so only (never the object library): the manifest symbol name is fixed, so a
 * compile-in binary that links several individuals must not contain two definitions of it.
 */

#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

#include "common/GModuleManifest.hpp"          // GenevaModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp"    // Gem::Geneva::individualManifest<>

#include "GStarterIndividual.hpp"

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::individualManifest<
        Gem::Geneva::Genome::GIndividualFactory<Gem::Geneva::GStarterIndividual>,
        "./config/GStarterIndividual.json", "GStarterIndividual">();
}
