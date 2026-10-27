# courtier/doc

Per-library documentation directory for the **Courtier** library
(consumer-based parallelisation framework: one consumer per process held
in the `GConsumerRegistry` -- local thread-pool / Asio / Beast-websocket /
MPI / optional GPU consumers -- command-container protocol,
processing-container CRTP).

## Building the docs

```sh
cd /your/build && make doc-courtier    # this library only
cd /your/build && make doc             # all libraries (aggregator)
```

`doc-courtier` automatically depends on `doc-common` and `doc-hap` so
both upstream tag files exist for cross-references.

## Output location

Generated HTML / XML / tag files land in the **build tree**:

```
<build>/courtier/doc/
    Doxyfile           configured from Doxyfile.in
    courtier.tag       Doxygen tag file (consumed by geneva)
    html/              browsable HTML reference
    xml/               XML for downstream tooling
```

## What lives here

- `Doxyfile.in`     -- per-library Doxygen configuration template
- `CMakeLists.txt`  -- registers the `doc-courtier` target and its
                       `doc-common` + `doc-hap` dependencies
- `README.md`       -- this file

Reserved for future Courtier-library manuals (consumer protocol,
submission/return flow diagrams, MPI deployment notes, websocket
security considerations).

## Settings notes

The Doxyfile is intentionally minimal: only keys that genuinely differ
from Doxygen's defaults are set. `WARN_AS_ERROR=NO` and
`WARN_IF_UNDOCUMENTED=NO` because Geneva is not fully Doxygenised yet.
