# common/doc

Per-library documentation directory for the **Common** library
(utilities: logging, thread pool, formula parser, plot designer,
serialization helpers, bounded buffers, exception types).

## Building the docs

```sh
cd /your/build && make doc-common      # this library only
cd /your/build && make doc             # all libraries (aggregator)
```

`make doc` depends on `doc-common`, `doc-hap`, `doc-courtier`, `doc-geneva`
in that order (per-lib `CMakeLists.txt` declares the dependency chain so
Doxygen TAGFILES from upstream libs are available when downstream libs
are documented).

## Output location

Generated HTML / XML / tag files land in the **build tree**, not in the
source tree:

```
<build>/common/doc/
    Doxyfile          configured from Doxyfile.in
    common.tag        Doxygen tag file (consumed by hap/courtier/geneva)
    html/             browsable HTML reference
    xml/              XML for downstream tooling (Sphinx + Exhale, etc.)
```

No upstream tag files for Common -- it is the foundation library.

## What lives here

- `Doxyfile.in`     -- per-library Doxygen configuration template
                       (~50 lines; configured by CMake's `configure_file`
                       with per-library variables -- see `CMakeLists.txt`)
- `CMakeLists.txt`  -- registers the `doc-common` target
- `README.md`       -- this file

Reserved for future Common-library manuals and design notes (markdown,
images, design diagrams).

## Settings notes

The Doxyfile is intentionally minimal: only keys that genuinely differ
from Doxygen's defaults are set. `WARN_AS_ERROR=NO` and
`WARN_IF_UNDOCUMENTED=NO` because Geneva is not fully Doxygenised yet --
focused doc-error warnings still fire.
