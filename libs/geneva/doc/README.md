# geneva/doc

Per-library documentation directory for the **Geneva** library
(the largest of the five; core optimisation: `Go2` driver, the flat
`GGenome` model with OA-owned adaption configs, the optimisation
algorithms -- EA, SA, swarm, CGD, Nelder-Mead, parameter scan, ACO,
PSO, generalized SA, sepCMA-ES, the meta-EA -- per-category
`GCommonInterfaceT<Root>` CRTP roots, individuals, pluggable monitors;
absorbed the former geneva-individuals library in the 1.99 redesign).

## Building the docs

```sh
cd /your/build && make doc-geneva      # this library only
cd /your/build && make doc             # all libraries (aggregator)
```

`doc-geneva` automatically depends on `doc-common`, `doc-hap`, and
`doc-courtier` so all three upstream tag files exist for cross-references.

## Output location

Generated HTML / XML / tag files land in the **build tree**:

```
<build>/geneva/doc/
    Doxyfile         configured from Doxyfile.in
    geneva.tag       Doxygen tag file (no downstream consumer in-tree;
                     handy for external projects that build against the
                     installed Geneva and want cross-refs into the docs)
    html/            browsable HTML reference
    xml/             XML for downstream tooling
```

## What lives here

- `Doxyfile.in`     -- per-library Doxygen configuration template
- `CMakeLists.txt`  -- registers the `doc-geneva` target and its
                       `doc-common` + `doc-hap` + `doc-courtier`
                       dependencies
- `README.md`       -- this file

Reserved for future Geneva-library manuals: the `Go2` workflow tutorial,
the algorithm-chaining example walkthrough, the per-category CRTP-roots
design note, the personality-traits dispatch model, the pluggable-monitor
extension guide, JSON config-file reference, etc.

## Settings notes

The Doxyfile is intentionally minimal: only keys that genuinely differ
from Doxygen's defaults are set. `WARN_AS_ERROR=NO` and
`WARN_IF_UNDOCUMENTED=NO` because Geneva is not fully Doxygenised yet --
focused doc-error warnings still fire and should be addressed.
