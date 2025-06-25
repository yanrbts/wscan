/*
 * Copyright (c) 2024-2024, yanruibinghxu@gmail.com All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ws_rbtree.h>
#include <ws_malloc.h>

#define rb_malloc  zmalloc
#define rb_free    zfree

/* Creates and returns a new table
 * with comparison function compare using parameter param
 * You must define a custom comparison function. 
 * If you do not define one, it returns NULL.
 * Returns NULL if memory allocation failed. */
rbTable *rbCreate(rb_comparison_func *compare, void *param) {
    struct rbTable *tree;

    if (compare == NULL)
        return NULL;

    tree = rb_malloc(sizeof(*tree));
    if (tree == NULL) return NULL;

    tree->rb_root = NULL;
    tree->rb_compare = compare;
    tree->rb_param = param;
    tree->rb_count = 0;
    tree->rb_generation = 0;

    return tree;
}

/* Frees storage allocated for tree.
 * If destroy != NULL, applies it to each data item in inorder. 
 * Post-order traversal method destroys the red-black tree structure
 * 
 *     A        	B			 D
 *    / \          / \			  \
 *   B   C   =>   D   A     =>     B
 *  / \				   \            \
 * D   E				C            A
 *					    /             \
 *					   E               C
 *									   /
 *                                    E
 */
void rbDestroy(rbTable *tree, rb_item_func *destroy) {
    rbNode *p, *q;

    if (tree == NULL) return;

    for (p = tree->rb_root; p != NULL; p = q) {
        if (p->rb_link[0] == NULL) {
            q = p->rb_link[1];
            if (destroy != NULL && p->rb_data != NULL)
                destroy(p->rb_data, tree->rb_param);
            zfree(p);
        } else {
            q = p->rb_link[0];
            p->rb_link[0] = q->rb_link[1];
            q->rb_link[1] = p;
        }
    }
    zfree(tree);
}

/* Inserts item into tree and returns a pointer to item's address.
 * If a duplicate item is found in the tree,
 * returns a pointer to the duplicate without inserting item.
 * Returns NULL in case of memory allocation failure. */
void **rbProbe(rbTable *tree, void *item) {
    rbNode *pa[RB_MAX_HEIGHT];          /* Nodes on stack. */
    unsigned char da[RB_MAX_HEIGHT];    /* Directions moved from stack nodes. */
    int k;                              /* Stack height. */
    rbNode *p;  /* Traverses tree looking for insertion point. */
    rbNode *n;  /* Newly inserted node. */

    if (tree == NULL || item == NULL)
        return NULL;
    
    pa[0] = (rbNode*)&tree->rb_root;
    da[0] = 0;
    k = 1;
    /* Find item and return it if it already exists */
    for (p = tree->rb_root; p != NULL; p = p->rb_link[da[k-1]]) {
        int cmp = tree->rb_compare(item, p->rb_data, tree->rb_param);
        if (cmp == 0)
            return &p->rb_data;
        pa[k] = p;
        da[k++] = cmp > 0;
    }

    n = pa[k-1]->rb_link[da[k-1]] = rb_malloc(sizeof(*n));
    if (n == NULL) return NULL;

    n->rb_data = item;
    n->rb_link[0] = n->rb_link[1] = NULL;
    n->rb_color = RB_RED;
    tree->rb_count++;
    tree->rb_generation++;

    while (k >= 3 && pa[k-1]->rb_color == RB_RED) {
        if (da[k-2] == 0) {
            rbNode *y = pa[k-2]->rb_link[1];

            if (y != NULL && y->rb_color == RB_RED) {
                pa[k-1]->rb_color = y->rb_color = RB_BLACK;
                pa[k-2]->rb_color = RB_RED;
                k -= 2;
            } else {
                rbNode *x;

                if (da[k-1] == 0) {
                    y = pa[k-1];
                } else {
                    x = pa[k-1];
                    y = x->rb_link[1];
                    x->rb_link[1] = y->rb_link[0];
                    y->rb_link[0] = x;
                    pa[k-2]->rb_link[0] = y;
                }

                x = pa[k-2];
                x->rb_color = RB_RED;
                y->rb_color = RB_BLACK;

                x->rb_link[0] = y->rb_link[1];
                y->rb_link[1] = x;
                pa[k-3]->rb_link[da[k-3]] = y;
                break;
            }
        } else {
            rbNode *y = pa[k-2]->rb_link[0];

            if (y != NULL && y->rb_color == RB_RED) {
                pa[k-1]->rb_color = y->rb_color = RB_BLACK;
                pa[k-2]->rb_color = RB_RED;
                k -= 2;
            } else {
                rbNode *x;

                if (da[k-1] == 1) {
                    y = pa[k-1];
                } else {
                    x = pa[k-1];
                    y = x->rb_link[0];
                    x->rb_link[0] = y->rb_link[1];
                    y->rb_link[1] = x;
                    pa[k-2]->rb_link[1] = y;
                }

                x = pa[k-2];
                x->rb_color = RB_RED;
                y->rb_color = RB_BLACK;

                x->rb_link[1] = y->rb_link[0];
                y->rb_link[0] = x;
                pa[k-3]->rb_link[da[k-3]] = y;
                break;
            }
        }
    }

    tree->rb_root->rb_color = RB_BLACK;
    return &n->rb_data;
}

/* Inserts item into table.
 * Returns NULL if item was successfully inserted
 * or if a memory allocation error occurred.
 * Otherwise, returns the duplicate item. */
void *rbInsert(rbTable *tree, void *item) {
    void **p = rbProbe(tree, item);
    return p == NULL || *p == item ? NULL : *p;
}

/* Inserts item into table, replacing any duplicate item.
 * Returns NULL if item was inserted without replacing a duplicate,
 * or if a memory allocation error occurred.
 * Otherwise, returns the item that was replaced. */
void *rbReplace(rbTable *tree, void *item) {
    void **p = rbProbe(tree, item);
    if (p == NULL || *p == item) {
        return NULL;
    } else {
        void *r = *p;
        *p = item;
        return r;
    }
}

/* Deletes from tree and returns an item matching item.
 * Returns a null pointer if no matching item found. */
void *rbDelete(rbTable *tree, const void *item) {
    rbNode *pa[RB_MAX_HEIGHT];          /* Nodes on stack. */
    unsigned char da[RB_MAX_HEIGHT];    /* Directions moved from stack nodes. */
    int k;                              /* Stack height. */
    rbNode *p;
    int cmp;

    if (tree == NULL || item == NULL)
        return NULL;
    
    k = 0;
    p = (rbNode*)&tree->rb_root;
    for (cmp = -1; cmp != 0; cmp = tree->rb_compare(item, p->rb_data, tree->rb_param)) {
        int dir = cmp > 0;
        pa[k] = p;
        da[k++] = dir;

        p = p->rb_link[dir];
        if (p == NULL)
            return NULL;
    }
    item = p->rb_data;

    if (p->rb_link[1] == NULL) {
        pa[k-1]->rb_link[da[k-1]] = p->rb_link[0];
    } else {
        enum rbColor t;
        rbNode *r = p->rb_link[1];

        if (r->rb_link[0] == NULL) {
            r->rb_link[0] = p->rb_link[0];
            t = r->rb_color;
            r->rb_color = p->rb_color;
            p->rb_color = t;
            pa[k-1]->rb_link[da[k-1]] = r;
            da[k] = 1;
            pa[k++] = r;
        } else {
            rbNode *s;
            int j = k++;

            for (;;) {
                da[k] = 0;
                pa[k++] = r;
                s = r->rb_link[0];
                if (s->rb_link[0] == NULL)
                    break;
                r = s;
            }

            da[j] = 1;
            pa[j] = s;
            pa[j-1]->rb_link[da[j-1]] = s;

            s->rb_link[0] = p->rb_link[0];
            r->rb_link[0] = s->rb_link[1];
            s->rb_link[1] = p->rb_link[1];

            t = s->rb_color;
            s->rb_color = p->rb_color;
            p->rb_color = t;
        }
    }

    if (p->rb_color == RB_BLACK) {
        for (;;) {
            rbNode *x = pa[k-1]->rb_link[da[k-1]];
            if (x != NULL && x->rb_color == RB_RED) {
                x->rb_color = RB_BLACK;
                break;
            }

            if (k < 2) break;

            if (da[k-1] == 0) {
                rbNode *w = pa[k-1]->rb_link[1];

                if (w->rb_color == RB_RED) {
                    w->rb_color = RB_BLACK;
                    pa[k-1]->rb_color = RB_RED;

                    pa[k-1]->rb_link[1] = w->rb_link[0];
                    w->rb_link[0] = pa[k-1];
                    pa[k-2]->rb_link[da[k-2]] = w;

                    pa[k] = pa[k-1];
                    da[k] = 0;
                    pa[k-1] = w;
                    k++;

                    w = pa[k-1]->rb_link[1];
                }

                if ((w->rb_link[0] == NULL || w->rb_link[0]->rb_color == RB_BLACK)
                    && (w->rb_link[1] == NULL || w->rb_link[1]->rb_color == RB_BLACK)) {
                    w->rb_color = RB_RED;
                } else {
                    if (w->rb_link[1] == NULL || w->rb_link[1]->rb_color == RB_BLACK) {
                        rbNode *y = w->rb_link[0];
                        y->rb_color = RB_BLACK;
                        w->rb_color = RB_RED;
                        w->rb_link[0] = y->rb_link[1];
                        y->rb_link[1] = w;
                        w = pa[k-1]->rb_link[1] = y;
                    }

                    w->rb_color = pa[k-1]->rb_color;
                    pa[k-1]->rb_color = RB_BLACK;
                    w->rb_link[1]->rb_color = RB_BLACK;

                    pa[k-1]->rb_link[1] = w->rb_link[0];
                    w->rb_link[0] = pa[k-1];
                    pa[k-2]->rb_link[da[k-2]] = w;
                    break;
                }
            } else {
                rbNode *w = pa[k-1]->rb_link[0];

                if (w->rb_color == RB_RED) {
                    w->rb_color = RB_BLACK;
                    pa[k-1]->rb_color = RB_RED;

                    pa[k-1]->rb_link[0] = w->rb_link[1];
                    w->rb_link[1] = pa[k-1];
                    pa[k-2]->rb_link[da[k-2]] = w;

                    pa[k] = pa[k-1];
                    da[k] = 1;
                    pa[k-1] = w;
                    k++;

                    w = pa[k-1]->rb_link[0];
                }

                if ((w->rb_link[0] == NULL || w->rb_link[0]->rb_color == RB_BLACK)
                    && (w->rb_link[1] == NULL || w->rb_link[1]->rb_color == RB_BLACK)) {
                    w->rb_color = RB_RED;
                } else {
                    if (w->rb_link[0] == NULL || w->rb_link[0]->rb_color == RB_BLACK) {
                        rbNode *y = w->rb_link[1];
                        y->rb_color = RB_BLACK;
                        w->rb_color = RB_RED;
                        w->rb_link[1] = y->rb_link[0];
                        y->rb_link[0] = w;
                        w = pa[k - 1]->rb_link[0] = y;
                    }

                    w->rb_color = pa[k - 1]->rb_color;
                    pa[k - 1]->rb_color = RB_BLACK;
                    w->rb_link[0]->rb_color = RB_BLACK;

                    pa[k - 1]->rb_link[0] = w->rb_link[1];
                    w->rb_link[1] = pa[k - 1];
                    pa[k - 2]->rb_link[da[k - 2]] = w;
                    break;
                }
            }

            k--;
        }
    }

    zfree(p);
    tree->rb_count--;
    tree->rb_generation++;
    return (void*)item;
}

/* Search tree for an item matching item, and return it if found.
 * Otherwise return NULL. */
void *rbFind(const rbTable *tree, const void *item) {
    const rbNode *p;

    if (tree == NULL || item == NULL) return NULL;

    for (p = tree->rb_root; p != NULL; ) {
        int cmp = tree->rb_compare(item, p->rb_data, tree->rb_param);

        if (cmp < 0)
            p = p->rb_link[0];
        else if (cmp > 0)
            p = p->rb_link[1];
        else
            return p->rb_data;
    }
    return NULL;
}

/* Initializes trav for use with tree
 * and selects the null node. */
void rbIterInit(rbIter *trav, rbTable *tree) {
    trav->rb_table = tree;
    trav->rb_node = NULL;
    trav->rb_height = 0;
    trav->rb_generation = tree->rb_generation;
}

/* Initializes trav for tree
 * and selects and returns a pointer to its least-valued item.
 * Returns NULL if tree contains no nodes. */
void *rbIterFirst(rbIter *trav, rbTable *tree) {
    rbNode *x;

    assert(tree != NULL && trav != NULL);

    trav->rb_table = tree;
    trav->rb_height = 0;
    trav->rb_generation = tree->rb_generation;

    x = tree->rb_root;
    if (x != NULL) {
        while (x->rb_link[0] != NULL) {
            assert(trav->rb_height < RB_MAX_HEIGHT);
            trav->rb_stack[trav->rb_height++] = x;
            x = x->rb_link[0];
        }
    }
    trav->rb_node = x;

    return x != NULL ? x->rb_data : NULL;
}

/* Initializes trav for tree
 * and selects and returns a pointer to its greatest-valued item.
 * Returns NULL if tree contains no nodes. */
void *rbIterLast(rbIter *trav, rbTable *tree) {
    rbNode *x;

    assert (tree != NULL && trav != NULL);

    trav->rb_table = tree;
    trav->rb_height = 0;
    trav->rb_generation = tree->rb_generation;

    x = tree->rb_root;
    if (x != NULL) {
        while (x->rb_link[1] != NULL) {
            assert (trav->rb_height < RB_MAX_HEIGHT);
            trav->rb_stack[trav->rb_height++] = x;
            x = x->rb_link[1];
        }
    } 
    trav->rb_node = x;

    return x != NULL ? x->rb_data : NULL;
}

/* Searches for item in tree.
 * If found, initializes trav to the item found and returns the item
 * as well.
 * If there is no matching item, initializes trav to the null item
 * and returns NULL. */
void *rbIterFind(rbIter *trav, rbTable *tree, void *item) {
    rbNode *p, *q;

    assert (trav != NULL && tree != NULL && item != NULL);

    trav->rb_table = tree;
    trav->rb_height = 0;
    trav->rb_generation = tree->rb_generation;

    for (p = tree->rb_root; p != NULL; p = q) {
        int cmp = tree->rb_compare(item, p->rb_data, tree->rb_param);

        if (cmp < 0)
            q = p->rb_link[0];
        else if (cmp > 0)
            q = p->rb_link[1];
        else {
            trav->rb_node = p;
            return p->rb_data;
        }
        assert(trav->rb_height < RB_MAX_HEIGHT);
        trav->rb_stack[trav->rb_height++] = p;
    }
    trav->rb_height = 0;
    trav->rb_node = NULL;
    return NULL;
}

/* Attempts to insert item into tree.
 * If item is inserted successfully, it is returned and trav is
 * initialized to its location.
 * If a duplicate is found, it is returned and trav is initialized to
 * its location.  No replacement of the item occurs.
 * If a memory allocation failure occurs, NULL is returned and trav
 * is initialized to the null item. */
void *rbIterInsert(rbIter *trav, rbTable *tree, void *item) {
    void **p;

    assert(trav != NULL && tree != NULL && item != NULL);

    p = rbProbe(tree, item);

    if (p != NULL) {
        trav->rb_table = tree;
        trav->rb_node = ((rbNode*)((char*)p - offsetof(rbNode, rb_data)));
        trav->rb_generation = tree->rb_generation - 1;
        return *p;
    } else {
        rbIterInit(trav, tree);
        return NULL;
    }
}

/* Initializes trav to have the same current node as src. */
void *rbIterCopy(rbIter *trav, const rbIter *src) {
    assert(trav != NULL && src != NULL);

    if (trav != src) {
        trav->rb_table = src->rb_table;
        trav->rb_node = src->rb_node;
        trav->rb_generation = src->rb_generation;

        if (trav->rb_generation == trav->rb_table->rb_generation) {
            trav->rb_height = src->rb_height;
            memcpy(trav->rb_stack, (const void*)src->rb_stack, 
                sizeof(*trav->rb_stack) * trav->rb_height);
        }
    }
    return trav->rb_node != NULL ? trav->rb_node->rb_data : NULL;
}

/* Refreshes the stack of parent pointers in |trav|
   and updates its generation number. */
static void trav_refresh(rbIter *trav) {
    assert(trav != NULL);

    trav->rb_generation = trav->rb_table->rb_generation;
    if (trav->rb_node != NULL) {
        rb_comparison_func *cmp = trav->rb_table->rb_compare;
        void *param = trav->rb_table->rb_param;
        rbNode *node = trav->rb_node;
        rbNode *i;

        trav->rb_height = 0;
        for (i = trav->rb_table->rb_root; i != node; ) {
            assert(trav->rb_height < RB_MAX_HEIGHT);
            assert(i != NULL);

            trav->rb_stack[trav->rb_height++] = i;
            i = i->rb_link[cmp(node->rb_data, i->rb_data, param) > 0];
        }
    }
}

/* Returns the next data item in inorder
 * within the tree being traversed with trav,
 * or if there are no more data items returns NULL. */
void *rbIterNext(rbIter *trav) {
    rbNode *x;

    assert(trav != NULL);

    if (trav->rb_generation != trav->rb_table->rb_generation)
        trav_refresh(trav);
    
    x = trav->rb_node;
    if (x == NULL) {
        return rbIterFirst(trav, trav->rb_table);
    } else if (x->rb_link[1] != NULL) {
        assert(trav->rb_height < RB_MAX_HEIGHT);
        trav->rb_stack[trav->rb_height++] = x;
        x = x->rb_link[1];

        while (x->rb_link[0] != NULL) {
            assert(trav->rb_height < RB_MAX_HEIGHT);
            trav->rb_stack[trav->rb_height++] = x;
            x = x->rb_link[0];
        }
    } else {
        rbNode *y;

        do {
            if (trav->rb_height == 0) {
                trav->rb_node = NULL;
                return NULL;
            }

            y = x;
            x = trav->rb_stack[--trav->rb_height];
        } while (y == x->rb_link[1]); // y == NULL
    }
    trav->rb_node = x;

    return x->rb_data;
}

/* Returns the previous data item in inorder
   within the tree being traversed with |trav|,
   or if there are no more data items returns |NULL|. */
void *rbIterPrev(rbIter *trav) {
    rbNode *x;

    assert(trav != NULL);

    if (trav->rb_generation != trav->rb_table->rb_generation)
        trav_refresh(trav);
    
    x = trav->rb_node;
    if (x == NULL) {
        return rbIterLast(trav, trav->rb_table);
    } else if (x->rb_link[0] != NULL) {
        assert(trav->rb_height < RB_MAX_HEIGHT);
        trav->rb_stack[trav->rb_height++] = x;
        x = x->rb_link[0];

        while (x->rb_link[1] != NULL) {
            assert(trav->rb_height < RB_MAX_HEIGHT);
            trav->rb_stack[trav->rb_height++] = x;
            x = x->rb_link[1];
        }
    } else {
        rbNode *y;

        do {
            if (trav->rb_height == 0) {
                trav->rb_node = NULL;
                return NULL;
            }

            y = x;
            x = trav->rb_stack[--trav->rb_height];
        } while (y == x->rb_link[0]); // y == NULL
    }
    trav->rb_node = x;

    return x->rb_data;
}

/* Replaces the current item in trav by new and returns the item replaced.
 * trav must not have the null item selected.
 * The new item must not upset the ordering of the tree. */
void *rbIterReplace(rbIter *trav, void *new) {
    void *old;

    assert(trav != NULL && trav->rb_node != NULL && new != NULL);
    old = trav->rb_node->rb_data;
    trav->rb_node->rb_data = new;
    return old;
}