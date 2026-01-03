// RUN: %clang-refold-tester identity
// ---------- object-like and nested ----------
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define CAT_HELPER(a,b) a##b
#define CAT(a,b) CAT_HELPER(a,b)

#define UNIQUE(prefix) CAT(prefix, __COUNTER__)

// ---------- variadics + __VA_OPT__ ----------
#define LOG(fmt, ...) \
  fprintf(stderr, "[%s:%d] " fmt __VA_OPT__(,) __VA_ARGS__, __FILE_NAME__, __LINE__)

// ---------- multi-statement safety ----------
#define ASSERT(cond) do { \
  if (!(cond)) { \
    LOG("ASSERT failed: %s\n", #cond); \
  } \
} while (0)

// ---------- statement-expression-ish helper (portable-ish) ----------
#define WITH_TEMP(type, name, init, body) do { \
  type name = (init); \
  body; \
} while (0)

// ---------- function-like macros with side-effect safety ----------
#define MIN(a,b) (( (a) < (b) ) ? (a) : (b))

// ---------- token pasting to declare/define ----------
#define DECL_FN(ret, name, args) ret name args
#define DEF_FN(ret, name, args, body) ret name args body

// ---------- X-macro list ----------
#define ERROR_LIST(X) \
  X(OK,        0)     \
  X(NOT_FOUND, 404)   \
  X(BAD,       500)

#define MAKE_ENUM(name, val) ERR_##name = (val),
typedef enum {
  ERROR_LIST(MAKE_ENUM)
} ErrorCode;

#define MAKE_CASE(name, val) case ERR_##name: return #name;
static const char* error_name(ErrorCode e) {
  switch (e) {
    ERROR_LIST(MAKE_CASE)
    default: return "UNKNOWN";
  }
}

// ---------- macro that references predefined macros inside another macro ----------
#define PRINT_LOC(MSG) \
  printf("%s @ %s:%d\n", (MSG), __FILE_NAME__, __LINE__)

// ---------- demonstrate “macro in macro” + argument forwarding ----------
#define PRINT_FILE(FMT, ...) \
  printf((FMT), __FILE_NAME__, __LINE__ __VA_OPT__(,) __VA_ARGS__)

// ---------- use token pasting to create a unique variable ----------
#define MAKE_UNIQUE_INT(init) \
  int UNIQUE(tmp_) = (init)

// ---------- use everything ----------
DEF_FN(int, main, (void), {
  LOG("hello %s\n", "world");

  ASSERT(MIN(3, 7) == 3);

  WITH_TEMP(int, x, 42, {
    LOG("x=%d str(x)=%s\n", x, STR(x));
  });

  PRINT_LOC("here");

  // Predefined macros inside a macro definition, then edited at the expansion site:
  PRINT_FILE("file=%s line=%d extra=%d\n", 123);

  MAKE_UNIQUE_INT(10);
  MAKE_UNIQUE_INT(20);

  ErrorCode e = ERR_NOT_FOUND;
  LOG("error=%s (%d)\n", error_name(e), (int)e);

  return 0;
})
