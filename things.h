#ifndef __THINGS__
#define __THINGS__

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* things is a list data structure with a simple _value_ field
 * used to compare mems.
 */
#include "list/list.h"


// alloc is a function used by things maker to allcoate memory
typedef void* (*alloc_t)(size_t size);

// oneach is called when the things maker allocates and creates an object
// it is called for header (things)  and each (thing)
typedef void (*oneach_t)(void *each, size_t len);

// a list of things
struct things{
  struct list_head list;
   uint8_t count;
};

// a thing
struct thing{
   struct list_head list;
  char value;
};


// makes a well known list of things using an allocator
// returns total memory allocated
int make_wellknown_things(void **where, alloc_t allocator, oneach_t oneach){
  int allocated = 0;
  if (allocator == NULL) return -1; // we can not work without an allocator
  // alloc header
  struct things *header = (struct things *) allocator(sizeof(struct things));
  if(header <= 0) {
    printf("allocator failed to create memory for header\n");
    return -1;
  }

  memset(header, 0, sizeof(struct things));
  *where = header;

  allocated += sizeof(struct things);

  list_head_init(&header->list);

  // list of capital letters A->Z
  for(int i = 90; i > 64; i--){
     struct thing *this_thing = (struct thing *) allocator(sizeof(struct thing));

     if(this_thing <= 0) {
      printf("allocator failed to create memory for thing:%c\n", i);
      return -1;
    }

    memset(this_thing, 0, sizeof(struct thing));

    allocated += sizeof(struct thing);
    this_thing->value = i;
    list_add_after(&header->list, &this_thing->list);
    header->count++;

    if(oneach != NULL){
      oneach(header, sizeof(struct things)); // header changes every time.
      oneach(this_thing, sizeof(struct thing));
    }
  }

  return allocated;
}

// compares a list of things to a well known thing
int verify_things(struct things *what){
	if(what == NULL) return -1;

	// create a copy to compare to
	void *_to = NULL;
	if (make_wellknown_things(&_to, malloc, NULL) <= 0) return -1;

	struct things *to = ( struct things *) _to;

	if(to->count != what->count){
		printf("expected count %d != %d\n", to->count, what->count);
		return -1;
	}

	// remember iterator does not go over head
	struct list_head *head = &what->list;
	struct list_head *current = head;
	struct list_head *to_current = what->list.next;

	int i = 0;
	list_for_each(current, head){
		struct thing *current_thing = list_entry(current, struct thing, list);
		struct thing *to_current_thing = list_entry(to_current, struct thing, list);
		if(current_thing->value != to_current_thing->value){
			printf("at %d expected value %c != %c\n", i, current_thing->value, to_current_thing->value);
			return -1;
		}
		 printf("at %d value %c == %c\n", i, current_thing->value, to_current_thing->value);

		to_current = to_current->next; // we are comparing two lists
		i++;
	}
	return 0;
}
#endif
