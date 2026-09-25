// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_macro_directive_gap_observed_by_payload
// A mixed TU/include closure falls back to consuming a gap's macro definition
// when the definition cannot be kept in place.
//
// The edit writes `GAP_VALUE` where `1,` and the included `2` were.  The
// written identifier names the macro the gap defines, and it has no aligned
// position relative to that definition, so the B payload cannot be split
// around it by proof.  Preserving the definition in place refuses, and the
// closure is proved again with the gap consumed, as before in-place
// preservation was tried first; refusing outright emitted the raw edited
// stream instead.
int arr[] = { GAP_VALUE
};
int use(void) { return arr[0]; }
