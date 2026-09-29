// Regression: a map whose `#define` extent disagrees with the file must not
// supply the bytes of a restore.  The restore after a synthetic `#undef`
// re-emits the definition as it was written, sliced by its recorded extent.
// Maps written before the extent came from Clang's lexer end a definition with
// a newline-crossing trailing comment at the first newline, inside the
// comment, and they still pass the schema and the model.  The restore then
// emitted `#define ZZ VALUE /* the value, whose note` with the comment
// unterminated, which swallowed `int later = ZZ;`.
//
// The structure index binds a recorded extent only when it equals its own
// lexical scan of the file, and a directive's bytes are now re-emitted only
// from a bound extent.  Here the restore declines, and the final audit refuses
// the payload that still reads the live definition.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: cp %s %t.dir/t.c
// RUN: cd %t.dir && clang -E -P --refold-map=map.json t.c -o a.i
// RUN: sed -e 's/{ 1, 2 }/{ ZZ }/' a.i > b.i
//
// The exact extent restores the whole definition.
// RUN: clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map map.json --out ok.c
// RUN: FileCheck --check-prefix=OK --input-file=ok.c %s
// OK:      {{^}}#undef ZZ
// OK-NEXT: {{^}}int arr[] = { ZZ };
// OK-NEXT: {{^}}#define ZZ VALUE /* the value, whose note
// OK-NEXT: {{^}}   spans two lines */
// OK-NEXT: {{^}}int later = ZZ;
//
// The stale extent ends where the old producer ended it.
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   d = next(i for i in m['items'] if i.get('subkind') == '#define' and i['name'] == 'ZZ'); \
// RUN:   assert d['site_e'] < d['directive_line_e']; d.update(directive_line_e=d['site_e']); \
// RUN:   json.dump(m, open(sys.argv[2], 'w'))" map.json stale.json
// RUN: not clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map stale.json --out stale.c 2>&1 \
// RUN:   | FileCheck --check-prefix=STALE %s
// RUN: not test -e stale.c
// STALE: stage=macro/liveness
// STALE: no admissible refold
#define VALUE 3
#define ZZ VALUE /* the value, whose note
   spans two lines */
int before = ZZ;
int arr[] = { 1, 2 };
int later = ZZ;
