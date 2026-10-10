// exit: 1
int c() { static int n = 10; n += 5; return n; }
int main() { int a = c(); return c() == 20 && a == 15; }
