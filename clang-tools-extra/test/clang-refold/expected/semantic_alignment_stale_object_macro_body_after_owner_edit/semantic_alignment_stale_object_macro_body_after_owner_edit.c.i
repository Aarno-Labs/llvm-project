int prefix_marker = 17;
static int character_classes[4] = {0, 1, 1, 1};
static int classify_ascii(const unsigned char *buffer) { int flag = character_classes[buffer[0]]; return !(flag != 1); }
int suffix_marker = 23;
