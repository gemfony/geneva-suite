# Development Invariants

Mandatory rules for **all** work in this repository — human or AI. They **override** convenience and
default behaviour. If a change would violate one, stop and find another approach. An invariant is changed
only by an explicit maintainer decision, recorded here.

Every rule below is, at bottom, an instance of **Invariant 1**.

---

## 1. Prefer existing Geneva capabilities over re-inventing them

Before writing a new utility, container, algorithm scaffold, parser, or helper, look for an existing one in
`common/`, `hap/`, `courtier/`, or `geneva/` and **use it** — improving or extending it if it falls short.
Reinvention fragments the codebase, duplicates bugs, and bypasses hardening and tests that already exist.

Creating a **new** general component (rather than extending an existing one) is allowed **only after
consultation with the maintainer, and only when extending an existing facility genuinely does not fit**. When
you do create one, it **must** be added to a library as a reusable building block, **never dropped at the
call site out of convenience**. Placement follows generality: **if the component is applicable to more than
one library, it goes to `common/`** (or at least to the more general of the libraries that may use it); only a
component specific to a single library belongs in that library. Utilities, containers, and concurrency
primitives are general → `common/`. Queues and the other concurrency primitives (Invariant 2) are one concrete
example of this rule; apply the same reflex everywhere.

## 2. Concurrency primitives come only from the shared concurrency facilities

All concurrency building blocks — **thread pools, thread groups, thread-safe queues, thread-safe keyed
stores, completion latches, content-addressed/aging stores, and any lock-free structures** — come from
Geneva's shared concurrency facilities, consolidated in the distinct sub-module
`projects/common/include/common/concurrency/` (namespace `Gem::Common::Concurrency`) — mirroring geneva's
`par`/`ind`/`oa`, so they stay able to use `common`'s facilities yet are clearly separated and impossible to
overlook. These include `GBlockingMPMCQueueT`, `GPreallocatedMPMCQueueT`, the `GMPMCQueueT` facade + the
`MPMCQueue` concept, the thread pool/group, `GContentAddressedStoreT`, `GThreadSafeKeyedStoreT`,
`GThreadSafeSetT`, `GCompletionLatchT`, `GSPSCStagingRingT` and `GAgingStoreT`. No part of Geneva rolls its
own. If a primitive does not meet a requirement, **improve it or add a variant there** — never a local
`deque`+`mutex`, bespoke ring, hand-rolled thread pool, or ad-hoc thread-safe map at the use site.

*Why:* one place to get memory ordering / ThreadSanitizer correctness right, one set of semantics, isolated
tests — and a distinct, named dependency makes Invariant 1 unforgettable. The original cleanups owed to this
rule are discharged (the consumer's `late_returns_` buffer + retained-original map → `GAgingStoreT`, the
in-flight borrow set → `GThreadSafeSetT`, Hap's `GRotatingPool` → `GSPSCStagingRingT`, the global option
stores → `GThreadSafeKeyedStoreT`). *Remaining (deferred, MPI-transport-local):* the MPI transport's raw
`receiverThread_`/`cleanUpThread_` → `GThreadGroup` and its `openSessions_` vector reaping → a library store.

## 3. Build out-of-source only

Never configure or build inside the source tree. Use an external build directory (e.g. `~/build`) via
`scripts/prepareBuild.sh`. In-source builds pollute the repository and are rejected by `prepareBuild.sh`.

## 4. Serialization is complete and single-sourced

Every class that adds data members implements `serialize()` and `load_()` / `save_()` (required for
checkpointing and network transport). Keep the compared/serialized member list **single-sourced** (the
`localMembers_` pattern) so that `serialize()`, `load_()`, and `compare_()` can never drift apart — a member
forgotten in one list is silent data loss. Serialization runs entirely through the in-house **GArchive**
(`Gem::Weft`) codecs — the flat binary codec (`GEM_BINARY`, the networked-wire default) and the
self-describing JSON codec (`GEM_JSON`, the checkpoint default). A `serialize()` body is written once,
codec-agnostic, against `Gem::Common::archive_named` / `archive_named_base` (never `boost::serialization`);
a polymorphic wire/checkpoint type is registered with `GEM_REGISTER_ARCHIVABLE`. Boost.Serialization has
been removed — do not reintroduce `boost::serialization`, `boost::archive`, or `BOOST_CLASS_EXPORT`.

## 5. One consumer per process

A process uses exactly one consumer, held in `GConsumerRegistry`. Optimization algorithms are
transport-agnostic: they submit to that single process consumer and **never** build, select, or inject
one (the former per-algorithm broker / executor injection is gone). The transport is chosen externally
(via `Go2` or the command line).

## 6. The genome is pure data; mutation strategy lives on the optimization algorithm

The genome (`GGenome`) carries the problem's parameter values and structure — **not** adaptors or
optimization-algorithm scratch. Adaption is configured on the OA-owned `GAdaptionConfig`; per-individual OA
scratch (e.g. self-adaptive σ) lives on OA-owned storage, never on the genome (the "config-strip" model).

## 7. When a test fails during a refactor or major change, triage it before touching code

A failing test during a refactoring or large-scale change is **not** automatically a signal to change the
production code. First decide which kind of failure it is:

- **Genuine failure** — the test encodes behaviour that must still hold, and the change broke it. **Fix the
  code.**
- **Expected consequence** — the change *legitimately and intentionally* altered the behaviour the test pinned
  (a new, valid contract). **Amend the test** to reflect the new reality.

Adapt the code to the test **only** for genuine errors. Never silence a genuine failure by loosening a test,
and never bend correct new code back to an obsolete expectation. State which case applies, and why, when you
make the change. Rule of thumb: in a **behaviour-preserving** refactor (e.g. the `common` concurrency
extraction) almost every red is genuine — a characterization test going red means the refactor changed
something it must not have. In a **behaviour-changing** redesign (e.g. the genome SoC work) a red may simply be
the test catching up to a deliberate new contract, and amending it is correct.

## 8. Full builds go through `prepareBuild.sh` + a `genevaConfig.gcfg`

For a full (clean) build of Geneva, use the project's build driver rather than a hand-rolled `cmake`
invocation — e.g.

```bash
cd $HOME/build && $HOME/ClionProjects/geneva/scripts/prepareBuild.sh --clean -y --build genevaConfig.gcfg
```

(use `$HOME`, never a hard-coded home path). The `genevaConfig.gcfg` in the build directory centralises the
build settings — compiler choice (clang vs g++), CUDA on/off, build type, MPI/GPU consumers, Boost location,
and more — so that one file, not scattered command-line flags, determines how Geneva is built. This keeps full
builds reproducible and consistent with Invariant 3 (out-of-source). Fast iterative rebuilds of a single target
may still use `cmake --build <dir> --target <t>`, but a full verifying build uses `prepareBuild.sh`.

## 9. A failing test is fixed before moving on — even if it looks unrelated

If any test errors during development, **fixing it has the highest priority**, even when the failure appears
unrelated to the change in hand. A red test is never stepped over, deferred, or written off as "pre-existing"
or "environmental noise" so that other work can proceed: it is diagnosed and made green first. (Invariant 7
still governs *how* — fix the code for a genuine failure, amend the test for a deliberate contract change — but
the failure is always addressed, never left red.) A green test suite is the precondition for continuing, and
for every commit.

The same discipline denies **silent disabling** in every form: commenting a test out, gating it behind a flag
that is never true, marking it skipped without a machine-detectable reason, or leaving it registered but
unrun "for now". A disabled test is a red that has been hidden instead of diagnosed. A test that cannot stay
green has exactly two honest exits: it is **fixed** (Invariant 7 decides whether the code or the test moves),
or — when it genuinely no longer makes sense — it is **removed outright**, as a visible, reviewable deletion
whose commit says why. Removal is a legitimate outcome; hiding never is. Conditional registration is
acceptable only for real, configure-time-detectable external dependencies (no MPI launcher, no GPU device on
the host), never as a place to park failures.

## 10. Newly discovered compilation warnings must be investigated

A warning that appears during a build is a signal, not noise: **investigate every newly surfaced compiler
warning** rather than letting it scroll past. Determine its cause, and either fix it at the root or, if it is
genuinely benign and unavoidable, understand and record why. The same discipline as Invariant 9 applies to
warnings as to test failures — they are not stepped over because they look unrelated to the change in hand.

## 11. Usage and impact searches must cover the WHOLE tree

When determining whether something is used, or assessing the blast radius of a change, **search across all of
Geneva — every library, both headers (`.hpp`/`.h`/`.cuh`) and sources (`.cpp`/`.cu`), and including examples,
tests, benchmarks, and demos.** Do not scope the search to a single directory, to `src/` only, or to
`.cpp` only: a symbol declared and consumed entirely in a header (e.g. an inline OA helper) is invisible to a
`src/*.cpp` grep, and a "no callers" conclusion drawn from a partial search is worse than none — it licenses a
wrong deletion or migration. Confirm "unused" only after a tree-wide search; when in doubt, widen the net.

## 12. Substantial changes keep the Doxygen documentation in sync

When making a substantial change — altered signatures, moved or renamed members, new/removed classes, changed
ownership or lifecycle, a different invariant — **cross-check the Doxygen documentation (class/file/function
comments, `@param`/`@return`, `@par` notes, cross-references) and bring it up to date in the same change.**
Stale API documentation silently misleads; the docs are part of the contract, not an afterthought, and drift is
fixed where it is introduced rather than left for a later sweep.

## 13. Tests and demos adapt to a worthwhile change — they never veto it

A worthwhile architecture or code change is **not** abandoned because it would make existing tests, demos, or
examples fail, nor because the **only** remaining consumer of a feature or data member is a test, demo, or
example. In that case it is the test/demo/example that is updated (or the feature retired), **not** the change
that is dropped. A sole test/demo consumer does **not** make a feature "used" for the purpose of justifying its
retention. This composes with two neighbours: Invariant 11 (search the whole tree — *including* tests and demos —
to find a feature's real consumers) and Invariant 7 (how to triage the resulting red test: amend it when the
change is a deliberate new contract, fix the code only for a genuine regression). Tests guard behaviour that must
hold; they are never an excuse to forgo an improvement to that behaviour.

## 14. Generalize a generalizable solution — no special-purpose code where a general one fits

When a problem can be solved at a general level, solve it there — do **not** write a special-purpose,
one-off, or per-case variant of something that admits a single general formulation. If the same logic would
otherwise be duplicated, hard-coded per algorithm/type/transport, or branched by a fixed enumeration of
cases, hoist it into one general mechanism (a shared function, a template/policy, a virtual hook with a
sensible default, a data-driven parameter) and let the specific sites specialize only where they genuinely
differ. A special-purpose solution is justified **only** when a general one is genuinely infeasible or would
be materially more complex, error-prone, or slower — and then the reason is recorded at the site. This is the
positive form of Invariant 1 (prefer existing capabilities) and composes with Invariant 13 (a general
improvement is not blocked by a special-purpose consumer): prefer the general shape, generalize the
special-purpose code you find, and only descend to the specific when the general level cannot express it.

## 15. A discovered defect earns a regression test

When a **genuine defect surfaces during development** — a compilation error, a crash, an assertion, or a
logical/numeric failure — fixing it is **not enough**: add a test that pins the problem down so it cannot
silently return. The test must **fail on the unfixed code and pass once the fix is in** (verify both when
practical), and it lives with the code it guards (a unit test in the owning library's suite; the existing
per-class `specificTests*_GUnitTests_` hooks — the opt-in `Gem::Common::GSelfTestable` facet — and the
`[net]`/serialization round-trip suites are the natural homes). This applies to problems *found while working*, not only to tickets — the moment you understand why
something broke, encode that understanding as a test. A defect you cannot yet reproduce is triaged first
(Invariant 7); once reproduced, the reproduction becomes the regression test.

*Why:* a fix without a test documents nothing and decays — the same mistake reappears under a refactor, a
compiler change, or a merge, and Invariant 9 (a failing test is fixed before moving on) has nothing to fire
on. A regression test converts a one-time debugging effort into a permanent guarantee, and turns "I fixed it"
into "it is proven fixed and will stay fixed."

*How to apply:* reproduce → add the failing test → fix → confirm the test now passes. Prefer the narrowest
test that still captures the root cause (a value that round-trips, an overload that resolves, a boundary that
holds) over re-running a whole scenario. If a genuine defect is genuinely untestable in the current harness,
record why at the fix site (cf. Invariant 14's recorded-exception discipline).

## 16. Write to the highest C++ standard the build is configured for

New and modified code uses the language and library features of the **highest C++ standard the Geneva build
is currently configured to use** — do not hand-limit yourself to an older dialect out of habit or muscle
memory. This rule deliberately **does not name a version**: the active standard is a build-system setting
(today C++23), chosen in **one central place** — `CMAKE_CXX_STANDARD` in
`CMakeModules/CommonGenevaBuild.cmake`, driven by the `genevaConfig.gcfg` — and it will rise over time. So
**deduce the current standard from the build system, not from this document**: read the central
`CMAKE_CXX_STANDARD` (or the configure banner it prints, "Using C++ standard NN"), and write to that. When a
newer construct genuinely reads better or removes hand-rolled machinery, prefer it over the pre-standard
idiom it replaces.

*Why:* the whole tree is compiled at one standard; code that silently targets an older dialect is
inconsistent, re-implements what the language now gives for free, and quietly blocks the standard from being
raised. Pinning the rule to a concrete version would rot the moment the build moves on — deducing it from the
build system keeps the instruction correct across every future bump.

*How to apply / caveats:* (a) the rule governs **host** code; **device (CUDA `.cu`/`.cuh`) code trails the
host standard** (nvcc has no matching device dialect — a separate central `CMAKE_CUDA_STANDARD`), so target
that lower standard there. (b) The configured `-std` is necessary but not sufficient for a given *library*
feature: a header may be missing on a particular toolchain (e.g. `<mdspan>` / `std::start_lifetime_as` absent
on some libstdc++ versions). Confirm the feature actually compiles on the supported compilers (Invariant 8's
gcc **and** clang builds) before relying on it, and fall back only with a recorded reason. (c) This composes
with Invariant 1 — adopt the newer standard where it *replaces* hand-rolled machinery or closes a bug, not as
churn for its own sake.

## 17. The source tree stays pristine — no in-source builds, no stray artifacts

The Geneva source tree is never a workspace. This extends Invariant 3 (build out-of-source only) from
*building* to *every* activity that could leave a footprint in the tree:

- **Never configure or build in-source.** All configuration and compilation happens in an external build
  directory (e.g. `$HOME/build`); `prepareBuild.sh` rejects an in-source build (cf. Invariant 3).
- **Never run tools or binaries from the repository root** (or any source subdirectory) when they write
  output. A Geneva example or client writes its files relative to its working directory — run it from an
  external scratch/working directory, never from inside the checkout, so it cannot deposit `config/*.json`,
  logs, checkpoints, result files, or plots into the tree.
- **After every commit the working tree is pristine:** `git status` shows *nothing* — no untracked stray
  files, no generated artifacts, no editor scratch, no leftover output. The only changes a commit contains
  are the intended edits. If some step generated a file in-tree, remove it (or relocate the activity to an
  external directory and regenerate) **before** committing; a commit is not complete while `git status` is
  dirty with anything unintended.
- **New intended files are tracked, not left untracked.** A source/header/CMake/config file created to stay
  in Geneva is `git add`-ed as part of the change that introduces it, so it is present in the commit.
  "Pristine" means `git status` shows nothing *because* stray files were removed **and** intended new files
  were staged — never because an intended file was silently dropped. Stage new build-relevant files the
  moment they are created, so the build system and every reviewer see them.

*Why:* stray files silently become part of the repository, mask real changes in `git status`, get
accidentally committed, and make "is the tree clean?" — the precondition for a trustworthy diff, build, and
commit — unanswerable. A pristine tree keeps every diff meaningful and every build reproducible.

## 18. Random numbers are never deterministic — never rely on reproducing them

Geneva's random numbers come from Hap (`GRandomFactory` / `GRandomT`), which produces them from **concurrent
producer threads** feeding a shared queue, seeded from entropy. The order in which numbers are consumed across
threads is therefore **not reproducible**, and there is no supported "fixed seed → identical sequence" mode.
Consequently:

- **Never write code or a test that depends on a specific random sequence, a specific seed, or a specific
  draw order.** No "golden" RNG trace, no byte-for-byte reproduction of a stochastic result. A test that
  needs to check a stochastic outcome uses a **behavioural / statistical** gate — a tolerance band, or
  best-of-N so a rare unlucky draw cannot flake the suite (cf. Invariant 9: a real failure is fixed, but a
  stochastic bar is expressed as best-of-N, not as a tightened threshold on one run).
- **Never spend effort "preserving RNG determinism"** across a refactor — there is none to preserve. When a
  change moves or re-shapes code that draws random numbers, the correctness question is whether the *logic*
  is faithfully transformed, not whether some sequence is reproduced. In particular, fitness evaluation
  (`evaluate()`) is a deterministic function of the individual's *parameters* and normally draws no RNG at
  all; where a body genuinely is stochastic (e.g. a nested sub-optimization), it has no reproducible value and
  is validated by faithful-transformation + the suite's tolerant checks.

*Why:* treating Geneva's RNG as reproducible is a category error that produces flaky tests and wasted effort
chasing a determinism the engine does not offer. Stating plainly that randomness is non-deterministic keeps
tests honest (statistical, not brittle) and frees refactors from a phantom constraint.

## 19. Every library header is installed — ship the whole public header tree, not a curated subset

A Geneva library's **entire** header tree is part of its installed interface: every `.hpp` under a library's
`include/<namespace>/` is installed, **regardless of whether anything in this repository currently uses it**.

- **Install whole header directories, not hand-maintained file lists.** Header installation uses
  `INSTALL(DIRECTORY … FILES_MATCHING PATTERN "*.hpp")` per library, so a newly added header is shipped
  automatically. Do NOT gate a header's installation on "is it used by an example / another header yet" — an
  explicit per-header install list silently drifts out of sync and omits headers.
- **The installed header set must be self-contained.** Because installed public headers include one another,
  omitting any header breaks out-of-tree compilation of the headers that include it (e.g. `Go2.hpp` /
  `GRandomT.hpp` pulling in a transitively-required header). A wholesale directory install makes the closure
  complete by construction; a curated list does not.
- **"Internal-looking" is not a reason to withhold a header.** If a header genuinely must never be part of the
  public interface, it does not belong in a library's public `include/` tree in the first place — move it into
  the library's `src/` (private) rather than excluding it from the install.

*Why:* an installed Geneva must build for a downstream / out-of-tree consumer. A curated install list is a
standing latent bug: it compiles in-tree (all headers present in the source tree) yet fails after install the
moment a shipped header includes an un-shipped one, and the failure is invisible until someone builds against
the install. Shipping the complete tree removes the drift and keeps every install self-contained.

## 20. Replacing a mechanism replaces its periphery — the old design leaves whole, in the same change

When a mechanism is redesigned or replaced, the change is **not complete** while any part of the old
mechanism's periphery is still standing. The periphery is everything that existed only to serve the old
design: its enums and enum values, constants, typedefs, serialized fields, virtual hooks, delivery/
registration scaffolding, config keys and command-line options, factory entries, build-system options and
install rules, tests and demos that exercised only the old path (Invariant 13), and every piece of
documentation vocabulary that presents the old design as current (Invariant 12).

- **The deleting sweep ships in the same change, or in an immediately-following one.** "Clean it up later"
  is how a replaced design's edges become permanent: later never has a trigger, and the next redesign
  stacks a second abandoned periphery on top of the first.
- **A whole-tree usage search is the gate** (Invariant 11). For every identifier, serialized field name,
  config key, option, and doc term of the replaced mechanism, search every library — headers AND sources,
  examples, tests, benchmarks, docs, scripts, CMake. Each hit is either migrated to the new mechanism or
  deleted with the old one; a change is reviewable as complete when that search comes back empty.
- **Never keep a hand-synced duplicate of the old mechanism "for compatibility" or "just in case".** A
  parallel copy maintained by hand next to the live one is worse than dead weight: the copies WILL drift,
  and the drift is a latent bug that surfaces far from its cause. If genuine compatibility is required,
  it is a designed, tested adapter over the new mechanism — not a retained copy of the old one.
- **State each surviving fact once.** If the replacement leaves the same fact (a member list, a default, a
  contract rule, a label/suffix convention) expressed in two places, fold them in the same change
  (cf. Invariant 4 for serialization). Two statements of one fact are a drift waiting for a trigger.

*Why:* a full-tree review found the same signature across half a dozen redesigns — each completed at its
core and abandoned at its edges — and the second-order cost dominated: hand-synced leftovers had already
drifted into real bugs (a status-machine rule duplicated into two classes where one copy silently lost an
error flag; a copy constructor that drifted from the serialized member list and dropped fields on every
clone; decorator and label conventions stated twice and disagreeing). Deleting the periphery at redesign
time is Invariants 11 + 13 applied when the knowledge is freshest and the diff is smallest; deferring it
converts cheap deletions into an accumulated review-and-repair bill, paid with interest.

## 21. Resources are owned by RAII; no naked owning pointers

Every resource — heap memory, threads, locks, file handles, sockets, device handles — is owned by a value
whose destructor releases it (RAII), never by a hand-managed `new`/`delete` pair or by a raw pointer that
"owns":

- **No owning raw pointers, no naked `new`/`delete`.** Express sole ownership with `std::unique_ptr` (the
  default) and shared ownership with `std::shared_ptr` only where ownership is genuinely shared. A raw pointer
  or reference is a **non-owning** observer, valid only for the duration of the call that receives it.
- **Prefer the rule of zero.** A class that owns nothing beyond its members declares no destructor, copy, or
  move — the members' own RAII composes. Declare special members only when the class manages a resource
  directly, and then declare the whole set the rule of five requires, kept consistent with the serialized
  member list (Invariant 4).
- **Acquire in a handle, release in its destructor.** A resource taken for the span of an operation is held
  by a scoped RAII object that releases it on **every** exit path, exceptions included (a lock guard, a lease
  from `GRandomLeasePool`, a thread joined by `GThreadGroup` / `GThreadPool`). Cleanup is never open-coded on
  the happy path alone.

*Why:* automatic, exception-safe resource management is the property the whole codebase already relies on — the
`shared_ptr`→`unique_ptr` migration, the RNG lease pool, the thread pool and thread group — but stating it as
an invariant stops a naked `new`, an owning raw pointer, or a hand-rolled cleanup path from slipping back in,
where it would leak or double-free on an error path far from its cause. This is the ownership-level complement
of Invariant 2 (shared concurrency machinery comes from the shared facilities) and composes with Invariant 4 (a
resource-owning class states its special members and its serialized members consistently).

## 22. Development runs are short by default; convergence-scale runs are the rare exception

Every optimization run made to exercise or validate a change — in a unit test, a manual test, a demo, a
benchmark used as a check, or an ad-hoc run — uses **few iterations and a small population** (the smallest
that still exercises the code path under test). A change is verified by whether the machinery *runs
correctly* — serializes, compares, clones, adapts, distributes, checkpoints, halts — not by whether it
*converges*, so a handful of iterations over a handful of individuals is sufficient and is the default.

The **only** exception is a change to an optimization algorithm's own search behaviour (a new or altered
adaptor, selection rule, step controller, velocity update, constraint handler, …) whose very purpose is
*convergence quality*. There, and only there, a longer run with a realistic population is warranted — and
the outcome is still gated **behaviourally** (a tolerance band / best-of-N, per Invariant 18), never by a
fixed iteration count reproduced for its own sake.

*Why:* full-scale optimizations dominate the wall-clock of the test suite and of every developer gate, yet a
contract-level change (serialization, comparison, cloning, the consumer transport, the halt logic) is fully
exercised in a few short iterations — the convergence tail adds minutes and verifies nothing the change
touched. Keeping runs short by default makes the green-suite precondition (Invariant 9) cheap enough to
honour on every change; reserving long runs for genuine convergence questions spends that time only where it
actually buys information. In practice this also means gating a contract-level change on the relevant test
subset (e.g. the serialization/comparison contract cases) rather than re-running the convergence suites that
the change cannot affect.

## 23. Geneva is a library — a complete public API is the contract, not "used" code

Geneva is a **toolkit**: it is linked into downstream applications whose needs this repository does not know
and cannot see. A public API therefore exists to serve callers who are **not** in this tree, and its value is
not measured by the presence of a local caller.

- **"No caller inside Geneva" is not a defect, and not grounds for deletion.** A public function, overload,
  accessor, or class that no example/test/benchmark happens to call is still part of the shipped interface and
  may be exactly what an out-of-tree consumer relies on. Do **not** treat a whole-tree "unused" result
  (Invariant 11) as a licence to remove a *public* API — that search proves only that *this repository* has no
  caller, which is the expected condition for a general-purpose library, not evidence the API is dead.
- **Buggy-but-public → fix it, don't delete it.** If a public API is discovered to be broken (a re-init trap,
  a wrong default, a lifecycle hazard), the correct response is to **repair** it (and add the regression test
  Invariant 15 requires), not to excise it because "nothing here calls it." Deleting a broken public entry
  point silently narrows the contract and breaks the very downstream callers who most needed the fix.
- **Completeness and symmetry are themselves API value.** A public setter implies a getter, an `add` implies a
  `remove`, a `reset` completes a factory/singleton accessor — the rounded-out surface is part of what makes
  the toolkit usable, even where the in-tree code exercises only one direction.

This does **not** license dead **internal** machinery: a genuinely private helper, an inner-workings detail
with no public exposure, or the periphery of a *replaced* mechanism (Invariant 20) is still removed once the
whole-tree search comes back empty. The distinction is exposure, not local call count — public interface is
kept and fixed; private orphans and abandoned peripheries are deleted.

*Why:* a library curated down to "only what our own examples call" is a library that fails its actual users —
the ones downstream. Confusing "no local caller" with "obsolete" would strip the interface of exactly the
general, reusable entry points a toolkit exists to provide, and would turn a discovered bug in a public API
into a reason to amputate rather than heal it. This is the public-interface complement of Invariant 19 (ship
the whole public header tree, not a curated subset): ship — and maintain — the whole public API, not a subset
justified by in-tree usage.

## 24. The tree carries only intentional, tracked configuration — never scattered or generated config litter

The source tree is not a scratch pad for configuration any more than for build output (Inv 3, Inv 17). A
configuration or dotfile is allowed in the tree **only** if it is (a) genuinely a repository input, (b)
tracked in git, and (c) still serving a live purpose. Everything else — tool/IDE state, run-generated config,
and configuration for a mechanism that no longer exists — is litter and does not belong.

- **Every in-tree config is tracked and justified.** A committed config is a deliberate repository input (the
  shared `.clang-format` / `.clang-tidy` / `.gitignore` / `CMakePresets.json`, an example's
  `config/config-overrides.json`, the `docs/config-reference/` reference set). If a file is not something a
  fresh clone needs, it does not get committed.
- **Generated and tool-local config never lands in the tree.** IDE project state (`.idea/`, `.vscode/`),
  Python caches (`__pycache__/`), in-source build directories (`cmake-build-*/`, `build/`), run-emitted config
  (`config/*.json` written by a `Go2` program), logs (`GENEVA-EXCEPTION.log`) and coverage artefacts are
  generated *outside* the checkout — the direct consequence of running every build and binary from an external
  working directory (Inv 3, Inv 17). When one appears in-tree, it is removed and the activity relocated, not
  gitignored-and-tolerated in place. (`.gitignore` is a backstop against accidental commits, not a licence to
  let generated files accumulate on disk.)
- **A config is removed when the mechanism it configures is gone.** Replacing or retiring a mechanism deletes
  its configuration in the same change (Inv 20 applied to config): a `.project` for a build system abandoned a
  decade ago, a config key for a removed option, a preset for a deleted target. An orphaned config outlives
  its purpose only to mislead.
- **Centralize; do not scatter.** Configuration that must live in the repository lives at the fewest,
  best-known locations — the tree root for tree-wide tooling, one reference directory for the config schema —
  never the same fact copied into many per-directory files (Inv 20, "state each fact once"). Prefer one
  central source a tool reads over a per-subdirectory sprinkling.

*Why:* scattered and stale configuration is the same disease as a littered build tree — it makes "what does a
clean checkout actually contain?" unanswerable, hides the few configs that matter among generated noise, and
lets a dead mechanism's settings linger long enough to be mistaken for live ones. A tree whose every config
file is tracked, justified, and current is one a newcomer (or a tool) can trust at face value. Note that some
inert dotfiles at the tree root may be **environment/sandbox mounts** (read-only, empty, and un-removable —
`rm` reports "busy"); those are harness infrastructure, not tree content, and are left untouched.

---

## 25. `CHANGES` is an overview — at most 50 lines per version, detail lives in a per-version file

`CHANGES` exists so a reader can see, at a glance, what each release is about. That only works while it
stays short. **A single version's entry may not exceed 50 lines.** When a release genuinely has more to
say, the surplus does not get squeezed in or trimmed to a stub — it moves into its own file.

- **The overflow file is `docs/CHANGES-<version>.md`.** It lives under the top-level `docs/` directory and
  its name carries the version it documents (`docs/CHANGES-2.0.md` for the 2.0 redesign). One file per
  version that needs one; a release that fits in 50 lines needs none.
- **The `CHANGES` entry then points at it.** The entry keeps the rationale, the preconditions and a
  one-line pointer; it never duplicates what the per-version file says (Inv 20, "state each fact once").
- **Count before committing a release note.** 50 lines is the ceiling for the whole entry — heading,
  prose, bullets and blank lines together — not a per-section budget.
- **Historical entries are frozen.** Release notes for versions already shipped are a record of what was
  said at the time; several pre-1.99 entries predate this rule and exceed the limit. They are left as
  they are. The rule binds every entry written or revised from now on.

*Why:* a release-note file that grows without bound stops being read, and the one thing it is for — "what
changed in this version?" — becomes the hardest question to answer from it. A hard line count forces the
choice the writer would otherwise avoid: what is genuinely the headline, and what is migration detail that
a porting reader wants in full but a browsing reader does not. Splitting them serves both, and keeps each
fact in exactly one place.

---

## 26. Every commit compiles cleanly — on every compiler it was exercised with

A commit is only allowed for code that **compiles cleanly on at least one of the supported compilers**
— zero errors, and zero new warnings (Invariant 10) — verified against the *committed* state, not
against a similar-looking earlier state of the tree. If **both** compilers were exercised for the change
(a clean-build gate, a pre-push verification, a vtable/build-system change that mandates it), the commit
must compile cleanly on **both**: a red second compiler is never stepped over by committing what the
first one accepted.

- **"Compiles" means the full affected build**, not a single target: for library/header changes the
  libraries plus everything the change instantiates (a header-only breakage that only surfaces in an
  example or test TU still counts as not compiling — cf. the full-Debug-build discipline).
- **Intermediate states are not committable.** A multi-commit sequence is ordered so that every commit
  builds on its own; if two halves of a change cannot build separately, they are one commit (an
  unbuildable intermediate is never "fixed by the next commit").
- This composes with Invariant 9 (green tests are likewise a per-commit precondition) and the false-green
  discipline: compile evidence comes from real exit codes on a genuinely rebuilt tree.

*Why:* every commit is a potential bisect point, review point, and rollback target. A commit that does
not compile poisons `git bisect`, blocks reverts, and turns the history from a sequence of working states
into a sequence of diffs. The one-compiler minimum keeps fast iteration honest (the per-commit gate runs
one compiler); the both-if-exercised rule stops a known cross-compiler failure from being committed as
if unknown.

---

## 27. The manual reads this tree

Three artifacts in this repository are load-bearing for the external reference manual and are never
decoration or convenience:

- **Snippet-marker comment pairs** — `//! [anchor]` (C++), `#! [anchor]` (CMake/shell) — bracket the
  regions the manual extracts at *its* build time. Treat a marker like code: moving, renaming or
  deleting one breaks the manual build, which names the affected chapter. When editing a marked
  region, the markers move with the meaning they bracket.
- **Documentation programs** live under `projects/<lib>/docprograms/`, never in `examples/` — an
  example teaches a user, a docu-program backs a listing. Each links its library's own dependency
  closure only and is registered as an S-size test, so a docu-program that stops compiling or stops
  doing what the manual says turns a gate red. The directory's README states the contract.
- **The tracked `docs/config-reference/` JSONs** are the source the manual renders its configuration
  tables from; they stay tracked and are regenerated, never hand-edited.

The manual repository carries all extraction machinery (see its `DOCUMENTATION-INVARIANTS.md`, D2);
this tree carries only these hooks.

*Why:* the manual's honesty rests on generation from this tree. The hooks are cheap (comments, a few
tiny programs, tracked JSON) but only work if nobody treats them as removable clutter — and the
people in a position to remove them work here, not in the manual repository.

---

*Add new invariants below as the maintainer establishes them. Keep each rule short, mandatory, and
general — design decisions for a specific feature belong in that feature's design notes, not here.*
