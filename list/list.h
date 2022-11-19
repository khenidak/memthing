#ifndef __LIST__
#define __LIST__

#include <stddef.h>

/*
 * standard double linked list styled after kernel own implementation
 */



struct list_head {
	struct list_head *next, *prev;
};

// inits a new list
void list_head_init(struct list_head *l);

// adds a new entry after another
void list_add_after(struct list_head *current, struct list_head *new);

// adds a new entry before another
void list_add_before(struct list_head *current, struct list_head *new);

// removes an entry
void list_remove_at(struct list_head *current);


#define container_of(ptr, type, member) ({                \
	const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
	(type *)( (char *)__mptr - offsetof(type,member) );})

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_backward(pos, head) \
	for (pos = (head)->prev; pos != (head); pos = pos->prev)

#endif
