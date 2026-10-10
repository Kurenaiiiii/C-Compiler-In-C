// exit: 25
int main() { int i = 0, s = 0; while (1) { i++; if (i % 2 == 0) continue; if (i > 9) break; s += i; } return s; }
