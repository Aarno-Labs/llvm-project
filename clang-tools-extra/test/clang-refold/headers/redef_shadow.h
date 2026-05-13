#define FOO 5
int y2 = FOO;
#define FOO 50
int z = FOO + 1;
int y = y2;
int result = (y + z) * FOO;
