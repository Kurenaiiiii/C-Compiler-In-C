// exit: 1
int main() { int x = 0; int a = (1 || (x = 5)); return x + a; }
