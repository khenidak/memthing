#include <stddef.h>
#include <string.h>
#include <stdbool.h>

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
static void fpage_merge(struct fmem_page *fpage){
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
    return;
  }

  // merge current into previous
  if(!prev_is_same && fpage_is_free(prev)){
    prev->size = prev->size + fpage->size;
    list_remove_at(&fpage->list);
    return;
  }

  // merge next into current
  if(!next_is_same && fpage_is_free(next)){
    fpage->size = fpage->size + next->size;
    list_remove_at(&next->list);
    return;
  }

  // no d ice. both prev, next are busy
}

// returns a refrence to memory owned by a page
static inline void* mem_from_fpage(struct fmem_page *fpage){
  return (void *) ( ((char *) fpage) + PAGE_OVERHEAD);
}
// returns the page owned by a memory reference
static inline struct fmem_page* fpage_from_mem(void *mem){
  return (struct fmem_page *) ( ((char *) mem) - PAGE_OVERHEAD);
}


// spin lock style locking mechnasim, we have only one lock
static void inline fmem_lock(struct fmem *fm){
  // TODO
}


static void inline fmem_unlock(struct fmem *fm){
  // TODO
}

// creates an allocator on preallocated memory. The allocator uses the entire length of memory
static struct fmem* fmem_create_new(void * on_mem, size_t length, uint32_t min_alloc){
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
  return fm;
}



// gets a reference to an existing allocator occupying on_mem memory
static struct fmem* fmem_from_existing(void * on_mem){
  // TODO POISON CHECKS
  //
  // our accounting object is stashed in head page
  struct fmem_page *fpage_head = (struct fmem_page *) on_mem;
  return (struct fmem *) mem_from_fpage(fpage_head);
}

static void* fmem_alloc(struct fmem *fm, uint32_t size){
  void* ret = NULL;
  uint32_t adjusted_alloc = size < fm->min_alloc ? fm->min_alloc : size;

  fmem_lock(fm);
  if (fm->total_available < adjusted_alloc) return (void *) FMEM_E_NOMEM; // do we have enough available?, note
                                                                          // we may do but it will require carving which
                                                                          // we won't be able to tell until we iterate

  // TODO
  // 1- bad mem checks
  // 2- memory move checks

  // carving pages always carve towards the end of the page
  // meaning: our free pages are always closer(on the right) to head. this result into:
  // 1- for initial allocs (until we fill up) they will be at o(1)
  // 2- worest alloc (if we fill up and then release the last alloc) then it will be o(n)
  // hence size of alloc matter. Larger is better. Larger yeilds into lower page count
  //  we can get clever by maintain a cache of free pages but that will be later on
    // get head page
  struct fmem_page *head_page = fpage_from_mem(fm); // this the page that holds our accounting object. We don't iterate over it
  struct fmem_page *selected = NULL;
  struct list_head *head = &head_page->list;
  struct list_head *current = head;
  list_for_each(current, head){
    struct fmem_page *this_page = list_entry(current, struct fmem_page, list);
    if (false == fpage_is_free(this_page)) continue; // can't work with a page in use
    int fit_status = fpage_can_fit(this_page, adjusted_alloc);
    switch(fit_status){
      case CAN_NOT_FIT:
        break;
      case FIT_AS_IS:
        selected = this_page;
        goto done;
      case FIT_WITH_CARVE: // we need to carve this page
        fpage_carve(this_page, &selected, adjusted_alloc);// carve it
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
  }

  fmem_unlock(fm);
  return ret == NULL ? (void *)FMEM_E_NOMEM : ret;
}

// frees a memory and returns total freed memory (includes page overhead, which will also be returned to bool)
// BAD_MEM is tested for this one
static int32_t fmem_free(struct fmem *fm, void *mem){
  // TODO  bad mem
  struct fmem_page *fpage = fpage_from_mem(mem); // get the page for that mem. page is always stashed before the mem

  fmem_lock(fm);

  uint32_t to_free = fpage->size; // keep the size aside
  fpage_set_free(fpage); // free it
  fpage_merge(fpage); // merge, if we can
  // accountig
  fm->alloc_objects -= 1;
  fm->total_available += to_free;

  fmem_unlock(fm);
  return to_free;
}

#ifdef __UNIT_TESTING__
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "munit/munit.h"

#define small_buffer_size 10
#define large_buffer_size 50 * 1024

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

static MunitResult test_fmem_alloc_fails(const MunitParameter params[], void* data){
    char buffer[large_buffer_size] = {0};

    struct fmem *fm_small_mem_big_min_alloc =  fmem_create_new(buffer, MIN_TOTAL_ALLOCATION, MIN_TOTAL_ALLOCATION/ 2);
    munit_assert(fm_small_mem_big_min_alloc == (void *) E_BAD_INIT_MEM);

    struct fmem *fm_fails = fmem_create_new(buffer, large_buffer_size, DEFAULT_MIN_ALLOC);

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
    struct fmem *fm =  fmem_create_new(buffer, large_buffer_size, PAGE_OVERHEAD);
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
    struct fmem *bad_fm = fmem_create_new(small_buffer, small_buffer_size, 5);
    munit_assert(bad_fm == (void *)E_TOTAL_ALLOCATION_SIZE_TOO_SMALL);

    // test that object was created correctly
    char buffer[large_buffer_size] = {0};
    struct fmem *fm =  fmem_create_new(buffer, large_buffer_size, 10);
    munit_assert(fm != NULL);

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
    struct fmem *fm_other = fmem_from_existing(buffer);
    munit_assert(fm_other == fm);
    // TODO bad mem check for from existing case

    // test that large min allocs are respected
    struct fmem *fm_large_alloc =  fmem_create_new(buffer, large_buffer_size, 5 * sizeof(struct fmem_page));
    munit_assert(fm_large_alloc->min_alloc == 5 * sizeof(struct fmem_page));
    munit_assert(fm_large_alloc > 0);

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
  {"/page-size", test_fmem_get_size, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-fit", test_fpage_fit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-carving", test_fpage_carving, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-all", test_fpage_merge_all, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-left", test_fpage_merge_left, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-right", test_fpage_merge_right, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-merging-none", test_fpage_merge_none, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-flags-handling", test_fpage_flags_handling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/page-mem-handling", test_fpage_mem_handling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  // fmem tests, because of the obvious dependancy fmem tests must run after fmem_page
  // tests. a failure above will almost certainly result in failure below
  {"/fmem-creation", test_fmem_creation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/fmem-simple-alloc-free", test_fmem_alloc_free_simple, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/fmem-simple-alloc-fails", test_fmem_alloc_fails, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
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
