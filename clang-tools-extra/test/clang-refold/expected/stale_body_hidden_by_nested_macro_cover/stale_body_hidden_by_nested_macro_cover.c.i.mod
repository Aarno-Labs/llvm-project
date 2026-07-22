
typedef unsigned short u16;
typedef unsigned int u32;
static union {
  char s[4];
  u32 u;
} cdf_bo_xjtr_0;
static u16 _cdf_tole2(u16 x) {
  return (u16)((x >> 8) | (x << 8));
}
u16 read_value(u16 *s, unsigned s_index_xj) {
  return (((u16)((cdf_bo_xjtr_0.u == ((u32)(0x01020304))) ? _cdf_tole2(s[s_index_xj]) : ((u16)(s[s_index_xj])))));
}
