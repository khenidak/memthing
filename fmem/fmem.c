#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>


#include "list/list.h"
#include "fmem.h"


// minimum overhead that each page carry
#define PAGE_OVERHEAD sizeof(struct fmem_page)
// if after we carve out mem, the remining is >= this minimum
// then the entire page is used and the free space is sacrified
// this ensure that we don't end up with unusable pages
#define PAGE_REMAIN_FREE 2 * sizeof(struct fmem_page) // if you change that check the fit tests below.


#define CAN_NOT_FIT    0 // this page can not fit the needed size
#define FIT_AS_IS      1 // this page can fit but without split, the entire page will be used
#define FIT_WITH_CARVE 2 // this page can fit with enough space to split into a new one

#define POISON 0xBEEF // when __BAD_MEM__ is defined we will use this value to check for corruption
// returns actual free size in byes
static inline uint32_t fpage_actual(struct fmem_page *fpage){
  return fpage->size - PAGE_OVERHEAD;
}

// given a page (WE DON'T CHECK IF IT IS FREE) and an arbitrary
// needed size to use the page, the result is
// CAN_NOT_FIT: page free size is less that what is needed
// FIT_AS_IS: page free is =< (needed + min remain free)
// FIT_WITH_CARVE: page free > (needed + min remain free)
static inline int fpage_can_fit(struct fmem_page *fpage, uint32_t size_needed){
  uint32_t actual = fpage_actual(fpage);
  // can't fit
  if(size_needed > actual) return CAN_NOT_FIT;
  // fit as is, the remining is too small to be used in a different page
  if(size_needed + PAGE_REMAIN_FREE >= actual) return FIT_AS_IS;
  // it is a big page, split it
  return FIT_WITH_CARVE;
}

// given fmem page (fpage) of arbitrary size, carve out a new page (created)
// and adjust sizes accordingly. The function does not perform size nor
// fit checks, it goes straight to carving logic. we always add overhead
// to the carve needed
static void fpage_carve(struct fmem_page *fpage, struct fmem_page **created, uint32_t to_carve){
  uint32_t actual_needed = to_carve + PAGE_OVERHEAD;
  // set new size on old page
  fpage->size = fpage->size - actual_needed;

  char * start = (char *) fpage;
  *created = (struct fmem_page *) (start + fpage->size);
  memset(*created, 0, sizeof(struct fmem_page)); // we let the user reset the memory if the want
  // set the new page
  (*created)->size = actual_needed;
  list_add_after(&fpage->list, &(*created)->list);
}

// a magic number is a 2 bytes wide arbitrary value that can
// be set by the owner of the page. in our system we are using them
// as poison checked TODO (if configured) TODO on allocation on release
// failure to match will result in a panic since it means something messed
// with the memory layout
static inline void fpage_set_magic(struct fmem_page *fpage, uint16_t magic){
  const uint32_t low_mask = 0x0000FFFF;
  fpage->flags &= low_mask; // clear
  fpage->flags |= ((uint32_t) magic) << 16;
}

static inline uint16_t fpage_get_magic(struct fmem_page *fpage){
  return fpage->flags >> 16;
}

// busy flag is the 17th high bit order
static inline bool fpage_is_free(struct fmem_page *fpage){
  const uint32_t mask =  1 << 15;
  return ((fpage->flags & mask) > 0) ? false : true;
}

static inline void fpage_set_busy(struct fmem_page *fpage){
  const uint32_t mask = 1 << 15;
  fpage->flags |= mask;
}

static inline void fpage_set_free(struct fmem_page *fpage){
  const uint32_t mask = ~(1 << 15);
  fpage->flags &= mask;
}

// merges the page with prev, next pages where possible
// we merge pages to:
// 1- minimize the # of iteration needed to find a free page
// 2- create holes of free mem big enough to accomdate large allocs
static struct fmem_page* fpage_merge(struct fmem_page *fpage){
// there are three possibilities:
// 1- prev and next are free (best case) merge all
// 2- prev is free merge current into previous
// 3- next is free merge next into current
  struct fmem_page *prev = list_entry(fpage->list.prev, struct fmem_page, list);
  struct fmem_page *next = list_entry(fpage->list.next, struct fmem_page, list);

  bool prev_is_same = (prev == fpage);
  bool next_is_same = (next == fpage);

  // merge current and next into prev
  if((!prev_is_same && fpage_is_free(prev)) && (!next_is_same && fpage_is_free(next))){
    prev->size = prev->size + fpage->size + next->size; // give all size to first one
    list_remove_at(&next->list);
    list_remove_at(&fpage->list);
    return prev;
  }

  // merge current into previous
  if(!prev_is_same && fpage_is_free(prev)){
    prev->size = prev->size + fpage->size;
    list_remove_at(&fpage->list);
    return prev;
  }

  // merge next into current
  if(!next_is_same && fpage_is_free(next)){
    fpage->size = fpage->size + next->size;
    list_remove_at(&next->list);
    return fpage;
  }

  // no dice. both prev, next are busy
  return fpage;
}

// returns a refrence to memory owned by a page
static inline void* mem_from_fpage(struct fmem_page *fpage){
  return (void *) ( ((char *) fpage) + PAGE_OVERHEAD);
}
// returns the page owned by a memory reference
static inline struct fmem_page* fpage_from_mem(void *mem){
  return (struct fmem_page *) ( ((char *) mem) - PAGE_OVERHEAD);
}


static inline bool atomic_compare_swap(uint32_t * ptr, uint32_t compare, uint32_t exchange) {
  return __atomic_compare_exchange_n(ptr, &compare, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// spin lock style locking mechnasim, we have only one lock
// we are trying to minimize deps so we are not trying to link
// to pthread
static void inline fmem_lock(struct fmem *fm){
   while(!atomic_compare_swap(&fm->lock, 0, 1)) {
            }
}


static void inline fmem_unlock(struct fmem *fm){
   __atomic_store_n(&fm->lock, 0, __ATOMIC_SEQ_CST);
}

static inline int64_t fail_on_poison_check(uint16_t poison, uint16_t expected, char* message){
#if defined(__UNIT_TESTING__) && defined(__BAD_MEM__)
  // used only during unit testing
  if(poison != expected){
    return EE_BAD_MEM;
  }
#endif

#if !defined(__UNIT_TESTING__) && defined(__BAD_MEM__)
  if if(poison != expected){
    // runtime mode. we need to exit. This should - hopefully - be detected in integration testing
    printf("******************* WARNING! WARNING!*********************");
    printf("memory corruption detected on: %s", message);
    printf("**********************************************************");
    exit(2);
  }
#endif

  return 0;
}

// creates an allocator on preallocated memory. The allocator uses the entire length of memory
struct fmem* fmem_create_new(void * on_mem, size_t length, uint32_t min_alloc, committer_t committer){
  if(length < MIN_TOTAL_ALLOCATION) return (void *) E_TOTAL_ALLOCATION_SIZE_TOO_SMALL;
  if (length < (min_alloc + (2 * PAGE_OVERHEAD) + sizeof(struct fmem))) return  (void *) E_BAD_INIT_MEM; // we can't really use it
  if (min_alloc < DEFAULT_MIN_ALLOC) min_alloc = DEFAULT_MIN_ALLOC;
  // TODO check that total size + overhead is > min alloc

  // we are doing this manually here, but other
  // allocs are done automatically
  struct fmem_page *fpage_head = (struct fmem_page *) on_mem; //first create a page, that will become our head
  fpage_head->size = (PAGE_OVERHEAD /*page header*/ + sizeof(struct fmem) /*where we stash fmem object*/);
  list_head_init(&fpage_head->list); // init the list

  // init acocunting object
  struct fmem *fm = mem_from_fpage(fpage_head); // create our main accounting object, stashed in the headerpage
  fm->total_size = length; // total is all
  fm->total_available = length - ((2 * PAGE_OVERHEAD) + sizeof(struct fmem)); // but we used two headers and main accounting object
  fm->min_alloc = min_alloc; // set the min alloc we operate on
  fm->alloc_objects = 0;
	if (committer != NULL){
		fm->committer = committer;
	}

	/* for rare cases where fmem left in locked state*/
	fmem_unlock(fm);

  // create a second page (this is first empty massive page
  char * start = (char *) fpage_head;
  struct fmem_page *main_fpage = (struct fmem_page *) (start + fpage_head->size);
  main_fpage->size = length - fpage_head->size; // assign the remaining to it
  list_add_after(&fpage_head->list, &main_fpage->list); // link pages


  fpage_set_busy(fpage_head); // set head page as busy
  fpage_set_free(main_fpage); // set main page as free
#ifdef __BAD_MEM__
  fpage_set_magic(fpage_head, POISON);
  fpage_set_magic(main_fpage, POISON);
#endif

	// commit if needed
	if(fm->committer != NULL){
		struct commit_range r = {0};
		r.start = on_mem;
		r.len = PAGE_OVERHEAD + sizeof(struct fmem) + PAGE_OVERHEAD;
		if (fm->committer(&r, 1) < 0) return (struct fmem *) E_COMMIT_FAILED;
	}

  return fm;
}

// gets a reference to an existing allocator occupying on_mem memory
struct fmem* fmem_from_existing(void * on_mem, committer_t committer){
  // our accounting object is stashed in head page
  struct fmem_page *fpage_head = (struct fmem_page *) on_mem;

  int64_t check = fail_on_poison_check(fpage_get_magic(fpage_head), POISON, "reloading existing allocator (head page)");
  if( check != 0) return  ((struct fmem *) check);


  struct fmem *fm = (struct fmem *) mem_from_fpage(fpage_head);
	if (committer != NULL){
		fm->committer = committer;
	}

	fmem_unlock(fm); // this could be potentially dangerous

	// we don't try to commit here. because one of the following is true
	// this was commited before and create_from_new does not change anything so there is no need to commit again
	// this was not committed before and commit is just used, in this case we will save the header anyway
	// on first alloc/free ops

  return fm;
}

// allocates memory from fmem
void* fmem_alloc(struct fmem *fm, uint32_t size){
  void* ret = NULL;
  struct fmem_page *selected = NULL;
  uint32_t adjusted_alloc = size < fm->min_alloc ? fm->min_alloc : size;

  fmem_lock(fm);
  if (fm->total_available < adjusted_alloc) {
          goto done;
    }

  // carving pages always carve towards the end of the page
  // meaning: our free pages are always closer(on the right) to head. this result into:
  // 1- for initial allocs (until we fill up) they will be at o(1)
  // 2- worest alloc (if we fill up and then release the last alloc) then it will be o(n)
  // hence size of alloc matter. Larger is better. Larger yeilds into lower page count
  //  we can get clever by maintain a cache of free pages but that will be later on
  // get head page
  struct fmem_page *head_page = fpage_from_mem(fm); // this the page that holds our accounting object. We don't iterate over it
  struct list_head *head = &head_page->list;
  struct list_head *current = head;
	bool carved = false;
  list_for_each(current, head){
    struct fmem_page *this_page = list_entry(current, struct fmem_page, list);

    // check for corruption
    int64_t check = fail_on_poison_check(fpage_get_magic(this_page), POISON, "iterating and checking mem pages");
    if( check != 0){
           ret = (void *) check;
           goto done;
    }

    if (false == fpage_is_free(this_page)) continue; // can't work with a page in use
    int fit_status = fpage_can_fit(this_page, adjusted_alloc);
    switch(fit_status){
      case CAN_NOT_FIT:
        break;
      case FIT_AS_IS:
        selected = this_page;
				carved = false;
        goto done;
      case FIT_WITH_CARVE: // we need to carve this page
        fpage_carve(this_page, &selected, adjusted_alloc);// carve it
				carved = true;
        goto done;
    }
  }

done:
  // at this point we don't care if it was selected as is or carved
  if(selected != NULL){
    fpage_set_busy(selected); // mark as busy
#ifdef __BAD_MEM__
    fpage_set_magic(selected, POISON);
#endif
    // do the accounting
    fm->total_available -= selected->size;
    fm->alloc_objects += 1;
    ret = mem_from_fpage(selected);

		if(fm->committer != NULL){
			if(carved){
				// we need to save
				// selected page header
				// next page list
				// previous page header
				struct commit_range ranges[3] = {0};
				ranges[0].start = selected;
				ranges[0].len = sizeof(struct fmem_page);

				struct fmem_page *prev = list_entry(selected->list.prev, struct fmem_page, list);
				ranges[1].start = prev;
				ranges[1].len = sizeof(struct fmem_page);

				struct list_head *next = selected->list.next;
				ranges[2].start = next;
				ranges[2].len = sizeof(struct list_head);
				if( fm->committer(ranges, 1) < 0) ret = (void *) E_COMMIT_FAILED;
			}else{
				//if as is then we just need to commit current page header
				struct commit_range range = {0};
				range.start = selected;
				range.len = sizeof(struct fmem_page);
				if( fm->committer(&range, 1) < 0) ret = (void *) E_COMMIT_FAILED;
			}
		}
  }

  fmem_unlock(fm);
  return ret == NULL ? (void *)FMEM_E_NOMEM : ret;
}

// frees a memory and returns total freed memory (includes page overhead, which will also be returned to pool)
// BAD_MEM is tested for this one
int64_t fmem_free(struct fmem *fm, void *mem){
  struct fmem_page *fpage = fpage_from_mem(mem); // get the page for that mem. page is always stashed before the mem

  // POISON CHECK
  int64_t check = fail_on_poison_check(fpage_get_magic(fpage), POISON, "reloading existing allocator");
  if( check != 0) return check;


  fmem_lock(fm);

  int64_t to_free = (int64_t) fpage->size; // keep the size aside
  fpage_set_free(fpage); // free it
  struct fmem_page *modified = fpage_merge(fpage); // merge, if we can
  // accountig
  fm->alloc_objects -= 1;
  fm->total_available += to_free;

	if(fm->committer != NULL){
		// commit
		// header for modified
		// header for previous (we only need the list pointers)
		// header for next (we only need the list pointers)
		// we can get clever here but will keep it simple for now
		struct commit_range ranges[3] = {0};
  	struct list_head *prev = fpage->list.prev;
  	struct list_head *next = fpage->list.next;

		ranges[0].start = modified;
		ranges[0].len = sizeof(struct fmem_page);

		// prev
		ranges[1].start = prev;
		ranges[1].len = sizeof(struct list_head);

		// next
		ranges[2].start = next;
		ranges[2].len =sizeof(struct list_head);

		 if(fm->committer(ranges, 3) < 0)  {
				to_free = E_COMMIT_FAILED;
		 }
	}

  fmem_unlock(fm);
  return to_free;
}

int64_t fmem_commit_user_data(struct fmem *fm){
	if(fm->committer == NULL) return E_COMMIT_FAILED;

  // POISON CHECK
	struct fmem_page *fhead_page = fpage_from_mem(fm);
  int64_t check = fail_on_poison_check(fpage_get_magic(fhead_page), POISON, "committing user data");
  if( check != 0) return check;

	// we don't need to lock here
	struct commit_range r = {0};
	r.start = (void *) &fm->user1;
	r.len = 4 * sizeof(fm->user1);
	if (fm->committer(&r, 1) < 0) return E_COMMIT_FAILED;

	return r.len;
}

int64_t fmem_commit_mem(struct fmem *fm, void *mem, uint32_t len){
	 if(fm->committer == NULL) return E_COMMIT_FAILED;

	// should we lock here?
	struct fmem_page *fpage = fpage_from_mem(mem);
  // POISON CHECK
  int64_t check = fail_on_poison_check(fpage_get_magic(fpage), POISON, "committing user memory");
  if( check != 0) return check;


	uint32_t page_size = fpage_actual(fpage);
	if (len == 0){
		len = page_size; // if no len then
	}
	// weather len is supplied or not, it must fit page boundries
	if ( ((char *) mem + len) > ((char *) fpage + PAGE_OVERHEAD +  page_size)) return E_COMMIT_FAILED;


	struct commit_range r = {0};
	r.start = mem;
	r.len = len;
	if (fm->committer(&r,1) < 0) return E_COMMIT_FAILED;

	return len;
}

#ifdef __UNIT_TESTING__
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "munit/munit.h"

#define small_buffer_size 10
#define large_buffer_size 50 * 1024

// test committer saves data here
#define MAX_COMMIT  3
struct commit_range  __test_ranges[MAX_COMMIT] = {0};
int committed_range_count = 0;

// resets committed values
static void reset_test_committer(){
	committed_range_count = 0;
}
static int failed_test_committer(struct commit_range *ranges, uint8_t count){
	return -1;
}
// a test committer
static int test_committer(struct commit_range *ranges, uint8_t count){
	if (count == 0){
			printf("TEST FAIL: committer got 0 count!\n");
			return -1;
	}

	if (committed_range_count+ count > MAX_COMMIT){
		printf("TEST FAIL: COMMITTER has %d and want additional %d which put it higher than max:%d\n",
						committed_range_count, count, MAX_COMMIT);
		return -1;
	}
	memcpy(__test_ranges + committed_range_count, ranges, count * sizeof(struct commit_range));
	committed_range_count += count;
	return 0;
}

// compares to whatever committer saved
static MunitResult test_committer_compare_to(struct commit_range *expected, uint8_t count, bool count_only){
	if(committed_range_count != count){
		munit_logf(MUNIT_LOG_INFO, "got:%d expected:%d",committed_range_count, count);
		return MUNIT_FAIL;
	}

	if (count_only) return MUNIT_OK;

	for(int pos = 0; pos < committed_range_count; pos++){
			struct commit_range *expected_current = expected + pos;
			struct commit_range *saved_current = &__test_ranges[pos];

			munit_logf(MUNIT_LOG_INFO, "at: %d", pos);
			munit_assert(expected_current->start == saved_current->start);


			munit_logf(MUNIT_LOG_INFO, "expected len:%lu: got:%lu",saved_current->len,  expected_current->len );
			munit_assert(expected_current->len == saved_current->len);
	}
	return MUNIT_OK;
}

// handy helper to make  apage
static void make_fpage(struct fmem_page *fpage, uint32_t size){
  // reset all
  memset(fpage, 0, size);
  fpage->size = size;

  list_head_init(&fpage->list);
}

// helper counter
static int count_pages(struct fmem_page *fpage){
  struct list_head *head = &fpage->list;
  struct list_head *current = head;
  int count = 0;

  list_for_each(current, head){
    count++;
  }
  // because our iterators does not like working with head
  return count+1;
}

static MunitResult test_fmem_commit(const MunitParameter params[], void* data){

	char buffer[large_buffer_size] = {0};
	struct commit_range compare_ranges[3] = {0};

	// CASE: create new with bad committer
	reset_test_committer();
  struct fmem *bad_committer_fm = fmem_create_new(buffer, large_buffer_size, 0, failed_test_committer);
  munit_assert(bad_committer_fm == (struct fmem *) E_COMMIT_FAILED);

	// CASE: create new with good one
	reset_test_committer();
	struct fmem *fm = fmem_create_new(buffer, large_buffer_size, 0, test_committer);
	// create compare
	compare_ranges[0].start = (void *) buffer;
	compare_ranges[0].len = PAGE_OVERHEAD + sizeof(struct fmem) + PAGE_OVERHEAD;
	MunitResult compare_res = test_committer_compare_to(compare_ranges, 1, false);
	if (compare_res != MUNIT_OK) return compare_res;

	// CASE: commit user data with bad committer
	reset_test_committer();
	fm->committer = failed_test_committer;
	munit_assert(fmem_commit_user_data(fm) ==  E_COMMIT_FAILED);

	// CASE: commit user data with good committer
	reset_test_committer();
	fm->committer = test_committer;
	munit_assert(fmem_commit_user_data(fm) > 0 );
	compare_ranges[0].start = &fm->user1;
	compare_ranges[0].len = 4 * sizeof(fm->user1);
	compare_res = test_committer_compare_to(compare_ranges, 1, false);
	if (compare_res != MUNIT_OK) return compare_res;


	// CASE: alloc with bad committer
	reset_test_committer();
	fm->committer = failed_test_committer;
	munit_assert(fmem_alloc(fm, 10) ==  (void *) E_COMMIT_FAILED);
	// TODO: alloc with good committer (for both cases as-is or carving)


	// CASE: commit user data with bad committer
	reset_test_committer();
	fm->committer = test_committer;
	void * alloc1 = fmem_alloc(fm, 10);
	munit_assert(alloc1 > 0);
	fm->committer = failed_test_committer;
	munit_assert(fmem_commit_mem(fm, alloc1, 0) == E_COMMIT_FAILED);

	// CASE: commit user data with good committer
	reset_test_committer();
	fm->committer = test_committer;
	void * alloc2 = fmem_alloc(fm, 10); // this allocates min allocation
	munit_assert(alloc2 > 0);
	reset_test_committer();
	munit_assert(fmem_commit_mem(fm, alloc2, 0) > 0);

	compare_ranges[0].start = alloc2;
	compare_ranges[0].len = 24; /* min defaulted allocation) */
	compare_res = test_committer_compare_to(compare_ranges, 1, false);
	if (compare_res != MUNIT_OK) return compare_res;


	// CASE: free with bad committer
	reset_test_committer();
	fm->committer = test_committer;
	void * alloc3 = fmem_alloc(fm, 10); // this allocates min allocation
	munit_assert(alloc3 > 0);
	reset_test_committer();
	fm->committer = failed_test_committer;
	munit_assert(fmem_free(fm, alloc3) == E_COMMIT_FAILED);


	// case: free with good committer
	reset_test_committer();
	fm->committer = test_committer;
	void * alloc4 = fmem_alloc(fm, 10); // this allocates min allocation
	munit_assert(alloc4 > 0);
	reset_test_committer();
	munit_assert(fmem_free(fm, alloc3) > 0);

	// it is hard to get the offsets without channging code with if def for testing, so we are testing against count only
	compare_res = test_committer_compare_to(NULL, 3, true);
	if (compare_res != MUNIT_OK) return compare_res;


	return MUNIT_OK;
}

static MunitResult test_fmem_poison(const MunitParameter params[], void* data){
    // test that small buffers are not welcomed
    char buffer[large_buffer_size] = {0};
    struct fmem *original_fm = fmem_create_new(buffer, large_buffer_size, 0, NULL);
    munit_assert(original_fm > 0);

    // corrupt head page
    // this also checks the correct stashing of our accounting object
    struct fmem_page *head_page = fpage_from_mem(original_fm);
    fpage_set_magic(head_page,0);

    // reuse it
    struct fmem *reused = fmem_from_existing(buffer, NULL);
    munit_assert(EE_BAD_MEM == (int64_t) reused);

    // reset to fix it.
    original_fm = fmem_create_new(buffer, large_buffer_size, 0, NULL);
    head_page = fpage_from_mem(original_fm);
    // we know that there is two pages, lets mess up the second on
    struct fmem_page *main_page = list_entry(head_page->list.next, struct fmem_page, list);
    fpage_set_magic(main_page, 0);

    void *mem = fmem_alloc(original_fm, large_buffer_size /2);
    munit_assert(EE_BAD_MEM == (int64_t) mem);

    return MUNIT_OK;
}

static MunitResult test_fmem_free_poison(const MunitParameter params[], void* data){
    char buffer[large_buffer_size] = {0};
    struct fmem *fm =  fmem_create_new(buffer, large_buffer_size, PAGE_OVERHEAD, NULL);
    munit_assert(fm > 0);

    // alloc one
    void* mem = NULL;
    mem = fmem_alloc(fm, PAGE_OVERHEAD);
    munit_assert(mem != NULL && mem > 0);

    // lets mess it up
    struct fmem_page *fpage = fpage_from_mem(mem);
    fpage_set_magic(fpage, 0);// invalidates the magic
    munit_assert(fmem_free(fm, mem) == EE_BAD_MEM);
    return MUNIT_OK;
}


static MunitResult test_fmem_alloc_fails(const MunitParameter params[], void* data){
    char buffer[large_buffer_size] = {0};

    struct fmem *fm_small_mem_big_min_alloc =  fmem_create_new(buffer, MIN_TOTAL_ALLOCATION, MIN_TOTAL_ALLOCATION/ 2, NULL);
    munit_assert(fm_small_mem_big_min_alloc == (void *) E_BAD_INIT_MEM);

    struct fmem *fm_fails = fmem_create_new(buffer, large_buffer_size, DEFAULT_MIN_ALLOC, NULL);

    // try to allocate larger than available
    munit_assert(fmem_alloc(fm_fails, large_buffer_size) == (void *) FMEM_E_NOMEM);

    size_t half = large_buffer_size / 2;
    // this should work
    munit_assert(fmem_alloc(fm_fails, half) > 0);

    // but this should fail, because we used some over head the second half can not possibly fit
    munit_assert(fmem_alloc(fm_fails, half) == (void *) FMEM_E_NOMEM);

    return MUNIT_OK;
}

static MunitResult test_fmem_alloc_free_simple(const MunitParameter params[], void* data){
    char buffer[large_buffer_size] = {0};
    struct fmem *fm =  fmem_create_new(buffer, large_buffer_size, PAGE_OVERHEAD, NULL);
    munit_assert(fm > 0);

    size_t original_available = fm->total_available;
    // check number of objects
    munit_assert(fm->alloc_objects == 0);
    // alloc one
    void* mem1 = NULL;
    mem1 = fmem_alloc(fm, PAGE_OVERHEAD);
    munit_assert(mem1 != NULL && mem1 > 0);
    munit_assert(fm->alloc_objects == 1);

    struct fmem_page *head_page = fpage_from_mem(fm);
    int count = count_pages(head_page);
    munit_assert(count == 3); // one head, one main,  one used

    // size checks are done  in carving tests
    // so we only need to test:
    // accounting object size has changed as expected
    // third page is set as busy
    struct list_head *second = head_page->list.next;
    struct fmem_page *third_page = list_entry(second->next, struct fmem_page, list);
    munit_assert(false == fpage_is_free(third_page));
    munit_assert(fm->total_available == (original_available - third_page->size));

    fmem_free(fm, mem1);
    munit_assert(fm->alloc_objects == 0);
    count = count_pages(head_page);
    munit_assert(count == 2); // should merge back into main (left merge)
    munit_assert(original_available == fm->total_available); // did we revert size correctly
    return MUNIT_OK;
}

static MunitResult test_fmem_creation(const MunitParameter params[], void* data){
    // test that small buffers are not welcomed
    char small_buffer[small_buffer_size] = {0};
    struct fmem *bad_fm = fmem_create_new(small_buffer, small_buffer_size, 5, NULL);
    munit_assert(bad_fm == (void *)E_TOTAL_ALLOCATION_SIZE_TOO_SMALL);

    // test that object was created correctly
    char buffer[large_buffer_size] = {0};
    struct fmem *fm =  fmem_create_new(buffer, large_buffer_size, 10, NULL);
    munit_assert(fm != NULL);

		munit_assert(fm->committer == NULL); // committer is not set

    // this also checks the correct stashing of our accounting object
    struct fmem_page *head_page = fpage_from_mem(fm);
    // ALL UNIT TESTS MUST RUN WITH BAD_MEM CHECKs
    munit_assert(POISON ==  fpage_get_magic(head_page));
    munit_assert(false == fpage_is_free(head_page));

    // we should have two pages
    munit_assert(count_pages(head_page) == 2);
    munit_assert( (char *) head_page == (char *) buffer);

    struct fmem_page *main_page = list_entry(head_page->list.next, struct fmem_page, list);
    munit_assert(POISON ==  fpage_get_magic(main_page));
    munit_assert(true == fpage_is_free(main_page));

    // check sizes
    size_t total_size_expected = large_buffer_size;
    munit_assert(fm->total_size == total_size_expected);
    munit_assert(fm->min_alloc == DEFAULT_MIN_ALLOC); // we are using too small alloc, we should default

    size_t total_expected_available = total_size_expected - (2 * PAGE_OVERHEAD + sizeof(struct fmem));
    munit_assert(fm->total_available == total_expected_available);

    uint32_t expected_main_page_size = total_size_expected - (PAGE_OVERHEAD + sizeof(struct fmem));
    munit_assert(main_page->size ==  expected_main_page_size);


    // check that create frome existing work as expected
    struct fmem *fm_other = fmem_from_existing(buffer, NULL);
    munit_assert(fm_other == fm);
		munit_assert(fm_other->committer == NULL); // committer is not set

    // test that large min allocs are respected
    struct fmem *fm_large_alloc =  fmem_create_new(buffer, large_buffer_size, 5 * sizeof(struct fmem_page), NULL);
    munit_assert(fm_large_alloc->min_alloc == 5 * sizeof(struct fmem_page));
    munit_assert(fm_large_alloc > 0);

		// test setting the committer

		struct fmem *fm_with_committer =  fmem_create_new(buffer, large_buffer_size, 10, test_committer);
		munit_assert(fm_with_committer->committer == test_committer);
		// clear it
		fm_with_committer->committer = NULL;
		struct fmem *fm_other_with_comitter = fmem_from_existing(buffer, test_committer);
		munit_assert(fm_other_with_comitter->committer == test_committer);


    return MUNIT_OK;
}


// tests page to mem and mem to page
static MunitResult test_fpage_mem_handling(const MunitParameter params[], void* data){
  char buffer[5 * 1024] = {0};
  struct fmem_page *p =  (struct fmem_page *) buffer;
  make_fpage(p, 200);

  void* mem = mem_from_fpage(p);
  munit_assert(mem == ( ((char *) p) + PAGE_OVERHEAD));

   struct fmem_page *other_p = fpage_from_mem(mem);
   munit_assert( (char *) other_p == ( ((char *) mem) - PAGE_OVERHEAD)); // <- this is ugly
   munit_assert((char *) other_p ==  (char *) p);

  return MUNIT_OK;
}

// tests flags set/get
static MunitResult test_fpage_flags_handling(const MunitParameter params[], void* data){
  char buffer[5 * 1024] = {0};
  struct fmem_page *p =  (struct fmem_page *)buffer;
  make_fpage(p, 200);

  // magic on its own
  const uint16_t magic = 2022;
  fpage_set_magic(p, magic);
  munit_assert(magic == fpage_get_magic(p));

  // busy setup
  fpage_set_busy(p);
  munit_assert(false == fpage_is_free(p));
  munit_assert(magic == fpage_get_magic(p));// paranoia, but it helps


  // resetting magic does not change the status
  fpage_set_magic(p, 123);
  munit_assert(123 == fpage_get_magic(p));
  munit_assert(false == fpage_is_free(p));

  // free set does not change magic
  fpage_set_magic(p, magic);
  fpage_set_free(p);
  munit_assert(true == fpage_is_free(p));
  munit_assert(magic == fpage_get_magic(p));

  // resetting magic does not change status
  fpage_set_magic(p, 123);
  munit_assert(123 == fpage_get_magic(p));
  munit_assert(true == fpage_is_free(p));


  return MUNIT_OK;
}

// test for a merge that covers prev+current+next
static MunitResult test_fpage_merge_all(const MunitParameter params[], void* data){
  // setup
  struct fmem_page fpage_A = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_B = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_C = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_D = {.size = 10 * sizeof(struct fmem_page)};

  list_head_init(&fpage_A.list);
  list_add_after(&fpage_A.list, &fpage_B.list);
  list_add_after(&fpage_B.list, &fpage_C.list);
  list_add_after(&fpage_C.list, &fpage_D.list);
  // mark first page as busy
  fpage_set_busy(&fpage_A);
  //merge them
  uint32_t expected_total_size_after = fpage_B.size + fpage_C.size +fpage_D.size;
  fpage_merge(&fpage_C); // should merge prev, current, next
  int count = count_pages(&fpage_A);
  munit_logf(MUNIT_LOG_INFO, " count:%d", count);

  munit_assert(count == 2); // 4 we lost 2
  munit_assert(fpage_B.size = expected_total_size_after); // size adjust correctly?

  struct fmem_page *current_b =list_entry (fpage_A.list.next, struct fmem_page, list);
  munit_assert(current_b ==  &fpage_B); // B shouldn't change

  return MUNIT_OK;
}

// test for a merge that covers prev+current
static MunitResult test_fpage_merge_left(const MunitParameter params[], void* data){
  // setup
  struct fmem_page fpage_A = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_B = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_C = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_D = {.size = 10 * sizeof(struct fmem_page)};

  list_head_init(&fpage_A.list);
  list_add_after(&fpage_A.list, &fpage_B.list);
  list_add_after(&fpage_B.list, &fpage_C.list);
  list_add_after(&fpage_C.list, &fpage_D.list);
  // mark first page as busy
  fpage_set_busy(&fpage_A);
  fpage_set_busy(&fpage_D); // marks last one busy
  //merge them
  uint32_t expected_total_size_after = fpage_B.size + fpage_C.size;
  fpage_merge(&fpage_C); // should merge prev, current, next
  int count = count_pages(&fpage_A);
  munit_logf(MUNIT_LOG_INFO, " count:%d", count);

  munit_assert(count == 3); // 4 we lost 1
  munit_assert(fpage_B.size = expected_total_size_after); // size adjust correctly?

  struct fmem_page *current_b =list_entry (fpage_A.list.next, struct fmem_page, list);
  munit_assert(current_b ==  &fpage_B); // B shouldn't change

  return MUNIT_OK;
}
// test for a merge that covers current+next
static MunitResult test_fpage_merge_right(const MunitParameter params[], void* data){
  // setup
  struct fmem_page fpage_A = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_B = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_C = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_D = {.size = 10 * sizeof(struct fmem_page)};

  list_head_init(&fpage_A.list);
  list_add_after(&fpage_A.list, &fpage_B.list);
  list_add_after(&fpage_B.list, &fpage_C.list);
  list_add_after(&fpage_C.list, &fpage_D.list);
  // mark first page as busy
  fpage_set_busy(&fpage_A);
  fpage_set_busy(&fpage_B); // marks second one as busy, forces merge C+D
  //merge them
  uint32_t expected_total_size_after = fpage_C.size + fpage_D.size;
  fpage_merge(&fpage_C); // should merge prev, current, next
  int count = count_pages(&fpage_A);
  munit_logf(MUNIT_LOG_INFO, " count:%d", count);

  munit_assert(count == 3); // 4 we lost 1
  munit_assert(fpage_B.size = expected_total_size_after); // size adjust correctly?

  struct fmem_page *current_b =list_entry (fpage_A.list.next, struct fmem_page, list);
  munit_assert(current_b ==  &fpage_B); // B shouldn't change

  return MUNIT_OK;
}
// test for a merge that shouldn't happen because prev and next are busy pages
static MunitResult test_fpage_merge_none(const MunitParameter params[], void* data){
  // setup
  struct fmem_page fpage_A = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_B = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_C = {.size = 10 * sizeof(struct fmem_page)};
  struct fmem_page fpage_D = {.size = 10 * sizeof(struct fmem_page)};

  list_head_init(&fpage_A.list);
  list_add_after(&fpage_A.list, &fpage_B.list);
  list_add_after(&fpage_B.list, &fpage_C.list);
  list_add_after(&fpage_C.list, &fpage_D.list);
  fpage_set_busy(&fpage_A);
  fpage_set_busy(&fpage_B);
  fpage_set_busy(&fpage_D);
  //merge them
  uint32_t expected_total_size_after = fpage_C.size;
  fpage_merge(&fpage_C); // should merge prev, current, next
  int count = count_pages(&fpage_A);
  munit_logf(MUNIT_LOG_INFO, " count:%d", count);

  munit_assert(count == 4); // no merge should happen
  munit_assert(fpage_B.size = expected_total_size_after); // size adjust correctly?

  struct fmem_page *current_b =list_entry (fpage_A.list.next, struct fmem_page, list);
  munit_assert(current_b ==  &fpage_B); // B shouldn't change

  return MUNIT_OK;
}

static MunitResult test_fpage_carving(const MunitParameter params[], void* data){
  struct test_case{
    char* name;
    uint32_t size_before; // total for page before
    uint32_t carve; // part to take out
    uint32_t size_after; // total remaining
    uint32_t carved_size; // total for the page that we carved out
  };

  // if you add tests here make sure that you don't overshoot buffer defined below
  struct test_case cases[] = {
    {
    .name = "cut a page two half",
    .size_before = 10 * sizeof(struct fmem_page),
    .carve = 5 *  sizeof(struct fmem_page),
    .size_after =  4 *  sizeof(struct fmem_page), // carve adds the over head
    .carved_size = 5 * sizeof(struct fmem_page) + PAGE_OVERHEAD,
    },
    {
    .name = "cut a some",
    .size_before = 10 * sizeof(struct fmem_page),
    .carve = 2 *  sizeof(struct fmem_page),
    .size_after =  7 *  sizeof(struct fmem_page), // carve adds the over head
    .carved_size = 2 * sizeof(struct fmem_page) + PAGE_OVERHEAD,
    },
  {
    .name = "cut most",
    .size_before = 10 * sizeof(struct fmem_page),
    .carve = 7 *  sizeof(struct fmem_page),
    .size_after =  2 *  sizeof(struct fmem_page), // carve adds the over head
    .carved_size = 7 * sizeof(struct fmem_page) + PAGE_OVERHEAD,
    }

  };

  int case_count = 3;
  char buffer[50 * 1024] = {0};
  // munit while slim does not have the idea of dictionary of tests
  // the below is hack, ugly but works
  for(int i = 0; i < case_count; i++){
    // make the page
    struct fmem_page *fpage = (struct fmem_page *) buffer;
    struct fmem_page *created = NULL;
    make_fpage(fpage, cases[i].size_before);
    munit_logf(MUNIT_LOG_INFO, "working on:[%s]", cases[i].name);

    //carve
    fpage_carve(fpage, &created, cases[i].carve);

    // check
    munit_assert(created != NULL); // did we create the page?
    munit_assert(fpage->list.next == &(created->list)); // did we add it right after

    munit_assert(fpage->size == cases[i].size_after); // did we adjust the size after?
    munit_assert(created->size == cases[i].carved_size); // did we create the new one using the correct size?
    // is new page pointer placed correctly at the end of the page we carve
    munit_assert((char *) created == ((char *) fpage) + fpage->size);
  }
  return MUNIT_OK;
}

static MunitResult test_fpage_fit(const MunitParameter params[], void* data){
  struct test_case{
    char* name;
    uint32_t size;
    uint32_t needed;
    int expected;
  };

  // if you add tests here make sure that you don't overshoot buffer defined below
  struct test_case cases[] = {
    {
    .name = "can't fit, needed is more than the size page",
    .size = 5 * sizeof(struct fmem_page),
    .needed = 10 *  sizeof(struct fmem_page),
    .expected = CAN_NOT_FIT,
    },
    {
    .name = "fits with a split",
    .size = 10 * sizeof(struct fmem_page),
    .needed = 2 *  sizeof(struct fmem_page),
    .expected = FIT_WITH_CARVE,
    },
    {
    .name = "fits as is",
    .size = 5 * sizeof(struct fmem_page),
    .needed =  5 * sizeof(struct fmem_page) - (PAGE_OVERHEAD + PAGE_REMAIN_FREE) , // the entire remaining
    .expected = FIT_AS_IS,
    },
  };

  int case_count = 3;
  char buffer[50 * 1024] = {0}; // makes testing easier as we don't need to alloc/release things
  // munit while slim does not have the idea of dictionary of tests
  // the below is hack, ugly but works
  for(int i = 0; i < case_count; i++){
    // make the page
    struct fmem_page *fpage = (struct fmem_page *) buffer;
    make_fpage(fpage, cases[i].size);
    int fit = fpage_can_fit(fpage, cases[i].needed);
    munit_logf(MUNIT_LOG_INFO, "working on:[%s] expected:%d actual:%d", cases[i].name,  cases[i].expected, fit);
    munit_logf(MUNIT_LOG_INFO, "SIZE:%d NEEEDED:%d", fpage_actual(fpage), cases[i].needed);
    munit_assert(fit == cases[i].expected);
  }
  return MUNIT_OK;
}

static MunitResult test_fmem_get_size(const MunitParameter params[], void* data){
  struct test_case{
    char* name;
    uint32_t size;
    uint32_t expected_actual;
  };

  // if you add tests here make sure that you don't overshoot buffer defined below
  struct test_case cases[] = {
    {
      .name = "case 1: test size, with header in place",
      .size = 10 * sizeof(struct fmem_page),
      .expected_actual = 9 * sizeof(struct fmem_page),
    },
    {
      .name = "case 2: test size, with header in place",
      .size = 15 * sizeof(struct fmem_page),
      .expected_actual = 14 * sizeof(struct fmem_page),
    }
  };

  int case_count = 2;
  char buffer[10 * 1024] = {0}; // makes testing easier as we don't need to alloc/release things
  // munit while slim does not have the idea of dictionary of tests
  // the below is hack, ugly but works
  for(int i = 0; i < case_count; i++){
    // make the page
    struct fmem_page *fpage = (struct fmem_page *) buffer;
    make_fpage(fpage, cases[i].size);
    uint32_t actual_size = fpage_actual(fpage);
    munit_logf(MUNIT_LOG_INFO, "working on:[%s] expected:%d actual:%d", cases[i].name,  cases[i].expected_actual, actual_size);
    munit_assert(actual_size == cases[i].expected_actual);
  }
  return MUNIT_OK;
}

// all tests
MunitTest fmem_tests[] = {
  /* tests struct: name(string),  test func, setup func, tear down func, opts, params*/
	// TEST ORDER IS IMPORTANT.
	// While tests are written new on top, they are wired to the suite in logical order
	// some failure in early tests will result into failure in later tests.
  {"/page-size", test_fmem_get_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-fit", test_fpage_fit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-carving", test_fpage_carving, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-all", test_fpage_merge_all, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-left", test_fpage_merge_left, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-right", test_fpage_merge_right, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-none", test_fpage_merge_none, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-flags-handling", test_fpage_flags_handling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-mem-handling", test_fpage_mem_handling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/fmem-creation", test_fmem_creation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/fmem-simple-alloc-free", test_fmem_alloc_free_simple, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/fmem-simple-alloc-fails", test_fmem_alloc_fails, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  // poison tests
  {"/fmem-all-free-poison", test_fmem_free_poison, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/fmem-reuse-poison", test_fmem_poison, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	// commit tests
	{"/fmem-commit", test_fmem_commit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},

  // final entry must be null, as we don't pass in count
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};



static const MunitSuite fmem_test_suite = {
  /* This string will be prepended to all test names in this suite;
   * for example, "/example/rand" will become "/µnit/example/rand".
   * Note that, while it doesn't really matter for the top-level
   * suite, NULL signal the end of an array of tests; you should use
   * an empty string ("") instead. */
  (char*) "fmem-tests",
  /* The first parameter is the array of test suites. */
  fmem_tests,
  /* In addition to containing test cases, suites can contain other
   * test suites.  This isn't necessary in this example, but it can be
   * a great help to projects with lots of tests by making it easier
   * to spread the tests across many files.  This is where you would
   * put "other_suites" (which is commented out above). */
  NULL,
  /* An interesting feature of µnit is that it supports automatically
   * running multiple iterations of the tests.  This is usually only
   * interesting if you make use of the PRNG to randomize your tests
   * cases a bit, or if you are doing performance testing and want to
   * average multiple runs.  0 is an alias for 1. */
  1,
  /* Just like MUNIT_TEST_OPTION_NONE, you can provide
   * MUNIT_SUITE_OPTION_NONE or 0 to use the default settings. */
  MUNIT_SUITE_OPTION_NONE
};


int main(int argc, char* argv[MUNIT_ARRAY_PARAM(argc + 1)]){
  return munit_suite_main(&fmem_test_suite, NULL, argc, argv);
}
#endif
