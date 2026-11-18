# Writing an optimization problem in Geneva (flat genome + OA-owned adaption config)

Geneva separates **what** is optimized (the problem's genome — a flat list of parameters with a
fixed structure) from **how** it is mutated (the adaptors — owned by the optimization algorithm, not
the genome). This guide covers the current API after the "config-strip": the genome builder is
**structure-only**, and adaptors live on an **OA-owned `GAdaptionConfig`**.

Canonical examples: `quickstart/` (minimal), `examples/03_GParameterObjectUsagePatterns`
(parameter patterns), `examples/09_GNeuralNetwork` (an architecture-decoded flat genome).

## 1. Subclass `GGenomeT` and override `evaluate()`

```cpp
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"

namespace gen = Gem::Geneva::Genome;

class MyProblem : public gen::GGenomeT<MyProblem> {       // CRTP: pass yourself
public:
    MyProblem() { buildGenome(); }                                 // build the STRUCTURE in the ctor
    MyProblem(const MyProblem &) = default;

protected:
    std::vector<double> evaluate() override {
        std::vector<double> x;
        this->streamline<double>(x);                               // read the (range-folded) values
        double sum = 0.;
        for(double v : x) { sum += v * v; }
        return {sum};                                              // one entry per criterion; minimized by default
    }

private:
    void buildGenome() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(5, -10., 10.).init(1.0);                  // STRUCTURE only -- no adaptor here
        this->setGenome(b.build());
    }
};
```

`GGenomeT<derived_type>` supplies the clone/load/compare/serialize machinery. Your genome is a
flat set of value channels (double / float / int32 / bool); read it with `streamline<T>()`,
`streamlineFP()`, or — for a structured genome — a `GGenomeArchitecture` decoder (see ex09).

## 2. Build the genome STRUCTURE with `GGenomeBuilder`

The builder defines parameters and their **structure only** (count, init value, bounds/range,
optional label). It does **not** attach adaptors.

```cpp
gen::GGenomeBuilder b;
b.addDouble(0., -1., 1.);              // one constrained double in [-1, 1)
b.addDoubleGroup(4, -2., 2.);          // ONE group of 4 constrained doubles (shared adaptor later)
b.addDoubleArray(3, -3., 3.);          // 3 independent groups of 1
b.addDoublePlainGroup(8, -1., 1.);     // UNBOUNDED doubles; the range is just the init perimeter
b.addInt32Group(3, 0, 9);              // constrained int32s
b.addBoolArray(2);                     // booleans
b.addDoubleGroup(2, -1., 1.).label("position"); // a label groups params for config-by-label
this->setGenome(b.build());
```

`ParamHandle` modifiers (chainable): `.init(v)`, `.perimeter(lo, hi)`, `.label(name)`,
`.adaptionMode(mode)`. A handle from an `add*Array`/`add*Group` call applies to **every** group it
created.

### Bounded vs unbounded, and the normalized coordinate model

Geneva stores a floating-point parameter in a **normalized internal coordinate** (confined to the
centered unit interval `[-0.5, 0.5)` for a bounded parameter) and presents the user-visible **external**
value — the one your `evaluate()` sees via `streamline<T>()` — as its affine image. You never
deal with the internal coordinate directly; the model only changes how you think about two things:

- **Bounded vs unbounded is a single choice at build time.** A bounded parameter (`addDouble(init, lo,
  hi)`, `addDoubleGroup`, …) has a hard `[lo, hi)` wall: a mutation that overshoots is folded back into
  range, and assigning an out-of-range external value (including exactly `upper`) **throws**. An
  unbounded parameter (`addDoublePlainGroup`, `addDouble(init)`, …) has no wall — it may roam ℝ; its
  `[min, max]` is only the init perimeter / mutation scale (e.g. a neural-net weight seeded in `[-10,10]`
  may reach 200). `.perimeter(lo, hi)` narrows the random-init region (it may be tighter than a bounded
  parameter's wall).
- **Mutation magnitudes are fractions of the parameter's range.** The Gauss σ (and σ-bounds), the
  GD/CGD finite step and the PSO velocity fraction are dimensionless fractions of the range: `σ = 0.1`
  means "10% of this parameter's range" for *every* parameter and *every* problem, so re-ranging a
  parameter never requires re-tuning its σ. Rates and probabilities (`sigma_sigma`, `ad_prob`, ACO `xi`)
  are dimensionless and unaffected.

## 3. Author adaptors on the OA-owned `GAdaptionConfig`

Adaptors are configured **per optimization algorithm**, on a `GAdaptionConfig` built from the
genome's structure. Use the fluent API to attach a kernel to a group (by index or label):

```cpp
namespace oa = Gem::Geneva::OptimizationAlgorithms;

MyProblem proto;                                                   // a prototype to read the structure
auto cfg = oa::makeAdaptionConfig<oa::GEAAdaptionConfig>(proto);   // GEA / GSA / base variant
cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);                 // sigma, sigma_sigma, min, max, ad_prob
cfg->groupInt32(0).intGauss(0.5, 0.8, 1e-3, 2., 1.);              // integer Gaussian
cfg->groupBool(0).flip(1.);                                        // bit-flip
cfg->forLabel("position").gauss(0.3, 0.8, 1e-3, 1., 1.);          // configure by structural label
```

`makeAdaptionConfig<config_type>(genome)` returns a `shared_ptr<config_type>` sized to the genome's groups.
The config is validated against the genome (`checkConsistency`) when it is adopted.

## 4. Distribute the config to the optimization algorithm

There are three ways to give an algorithm its adaption config:

- **Directly on the algorithm** — `oa_ptr->setAdaptionConfig(cfg);`
- **Through Go2 (by OA personality type)** — when you let Go2 build the algorithm chain:
  ```cpp
  go.registerAdaptionConfig("PERSONALITY_EA", cfg);   // every EA in the chain adopts this cfg
  ```
- **Self-driven adaption (no OA)** — to adapt a genome outside an algorithm:
  ```cpp
  oa::StandaloneAdapter adapter(genome, cfg);
  adapter.adapt(genome);                              // mutates in place
  ```

An error the program does not handle -- a configuration file that cannot be applied, say -- ends
the program with its message and the exit code 1, also under MPI, where the launcher then ends the
whole job: Go2's constructor installs `Gem::Common::installFatalReport()`
(`common/GFatalReport.hpp`) as the terminate handler. A program without Go2 calls it in the first
line of `main()`.

## 5. Adapting without a config is a hard error

An algorithm that needs to adapt but has **no** adaption config is a **fatal error** at setup — there
is no silent auto-derivation of adaptors from the genome. Always provide a config via one of the
mechanisms above (or, for a quick start, build a default from the genome's structure with
`makeAdaptionConfig` + a Gaussian on every FP group, as the generic post-optimizer does).

## Why this split?

The genome is pure data (structure + values + fitness); the same problem can be optimized by EA, SA,
swarm, gradient descent or a parameter scan without changing the individual, because the mutation
strategy is the algorithm's concern, authored on its own config. This is what makes a Geneva
individual transport-cheap (a flat genome with a shared, immutable layout) and lets one problem
definition run serially, multi-threaded, over MPI or via websockets unchanged.

## 6. Packaging a problem: compiled in, or loaded at runtime

A problem can reach an optimizer two ways, and one individual can be packaged **both** ways at once
from a single source:

- **Compiled in** — your program `#include`s the individual and constructs it (`new MyProblem(...)` /
  `Go2::registerContentCreator`). This is the classic path and always available.
- **Loaded at runtime** — the individual is a shared object (`libMyProblem.so`) that the generic
  optimizer `dlopen`s on request (`--individual path/to/libMyProblem.so`), reads its config, and runs.
  This is the model Geneva ships and defaults to; the shipped library itself contains **no** concrete
  problem individuals.

### What a loadable individual needs

To be built by the generic factory (and thus loadable), the individual is a **Tier-2, config-driven**
flat individual — it supplies the static hooks `GIndividualFactory<derived_type>` calls (see section 3
of `GIndividualFactory.hpp` and the `GFunctionIndividual` / `GLineFitIndividual` examples):

- a public default constructor,
- a nested `struct Config` holding the configurable values,
- `static void describeConfig(GParserBuilder&, Config&)` — registers the config-file options,
- `static GenomeData buildGenome(const Config&)` — builds the structure-only genome,
- optionally `static void applyConfig(derived_type&, const Config&)` and
  `static std::shared_ptr<...GAdaptionConfigBase> buildAdaptionConfig(const GGenome&, const Config&)`.

**External data is no obstacle.** A loaded `.so` runs in the host process with full, unsandboxed
runtime access: name the resource (data file, URL, database, helper program) in `Config`, and open it
in `buildGenome`/`applyConfig`. Data need not be resident — a large-data individual keeps a file
handle or memory-map as a member and streams in `evaluate()`; `GLineFitIndividual` reads its
`(x,y)` points from a config-named file, `GExternalEvaluatorIndividual` launches an external evaluator.

### The module "glue" translation unit

Serialization registration stays **your** job and lives once with the individual (the same registration a
compiled-in individual needs): `GEM_REGISTER_ARCHIVABLE(MyProblem)` (`weft/GArchivePolymorphic.hpp`), one
per serialized type.

The module adds one small **glue** TU carrying the fixed entry point the loader resolves via `dlsym`.
It does *not* repeat the export:

```cpp
#include <boost/config.hpp>                     // BOOST_SYMBOL_EXPORT
#include "common/GModuleManifest.hpp"           // GenevaModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp"  // Gem::Geneva::individualManifest<>
#include "MyProblem.hpp"

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::individualManifest<
        Gem::Geneva::Genome::GIndividualFactory<MyProblem>,
        "./config/MyProblem.json", "MyProblem">();
}
```

The `extern "C"` wrapper is the only irreducible boilerplate — its symbol name is fixed for the loader,
so no template can synthesize it. Everything else is the typed `individualManifest<>` helper, which
stamps the module's toolchain-compatibility fingerprint and its module-ABI version (both validated before
any C++ contribution runs) and one INDIVIDUAL contribution. The config path is auto-created with the
individual's defaults if absent.

### Declaring the package in CMake

`GENEVA_DECLARE_INDIVIDUAL` packages the individual — it compiles the sources you list into the right
target types and **never modifies or generates any C++**:

```cmake
GENEVA_DECLARE_INDIVIDUAL(MyProblem
    MODE    both                       # load | compile | both
    SOURCES MyProblem.cpp              # the individual (class + its archive tag); into both targets
    PLUGIN  MyProblemPlugin.cpp        # the glue TU above; into the module .so ONLY
    CONFIG  ./config/MyProblem.json)   # recorded for config materialization
```

- `MODE compile` / `both` builds an **object library** `MyProblem-obj` that a compile-in consumer
  links (so the `GEM_REGISTER_ARCHIVABLE` initializers are never stripped).
- `MODE load` / `both` builds the module `libMyProblem.so` (object files + the `PLUGIN` glue), linking
  **no** Geneva libraries — their symbols resolve from the host at load time.
- `PLUGIN` goes into the module only: the manifest symbol name is fixed, so a compile-in binary that
  links two individuals must not contain two copies of it.

**The one hard rule:** a single *process* must never both compile-in and load the same individual — Geneva
allows exactly one optimization problem per process and refuses the second claim, naming both sources. (The
same `.cpp` compiled into several *separate* binaries is fine; each is its own process.)

See `examples/18_GLoadableIndividual/` for the end-to-end reference: a loadable problem `.so`, a
generic optimizer that loads it, and the CTest that doubles as the single-process-singleton proof. To ship
an *optimization algorithm* rather than a problem, `examples/20_GLoadableOA/README.md` is the same story one
layer up.

### Stamping a program with its build

A server of the networked consumers greets each client with the commit of Geneva and the identity
of the user's program; a client whose program has another identity ends instead of computing with
other code. A program gets its identity from one line of CMake per executable:

```cmake
GENEVA_STAMP_PROGRAM(MyOptimizer)                        # the commit of this project's sources
GENEVA_STAMP_PROGRAM(MyOptimizer IDENTITY "release-7")   # or a text of one's own
```

Without `IDENTITY` the identity is the commit of the project's sources, taken from git at every build
when the top of the project is the top of a git work tree (`-dirty` appended when a tracked file has
changed); without git the program is not stamped, CMake says so, and a client logs once that its
program is not checked. A program may also call `Gem::Common::setProgramIdentity()` itself, before it
makes its consumer. Only executables are stamped; a loadable module is checked by the module loader.
The quick start's template stamps its two programs.
