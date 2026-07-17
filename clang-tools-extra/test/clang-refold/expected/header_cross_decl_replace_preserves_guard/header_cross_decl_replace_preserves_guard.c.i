
typedef int kept_before_t;
typedef int(*first_callback_t)(int);
typedef long(*second_callback_t)(long);
typedef int kept_after_t;

int add_kept_values(kept_before_t before, kept_after_t after) {
  return before + after;
}
