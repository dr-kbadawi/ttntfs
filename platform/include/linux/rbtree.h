/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2026 TechTag GmbH */
#ifndef _LINUX_RBTREE_H
#define _LINUX_RBTREE_H
#include <linux/types.h>
#include <linux/kernel.h>
#define rb_parent(r) ((struct rb_node *)((r)->__rb_parent_color & ~3))
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define RB_EMPTY_ROOT(root) ((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node) ((node)->__rb_parent_color == (unsigned long)(node))
#define RB_CLEAR_NODE(node) ((node)->__rb_parent_color = (unsigned long)(node))
void rb_insert_color(struct rb_node *, struct rb_root *);
void rb_erase(struct rb_node *, struct rb_root *);
struct rb_node *rb_next(const struct rb_node *);
struct rb_node *rb_prev(const struct rb_node *);
struct rb_node *rb_first(const struct rb_root *);
struct rb_node *rb_last(const struct rb_root *);
void rb_replace_node(struct rb_node *victim, struct rb_node *new_, struct rb_root *root);
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **rb_link)
{ node->__rb_parent_color = (unsigned long)parent; node->rb_left = node->rb_right = NULL; *rb_link = node; }
#define rb_entry_safe(ptr, type, member) ({ __typeof__(ptr) ____ptr = (ptr); ____ptr ? rb_entry(____ptr, type, member) : NULL; })
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
	for (pos = rb_entry_safe(rb_first_postorder(root), __typeof__(*pos), field); \
	     pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field), __typeof__(*pos), field); 1; }); pos = n)
struct rb_node *rb_first_postorder(const struct rb_root *);
struct rb_node *rb_next_postorder(const struct rb_node *);
#endif
