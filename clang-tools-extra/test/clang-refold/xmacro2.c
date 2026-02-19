// RUN: %clang-refold-tester xmacro2

// The "List" macro
#define GAME_STATE_LIST(X) \
  X(Idle,    "Waiting for player...") \
  X(Loading, "Loading assets...")     \
  X(Running, "Game in progress")     \
  X(Paused,  "Game paused")          \
  X(GameOver,"Better luck next time!")

// --- Step A: Generate the Enum ---
#define AS_ENUM(ID, STR) STATE_##ID,
typedef enum {
  GAME_STATE_LIST(AS_ENUM)
  STATE_COUNT
} GameState;

// --- Step B: Generate the String Lookup Table ---
#define AS_STRING(ID, STR) STR,
const char* StateDescriptions[] = {
  GAME_STATE_LIST(AS_STRING)
};

int main() {
  GameState current = STATE_Running;

  printf("Current State ID: %d\n", current);
  printf("Description: %s\n", StateDescriptions[current]);

  return 0;
}
