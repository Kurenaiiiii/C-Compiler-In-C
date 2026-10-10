// exit: 1
int slen(char *s) { int n = 0; while (s[n]) n++; return n; }
int main() { char s[6] = "hello"; int n = slen(s); for (int i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } return s[0] == 'o' && s[4] == 'h' && s[2] == 'l'; }
