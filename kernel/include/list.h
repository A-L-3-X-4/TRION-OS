#pragma once
#include <stdbool.h>
#include <stddef.h>

struct list_node {
    struct list_node *next, *prev;
};

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_for_each(n, head) \
    for (struct list_node *n = (head)->next; n != (head); n = n->next)

static inline void list_init(struct list_node *n)
{
    n->next = n->prev = n;
}

static inline bool list_empty(const struct list_node *head)
{
    return head->next == head;
}

static inline bool list_linked(const struct list_node *n)
{
    return n->next != n;
}

static inline void list_insert_before(struct list_node *pos, struct list_node *n)
{
    n->prev = pos->prev;
    n->next = pos;
    pos->prev->next = n;
    pos->prev = n;
}

static inline void list_add_tail(struct list_node *head, struct list_node *n)
{
    list_insert_before(head, n);
}

static inline void list_del(struct list_node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    list_init(n);
}
