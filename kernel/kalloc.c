// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

//my add
#define   REFENENCE_MAX   ((PHYSTOP - KERNBASE)/PGSIZE)       
#define   PA2IDX(pa)      (((uint64)pa - KERNBASE)/PGSIZE) 
struct {
  struct spinlock lock;
  uint64 reference_count[REFENENCE_MAX + 1];
} reference_struct;

int reference_inc(uint64 pa) {
  if (pa < KERNBASE || pa >= PHYSTOP) {
    return -1;
  }
  acquire(&reference_struct.lock);
  reference_struct.reference_count[PA2IDX(pa)]++;
  release(&reference_struct.lock);
  return 0;
}

int reference_dec(uint64 pa) {
  int r_val = -1;
  if (pa < KERNBASE || pa >= PHYSTOP) {
    return -1;
  }
  acquire(&reference_struct.lock);
  if (reference_struct.reference_count[PA2IDX(pa)] <= 0) {
    reference_struct.reference_count[PA2IDX(pa)] = 0;
    release(&reference_struct.lock);
    return -1;
  } else {
    r_val = --reference_struct.reference_count[PA2IDX(pa)];
  }
  release(&reference_struct.lock);

  return r_val;
}

int reference_get(uint64 pa) {
  int count;
  if (pa < KERNBASE || pa >= PHYSTOP) {
    return -1;
  }
  acquire(&reference_struct.lock);
  count = (int)reference_struct.reference_count[PA2IDX(pa)];
  release(&reference_struct.lock);
  return count;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&reference_struct.lock, "reference_count");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    reference_inc((uint64)p);
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int count;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  count = reference_dec((uint64)pa);
  if (count == 0) {
      memset(pa, 1, PGSIZE);
      r = (struct run*)pa;

      acquire(&kmem.lock);
      r->next = kmem.freelist;
      kmem.freelist = r;
      release(&kmem.lock);
  } 
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r) {
    memset((char*)r, 5, PGSIZE); // fill with junk
    reference_inc((uint64)r);
  }
  return (void*)r;
}
