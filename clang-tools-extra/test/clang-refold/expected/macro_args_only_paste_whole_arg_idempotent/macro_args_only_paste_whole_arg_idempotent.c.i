
void open(void) { printf("Opening file...\n"); }
void close(void) { printf("Closing file...\n"); }
typedef struct {
  void (*func)(void);
  const char *name;
} CommandMeta;
CommandMeta open_struct = { open, "open" }; void run_open() { printf("Executing internal: %s\n", "open"); open(); }
CommandMeta close_struct = { close, "close" }; void run_close() { printf("Executing internal: %s\n", "close"); close(); }
int main() {
  run_open();
  printf("Struct info: %s is at address %p\n", open_struct.name,
         (void *)open_struct.func);
  return 0;
}
