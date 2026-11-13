# hap/doc

Per-library documentation directory for the **Hap** library
(random-number factory, `GRandomT` generators, distribution wrappers,
optional CUDA-based RNG `GCUDARng` under `USECUDARNG`).

## Building the docs

```sh
cd /your/build && make doc-hap         # this library only
cd /your/build && make doc             # all libraries (aggregator)
```

`doc-hap` automatically depends on `doc-common` so the Common tag file
exists when Doxygen processes Hap (enables cross-references into Common's
HTML).

## Output location

Generated HTML / XML / tag files land in the **build tree**:

```
<build>/hap/doc/
    Doxyfile          configured from Doxyfile.in
    hap.tag           Doxygen tag file (consumed by courtier/geneva)
    html/             browsable HTML reference
    xml/              XML for downstream tooling
```

## What lives here

- `Doxyfile.in`     -- per-library Doxygen configuration template
- `CMakeLists.txt`  -- registers the `doc-hap` target and its `doc-common`
                       dependency
- `README.md`       -- this file

Reserved for future Hap-library manuals (e.g. an overview of the RNG
hierarchy, CUDA setup notes, distribution-wrapper extension guide).

## Settings notes

The Doxyfile is intentionally minimal: only keys that genuinely differ
from Doxygen's defaults are set. `WARN_AS_ERROR=NO` and
`WARN_IF_UNDOCUMENTED=NO` because Geneva is not fully Doxygenised yet.
