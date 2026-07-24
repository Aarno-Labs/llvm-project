typedef unsigned short word16;
typedef unsigned int word32;
int prefix_marker = 17;
static union {
  char bytes[4];
  word32 value;
} byte_order_refolded;
static word16 swap16(word16 x) {
  return (word16)((x >> 8) | (x << 8));
}
word16 read_word(word16 *source, unsigned source_index) {
  return (((word16)((byte_order_refolded.value == ((word32)(0x01020304))) ? swap16(source[source_index]) : ((word16)(source[source_index])))));
}
int suffix_marker = 23;
