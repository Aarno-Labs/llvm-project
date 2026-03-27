// RUN: %clang-refold-tester macro_api_generation2 REFOLD
// RUN: %clang-refold-tester macro_api_generation2 NOREFOLD1
// RUN: %clang-refold-tester macro_api_generation2 NOREFOLD2
// RUN: %clang-refold-tester macro_api_generation2 BODYMOD
void OPEN(void) { printf("Opening file...\n"); }
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

CommandMeta OPEN_struct = { open, "OPEN" }; void run_OPEN() { printf("Executing internal: %s\n", "OPEN"); OPEN(); }
REGISTER_COMMAND(close)

int main() {
  run_OPEN();
  printf("Struct info: %s is at address %p\n", OPEN_struct.name,
         (void *)OPEN_struct.func);
  return 0;
}
