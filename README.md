## What is this?

mem thing is a memory allocator that sets on top of a fixed sized memory allocated by the caller. It is designed for apps that uses configuration or control-plan data and would want to stash it in a shared memory map that outlives the processes that either produce, consume, or both this data. It can do:
1. Fast - in most cases - arbitrary size memory allocation.
2. Cached memory allocation. Memory is allocated once by caller. Freed memory is cached and is not returned to system. This is not designed as a buddy allocator though it shares some properties with it.
3. Ability to commit memory segments. For applications that needs to cache this data to survive system reboots they will have to persist this data on disk. The allocator has automatic persistence for its own internal data and offers an external `commit` interface for callers. The commit interface is a function provided by caller.
4. Automatic detection of memory corruptions (build using `__BAD_MEM__` macro) via memory poisoning.
5. The allocator allocates in o(1) in most cases. However if the entire memory was allocated then arbitrary freed max allocation time is o(n) where n is the number of objects allocated.

## Examples Provided
1. An allocator that sits on top of a shared memory object mapped into proc memory. The example uses no persistence run `make example-things-mem`.
2. An allocator that sits on a memory mapped (with file backing) the application provides its own persistence func to commit memory via `msync(2)` calls run `make example-things-mem-persisted`

> unit test code should provide enough details on the ins and outs of the fixed memory allocator.

## On Memory Moves
The allocator does not support memory moves. Memory moves is a situation where an application creates a memory map at address `xyz` then restarts, then remap the same memory to a different address `xxz` essentially invalidating all the pointers in any in memory object graph including allocator own. For now we assume that apps will always remap at the same address. If and only if we find that apps are unable to always do that then we can find a solution but that will require offering `adjust(..)` interface for caller to fix the memory shift that has happened. For that reason we always used fixed maps in our examples


## Building & Testing
The code is deliberately heavy on unit testing (we use `munit` https://nemequ.github.io/munit/ - Thanks!). You can run `make unit-tests` to go through all tests. Additional make targets exist for selective module specific unit tests.


## Basis Usecase

```C
struct some {
  // some user specific data
};


void *m = // {memory mapped location or memory mapped with file backing}
size m_length = <whatever>;

// create the fixed allocator
struct fmem* fm = fmem_create_new(m, m_length, 0 /* defaults each allocation to whatever allocator have */ );

if (fm < 0) /* error */ all our errors are returned as negative values

// start allocating

void * my_mem = fmem_alloc(fm, sizeof(struct some));
if(m <0) /* error */

struct some *s = (struct some) my_mem;

// if s is the root index to the object graph then i can store it
fm->user1 = s;


/* process restarts and comes back with memory mapped at the same location

struct fmem *fm = fmem_from_existing(m); // if mem checks enabled and memory wsa corrupted the entire process will exit.
// get reference to our stashed index
struct some *s = (struct some *) fm->user1;

// memory can be freed by
fmem_free(fm, my_mem);

```


