// exit: 55
int main() { int s = 0; for (int i = 0; i < 10; i++) { for (int j = 0; j < 10; j++) { if (j > i) break; s++; } } return s; }
