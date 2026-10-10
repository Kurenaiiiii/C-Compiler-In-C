// exit: 8
int main() { int n = 0; for (int i = 2; i <= 20; i++) { int p = 1; for (int j = 2; j * j <= i; j++) if (i % j == 0) p = 0; n += p; } return n; }
