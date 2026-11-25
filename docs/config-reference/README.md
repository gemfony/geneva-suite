# Configuration reference

The `config/` subdirectory holds a **generated reference** of Geneva's standard configuration files:
`GOptimizer.json`, every optimization-algorithm configuration, and the standard individual configuration
(`GFunctionIndividual.json`). It exists so you can see, from the source tree and without building or running
anything, **which configuration files exist and what every parameter means** — each parameter node carries
its `comment`, its code `default` and its `value`.

## This is generated — do not edit by hand

Every file here is produced from the code's registered parameters and comments (the same machinery that
backs `--update-configs`), so it can never drift from the code. The header carries no creation timestamp,
so the output is byte-stable and regenerating it produces no spurious diff.

- **Regenerate** (after the code's registered defaults or comments change):

  ```
  make config-reference
  ```

- **Verify it is current** (CI / pre-commit — regenerates to a temporary directory and fails if it differs
  from what is checked in, without writing into the source tree):

  ```
  make config-reference-check
  ```

## What this is not

These files are **documentation**, not the configuration any binary reads at runtime. A built or installed
binary reads its own `config/` directory. To get a runnable, editable copy of a binary's configuration, run
that binary once with `--update-configs` (it creates any missing config from these same defaults), or copy
the file you want from here. Per-example directories carry only their intentional overrides
(`config-overrides.json`), not a full copy of these defaults.
