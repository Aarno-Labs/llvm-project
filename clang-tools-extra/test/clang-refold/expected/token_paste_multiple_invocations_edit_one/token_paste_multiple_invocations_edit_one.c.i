typedef long int ptrdiff_t;
typedef long unsigned int size_t;
typedef int wchar_t;
typedef long double max_align_t;
typedef struct { size_t n; int *data; } vec_int_t; vec_int_t vec_int_create(int *p, size_t n);
typedef struct { size_t n; short *data; } vec_short_t; vec_short_t vec_short_create(short *p, size_t n);
int main() { return 0; }
