#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/shm.h>

#include "fmem/fmem.h"
#include "things.h"


/*
  this is an example of using shared memory object as a stash to keep control plane data
  this way control plane data out lives the process life time (e.g., a process restart does not
  lose this data. We are doing this by
  1. creating a shared memory object
  2. map it to our memory
  3. create a fixed memory allocator on top of it
  4. create data via things maker which takes an allocator (func pointer wrapping fmem).

  the app runs in three modes
  1. init mode (-i) to create the data
  2. read (-r) the data and compare that it is correct (for those who doubt).
  3. clean (-c) removes the shared memory object

  note: on things maker, things maker is really nothing other than a thing that creates
  a well known set of objects for us to read and validate that they were in fact saved correcty.
  it does this by depending on an allocator (a malloc style func) that is called for every allocator
*/
#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE); } while (0)
#define MAP_SIZE sizeof(char) * 1024 * 100

const char* shared_mem_path = "/things-mem";
struct fmem *fm = NULL; // our fixed mem allcoator


// used by our things maker to allocate using fm
void * alloc_using_fm(size_t size){
  return fmem_alloc(fm, size);
}

// fixed map address must be multiple of page sizes.
size_t get_map_address(){
  size_t heap = getpagesize();
  return heap * 1000000;  // skip the first million page
}

int mode_init(){
  size_t *shared_mem = NULL;

  int fd = shm_open(shared_mem_path, O_CREAT |  O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1) errExit("shm_open\n");

  if(ftruncate(fd, MAP_SIZE) != 0) errExit("failed to turncate file\n");

  void *map_to = (void *) get_map_address();

  shared_mem = mmap(map_to, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED /*if you expect multiple thread to race to cerate map then use _REPLACE flag*/, fd, 0);
  if (shared_mem == MAP_FAILED) errExit("mmap\n");

  // create a fixed memory allocator on the shared memory
  fm = fmem_create_new(map_to, MAP_SIZE, 0 /* use default alloc size */, NULL /* we are not committing anything */);
  if(fm <= 0) errExit("failed to create fixed mem object\n");

  void *header = NULL;

  int allocated = make_wellknown_things(&header, alloc_using_fm, NULL /* for this example we are not presisting anything */);
  if(allocated <= 0) errExit("failed to make things on memory owned by fmem\n");

  // header now has a ref to things (header)
  // stash it in fm user data
  fm->user1 = header;
  return EXIT_SUCCESS;
}


int mode_read(){
  printf("running READ mode \n");

  // remap
  size_t *shared_mem = NULL;

  int fd = shm_open(shared_mem_path, O_CREAT |  O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1) errExit("shm_open\n");


  void *map_to = (void *) get_map_address();

  shared_mem = mmap(map_to, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED /*if you expect multiple thread to race to cerate map then use _REPLACE flag*/, fd, 0);
  if (shared_mem == MAP_FAILED) errExit("mmap\n");

  // create a fixed memory allocator on the shared memory
  fm = fmem_from_existing(map_to, NULL /* we are not committing anything */);
  if(fm <= 0) errExit("failed to create fixed mem object\n");

  struct things *header = (struct things *) fm->user1;

  // verify
  if(verify_things(header) != 0) errExit("memory is not the same\n");

  printf("data is the same after a remap\n");
  return EXIT_SUCCESS;

}

int mode_cleanup(){
  printf("running CLEANUP mode \n");

  int fd = shm_open(shared_mem_path, O_CREAT |  O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1) errExit("shm_open\n");

  if (shm_unlink(shared_mem_path) != 0){
    printf("warning: shmunlink failed!\n");
  }
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
