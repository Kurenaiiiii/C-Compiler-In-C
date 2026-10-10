// exit: 5
enum E { RED, GREEN, BLUE };
int main() { enum E c = GREEN; switch (c) { case RED: return 1; case GREEN: return 5; case BLUE: return 9; } return 0; }
