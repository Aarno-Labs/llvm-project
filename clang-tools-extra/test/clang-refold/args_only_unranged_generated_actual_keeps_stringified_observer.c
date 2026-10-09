// RUN: %clang-refold-tester-verify-off args_only_unranged_generated_actual_keeps_stringified_observer
// An args-only rewrite is refused when an unchanged token comes from a
// generated call whose argument the producer could not delimit.
//
// `CALL PAIR` takes its name from WRAP's replacement list and its argument
// list from WRAP's actual, so no one piece of text spells the call and its
// argument range is unknown.  The producer used to record `arg_deps: [[]]`
// there anyway, which told the descendant gate that CALL's `"x"` ignores PAIR.
// B renames `int x` and the root's own `#PAIR`; rewriting PAIR to `(y)` would
// also turn the unchanged `"x"` into `"y"`.  The fold fails closed and the
// invocation is expanded.  This used to emit `WRAP((y))` at verify-off.
//
// A map that still carries the claim is refused too: the consumer drops
// arg_deps when an argument has no range.
// RUN: rm -rf %t.dir && mkdir -p %t.dir && cp %s %t.dir/t.c
// RUN: cd %t.dir && clang -E -P --refold-map=map.json t.c -o a.i
// RUN: sed -e 's/int x; char \*t = "(x)";/int y; char *t = "(y)";/' a.i > b.i
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   c = next(i for i in m['items'] if i.get('kind') == 'macro' and i['name'] == 'CALL'); \
// RUN:   assert 'arg_deps' not in c and c['inv_arg_ranges'] == [{'b': None, 'e': None}]; \
// RUN:   c.update(arg_deps=[[]], arg_refs=[[]]); json.dump(m, open(sys.argv[2], 'w'))" map.json old.json
// RUN: clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map old.json --out old.c
// RUN: FileCheck --check-prefix=OLD --input-file=old.c %s
// OLD: {{^}}char *s = "x"; int y; char *t = "(y)";
#define CALL(X) char *s = #X; int X;
#define WRAP(PAIR) CALL PAIR char *t = #PAIR;
WRAP((x))
