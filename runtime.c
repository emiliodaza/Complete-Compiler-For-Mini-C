/* Link with generated assembly on Linux: cc -m32 -no-pie program.s runtime.c -o program */
#include <stdio.h>
#include <stdlib.h>

extern int main_function(int);

int read(void) {
    int value;
    if (scanf("%d", &value) != 1) {
        fprintf(stderr, "read: expected an integer on standard input\n");
        exit(1);
    }
    return value;
}

void print(int value) {
    printf("%d\n", value);
}

int main(int argc, char **argv) {
    int argument = argc > 1 ? atoi(argv[1]) : 0;
    int result = main_function(argument);
    fprintf(stderr, "return=%d\n", result);
    return 0;
}
