// RUN: %clang-refold-tester stale_body_hidden_by_nested_macro_cover
typedef unsigned short u16;
typedef unsigned int u32;

#define CAST(type, value) ((type)(value))

static union {
  char s[4];
  u32 u;
} cdf_bo;

#define NEED_SWAP (cdf_bo.u == CAST(u32, 0x01020304))
#define CDF_TOLE2(x) \
  (CAST(u16, NEED_SWAP ? _cdf_tole2(x) : CAST(u16, x)))

static u16 _cdf_tole2(u16 x) {
  return (u16)((x >> 8) | (x << 8));
}

u16 read_value(u16 *s, unsigned s_index_xj) {
  return CDF_TOLE2(*s);
}
