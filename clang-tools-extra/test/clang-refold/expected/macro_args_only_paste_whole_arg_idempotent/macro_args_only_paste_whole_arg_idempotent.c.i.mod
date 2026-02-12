
void OPEN(void) { printf("Opening file...\n"); }
void close(void) { printf("Closing file...\n"); }
typedef struct {
  void (*func)(void);
  const char *name;
} CommandMeta;
CommandMeta OPEN_struct = { OPEN, "OPEN" }; void run_OPEN() { printf("Executing internal: %s\n", "OPEN"); OPEN(); }
CommandMeta close_struct = { close, "close" }; void run_close() { printf("Executing internal: %s\n", "close"); close(); }
int main() {
  run_OPEN();
  printf("Struct info: %s is at address %p\n", OPEN_struct.name,
         (void *)OPEN_struct.func);
  return 0;
}
