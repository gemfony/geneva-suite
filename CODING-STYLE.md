# Coding style — the single source of truth

This document is the **single source of truth** for how Geneva source code is written. It consolidates the
three artifacts that previously held the style piecemeal:

- [`.clang-format`](.clang-format) — mechanical layout (indentation, line length, brace placement, alignment).
- [`.clang-tidy`](.clang-tidy) — the semantic rules a formatter cannot see (naming, modern-C++ usage, and a
  large battery of correctness/performance checks).
- the manual chapter `arch-coding.tex` (in the separate `geneva-manual-revised` LaTeX repository).

The relationship is deliberate: **where a rule can be machine-checked, `.clang-format` / `.clang-tidy` remain
the mechanical enforcers** — run them and what they say goes. This document states the *intent*, shows the
*examples*, and records the rules the tools cannot express. The manual's coding-conventions chapter is
intended to be **derived from this file**, so the prose and code samples here are authoritative; if the manual
disagrees, this file wins and the manual is regenerated.

This complements [`DEVELOPMENT-INVARIANTS.md`](DEVELOPMENT-INVARIANTS.md) (architecture-level rules) — in
particular Inv 16 (write to the highest configured C++ standard), Inv 23 (a complete public API is the
contract), and the config-key-stability rule below.

> **Key points**
> 1. The house style is captured in `.clang-format` (layout) and `.clang-tidy` (naming, modernization,
>    correctness), both in the source-tree root; this file is their prose-and-examples single source of truth.
> 2. Run `clang-format` and `clang-tidy` on every file you touch before submitting — what they enforce is not
>    optional.
> 3. Identifiers follow a fixed scheme: `CamelCase` types, `camelBack` functions/methods, `snake_case` locals
>    and parameters, `snake_case` private/protected members with a trailing underscore, and template
>    parameters in `snake_case` ending in `_type` (a single capital letter allowed) or, for values, `UPPER_CASE`.
> 4. The code targets **C++23**: `std` sized integers, `noexcept`, `static_cast`, `#pragma once` and
>    `inline constexpr` replace their older Boost and preprocessor equivalents.
> 5. Documentation is written in Doxygen format; a few rules remain a matter of review rather than tooling.

## 1. Tooling

Before opening a pull request, run both tools over the files you have touched. The formatter rewrites layout
in place; the linter reports (and, with `-fix`, applies) the semantic rules.

```shell
# Re-flow layout in place, using the repository's .clang-format
clang-format -i path/to/GMyClass.cpp

# Run the semantic checks, using the build's compilation database
clang-tidy -p build path/to/GMyClass.cpp
```

`.clang-format` is based on the LLVM style, adjusted to Geneva's habits: a four-space indent with tabs never
used, a hundred-column limit, and constructor-initializer / inheritance lists broken one entry per line with a
leading comma (§5). `.clang-tidy` enables the `bugprone`, `cert`, `clang-analyzer`, `concurrency`,
`cppcoreguidelines`, `modernize`, `performance`, `portability` and `readability` families, with a small set of
documented exceptions. Its `HeaderFilterRegex` restricts warnings to Geneva's own headers, so noise from Boost
and the standard library does not drown out the signal.

The whole tree was formatted once with `.clang-format`; that commit is listed in `.git-blame-ignore-revs`, so
`git blame` can skip it once git is told to use the file:
`git config blame.ignoreRevsFile .git-blame-ignore-revs`.

## 2. File layout

### 2.1 The license banner

Every source file — header, implementation or script — opens with the standard Geneva license banner. The
collection is released under the Apache License, version 2.0; the banner states as much, notes that individual
files may carry a different license, and points at the top-level `NOTICE` file for authorship:

```cpp
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
```

### 2.2 Include protection and include order

Headers protect against multiple inclusion with `#pragma once`, placed immediately after the license banner.
The older include-guard idiom (`#ifndef`/`#define`/`#endif`) is no longer used anywhere. Includes then follow
in clearly labelled groups — the global Geneva defines first, then standard-library headers, then Boost, then
Geneva's own headers — so a file's dependencies read at a glance:

```cpp
#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <filesystem>
#include <string>

// Boost header files go here
#include <boost/json/value.hpp>

// Geneva header files go here
#include "common/GLogger.hpp"
```

**Carve-out — Catch2 test drivers (maintainer ruling 2026-07-26).** Files under a library's `tests/`
tree follow the test-suite convention instead: the Catch2 headers may come first and
`common/GGlobalDefines.hpp` may be omitted — a test driver consumes the already-built library interface
rather than participating in it. This blesses the existing, uniform pattern across the test suites; do
not "normalize" test drivers to the library layout, and do not flag them for it in reviews.

### 2.3 File names and extensions

Header files use the extension `.hpp` and implementation files `.cpp`. A file is named after the principal
class it contains and, as a rule, holds only that one class — small helper classes and structs closely tied to
it being the usual exception. The name mirrors the class, so `GParserBuilder` lives in `GParserBuilder.hpp`
and `GParserBuilder.cpp`.

## 3. Documentation

Comments are written in Doxygen format, so a reference manual can be generated directly from the source. Every
file, class, free function, member function and data member should carry a comment.

A class is preceded by a description of its role; a member-function **declaration** in a header carries a
one-line `@brief`; a member-function **definition** carries a fuller description, documenting each parameter
with `@param` and the result with `@return`. Definitions are separated by a banner comment, which makes a long
implementation file easy to scan.

```cpp
/******************************************************************************/
/**
 * A description of the class's role goes here, before the class declaration.
 */
class GExample {
public:
    /** @brief The copy constructor */
    GExample(const GExample &);

    // ...
};

/******************************************************************************/
/**
 * Checks for equality with another GExample object.
 *
 * @param cp A constant reference to another GExample object
 * @return A boolean indicating whether both objects are equal
 */
bool GExample::operator==(const GExample &cp) const {
    // comparison code
}
```

A data member that is **not serialized** says so in its comment, beginning with `transient:` and the reason
(`///< transient: <why>` after the member, or `/// transient: <why>` above it), so a reader sees at once what
a checkpoint or the wire does not carry. Where the class has a member list (`localMembers_()`), that list
stays the one source of what is serialized, loaded and compared (Invariant 4); the comment explains a
member's absence from it, it does not replace it.

```cpp
std::unique_ptr<GThreadPool> pool_; ///< transient: a run-time resource, built on first use
```

An empty body — common for an intentionally empty constructor — is marked as such rather than left blank, so
the emptiness reads as deliberate:

```cpp
explicit g_error_streamer(std::source_location const &loc = std::source_location::current())
    : where_and_when_(Gem::Common::timeAndPlace(loc)) { /* nothing */ }
```

## 4. Naming

Naming is the convention most thoroughly machine-enforced, through the `readability-identifier-naming` check.
Each kind of identifier has a fixed case:

| Entity | Case | Example |
|---|---|---|
| Class, struct, enum, union | `CamelCase` | `GParserBuilder` |
| Namespace, concept | `CamelCase` | `Gem::Geneva`, `MPMCQueue` |
| Type or template template parameter | `snake_case` ending in `_type` | `archive_type`, `derived_type` |
| Type parameter without a specific role | a single capital letter, `s` for a pack | `T`, `U`, `F`, `Ts` |
| Non-type template parameter | `UPPER_CASE` | `N`, `N_DIM`, `CAP` |
| Function, method | `camelBack` | `setClientMode` |
| Parameter, local variable | `snake_case` | `client_mode` |
| Private / protected member | `snake_case_` (trailing underscore) | `client_mode_` |
| Public member (rare, discouraged) | `camelBack` | `someValue` |
| Enum constant | `UPPER_CASE` | `GEM_BINARY` |
| Macro, global / static / class constant, `constexpr` variable | `UPPER_CASE` | `CONFIG_EMISSION_FLAG` |
| Type alias, typedef | unconstrained | `value_type` |

Template parameters follow their own rule (maintainer decision of 2026-11-15). A type parameter -- also a
constrained one, `template <std::integral int_type>`, and a template template parameter -- is `snake_case`
and ends in `_type`, so a reader sees at once that the name stands for a type: `archive_type`,
`processable_type`, `args_type` for a pack. A trailing `T` or `Type` of the old form is not kept
(`ConfigT` became `config_type`, not `config_t_type`). A parameter without a specific role may stay a single
capital letter, with an `s` for a pack (`T`, `U`, `R`, `F`, `Ts`); nothing else is exempt (`Args` is
`args_type`, `URBG` is `urbg_type`). A non-type parameter is `UPPER_CASE`, as the other constants: `N`,
`N_DIM`, `CAP`, `N_POOLS`. Where the natural name is already a member alias of the template
(`using key_type = ...`), the parameter takes a more specific name (`ordered_key_type`), because the alias
is public API and stays.

```cpp
template <typename archive_type>
void serialize(archive_type &ar, unsigned int version);

template <typename T, std::size_t CAP>
class GPreallocatedMPMCQueueT;

template <typename... args_type>
void emplace_back(args_type &&...args);
```

Two points deserve emphasis, because they changed when the conventions were modernized. First, **local
variables and function parameters are `snake_case`**, not the camel case the older manual used. Second,
**private and protected data members are `snake_case` with a trailing underscore**; the trailing underscore is
retained from the original style, but the body of the name is now lower-case. Public data members — rare and
discouraged — keep camel case.

```cpp
// From GOptimizer (excerpt): a camelBack method, a snake_case parameter, a snake_case_ member.
class GOptimizer {
public:
    void setCopyBestIndividualsOnly(bool copy_best_individuals_only);
    bool onlyBestIndividualsAreCopied() const;
    // ...

private:
    bool copy_best_individuals_only_ = true;
    // ...
};
```

### 4.1 The method-name carve-out (methods containing an underscore)

`camelBack` is the rule for functions and methods, but `.clang-tidy` exempts **any method whose name contains
an underscore** (`readability-identifier-naming.MethodIgnoredRegexp: '.*_.*'`). That single carve-out
deliberately covers three legitimate `snake_case`-flavoured method families, so none of them is a lint
violation:

1. **The trailing-underscore private-virtual hooks** — Geneva's core extension pattern: `load_`, `save_`,
   `clone_`, `compare_`, `addConfigurationOptions_`, and the like.
2. **The STL container surface** — the container templates mirror `std::`: `push_back`, `pop_back`,
   `emplace_back`, `shrink_to_fit`, `max_size`, `begin`/`end`, and so on. Forcing these to `camelBack` would
   break the drop-in-replacement contract.
3. **`snake_case` internal helpers** — e.g. `compare_base`, and helpers in the newer
   transport / concurrency layers that mirror an STL/Boost.Asio idiom (`async_schedule`).

New code that is **not** one of these cases uses `camelBack`. When a name could go either way, prefer
`camelBack`.

### 4.2 The type aliases are unconstrained on purpose

The type-alias row above is deliberately unconstrained. Geneva's container templates mirror the standard
library surface (`value_type`, `const_iterator`, `size_type`, …), and forcing those into `CamelCase` would
break the contract that lets the containers be used as drop-in replacements.

### 4.3 The `G` / `T` / `I` conventions (not machine-enforced)

Three older habits survive as conventions but are **not** machine-enforced, because the original rules hedge
them with "usually" / "where appropriate" and enforcing them trips over code that interfaces with external
libraries: core-framework classes begin with an upper-case `G` (`GParserBuilder`); template classes end with
an upper-case `T` (`GSingletonT`); and the rare interface class ends with an upper-case `I` (`GGPUEvaluableI`).
New code follows these where it sensibly can.

### 4.4 Stability: config keys and public API names are frozen (from the invariants, not the linter)

Style convergence must never break a caller or a config file. The following are **frozen** — never renamed to
satisfy a naming rule, and never mass-renamed by a lint sweep:

- **Configuration-option keys** — every string key read from a JSON config or `program_options`
  (`add_options()` keys, JSON member names, checkpoint field names). Renaming a config key silently
  incapacitates existing configuration files; a config key is a format contract, not code style. If a method
  that *backs* a key is renamed, the **key string stays put**.
- **Public API names** — a method reachable by an out-of-tree consumer is the contract (Inv 23); its name is
  not churned for style. A grandfathered public name that predates a rule is left alone, not renamed.

Consequently there is **no standalone identifier-rename sweep**: the naming rules govern new and modified code;
the existing tree converges only as files are touched for other reasons, and never across a config key or a
public signature. The one exception is the collection-neutral `gem` naming of the libraries below geneva
and of the build system (`geneva_exception` → `gem_exception`, the `GENEVA_*` build options → `GEM_*`, and
so on), the maintainer's decision of 2026-11-17/19.

### 4.5 Short, telling names

A name says what the thing *is*, in the reader's terms: `best_parameters`, not `xy` or `p`, and not
`vector_of_best_parameter_values_found`. Prefer the shortest name that still tells; a reader should not have
to look up a declaration to know what a variable holds. Single letters and terse names are fine where they
mirror a formula the code implements (`dx`, `dy` for x - 1 and y + 2) or index a short loop. This is a
judgment rule, not a linter rule, and like every naming rule it applies to new and modified code (§4.4).

## 5. Layout and control flow

Layout is owned entirely by `.clang-format`: rather than memorise it, run the formatter. The rules below are
recorded so the intent is on file and hand-written code starts close to target.

Indentation is four spaces; tabs are never used. Lines stay within a hundred columns. The opening brace of a
function, class, `struct`, `enum` or `namespace` sits on the same line as its header, and so does the brace
that follows a control statement. Namespace bodies are **not** indented, and access specifiers are dedented to
the enclosing scope. There is no space between a control keyword and its parenthesis (`if(condition)`, not
`if (condition)`).

```cpp
if(condition) {
    // ...
}
else if(other_condition) {
    // ...
}
else {
    // ...
}

for(auto it = v.begin(); it != v.end(); ++it) {
    // prefer the pre-increment ++it, which matters for iterators
}

try {
    // code that may throw
}
catch(const gem_exception &e) {
    // handle it; catch by const reference
}

do {
    // ...
}
while(condition);
```

A `switch` indents its `case` labels to the `switch`, gives each case a `break`, and wraps any case that needs
its own local variables in a block:

```cpp
switch(mode) {
case serializationMode::GEM_BINARY:
    // ...
    break;

case serializationMode::GEM_JSON: {
    std::string buffer; // a case-local variable needs its own block
    // ...
} break;

default:
    // ...
    break;
}
```

Constructor-initializer lists and inheritance lists are broken one entry per line, with the colon and the
separating commas leading each line. The opening brace follows the final entry. A `template` declaration goes
on its own line, and pointers and references bind to the right (`T *p`, `T &r`).

```cpp
class GDerived
  : public GBase
  , private GHelper {
public:
    GDerived();
};

GDerived::GDerived()
  : GBase()
  , value_one_(1)
  , value_two_(2) {
    // body
}

template <typename T>
bool isAcceptable(const T &candidate) {
    return candidate.valid();
}
```

A too-long **argument list** also wraps one argument per line with the closing parenthesis on its own line —
but with **trailing** commas, not comma-first: clang-format cannot emit leading commas for parameters (it does
for constructor-initializer and inheritance lists, above, but not for parameter lists), so the trailing-comma
form is the formatter's output and is authoritative here:

```cpp
void myComplicatedFunction(
    const double &argument_one,
    const double &argument_two,
    double &write_to_me
) {
    // ...
}
```

Every parameter of a function with a body is named, and a declaration in a header uses the same name as the
definition (`readability-inconsistent-declaration-parameter-name`, strict, checks that they agree). The source
of a copy or move constructor or assignment is called `cp`. Defaulted and deleted special members keep their
parameters unnamed, since they have no body. For a setter of a private member, the parameter name mirrors the
member (without the trailing underscore), and the function name mirrors it too.

Namespaces close with a naming comment — `} /* namespace Gem::Common */`. clang-format **preserves** this form
but never generates it (`FixNamespaceComments: false`), so it is author-maintained; write it by hand and do
not let a tool rewrite it to `} // namespace ...`.

### 5.1 Where clang-format cannot express the convention

A few layout intentions cannot be produced by clang-format; they are the author's (and reviewer's)
responsibility, and this document — not an imagined formatter output — is their statement of record. These are
**limitations, not contradictions**: the configuration realizes everything it can, and `.clang-format`'s own
comments note the same gaps.

- **Leading commas apply to constructor-initializer and inheritance lists, but not to parameter lists.**
  clang-format emits comma-first for the former and trailing-comma for the latter (§5); there is no setting
  that makes parameter lists comma-first.
- **The class opening brace is always attached.** The older habit of giving a *derived* class its own brace
  line while a plain class keeps `class Base {` cannot be made conditional; clang-format attaches the brace in
  both cases, so all class heads read `… {` on their final line.
- **The namespace-closing comment** (`} /* namespace X */`) is written and maintained by hand (above).

The remaining human-only conventions (Doxygen completeness, the license banner, header parameter-name
omission, `typename` over `class`, the `G`/`T`/`I` affixes, one-class-per-file) are listed in §8.

## 6. Modern C++ usage

The code targets **C++23** throughout (the active standard is set centrally by `CMAKE_CXX_STANDARD` in
`CMakeModules/CommonGemBuild.cmake`, driven by `genevaConfig.gcfg`; write to whatever standard that names —
Inv 16). A number of older idioms have been retired, most of it enforced by the `modernize`,
`cppcoreguidelines` and `google` check families.

- **Sized integers from the standard library.** Use `std::uint32_t` and friends from `<cstdint>` rather than
  the old `boost::uint32_t`. The intent — explicit, portable widths instead of a bare `int` — is unchanged.
- **`noexcept` instead of `throw()`.** Exception specifications use `noexcept`; the obsolete dynamic `throw()`
  form is gone.
- **No `using namespace` in a header**, also not inside a function body: it would reach into every file that
  includes the header. In the file that holds `main()` it is acceptable, and in tests it does not matter.
  `using namespace std;` is not used; `std::vector` is written with its scope. The check
  `google-build-using-namespace` is switched off, because it would also flag the allowed uses, so the rule is
  kept in review.
- **No `#define` for constants, and no function-like macros.** A named constant is an `inline constexpr`
  variable, which is typed and obeys scope; a would-be function macro is a template or an `inline` function.
  The handful of macros that genuinely must remain (visibility and build-guard macros, and a few that capture
  `__FILE__`/`__LINE__` at a throw site) are listed explicitly in `.clang-tidy`.
- **A check is one call of one of the two helpers of `common/GChecks.hpp`**, so a reader sees at once
  that it is a check and not program logic. A cheap precondition or invariant (a comparison, a size, a null
  test) is `Gem::Common::checkThat(condition, "what went wrong ", value)` and is on in every build; a check
  whose cost would count in a release build (a loop over all elements, a `dynamic_cast` or a test on a path
  run for every random number) is `GEM_DEBUG_CHECK(condition, ...)` and is on only without `NDEBUG`. Both
  throw `gem_exception` with the place of the check. A check is not open-coded as `if(...) throw`, and no
  project macro switches code on by build type.
- **No C-style casts.** Use `static_cast` and the other named C++ casts, so the kind of conversion is explicit
  and searchable.
- **Smart pointers are made with `std::make_shared` and `std::make_unique`**, not from a naked `new`
  (`std::shared_ptr<T> p(new T(...))`, `p.reset(new T(...))`): the shared case needs one allocation
  instead of two, the object is owned from the moment it exists, and a review finds no `new` to check (Inv 21).
  `new` stays only where a make function cannot do the job -- a constructor that is private and reached
  through a friend, a `unique_ptr` with a deleter of its own -- and then the site says why, in a comment
  above a `NOLINTNEXTLINE` for `modernize-make-shared` / `modernize-make-unique`, the two checks that
  keep the pattern out.
- **Prefer the modern spelling.** `override` on overriding functions, `nullptr` for null pointers,
  `emplace_back` over `push_back` of a temporary, range-based `for` where it reads more clearly, and `auto`
  where it removes a long, redundant type name — all nudged by the `modernize` checks.

```cpp
// Not this:
//   #define DEFAULT_POPULATION_SIZE 100
// but this:
inline constexpr std::size_t DEFAULT_POPULATION_SIZE = 100;

// Not this:
//   std::shared_ptr<GHistogram1D> histogram(new GHistogram1D(100, 0., 1.));
// but this:
auto histogram = std::make_shared<GHistogram1D>(100, 0., 1.);
```

Two long-standing rules carry over unchanged. **const-correctness** is taken seriously: anything that can be
`const` should be, from member functions that do not mutate their object to parameters passed by reference.
And **non-const global variables are forbidden** — Geneva is heavily multi-threaded, and mutable global state
is a race waiting to happen.

## 7. Concurrency

Because so much of Geneva runs in parallel, a few rules exist not for tidiness but for correctness, and the
`concurrency` check family watches for the common mistakes. The most useful habit: a function that may be
called from several threads and that takes or returns data should prefer to pass it **by value** rather than
by reference, so no two threads end up sharing — and racing on — the same object. Shared mutable state is
confined to a few well-audited, explicitly-guarded places; everywhere else, immutability and value semantics
are the first line of defence. (This is the code-level companion of DEVELOPMENT-INVARIANTS Inv 2 — concurrency
primitives come only from `common/concurrency/` — and Inv 21 — resources are owned by RAII.)

## 8. What the tools do not check

A handful of conventions cannot be expressed as a `clang-tidy` check, or only partially, and so remain the
responsibility of the author and the reviewer (they are also noted at the foot of `.clang-tidy`):

- the presence and completeness of Doxygen comments on every file, class, function and member;
- the exact license banner at the top of each file;
- no `using namespace` in a header (its check would also flag the uses that are allowed);
- the preference for `typename` over `class` in template parameter lists;
- the `G` prefix and the `T` / `I` suffixes, which are conventions rather than rules;
- the file-naming and one-class-per-file rules, which live at the filesystem level;
- the config-key / public-API name freeze (§4.4), which is a project invariant rather than a lint rule.

None of these is onerous, and all of them repay the small effort: together with the machine-checked rules,
they are what lets a newcomer read any file in the collection and find it already familiar.
