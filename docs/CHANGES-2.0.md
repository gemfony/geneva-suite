# Major changes in Geneva 2.0 (migrating from 1.11 "Hendaye")

Geneva 2.0 "Puente de Santiago" is a ground-up redesign, so it is a single large
breaking change relative to the 1.x series (last stable: 1.11 "Hendaye"). It is being
stabilized through the 1.99.x development betas. This file collects the **behavioral,
source- and serialization-breaking** changes you need in order to port existing code,
configuration files or checkpoints. The high-level rationale and build prerequisites
are in `CHANGES` and `INSTALL`; this file is the practical upgrade guide.

> **Checkpoints and binary archives from 1.11 (or earlier) no longer load.** The
> individual model, the serialization layout and the stored value representation all
> changed. This is a clean break: re-run optimizations from scratch. JSON
> configuration files are *not* binary and are covered case-by-case below.

---

## 1. Toolchain and build

- **C++23 is now required** (was C++20). Minimum compilers: **GCC ≥ 15** or
  **Clang ≥ 22**, the versions Ubuntu 26.10 ships. The build pins `CMAKE_CXX_STANDARD=23` and rejects anything lower,
  and the exported CMake package advertises `cxx_std_23`, so **downstream consumers of
  the `Geneva::` targets are compiled as C++23 too**. A C++20-only toolchain can no
  longer build or link against Geneva.
- **Boost ≥ 1.91**, built in C++23 mode; **CMake ≥ 3.27**; **Catch2 v3** for the tests.
- **CUDA (optional) raised to ≥ 13.3, and now gated on the device too.** CUDA code (the
  CUDA examples/benchmark, the CUDA RNG backend, the GPU consumer) is enabled only when
  *both* the toolkit *and* the installed driver support CUDA ≥ 13.3; otherwise CUDA is
  auto-disabled with a warning and the rest of Geneva still builds. Escape hatches:
  `SKIPALLCUDA=1` / `-DGENEVA_SKIP_CUDA=ON` (no CUDA); `FORCECUDA=1` /
  `-DGENEVA_FORCE_CUDA=ON` (bypass the *driver* check for build farms/CI, still
  requiring the 13.3 toolkit).
- **A CUDA build starts on machines without an NVIDIA driver.** The GPU consumer loads the
  driver library (`libcuda.so.1`) when it is first used, instead of linking it, so a program
  built with CUDA runs everywhere and uses the GPU where there is one; the CUDA toolkit's
  libraries (NVRTC, cudart, cuRAND) remain link dependencies.
- **The source tree's libraries live under `libs/`** (`libs/<library>/{include,src,...}`),
  no longer under `projects/`. Include paths (`geneva/...`, `courtier/...`) are unchanged;
  scripts or build files that named `projects/` must follow.
- **Container images.** `containers/geneva/` builds an image with a complete, tested and
  installed Geneva (CPU, or with `--cuda`), with Podman or Docker.
- **Processors:** 64-bit **x86-64** or **AArch64** (both first-class). SIMD (AVX2/NEON)
  and a CUDA GPU are used when present but never required.
- **Build and test hygiene changed in ways a 1.11 build script will notice.** Builds are
  **out-of-source only** — `scripts/prepareBuild.sh` refuses to run inside the checkout,
  so a script that configured in place must be pointed at a separate build directory. The
  **installed header set is self-contained**: a consumer includes from the install prefix
  only and never reaches back into the source tree, so an install directory is now a
  complete SDK. And the **test suite is registered with CTest in full and sized by
  label** — every test carries one of S / M / L / XL, measured, and a subset is chosen with
  `ctest -LE` rather than by rebuilding with a different CMake flag; the flags that used to
  hide whole groups of tests behind an always-false option are gone, so those tests simply
  run. `INSTALL` names the sizes and the commands.

## 2. Defining a problem (the individual model)

- **The tree of parameter objects is gone.** `GParameterSet` and the whole
  `GParameterBase` / `GParameterT` / `GConstrainedDoubleObject` / `GDoubleCollection` /
  `GParameterObjectCollection` hierarchy (each node carrying its own adaptor) has been
  removed and replaced by a single **flat genome**.
- **`GOptimizableEntity` is gone; `GGenome` is the hierarchy root.** The two were a
  one-child pair -- the abstract category root and its only derivative -- with the root
  declaring a matrix of typed virtuals that the genome implemented. They are now one
  class. *Migration:* replace every `GOptimizableEntity` with `GGenome`; the include
  `geneva/genome/GOptimizableEntity.hpp` becomes `geneva/genome/GGenome.hpp`. The
  surrounding types keep their names (`GOptimizableEntityFactory`,
  `GOptimizableEntityConstraint`, …). Checkpoints written before this change do not load:
  the entity slice no longer travels under its own archive tag.
- **Author a problem by subclassing `GGenome`** (typically via the CRTP helper
  `GGenomeT<Derived>`). Build a flat, fixed-structure array of double / float / int32 /
  bool values once in the constructor with `GGenomeBuilder` (`addDoubleGroup`,
  `addDouble`, `addInt32Group`, `addBoolArray`, …) and install it with `setGenome()`.
  Read values by position via `streamline<T>()` / `streamlineFP()`.
- **Fitness hook changed: override `std::vector<double> evaluate()`** — a virtual member
  returning the raw fitness values (size 1 for a single-criterion problem, one entry per
  criterion otherwise) — **in place of `double fitnessCalculation()`**, which has been
  removed from the base and every individual. `evaluate()` *returns* the results (it
  sets no fitness itself); the framework applies the feasibility / policy / PROCESSED
  pass. A multi-criterion problem populates every criterion from the one returned vector.
  *Migration:* replace `double fitnessCalculation()` with
  `std::vector<double> evaluate()` returning the raw vector.
- **The genome is pure data; mutation lives on the algorithm.** Adaptors are no longer
  attached to parameters. Author them on an **OA-owned `GAdaptionConfig`** (via
  `oa::makeAdaptionConfig<...>(genome)` and the fluent API) and distribute it
  (`setAdaptionConfig`, `Go2::registerAdaptionConfig`, or `oa::StandaloneAdapter`). An
  adapting algorithm with **no** config is a hard error — there is no auto-derivation.
- **OA state removed from the individual.** The per-individual best-known-fitness and
  stall counters are gone (that bookkeeping lives only on the algorithm). The
  adaption-retry limits `max_unsuccessful_adaptions` / `max_retries_until_valid` **moved
  onto the OA-owned adaption configuration** — the individual's config file no longer
  accepts those keys; set them on the adaption config instead.
- **Population-uniform rules are shared.** Optimization direction, evaluation policy,
  sigmoid parameters and the constraint object were hoisted out of every individual into
  a single reference-counted `GProblemPolicy`, so they travel/checkpoint once per
  population.
- **Candidates hold no random-number engine.** The per-individual RNG (`gr_` /
  `getRandomEngine()`) was removed; a candidate is pure data. Code that drew randomness
  inside `evaluate()` via `getRandomEngine()` should now lease a proxy:
  `auto l = Gem::Hap::randomLeasePool().acquire(); /* draw from *l */;`.
- **Large problem constants** (a training set, a target image) live in the individual
  module's load-once store (`GProblemStoreT`, filled from the factory's `init_()` hook),
  not on each individual.

## 3. Normalized coordinates and dimensionless mutation

- A floating-point parameter is stored in a **normalized internal coordinate**; the
  user-visible value is its affine image. This is invisible above the genome
  read/write layer — `evaluate()` and inspection always see external (user) values.
- A parameter is **bounded** (`addDouble(init, lo, hi)`, `addDoubleGroup`, …) or
  **unbounded** (`addDoublePlainGroup`, `addDouble(init)`, …). A bounded value folds
  into `[lo, hi)` on every write; an out-of-range **external assignment throws**.
- **Mutation parameters are dimensionless fractions of a parameter's range** (σ, σ
  bounds, the GD/CGD step, the PSO velocity fraction): `1.0` = the full range. The same
  default works for any range. Rates/probabilities (`sigma_sigma`, `ad_prob`, …) are
  unchanged.

## 4. Configuration files

- **Configuration files are now strict, standard JSON** (previously Boost.PropertyTree /
  XML). Boost.PropertyTree has been removed from Geneva entirely; the individual→tree
  dump moved to JSON as well. Configs use native JSON scalars (numbers, booleans).
- **The external-evaluation protocol was rewritten** (a breaking change for external
  evaluators; the bundled `evaluator.py` was rewritten to match).
- A generated **configuration reference** lives in `docs/config-reference/`. Programs can
  materialize/refresh their config files with `--update-configs` (non-Go2 programs honor
  it too).

## 5. Parallelization and consumers

- **One consumer per process.** A process uses a single consumer held in a process-global
  `GConsumerRegistry`; an algorithm submits to it and gets back a fully-evaluated
  population, transport-agnostic. The former per-algorithm broker/executor injection —
  `setBroker` / `setLocalConsumer` and the `GBrokerT` / `GExecutorT` wrappers — has been
  **removed**. Serial execution is just the local thread consumer (`stc`) with one worker.
- **Meta-optimization** (tuning an algorithm's own parameters) is now the single facility
  `GMetaEvolutionaryAlgorithm` (example 10), which evaluates its umbrella individuals on
  its own orchestration pool while their sub-optimizations submit to the one process
  consumer — so the consumer is never re-entered.
- **The MPI consumer is a module of its own.** `libgemfony-geneva-mpi` carries the `mpi`
  consumer and `GMPISubClientOptimizer` / `GMPISubClientIndividual`; no other Geneva library
  links MPI any more, so Geneva programs start on machines without an MPI library. The module
  is installed next to `libgemfony-geneva`, which loads it when the consumer catalogue is
  first consulted; `--consumer mpi` exists when that load succeeds, and otherwise the error
  names the reason. **Source-breaking for users of the sub-client classes:** a program that
  uses `GMPISubClientOptimizer` or `GMPISubClientIndividual` links `gemfony-geneva-mpi`
  (out of tree: `Geneva::geneva-mpi`) and MPI itself, in addition to Geneva.
- **The GPU consumer is a first-class mnemonic** (`--consumer gpu`, when built with
  `GENEVA_BUILD_WITH_GPU_CONSUMER`). A GPU problem contributes only its device marshaller
  via `Gem::Geneva::registerGPUMarshaller<...>(...)`; Go2 builds/selects the consumer
  like any other.
- **The networked protocol is two-layered: courtier frames, Geneva messages.** Courtier owns
  a closed frame vocabulary (`GFrameKind`: pull / no-work / work / return / shutdown /
  blob-request / blob-reply) plus the per-slot `GProcessingOutcome` it reconciles on, and
  nothing else. What a message *means* is Geneva's: a numeric tag (`geneva_command`:
  `evaluate` / `evaluation` / `evaluated_and_modified` / `terminate`) and a per-command
  payload, both relayed by courtier without interpretation. A library plugs its own answers
  in by specializing `Gem::Courtier::GWireProtocolT<work_item_type>`; courtier links no
  Geneva code and knows no Geneva vocabulary.
- **A processed item returns its RESULTS, not a hollowed-out copy of itself.** The ordinary
  return is an `evaluation` carrying the computed result store plus the validity level --
  a few hundred bytes whose size does not grow with the individual, its parameters or any
  data the problem definition carries. A worker that genuinely modified the genome answers
  `evaluated_and_modified` with the whole individual instead; the choice is a message type,
  so the per-item `setReturnFullIndividual` flag, the `genome_omitted` encoding and the
  entity/genome member-group split it forced are all **removed**.
- **The shared genome layout is sent once per client** and referenced by a content id
  thereafter. Interning is source-side only: a worker always inlines what it sends back.
  Checkpoint/file serialization stays self-contained (full layout by value).
- **Networked consumers' timeout / death-detection is now user-configurable** through a
  config file.
- **The EA post-optimizer always refines inline.** `GEvolutionaryAlgorithmPostOptimizer`
  is constructed from the inner-EA config file alone; the former `execMode` constructor
  parameter and `setExecMode` / `getExecMode` have been **removed** (the nested
  refinement never submits to the process consumer, so there was no mode to select).

## 6. Packaging: libraries and runtime-loadable modules

- **A new foundation library, `gemfony-weft`, carries Geneva's serialization engine, and
  Boost.Serialization has been removed entirely.** `Gem::Weft` (public headers under `weft/`)
  is a small, standalone serialization codec — an intrusive/non-intrusive `serialize` contract
  with a compact flat-binary codec (`GEM_BINARY`, the networked-wire default) and a
  human-readable JSON codec (`GEM_JSON`, the checkpoint default) — that depends on nothing from
  Geneva and only on Boost.JSON. It is the deepest layer of the collection: `gemfony-common`
  links it, and it is now Geneva's **only** wire and checkpoint codec. Geneva no longer links
  `libboost_serialization` (dropped from the build, the installed `FindGeneva` config, and the
  Debian dependency list). The change is transparent to problem code (`evaluate()` and the genome
  API are unaffected); a hand-written `serialize()` uses `Gem::Common::archive_named` /
  `archive_named_base` and a polymorphic wire/checkpoint type is registered with
  `GEM_REGISTER_ARCHIVABLE` (replacing `BOOST_CLASS_EXPORT`). The old Boost text/XML/binary
  serialization modes are gone; only `GEM_BINARY` and `GEM_JSON` remain. Downstream consumers link
  Weft automatically through the exported `Geneva::` targets.
- **Geneva's Boost link footprint is now just Boost.JSON and Boost.ProgramOptions.** Beyond
  Boost.Serialization (above), **Boost.Filesystem has also been dropped**: Geneva's own code uses
  `std::filesystem` throughout, and the plugin loader (Boost.DLL) is compiled with
  `BOOST_DLL_USE_STD_FS`, so it too uses `std::filesystem` / `std::system_error`. `Boost::filesystem`
  was removed from the library link lines, the `GENEVA_BOOST_LIBS` component list, the installed
  `FindGeneva` config, and the `libboost-filesystem-dev` Debian dependency. An installed Geneva
  library now links only `libboost_json` and `libboost_program_options` (Boost headers are still
  needed at build time, e.g. Boost.Asio/Beast for the networked consumers and Boost.DLL for module
  loading).
- **The Geneva library ships no concrete optimization individual.** The reusable sample
  problems were folded into `gemfony-geneva` (`Gem::Geneva::Individuals`); there is no
  separate `geneva-individuals` library.
- **Individuals, optimization algorithms and GPU marshallers can be shipped as
  runtime-loadable `.so` modules** (`GENEVA_DECLARE_INDIVIDUAL` /
  `GENEVA_ADD_INDIVIDUAL_MODULE`, `individualManifest<>` / `oaManifest<>` /
  `marshallerManifest<>`, and `consumerManifest<>` for a consumer -- the MPI module is one),
  loaded with `--module <path>.so` (repeatable) — or, for the one
  module that carries the optimization problem, with `--individual <path>.so`. Example 20's
  `README.md` is the reference author path for an algorithm module, example 18's for a problem.
- **A module load either succeeds or says exactly what is wrong with the module.** Two gates run
  before any of a module's C++ is touched: the toolchain-compatibility fingerprint
  (`GenevaCompat` — compiler, standard library, Boost, build-mode ABI switches and Geneva version,
  matched axis by axis) and the **module-ABI stamp** `GENEVA_MODULE_ABI_VERSION`, which covers this
  release's manifest layout, contribution-kind numbering and per-kind payload types. The stamp is
  what catches a module built against a different `common/GModuleManifest.hpp` within one Geneva
  version — something the version number alone cannot see. Beyond the gates, every other defect is
  a refusal naming the module and the fault rather than a silent partial load: a shared object with
  no manifest entry point, a manifest that advertises nothing, a contribution without its factory
  entry point, a contribution of a kind this Geneva cannot serve (previously skipped in silence),
  a mnemonic or device target that is already taken, and a module named with `--individual` that
  carries no individual. Module authors never write the stamp by hand — the manifest helpers, and
  the `GENEVA_MODULE_ABI_STAMP` initializer prefix for a hand-written multi-contribution manifest,
  supply it.
- **An optimization-algorithm factory is now itself the provider the algorithm store holds.**
  `GOAFactoryT` implements `Gem::Common::GProviderT` directly, so the wrapper `GOAFactoryProviderT`
  is gone, and a factory registers through one function, `Gem::Geneva::registerOptimizationAlgorithm()`
  — the same one whether the algorithm is built in (via `GInitializerT`) or loaded from a module (via
  `oaManifest<>` and the module loader). Consequences for existing code: the per-algorithm factory
  types and their headers (`GEvolutionaryAlgorithmFactory`, `GSwarmAlgorithmFactory`, … and
  `geneva/oa/G*Factory.hpp`) are **gone**. For every algorithm but one the factory added nothing to
  the scaffold, so the factory *is* the scaffold: write
  `GOptimizationAlgorithmFactoryT<GTheAlgorithm>` where the old type name stood (the algorithm's
  own personality traits are the default second argument), and include the algorithm's header and
  `geneva/oa/GOptimizationAlgorithmFactoryT.hpp`. `GParameterScanFactory` is the exception and remains
  a class of its own — it adds the parameter-spec command-line option; a user-written factory that
  derived from one of the removed types derives from the instantiation instead. `GOAFactoryT` gains
  `provide()` and a `final` `getName()` (the provider-interface spelling of `getAlgorithmName()`).

## 7. Renames and small API removals

- **The genome-only marker is `using genome_only = void;`** (was
  `using gemfony_flat_individual = void;`). A leaf individual whose state is its genome
  declares it to opt into the generated empty member list; the meaning is unchanged.

- **The best-individual accessors hand out sole ownership** (source-breaking; the return type
  changed, the semantics did not — these accessors always returned a fresh clone). On every
  optimization algorithm and on `Go2`:

  | before | after |
  |---|---|
  | `std::shared_ptr<T> getBestGlobalIndividual<T>() const` | `std::unique_ptr<T> getBestGlobalIndividual<T>() const` |
  | `std::vector<std::shared_ptr<T>> getBestGlobalIndividuals<T>() const` | `std::vector<std::unique_ptr<T>> getBestGlobalIndividuals<T>() const` |
  | `std::shared_ptr<T> getBestIterationIndividual<T>() const` | `std::unique_ptr<T> getBestIterationIndividual<T>() const` |
  | `std::vector<std::shared_ptr<T>> getBestIterationIndividuals<T>() const` | `std::vector<std::unique_ptr<T>> getBestIterationIndividuals<T>() const` |

  Code that binds the result with `auto` needs no change; code that spells the type replaces it
  with `auto` (or with the `unique_ptr` spelling). A caller that genuinely wants shared ownership
  converts at its own site — `unique_ptr` converts to `shared_ptr` implicitly. Storing the result
  in a container of `shared_ptr` needs a `std::move`. Two follow-on signatures moved with them: the
  smart-pointer `operator<<` overloads of the example/library individuals take a
  `const std::unique_ptr<T>&`, and `GExternalEvaluatorIndividual::archive()` takes a
  `std::vector<std::unique_ptr<GExternalEvaluatorIndividual>>`.
- **Several public headers moved to sit with the layer they belong to** (source-breaking
  `#include`-path changes; update the paths). The flat-genome / entity headers moved from
  `geneva/ind/` to `geneva/genome/`; the entity multi-constraint headers (`GMultiConstraintT`,
  `GIndividualMultiConstraint`) moved under `geneva/genome/`; the pluggable optimization
  monitors (`GPluggableOptimizationMonitors`) and the nested-EA refiner (`GPostProcessorT`)
  moved under `geneva/oa/`; and the generic OS-signal handler `GSigHupHandler` moved from
  `geneva/` to `common/` (namespace `Gem::Geneva` → `Gem::Common`, so register it as
  `Gem::Common::sigHupHandler`).
- **The vestigial "Flat" qualifier was dropped from the genome layer** (source-breaking;
  update any references to the old `*Flat*` names).
- **The standalone gradient descent ("gd") is gone.** Plain gradient descent survives as
  the steepest-descent (β = 0) mode of the conjugate gradient descent (`cgd`);
  `GGradientDescent_PersonalityTraits` was removed with it.
- **`GRandomFactory::init()` was removed.** It had been an empty formality for a long
  time: constructing the factory singleton (`Gem::Hap::randomFactory()`) is all that is
  needed, and the producer threads start lazily on the first container request. Drop the
  call; `finalize()` is unchanged.
- **`Gem::Hap::resetRandomFactory()` was removed** (along with the factory's internal
  single-instantiation trap). The random-number factory is a process-global, thread-safe
  singleton handed out by `shared_ptr` and shared for the life of the process; there is no
  sound way — and no need — to drop and rebuild it mid-run (randomness is non-deterministic
  by design, the producer-thread count has its own setter, and the singleton's destructor
  joins the threads at static teardown). Access it only through `Gem::Hap::randomFactory()`.
  The generic `Gem::Common::GSingletonT<T>::reset()` is unaffected.
- **The `execMode` enum is gone.** It was the selector of the removed per-algorithm
  broker parallelization model; under the one-consumer model the choice is simply which
  consumer the process registers. The direct-mode examples/benchmark keep their numeric
  `--parallelizationMode 0|1|2` command-line option (now a plain integer).
- **The three self-test hooks left the universal base and became an opt-in interface**
  (source-breaking for any class that implemented them, and for any code that called them
  through a base pointer). `modify_GUnitTests()`,
  `specificTestsNoFailureExpected_GUnitTests()` and
  `specificTestsFailuresExpected_GUnitTests()` — and their protected `*_()` counterparts —
  are no longer members of `Gem::Common::GCommonInterfaceT`. They now live on the standalone
  `Gem::Common::GSelfTestable` (`common/GSelfTestable.hpp`), which a class inherits
  **unconditionally** when it has self-tests to contribute and does not inherit at all when
  it has none. To port a class that has tests: add `, public Gem::Common::GSelfTestable` to
  its base list and keep the `override`s exactly as they were — the signatures, the
  `#ifdef GEM_TESTING` bodies and the `Gem::Common::condnotset()` fallbacks are unchanged.
  A class with no tests of its own simply drops its empty implementations.
  The **category roots deliberately do not inherit the interface**: `GGenome`,
  `GPersonalityTraits`, `GBasePluggableOM`, `GOptimizationAlgorithmBase`,
  `GPreEvaluationValidityCheckT`, `Gem::Dietrich::GBasePlotter` and the CRTP container
  mixins expose their own test bodies as *protected, non-virtual helpers* instead, so a
  derived class that opts in chains to them by qualified call exactly as before
  (`GGenome::modify_GUnitTests_()`). Consequences worth knowing:
  - a user individual that writes no tests carries **no** test-related vtable slots — which
    is what makes a runtime-loadable module (compiled `-UGEM_TESTING`) layout-compatible
    with a testing-enabled core by construction rather than by convention;
  - code that walks a heterogeneous population must cast:
    `if(auto *st = dynamic_cast<Gem::Common::GSelfTestable *>(p); st != nullptr) { … }` —
    an element without the facet is skipped, which is what the old no-op default did;
  - the templated standard-test harnesses now `static_assert` that the tested type opts in,
    so a type cannot silently lose its round-trip coverage.
- **A custom optimization algorithm no longer has to override `actOnStalls_()`.** The
  hook now has an empty default on `GOptimizationAlgorithmBase`; override it only when
  the algorithm actually reacts to a stall (as the parent-child EA base does).
- **Geneva's enums stream through one shared, opt-in operator template**
  (`Gem::Common::numeric_enum_io_v` in `GCommonEnums.hpp`) instead of per-enum
  hand-written `operator<<`/`operator>>` pairs. The textual form (the underlying
  number) is unchanged, so configuration files and archives are unaffected; only code
  that took the address of one of the old operator functions needs adjusting. A
  user-defined enum can opt into the same machinery by specializing the marker.
- **dietrich: `project<I>()`/`projectX..W()` on a non-all-double collector is now a
  compile-time error** (it used to compile and throw at run time), and the projections
  are available for every axis of every all-double collector arity.
- **The optimization algorithms' population-sweep hook `runFitnessCalculation_()` was
  renamed to `evaluatePopulation_()`** (a protected virtual on
  `GOptimizationAlgorithmBase`; source-breaking for an out-of-tree algorithm that
  overrides it). The old name referenced the removed `fitnessCalculation()` user hook.
- **courtier: `executor_status_t` was renamed to `submission_status_t`** (and its header
  `GExecutorStatusT.hpp` to `GSubmissionStatusT.hpp`) — the type is the return of the
  submission entry point `workOn` and was named for the removed `GExecutorT` wrapper.
- **Two benchmark directories were renamed** for the same reason: `GBrokerOverhead` →
  `GConsumerOverhead` and `GBrokerSanityChecks` → `GConsumerSanityChecks` (the
  executable names change accordingly).
- **common: `GLogger` is no longer a class template.** The single streamer type it ever
  produced (`GLogStreamer`) is now fixed; code naming `GLogger<GLogStreamer>` drops the
  template arguments. The `glogger` singleton and all streaming behavior are unchanged.
- **common: the parser's reference-parameter proxy classes were removed**
  (`GFileReferenceParsableParameterT` and its vector/array siblings). The
  `registerFileParameter(name, reference, default, ...)` overloads are **unchanged** —
  they now assign through the callback proxy internally; the on-disk config format is
  identical. The test-only helpers `configureFromFile()` and `GParserBuilder::cl_at()`
  were removed as well.
- **dietrich: the public headers no longer re-export all of `Gem::Common`** (the former
  `using namespace Gem::Common;` in every header). The specific common names dietrich's
  own API uses are still imported per-name; a consumer that relied on the blanket
  re-export for its *own* unqualified use of common names must now qualify or import
  them itself.
- **A process-wide thread budget now accounts every pool** (`GThreadBudget` in
  `common/concurrency/`). The budget does accounting only — every reservation is granted in
  full; its single behavioral effect is a warning (at most three per process) when the
  reserved thread total crosses 2x the hardware ceiling, naming the largest reservations —
  previously oversubscription was silent.
- **courtier: the socket consumers/clients gained shared bases** — `GAsioConsumerT` /
  `GWebsocketConsumerT` now derive from `GTcpAcceptingConsumerT` (the common TCP server
  shell) and `GAsioConsumerClientT` / `GWebsocketClientT` from `GPrefetchingClientT`
  (the common prefetch/compute pipeline). Public construction, options and the wire
  protocol are unchanged; only code naming the old direct base classes is affected.

## 8. Checkpointing

- **The checkpoint directory is created lazily**, at the first actual checkpoint write;
  merely configuring an algorithm (`setCheckpointBaseName`) no longer touches the
  filesystem.
- **A final checkpoint is written when a run halts** (file name tagged `final`) whenever
  checkpointing is enabled (`cp_interval != 0`); with checkpointing disabled a run
  performs no checkpoint I/O at all.
- **The personality-traits archive layout changed within the 1.99 development series**:
  the position-only traits of CGD / Nelder-Mead / parameter scan / GSA / ACO / PSO2011 and
  the parent-child traits now serialize their population position through the shared
  `GPositionPersonalityTraits` base (one added nesting level; the GSA/ACO
  `population_position_` and PSO `particle_` tags became `pop_pos_`). Checkpoints written
  by earlier 1.99 development builds do not load; 1.11 checkpoints never loaded in 2.0
  anyway (see the top of this document).

---

*For the day-to-day authoring model with worked examples, see
`docs/writing-optimization-problems.md`, `libs/geneva/quickstart/` (minimal) and
`libs/geneva/examples/03_GParameterObjectUsagePatterns/`.*
