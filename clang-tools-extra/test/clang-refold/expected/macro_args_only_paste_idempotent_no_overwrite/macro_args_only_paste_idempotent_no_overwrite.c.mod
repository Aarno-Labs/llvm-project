// RUN: %clang-refold-tester macro_args_only_paste_idempotent_no_overwrite
#define DECLARE_ARRAY(TYPE) \
    typedef struct { \
        TYPE *data; \
        size_t size; \
        size_t capacity; \
    } array_##TYPE##_t; \
    \
    array_##TYPE##_t* array_##TYPE##_create(size_t initial_capacity); \
    void array_##TYPE##_destroy(array_##TYPE##_t *arr); \
    int array_##TYPE##_push(array_##TYPE##_t *arr, TYPE value); \
    TYPE array_##TYPE##_get(array_##TYPE##_t *arr, size_t index); \
    size_t array_##TYPE##_size(array_##TYPE##_t *arr); \
    void array_##TYPE##_clear(array_##TYPE##_t *arr);

DECLARE_ARRAY(float)
