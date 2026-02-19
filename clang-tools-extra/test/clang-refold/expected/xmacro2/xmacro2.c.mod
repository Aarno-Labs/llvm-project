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
  STATE_Idle, STATE_Loading, STATE_Executing, STATE_Paused, STATE_GameOver,
  STATE_COUNT
} GameState;

// --- Step B: Generate the String Lookup Table ---
#define AS_STRING(ID, STR) STR,
const char* StateDescriptions[] = {
  "Waiting for player two...", "Loading assets...", "Game in progress", "Game is paused", "Better luck next time!",
};

int main() {
  GameState current = STATE_Running;

  printf("Current State ID: %d\n", current);
  printf("Description: %s\n", StateDescriptions[current]);

  return 0;
}
