// exit: 10
int main() { int x = 0; (1 && (x = 7)); (0 || (x = x + 3)); return x; }
