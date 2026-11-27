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
  **Clang ≥ 22**, the versions Ubuntu 26.10 ships. The build pins `CMAKE_CXX_STANDARD=23` and
  rejects anything lower, and the exported CMake package advertises `cxx_std_23`, so **downstream
  consumers of the `Geneva::` targets are compiled as C++23 too**. A C++20-only toolchain can no
  longer build or link against Geneva.
- **Boost ≥ 1.91**, built in C++23 mode and with `boost.process.fs=std` (Boost.Process, which
  common now links, then uses `std::filesystem`; INSTALL, "Compiling Boost"); **CMake ≥ 4.0**;
  **Catch2 v3** for the tests.
- **CUDA (optional) raised to ≥ 13.3, and now gated on the device too.** CUDA code (the
  CUDA examples/benchmark, the CUDA RNG backend, the GPU consumer) is enabled only when
  *both* the toolkit *and* the installed driver support CUDA ≥ 13.3; otherwise CUDA is
  auto-disabled with a warning and the rest of Geneva still builds. Escape hatches:
  `SKIPALLCUDA=1` / `-DGEM_SKIP_CUDA=ON` (no CUDA); `FORCECUDA=1` /
  `-DGEM_FORCE_CUDA=ON` (bypass the *driver* check for build farms/CI, still
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
  `GGenomeT<derived_type>`). Build a flat, fixed-structure array of double / float / int32 /
  bool values once in the constructor with `GGenomeBuilder` (`addDoubleGroup`,
  `addDouble`, `addInt32Group`, `addBoolArray`, …) and install it with `setGenome()`.
  Read values by position via `parameterValues<T>()` / `fpParameterValues()`.
- **Parameter values are read under names that say what they do.** The `streamline` family
  is renamed, and the readers return their values. *Migration:* `streamline<T>(vec, am)`
  becomes `vec = parameterValues<T>(am)`; `streamlineInto(dst, am)` becomes
  `copyParameterValuesTo(dst, am)` (the same count returned); `streamlineFP(vec, am)` becomes
  `vec = fpParameterValues(am)`; `streamlineFPInternal(vec, am)` becomes
  `vec = internalFPValues(am)`, or one of the spans `internalDoubleValues()` /
  `internalFloatValues()` where neither activity filtering nor the float channel matters. The
  writers (`assignValueVector`, `assignFPValueVector`, ...) keep their names. The private
  virtual channel functions behind them (`streamline_`, `assignValueVector_`,
  `countParameters*_`, `boundaries_`, `getVarVal_*_`, `streamlineInternal_`,
  `assignValueVectorInternal_`) are gone: the public templates choose the channel at compile
  time, so an individual cannot override how values are read or written.
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
  (`setAdaptionConfig`, `GOptimizer::registerAdaptionConfig`, or `oa::StandaloneAdapter`). An
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
- **A number in quotes is refused.** A number, a boolean or an enumeration that is written as
  a number (`cp_ser_mode`, `sorting_method`, `maxmode`, ...) is written without quotes,
  `"value": 5`; the reader no longer takes `"value": "5"`, the all-strings form of older files,
  as the number 5, but refuses it with a message that names the key and what is expected.
  Enumerations were written in quotes until now: a configuration file written by an earlier
  version is rewritten with `--update-configs` after the quotes around its numbers are removed,
  or created anew.
- **The external-evaluation protocol was rewritten** (a breaking change for external
  evaluators; the bundled `evaluator.py` was rewritten to match).
- A generated **configuration reference** lives in `docs/config-reference/`. Programs can
  materialize/refresh their config files with `--update-configs` (programs without Geneva's
  launcher honor it too: `Gem::Common::finishConfigEmission()` logs the end of the pass and the
  program's `main()` returns after it; a GOptimizer program ends inside `optimize()`).
- **A parameter entry of the wrong form is refused.** Each parameter is an object with a
  `"value"` -- a list for a vector or array parameter -- as in
  `"scan_randomly": { "value": false }`; a combined parameter is an object of such entries. A
  known key in another form (the flat `"scan_randomly": false`, an object without `"value"`, a
  list where one value belongs) was skipped without a word, leaving the default in force; it is
  now an error that names the key and the form it needs. An absent key keeps its default.
  An array parameter refuses a list of another length than its own; a longer list lost its
  surplus, and the missing values of a shorter one kept their defaults. A registration whose
  key in the file is taken is refused as well: two combined parameters with one group label, a
  parameter named like a label or `header`, or a combined parameter with two equal names wrote
  the same entry, and the later one replaced the earlier one.
  So is a value that does not read as its parameter's type: a boolean other than `true`,
  `false`, `1` or `0` (it read as `false`), a number with text or a remainder (`2.5` for an
  integer read as 2), a negative number for an unsigned key (`-1` wrapped around to
  4294967295), and for an enumeration text or a number that is none of its values
  (`"MAXIMIZE"` for `maxmode` read as 0, i.e. minimize). The error names what the value must be.
- **The environment variable `GEM_CONFIG_BASENAME`** names a directory below which a
  program's configuration files are placed (a relative path is resolved against it, the
  directory is created). `GParserBuilder::writeConfigFile()` follows it too; it wrote to the path
  as given, so the file a program wrote was not the one it then read.
- **`GParserBuilder::parseConfigFile()` and `updateConfigFile()` throw** a `gem_exception`
  that names the file and the cause when the file cannot be read or applied, and return
  `void`. They used to log the error and return `false`, which most callers ignored -- a direct
  `readConfigFile()` of an optimization algorithm among them -- so a configuration that could not be
  applied ran on its defaults. Code that tested the result drops the test.
- **`GParserBuilder::parseCommandLine()` throws `Gem::Common::GCommandLineError`** for a
  malformed command line (an unknown option, a value that does not read as its type, a missing
  value). It used to print the error and the usage and end the process. The exception carries the
  help text (`usage()`); a program that does not catch it ends through the fatal report with the
  message and the exit code 1.
- **A Geneva program ends on an error nobody handles with a message and the exit code 1**, not
  with an abort and a core dump: the constructor of Geneva's description
  (`GOptimizationDescription`, built by `runOptimization()`) makes
  `Gem::Common::installFatalReport()` (`common/GFatalReport.hpp`) the terminate handler of the
  process; a program without it calls it in the first line of `main()`. Nothing is unwound on the
  way, so under MPI no process finalizes MPI while others wait for it, and `mpirun` ends the whole
  job. A program that wants its own terminate handler sets it after constructing the description. A `std::terminate()` with no exception in
  flight, an internal fault, still aborts, for the core dump.
- **Processes that start together in one directory without configuration files no longer
  race on creating them** (the ranks of an MPI job, clients started by one script): a missing
  file is written to a temporary name and renamed into place, so no process reads a part of it,
  and a process that finds the file created in the meantime goes on instead of failing with
  "You have specified an existing file". `Gem::Common::writeJsonFile()` and
  `GParserBuilder::writeConfigFile()` write the same way: a failed write (a full disk) is a
  `gem_exception` and leaves the old file, and a symbolic link at the target is replaced by a file.

## 5. Parallelization and consumers

- **A program's `main()`: Geneva on courtier's launcher.** The start of a program -- the command
  line, the modules, the choice of consumer, the role of the process, a client's run -- moved
  from the driver (`Go2`, now `GOptimizer`, see section 7) to courtier's launcher
  `Gem::Courtier::GLauncherT<GGenome>` (`courtier/GLauncherT.hpp`); `GOptimizer` is the
  optimization driver only. What 1.11 and the development snapshots wrote as
  ```cpp
  Go2 go(argc, argv, "./config/Go2.json", user_options);
  if(go.clientMode()) { return go.clientRun(); }
  /* ... */ go.optimize();
  ```
  is now, in the short form,
  ```cpp
  return Gem::Geneva::runOptimization(argc, argv, {
      .userOptions = [&](Gem::Common::GParserBuilder &gpb) { /* gpb.registerCLParameter(...) */ },
      .produce = [&](GOptimizer &go, const GOptimizationDescription &) { /* ... */ go.optimize(); return 0; }});
  ```
  and, in the long form for a program that sets something on the launcher (a consumer chosen in
  code with `setConsumer()`, `addModulePath()`, `registerItemProcessor()` for a client), Geneva's
  description and the launcher are built in `main()`:
  `GOptimizationDescription geneva; GLauncherT<GGenome> launcher(argc, argv, "./config/GOptimizer.json");
  return launcher.run(geneva, {.produce = [&] { GOptimizer go(geneva); /* ... */ return 0; }});`
  (examples 01 and 02 show the two forms, example 11 a choice made on the launcher). The
  producing function runs on the submitter only; `prepare(role)` runs in every process (constant
  problem data, an exit before anything runs). `run()` returns: `--help` gives 0, a malformed
  command line prints the error and the help text and gives 1. With `--waitForClients N` the
  submitter waits before producing until its consumer reports N connected clients, and ends with
  an error after `--clientWaitLimit` seconds (600) with fewer. Removed from the driver: the
  constructor `Go2(argc, argv, config_file, options)`, `clientMode()`, `clientRun()`, `setClientMode()`,
  `registerConsumer()`, `consumer()`, `setConsumerName()`, `getConsumerName()`, `addModulePath()`,
  `setModulePaths()`, `getModulePaths()`, `setMaxClientTime()`, `getMaxClientTime()`,
  `updateConfigsMode()` (ask `Gem::Common::GParserBuilder::updateInPlace()`), `helpRequested()`
  (the launcher's `helpRequested()`), `parseCommandLine()`, `parseConfigFile()`,
  `addConfigurationOptions()` and `getNProducerThreads()`; `GOptimizer()` and
  `GOptimizer(const GOptimizationDescription &)` remain. The keys of `GOptimizer.json` are unchanged
  (`consumer` and `module_paths` are the launcher's, the others Geneva's). A program's own options register into
  the launcher's `GParserBuilder` (`userOptions`), not into a `boost::program_options` group; the
  algorithms' options do the same (`GProviderT::addCLOptions(GParserBuilder &)`, hidden ones listed
  by `--showAll`).
- **One consumer per process.** A process uses a single consumer held in a process-global
  `GConsumerRegistry`; an algorithm submits to it and gets back a fully-evaluated
  population, transport-agnostic. The former per-algorithm broker/executor injection —
  `setBroker` / `setLocalConsumer` and the `GBrokerT` / `GExecutorT` wrappers — has been
  **removed**. Serial execution is just the local thread consumer (`stc`) with one worker.
- **Meta-optimization** (tuning an algorithm's own parameters) is now the single facility
  `GMetaEvolutionaryAlgorithm` (example 10), which evaluates its umbrella individuals on
  its own orchestration pool while their sub-optimizations submit to the one process
  consumer — so the consumer is never re-entered.
- **Every consumer is a module of its own**, built for `GGenome` like a module of a third party
  (one line on courtier's provider: `consumerManifest<GStdThreadConsumerProviderT<GGenome>>()`) and
  installed next to `libgemfony-geneva` as `libgemfony-geneva-<consumer>.so` (`stc`, `beast`,
  `mqtt`, `mpi`, `gpu`, as built); the launcher loads the chosen one (`Gem::Geneva::consumerModules()`
  says where they lie), `stc` when none is named. Geneva's own consumer catalogue
  (`geneva/GConsumerSetup.hpp`, `geneva/GConsumerPlugin.hpp`) is removed. `libgemfony-geneva-mpi`
  carries the `mpi` consumer. The process's one MPI environment lives in courtier's MPI library
  `libgemfony-courtier-mpi`, which the module links:
  `Gem::Courtier::mpiEnvironment()`, `mpiEnvironmentOn()` and `mpiProcessRole()` in
  `courtier/mpi/GMPIEnvironmentAccess.hpp`, next to the environment class
  (`courtier/mpi/GMPIEnvironment.hpp`, formerly under `courtier/transport/`). No other Geneva
  library links MPI, so Geneva programs start on machines without an MPI library; asked for
  `--consumer mpi` where the module cannot be loaded, the launcher names the module and the
  reason. No program links the module: it is a module library, not an exported
  target (`Geneva::geneva-mpi` is gone). A program that names the environment's functions links
  `gemfony-courtier-mpi` (`Geneva::courtier-mpi`, which carries MPI; `find_package(Geneva)`
  finds MPI for it).
- **Parallelism inside one evaluation: `GMPISubClientOptimizer` and `GMPISubClientIndividual`
  are removed; their replacement is courtier's MPI cascade**, `Gem::Courtier::GMPICascade`
  (`courtier/mpi/GMPICascade.hpp`, in `libgemfony-courtier-mpi`, `Geneva::courtier-mpi`). It
  splits the job into the submitter and groups of a lead and its members, as before with
  1 + clients x group size processes. Porting: the sub-client job given to
  `registerSubClientJob()` becomes a `GMPIGroupMemberI` that `cascade.serve()` runs on every
  member; an individual that used `getCommunicator()` in `evaluate()` becomes MPI-free, and the
  lead's item processor (`registerItemProcessor()` of the launcher, which hands it to the lead's
  client) evaluates it in a round,
  `cascade.round()`; `sub_client_group_size` is the same key in `config/GMPICascade.json`
  (`GMPICascadeConfig::load()`). Example 16 shows it. `courtier/GMPIHelperFunctions.hpp`
  (`mpiScatterWhile()`, `mpiGatherWhile()`, `MPICompletionStatus`, ...) is removed with them; its
  replacement is `courtier/mpi/GMPIPolledCalls.hpp` (`mpiScatter()`, `mpiGather()`,
  `waitPolled()`, `probePolled()`).
- **A program that builds its consumer without the launcher passes the configuration struct of
  the consumer's family** to courtier's functions, with the location of the consumer modules:
  `Gem::Courtier::buildConsumer<GGenome>(GStdThreadConsumerConfig{.nThreads = 1},
  Gem::Geneva::consumerModules())`, `buildConsumerClient<GGenome>(GWebsocketConsumerConfig{.ip =
  "server", .port = 10000}, Gem::Geneva::consumerModules())` (`courtier/GLauncherT.hpp`; also
  `consumerProvider<T>()` for the role of a process, `ensureProcessConsumer<T>()`). The struct names
  the mnemonic (`MNEMONIC`), so struct and consumer cannot be mismatched, and a misspelt member does
  not compile. The structs are `GStdThreadConsumerConfig`, `GWebsocketConsumerConfig` and
  `MPIConsumerConfig` (`courtier/consumers/G*ConsumerConfig.hpp`; none includes a network or MPI
  header). Development snapshots of the redesign had a `ConsumerSpec`
  with one field per option of every consumer and a function `buildConsumerSetup()` returning a
  `ConsumerSetup`; `Gem::Courtier::ConsumerSpec` (`courtier/GConsumerSpec.hpp`) is now a mnemonic
  plus a `std::any config` (empty: the family's defaults), for a mnemonic known only at run time,
  `buildConsumer()` returns the consumer, and a provider reads the struct with
  `Gem::Courtier::configOf<config_type>(spec)`, which names both types when they differ.
- **The GPU consumer is a first-class mnemonic** (`--consumer gpu`, when built with
  `GEM_BUILD_WITH_GPU_CONSUMER`). A GPU problem contributes only its device marshaller
  via `Gem::Geneva::registerGPUMarshaller<...>(...)`; the launcher builds the consumer like any
  other, after checking that a marshaller is there.
- **The networked protocol is two-layered: courtier frames, Geneva messages.** Courtier owns
  a closed frame vocabulary (`GFrameKind`: hello / pull / work / result) plus the per-slot
  `GProcessingOutcome` it reconciles on, and nothing else. The end of a run is said by the transport's close, not by a frame: the
  `shutdown` frame, Geneva's `terminate` command and `GWireProtocolT::shutdownTag()` are gone. What
  a message *means* is Geneva's: a numeric tag (`geneva_command`: `evaluate` / `evaluation` /
  `evaluated_and_modified`) and a per-command payload, both relayed by courtier without
  interpretation. A library plugs its own answers in by specializing
  `Gem::Courtier::GWireProtocolT<work_item_type>`; courtier links no Geneva code and knows no Geneva
  vocabulary.
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
- **Networked consumers set no time limit by default.** A lost client is noticed by its
  connection (both transports keep signs of life), and what it held is handed out again at once.
  `config/GNetworkedConsumer.json` holds two limits, both 0 (none) by default: `reclaim_lease_ms`
  (an item in flight longer than this is handed out again -- the only bound on a client that hangs
  inside an evaluation while its process answers) and `give_up_window_ms` (a batch without any
  result for that long is given up; the submission policy decides the rest). A negative value is
  refused. The estimate from measured return times (the treatments `adaptive` / `fixed` /
  `wait_indefinitely` and their ten keys) is gone.
- **A websocket (`beast`) client that finds no work waits at the server instead of polling.**
  The server keeps the request and answers it when an item becomes available; one waiting client
  is woken per item, the one that has waited longest first. A client thus gets its next item as
  soon as there is one, and idle clients cause no traffic beyond the keep-alive. Measured with
  400 clients on one host, an iteration took 1 to 8 ms longer than its evaluations, where the
  fixed pause used to add 70 to 200 ms; 360 idle clients cost the server 0.4 % of a core. `mpi`
  clients now do the same (next item).
- **The MPI consumer works as the websocket consumer does.** Its client waits at the submitter when
  there is no work (the pause of 50 to 200 ms after NO_WORK, and the cost it added to every
  iteration, are gone) and fetches the next item while it evaluates the current one. A client opens when the submitter's consumer has started: the consumer
  greets every client rank, and `--mpi_firstContactLimit` (seconds, default 0: as long as it runs)
  bounds the wait for it. A submitter without a client rank (a program started as one process, with
  or without the launcher) refuses to start, says how the program is started, and ends with a
  non-zero code; it used to wait for ever. A client honours its time and item limits and a close
  request, which had no effect on an MPI client before: it sends the results of the evaluations that
  run and leaves; the submitter goes on with the others and hands out again what it held. At the end
  of a run every client is told; nothing waits for an answer to that close (a close proves
  nothing; what a client held is put back by the lease). A send that is still under way when the
  work ends (a large item for a rank that has left, say) is waited for up to 2 s
  (`GMPIPEERDRAINGRACE`) and then given up. The consumer answers
  `connectedClients()` (the clients that have asked for work), so the launcher's
  `--waitForClients` works for MPI. The default depth is 2 for MPI and 1 for websocket. The submitter serves waiting requests oldest
  first and does not ask whether their client is evaluating. While items are pending no request
  waits, and one sent ahead takes an item nobody waits for; but when fewer items become available
  than requests wait (between iterations, or when items are put back), or a new request comes while
  waiting ones are being woken, a request sent during an evaluation can take an item while an idle
  client waits for the next one. A submitter whose consumer is destroyed without having been started
  tells its clients that the run has ended, and they end at once; so does a submitter whose side of
  a client fails (a failed MPI poll, a session that cannot be made, the end of its work before a
  close went out): no client is left waiting for a submitter that has gone. The ranks keep signs of
  life: `--mpi_pingInterval` (seconds, default 15; 0: off) is set on the submitter and named to
  every client when it is greeted; each side sends an empty message to the other that often, and a
  rank not heard from for more than twice that long is lost. A lost
  client's items are handed out again and the run finishes with the others; a client whose
  submitter is lost ends. MPI_Finalize would wait for a lost rank, so the job is then ended
  (MPI_Abort, exit code 1) where MPI would be finalized; a program that initialized MPI itself is
  told so in the log. A client whose submitter ends without starting its consumer and without
  telling it (a program that exits before it builds its consumer) waits until the job is ended.
  Before 1.99, a worker ended after 120 s without word from its master. A message is limited to INT_MAX bytes (about 2 GiB): an item that
  would be longer fails where it is handed out, a result that would be longer comes back flagged;
  the connection, and the run, go on. Options: `--mpi_asyncReq` is removed --
  `true` is now `--mpi_prefetchDepth 2` (the default), `false` is `--mpi_prefetchDepth 1`;
  `--mpi_parallelEvaluations` (default 1) says how many of the items held are evaluated at once;
  `--mpi_nHandlerThreads` is the number of threads the submitter's sessions run on, default 2 (it
  was one per hardware thread; 0 still means that), and one more thread polls MPI for every client;
  `--mpi_cleanSessInterval` is removed. In `MPIConsumerConfig`, `useAsyncReq`,
  `masterCleanSessIntervalMSec` and `nHandlerThreadsRecommendation()` are replaced by
  `clientPrefetchDepth`, `clientParallelEvaluations` and `clientFirstContactLimit`. `GMPIConsumerWorkerNodeT` is a `GPeerClientT` and must be held by a
  `std::shared_ptr`.
- **One keep-alive for both ends of a websocket connection, set by the server.**
  `--beast_pingInterval` (default 15 s) applies to the server and to the client; the server names
  its value in its first message, and on a client the value holds until its server has named its
  own: after that much silence a ping is sent, and after twice as much without any inbound data the
  connection is closed. The same limit bounds the
  websocket handshake and the close handshake, so a peer that has stopped answering cannot hold
  a connection or a thread. A client answers pings while it evaluates, and both ends answer them
  while they encode or decode a message (neither does that on the thread that moves the
  connection's bytes), so an evaluation, and the encoding of a message, may take any length of
  time. A connection is accepted, opened and kept alive while every session thread of the server
  is busy: the server takes a new client on, and answers its handshake and its pings, on the
  threads that move the bytes. There is no limit on the size of a message. Without the keep-alive
  (an interval of 0) a lost client is not noticed by the transport; only a `reclaim_lease_ms` the
  user sets brings its items back.
- **The number of threads of the websocket server** is set with `--beast_nListenerThreads`
  (default 4; 0 means one per hardware thread), as in 1.11; the server runs its sessions on as
  many threads again.
  Development snapshots of the redesign ignored these options and ran one io thread per hardware
  thread. Measured with 400 websocket clients and evaluations of 10 ms, an iteration took 25.8 ms
  with 4 threads, 26.8 ms with 2 and 26.5 ms with 8; the server needed 1.4, 1.1 and 2.4 cores.
- **The asio consumer is removed; the websocket consumer takes its place.** A program started
  with `--consumer asio` or with an `asio_*` option is refused like one with any unknown consumer
  or option. Use `--consumer beast` with the same host and port options under their websocket
  names: `--beast_ip`, `--beast_port`, `--beast_serializationMode`, `--beast_nListenerThreads`
  and `--beast_prefetchDepth`. Code that built the consumer from `GAsioConsumerConfig` uses
  `GWebsocketConsumerConfig`, whose members are the same but for `maxReconnects`.
  `--asio_maxReconnects` (attempts of one exchange, about 7 s by default) has no counterpart; see
  the next item. The key
  `session_timeout_ms` of the networked timeout configuration (the asio server's limit on one
  exchange) is gone; the websocket keep-alive covers a client that falls silent, and
  `--update-configs` drops the key from an existing file. The classes `GAsioConsumerT`,
  `GAsioConsumerClientT` and `GTcpAcceptingConsumerT`, the constants `GASIO*` and the frame's
  announced peer id (`GCommandContainerT::setPeerId()` / `peerId()`) are gone with it.
- **The websocket server listens on an address of the user's choice.** `--beast_listenAddress`
  (`GWebsocketConsumerConfig::listenAddress`) names the IP address, IPv4 or IPv6, that the server
  listens on: 127.0.0.1 for clients that come through an ssh tunnel, or the address of one
  interface. The default, 0.0.0.0 (every IPv4 interface), is where the server always listened
  before. An address that is no IP address, or of no interface of the host,
  ends the server's start with an error that names it. `--beast_ip` stays the address under which
  the clients reach the server. Geneva assumes a trusted network (INSTALL, "Distributed runs need
  a trusted network").
- **A websocket client connects again.** It may be started before its server: a failed attempt
  is followed by another after a pause that grows from 0.25 to 30 s, until
  `--beast_firstContactLimit` seconds (default 0: no limit) pass without contact (the server's
  greeting). A client whose connection is lost (reset, closed by the server, or cut
  by the keep-alive) connects again until `--beast_reconnectLimit` seconds (default 600; 0: no
  limit) pass since the end of the last connection that had contact; a limit that runs out ends it
  with an error. It sends again, once, the results the server may not have read, evaluates what it
  holds, and delivers the results on the next connection; the server drops a result it already
  has, or one of another server process. This is best effort: a result a client still holds when
  it ends is dropped, and the server hands its item out again.
- **A websocket client evaluates one item at a time unless told otherwise.** It has two
  numbers: `--beast_prefetchDepth` n, how many items it holds at once (those evaluated and those
  fetched ahead, so that the transfer of the next item overlaps the evaluation of the current
  one), and `--beast_parallelEvaluations` m (`clientParallelEvaluations`), how many of them it
  evaluates at the same time -- 1 by default, from 1 to n. A client with a depth above 1 used to
  evaluate as many items side by side as its depth; it now evaluates them one after the other.
  `--beast_parallelEvaluations` set to the depth gives that back. In `GPeerClientOptions` the
  member `computeWidth` (0: as many as the depth) is now `parallelEvaluations` (default 1); m
  above n, or 0, is refused when the client is built.
- **A socket server lives as long as it is referenced.** Dropping the last reference stops and
  tears down the server; `stopServer()` joins the io threads and frees the listening port.
- **A websocket server tells its clients that the run has ended.** `stopServer()` closes every
  connection with the close code 4000 (`GCONSUMERCLOSECODERUNENDED`), also those of clients that
  evaluate or wait for work, and waits for the connections to end, at most a grace
  (`GWEBSOCKETCONSUMERSTOPGRACE`, 10 s; the second argument of the consumer's constructor). A
  client that is told so ends. A client that reaches a time or an item limit starts no further
  evaluation, sends the results of those that run as they finish, and closes when none runs; the
  server keeps these results (the processed count may pass an item limit by them). A client that
  is still connecting when a limit is reached gives the attempt up. Both ends close with 1000
  (normal), 1011 (error) or 4000 (end of the run); a close proves nothing about what the other
  end received, and nothing waits for an answer to it.
- **A failure stays with the item it belongs to.** An evaluation that throws on an MPI worker no
  longer ends the worker's rank; the item comes back flagged. Every message has a head that is
  read first and names the item; a message whose head cannot be read ends the connection. A
  client that cannot read an item returns it as failed and stays; the server sends it its blobs
  anew. A server that cannot read a result fails its item. A result that cannot be
  applied to its item marks the item as failed, and so does an item that the server cannot encode
  for a client, or whose result the client cannot encode or return (the client then sends the
  outcome alone, marked as failed, and stays). In each case the submission policy decides what
  happens to the item, as it does for any failed evaluation.
- **A failed evaluation is handled once, by the consumer, and the item says so.** The consumer
  reconciles every batch by the algorithm's submission policy and returns only evaluated items: a
  slot it refills with a copy of an evaluated item is marked (`GProcessable::isSubstitute()`, until
  the item is submitted again). A submitter forbids a substitute with `setSubstitutable(false)`; such
  an item is evaluated again within the policy's budgets, or the run ends. The evolutionary
  algorithm forbids it for its parents, so a parent whose evaluation fails in the first iteration
  ends the run. The algorithms' own code for failed items, which could no longer run, is gone:
  `GOptimizationAlgorithmBase::requireCompleteEvaluation_()` and its calls, the calls of
  `discardUnusableItems_()` in simulated annealing, sep-CMA-ES and the swarm (it stays for the
  meta-EA, which evaluates on a pool of its own), and the failed-vertex case of Nelder-Mead.
  Nelder-Mead, CGD and the parameter scan, whose policy ends the run at a failed evaluation, have a
  setting `max_failed_retries` (`setMaxFailedRetries()`, default 0 as before): a failed evaluation
  is evaluated again up to that many times before the run ends.
- **Several submitters may use the GPU consumer at once**; their batches are queued to one device
  thread.
- **The GPU path keeps the processing contract of a CPU consumer.** A marshaller begins each
  item's processing in `flatten()` (`beginProcessing()`: the item's pre-processor runs before the
  device sees its values) and completes it in `scatter()` (`completeProcessing(results,
  evaluation_time)`: the post-processor runs after). `scatter()` takes the time of the batch's
  device evaluation as a third argument, and each item reports it as its processing time; an item
  whose processing fails fails alone. `process(results)` refuses an item whose processing was
  begun so. `GBaseGPUMarshallerT` does all this for geneva's individuals.
- **A batch may hold up to 2^24 items** (`GNetworkedConsumerT::maxBatchSize()`); a larger span
  is refused with a fatal error. Submit it in several batches.
- **The EA post-optimizer always refines inline.** `GEvolutionaryAlgorithmPostOptimizer`
  is constructed from the inner-EA config file alone; the former `execMode` constructor
  parameter and `setExecMode` / `getExecMode` have been **removed** (the nested
  refinement never submits to the process consumer, so there was no mode to select).
- **An MQTT consumer: `--consumer mqtt`.** Server and clients talk through an MQTT 5 broker
  instead of a port of the server's: `--mqtt_ip` and `--mqtt_port` name the broker (default
  127.0.0.1:1883), `--mqtt_topic` the prefix of the topics (default `gem`; runs that share a
  broker use different ones). `--mqtt_prefetchDepth`, `--mqtt_parallelEvaluations`,
  `--mqtt_firstContactLimit`, `--mqtt_reconnectLimit` and `--mqtt_serializationMode` work as
  their `beast` namesakes. Every connection has an MQTT client of its own at both ends: a lost
  broker connection ends the connection, and its client connects again, as with `beast`; the
  broker's will of a process that died or hangs ends the other end, and its items go back.
  `--mqtt_pingInterval` (default 15 s) is each end's MQTT keep-alive; a client keeps its own
  value (it is fixed when it connects) and says so in its log when its server names another, so
  set it alike on both sides. A client may start long before its server: it waits without a
  limit by default (`--mqtt_firstContactLimit` 0), joining again after two keep-alive intervals
  without an answer and a pause that grows to 30 s (as `beast`'s does after a failed attempt),
  so it finds a server at most both of them after the server has started, about a minute at the
  defaults. An item travels as one packet of at most 2,000,000
  bytes; a larger one fails (`beast` and `mpi` carry larger ones). No encryption and no login, as
  with `beast`: the broker belongs on a trusted network and needs `set_tcp_nodelay` (`INSTALL`).
  With 402 clients it reached 71 % of `beast`'s speed-up at evaluations of 10 ms and the same
  from about 1 s on. Built when the Boost in use has Boost.MQTT5 (`BUILDMQTTCONSUMER`).
- **0 threads means the hardware threads assigned to the process.** `getNHardwareThreads()`
  counts the hardware threads the process may run on -- on Linux its affinity mask, so a job that
  a batch system or `taskset` gives 4 cores of a 64-core node gets 4 -- and every request for 0
  threads uses it: a `GThreadPool` (it started 2), an algorithm's `setNThreads(0)` and
  `n_adaption_threads` 0 (they set 2 with a warning; the 0 is now kept and resolved on the
  machine of the run), the meta-EA's orchestration pool, the thread-pool consumer, the session
  and I/O threads of the networked consumers and hap's producer threads (they counted all
  threads of the machine).

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
- **Geneva's Boost link footprint is now Boost.JSON, Boost.ProgramOptions and Boost.Process.**
  Beyond Boost.Serialization (above), **Boost.Filesystem has also been dropped**: Geneva's own
  code uses `std::filesystem` throughout, and the plugin loader (Boost.DLL) is compiled with
  `BOOST_DLL_USE_STD_FS`, so it too uses `std::filesystem` / `std::system_error`.
  `Boost::filesystem` was removed from the library link lines, the `GEM_BOOST_LIBS` component
  list, the installed `FindGeneva` config, and the `libboost-filesystem-dev` Debian dependency. An
  installed Geneva library now links only `libboost_json`, `libboost_program_options` and, for
  starting external programs, `libboost_process` (Boost headers are still needed at build time,
  e.g. Boost.Asio/Beast for the networked consumers and Boost.DLL for module loading).
- **courtier depends on common (and so on weft) only.** `gemfony-courtier` no longer links `gemfony-hap`, so
  `Geneva::courtier` no longer brings `Geneva::hap` with it: a program that used hap through
  courtier links `Geneva::hap` itself. `Geneva::geneva` links hap directly and brings it as
  before. The demo work items `GSimpleContainer`, `GRandomNumberContainer` and `GFaultyContainer`
  (header `courtier/GDemoProcessingContainers.hpp`) are no longer part of the library: they are
  test code of courtier's own tests and are neither installed nor exported.
- **A project outside the tree finds Geneva with `find_package(Geneva)` only.** The installation
  no longer carries the modules that build the collection itself (`CommonGemBuild.cmake`,
  the modules it includes, and `IndependentBuild.cmake` with its `SETUP_BUILD_SYSTEM` macro);
  its `CMakeModules/` directory holds `FindGeneva.cmake`, the fallback of
  `find_package(Geneva MODULE)`, and nothing else.
- **The Geneva library ships no concrete optimization individual.** The reusable sample
  problems live in `Gem::Geneva::Individuals` and are packaged next to the library (object
  libraries and loadable modules), not compiled into `gemfony-geneva`; there is no separate
  `geneva-individuals` library.
- **Individuals, optimization algorithms and GPU marshallers can be shipped as
  runtime-loadable `.so` modules** (`GENEVA_DECLARE_INDIVIDUAL` /
  `GENEVA_ADD_INDIVIDUAL_MODULE`, `individualManifest<>` / `oaManifest<>` /
  `marshallerManifest<>`, and courtier's `consumerManifest<>` for a consumer -- Geneva's consumers
  are such modules),
  loaded with `--module <path>.so` (repeatable) — or, for the one
  module that carries the optimization problem, with `--individual <path>.so`. Example 20's
  `README.md` is the reference author path for an algorithm module, example 18's for a problem.
- **A module load either succeeds or says exactly what is wrong with the module.** Two gates run
  before any of a module's C++ is touched: the toolchain-compatibility fingerprint
  (`GemCompat` — compiler, standard library, Boost, build-mode ABI switches and Geneva version,
  matched axis by axis) and the **module-ABI stamp** `GEM_MODULE_ABI_VERSION`, which covers this
  release's manifest layout, contribution-kind names and per-kind payload types. The stamp is
  what catches a module built against a different `common/GModuleManifest.hpp` within one Geneva
  version — something the version number alone cannot see. Beyond the gates, every other defect is
  a refusal naming the module and the fault rather than a silent partial load: a shared object with
  no manifest entry point, a manifest that advertises nothing, a contribution without its factory
  entry point, a contribution of a kind this program does not accept (previously skipped in
  silence; the refusal names the kinds it does accept),
  a mnemonic or device target that is already taken, and a module named with `--individual` that
  carries no individual. Module authors never write the stamp by hand — the manifest helpers, and
  the `GEM_MODULE_ABI_STAMP` initializer prefix for a hand-written multi-contribution manifest,
  supply it; the stamp also carries the size of a `GemContribution`. A contribution has a fourth
  member, `subject`, last: for a consumer, the item type its provider distributes
  (`typeid(...).name()`, written by `consumerManifest<>`); a hand-written entry of any other kind
  leaves it out (null). A consumer contribution without a subject, or for another item type, is
  refused before anything of the module is called; one whose provider's `getMnemonic()` is not the
  mnemonic of its manifest entry is refused too. No contribution of a module is taken before all
  of them have been checked (a factory entry point, a kind this program serves, the item type of a
  consumer), so a module refused for one of these registers nothing.
  The loader is `common/GModuleLoader.hpp`, which takes one handler per kind of contribution; Geneva
  uses it through `Gem::Geneva::loadModule()`.
- **Contribution kinds are names with the prefix of their library**: `"geneva.individual"`,
  `"geneva.oa"`, `"geneva.marshaller"`, `"courtier.consumer"` (the constants
  `Gem::Geneva::INDIVIDUAL_CONTRIBUTION_KIND`, `OA_CONTRIBUTION_KIND`,
  `MARSHALLER_CONTRIBUTION_KIND`, `Gem::Courtier::CONSUMER_CONTRIBUTION_KIND`). The numbers
  `GEM_CONTRIBUTION_*` are gone: common's manifest lists no kind of the libraries above it, and a
  program states at each load which kinds it accepts. A hand-written manifest names the constant
  (example 21). Module ABI version 6.
- **An optimization-algorithm factory is now itself the provider the algorithm store holds.**
  `GOAFactoryT` implements `Gem::Common::GProviderT` directly, so the wrapper `GOAFactoryProviderT`
  is gone, and a factory registers through one function,
  `Gem::Geneva::registerOptimizationAlgorithm()` — the same one whether the algorithm is built in
  (via `GInitializerT`) or loaded from a module (via `oaManifest<>` and the module loader).
  Consequences for existing code: the per-algorithm factory types and their headers
  (`GEvolutionaryAlgorithmFactory`, `GSwarmAlgorithmFactory`, … and `geneva/oa/G*Factory.hpp`) are
  **gone**. For every algorithm but one the factory added nothing to the scaffold, so the factory
  *is* the scaffold: write `GOptimizationAlgorithmFactoryT<GTheAlgorithm>` where the old type name
  stood, and include the
  algorithm's header and `geneva/oa/GOptimizationAlgorithmFactoryT.hpp`. `GParameterScanFactory` is
  the exception and remains a class of its own — it adds the parameter-spec command-line option; a
  user-written factory that derived from one of the removed types derives from the instantiation
  instead. `GOAFactoryT` gains `provide()` and a `final` `getName()` (the provider-interface
  spelling of `getAlgorithmName()`).
- **An algorithm states its identity once, on itself.** Next to `class_name` and
  `oa_algorithm_name` an algorithm declares its command-line mnemonic,
  `static constexpr std::string_view oa_mnemonic = "foo";`; the factory scaffold, the module
  registration and `GOptimizationAlgorithmBase::getAlgorithmMnemonic()` (new) read it there. The
  personality traits carry per-individual scratch only: their static `nickname` and the virtual
  `GPersonalityTraits::getMnemonic()` are gone, and `GOptimizationAlgorithmFactoryT` has no second
  template argument. `GPositionPersonalityTraits` is concrete, and the algorithms whose individuals
  need nothing beyond their position install it directly, so the traits classes of Nelder-Mead,
  CGD, ACO, GSA, the parameter scan and SPSO2011 (`G*_PersonalityTraits`), the scaffold
  `GAlgorithmPersonalityTraitsT` and SPSO2011's `setParticle()` / `getParticle()` are gone, and
  simulated annealing installs `GBaseParChildPersonalityTraits` (its own empty
  `GSimulatedAnnealing_PersonalityTraits` is gone); a
  checkpoint names its algorithm in its own first field, so algorithms sharing a traits type are
  told apart there. For a module author (example 20): add `oa_mnemonic` to the algorithm and, if
  its individuals need no state of their own, name `GPositionPersonalityTraits` as its
  `personality_traits_type`, drop the traits class and its `GEM_REGISTER_ARCHIVABLE`. Personality
  traits are part of a checkpoint, so checkpoints of those seven algorithms written before this
  change do not load. `GMetaEvolutionaryAlgorithm` now reports its own name.
- **The mnemonic replaces the personality type.** `oa_personality_type` and
  `getAlgorithmPersonalityType()` are gone; everything that read them reads the mnemonic.
  Users change the key of `GOptimizer::registerAdaptionConfig()`: `"PERSONALITY_EA"` becomes
  `"ea"`, `"PERSONALITY_SA"` becomes `"sa"`, and so on (the meta-EA still shares the EA's key,
  `"ea"`). A checkpoint's first field and its file name carry the mnemonic
  (`checkpoint-ea-<iteration>-...`), so checkpoints written before this change do not load. The check whether a
  checkpoint fits an algorithm is called `cp_mnemonic_fits()` (was `cp_personality_fits()`).

## 7. Renames and small API removals

- **The libraries below geneva and the collection's build say `gem`, not `geneva`.** Their names
  are collection-neutral: inside a name `geneva` became `gem`, `Geneva` became `Gem`, `GENEVA`
  became `GEM`; nothing else changed. What a user changes:
  - the CMake options and compile switches passed with `-D`: `GEM_BUILD_TYPE`,
    `GEM_BUILD_TESTS`, `GEM_BUILD_EXAMPLES`, `GEM_BUILD_BENCHMARKS`,
    `GEM_BUILD_WITH_MPI_CONSUMER`, `GEM_BUILD_WITH_GPU_CONSUMER`, `GEM_USE_CUDA_RNG`,
    `GEM_SKIP_CUDA`, `GEM_FORCE_CUDA`, `GEM_MIN_CUDA_VERSION`, `GEM_SANITIZER`,
    `GEM_BUILD_WITH_COVERAGE` (each was `GENEVA_...`). The keys of `genevaConfig.gcfg` are
    unchanged; a build directory configured with the old names is configured again
    (`prepareBuild.sh <gcfg> --clean -y`). The six tuning switches `GENEVA_COMMON_DEFAULT_BUFFER_SIZE`,
    `GENEVA_HAP_RANDOM_FACTORY_DEFAULT_ARRAY_SIZE`, `..._DEFAULT_BUFFER_SIZE`, `..._DEFAULT_GET_WAIT`,
    `..._SEED_VECTOR_SIZE` and `GENEVA_HAP_FACTORY_QUEUE_PREALLOCATED` are gone: they changed the
    layout of installed classes inside Geneva's own build only. Their values are fixed constants
    in `hap/GHapEnums.hpp` and `common/GCommonEnums.hpp`;
  - the environment variables `GEM_CONFIG_BASENAME` and `GEM_RUN_TIMEOUT` (`startLocalJobs.sh`);
  - the log files a program writes into its working directory: `GEM-EXCEPTION.log`,
    `GEM-TERMINATION.log`;
  - the file `GEM_COMMIT` at the top of the sources and the container build argument
    `GEM_COMMIT` (`--build-arg GEM_COMMIT=$(git rev-parse HEAD)`);
  - the CMake presets `gem-default` (`CMakePresets.json`) and `gem-from-script` (written by
    `prepareBuild.sh`);
  - the functions of the installed CMake package `GEM_STAMP_PROGRAM()` and
    `GEM_MATERIALIZE_CONFIGS()`;
  - in C++: `gem_exception`, the module manifest (`GemModuleManifest`, `GemContribution`,
    `GemCompat`, `GEM_MODULE_ABI_STAMP`, `GEM_MODULE_ABI_VERSION`, `GEM_BUILD_FINGERPRINT`), the exported entry point `gem_module_manifest()` (a module built
    before the rename is not found and is rebuilt), the version macros `GEM_VERSION`,
    `GEM_VERSION_MAJOR` / `_MINOR` / `_PATCH` / `_PRERELEASE` / `_STRING`, and
    `Gem::Common::gemCommit()` in `common/GGemCommit.hpp`.

  What stays: the package (`find_package(Geneva)`, the `Geneva::` targets and every result
  variable of the package, `GENEVA_VERSION` included), `GENEVA_ADD_INDIVIDUAL_MODULE()` and
  `GENEVA_DECLARE_INDIVIDUAL()`, install paths, image and package names, `genevaConfig.gcfg`
  and its keys, the keys of configuration files and the command-line options of programs, and
  everything of the geneva library (`Gem::Geneva`).

- **A parameter-scan or profile specification carries no label.** A variable is addressed by
  its index in the value channel of its type (`d(0, -1., 1., 10)`); a trailing label such as
  `d(0, -1., 1., 10, x)` is refused when the specification is parsed, with a message that names
  it (a progress plot names its variables with `setVariableNames()`). With the label the
  remainder of the by-name addressing left: `parPropSpec<T>::index` replaces the
  `(mode, name, index)` tuple `var` (and the `NAMEANDIDTYPE` alias),
  `GScanParInterface::getVarIndex()` replaces `getVarAddress()`, and `GGenome::getVarVal<T>()`
  takes the index.

- **The genome-only marker is `using genome_only = void;`** (was
  `using gemfony_flat_individual = void;`). A leaf individual whose state is its genome
  declares it to opt into the generated empty member list; the meaning is unchanged.

- **The best-individual accessors hand out sole ownership** (source-breaking; the return type
  changed, the semantics did not — these accessors always returned a fresh clone). On every
  optimization algorithm and on `GOptimizer`:

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
- **`GOptimizer::getBestGlobalIndividual(s)()` return the bests of the whole run** (behaviour change):
  with `copy_best_individuals_only` set to `false` they returned the best of the last algorithm's
  last population, which with comma selection, annealing or a swarm need not hold the best found.
  They now read the algorithms' own archives of best individuals, whatever the setting; the setting
  only says which individuals go on to the next algorithm of a chain.
  `GOptimizer::getBestIterationIndividual(s)()`, which threw, return the best of the last iteration of
  the chain's last algorithm.
- **The EA's iteration accessors describe the last iteration** (behaviour change): in the
  single-criterion sorting modes `getBestIterationIndividual(s)()` returned the best of all
  iterations so far, as the global accessors do; the iteration archive is now replaced in every
  iteration, as in the other algorithms and the Pareto modes.
- **`n_record_best_individuals` sets the capacity of both archives** (behaviour change): the
  iteration archive kept 10 whatever the key said, and a run resumed from a checkpoint had 10 as
  well. `setNRecordBestIndividuals()` / `getNRecordBestIndividuals()` are public, as the setters of
  the other keys are.
- **The archives of best individuals hold each candidate once** (behaviour change):
  `GGenomeFixedSizePriorityQueue` treats individuals with equal parameter values as one entry.
  Under plus selection a surviving parent was recorded again in every iteration, so
  `getBestGlobalIndividuals()` could return the same individual several times (and GOptimizer's merge of a
  chain's archives, too). `GFixedSizePriorityQueueT` has a hook `isSameEntry()` for this, by default
  the same object, as before.
- **`GFixedSizePriorityQueueT` decides which items get a place before it copies them** (behaviour
  change): the archives of best individuals clone only what they keep, not the whole population in
  every iteration. The bulk forms of `add()` / `addClone()` therefore take a forward range (they read
  it twice), and a take-over leaves the handles of the items not kept with the caller. A full queue
  admits a better item at once also after a copy, move, assignment or load (the first item was
  dropped), a bulk insertion fills free places with the best items offered, and a replacing bulk
  insertion of which nothing is admissible throws with the queue left as it was (it was emptied).
  `setSortOrder()` orders the items held at once. A NaN evaluation is worse than every number,
  through `Gem::Common::isBetter(x, y, sortOrder)`, which geneva's `isBetter()` / `isWorse()` use too.
- **Several public headers moved to sit with the layer they belong to** (source-breaking
  `#include`-path changes; update the paths). The flat-genome / entity headers moved from
  `geneva/ind/` to `geneva/genome/`; the entity multi-constraint headers (`GMultiConstraintT`,
  `GIndividualMultiConstraint`) moved under `geneva/genome/`; the pluggable optimization
  monitors (`GPluggableOptimizationMonitors`) and the nested-EA refiner (`GPostProcessorT`)
  moved under `geneva/oa/`.
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
- **`Gem::Hap::bi_normal_distribution::reset()` has the standard meaning** (behaviour change):
  it discards cached values, as the standard distributions do, and keeps the parameters.
  Restoring the parameters of the construction is `restoreParams()`. The class is now
  assignable. The bi-Gauss adaption's "no change" nudge follows the direction of the step.
- **The `execMode` enum is gone.** It was the selector of the removed per-algorithm
  broker parallelization model; under the one-consumer model the choice is simply which
  consumer the process registers. The direct-mode examples/benchmark keep their numeric
  `--parallelizationMode 0|1|2` command-line option (now a plain integer).
- **The self-test hooks are gone; test code left the libraries** (source-breaking for any class
  that implemented them). `modify_GUnitTests()`, `specificTestsNoFailureExpected_GUnitTests()`
  and `specificTestsFailuresExpected_GUnitTests()` are no longer members of
  `Gem::Common::GCommonInterfaceT`, and the `GEM_TESTING` macro, `common/GSelfTestable.hpp` and
  `common/GUnitTestFrameworkT.hpp` no longer exist. The libraries contain no test code and do
  not depend on Catch2. To port a class:
  - its modification for the generic tests becomes an optional, public, non-virtual
    `bool modifyForTest()`, always compiled. The roots carry one (`GGenome`,
    `GOptimizationAlgorithmBase`, `GPositionPersonalityTraits`, `GBasePluggableOM`,
    `GPreEvaluationValidityCheckT`, `Gem::Dietrich::GBasePlotter`); a class that adds members
    declares its own, calls its base's and changes its own members;
  - its `specificTests*` bodies become ordinary Catch2 test cases in its own test program;
  - the standard tests come from the installed header `common/testing/GStandardTests.hpp`
    (namespace `Gem::Common::Testing`: `checkRoundTrips<T>()`,
    `checkMisuse<T>()`, `makeTestObject<T>()`, `GEqualityPrinter`).
    They find `modifyForTest()` with a `requires` expression; a type without one is tested
    unchanged, and the round-trips that need a changed object are skipped. A specialization
    of `makeTestObject` is written in namespace `Gem::Common::Testing`.
  The test individuals `GTestIndividual1/2/3` are no longer installed.
- **A custom optimization algorithm no longer has to override `actOnStalls_()`.** The
  hook now has an empty default on `GOptimizationAlgorithmBase`; override it only when
  the algorithm actually reacts to a stall (as the parent-child EA base does).
- **Geneva's enums stream through one shared, opt-in operator template**
  (`Gem::Common::numeric_enum_values_v` in `GCommonEnums.hpp`) instead of per-enum
  hand-written `operator<<`/`operator>>` pairs. The textual form (the underlying
  number) is unchanged, so configuration files and archives are unaffected; only code
  that took the address of one of the old operator functions needs adjusting. A
  user-defined enum opts into the same machinery by specializing `numeric_enum_values_v`
  with the list of its values (`numeric_enum_range<FIRST, LAST>()` for a contiguous enum);
  the reader fails on a number that is none of them.
- **The names of an enum's values are written once, in a table next to the enum**
  (`Gem::Common::enum_names_v` in `GCommonEnums.hpp`), and read by `enumName(value)` and
  `enumFromName<E>(text)`; the per-enum functions that spelled the names out again are gone.
  The printed names are unchanged. Removed: `Gem::Common::serModeToString()`,
  `Gem::Common::Record::to_string(columnType)`, courtier's `psToStr()`, `fkToStr()` and
  `GPU::toString(BackendKind)`, dietrich's `to_string(plotKind)` and `to_string(legendSample)`,
  and the meta-optimizer's protected `getClearTextMOT()` -- call `Gem::Common::enumName()`
  instead. A number cast to an
  enum that is none of its values prints as `<invalid value N>`. A user-defined enum gets names
  by specializing `enum_names_v` with its value/name pairs; where it also lists its values in
  `numeric_enum_values_v`, a table that misses one does not compile. A test pins the names with
  `Gem::Common::Testing::checkEnumNames<E>()` (`common/testing/GStandardTests.hpp`).
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
- **common: `GConsoleLogger` offers the severity overloads** `log(message, severity)` and
  `logWithSource(message, extension, severity)` through its own type; they were hidden by its
  overrides of the forms without severity. A user's log target that overrides only some of the
  four overloads brings the others in with `using GBaseLogTarget::log;` and
  `using GBaseLogTarget::logWithSource;`.
- **common: the parser's reference-parameter proxy classes were removed**
  (`GFileReferenceParsableParameterT` and its vector/array siblings). The
  `registerFileParameter(name, reference, default, ...)` overloads are **unchanged** —
  they now assign through the callback proxy internally; the on-disk config format is
  identical. The test-only helpers `configureFromFile()` and `GParserBuilder::cl_at()`
  were removed as well, and so were the base classes the two kinds of proxy had shared
  (`GSingleParmT`, `GCombinedParT`, `GVectorParT`, `GArrayParT`): each is folded into its one
  remaining proxy class (`GFileSingleParsableParameterT` and its siblings), whose constructors
  without a comment are gone too, as is the helper `GParsableI::makeVector()` (a braced list
  does the same).
- **dietrich: the public headers no longer re-export all of `Gem::Common`** (the former
  `using namespace Gem::Common;` in every header). The specific common names dietrich's
  own API uses are still imported per-name; a consumer that relied on the blanket
  re-export for its *own* unqualified use of common names must now qualify or import
  them itself.
- **dietrich: one description of how a series is drawn and what its legend shows.**
  `GBasePlotter` has `drawStyle()` (virtual; by default resolved from `plotSpec()`) and
  `legendEntries()` (the legend of a pad, in every backend). A plotter of one's own overrides
  `drawStyle()` where it draws otherwise. The matplotlib, Octave and
  gnuplot writers and the external renderer now draw a series as that description says, as
  ROOT does: a 2-d scatter plot as markers alone, an error-bar curve with its line, a 3-d
  curve as a line through markers.
- **dietrich: ROOT draws from the plot spec, as every other backend.** `GRootEmitter` writes
  the macro from what a plotter reports -- `plotSpec()`, `dataColumns()`, `drawStyle()` -- so a
  plot is described once for ROOT, gnuplot, matplotlib, Octave and the data export. A plotter of
  one's own reports a `plotKind` and its columns and is drawn as that kind. `GPlotSpec` gained
  `function` (a function's formula; its range and sampling are `range_x`/`range_y` and
  `n_bins_x`/`n_bins_y`) and, for a 4-d graph, `marker_size`, `small_w_large_marker` and
  `n_best`; `makePlotter()` now rebuilds functions and 4-d graphs. **Removed** (the plotters'
  own ROOT text): `GBasePlotter::headerData(const std::string &indent) const`,
  `bodyData(...)` and `footerData(...)` of the same signature; the protected virtuals
  `headerData_(bool is_secondary, std::size_t parent_id, std::size_t own_id,
  const std::string &indent) const`, `bodyData_(...)` and `footerData_(...)` of the same
  signature, `drawingArguments(bool is_secondary) const` and
  `rootLegendObject_(bool is_secondary, std::size_t parent_id, std::size_t own_id) const`,
  and `suffix(bool is_secondary, std::size_t p_id, std::size_t own_id) const` -- with their
  overrides in `GGraph2D`, `GGraph2ED`, `GGraph3D`, `GGraph4D`, `GFunctionPlotter1D/2D` and
  `GHistogram1D/1I/2D`; and `GPlotDesigner::staticHeader(const std::string &indent) const`
  (protected). A `GHistogram1I` without a range (minimum equal to maximum) now takes its range
  from its data, as `GHistogram1D` does.
- **A process-wide thread budget now accounts every pool** (`GThreadBudget` in
  `common/concurrency/`). The budget does accounting only — every reservation is granted in
  full; its single behavioral effect is a warning (at most three per process) when the
  reserved thread total crosses 2x the hardware ceiling, naming the largest reservations —
  previously oversubscription was silent.
- **courtier: no pause of a worker after NO_WORK.** A worker's request waits at the server, so
  there is no NO_WORK and no pause after it: `GMPICONSUMERWORKERNODERETRYINTERVALLOWERBOUNDARYMSEC`
  / `...UPPERBOUNDARYMSEC` are **removed**. The pauses, time limits and defaults of the networked
  consumers are named constants in `courtier/GCourtierEnums.hpp`.
- **courtier: the frame protocol.** A connection begins with the server's greeting (`HELLO`: the
  commit of the server's Geneva sources, `Gem::Common::gemCommit()`, the keep-alive interval
  and the Geneva release, `GEM_VERSION_STRING`); a client built from another commit ends with a
  message that names both releases and both commits; before it closes it sends its own `HELLO`,
  and the server logs once which client has left, naming both builds. The fields of the `HELLO` keep their binary
  layout from release to release, and new ones are only appended, so that a client can read the
  greeting of a server of another release.
  Every Geneva build knows its commit: from git (`-dirty` appended when a tracked file has
  changed), else from the file `GEM_COMMIT`, which `git archive` fills in and a build that
  copies the sources without `.git` writes (the container build: the build argument
  `GEM_COMMIT`); a tree without either does not configure. The `HELLO` names the identity of
  the user's program as well (`Gem::Common::programIdentity()`): a client whose program has
  another one ends in the same way; where one side has none, the program is not checked and the
  client says so once in its log. `GEM_STAMP_PROGRAM(<target> [IDENTITY <text>])` of Geneva's
  CMake support stamps an executable with the commit of the project's own sources or a text of
  one's own; the quick start's template stamps its programs. An option of the program that
  changes what it sends (float or double parameters, say) is not seen in the commit: such a
  program names it in its `IDENTITY` (example 14 stamps its float or double build so). A client asks for one item with `PULL`, the
  server answers with one `WORK`, and a client returns an item with `RESULT`, which asks for
  nothing. The frame kinds `NO_WORK`, `RETURN` and `UNREADABLE` are gone, and so are the client's
  files of results it could not deliver (`--beast_undeliveredDir`, `--mpi_undeliveredDir`,
  `clientUndeliveredDir`) and the option `lookAhead` of `GPeerClientOptions`.
- **courtier: the websocket consumer and client gained shared bases** — `GWebsocketConsumerT`
  derives from `GPeerConsumerT` (the server's shell over any transport) and `GWebsocketClientT`
  from `GPeerClientT` (the client over any transport, on `GBaseClientT`).
  Public construction and options are unchanged; only code naming the old direct base classes is
  affected.
- **courtier/geneva: the client data template was removed** (`GBaseClientT`'s constructor
  taking `additionalDataTemplate`, `loadDataTemplate()`, and `loadConstantData()` /
  `loadConstantData_()` on `GProcessingContainerT` and `GGenome`); no transport applied it.
  Constant problem data is loaded in every process (the `prepare()` of the launcher's program).
- **geneva: `GExternalEvaluatorIndividual::setDataType()` / `getDataType()` and
  `GEEI_DEF_DATATYPE` were removed.** The two accessors were declared but never defined, so no
  program could have called them. The `dataType` field of the exchange file names the kind of
  message and is fixed by the operation that writes it; it is not a property of an individual.
- **courtier: `GProcessable::graftOaScratchFrom()` / `graftOaScratchFrom_()` are
  `graftRetainedStateFrom()` / `graftRetainedStateFrom_()`.** The hook takes back from the
  originally-submitted item the state that does not travel on the wire; courtier names it for
  that, not for the use Geneva makes of it. A work-item class that overrides the hook renames
  its override.
- **common: `convertSmartPointer()` and `Gem::Common::erase_if()` were removed.**
  `g_ptr_conversion()` is the one pointer conversion (the template arguments in the same order;
  an empty pointer comes back empty instead of throwing in a Debug build); `std::erase_if`
  replaces `erase_if`.
- **common: `sharedPtrFromString()` throws on a string it cannot decode.** It returned an empty
  pointer and logged a warning; the decode error (a `Gem::Weft::weft_exception`, or another
  `std::exception`) now reaches the caller, as with `sharedPtrToString()`.
- **geneva: the build options `GENEVA_ALGO_SA`, `GENEVA_ALGO_SWARM`, `GENEVA_ALGO_CGD`,
  `GENEVA_ALGO_NM` and `GENEVA_ALGO_PS` were removed.** Every algorithm is always built; an
  algorithm of one's own is added as a loadable module.
- **geneva: `GEndOfPar` was removed** from `GParameterScan.hpp`; nothing threw or caught it.
- **dietrich: `dietrich/plotting/GPlots.hpp` was removed.** `dietrich/GPlotDesigner.hpp` is the
  one umbrella header; a file that needs only some plot types includes `GDataCollectors.hpp`,
  `GGraphPlots.hpp` or `GHistogramPlots.hpp` from `dietrich/plotting/`.
- **common: one way to raise an error, reported where it is handled** (source-breaking). An
  error is raised with `throw gem_exception(g_error_streamer() << ...)`; the message names the
  time, file, line and function of the throw. `raiseException`, the `GEXCEPTION` manipulator,
  `GLogger::throwException()` and the first two arguments of `g_error_streamer` (`DO_LOG` /
  `NO_LOG`, `timeAndPlace()`) are gone. Nothing is logged at the throw: an error a caller handles
  prints nothing, and one is written to the log and to `GEM-EXCEPTION.log` once, where it ends a
  task (the new manipulator `GERROR`, which does not throw) or the program (the fatal report).
  `logType::EXCEPTION` is now `logType::ERROR_REPORT`.
- **common: a check is written one way, and the `DEBUG` macro is gone.** A check calls
  `Gem::Common::checkThat(condition, message...)` from `common/GChecks.hpp`, which checks in every
  build, or `GEM_DEBUG_CHECK(condition, message...)`, which checks only without `NDEBUG`; both throw
  `gem_exception` with the place of the check. Debug builds no longer define `DEBUG`: the checks it
  switched on now run in every build, except the costly ones (the `dynamic_cast` of
  `g_ptr_conversion()`, loops over a population, a check per random number or per parameter read),
  which follow `NDEBUG`. Code that tested `DEBUG` tests `NDEBUG`.
- **common: external programs are started without a shell** (behaviour change).
  `Gem::Common::runExternalCommand` starts the program through Boost.Process with an argument
  vector instead of `system()`: each argument arrives unchanged, spaces included, and shell syntax
  (pipes, redirections, `$VAR`, globs, quotes) is no longer interpreted -- a program that needs it
  is given a script. A name without a directory part is still looked up in `PATH`. It now returns
  the program's exit status (1, where `system()` returned 256), or the number of the signal that
  ended the program, and raises `gem_exception` when the program is not found or cannot be started.
  `GExternalEvaluatorIndividual` passes file names without the quotes it used to add for the shell,
  and splits its `custom_options` at whitespace into separate arguments; quotes in them are not
  interpreted.
- **geneva: `Go2` is `GOptimizer`** (source-breaking; the behaviour is unchanged). The "2" of the
  old name was a pun on `goto`, not a version. What a user changes:
  - the class `Gem::Geneva::Go2` is `Gem::Geneva::GOptimizer`, its header `geneva/Go2.hpp` is
    `geneva/GOptimizer.hpp`, and the constant `GO2_DEF_COPYBESTINDIVIDUALSONLY` is
    `GOPTIMIZER_DEF_COPYBESTINDIVIDUALSONLY`;
  - its configuration file `config/Go2.json` is `config/GOptimizer.json` (the keys are unchanged;
    an existing `Go2.json` is not read -- rename it). The default of
    `GOptimizationDescription::configFile` and the file of a program's `GLauncherT` follow;
  - the snippet anchors the manual reads: `adaptors-go2-register`, `go2-long-form`,
    `go2-user-options#1..3`, `go2-chain#1..2` and `go2-main-quickstart` are
    `adaptors-goptimizer-register`, `goptimizer-long-form`, `goptimizer-user-options#1..3`,
    `goptimizer-chain#1..2` and `goptimizer-main-quickstart`.
- **geneva: `optimize()` returns a reference to the optimizer**, for every optimization algorithm
  and for `GOptimizer` alike (`GOptimizerIT<T>::optimize()` returns `T &`, was a `const` pointer;
  source-breaking where the result was used): a result is read in the same expression,
  `go.optimize().getBestGlobalIndividual<T>()` instead of `go.optimize()->...`; a call that ignores
  the return is unchanged. An algorithm's `optimize()` returns `GOptimizationAlgorithmBase &`.
- **geneva: an algorithm factory's `get()` works without a template argument.**
  `GOAFactoryT::get<target_type>()` defaults `target_type` to the factory's product type, so
  `factory.get()` compiles on a concrete factory (it did not: the member template hid the base's
  `get()`) and returns what `get()` returns through the `GFactoryT` base. `get<T>()` is the way to
  an algorithm; `provide()` stays as the algorithm store's interface.
- **common: a `GFactoryT` refuses an empty product.** When `getObject_()` returns an empty pointer,
  `get()` (and with it every algorithm factory's `get()`, `get<T>()` and `provide()`) and
  `writeConfigFile()` throw `gem_exception` before `postProcess_()` sees the pointer; before,
  `postProcess_()` received it and a dereference there crashed.
- **geneva: `GMETAOPT_DEF_INDCONFIG` was removed** from `GMetaOptimizerIndividualT.hpp`; nothing
  read it.
- **common: `from_string<T>()` throws on a text that is not one value of `T`.** It replaced
  `boost::lexical_cast` but returned whatever the stream had read: "abc" as 0, "12abc" as 12, "1.5"
  as the integer 1, "-1" as an unsigned 4294967295. It now throws `gem_exception` naming the text
  and the type, and so do `splitStringT<>()` and `environmentVariableAs<>()` (which names the
  variable). The reader is the new `Gem::Common::parseWhole<T>()`, which returns an empty
  `std::optional` instead of throwing; configuration values, the parameter-scan specification and
  the record sinks read numbers through it. Surrounding white space and a leading `+` are
  accepted; a `std::string` is the text unchanged (it was its first word), `std::int8_t` and
  `std::uint8_t` are read as numbers (they were read as a character), a `bool` is `true`, `false`,
  `1` or `0`, and "inf" and "nan" are read as numbers. `narrow<>()` refuses a change of sign at
  equal or larger width (`narrow<std::size_t>(-1)`) and a NaN.
  `to_string()` of a floating-point value writes the shortest text that reads back as the same
  value ("0.1", was "0.10000000000000001"); help texts and string-typed configuration values
  change accordingly.
  `touch_time()` appends its content to an existing file and sets its last-write time; it used to
  empty the file.
- **common: the limit passed to `compare()` is applied.** `compare(cp, FP_SIMILARITY, limit)` and
  the free `compare()` used 1e-5 for every floating-point member, whatever `limit` said; the limit
  now reaches every member, the parent slices, the elements of container members and
  `GContainerT::compare_base()`. 1e-5 (`CE_DEF_SIMILARITY_DIFFERENCE`) stays the default. The limit
  travels in the token: `GToken` takes it as a third constructor argument (`getLimit()` returns it),
  and `compare_t()` compares under it. A hand-written `compare_()` creates its token with the
  `limit` it receives, `GToken token("MyClass", e, limit)`; `identity` no longer carries a limit
  (its constructor takes the two items and their names). `GEqualityPrinter` of the standard tests
  applies the limit it is made with.
- **common: in `compare()` two NaN are equal.** `compare()` checks the equality of object states,
  so an object that holds a NaN now equals its clone and itself after a round trip; a NaN and a
  number differ under every expectation. Two equal infinities are similar as well (they failed
  `FP_SIMILARITY`, their difference being NaN).
- **common: `compare()` takes unordered containers by their `==`.** `std::unordered_set`,
  `std::unordered_map` and their multi forms were compared in iteration order, so two equal
  containers with different bucket counts were reported as different. A `std::map` (whose pairs
  cannot be printed) did not compile in `compare()`; it does now.
- **common: the struct type traits of `GTypeTraitsT.hpp` were removed.** `has_compare_member`,
  `has_clone_member`, `has_load_member` and `has_gemfony_common_interface`: the second and third
  were false for every class of the common interface (it overloads `clone()` and `load()`), and
  nothing in the collection used any of them. A type is tested with the concept
  `gemfony_common_interface<T>` (was `has_gemfony_common_interface<T>::value`).
- **common: the standard tests use an object after each copy.** A type with a public member
  `useForTest()` (its result compared with `==`, concept `UsableForTest`) is used by
  `checkRoundTrips()` before and after each copy, clone, load, assignment and round trip; copy and
  original must give the same result. It finds a member kept outside the compared ones (a cache)
  that a load forgot to renew, as `GFactoryT::load()` once kept its parse cache.

## 8. Checkpointing

- **The checkpoint directory is created lazily**, at the first actual checkpoint write;
  merely configuring an algorithm (`setCheckpointBaseName`) no longer touches the
  filesystem.
- **A checkpoint names its algorithm inside the file**: the algorithm's mnemonic is its first
  field, and `loadCheckpoint()` / `cp_mnemonic_fits()` read it from there. The file name
  stays a convention, so a renamed checkpoint loads; `saveCheckpoint()` is public, and a
  checkpoint is no longer the plain `toFile()` archive of the algorithm.
- **A final checkpoint is written when a run halts** (file name tagged `final`) whenever
  checkpointing is enabled (`cp_interval != 0`); with checkpointing disabled a run
  performs no checkpoint I/O at all.
- **SIGHUP and SIGUSR1 end a run cleanly** (Slurm: `--signal=USR1@<s>`): after the current
  iteration, with its final checkpoint; SIGTERM still ends it at once. The launcher installs the
  handlers in every process (`common/GStopRequest.hpp`); a program no longer registers one itself.
- **A malformed JSON checkpoint or archive throws `Gem::Weft::weft_exception`** naming the path
  of the value in the document and the reason (text that is no JSON, a value of the wrong type, a
  missing key, an array of the wrong length, a number out of range, a malformed floating-point or
  hex string), as a malformed binary archive does; no exception of Boost.JSON leaves the codec.
  A JSON archive may nest up to 1024 levels; the parser's default of 32 refused the checkpoint of
  a swarm, which nests about 43.
- **The personality-traits archive layout changed within the 1.99 development series**:
  the position-only traits of CGD / Nelder-Mead / parameter scan / GSA / ACO / PSO2011 and
  the parent-child traits now serialize their population position through the shared
  `GPositionPersonalityTraits` base (one added nesting level; the GSA/ACO
  `population_position_` and PSO `particle_` tags became `pop_pos_`). Checkpoints written
  by earlier 1.99 development builds do not load; 1.11 checkpoints never loaded in 2.0
  anyway (see the top of this document).
- **A resumed run goes on with the run state of its algorithm**: Standard PSO 2011 (velocities,
  personal and global bests), the generalized simulated annealing (per-chain cooling clocks, best
  energies and stall counters), the ant colony optimization (its solution archive) and the EA's
  global step-size controllers ONE_FIFTH / CSA (global sigma, CSA evolution path) hold it in their
  member lists, so the checkpoint carries it, and only a new run builds it afresh; a state that
  does not fit the population (another swarm size, number of chains or archive size) is refused. Checkpoints of these algorithms written by earlier 1.99
  builds do not load. Nelder-Mead keeps the simplices of its checkpoint, the conjugate gradient
  descent its difference-quotient children and the parameter scan the count of its random scans.
  The first iteration of a resumed run is not the first of the run (`inFirstIteration()`,
  `afterFirstIteration()` answer for the run as a whole): the EA does not evaluate its parents
  again, the swarm moves on from its bests, and the gradient descent, Nelder-Mead and the
  generalized simulated annealing take the step that was due when the checkpoint was written.
- **The run state of a new run is built in one hook, `initRunState_()`**, a protected virtual
  function of `GOptimizationAlgorithmBase` (empty by default). The setup calls it after
  `adjustPopulation_()` when a run starts fresh, never when it was resumed from a checkpoint;
  `init()` does what every start needs. No algorithm asks whether its run was resumed, so an
  algorithm a module adds keeps its state across a resume by the same rule: what carries a run
  is in the member list, what a new run starts with is built in the hook.

## 9. Recording: monitors write streams

- **A recording framework in common** (`common/record/`, namespace `Gem::Common::Record`). The
  process-wide `recorder()` hands out streams -- named tables with fixed, typed columns
  (integers, floating point, bool, text, time stamps; optionally an automatic `time` column of
  the wall or the steady clock) -- to which any thread appends rows:
  `auto s = recorder().stream<std::uint64_t, double>("progress", {"iteration", "best_fitness"});
  s.append(i, f);`. Columns known only at run time are declared as a list of `GColumn`. The rows
  go to sinks: `GCsvSink` (the default) and `GJsonLinesSink` in common, `GPlotSink` in dietrich
  (the stream drawn as a `GDataLog` plot at its end, ROOT by default). `recorder().addSink(name,
  sink)` adds a sink to a stream before another part of the program -- or another library --
  declares it, e.g. a plot of courtier's end-of-run report.
- **One output rule for every file.** A stream writes `<directory>/<stream name>.<extension>`;
  the directory is `./records`, created when needed, set with `--recordDir` (courtier's launcher)
  or `recorder().setDirectory()`. Two open streams of one name are refused, naming both
  declarers -- two monitors of one class in one process need different stream names. An existing
  file is backed up at the first write; a run resumed from a checkpoint appends to the files of
  its monitors when their columns are the same, after cutting them back to the checkpoint: the
  rows the earlier run recorded after it -- after a crash, those since the last checkpoint, and a
  row cut in the middle -- are dropped (`GStreamOptions::resumeColumn`, `resumeAfter`); the plot
  of a resumed stream reads the earlier rows from the stream's CSV file and draws the whole run.
  Each row is flushed to its file. Courtier's launcher closes all streams when `run()` ends -- also after
  an exception or a stop request; a program without the launcher calls `recorder().closeAll()`
  itself.
- **The shipped monitors record streams instead of writing files of their own.** Each declares
  its stream at the first INFOINIT and appends at INFOPROCESSING; every row begins with
  `algorithm`, `iteration` (the algorithm's own) and `chain_iteration` (`GMonitorStream`). In a
  GOptimizer chain one stream (and one plot) holds every algorithm of the chain; the plots run over
  `chain_iteration`, which counts on through the chain, so they do not jump back when the next
  algorithm starts at 0. GOptimizer tells every algorithm how many iterations of the chain ran before
  it (`setChainOffset()`); a checkpoint carries that number, so a run resumed from a checkpoint
  of a later algorithm goes on counting where it stood. A copy of a monitor has no stream of its own. The default stream
  names: `GFitnessMonitor` "best_fitness" (it now writes its data), `GProgressPlotterT`
  "progress", `GAllSolutionFileLogger` "all_solutions", `GNAdpationsLogger` "n_adaptions",
  `GAdaptorPropertyLoggerT` "adaptor_properties", `GProcessingTimesLogger` "processing_times",
  `GOptOptMonitorT` "meta_optimization"; `setStreamName()` changes it. `GStandardMonitor` writes
  its summary through the logger, as before, and is registered only when a program registers it.
- **`GIterationResultsFileLogger` is gone**; it recorded a part of what `GAllSolutionFileLogger`
  records. `GAllSolutionFileLogger::setFitnessOnly()` records what it did: the individual and its
  fitness values (raw or transformed, `setUseTrueFitness()`), without parameters and validity.
- **What leaves the monitors.** The file names, their setters and the constructor arguments that
  took them (`setFileName()`, `GFitnessMonitor::setResultFileName()`, `setFileName_pth()`,
  `setFileName_pth2()`, `setFileName_txt()`, the file name of `GOptOptMonitorT`'s constructor);
  the canvas of `GFitnessMonitor`, which drew nothing (`setDims()`, `getDims()`, `getXDim()`,
  `getYDim()`); the text layout of `GAllSolutionFileLogger` (`setPrintWithNameAndType()`,
  `setPrintWithCommas()`, `setShowIterationBoundaries()` -- a CSV file has a header line and
  commas); `GProcessingTimesLogger`'s two canvases (one plot now:
  `setCanvasDimensions()`). Constructors: `GAllSolutionFileLogger(boundaries)`,
  `GAdaptorPropertyLoggerT(adaptor_name, property)`, `GProcessingTimesLogger(n_bins_x, n_bins_y)`,
  `GOptOptMonitorT()`. The constants `DEFAULTROOTRESULTFILEOM`, `DEFAULTXDIMOM` and
  `DEFAULTYDIMOM` are gone. 1.99 keeps no archive compatibility: a checkpoint written before
  this change that holds such a monitor is not meant to load.
- **Corrected on the way.** The progress plotter of a GOptimizer chain kept only the last algorithm's
  data; `GNAdpationsLogger` and `GAdaptorPropertyLoggerT` shared the default file `NAdaptions.C`.
- **The log file is a stream.** `GRecordLogTarget` (columns time, severity, source, text; plain
  text with `GTextSink`) replaces `GFileLogger`; a target may override `log(message, logType)`.
- **courtier's end-of-run report.** When the launcher releases a consumer that counts something,
  the consumer appends one row of its counters to the stream `courtier`
  (`GBaseConsumerT::recordEndOfRun()`, `endOfRunFigures()`): a networked consumer its connected
  clients, if it can tell, and its requeued items, dropped, foreign and dropped late returns and
  empty checkouts. A local consumer counts nothing and writes no report.

---

*For the day-to-day authoring model with worked examples, see
`docs/writing-optimization-problems.md`, `libs/geneva/quickstart/` (minimal) and
`libs/geneva/examples/03_GParameterObjectUsagePatterns/`.*
