#!/usr/bin/env python3
#
# ===- refold_replay_diff.py - clang-refold binary equivalence --*- python -*-===#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ===-----------------------------------------------------------------------===#
#
# Show that two clang-refold binaries make identical decisions.
#
# A green lit suite only shows that each emitted file matches its expectation;
# a changed proof decision that still produces the expected bytes is invisible
# to it.  This script replays a binary over inputs that already exist on disk
# and records, per unit, the emitted source, the exit status, and the
# normalized log.  The lit harness runs at --log-level=trace, so the log records
# every proof, witness and fallback decision.  Diffing two such records is the
# equivalence check that organization-only changes must pass.
#
#   replay-lit     Replay every lit test whose artifacts are present under the
#                  build tree's Output/ directory, with the exact flags,
#                  working directory, inputs and output naming that
#                  refold_tester.py used.
#   replay-corpus  Replay the tenjin corpus units under /tmp/pytest-of-$USER,
#                  with the invocation of corpus-sweep/sweep.sh.  Units are keyed
#                  by path, never by position.
#   diff           Compare two replay directories.  Exit status 1 when any unit
#                  differs, is missing, or was replayed with a different command.
#
# The binary is copied into the replay directory before the first unit runs,
# so a rebuild during a sweep cannot swap it.  Units run one at a time.
#
# Every replay writes its outputs through one fixed staging directory and then
# moves them into its own record.  The output's path is observable: it reaches
# `__FILE__`-style observers and every length derived from them, so two replays
# writing to differently named directories would differ in logged byte counts
# for reasons that have nothing to do with the binary.  Two replays must
# therefore not run at the same time.
#
# Normalization removes only run-to-run noise: the elapsed-seconds log prefix,
# inline elapsed stamps, random temporary-file suffixes, and the replay
# directory's own path.  Everything else must match byte for byte.
#
# ===-----------------------------------------------------------------------===#

import argparse
import getpass
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

DEFAULT_LIT_OUTPUT = os.path.expanduser(
    "~/Projects/cpp/tenjin-llvm/release-build/tools/clang/tools/extra/test/"
    "clang-refold/Output"
)
REPLAY_DEST_TOKEN = "<REPLAY>"
DEFAULT_STAGE = os.path.join(
    tempfile.gettempdir(), f"clang-refold-replay-stage-{getpass.getuser()}"
)

ELAPSED_PREFIX_RE = re.compile(r"^\s*\[\s*\d+\.\d+\]", re.M)
INLINE_ELAPSED_RE = re.compile(r"\[\s*\d+\.\d+\]")
# llvm::sys::fs::createTemporaryFile names: <prefix>-<six hex digits>.<suffix>
TEMP_NAME_RE = re.compile(r"(clang-refold-[a-z-]+)-[0-9a-f]{6}(?=\.)")
TEST_ONLY_ENV_PREFIX = "CLANG_REFOLD_TEST_ONLY_"
ASSIGNMENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def normalize(text, dest):
    """Remove run-to-run noise from a replay log."""
    text = text.replace(dest, REPLAY_DEST_TOKEN)
    text = ELAPSED_PREFIX_RE.sub("[T]", text)
    text = INLINE_ELAPSED_RE.sub("[T]", text)
    return TEMP_NAME_RE.sub(r"\1-RAND", text)


def tester_arguments():
    """Mirror the argument surface of test/clang-refold/refold_tester.py."""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--with-lines", action="store_true")
    parser.add_argument("--relaxed", action="store_true")
    parser.add_argument("--verify-output", default="fatal")
    parser.add_argument("--expect-refold-fail", action="store_true")
    parser.add_argument("--emit-edit-map", action="store_true")
    parser.add_argument("--clang-flags-mode", action="store_true")
    for name in ("clang", "refolder", "headers", "expected", "log", "src", "tmp"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("testname")
    parser.add_argument("extras", nargs=argparse.REMAINDER)
    return parser


def executed_commands(script_text):
    """Return each executed RUN command of a lit .script as a word list.

    A lit .script echoes each RUN line inside single quotes and then executes
    it; only the executed copies are returned.
    """
    commands = []
    for line in script_text.splitlines():
        executed = line.split("&& {   ", 1)
        if len(executed) == 2:
            commands.append(shlex.split(executed[1].split("; }", 1)[0]))
    return commands


def command_segments(words):
    """Split one executed command at `&&` into (environment, words) pieces.

    A leading `env NAME=VALUE ...` is removed from a piece and returned as its
    environment; RUN lines use it to set clang-refold's test-only budgets.
    """
    segments = []
    segment = []
    for word in words + ["&&"]:
        if word != "&&":
            segment.append(word)
            continue
        environment = {}
        if segment and segment[0] == "env":
            segment = segment[1:]
            while segment and ASSIGNMENT_RE.match(segment[0]):
                name, value = segment.pop(0).split("=", 1)
                environment[name] = value
        segments.append((environment, segment))
        segment = []
    return segments


def tester_invocations(script_text):
    """Return (environment, refold_tester.py argv) pairs in execution order."""
    invocations = []
    for words in executed_commands(script_text):
        for environment, segment in command_segments(words):
            for i, word in enumerate(segment):
                if word.endswith("refold_tester.py"):
                    invocations.append((environment, segment[i + 1 :]))
                    break
    return invocations


SHELL_OPERATOR_RE = re.compile(r"^(\||\|\||;|<|[0-9]*>>?(&[0-9]+)?)$")


def command_words(words):
    """Return the arguments before the first pipe or redirection."""
    for index, word in enumerate(words):
        if SHELL_OPERATOR_RE.match(word):
            return words[:index]
    return words


def direct_invocations(script_text):
    """Return (cwd or None, environment, argv) per direct refold command.

    Some tests bypass refold_tester.py and run clang-refold themselves, usually
    as `cd DIR && clang-refold ...`.  `--check` runs are verification, not
    refolds, and are not returned.
    """
    invocations = []
    for words in executed_commands(script_text):
        cwd = None
        for environment, segment in command_segments(words):
            if len(segment) == 2 and segment[0] == "cd":
                cwd = segment[1]
            elif (
                segment
                and segment[0].endswith("clang-refold")
                and "--check" not in segment
            ):
                invocations.append((cwd, environment, command_words(segment[1:])))
    return invocations


def expected_base(args):
    """refold_tester.py's expected-directory rule, including define labels."""
    extras = list(args.extras)
    if extras and extras[0] == "--":
        extras = extras[1:]
    if args.clang_flags_mode or not extras:
        return os.path.join(args.expected, args.testname)
    label = "define_" + "_".join(m.replace("=", "@") for m in extras)
    return os.path.join(args.expected, args.testname, label)


def lit_units(lit_output):
    """Yield (key, invocation, shadowed) for every replayable lit refold.

    An invocation is either parsed refold_tester.py arguments (with the RUN
    line's environment attached as `.environment`) or a
    ("direct", cwd, environment, argv) tuple for a test that runs clang-refold
    itself.
    """
    parser = tester_arguments()
    for script in sorted(os.listdir(lit_output)):
        if not script.endswith(".script"):
            continue
        with open(os.path.join(lit_output, script), encoding="utf-8") as f:
            text = f.read()
        invocations = tester_invocations(text)
        if not invocations:
            for index, (cwd, env, argv) in enumerate(direct_invocations(text)):
                key = script[: -len(".script")]
                yield f"{key}.direct{index}", ("direct", cwd, env, argv), 0
            continue
        # Invocations sharing one (tmp, testname) overwrite each other's
        # artifacts; only the last one executed left inputs on disk.
        last = {}
        for environment, words in invocations:
            args = parser.parse_args(words)
            args.environment = environment
            last[(args.tmp, args.testname)] = args
        shadowed = len(invocations) - len(last)
        for (tmp, testname), args in sorted(last.items()):
            key = script[: -len(".script")]
            if len(last) > 1 or testname != key.rsplit(".", 1)[0]:
                key = f"{key}.{testname}"
            yield key, args, shadowed


def run_unit(binary, argv, cwd, environment, unit_dir, dest, collect=()):
    """Run one replay and write its normalized record into unit_dir.

    Files named in collect are moved into unit_dir after the run, so a unit's
    record is always a flat directory of files.
    """
    os.makedirs(unit_dir, exist_ok=True)
    # Test-only budgets come from the RUN line alone, never from the caller.
    env = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith(TEST_ONLY_ENV_PREFIX)
    }
    env.update(environment)
    started = time.monotonic()
    result = subprocess.run(
        [binary] + argv, cwd=cwd, env=env, capture_output=True, text=True,
        errors="replace",
    )
    elapsed = time.monotonic() - started
    for path in collect:
        if os.path.exists(path):
            shutil.move(path, os.path.join(unit_dir, os.path.basename(path)))
    with open(os.path.join(unit_dir, "exit"), "w", encoding="utf-8") as f:
        f.write(f"{result.returncode}\n")
    with open(os.path.join(unit_dir, "command"), "w", encoding="utf-8") as f:
        # The binary path differs between replays by construction.
        f.write(normalize(shlex.join(["clang-refold"] + argv), dest) + "\n")
        f.write(f"cwd={cwd}\n")
        for name, value in sorted(environment.items()):
            f.write(f"env {name}={value}\n")
    with open(os.path.join(unit_dir, "log"), "w", encoding="utf-8") as f:
        f.write(normalize(result.stdout + result.stderr, dest))
    for name in os.listdir(unit_dir):
        path = os.path.join(unit_dir, name)
        if name in ("exit", "command", "log") or os.path.isdir(path):
            continue
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        # Emitted files are compared byte for byte; only the replay directory's
        # own path, which differs between replays by construction, is replaced.
        normalized = text.replace(dest, REPLAY_DEST_TOKEN)
        if normalized != text:
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(normalized)
    return result.returncode, elapsed


def fresh_stage(stage):
    """Empty the fixed staging directory for the next unit."""
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    return stage


def prepare_destination(out, binary):
    """Create the replay directory and pin a private copy of the binary."""
    if os.path.exists(out) and os.listdir(out):
        sys.exit(f"error: {out} is not empty; replay into a fresh directory")
    os.makedirs(out, exist_ok=True)
    pinned = os.path.join(out, "clang-refold")
    shutil.copy2(binary, pinned)
    return os.path.abspath(out), pinned


def produced_paths(argv):
    """Return the files a clang-refold refold command writes."""
    paths = []
    for index, word in enumerate(argv):
        if word in ("--out", "-o", "--emit-edit-map") and index + 1 < len(argv):
            paths.append(argv[index + 1])
        elif word.startswith("--emit-edit-map="):
            paths.append(word.split("=", 1)[1])
    return paths


def replay_direct(binary, dest, key, invocation, lit_output):
    """Replay a test that runs clang-refold itself, in place.

    Such tests stage sources and headers in a scratch directory and write the
    output beside them.  The output's directory is part of the include-lookup
    surface and the map names staged headers by absolute path, so a copy of the
    directory would change physical file identity and with it the proof
    outcome.  The command therefore runs exactly where lit ran it: lit's own
    output files are moved aside first, the replay's results are moved into
    the unit record, and lit's files are put back.  Without a `cd` the command
    runs where lit runs it, the Output directory.
    """
    _, cwd, environment, argv = invocation
    unit_dir = os.path.join(dest, "lit", key)
    backup_dir = os.path.join(unit_dir, ".lit-originals")
    os.makedirs(backup_dir)
    produced = produced_paths(argv)
    backups = []
    for index, path in enumerate(produced):
        if os.path.exists(path):
            backup = os.path.join(backup_dir, str(index))
            shutil.move(path, backup)
            backups.append((backup, path))
    try:
        return run_unit(
            binary, argv, cwd or lit_output, environment, unit_dir, dest, produced
        )
    finally:
        for backup, path in backups:
            shutil.move(backup, path)
        shutil.rmtree(backup_dir)


def replay_lit(args):
    dest, binary = prepare_destination(args.out, args.binary)
    units = list(lit_units(args.lit_output))
    if args.only:
        wanted = set(args.only)
        units = [u for u in units if u[0].split(".c", 1)[0] in wanted or u[0] in wanted]
    skipped = []
    shadowed_tests = 0
    total = 0.0
    with open(os.path.join(dest, "units.txt"), "w", encoding="utf-8") as index:
        for key, tester, shadowed in units:
            if isinstance(tester, tuple):
                status, elapsed = replay_direct(
                    binary, dest, key, tester, args.lit_output
                )
                total += elapsed
                index.write(f"{key}\texit={status}\n")
                if args.verbose:
                    print(f"{key}: exit={status} {elapsed:.2f}s", flush=True)
                continue
            outputs = os.path.join(tester.tmp, "outputs")
            pp = os.path.join(outputs, f"{tester.testname}.c.i")
            refold_map = os.path.join(outputs, f"{tester.testname}.c.refold.json")
            pp_mod = os.path.join(expected_base(tester), f"{tester.testname}.c.i.mod")
            missing = [p for p in (pp, refold_map, pp_mod) if not os.path.exists(p)]
            if missing:
                skipped.append((key, missing[0]))
                continue
            shadowed_tests += bool(shadowed)
            unit_dir = os.path.join(dest, "lit", key)
            argv = []
            if not tester.with_lines:
                argv.append("--no-lines")
            if not tester.relaxed:
                argv.append("--strict")
            stage = fresh_stage(args.stage)
            produced = [os.path.join(stage, f"{tester.testname}.c.mod")]
            argv += [
                f"--verify-output={tester.verify_output}",
                f"--log-level={tester.log}",
                "--pp", pp,
                "--pp-mod", pp_mod,
                "--refold-map", refold_map,
                "--out", produced[0],
            ]
            if tester.emit_edit_map:
                produced.append(
                    os.path.join(stage, f"{tester.testname}.editmap.json")
                )
                argv.append("--emit-edit-map=" + produced[1])
            status, elapsed = run_unit(
                binary, argv, os.path.dirname(tester.src), tester.environment,
                unit_dir, dest, produced,
            )
            total += elapsed
            index.write(f"{key}\texit={status}\n")
            if args.verbose:
                print(f"{key}: exit={status} {elapsed:.2f}s", flush=True)
    print(
        f"replayed {len(units) - len(skipped)} lit unit(s) in {total:.1f}s; "
        f"{len(skipped)} skipped for missing artifacts; "
        f"{shadowed_tests} unit(s) are the last of several RUN variants"
    )
    for key, path in skipped:
        print(f"  skipped {key}: missing {path}")
    return 0


def corpus_units(corpus_root):
    """Yield (key, map path) for every replayable corpus unit."""
    for directory, _, names in sorted(os.walk(corpus_root)):
        if "c_17_refold" not in directory:
            continue
        for name in sorted(names):
            if not name.endswith(".nolines.refoldmap.json"):
                continue
            path = os.path.join(directory, name)
            key = os.path.relpath(path, corpus_root)
            key = key[: -len(".nolines.refoldmap.json")].replace(os.sep, "__")
            yield key, path


def replay_corpus(args):
    dest, binary = prepare_destination(args.out, args.binary)
    count = 0
    total = 0.0
    with open(os.path.join(dest, "units.txt"), "w", encoding="utf-8") as index:
        for key, refold_map in corpus_units(args.corpus_root):
            if args.only and not any(part in key for part in args.only):
                continue
            base = refold_map[: -len(".nolines.refoldmap.json")]
            pp_mod = base + ".nolines.i"
            pp = base + ".nolines.unmodified.i"
            if not (os.path.exists(pp_mod) and os.path.exists(pp)):
                continue
            directory = os.path.dirname(base)
            stage = directory
            while (
                os.path.basename(stage) != "c_17_refold_preprocessor"
                and stage != "/"
            ):
                stage = os.path.dirname(stage)
            unit_dir = os.path.join(dest, "corpus", key)
            produced = os.path.join(fresh_stage(args.stage), "out.c")
            argv = [
                f"--log-level={args.log_level}", "--verify-output=fatal",
                "-P", pp_mod, "-p", pp, "-r", refold_map,
                "-o", produced,
                f"--verify-include-dir={directory}",
                f"--verify-include-dir={stage}",
            ]
            status, elapsed = run_unit(
                binary, argv, directory, {}, unit_dir, dest, [produced]
            )
            total += elapsed
            count += 1
            index.write(f"{key}\texit={status}\n")
            print(f"{key}: exit={status} {elapsed:.1f}s", flush=True)
    print(f"replayed {count} corpus unit(s) in {total:.1f}s")
    return 0


def read_units(root):
    units = {}
    for kind in ("lit", "corpus"):
        base = os.path.join(root, kind)
        if not os.path.isdir(base):
            continue
        for key in sorted(os.listdir(base)):
            units[f"{kind}/{key}"] = os.path.join(base, key)
    return units


def apply_renames(text, renames):
    for old, new in renames:
        text = text.replace(old, new)
    return text


def first_difference(a, b):
    for number, (left, right) in enumerate(zip(a.splitlines(), b.splitlines()), 1):
        if left != right:
            return number, left, right
    return None


def diff(args):
    renames = [tuple(r.split("=", 1)) for r in args.rename]
    baseline = read_units(args.baseline)
    candidate = read_units(args.candidate)
    problems = 0
    for key in sorted(set(baseline) ^ set(candidate)):
        side = "baseline" if key in baseline else "candidate"
        print(f"ONLY-IN-{side.upper()} {key}")
        problems += 1
    counts = {"same": 0, "output": 0, "exit": 0, "log": 0, "command": 0}
    for key in sorted(set(baseline) & set(candidate)):
        left_dir, right_dir = baseline[key], candidate[key]
        names = sorted(set(os.listdir(left_dir)) | set(os.listdir(right_dir)))
        differing = []
        for name in names:
            left_path = os.path.join(left_dir, name)
            right_path = os.path.join(right_dir, name)
            if not (os.path.exists(left_path) and os.path.exists(right_path)):
                differing.append((name, "present on one side only"))
                continue
            with open(left_path, encoding="utf-8", errors="surrogateescape") as f:
                left = apply_renames(f.read(), renames)
            with open(right_path, encoding="utf-8", errors="surrogateescape") as f:
                right = f.read()
            if left != right:
                where = first_difference(left, right)
                detail = (
                    f"line {where[0]}:\n"
                    f"      - {where[1][:200]}\n"
                    f"      + {where[2][:200]}"
                    if where
                    else "length differs"
                )
                differing.append((name, detail))
        if not differing:
            counts["same"] += 1
            continue
        problems += 1
        for name, _ in differing:
            kind = name if name in ("exit", "log", "command") else "output"
            counts[kind] += 1
        print(f"DIFF {key}")
        for name, detail in differing:
            print(f"    {name}: {detail}")
    print(
        f"{counts['same']} identical unit(s); differences: "
        f"{counts['output']} output file(s), {counts['exit']} exit status(es), "
        f"{counts['log']} log(s), {counts['command']} command(s); "
        f"{problems} unit(s) not identical"
    )
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(
        description="Replay clang-refold binaries and diff their decisions."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    lit = commands.add_parser("replay-lit", help="replay lit test artifacts")
    lit.add_argument("--binary", required=True)
    lit.add_argument("--out", required=True)
    lit.add_argument("--lit-output", default=DEFAULT_LIT_OUTPUT)
    lit.add_argument("--only", nargs="*", help="test names to replay")
    lit.add_argument("--verbose", action="store_true")
    lit.add_argument("--stage", default=DEFAULT_STAGE, help=argparse.SUPPRESS)
    lit.set_defaults(handler=replay_lit)

    corpus = commands.add_parser("replay-corpus", help="replay corpus units")
    corpus.add_argument("--binary", required=True)
    corpus.add_argument("--out", required=True)
    corpus.add_argument(
        "--corpus-root", default=f"/tmp/pytest-of-{getpass.getuser()}"
    )
    corpus.add_argument("--log-level", default="warn")
    corpus.add_argument("--stage", default=DEFAULT_STAGE, help=argparse.SUPPRESS)
    corpus.add_argument(
        "--only", nargs="*", help="replay units whose key contains any of these"
    )
    corpus.set_defaults(handler=replay_corpus)

    compare = commands.add_parser("diff", help="compare two replay directories")
    compare.add_argument("baseline")
    compare.add_argument("candidate")
    compare.add_argument(
        "--rename",
        action="append",
        default=[],
        metavar="OLD=NEW",
        help="a declared rename, applied to the baseline before comparing",
    )
    compare.set_defaults(handler=diff)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
