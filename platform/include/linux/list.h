/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_LIST_H
#define _LINUX_LIST_H

#include <linux/types.h>
#include <linux/kernel.h>

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) { list->next = list; list->prev = list; }

static inline void __list_add(struct list_head *n, struct list_head *prev, struct list_head *next)
{
	next->prev = n; n->next = next; n->prev = prev; prev->next = n;
}
static inline void list_add(struct list_head *n, struct list_head *head) { __list_add(n, head, head->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *head) { __list_add(n, head->prev, head); }
static inline void __list_del(struct list_head *prev, struct list_head *next) { next->prev = prev; prev->next = next; }
static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = (struct list_head *)0x100; entry->prev = (struct list_head *)0x122;
}
static inline void list_del_init(struct list_head *entry) { __list_del(entry->prev, entry->next); INIT_LIST_HEAD(entry); }
static inline void list_move(struct list_head *l, struct list_head *head) { __list_del(l->prev, l->next); list_add(l, head); }
static inline void list_move_tail(struct list_head *l, struct list_head *head) { __list_del(l->prev, l->next); list_add_tail(l, head); }
static inline int list_empty(const struct list_head *head) { return head->next == head; }
static inline int list_is_last(const struct list_head *l, const struct list_head *head) { return l->next == head; }
static inline int list_is_first(const struct list_head *l, const struct list_head *head) { return l->prev == head; }
static inline void list_replace(struct list_head *old, struct list_head *n)
{
	n->next = old->next; n->next->prev = n; n->prev = old->prev; n->prev->next = n;
}
static inline void list_replace_init(struct list_head *old, struct list_head *n) { list_replace(old, n); INIT_LIST_HEAD(old); }
static inline void list_splice(const struct list_head *list, struct list_head *head)
{
	if (list_empty(list)) return;
	struct list_head *first = list->next, *last = list->prev, *at = head->next;
	first->prev = head; head->next = first; last->next = at; at->prev = last;
}
static inline void list_splice_init(struct list_head *list, struct list_head *head) { list_splice(list, head); INIT_LIST_HEAD(list); }
static inline void list_splice_tail(const struct list_head *list, struct list_head *head)
{
	if (list_empty(list)) return;
	struct list_head *first = list->next, *last = list->prev, *at = head->prev;
	at->next = first; first->prev = at; last->next = head; head->prev = last;
}
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head) { list_splice_tail(list, head); INIT_LIST_HEAD(list); }

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_first_entry_or_null(ptr, type, member) ({ struct list_head *__h = (ptr); __h->next != __h ? list_entry(__h->next, type, member) : NULL; })
#define list_next_entry(pos, member) list_entry((pos)->member.next, __typeof__(*(pos)), member)
#define list_prev_entry(pos, member) list_entry((pos)->member.prev, __typeof__(*(pos)), member)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) \
	for (pos = list_first_entry(head, __typeof__(*pos), member); &pos->member != (head); pos = list_next_entry(pos, member))
#define list_for_each_entry_reverse(pos, head, member) \
	for (pos = list_last_entry(head, __typeof__(*pos), member); &pos->member != (head); pos = list_prev_entry(pos, member))
#define list_for_each_entry_safe(pos, n, head, member) \
	for (pos = list_first_entry(head, __typeof__(*pos), member), n = list_next_entry(pos, member); \
	     &pos->member != (head); pos = n, n = list_next_entry(n, member))
#define list_entry_is_head(pos, head, member) (&pos->member == (head))

/* hlist */
#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = { .first = NULL }
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *h) { h->next = NULL; h->pprev = NULL; }
static inline int hlist_unhashed(const struct hlist_node *h) { return !h->pprev; }
static inline int hlist_empty(const struct hlist_head *h) { return !h->first; }
static inline void hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next, **pprev = n->pprev;
	*pprev = next;
	if (next) next->pprev = pprev;
	n->next = (struct hlist_node *)0x100; n->pprev = (struct hlist_node **)0x122;
}
static inline void hlist_del_init(struct hlist_node *n) { if (!hlist_unhashed(n)) { hlist_del(n); INIT_HLIST_NODE(n); } }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;
	n->next = first;
	if (first) first->pprev = &n->next;
	h->first = n; n->pprev = &h->first;
}
#define hlist_entry(ptr, type, member) container_of(ptr, type, member)
#define hlist_entry_safe(ptr, type, member) ({ __typeof__(ptr) ____ptr = (ptr); ____ptr ? hlist_entry(____ptr, type, member) : NULL; })
#define hlist_for_each(pos, head) for (pos = (head)->first; pos; pos = pos->next)
#define hlist_for_each_safe(pos, n, head) for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)
#define hlist_for_each_entry(pos, head, member) \
	for (pos = hlist_entry_safe((head)->first, __typeof__(*(pos)), member); pos; pos = hlist_entry_safe((pos)->member.next, __typeof__(*(pos)), member))
#define hlist_for_each_entry_safe(pos, n, head, member) \
	for (pos = hlist_entry_safe((head)->first, __typeof__(*pos), member); pos && ({ n = pos->member.next; 1; }); pos = hlist_entry_safe(n, __typeof__(*pos), member))

#endif /* _LINUX_LIST_H */
