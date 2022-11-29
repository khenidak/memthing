#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "fmem/fmem.h"
#include "things.h"


/*
 * This example uses a shared memory backed with a file. and supports process restarts with state maintained in a file
 * we are relaying on fixed mem allocator ability to call back on memory changes to presist data into the backing the file
 * because of that we don't need a shared memory segement like the non persisted example.
 */
#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); } while (0)
#define MAP_SIZE sizeof(char) * 1024 * 1024 * 10

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!   !!!!!!!!!!!!!!!!!!!!!!!!!!!
 * MAKE SURE THAT YOU MODIFY THIS BEFORE BUILDING+RUNNING IT
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!   !!!!!!!!!!!!!!!!!!!!!!!!!!!
*/
const char* MEM_FILE = "/opt/nvme0/mem"; // this mapped to nvme disk on a cloud VM, but any fast disk will do
struct fmem *fm = NULL; // our fixed mem allcoator
int fd = 0;// file handle for the file backing the memory
void *map_to = NULL;

// msync(2) needs aligned addresses to page size
// this function fix them in place.
// it expects map_to global variable to be aligned.
// TODO: check if we should move this fmem via flag in creation
static inline void align_addr(struct commit_range* range){
	const size_t page_size = getpagesize();
	size_t original = (size_t) range->start;
	size_t start = original;
	// round down to nearest page size
	start =    start - (start % page_size);
	// make sure that we don't undershoot mem barrier
	if( start < (size_t) map_to) start = (size_t) map_to;

	range->start = (void *) start;
	range->len = original - start /* equal or less than original*/ + range->len;
}



// fixed map address must be multiple of page sizes.
size_t get_map_address(){
  size_t heap = getpagesize();
  return heap * 1000000;  // skip the first million page
}

// used by our things maker to allocate using fm
void * alloc_using_fm(size_t size){
  return fmem_alloc(fm, size);
}

// fixed memory allocator is created with a comitter. The committer is a function
// that is called by the allcoator every time the memory content changes. this func
// is meant to write content of the memory to the backing file.
int the_committer(struct commit_range* ranges, uint8_t count){
	// one range
	if(count == 1){
		align_addr(ranges);
		int res = msync(ranges->start, ranges->len, MS_SYNC);
		if(res != 0 ){
			errExit("WARNING: msync sync failed! -- this will result into chaos\n");
			return -1;
		}
		return ranges->len;
	}

	// multi range, scatter+gather
	for(int i = 0; i < count; i++){
		// if failed
		 align_addr(&ranges[i]);
		int res = msync(ranges[i].start, ranges[i].len, MS_ASYNC);
		if(res != 0){
			errExit("WARNING: msync async failed! -- this will result into chaos");
			return -1;
		}
	}

	// lets wait on all writes,
	int res = fsync(fd);
	if(res != 0){
		errExit("WARNING: fsync failed! -- this will result into chaos");
		return -1;
	}

	return 1;
}

// our things maker calls this one when an arbitrary memory
// has changed. We use this to ask our fmem to commit mem
// which in turn calls our committer
void things_maker_oneach(void *each, size_t len){
	if(fmem_commit_mem(fm, each, len) <=0){
		errExit("WARNING: fmem failed to commit memory");
	}
}

int mode_init(){

	// open the file
  fd = open(MEM_FILE, O_CREAT | O_RDWR | O_SYNC);
  if (fd == -1) errExit("file open\n");

  if(ftruncate(fd, MAP_SIZE) != 0) errExit("failed to turncate file\n");

	// resize
  map_to = (void *) get_map_address();
  void *shared_mem = mmap(map_to, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
  if (shared_mem == MAP_FAILED) errExit("mmap\n");

  // create a fixed memory allocator on the shared memory
  fm = fmem_create_new(map_to, MAP_SIZE, 0 /* use default alloc size */, the_committer /* our committer */);
  if(fm <= 0) errExit("failed to create fixed mem object\n");

  void *header = NULL;
  int allocated = make_wellknown_things(&header, alloc_using_fm, things_maker_oneach /* call for every mem change*/);
  if(allocated <= 0) errExit("failed to make things on memory owned by fmem\n");

  // header now has a ref to things (header)
  // stash it in fm user data
  fm->user1 = header;

	// we need to commit user data in our fixed memory
	if(fmem_commit_user_data(fm) < 0) errExit("failed to commit user data");

  return EXIT_SUCCESS;
}


int mode_read(){
  printf("running READ mode \n");

  // remap the file
	// open the file
  fd = open(MEM_FILE, O_RDWR | O_SYNC);
  if (fd == -1) errExit("file open\n");

  if(ftruncate(fd, MAP_SIZE) != 0) errExit("failed to turncate file\n");

	// resize
  map_to = (void *) get_map_address();
  void *shared_mem = mmap(map_to, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
  if (shared_mem == MAP_FAILED) errExit("mmap\n");

  map_to = (void *) get_map_address();

  shared_mem = mmap(map_to, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED , fd, 0);
  if (shared_mem == MAP_FAILED) errExit("mmap\n");

  // create a fixed memory allocator on the shared memory
  fm = fmem_from_existing(map_to, NULL );
  if(fm <= 0) errExit("failed to create fixed mem object\n");

  struct things *header = (struct things *) fm->user1;

  // verify
  if(verify_things(header) != 0) errExit("memory is not the same\n");

  printf("data is the same after a remap\n");

  return EXIT_SUCCESS;
}

int mode_cleanup(){
  printf("running CLEANUP mode \n");
	remove(MEM_FILE);
  return EXIT_SUCCESS;
}


int main(int argc, char *argv[]) {
  if(argc != 2) goto arg_failed;
  int opt = 0;
  while ((opt = getopt(argc, argv, "irc")) != -1) {
    switch (opt) {
      case 'i': return mode_init();
      case 'r': return mode_read();
      case 'c': return mode_cleanup();
      default:
        goto arg_failed;
    }
  }

arg_failed:
    errExit("Usage: %s [-irc] (select one) \n");
}
