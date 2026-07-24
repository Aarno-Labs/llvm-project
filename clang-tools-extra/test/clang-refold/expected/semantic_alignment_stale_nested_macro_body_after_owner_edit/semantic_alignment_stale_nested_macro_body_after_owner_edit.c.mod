// RUN: %clang-refold-tester semantic_alignment_stale_nested_macro_body_after_owner_edit
// Regression: a covering nested macro must not hide a stale body reference
// after the referenced TU object and the invocation payload are both edited.
typedef unsigned short word16;
typedef unsigned int word32;
int prefix_marker = 17;
#define CAST_VALUE(type, value) ((type)(value))
static union {
  char bytes[4];
  word32 value;
} byte_order_refolded;
#define NEED_BYTE_SWAP (byte_order.value == CAST_VALUE(word32, 0x01020304))
#define TO_LITTLE16(x) \
  (CAST_VALUE(word16, NEED_BYTE_SWAP ? swap16(x) : CAST_VALUE(word16, x)))
static word16 swap16(word16 x) {
  return (word16)((x >> 8) | (x << 8));
}
word16 read_word(word16 *source, unsigned source_index) {
  return (((word16)((byte_order_refolded.value == ((word32)(0x01020304))) ? swap16(source[source_index]) : ((word16)(source[source_index])))));
}
int suffix_marker = 23;
