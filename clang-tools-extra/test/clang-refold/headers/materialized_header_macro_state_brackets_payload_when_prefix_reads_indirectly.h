// Header for materialized_header_macro_state_brackets_payload_when_prefix_reads_indirectly.
//
// The edit lands inside `ID(4242)`, and `IND +` before it on the same line
// reads `VAL` through `IND` without spelling it.
#define VAL 99
#define IND VAL
#define ID(x) (x)
int patched(void) { return IND + ID(4242); }
