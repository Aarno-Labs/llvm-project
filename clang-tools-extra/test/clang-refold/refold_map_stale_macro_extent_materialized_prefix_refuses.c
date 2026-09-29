// Regression: a map whose `#define` extent disagrees with its header must not
// supply the bytes of a definition carried in front of a materialized include.
// Folding the header's initializer consumes the `#define LIMIT` between its
// tokens, and the surviving use below still needs it, so the repair prepends
// the definition, as it was written, to the materialized header body.  The
// include is consumed as a whole, so nothing compared the definition's
// recorded extent with the header.
//
// Maps written before the extent came from Clang's lexer end a definition with
// a newline-crossing trailing comment at the first newline, inside the
// comment.  The prefix was then `#define LIMIT (-1) /* ...` with the comment
// unterminated, which swallowed the materialized body.  A directive's bytes are
// now re-emitted only from an extent the header's structure index bound, and
// the materialized-include repair fails closed without one.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/t.c
// RUN: cp %S/headers/stale_macro_directive_extent/consumed_define.h %t.dir/headers/
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=map.json t.c -o a.i
// RUN: sed -e '/^int value = 1 +$/{N;s/.*/int value = 3;/;}' a.i > b.i
//
// The exact extent carries the whole definition.
// RUN: clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map map.json --out ok.c
// RUN: FileCheck --check-prefix=OK --input-file=ok.c %s
// OK:      {{^}}#define LIMIT (-1) /* the limit, whose note
// OK-NEXT: {{^}}   spans two lines */
// OK-NEXT: {{^}}int value = 3;
// OK-NEXT: {{^}}int result = LIMIT;
//
// The stale extent ends where the old producer ended it.
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   d = next(i for i in m['items'] if i.get('subkind') == '#define' and i['name'] == 'LIMIT'); \
// RUN:   assert d['site_e'] < d['directive_line_e']; d.update(directive_line_e=d['site_e']); \
// RUN:   json.dump(m, open(sys.argv[2], 'w'))" map.json stale.json
// RUN: not clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map stale.json --out stale.c 2>&1 \
// RUN:   | FileCheck --check-prefix=STALE %s
// RUN: not test -e stale.c
// STALE: stage=include/materialized-macro-state
// STALE: no admissible refold
#include "consumed_define.h"
int result = LIMIT;
