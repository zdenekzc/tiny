#include <stdio.h>
#include <stdlib.h>

#define Max 4

typedef struct Item
{
    int cnt; // number of keys
    int key [Max+1]; // key[0], ... key[cnt-1], plus one extra
    struct Item * ref [Max+2]; // ptr [0], ... ptr [cnt], plus one extra
} Item;

static Item * item_new (void)
{
    Item * p = malloc (sizeof *p);
    if (p == NULL)
    {
        perror ("malloc");
        exit (1);
    }
    p->cnt = 0;
    for (int i = 0; i < Max + 1; i++) p->key[i] = -1;
    for (int i = 0; i < Max + 2; i++) p->ref [i] = NULL;
    return p;
}

void enter0 (Item * p, int val)
{
    int i = 0;
    while (i < p->cnt && p->key[i] < val) i++;
    // i == p->cnt, all were smaller, insert
    // i < p->cnt, p->key[i] >= val
    if (i < p->cnt && p->key[i] == val) {  }
    else {
        Item * t = p->ref[i];
        if (t == NULL)
        {
            for (int k = p->cnt - 1; k >= i; k--) p->key[k + 1] = p->key[k];
            for (int k = p->cnt; k >= i; k--) p->ref[k + 1] = p->ref[k];
            p->key[i] = val;
            p->ref[i] = NULL;
            p->cnt++;
        }
        else
        {
            enter0 (t, val);
            if (t->cnt == Max + 1)
            {
                int a = Max / 2; // a keys remain in t
                int b = t->cnt - a -1; // b keys go to the new block
                int d = t->key[a]; // split point

                Item * n = item_new ();
                n->cnt = b;
                for (int k = a + 1; k < t->cnt; k++) n->key[k - a - 1] = t->key[k];
                for (int k = a + 1; k < t->cnt+1; k++) n->ref[k - a - 1] = t->ref[k];

                for (int k = a + 1; k < t->cnt; k++) t->key[k] = -1;
                for (int k = a + 1; k < t->cnt + 1; k++) t->ref[k] = NULL;
                t->cnt = a;

                for (int k = p->cnt - 1; k >= i; k--) p->key[k + 1] = p->key[k];
                for (int k = p->cnt; k > i; k--) p->ref[k + 1] = p->ref[k];
                p->key[i] = d;
                p->ref[i+1] = n;
                p->cnt++;
            }
        }
    }
}

void enter (Item ** p, int val)
{
    if (*p == NULL)
    {
        *p = item_new ();
        (*p)->cnt = 1;
        (*p)->key[0] = val;
        (*p)->ref[0] = NULL;
        (*p)->ref[1] = NULL;
    }
    else
    {
        enter0 (*p, val);
        Item * t = *p;
        if (t->cnt == Max + 1)
        {
            int a = Max / 2; // a keys remain in t
            int b = t->cnt - a - 1; // b keys go to the new block
            int d = t->key[a]; // split point

            Item * n = item_new ();
            n->cnt = b;
            for (int k = a + 1; k < t->cnt; k++) n->key[k - a - 1] = t->key[k];
            for (int k = a + 1; k < t->cnt + 1; k++) n->ref[k - a - 1] = t->ref[k];

            for (int k = a + 1; k < t->cnt; k++) t->key[k] = -1;
            for (int k = a + 1; k < t->cnt + 1; k++) t->ref[k] = NULL;
            t->cnt = a;

            *p = item_new ();
            (*p)->cnt = 1;
            (*p)->ref[0] = t;
            (*p)->key[0] = d;
            (*p)->ref[1] = n;
        }
    }
}

void print (Item * p)
// at least print the values in increasing order
{
    if (p != NULL)
    {
        for (int i = 0; i < p->cnt; i++)
        {
            print (p->ref[i]);
            printf ("%d\n", p->key[i]);
        }
        print (p->ref[p->cnt]);
    }
}

int main ()
{
    Item * root = NULL;
    enter(&root, 30);
    enter(&root, 20);
    enter(&root, 10);
    enter(&root, 40);
    enter(&root, 50);

    for (int k = 21; k <= 29; k++) enter(&root, k);

    for (int k = 51; k <= 800; k++) enter(&root, k);

    print (root);
}
