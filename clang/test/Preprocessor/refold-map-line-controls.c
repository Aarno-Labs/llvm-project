// Record every executed `#line` and GNU line marker with its exact extent.
//
// Each "line_controls" event covers [site_b, site_e): the directive introducer
// through the first byte after the directive.  The presumed location at site_e
// is (logical_file_after, logical_line_after), and from there every physical
// line break advances the line by one until the next event.  "reason" says how
// the presumed file changed and "file_kind" is the characteristic the directive
// established.

// RUN: rm -rf %t && split-file %s %t
// RUN: printf '#line 90' > %t/noeol.h
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/main.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// The predefines buffer's own line markers come first.  They name no file a
// consumer refolds, so they are skipped here.  Then a plain `#line` at [7,28)
// of main.c.

// CHECK:      "line_controls": [
// CHECK:          "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 7,
// CHECK-NEXT:     "site_e": 28,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 10,
// CHECK-NEXT:     "logical_file_after": "renamed.c",
// CHECK-NEXT:     "text": "#line 10 \"renamed.c\"\n",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "user"
// CHECK-NEXT:   },

// A splice before the line number moves Clang's line note onto the backslash,
// so the line after this directive is 21, not 20.  The extent covers both
// physical lines, and the file is kept.

// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 35,
// CHECK-NEXT:     "site_e": 46,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 21,
// CHECK-NEXT:     "logical_file_after": "renamed.c",
// CHECK-NEXT:     "text": "#line \\\n20\n",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "user"
// CHECK-NEXT:   },

// The `#line 999` in the skipped `#ifdef NOPE` arm was never executed, so it
// has no event.  A header's directives carry the include occurrence that lexed
// them.  A directive that ends at the end of its file without a newline ends
// there, and since site_e is then still on the directive's own physical line,
// the presumed line there is 89.

// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}inc.h",
// CHECK-NEXT:     "site_b": 0,
// CHECK-NEXT:     "site_e": 25,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 80,
// CHECK-NEXT:     "logical_file_after": "inc_renamed.h",
// CHECK-NEXT:     "owner_include_id": [[INC:[0-9]+]],
// CHECK-NEXT:     "text": "#line 80 \"inc_renamed.h\"\n",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "user"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}noeol.h",
// CHECK-NEXT:     "site_b": 0,
// CHECK-NEXT:     "site_e": 8,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 89,
// CHECK-NEXT:     "logical_file_after": "{{.*}}noeol.h",
// CHECK-NEXT:     "owner_include_id": [[NOEOL:[0-9]+]],
// CHECK-NEXT:     "text": "#line 90",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "user"
// CHECK-NEXT:   },

// Line markers with flag 1 and 2 enter and exit a presumed file.  FileChanged
// reports them exactly like a real `#include`, so they were not recorded at
// all before this callback existed.

// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 118,
// CHECK-NEXT:     "site_e": 137,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 30,
// CHECK-NEXT:     "logical_file_after": "entered.h",
// CHECK-NEXT:     "text": "# 30 \"entered.h\" 1\n",
// CHECK-NEXT:     "reason": "enter",
// CHECK-NEXT:     "file_kind": "user"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 144,
// CHECK-NEXT:     "site_e": 160,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 40,
// CHECK-NEXT:     "logical_file_after": "main.c",
// CHECK-NEXT:     "text": "# 40 \"main.c\" 2\n",
// CHECK-NEXT:     "reason": "exit",
// CHECK-NEXT:     "file_kind": "user"
// CHECK-NEXT:   },

// Flags 3 and 3 4 establish a system and an extern "C" system file.

// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 167,
// CHECK-NEXT:     "site_e": 182,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 50,
// CHECK-NEXT:     "logical_file_after": "sys.h",
// CHECK-NEXT:     "text": "# 50 \"sys.h\" 3\n",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "system"
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 182,
// CHECK-NEXT:     "site_e": 199,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 60,
// CHECK-NEXT:     "logical_file_after": "sys.h",
// CHECK-NEXT:     "text": "# 60 \"sys.h\" 3 4\n",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "extern_c_system"
// CHECK-NEXT:   },

// A digraph introducer starts the extent at `%:`, and `#line` keeps the file
// characteristic already in effect.

// CHECK-NEXT:   {
// CHECK-NEXT:     "id": {{[0-9]+}},
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "site_b": 199,
// CHECK-NEXT:     "site_e": 209,
// CHECK-NEXT:     "active": true,
// CHECK-NEXT:     "producer_proven": true,
// CHECK-NEXT:     "logical_line_after": 70,
// CHECK-NEXT:     "logical_file_after": "sys.h",
// CHECK-NEXT:     "text": "%:line 70\n",
// CHECK-NEXT:     "reason": "rename",
// CHECK-NEXT:     "file_kind": "extern_c_system"
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// CHECK:      "id": [[INC]],
// CHECK-NEXT: "kind": "directive",
// CHECK-NEXT: "subkind": "#include",
// CHECK-NEXT: "text": "#include \"inc.h\"\n",
// CHECK:      "id": [[NOEOL]],
// CHECK-NEXT: "kind": "directive",
// CHECK-NEXT: "subkind": "#include",
// CHECK-NEXT: "text": "#include \"noeol.h\"\n",

//--- main.c
int a;
#line 10 "renamed.c"
int b;
#line \
20
int c;
#ifdef NOPE
#line 999
#endif
#include "inc.h"
#include "noeol.h"
# 30 "entered.h" 1
int d;
# 40 "main.c" 2
int e;
# 50 "sys.h" 3
# 60 "sys.h" 3 4
%:line 70
int f;
//--- inc.h
#line 80 "inc_renamed.h"
int g;
