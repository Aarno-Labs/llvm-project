#pragma once

#ifdef XXX
#ifdef YYY
int xxx(int x, int y);
int yyy(int x, int y);
int no_zzz(int x, int y);
#elif ZZZ
unsigned long xxx(int x, int y);
unsigned long no_yyy(int x, int y);
unsigned long zzz(int x, int y);
#else
int xxx(int x, int y);
int no_yyy(int x, int y);
int no_zzz(int x, int y);
#endif
#else
int no_xxx(int x, int y);
int no_yyy(int x, int y);
int no_zzz(int x, int y);
#endif
