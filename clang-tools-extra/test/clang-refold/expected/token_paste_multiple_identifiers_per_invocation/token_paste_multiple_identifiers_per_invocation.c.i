typedef long int ptrdiff_t;
typedef long unsigned int size_t;
typedef int wchar_t;
typedef long double max_align_t;
typedef struct { size_t n; int *data; } arr_int_t; int arr_int_push(arr_int_t *a, int v); int arr_int_pop(arr_int_t *a);
int main() { return 0; }
