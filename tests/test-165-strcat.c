// exit: 1
int slen(char *s) { int n = 0; while (s[n]) n++; return n; }
void scat(char *d, char *s) { int i = slen(d); int j = 0; while ((d[i++] = s[j++])) { } }
int scmp(char *a, char *b) { while (*a && *a == *b) { a++; b++; } return *a - *b; }
int main() { char buf[16]; buf[0] = 0; scat(buf, "foo"); scat(buf, "bar"); return scmp(buf, "foobar") == 0 && slen(buf) == 6; }
