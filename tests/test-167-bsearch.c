// exit: 5
int main() { int a[8] = {2, 5, 8, 12, 16, 23, 38, 56}; int key = 23; int lo = 0, hi = 7, ans = -1; while (lo <= hi) { int mid = (lo + hi) / 2; if (a[mid] == key) { ans = mid; break; } else if (a[mid] < key) lo = mid + 1; else hi = mid - 1; } return ans; }
