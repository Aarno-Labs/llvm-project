// RUN: %clang-refold-tester-with-lines mixed_include_zero_token_va_opt_payload_noncanonical_definition_gap
//
// Regression: a variadic forwarder whose __VA_OPT__ payload is a zero-token
// macro call is proved neutral when the definition is spelled with extra
// blanks.  The tail is not empty here, so the payload itself must be tiled by
// the nested EMPTY call.  The proof used to read the replacement list out of
// the directive's `text`, whose canonical spacing shifted that call's offset,
// and the edit refused.  The proof now walks the producer's source range for
// each replacement-list token.
#define EMPTY
#define FORWARD( ... )  __VA_OPT__( EMPTY )
int x =
3
FORWARD(1)
;
int y = 7;
