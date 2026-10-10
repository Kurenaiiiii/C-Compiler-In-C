// exit: 23
int f(int x, int y) { int r = 0; switch (x) { case 1: switch (y) { case 1: r = 11; break; default: r = 12; } break; default: r = 20; } return r; }
int main() { return f(1, 1) + f(1, 2); }
