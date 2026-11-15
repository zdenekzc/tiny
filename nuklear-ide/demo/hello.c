/*
 * Demo program for nuklear-ide.
 *
 * Press F5 in the IDE to compile and run it.  The point is that editing this
 * file and rerunning is a two-key loop: edit, F5.
 */
#include <stdio.h>

static int square(int x)
{
    re turn x * x;
}

int main(void)
{
    int sum = 0;
    for (int i = 1; i <= 10; ++i) {
        sum += square(i);
        printf("%2d^2 = %3d\n", i, square(i));
    }
    printf("\nsum of squares 1..10 = %d\n", sum);
    return 0;
}
