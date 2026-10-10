// exit: 50
int f(int x) { switch (x) { case 1: return 10; case 2: return 20; default: return 30; } }
int main() { return f(2) + f(9); }
