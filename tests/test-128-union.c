// exit: 1
union U { int i; char c[4]; };
int main() { union U u; u.i = 1; return u.c[0] + u.c[1]; }
