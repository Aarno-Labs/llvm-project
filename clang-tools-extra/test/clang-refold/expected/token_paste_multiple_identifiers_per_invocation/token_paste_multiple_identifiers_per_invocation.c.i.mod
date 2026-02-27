typedef long int ptrdiff_t;
typedef long unsigned int size_t;
typedef int wchar_t;
typedef long double max_align_t;
typedef struct { size_t n; float *data; } arr_float_t; int arr_float_push(arr_float_t *a, float v); float arr_float_pop(arr_float_t *a);
int main() { return 0; }
