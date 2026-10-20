extern void print(int);
extern int read();

int factorial(int n) {
    int result;
    result = 1;
    while (n > 1) {
        result = result * n;
        n = n - 1;
    }
    print(result);
    return result;
}
