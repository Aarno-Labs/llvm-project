// Record the HeaderSearch state a consumer needs before it lets a different
// file, or the same file reached another way, stand in for an included header.
//
// `once_only` is written on every include that opened a file: include-once
// state belongs to one physical file, and Clang tests it before the
// controlling macro.  A skipped include carries
// `skip_controlling_macro_defined`, which says whether the macro was defined
// when Clang skipped it; an include-once header can be skipped with it
// undefined.  An entered include carries `observes_lookup_context`, which says
// whether reading the file searched for another file outside an include edge,
// including through a macro that hides the operator, and through
// `__has_embed`, which Clang reports by a callback of its own.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P -I%t --refold-map=%t/map.json %t/tu.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// CHECK:      "version": "3.11",

// CHECK:      "text": "#include \"plain.h\"\n",
// CHECK:      "controlling_macro": "PLAIN_H",
// CHECK-NEXT: "once_only": false,
// CHECK-NEXT: "observes_lookup_context": false,

// Skipped by its guard.

// CHECK:      "text": "#include \"plain.h\"\n",
// CHECK:      "controlling_macro": "PLAIN_H",
// CHECK-NEXT: "once_only": false,
// CHECK-NEXT: "skip_controlling_macro_defined": true,

// `#pragma once` inside the guard keeps the controlling macro.

// CHECK:      "text": "#include \"once.h\"\n",
// CHECK:      "controlling_macro": "ONCE_H",
// CHECK-NEXT: "once_only": true,
// CHECK-NEXT: "observes_lookup_context": false,

// Skipped as include-once after its macro was undefined.

// CHECK:      "text": "#include \"once.h\"\n",
// CHECK:      "controlling_macro": "ONCE_H",
// CHECK-NEXT: "once_only": true,
// CHECK-NEXT: "skip_controlling_macro_defined": false,

// CHECK:      "text": "#include \"probe.h\"\n",
// CHECK:      "controlling_macro": "PROBE_H",
// CHECK-NEXT: "once_only": false,
// CHECK-NEXT: "observes_lookup_context": true,

// CHECK:      "text": "#include \"embed.h\"\n",
// CHECK:      "controlling_macro": "EMBED_H",
// CHECK-NEXT: "once_only": false,
// CHECK-NEXT: "observes_lookup_context": true,

//--- tu.c
#define PROBE(name) __has_include(name)
#include "plain.h"
#include "plain.h"
#include "once.h"
#undef ONCE_H
#include "once.h"
#include "probe.h"
#include "embed.h"

//--- plain.h
#ifndef PLAIN_H
#define PLAIN_H
int plain;
#endif

//--- once.h
#ifndef ONCE_H
#define ONCE_H
#pragma once
int once;
#endif

//--- probe.h
#ifndef PROBE_H
#define PROBE_H
#if PROBE("plain.h")
int probe;
#endif
#endif

//--- embed.h
#ifndef EMBED_H
#define EMBED_H
#if __has_embed("data.txt")
int embed;
#endif
#endif

//--- data.txt
x
