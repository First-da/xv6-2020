// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUCKETS_CACHE 13

extern struct spinlock tickslock;
extern uint ticks;
struct hash_struct{
  struct spinlock lock;
  struct buf *head;
} ;

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  //new hash
  struct hash_struct cache_hash_table[BUCKETS_CACHE];
} bcache;

void binit(void)
{
  struct buf *b;
  
  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->ticks_now = 0;
    b->buf_next = 0;
    initsleeplock(&b->lock, "buffer");
  }

  for (int i = 0; i < BUCKETS_CACHE; i++) {
    initlock(&bcache.cache_hash_table[i].lock, "bcache.bucket");
    bcache.cache_hash_table[i].head = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf* bget(uint dev, uint blockno)
{
  struct hash_struct *h;
  struct buf *b;

  h = &bcache.cache_hash_table[blockno % BUCKETS_CACHE];
  acquire(&h->lock);
  for (b = h->head; b != 0; b = b->buf_next) {
    if (b->dev == dev && b->blockno == blockno) {
        b->refcnt++;
        // release(&bcache.lock);
        release(&h->lock);
        acquiresleep(&b->lock);
        return b;
    }
  }
  release(&h->lock);

  struct buf *b_target = 0;
  struct hash_struct *h_target;
  acquire(&bcache.lock);

  // 重新检查目标磁盘块是否已经被其他 CPU 缓存
  acquire(&h->lock);
  for (b = h->head; b != 0; b = b->buf_next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&h->lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&h->lock);

  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    //1.找到最老的buf
    h_target = &bcache.cache_hash_table[b->blockno % BUCKETS_CACHE];
    acquire(&h_target->lock);
    if(b->refcnt == 0) {
      if (b_target == 0 || b->ticks_now < b_target->ticks_now)
          b_target = b;
    }
    release(&h_target->lock);
  }
  if (b_target == 0)
    panic("bgets:no free buffers!");
  // release(&bcache.lock);

  //2.删除原先的hash节点
  h_target = &bcache.cache_hash_table[b_target->blockno % BUCKETS_CACHE];
  acquire(&h_target->lock);
  if (b_target->refcnt != 0) {
    release(&h_target->lock);
    release(&bcache.lock);
    return bget(dev, blockno);
  } 
  b_target->refcnt = 1;
  b = h_target->head;

  if (b) {
    if (b == b_target)
      h_target->head = b->buf_next;
    else {
        while (b->buf_next){
        if (b->buf_next == b_target) {
          b->buf_next = b->buf_next->buf_next;
          break;
        }
        b = b->buf_next;
      }
    }
  }
  release(&h_target->lock);

  //3.增加一个hash节点
  h_target = &bcache.cache_hash_table[blockno % BUCKETS_CACHE];
  acquire(&h_target->lock);
  if(h_target->head){
    b_target->buf_next = h_target->head;
    h_target->head = b_target;
  } else {
    b_target->buf_next = 0;
    h_target->head = b_target;
  }
  
  b_target->dev = dev;
  b_target->blockno = blockno;
  b_target->valid = 0;
  
  release(&h_target->lock);
  release(&bcache.lock);
  acquiresleep(&b_target->lock);
  return b_target;
    
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf* bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // acquire(&bcache.lock);
   struct hash_struct *h = &bcache.cache_hash_table[b->blockno % BUCKETS_CACHE];

  acquire(&h->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    acquire(&tickslock);
    b->ticks_now = ticks;
    release(&tickslock);
  }

  release(&h->lock);
  // release(&bcache.lock);
}

void bpin(struct buf *b) {
  // acquire(&bcache.lock);
  struct hash_struct *h = &bcache.cache_hash_table[b->blockno % BUCKETS_CACHE];

  acquire(&h->lock);
  b->refcnt++;
  release(&h->lock);
  // release(&bcache.lock);

}

void
bunpin(struct buf *b) {
  // acquire(&bcache.lock);
  struct hash_struct *h = &bcache.cache_hash_table[b->blockno % BUCKETS_CACHE];
  acquire(&h->lock);
  b->refcnt--;
  // release(&bcache.lock);
  release(&h->lock);

}


