// RUN: %clang-refold-tester macro_arg_edit_no_expansion3 XXX
#if XXX
#define BYTE4 0
#else
#define BYTE4 3
#endif
#define set_zero(number, byte, bit) \
  *((char *)&(number)+byte) &= ~(0x1 << (bit))
float x = -100;
set_zero(x, BYTE4, 8);
