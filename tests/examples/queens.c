#include <stdio.h>
#include <stdbool.h>

#define N 8

int p [N];
int cnt = 0;

void place (int s)
{
    for (int r = 0; r < N; r++) {
        bool ok = true;
        for (int v = 1; v <= s && ok; v++)
        {
            int d = p[s - v];
            if (d == r || d == r-v || d == r+v)
                ok = false;
        }
        if (ok)
        {
            p[s] = r;
            if (s < N - 1)
                place(s + 1);
            else
            {
                cnt++;
                for (int i = 0; i < N; i++) printf ("%d ", p[i]);
                printf (" : %d\n", cnt);
            }
        }
    }
}

int main ()
{
    for (int i = 0; i < N; i++) p[i] = -1;
    place(0);
    printf ("O.K.\n");
}
