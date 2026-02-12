// RUN: %clang-refold-tester macro_args_only_paste_whole_arg_idempotent
void OPEN (void) { printf("Opening file...\n"); }
void close(void) { printf("Closing file...\n"); }

typedef struct {
  void (*func)(void);
  const char *name;
} CommandMeta;

#define REGISTER_COMMAND(cmd)                                                  \
  CommandMeta cmd##_struct = {                                                 \
      cmd, /* 1. Standalone: Points to the actual function */                  \
      #cmd /* 2. Stringified: For logging/UI */                                \
  };                                                                           \
  void run_##cmd() {                                                           \
    printf("Executing internal: %s\n", #cmd);                                  \
    cmd(); /* 3. Standalone: Second use of the raw argument */                 \
  }

REGISTER_COMMAND(OPEN)
REGISTER_COMMAND(close)

int main() {
  run_OPEN ();
  printf("Struct info: %s is at address %p\n", OPEN_struct.name,
         (void *)OPEN_struct.func);
  return 0;
}
