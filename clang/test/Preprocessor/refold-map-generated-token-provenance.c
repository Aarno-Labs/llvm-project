// Record token-location provenance for macro calls formed during another
// expansion: the substitution history of the callee name (callee_subst), and
// the dependence of each actual on the formals of the expansion that held the
// argument list (arg_token_deps).
//
// Neither reads the caller's callsite text, so both exist for calls whose
// callers are themselves generated, and both name the invocation that
// substituted or spelled the tokens, which need not be the immediate caller.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/main.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// CHECK: "version": "3.14",

// A callee formed while another macro prescans its actual: `F(a1)` expands
// while EVAL prescans `m(v)`, so DISPATCH, not the immediate caller EVAL,
// substituted the name `F` and its actual.  The name came from the file, so the
// history has no replacement-list element.  EVAL's text arg_deps take precedence
// over token-derived ones.

// CHECK:      "text": "#define DISPATCH(
// CHECK:      "id": [[DISPATCH:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "DISPATCH",
// CHECK:      "id": [[EVAL:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "EVAL",
// CHECK-NOT:  "callee_subst"
// CHECK-NOT:  "arg_token_deps"
// CHECK:      "spans": [
// CHECK:      "id": [[F:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "F",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[DISPATCH]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[DISPATCH]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// A callee name passed through generated callers.  FWD's name is APPLY's
// `f`, spelled in CALL's replacement list; PASS's name went through FWD's `g`,
// APPLY's `g` and CALL's `g`.  APPLY is spelled in CALL's replacement list, so
// it carries text arg_deps instead.

// CHECK:      "text": "#define CALL(
// CHECK:      "id": [[CALL:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "CALL",
// CHECK:      "id": [[APPLY:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "APPLY",
// CHECK-NOT:  "callee_subst"
// CHECK-NOT:  "arg_token_deps"
// CHECK:      "spans": [
// CHECK:      "id": [[FWD:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "FWD",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[APPLY]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[CALL]]
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[APPLY]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [
// CHECK:      "id": [[PASS:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "PASS",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[FWD]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[APPLY]],
// CHECK-NEXT:     "param": 1
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[CALL]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[FWD]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// An empty substitution supplies no token but still counts: `a` is empty
// here, and G1's actual depends on both `a` and `b`.

// CHECK:      "text": "#define CAT2(
// CHECK:      "id": [[CAT2:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "CAT2",
// CHECK:      "id": [[G11:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "G1",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[CAT2]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[CAT2]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// A top-level reference before an actual can move its boundaries only for a
// variadic callee: a fixed-arity call would be rejected instead.

// CHECK:      "text": "#define PAIR(
// CHECK:      "id": [[PAIR1:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "PAIR",
// CHECK:      "id": [[G21:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "G2",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[PAIR1]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[PAIR1]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [
// CHECK:      "id": [[PAIR2:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "PAIR",
// CHECK:      "id": [[NAME1:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "NAME",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[PAIR2]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[PAIR2]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// A `__VA_OPT__` group or a `, ## __VA_ARGS__` comma whose `,` would end
// `name` leaves `name` independent of the variadic formal, switched on or off,
// unless material in the actual follows it, as `tail` does in MID.

// CHECK:      "text": "#define WG(
// CHECK:      "id": [[FW1:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "FW",
// CHECK:      "id": [[NAME2:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "NAME",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[FW1]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[FW1]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [
// CHECK:      "id": [[FW2:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "FW",
// CHECK:      "id": [[NAME3:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "NAME",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[FW2]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[FW2]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [
// CHECK:      "id": [[MID1:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "MID",
// CHECK:      "id": [[NAME4:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "NAME",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[MID1]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[MID1]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [
// CHECK:      "id": [[MID2:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "MID",
// CHECK:      "id": [[NAME5:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "NAME",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[MID2]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[MID2]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [
// CHECK:      "id": [[WG:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "WG",
// CHECK:      "id": [[NAME6:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "NAME",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[WG]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[WG]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1,
// CHECK-NEXT:       2
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// A reference between the name and `(` substituted nothing, or the call
// would not have formed; it counts for every actual.

// CHECK:      "text": "#define FORM(
// CHECK:      "id": [[FORM:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "FORM",
// CHECK:      "id": [[G12:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "G1",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[FORM]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[FORM]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// A `,` from a substitution ends an actual; that formal counts for both
// actuals it borders.

// CHECK:      "text": "#define SPLIT(
// CHECK:      "id": [[SPLIT:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "SPLIT",
// CHECK:      "id": [[COMMA:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "obj",
// CHECK-NEXT: "name": "COMMA",
// CHECK:      "id": [[G22:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "G2",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[SPLIT]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_token_deps": {
// CHECK-NEXT:   "macro_id": [[SPLIT]],
// CHECK-NEXT:   "formals": [
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ],
// CHECK-NEXT:     [
// CHECK-NEXT:       1
// CHECK-NEXT:     ]
// CHECK-NEXT:   ]
// CHECK-NEXT: },
// CHECK:      "spans": [

// The argument list continues after SUF's callsite, in the file, so its
// dependence is withheld; the name's history does not need the argument list.

// CHECK:      "text": "#define SUF(
// CHECK:      "id": [[SUF:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "SUF",
// CHECK:      "id": [[G13:[0-9]+]],
// CHECK-NEXT: "kind": "macro",
// CHECK-NEXT: "subkind": "func",
// CHECK-NEXT: "name": "G1",
// CHECK:      "callee_subst": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "macro_id": [[SUF]],
// CHECK-NEXT:     "param": 0
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK-NOT:  "arg_token_deps"
// CHECK:      "spans": [

//--- main.c
#define F(v) v
#define EVAL(x) x
#define DISPATCH(m, v) int EVAL(m(v)); int m;
DISPATCH(F, a1)
#define PASS(v) v
#define FWD(g, a) g(a)
#define APPLY(f, g, a) f(g, a)
#define CALL(g, a) int APPLY(FWD, g, a); int g;
CALL(PASS, a2)
#define G1(x) x
#define CAT2(f, a, b) f(a b)
CAT2(G1, , 3)
#define NAME(name, ...) name
#define G2(a, b) a b
#define PAIR(f, a, b) f(a, b)
PAIR(G2, 4, 5) PAIR(NAME, 4, 5)
#define FW(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__))
#define MID(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__) tail)
#define WG(G, X, ...) G(X, ## __VA_ARGS__)
FW(NAME, 6) FW(NAME, 6, 7) MID(NAME, 8) MID(NAME, 8, 9) WG(NAME, 10)
#define FORM(f, e) f e (11)
FORM(G1, )
#define COMMA ,
#define SPLIT(f, c) f(12 c 13)
SPLIT(G2, COMMA)
#define SUF(f, a) int a; int f(a
SUF(G1, a14));
