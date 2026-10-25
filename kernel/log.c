#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// 日志头块的内容，既用于磁盘上的日志头块，
// 也用于在事务提交前，在内存中记录已经加入日志的磁盘块编号。
// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
// 一个日志事务包含多个文件系统系统调用产生的更新。
// 只有当没有正在执行的文件系统系统调用时，日志系统才会提交事务。
// 因此，不需要考虑提交事务时是否会将尚未完成的系统调用所产生的更新写入磁盘的问题。


// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
// 文件系统系统调用应该通过 begin_op()/end_op() 标记其开始和结束。
// 通常，begin_op() 只是将正在执行的文件系统系统调用计数加一，然后返回。
// 但如果它认为日志空间即将耗尽，就会进入睡眠，
// 直到最后一个尚未完成的文件系统操作调用 end_op()，并完成日志事务的提交。

//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.
// 日志采用物理重做日志（Physical Re-do Log），存储的是磁盘块。
// 磁盘上的日志格式：
//   日志头块，包含磁盘块 A、B、C 等对应的磁盘块编号
//   磁盘块 A
//   磁盘块 B
//   磁盘块 C
//   ...
// 日志追加操作是同步执行的。

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;                //num of log start(blocks)
  int size;                 //size of log(blocks)
  int outstanding;          // how many FS sys calls are executing.
  int committing;           // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

//初始化log的大小和磁盘块数量，从superblock中读取。
//并将log装载至磁盘data区域
void initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();
}

// Copy committed blocks from log to their home location
// 将磁盘中的log装载进data区域(读出磁盘中的log和data到buf，将log buf中的数据拷贝到data buf)，然后刷新bcache
static void install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
//读出bcache中的log header磁盘块，更新log中的lh
static void read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
//从bcache中读出log header磁盘块,并将log结构体中的lh写入bcache，刷新bcache
static void write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

//上电恢复：从disk中读出log header，根据log header的n和block编号，将log中的数据经过bcache写入disk data区域
//设置log header的n为0,刷新bcache，清空log header
static void recover_from_log(void)
{
  read_head();
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
//如果事务在commiting、log空间不足，睡眠等待；否则的话引用outstanding + 1
void begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
// 在每次文件系统系统调用结束时调用。
// 如果这是最后一个尚未完成的文件系统操作，则提交事务。
//引用outstanding - 1，唤醒别的log获取；如果引用为0,那么就结束事务，执行提交.
void end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");

  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    // begin_op() 可能正在等待日志空间，
    // 而 log.outstanding 减少后，为正在执行的文件系统操作预留的日志空间也随之减少。
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
//将修改后的bcache数据写入到disk log
static void write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *from = bread(log.dev, log.lh.block[tail]);  // cache block
    struct buf *to = bread(log.dev, log.start+tail+1);      // log block
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

//将bcache中的修改提交到了disk中去：写入disk中log data区域，写入log header；然后将disk log安装到disk data区域；
//最后设置n为0，刷新bcache，写入磁盘。
static void commit()
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
// 调用者已经修改了 b->data，并且完成了对该缓冲区的操作。
// 记录磁盘块编号，并通过增加 refcnt 将该缓冲区固定在缓存中，防止它被替换。
// 真正的磁盘写入将由 commit()/write_log() 完成。
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
//将一次事务的磁盘块修改添加进log的block中，如果已经存在，相当于什么都没做；如果不存在，那么在block数组末尾添加一个元素
void log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorbtion
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}

