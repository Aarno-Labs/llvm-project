typedef enum {
  STATE_Idle, STATE_Loading, STATE_Executing, STATE_Paused, STATE_GameOver,
  STATE_COUNT
} GameState;
const char* StateDescriptions[] = {
  "Waiting for player two...", "Loading assets...", "Game in progress", "Game is paused", "Better luck next time!",
};
int main() {
  GameState current = STATE_Running;
  printf("Current State ID: %d\n", current);
  printf("Description: %s\n", StateDescriptions[current]);
  return 0;
}
