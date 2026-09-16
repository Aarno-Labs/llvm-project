# clang-refold architecture

This document states the dependency rules for the clang-refold sources and how
they are enforced. The proof-level design — how evidence becomes a witness, a
candidate, and finally an edit — is described in `ProofArchitecture.md`.

## Layers

Every C/C++ file belongs to exactly one layer. A file may include files in its
own layer or in any lower layer, and never a higher one.

| Rank | Layer | Contents | May not contain |
| ---: | --- | --- | --- |
| 0 | `support` | Domain-free utilities: string and lexical helpers, logging, `format_provider`s, language options, path canonicalization. | Any clang-refold domain type. |
| 1 | `model` | The producer refold map (`RefoldModel`, schema), the A/B token carrier (`PPTok`), path identity. | Derived facts; anything that consults B beyond lexing it. |
| 2 | `carriers` | Value types and pure operations on them: proof vocabulary, owner/state records, candidate/patch/edit carriers. | A service, or a dependency on one. |
| 3 | `analysis` | Facts derived from the model and the A/B streams: alignment and diff, source mapping, directive scanning, structure indexes, macro topology and replay, line-state models. | Theorem admission; candidate construction. |
| 4 | `proof` | Services that decide whether an obligation is discharged: validators, certifiers, classifiers, witness resolution, theorem audit. | Candidate construction; final text. |
| 5 | `planning` | Construction of candidates, patches and edits: macro/include/TU planners, fallback, structural tiling, hunk dispatch, the B-insertion ledger, line-observer layout, sideband edits. | Final assembly. |
| 6 | `emission` | Final text assembly, include materialization, once-guard rewriting, output line-control pruning, closing verification. | Orchestration policy. |
| 7 | `core` | Run orchestration and service composition. | — |
| 8 | `tool` | `clang-refold.cpp`. | — |

When a file's name suggests one layer and its content another, the content
wins. The manifest records the reason, for example `RefoldTokenDiffPlanner`
(alignment, so `analysis`) and `RefoldMacroPlannerHelpers` (model queries and
lexical helpers, so `analysis`). A file that genuinely mixes two layers is
split along that boundary, so each part gets the layer its content and
includers need.

## Directories

Directories are organized by domain (`macro/`, `include/`, `line-control/`,
…), not by layer. A domain directory may hold files from several layers, so
the directory-level include graph is not a meaningful measure of layering and
is expected to stay cyclic. The rules are expressed per file, in the manifest.

The two bottom layers are the exception. They are domain-free and have the
highest fan-in, so they live in their own directories, `support/` and
`model/`. `core/` holds orchestration only.

## Rules

1. **Every file is in `docs/layers.txt`.** Adding, splitting, moving or
   deleting a file updates the manifest in the same commit.
2. **No upward include.** An include that points from a lower layer to a
   higher one fails the check. There is no exception list: fix the
   dependency, or move the code to the layer its content belongs to.
3. **A `.cpp` shares its header's layer.** A `.cpp` without a same-named
   header (an implementation split such as
   `RefoldMacroStandardArgsOnlyOccurrence.cpp`) takes the layer of the class
   it implements.
4. **No file-level include cycles.**
5. **Carriers contain no services.** A service that exists only to be reached
   through a callback is recorded in `docs/CallbackInventory.md`.

## Enforcement

`utils/check_refold_layers.py` enforces rules 1–4. It runs as the lit test
`test/clang-refold/architecture_layers.test`, so every lit run of the suite
checks the architecture.

```bash
# Enforce (what the lit test runs).
python3 clang-tools-extra/clang-refold/utils/check_refold_layers.py

# Print the structural census tracked by the organization roadmap.
python3 clang-tools-extra/clang-refold/utils/check_refold_layers.py --metrics

# List every upward edge, one per line.
python3 clang-tools-extra/clang-refold/utils/check_refold_layers.py --list-upward
```

Include-graph layering is enforced at the source level on purpose. The
library is linked as one target, and a per-layer split would not catch
type-only or inline dependencies in any case.

## Organization-only changes must not change decisions

A change that only reorganizes code must leave every refold decision
unchanged, not merely every emitted file. `utils/refold_replay_diff.py`
checks that by replaying two binaries over the same inputs and comparing
outputs, exit statuses and trace logs.

```bash
B=~/Projects/cpp/tenjin-llvm/release-build
T=clang-tools-extra/clang-refold/utils/refold_replay_diff.py

# 1. Before editing: keep the baseline binary.
cp $B/bin/clang-refold /path/to/scratch/clang-refold.base

# 2. Make sure the lit artifacts exist (any recent full lit run).
$B/bin/llvm-lit -s $B/tools/clang/tools/extra/test/clang-refold

# 3. After rebuilding: replay both binaries and diff.
python3 $T replay-lit --binary /path/to/scratch/clang-refold.base --out /path/to/scratch/base
python3 $T replay-lit --binary $B/bin/clang-refold --out /path/to/scratch/new
python3 $T diff /path/to/scratch/base /path/to/scratch/new

# 4. The scaled check: the tenjin corpus units (25-40 minutes each sweep).
python3 $T replay-corpus --binary /path/to/scratch/clang-refold.base --out /path/to/scratch/cbase
python3 $T replay-corpus --binary $B/bin/clang-refold --out /path/to/scratch/cnew
python3 $T diff /path/to/scratch/cbase /path/to/scratch/cnew
```

`diff` exits 0 only when every unit is identical. A commit that deliberately
renames a logged identifier declares the rename, and the diff applies it to
the baseline first, for example
`diff --rename MixedOwnerTilingProof=StructuralHunkTilingProof base new`. The
flag repeats, and the renames are applied in the order given.
Every remaining difference must be explained. A difference is never waived
because the lit suite still passes.

What the replay reproduces, and why each part matters:

- **Invocation.** Each unit reproduces `refold_tester.py`'s exact invocation:
  flags, working directory (the test's source directory, where the map's
  relative paths resolve), B taken from `expected/`, and the output file
  name. The output's directory is part of the include-replay lookup surface,
  so the output lands in a directory holding no headers, as the harness's
  does.
- **Test-only budgets.** A RUN line's `env CLANG_REFOLD_TEST_ONLY_*=…`
  assignments are applied, and any such variable inherited from the calling
  shell is removed. Several tests set alignment budgets this way.
- **Direct tests.** Tests that run `clang-refold` themselves are replayed in
  place, because their staged headers are named by absolute path. Lit's
  output files are moved aside for the run and restored afterwards.
- **Pinned binary.** The binary is copied into the replay directory first, so
  a rebuild during a sweep cannot swap it.
- **Normalization.** Only run-to-run noise is normalized, and only in logs:
  elapsed-time stamps, random temporary-file suffixes, and the replay
  directory's own path. Emitted files are compared byte for byte.

Known limits:

- A test with several RUN lines sharing one test name keeps only its last
  variant's artifacts on disk; 68 units are such last variants.
- Producer-only tests (`FileCheck` on the map) have no refold to replay.
- An `XFAIL` test with no expected output is skipped and reported.
- A replay reads the producer maps that the last lit run wrote, and a lit run
  regenerates them. One test spells `#line 123 __DATE__`, so its map records
  the date of the lit run, and that date reaches a hashed field of its trace
  log. Replay both binaries against the same lit artifacts; a stored baseline
  replay goes stale for that unit when lit runs on a later day.
- A declared rename is plain text, and it is applied to every file of a
  baseline unit, emitted files included. Before relying on one, check that its
  old spelling occurs in no emitted file, so the rename cannot hide an output
  difference.
