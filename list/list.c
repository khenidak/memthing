#include "list.h"

void list_head_init(struct list_head *l)
{
	l->next = l;
	l->prev = l;
}


void list_add_after(struct list_head *current, struct list_head *new){
	struct list_head *next = current->next;

	next->prev = new;
	new->next = next;
	new->prev = current;
	current->next = new;
}

void list_add_before(struct list_head *current, struct list_head *new){
	struct list_head *prev = current->prev;

	new->prev = prev;
	new->next = current;

	current->prev = new;
	prev->next = new;
}


void list_remove_at(struct list_head *current){
	struct list_head *prev = current->prev;
	struct list_head *next = current->next;

	prev->next = next;
	next->prev = prev;
}


#ifdef __UNIT_TESTING__
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "munit/munit.h"

struct carrier{
	char content;
	struct list_head list;
};

// tests basic init logic
static MunitResult test_init(const MunitParameter params[], void* data){
	struct list_head list = {0};
	list_head_init(&list);
	munit_assert(list.next == list.prev);
	munit_assert(list.next == &list);
	return MUNIT_OK;
}

// basic container_of, list of
static MunitResult test_container_of(const MunitParameter params[], void* data){
	struct carrier our_carrier = {0};

	// init it
	list_head_init(& our_carrier.list);
	struct list_head *marker = &our_carrier.list;

	// check if container_of, list_entry work as expected
	struct carrier *got_carrier = list_entry(marker, struct carrier, list);
	munit_assert(got_carrier == &our_carrier);

	return MUNIT_OK;
}

// add, sequential
static MunitResult test_add(const MunitParameter params[], void* data){
	struct carrier carriers[] = {
		{.content = 'A'},
		{.content = 'B'},
		{.content = 'C'},
		{.content = 'D'},
		{.content = 'E'},
	};

	// init it
	list_head_init(& carriers[0].list);

	// add them
	list_add_after(&carriers[0].list, &carriers[1].list);
	list_add_after(&carriers[1].list, &carriers[2].list);
	list_add_after(&carriers[2].list, &carriers[3].list);
	list_add_after(&carriers[3].list, &carriers[4].list);

		// loop to confirm correct add
	// we are avoiding iterators here.
	struct carrier *current = &carriers[0];
	for(int i = 0; i < 5; i++){
		munit_assert(current->content == carriers[i].content);
		struct list_head *next = current->list.next;
		current = list_entry(next, struct carrier, list);
	}

	// did we lap correctly?
	munit_assert(current->content == carriers[0].content);

	return MUNIT_OK;
}

// prepend, sequential
static MunitResult test_add_before(const MunitParameter params[], void* data){
	struct carrier carriers[] = {
		{.content = 'A'},
		{.content = 'B'},
		{.content = 'C'},
		{.content = 'D'},
		{.content = 'E'},
	};

	// init it
	list_head_init(& carriers[0].list);

	// add them
	list_add_before(&carriers[0].list, &carriers[1].list);
	list_add_before(&carriers[1].list, &carriers[2].list);
	list_add_before(&carriers[2].list, &carriers[3].list);
	list_add_before(&carriers[3].list, &carriers[4].list);

		// loop to confirm correct add
	// we are avoiding iterators here.
	struct carrier *current = &carriers[0];
	for(int i = 0; i < 5; i++){
		munit_assert(current->content == carriers[i].content);
		struct list_head *prev = current->list.prev;
		current = list_entry(prev, struct carrier, list);
	}

	// did we lap correctly?
	munit_assert(current->content == carriers[0].content);

	return MUNIT_OK;
}

// remove_at
static MunitResult test_remove_at(const MunitParameter params[], void* data){
	struct carrier carriers[] = {
		{.content = 'A'},
		{.content = 'B'},
		{.content = 'C'},
		{.content = 'D'},
		{.content = 'E'},
	};

// we remove the second on the right
	struct carrier carriers_after[] = {
		{.content = 'A'},
		{.content = 'C'},
		{.content = 'D'},
		{.content = 'E'},
	};


	// init it
	list_head_init(& carriers[0].list);

	// add them
	list_add_after(&carriers[0].list, &carriers[1].list);
	list_add_after(&carriers[1].list, &carriers[2].list);
	list_add_after(&carriers[2].list, &carriers[3].list);
	list_add_after(&carriers[3].list, &carriers[4].list);

	struct carrier *current = &carriers[0];
	struct carrier *B = list_entry(current->list.next, struct carrier,list);
	list_remove_at(&B->list);
	munit_assert(current->content == carriers[0].content);

	for(int i = 0; i < 4; i++){
		munit_assert(current->content == carriers_after[i].content);
		struct list_head *next = current->list.next;
		current = list_entry(next, struct carrier, list);
	}

	// did we lap correctly?
	munit_assert(current->content == carriers[0].content);

	return MUNIT_OK;
}



// iterator
static MunitResult test_iterator_forward(const MunitParameter params[], void* data){
	struct carrier carriers[] = {
		{.content = 'A'},
		{.content = 'B'},
		{.content = 'C'},
		{.content = 'D'},
		{.content = 'E'},
	};

	// init it
	list_head_init(& carriers[0].list);

	// add them
	list_add_after(&carriers[0].list, &carriers[1].list);
	list_add_after(&carriers[1].list, &carriers[2].list);
	list_add_after(&carriers[2].list, &carriers[3].list);
	list_add_after(&carriers[3].list, &carriers[4].list);

	struct list_head *head = &carriers[0].list;
	struct list_head *current = head;

	int i = 1; // iterator gloss over head
	int max = 4;

	list_for_each(current, head){
		if (i > max){
			munit_logf(MUNIT_LOG_ERROR, "iteration count:%d is more than max:%d", i, max);
			return MUNIT_FAIL;
		}
		struct carrier *current_carrier = list_entry(current, struct carrier, list);

		munit_assert(current_carrier->content == carriers[i].content);
		i++;
	}
	return MUNIT_OK;
}

// iterator
static MunitResult test_iterator_backward(const MunitParameter params[], void* data){
	struct carrier carriers[] = {
		{.content = 'A'},
		{.content = 'B'},
		{.content = 'C'},
		{.content = 'D'},
		{.content = 'E'},
	};


	// init it
	list_head_init(& carriers[0].list);

	// add them
	list_add_after(&carriers[0].list, &carriers[1].list);
	list_add_after(&carriers[1].list, &carriers[2].list);
	list_add_after(&carriers[2].list, &carriers[3].list);
	list_add_after(&carriers[3].list, &carriers[4].list);

	struct list_head *head = &carriers[0].list;
	struct list_head *current = head;

	int i = 4; // iterator gloss over head
	int min = 1;

	list_for_each_backward(current, head){
		if (i < min){
			munit_logf(MUNIT_LOG_ERROR, "iteration count:%d is more than expeted 4", i);
			return MUNIT_FAIL;
		}
		struct carrier * current_carrier = list_entry(current, struct carrier, list);
		munit_assert(current_carrier->content == carriers[i].content);
		i--;
	}
	return MUNIT_OK;
}

// all tests
MunitTest list_tests[] = {
	/* tests struct: name(string),  test func, setup func, tear down func, opts, params*/
  {"/list-head-init", test_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/container-of", test_container_of, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/add", test_add, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/add-before", test_add_before, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/remove-at", test_remove_at, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{"/fwd-iterator", test_iterator_forward, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/bwd-iterator", test_iterator_backward, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	// final entry must be null, as we don't pass in count
 	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};


static const MunitSuite list_test_suite = {
  /* This string will be prepended to all test names in this suite;
   * for example, "/example/rand" will become "/µnit/example/rand".
   * Note that, while it doesn't really matter for the top-level
   * suite, NULL signal the end of an array of tests; you should use
   * an empty string ("") instead. */
  (char*) "list-tests",
  /* The first parameter is the array of test suites. */
  list_tests,
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
	return munit_suite_main(&list_test_suite, NULL, argc, argv);
}
#endif
