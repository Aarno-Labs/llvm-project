#!/usr/bin/env python3
#
# ===- check_refold_layers.py - clang-refold layer checker --*- python -*--===#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ===-----------------------------------------------------------------------===#
#
# Enforce the clang-refold layer architecture described in
# docs/Architecture.md.
#
# Every C/C++ file under the tool root is assigned exactly one layer by the
# manifest (docs/layers.txt).  A file may include files in its own layer or in
# any lower layer.  An include that points upward is an error; there is no
# exception list.
#
# The checker also rejects file-level include cycles, files missing from the
# manifest, manifest entries for files that no longer exist, and a .h/.cpp
# pair assigned to different layers.
#
# With --metrics it prints the structural census used by the organization
# roadmap instead of (in addition to) enforcing the rules.
#
# ===-----------------------------------------------------------------------===#

import argparse
import collections
import os
import re
import sys

LAYERS = [
    "support",
    "model",
    "carriers",
    "analysis",
    "proof",
    "planning",
    "emission",
    "core",
    "tool",
]
RANK = {name: index for index, name in enumerate(LAYERS)}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
SOURCE_SUFFIXES = (".h", ".cpp")


def read_sources(root):
    """Return {relative path: text} for every C/C++ file under root."""
    sources = {}
    for directory, _, names in os.walk(root):
        for name in names:
            if name.endswith(SOURCE_SUFFIXES):
                path = os.path.join(directory, name)
                with open(path, encoding="utf-8", errors="replace") as f:
                    sources[os.path.relpath(path, root)] = f.read()
    return sources


def include_graph(sources):
    """Return {file: sorted local includes}.

    Local includes are spelled relative to the tool root ("dir/File.h"); an
    include naming no file in the tree (llvm/, clang/, system) is external.
    A spelling relative to the including file's directory is also accepted.
    """
    graph = {}
    for path, text in sources.items():
        targets = set()
        for spelling in INCLUDE_RE.findall(text):
            candidates = (
                spelling,
                os.path.normpath(os.path.join(os.path.dirname(path), spelling)),
            )
            for candidate in candidates:
                if candidate in sources:
                    targets.add(candidate)
                    break
        graph[path] = sorted(targets)
    return graph


def strongly_connected_components(nodes, edges):
    """Iterative Tarjan; returns components in a deterministic order."""
    index = {}
    low = {}
    on_stack = set()
    stack = []
    components = []
    counter = 0
    for root in sorted(nodes):
        if root in index:
            continue
        work = [(root, iter(sorted(edges.get(root, ()))))]
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            node, successors = work[-1]
            advanced = False
            for succ in successors:
                if succ not in index:
                    index[succ] = low[succ] = counter
                    counter += 1
                    stack.append(succ)
                    on_stack.add(succ)
                    work.append((succ, iter(sorted(edges.get(succ, ())))))
                    advanced = True
                    break
                if succ in on_stack:
                    low[node] = min(low[node], index[succ])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
            if low[node] == index[node]:
                component = []
                while True:
                    member = stack.pop()
                    on_stack.discard(member)
                    component.append(member)
                    if member == node:
                        break
                components.append(sorted(component))
    return components


def parse_manifest(path, errors):
    """Return {file: layer} from a manifest of '<path> <layer> [# note]'."""
    layers = {}
    with open(path, encoding="utf-8") as f:
        for number, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) != 2:
                errors.append(f"{path}:{number}: expected '<path> <layer>'")
                continue
            file, layer = fields
            if layer not in RANK:
                errors.append(f"{path}:{number}: unknown layer '{layer}'")
            elif file in layers:
                errors.append(f"{path}:{number}: duplicate entry for {file}")
            else:
                layers[file] = layer
    return layers


def upward_edges(graph, layers):
    """Return every include edge whose target sits in a higher layer."""
    result = []
    for source, targets in sorted(graph.items()):
        if source not in layers:
            continue
        for target in targets:
            if target in layers and RANK[layers[target]] > RANK[layers[source]]:
                result.append((source, target))
    return result


def check(sources, graph, layers, errors):
    """Append every architecture violation to errors."""
    for path in sorted(set(sources) - set(layers)):
        errors.append(f"{path}: not assigned a layer in the manifest")
    for path in sorted(set(layers) - set(sources)):
        errors.append(f"{path}: manifest entry names a file that does not exist")

    for path in sorted(layers):
        if not path.endswith(".cpp"):
            continue
        header = path[: -len(".cpp")] + ".h"
        if header in layers and layers[header] != layers[path]:
            errors.append(
                f"{path}: layer '{layers[path]}' differs from its header "
                f"{header} ('{layers[header]}')"
            )

    for component in strongly_connected_components(sources, graph):
        if len(component) > 1:
            errors.append("file-level include cycle among: " + ", ".join(component))

    for source, target in upward_edges(graph, layers):
        errors.append(
            f"{source} ({layers[source]}) includes {target} ({layers[target]}): "
            "upward layer dependency"
        )


def directory_of(path):
    return path.split("/", 1)[0] if "/" in path else "(root)"


def print_metrics(sources, graph, layers):
    """Print the structural census tracked by the organization roadmap."""
    line_counts = {path: text.count("\n") for path, text in sources.items()}
    fan_in = collections.Counter(t for targets in graph.values() for t in targets)
    live = upward_edges(graph, layers)
    directory_edges = collections.defaultdict(set)
    for source, targets in graph.items():
        for target in targets:
            if directory_of(source) != directory_of(target):
                directory_edges[directory_of(source)].add(directory_of(target))
    directories = {directory_of(path) for path in sources}
    directory_sccs = [
        c
        for c in strongly_connected_components(directories, directory_edges)
        if len(c) > 1
    ]
    file_cycles = [
        c for c in strongly_connected_components(sources, graph) if len(c) > 1
    ]
    headers = {p: t for p, t in sources.items() if p.endswith(".h")}

    def count_pattern(pattern, texts):
        regex = re.compile(pattern, re.M)
        return sum(len(regex.findall(text)) for text in texts)

    engine = sources.get("core/RefoldEngine.h", "")
    rows = [
        ("C/C++ files", len(sources)),
        ("C/C++ lines", sum(line_counts.values())),
        ("local include edges", sum(len(t) for t in graph.values())),
        ("file-level include cycles", len(file_cycles)),
        (
            "directory-level SCCs (size > 1)",
            "; ".join(",".join(c) for c in directory_sccs) or 0,
        ),
        ("upward layer edges", len(live)),
        ("files >= 2000 lines", sum(n >= 2000 for n in line_counts.values())),
        ("files >= 3000 lines", sum(n >= 3000 for n in line_counts.values())),
        ("files >= 4000 lines", sum(n >= 4000 for n in line_counts.values())),
        (
            ".cpp > 3500 lines",
            sum(
                n > 3500 for p, n in line_counts.items() if p.endswith(".cpp")
            ),
        ),
        (
            "largest .cpp",
            "%s (%d)"
            % max(
                ((p, n) for p, n in line_counts.items() if p.endswith(".cpp")),
                key=lambda item: (item[1], item[0]),
            ),
        ),
        (
            "largest .h",
            "%s (%d)"
            % max(
                ((p, n) for p, n in line_counts.items() if p.endswith(".h")),
                key=lambda item: (item[1], item[0]),
            ),
        ),
        (
            "feature includers of proof/RefoldProofServices.h",
            sum(
                "proof/RefoldProofServices.h" in targets
                and layers.get(path) not in ("core", "tool")
                and path != "proof/RefoldProofServices.cpp"
                for path, targets in graph.items()
            ),
        ),
        (
            "RefoldEngine::Initialize*() declarations",
            count_pattern(r"^\s*void\s+Initialize[A-Z]\w*\s*\(", [engine]),
        ),
        ("RefoldEngine.h lines", line_counts.get("core/RefoldEngine.h", 0)),
        (
            "service late binding (void Bind*())",
            count_pattern(r"^\s*void\s+Bind[A-Z]\w*\s*\(", headers.values()),
        ),
        (
            "std::function< in headers",
            count_pattern(r"std::function<", headers.values()),
        ),
    ]
    print("| Metric | Value |")
    print("| --- | --- |")
    for name, value in rows:
        print(f"| {name} | {value} |")
    print()
    print("Top direct fan-in:")
    for path, count in sorted(fan_in.items(), key=lambda i: (-i[1], i[0]))[:10]:
        print(f"  {count:4d} {path}")
    print()
    print("Upward edges by layer pair:")
    pairs = collections.Counter((layers[s], layers[t]) for s, t in live)
    for (low_layer, high_layer), count in sorted(
        pairs.items(), key=lambda i: (-i[1], i[0])
    ):
        print(f"  {count:4d} {low_layer} -> {high_layer}")


def main():
    tool_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(
        description="Enforce the clang-refold layer architecture."
    )
    parser.add_argument("--root", default=tool_root, help="clang-refold root")
    parser.add_argument("--manifest", help="default: <root>/docs/layers.txt")
    parser.add_argument(
        "--metrics", action="store_true", help="print the structural census"
    )
    parser.add_argument(
        "--list-upward",
        action="store_true",
        help="print every upward edge, one per line, and exit",
    )
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    manifest = args.manifest or os.path.join(root, "docs", "layers.txt")

    errors = []
    sources = read_sources(root)
    graph = include_graph(sources)
    layers = parse_manifest(manifest, errors)

    if args.list_upward:
        for source, target in upward_edges(graph, layers):
            print(
                f"{source} -> {target}  # {layers[source]} -> {layers[target]}"
            )
        return 0

    if args.metrics:
        print_metrics(sources, graph, layers)
        print()

    check(sources, graph, layers, errors)
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors:
        print(f"{len(errors)} architecture error(s)", file=sys.stderr)
        return 1
    print(
        f"clang-refold layers OK: {len(sources)} files, "
        f"{sum(len(t) for t in graph.values())} local include edges, "
        "no upward edges"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
