// exit: 13
int f(int x) { int r = 0; switch (x) { case 1: r += 1; case 2: r += 2; case 3: r += 4; break; default: r = 99; } return r; }
int main() { return f(1) + f(2); }
