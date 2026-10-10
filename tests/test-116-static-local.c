// exit: 3
int c() { static int n = 0; n++; return n; }
int main() { c(); c(); return c(); }
