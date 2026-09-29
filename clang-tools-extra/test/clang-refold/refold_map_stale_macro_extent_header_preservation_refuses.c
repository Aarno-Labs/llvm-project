// Regression: a map whose `#define` extent disagrees with its header must not
// supply the bytes of a preserved header directive.  Deleting everything the
// include contributed consumes the header's `#define`, which the surviving use
// below still needs, so the repair re-emits the definition in the translation
// unit as it was written in the header, sliced by its recorded extent.  The
// consumed include is authorized as a whole, so nothing compared that extent
// with the header.
//
// Maps written before the extent came from Clang's lexer end a definition with
// a newline-crossing trailing comment at the first newline, inside the
// comment.  The repair then emitted `#define STATE_CARRIED 1 /* ...` with the
// comment unterminated, which swallowed the use.  A directive's bytes are now
// re-emitted only from an extent the header's structure index bound, and the
// queued preservation fails closed without one.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/t.c
// RUN: cp %S/headers/stale_macro_directive_extent/preserved_define.h %t.dir/headers/
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=map.json t.c -o a.i
// RUN: sed -e '/int state_define_decl = 1;/d' a.i > b.i
//
// The exact extent re-emits the whole definition.
// RUN: clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map map.json --out ok.c
// RUN: FileCheck --check-prefix=OK --input-file=ok.c %s
// OK:      {{^}}#define STATE_CARRIED 1 /* the carried value, whose note
// OK-NEXT: {{^}}   spans two lines */
// OK-NEXT: {{^}}int state_define_use = STATE_CARRIED;
//
// The stale extent ends where the old producer ended it.
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   d = next(i for i in m['items'] if i.get('subkind') == '#define' and i['name'] == 'STATE_CARRIED'); \
// RUN:   assert d['site_e'] < d['directive_line_e']; d.update(directive_line_e=d['site_e']); \
// RUN:   json.dump(m, open(sys.argv[2], 'w'))" map.json stale.json
// RUN: not clang-refold --strict --verify-output=off --pp a.i --pp-mod b.i \
// RUN:   --refold-map stale.json --out stale.c 2>&1 \
// RUN:   | FileCheck --check-prefix=STALE %s
// RUN: not test -e stale.c
// STALE: reason=MissingProducerFacts stage=macro-state-preservation
// STALE: no admissible refold
#include "preserved_define.h"
int state_define_use = STATE_CARRIED;
