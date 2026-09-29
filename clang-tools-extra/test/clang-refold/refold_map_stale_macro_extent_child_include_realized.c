// Regression: a map whose `#undef` extent disagrees with its header must not
// supply the bytes of a directive preserved from a consumed child include.
// The header edit below replaces the tokens around `child_undef.h`, and the
// child's `#undef FOO` is kept by re-emitting it, as it was written in the
// child header, inside the outer header's replacement.
//
// Maps written before the extent came from Clang's lexer end a directive with a
// newline-crossing trailing comment at the first newline, inside the comment.
// The replacement then held `#undef FOO /* ...` with the comment unterminated,
// which swallowed ` after = 20;`.  The spelling is now taken only from an
// extent the child's structure index bound.  Without one the preservation
// declines, and the outer header is realized from B instead.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/t.c
// RUN: cp %S/headers/stale_macro_directive_extent/child_outer.h \
// RUN:   %S/headers/stale_macro_directive_extent/child_undef.h %t.dir/headers/
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=map.json t.c -o a.i
// RUN: sed -e '/^int before = 1;$/{N;N;s/.*/int before = 10, after = 20;/;}' a.i > b.i
//
// The exact extent re-emits the whole directive.
// RUN: clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map map.json --out ok.c
// RUN: FileCheck --check-prefix=OK --input-file=ok.c %s
// OK:      {{^}}int before = 10,
// OK-NEXT: {{^}}#undef FOO /* the undef, whose note
// OK-NEXT: {{^}}   spans two lines */
// OK-NEXT: {{^}} after = 20;
//
// The stale extent ends where the old producer ended it.
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   d = next(i for i in m['items'] if i.get('subkind') == '#undef' and i['name'] == 'FOO'); \
// RUN:   assert d['site_e'] < d['directive_line_e']; d.update(directive_line_e=d['site_e']); \
// RUN:   json.dump(m, open(sys.argv[2], 'w'))" map.json stale.json
// RUN: clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map stale.json --out stale.c
// RUN: clang-refold --check stale.c --pp-mod b.i --refold-map map.json
// RUN: FileCheck --check-prefix=STALE --input-file=stale.c %s
// STALE-NOT: {{^}}#undef FOO
// STALE:     {{^}}int before = 10, after = 20;
// STALE-NOT: {{^}}#undef FOO
#include "child_outer.h"
