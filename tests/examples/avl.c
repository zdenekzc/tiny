#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct Item
{
    int key;
    int height;
    struct Item * left;
    struct Item * right;
} Item;

static Item * item_new (int key)
{
    Item * p = malloc (sizeof *p);
    if (p == NULL)
    {
        perror ("malloc");
        exit (1);
    }
    p->key = key;
    p->height = 1;
    p->left = NULL;
    p->right = NULL;
    return p;
}

static int h (Item * p)
{
    if (p == NULL)
        return 0;
    else
        return p->height;
}

void update (Item * p)
{
    int L = h (p->left);
    int R = h (p->right);
    if (L > R)
        p->height = L + 1;
    else
        p->height = R + 1;
}

void enter (Item ** root, int val)
{
    Item * a;

    if (*root == NULL)
    {
        *root = item_new (val);
    }
    else if (val < (*root)->key) enter (&(*root)->left, val);
    else if (val > (*root)->key) enter (&(*root)->right, val);
    else { }

    a = *root;
    update (a);
    int L = h (a->left);
    int R = h (a->right);
    if (L > R + 1)
    {
        Item * b = a->left;
        int LL = h (b->left);
        int RR = h (b->right);
        if (LL > RR)
        {
            //     a
            //   b
            // c
            Item * t = b->right; // subtree
            *root = b; // new root
            //     b
            //  c     a
            b->right = a;
            a->left = t;
            update (a);
            update (b); // last
        }
        else
        {
            Item * c = b->right;
            //         a
            //   b
            //      c
            Item * t = c->left; // subtree
            Item * u = c->right; // subtree
            *root = c; // new root
            //     c
            //  b     a
            c->left = b;
            c->right = a;
            b->right = t;
            a->left = u;
            update (a);
            update (b);
            update (c); // last
        }
    }
    else if (R > L + 1)
    {
        Item * b = a->right;
        int LL = h (b->right);
        int RR = h (b->left);
        if (LL > RR)
        {
            //  a
            //     b
            //        c
            Item * t = b->left; // subtree
            *root = b; // new root
            //     b
            //  a     c
            b->left = a;
            a->right = t;
            update (a);
            update (b); // last
        }
        else
        {
            Item * c = b->left;
            //  a
            //        b
            //     c
            Item * t = c->right; // subtree
            Item * u = c->left; // subtree
            *root = c; // new root
            //     c
            //  a     b
            c->right = b;
            c->left = a;
            b->left = t;
            a->right = u;
            update (a);
            update (b);
            update (c); // last
        }
    }
}

void print (Item * branch, int level)
{
    if (branch != NULL)
    {
        for (int i = 0; i < level; i++)
            printf ("-");

        printf ("%d (%d)\n", branch->key, branch->height);

        print (branch->left, level + 1);
        print (branch->right, level + 1);
    }
}

bool ok;
bool first;
int  last_value;

void checkBranch (Item * branch)
{
    if (branch != NULL)
    {
        checkBranch (branch->left);

        if (! first)
           if (last_value >= branch->key)
           {
               printf ("WRONG\n");
               ok = false;
           }

        last_value = branch->key;
        first = false;
        checkBranch (branch->right);
    }
}

void check (Item * root)
{
   ok = true;
   first = true;
   checkBranch (root);
   if (ok) printf ("O.K.\n");
}

int main ()
{
    Item * root = NULL;

    const int N = 32;
    // const int N = 16*1024;

    for (int k = 1; k <= N; k++)
       enter (&root, k);

    if (N <= 1000)
       print (root, 0);

    printf ("Tree height: %d\n", h (root));
    check (root);
}
