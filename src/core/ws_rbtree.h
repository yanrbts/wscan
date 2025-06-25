/*
 * Copyright (c) 2024-2024, yanruibinghxu@gmail.com All rights reserved.
 * libavl - library for manipulation of binary trees.
 *  Copyright (C) 1998, 1999, 2000, 2001, 2002, 2004 Free Software
 *  Foundation, Inc.
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
#ifndef __RBTREE_H__
#define __RBTREE_H__

#include <stddef.h>

/* Function types. */
typedef int rb_comparison_func(const void *rb_a, const void *rb_b, void *rb_param);
typedef void rb_item_func(void *rb_item, void *rb_param);
typedef void *rb_copy_func(void *rb_item, void *rb_param);

/* Maximum RB height. */
#ifndef RB_MAX_HEIGHT
#define RB_MAX_HEIGHT 128
#endif

/* Tree data structure. */
typedef struct rbTable {
    struct rbNode *rb_root;            /* Tree's root. */
    rb_comparison_func *rb_compare;    /* Comparison function. */
    void *rb_param;                    /* Extra argument to |rb_compare|. */
    size_t rb_count;                   /* Number of items in tree. */
    unsigned long rb_generation;       /* Generation number. */
} rbTable;

/* Color of a red-black node. */
enum rbColor {
    RB_BLACK,   /* Black. */
    RB_RED      /* Red. */
};

/* A red-black tree node. */
typedef struct rbNode {
    struct rbNode *rb_link[2];  /* Subtrees. */
    void *rb_data;              /* Pointer to data. */
    unsigned char rb_color;     /* Color. */
} rbNode;

typedef struct rbIter {
    rbTable *rb_table;                  /* Tree being traversed. */
    rbNode *rb_node;                    /* Current node in tree. */
    rbNode *rb_stack[RB_MAX_HEIGHT];
                                        /* All the nodes above |rb_node|. */
    size_t rb_height;                   /* Number of nodes in |rb_parent|. */
    unsigned long rb_generation;        /* Generation number. */
} rbIter;

/* Table functions. */
rbTable *rbCreate(rb_comparison_func *compare, void *param);
void rbDestroy(rbTable *tree, rb_item_func *destroy);
void **rbProbe(rbTable *tree, void *item);
void *rbInsert(rbTable *tree, void *item);
void *rbReplace(rbTable *tree, void *item);
void *rbDelete(rbTable *tree, const void *item);
void *rbFind(const rbTable *tree, const void *item);
/* Table traverser functions. */
void rbIterInit(rbIter *trav, rbTable *tree);
void *rbIterFirst(rbIter *trav, rbTable *tree);
void *rbIterLast(rbIter *trav, rbTable *tree);
void *rbIterFind(rbIter *trav, rbTable *tree, void *item);
void *rbIterInsert(rbIter *trav, rbTable *tree, void *item);
void *rbIterCopy(rbIter *trav, const rbIter *src);
void *rbIterNext(rbIter *trav);
void *rbIterPrev(rbIter *trav);
void *rbIterReplace(rbIter *trav, void *new);

#define rbCount(tree) ((size_t)(tree)->rb_count)
#define rbCurData(it) ((it)->rb_node != NULL ? (it)->rb_node->rb_data : NULL)
#define rbGetColor(node) ((node)->rb_color)

#endif