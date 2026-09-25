// RUN: %clang-refold-tester mixed_tu_include_closure_preserves_tu_macro_directive_gap
// A mixed TU/include closure keeps a macro definition in its replaced gap at
// the definition's original position.
//
// The edit replaces `1,` and the included `2` with `3`, so the include is given
// up and the replacement spans the `#define` between them.  Consuming the
// definition was token-sound -- nothing observes `GAP_VALUE` -- but it deleted
// meaningful source.  The payload does not name the macro, so it is placed
// before the definition by proof, and the definition survives.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 3
#define GAP_VALUE 99
};
