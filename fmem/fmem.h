#ifndef __FMEM__
#define __FMEM__
#include <stdint.h>
#include <stdbool.h>
#include "list/list.h"


// fmem_page is page in typical memory mgmt systems. with obvious few differences. page
// is both the memory, status and accounting of an arbitrary sized memory. the memory the
// page owns is at the end of the page itself. the system starts with one massive page
// (to be clear, two one that holds fmem object, but that is never used). that gets
// split and merged as needed
// -- allocating memory is done by finding page with size > needed. carve a smaller page
// out of it and return it to caller
// this struct should use no padding and is 192 in size representing the entire overhead for a page
struct fmem_page{
	/*
	 * format: high order to low order, only on little endian, we don't do magic around endian
	 * -- highest order 2 bytes magic: used by whoever owns the page, not checked, not validated, only stored. // TODO: is this too big?
	 *  	This *must* first field in the struct because we assume memory corruption will be sombody overshooting
	 *  	into pages.
	 * -- 16th bit status on/off
	 * -- 17th++ bits for future use
	 */
	uint32_t flags; // status and other things to come
	uint32_t size; // that mean max alloc is 2^32 bits - page size
	struct list_head list; // prev, next
};

// a commit range desicrbes a range that needs to be written to the backing
// store it has starting reference and length to write
struct commit_range{
	void *start;
	size_t len;
};

// committer is a function provided by owner of fmem. fmem will call it
// when memory value needs to be presisted (such as in the case of adjusting
// pages, or user triggered). fmem makes the following assumption:
// 1- writes are presisted once the function retrurns.
// 2- if the committer is implemented in a way to support async then
// 		committer must copy commit ranges locally before returning.
// 3- failure in commmits will leave fmem in a broken unoperable state
// 4- any >= 0 return is considered success
typedef int (*committer_t)(struct commit_range*, uint8_t count);

// fmmem is the root object for all fixed memory on which this allocator operate on. it has the lock,
// the accounting data and a reference to our double linked list of pages. and is hidden
// in the first page (aka head). Because our iterators skip head *and* we never allocate
// release it is safe there
struct fmem{
	// ** the entire struct is committed on creation

	// * the follow is only committed with every alloc/free
	size_t total_size;      // total size owned by this fmem
	size_t total_available; // total available (note: includes overhead that will be used on alloc)
	uint32_t alloc_objects; // pages in use
	uint32_t min_alloc;     // minimum unit of allocation

	// * the following can be recommitted on demand using commit_user_data();
	// this where user can stash a root pointer to thier own data
	// we never touch these data. we went with 4 parts data assuming
	// it should cover most usecases without have to get the caller
	// to build thier own index of root pointers.
	void *user1;
	void *user2;
	void *user3;
	void *user4;

	// * the following is never committed (except on creation)
	// * and is set on create from existing
	committer_t committer;  // commit funciton supplied by fmem owner
	// * the following is never committed after ceration
	uint32_t lock;          // spin lock
};

// we can not really operate on less than that
#define MIN_TOTAL_ALLOCATION 3 * sizeof(struct fmem_page) + sizeof(struct fmem) // total minimum size we can operate on
#define E_TOTAL_ALLOCATION_SIZE_TOO_SMALL -1 // error in case we got mem too small
#define E_COMMIT_FAILED -2 // failed to commit memory to backing store
#define DEFAULT_MIN_ALLOC  sizeof(struct fmem_page) // minimum allocation, we try to avoid too many small objects
#define E_BAD_INIT_MEM -2 // mini alloc > total alloc
// the following are possible return values for all the below function
// positive value (mem reference or mem size as applicable)
#define FMEM_E_NOMEM -1 // no more mem to allcate

// unit testing only
#if defined(__UNIT_TESTING__) && defined(__BAD_MEM__)
#define EE_BAD_MEM -2 // to test memory corruption we use this. otherwise the process exist with error logs
#endif

// this is the public interface for memory allocation
// creates an allocator on preallocated memory. The allocator uses the entire length of memory
// if min alloc is less than DEFAULT_MIN_ALLOC we will ddefault to it
// returns E_COMMIT_FAILED
struct fmem* fmem_create_new(void *on_mem, size_t length, uint32_t min_alloc, committer_t committer);

// gets a reference to an existing allocator occupying on_mem memory
// BAD_MEM is tested for this one
struct fmem* fmem_from_existing(void *on_mem, committer_t committer);

// allocates memory, returns reference
// we don't support allocation more than 2^32- PAGE_OVERHEAD
// returns E_COMMIT_FAILED if commit failed
void* fmem_alloc(struct fmem *fm, uint32_t size);

// frees a memory and returns total freed memory (includes page overhead, which will also be returned to pool)
// BAD_MEM is tested for this one (this is why we have such a wide return data type).
// returns E_COMMIT_FAILED if commit failed
int64_t fmem_free(struct fmem *fm, void *mem);

//commits user set root pointers to backing store
// returns E_COMMIT_FAILED if commit failed
// BAD_MEM is tested here
int64_t fmem_commit_user_data(struct fmem *fm);

// commits a memory that was allocated from that fmem into backing store
// returns E_COMMIT_FAILED if commit fails
// returns E_COMMIT_FAILED if mem is outside the allocated range
// len == 0 means the entire allocation used for this mem
// start to len must fit within the original allocation
// that was used to allocate this memory
// BAD_MEM is tested here
int64_t fmem_commit_mem(struct fmem *fm, void *mem, uint32_t len); // TODO
#endif
