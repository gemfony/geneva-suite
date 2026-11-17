# AI Invariants

Mandatory rules for **AI-driven work** in this repository — orchestration, model selection, and the
division of labour between AI tiers. They complement [`DEVELOPMENT-INVARIANTS.md`](DEVELOPMENT-INVARIANTS.md),
which binds **all** work (human or AI) and remains the sole source of the engineering invariants; nothing
here weakens or duplicates a rule there. Every AI working in this repository reads **both** files before
making any change. An AI invariant is changed only by an explicit maintainer decision, recorded here.

(Cross-check 2026-07-26: no invariant in `DEVELOPMENT-INVARIANTS.md` is purely AI-specific — all bind human
and AI work alike — so none was moved; this file starts with the rules below.)

---

## AI-1. The sweep rule — supervision, mechanical work, and intermediate work run on different tiers

Large parallel efforts are split by the kind of judgment they need, and each kind runs on the tier suited
to it:

- **Mechanical work → Haiku (many agents in parallel).** High-volume, pattern-shaped detection: scanning
  for stale vocabulary, copy-paste identity errors, narration comments, convention drift, sibling
  asymmetries. Mechanical scanners are *detection-only*: they never change code, never decide fixes, and
  every finding must carry the repo-relative file, line, and a **verbatim quote** (the machine-validation
  hook — a finding whose quote does not match the tree is discarded unread).
- **Intermediate work → Sonnet or Opus (one fresh agent per phase).** Executing a reviewed design or plan
  phase: gated refactors, implementation, test writing, per-phase handoff documents.
- **Supervision → Fable (the supervising model).** Design review, ruling preparation for the maintainer,
  independent gate verification before any push, and — critically — **verification of every load-bearing
  finding** a lower tier produced. The supervisor reads the code itself; it does not vote on summaries.

*Why:* the tiers fail differently. Haiku is reliable at pattern detection and unreliable at judgment —
its severity labels are noise and its design/concurrency claims are often confidently wrong; Fable's
attention is the scarce resource and is spent where wrongness is expensive. Matching work to tier makes
the sweep both cheap and safe; mismatching it produces either wasted tokens or plausible-but-wrong changes.

## AI-2. Scanner findings are evidence, never actions

No fix, migration, or deletion is ever executed from a mechanical scan result alone:

- Quotes are validated mechanically against the tree before a finding is even read.
- Scanner severity labels are **untrusted**; the supervisor re-classifies.
- Any finding whose fix would touch behaviour — concurrency, ownership, protocol, ABI, public API — is
  verified **at the supervisor tier, in the code**, before it becomes work. Calibration from the first
  sweep (2026-07-26): of ~15 scanner claims that mutex use violated Invariant 2, **zero** survived
  supervisor verification as migrations — the "violations" were bespoke coordination logic, deliberately
  leaked registries, and plain data guards. Executing them blindly would have damaged the most delicate
  infrastructure in the tree.
- "Unused" claims from scanners compose with Invariants 11 and 23: they are input for a maintainer
  exposure decision, never a licence to delete.

## AI-3. One writer in the checkout; fresh contexts; all state in files

At most **one** AI agent writes in the working tree at a time (read-only work may run in parallel). Each
plan phase is executed by a **fresh** agent that inherits no conversation state; everything a successor
needs lives in persistent files — the active plan with its progress ledger, the per-phase handoff
documents, and the git history. An agent's context is perishable by design; work that exists only in a
conversation does not exist.

## AI-4. AI work is never attributed in the repository

Commit messages, code comments, and documentation carry **no** AI attribution of any kind: no
`Co-Authored-By` trailers naming an AI, no "Generated with …" lines, no session links, no model names in
prose. The git author is the maintainer; the work stands on its own.

The same holds for indirect traces. Commit messages, merge-commit messages and pull-request texts
contain **no paths that point to an AI**: no build, scratch, session or configuration directory named
after an AI tool, and no file inside one. A message that has to say where something was built or
measured names the role of the place ("an external build directory", "a worktree at the pinned
commit"), not its local name. (Maintainer decision, 2026-11-16.)

## AI-5. The supervisor verifies gates independently before anything is pushed

An executing agent's green report is a claim, not a fact. Before a push, the supervising tier re-runs or
independently confirms the gates (build exit codes, warning greps, test counts, the dlopen/e2e evidence
appropriate to the change) on the actual trees. The false-green discipline of the engineering invariants
(separate configure/make/ctest exit codes; a stale tree proves nothing) applies to this verification
exactly as to the original run.

## AI-6. One full review per package; a fix does not earn another

Review has a stopping rule, because a fresh reader always finds something:

- A package of work gets **one** full review by a reader without the drafting context (a frozen
  checkout, verbatim quotes — AI-2). The round that fixes its findings is followed by a **targeted
  check of the changed lines** — does each fix close its finding, did it disturb its surroundings —
  and not by another full review.
- Findings are sorted by what they cost a user. What loses or duplicates results, hangs, crashes or
  produces wrong output is fixed before the series is merged. Documentation, test weaknesses and
  older defects outside the package are fixed as well (Invariants 9 and 15: fixed, not listed), in
  the same series or in a small package of their own, but they start no further round of review.
- The supervisor's verification from scratch (AI-5) is run for a state that is about to be pushed,
  not for intermediate states; between pushes the executing agent's gates carry the work.
- This composes with Invariant 29: a thin slice gets a review of its design before depth is added,
  and its one full review when the depth is in.

*Why:* without a stopping rule the cycle of review and fix runs on with falling yield and rising
cost. In the courtier rework of November 2026 the main branch went through four such rounds, a
side branch through three; the first rounds found a lost result and several hangs, the later ones
rare, cosmetic or older defects, at the same price per round.

## AI-7. The work lists are kept current by whoever changes the work

The work of this repository is tracked in three files under `prompts/`:

- `PLANNED.md` -- decided work, by priority. Its first section, "Now", says what is running and what
  comes next; it is where a successor starts.
- `IDEAS.md` -- undecided work, by priority.
- `DONE.md` -- finished or dropped work, newest first, each entry with its evidence (commit, pull
  request, or the reason it was dropped).

The design papers and packages these entries refer to live beside them (`prompts/designs/`, the
packages and reviews of a work stream); an entry links to them instead of repeating them.

- **Every change of the work updates the lists in the same step.** An agent -- the supervisor as
  much as an executing agent -- that finishes, drops, starts, splits or reorders a piece of work,
  or receives a decision on it, moves the entry accordingly: finished or dropped to `DONE.md` with
  its evidence, decided from `IDEAS.md` to `PLANNED.md`, a new idea sorted into `IDEAS.md` at its
  priority; and keeps "Now" true.
- **Each item stands in exactly one of the three files** (Invariant 20, "state each fact once").
- **A document the lists no longer need** (a design that has been carried out, a superseded
  draft) moves to `prompts/obsolete/`, and the references to it are updated.

*Why:* open work had spread over dated plan files, hand-over notes, packages and design papers,
until nobody could say from one place what was decided, what was running and what was done.
(Maintainer decision, 2026-11-17.)

---

*Add new AI invariants below as the maintainer establishes them. Keep each rule short, mandatory, and
about HOW AI work is organized — engineering rules for the code itself belong in
`DEVELOPMENT-INVARIANTS.md`.*
