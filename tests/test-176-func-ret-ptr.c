// exit: 42
char *pick(int x) { char *a = "aa"; char *b = "BBBB"; return x ? a + 1 : b + 2; }
int *idp(int *p) { return p; }
int main() { int v = 40; int *p = idp(&v); char *s = pick(0); return *p + s[0] - 64; }
