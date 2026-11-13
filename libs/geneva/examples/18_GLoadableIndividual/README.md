# Example 18 — a runtime-loadable optimization problem (individual plugin)

This example shows how to express an optimization problem as a **runtime-loadable plugin**, so Geneva can
run a new problem **without being recompiled**. It has two artifacts:

- **`libGLoadableParaboloid.so`** — the problem (an *n*-dimensional paraboloid), built as a shared object.
- **`GGenericOptimizer`** — a generic optimizer with **no problem compiled in**; it loads a problem at
  runtime via `--individual`.

## Run it

```bash
# from this example's build directory (so ./config/Go2.json is found):
./GGenericOptimizer --individual ./libGLoadableParaboloid.so
```

You can also point at the plugin from the config file instead of the command line — set
`individual_plugin_path` in `config/Go2.json` (the `--individual` option overrides it).

Because a Geneva server and client are the **same binary** launched with different options, a networked run
just needs the same `.so` present on every node (already there on a shared HPC filesystem; staged next to
the binary in the cloud):

```bash
./GGenericOptimizer --individual ./libGLoadableParaboloid.so --consumer beast          # server
./GGenericOptimizer --individual ./libGLoadableParaboloid.so --consumer beast --client  # worker
```

## How to make your OWN loadable individual

1. **Write an ordinary Geneva flat individual** (`GLoadableParaboloid.hpp` here). Nothing about it is
   plugin-specific — the same class could be compiled in. It supplies the usual config-driven hooks
   (`Config`, `describeConfig`, `buildGenome`, `buildAdaptionConfig`, `evaluate`, `serialize`).

2. **Add one translation unit** (`GLoadableParaboloidPlugin.cpp`) with exactly two registrations:

   ```cpp
   #include "geneva/genome/GIndividualFactory.hpp"
   #include "geneva/genome/GIndividualPlugin.hpp"
   #include "weft/GArchivePolymorphic.hpp"
   #include "MyProblem.hpp"

   GEM_REGISTER_ARCHIVABLE(MyProblem)                                // wire / checkpoint tag

   extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
       return Gem::Geneva::individualManifest<
           Gem::Geneva::Genome::GIndividualFactory<MyProblem>,
           "./config/MyProblem.json", "MyProblem">();               // factory / config file / name
   }
   ```

   The `extern "C"` `geneva_module_manifest()` is the only irreducible boilerplate (the loader resolves this
   fixed, unmangled symbol via `dlsym`); everything else is the typed helper
   `Gem::Geneva::individualManifest<Factory, Config, Name>()` — no Geneva macro. It builds the module
   manifest: the **toolchain-compatibility fingerprint** (`GenevaCompat`) and the **module-ABI stamp**, both
   of which the loader validates before it touches anything else, plus one **individual contribution** whose
   factory is built with the given config file (auto-created with the individual's defaults if absent).
   `GEM_REGISTER_ARCHIVABLE` registers the serialization tag so the individual can cross the wire and a
   checkpoint — you write it yourself, exactly as for a compiled-in individual.

3. **Package it** with `GENEVA_ADD_INDIVIDUAL_MODULE(<target> <glue.cpp>)` (see `CMakeLists.txt`; use
   `GENEVA_DECLARE_INDIVIDUAL(... MODE both ...)` instead when the same individual is also to be
   compiled in). The module
   links **none** of the Geneva libraries — it needs only their headers, and resolves their symbols from the
   host process at load time.

That is the entire author-facing surface.

## What the mechanism guarantees

- **Exactly one problem per process.** Compiling an individual in *and* passing `--individual` (or passing
  two plugins) is a hard error naming both sources. Providing none is a clear error listing the three ways
  to supply a problem.
- **Version safety.** A plugin that does not fit is **rejected at load**, before any individual is
  constructed, with a message naming what is wrong: a different compiler / standard library / Boost /
  build mode / Geneva version (the `GenevaCompat` fingerprint, matched axis by axis), or a different module
  ABI (`GENEVA_MODULE_ABI_VERSION`). A shared object that is not a Geneva module at all, a manifest that
  advertises nothing, and a contribution of a kind this Geneva cannot serve are refused the same way — a
  failed load is never silent. (Boost.DLL itself performs no compatibility check; these gates are Geneva's.)
- **Checkpoints.** A checkpoint stores the concrete individuals, so resuming one requires the **same**
  individual provided the same way (compiled in, or the same `--individual` plugin) that wrote it. Resuming
  without it fails with actionable guidance instead of an opaque deserialization error.
