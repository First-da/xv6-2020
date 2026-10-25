// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation routines.  
// The (higher-level) system call implementations are in sysfile.c.

// 文件系统实现。共分为五个层次：
//   + Blocks（磁盘块）：负责原始磁盘块的分配。
//   + Log（日志）：用于多步骤更新操作的崩溃恢复。
//   + Files（文件）：inode 分配、读取、写入以及元数据管理。
//   + Directories（目录）：具有特殊内容的 inode（保存其他 inode 的列表！）。
//   + Names（名称）：通过 /usr/rtm/xv6/fs.c 这样的路径方便地命名和定位文件。
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
// 从disk中读取superblock到bcache，更新内存中的superblock实例化sb结构体
static void readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
// 读取disk中的superblock，初始化log(读取superblock的size、start，恢复日志)
void fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// Zero a block.
// 读取指定的bno磁盘块，清零后更新日志提交信息struct log
static void bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// 遍历bitmap磁盘块中的每一个bit，找到一个空闲的磁盘块，并返回磁盘块编号
static uint balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
// 清除指定磁盘块b的bitmap位，并更新日志提交信息struct log
static void bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

//初始化icache和inode数组的锁
void iinit()
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
// 遍历disk的inode区域，找到一个空闲的inode。
// 通过将 inode 的 type 字段设置为指定的 type，标记该 inode 已被分配，更新日志提交信息
struct inode* ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
// 将内存中修改过的 inode 信息写入磁盘。
// 由于 inode Cache 采用写直达（write-through）策略，
// 因此每次修改 ip->xxx 中需要保存在磁盘上的字段后，都必须调用此函数。
// 调用者必须持有 ip->lock。
// 将传入的inode更新磁盘中的inode，写入日志提交信息，
void iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
// 在设备 dev 上查找编号为 inum 的 inode，并返回其在内存中的副本。
// 此函数不会锁定该 inode，也不会从磁盘读取它。
// 从icache中返回制定dev和inum的inode，如果没有找到，那么设置空的inode为dev+inum。不会从bcache中查找
static struct inode *iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
// 对传入的inode引用数+1,并返回inode
struct inode* idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
// 获取inode的锁；如果inode的valid为0,那么从磁盘中读取并更新传入的inode
void ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
//释放inode的睡眠锁
void iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// 减少一个内存 inode 的引用计数。
// 如果这是最后一个引用，那么该 inode 的缓存条目就可以被复用。
// 如果这是最后一个引用，并且该 inode 已经没有任何硬链接指向它，那么就释放磁盘上的 inode（以及它对应的文件内容）。
// 所有对 iput() 的调用都必须在事务内部进行，因为它可能需要释放磁盘上的 inode。
// 减少一次inode引用
// 如果这是最后一次引用，那么清除addr内容、修改type和valid为0，更新inode磁盘(更新日志提交信息)
void iput(struct inode *ip)
{
  acquire(&icache.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&icache.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&icache.lock);
  }

  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
// 释放睡眠锁，减少inode引用
void iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// Inode 的内容
//
// 每个 inode 对应的内容（数据）存储在磁盘块中。
// 前 NDIRECT 个磁盘块的块号记录在 ip->addrs[] 中。
// 接下来的 NINDIRECT 个磁盘块的块号记录在
// ip->addrs[NDIRECT] 所指向的磁盘块中。

// 返回 inode ip 中第 n 个数据块对应的磁盘块地址。
// 如果该数据块尚未分配，bmap 就会为其分配一个。
// 输入:inode->addr中的索引(0 ~ 256 + 12)
// 如果间接地址的磁盘块或者数据块没有分配，会分配一个新的空闲磁盘块
// 输出:inode->addr[bn]中存储的可记数据磁盘块编号addr
static uint bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  bn -= NINDIRECT;
  if (bn < NINDIRECT_2 * NINDIRECT_2){
    if ((addr = ip->addrs[NDIRECT+1]) == 0){
      ip->addrs[NDIRECT+1] = addr = balloc(ip->dev);
    }

    //第一级寻址
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if ((addr = a[bn/NINDIRECT_2]) == 0){
      a[bn/NINDIRECT_2] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    
    //第2级寻址
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if ((addr = a[bn%NINDIRECT_2]) == 0){
      a[bn%NINDIRECT_2] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).     截断 inode（丢弃其内容）。
// Caller must hold ip->lock.
// 清除inode的内容(addr以及对应磁盘块的bitmap)
void itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;
  uint *a2;
  struct buf *bp_tmp;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  //清除2级寻址
  if (ip->addrs[NDIRECT+1]){
    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    for (int i = 0; i < NINDIRECT_2; i++) {
      if (a[i]){
        bp_tmp = bread(ip->dev, a[i]);
        a2 = (uint*)bp_tmp->data;
        for (j = 0; j < NINDIRECT_2; j++){
          if (a2[j])
            bfree(ip->dev, a2[j]);
        }
        brelse(bp_tmp);
        bfree(ip->dev, a[i]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT+1]);
    ip->addrs[NDIRECT+1] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
//将inode磁盘块的管理状态复制进st
void stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
// 从inode的偏移off所在磁盘块开始读取数据到buf，将buf磁盘块的数据拷贝至dst
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address；otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n, there was an error of some kind.
// 将src的数据写入inode文件的off偏移处，然后更新文件大小
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories
// 字符串比较(用于目录)
int namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// 在inode的数据区域遍历dirent，找到name所在的dirent，返回对应的inode指针
struct inode* dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// 在inode中创建一个新的name目录项，设置目录inum
int dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
// 路径

// 将 path 中的下一个路径组成部分复制到 name 中。
// 返回一个指向被复制部分之后的路径的指针。
// 返回的 path 不包含开头的斜杠，
// 因此调用者可以通过检查 *path == '\0' 来判断
// 当前名称是否是路径中的最后一个组成部分。
// 如果没有可提取的名称，则返回 0。
//
// 示例：
//   skipelem("a/bb/c", name) = "bb/c"，同时将 name 设置为 "a"
//   skipelem("///a//bb", name) = "bb"，同时将 name 设置为 "a"
//   skipelem("a", name) = ""，同时将 name 设置为 "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
// 分离出第一段路径给name，返回剩下的路径
static char* skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}


// 根据路径名查找并返回对应的 inode。
// 如果 parent != 0，则返回父目录对应的 inode，
// 并将路径的最后一个组成部分复制到 name 中。
// name 必须有足够的空间容纳 DIRSIZ 个字节。
// 由于该函数会调用 iput()，因此必须在事务内部调用。
// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
// 根据path查找inode；根据nameiparent返回对应的inode或者上一级inode
static struct inode* namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode* namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode* nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}


// Inode。
//
// 一个 inode 描述一个没有名称的文件。
// 磁盘上的 inode 结构保存文件的元数据，包括：文件类型、
// 文件大小、指向该 inode 的链接数量，以及存放文件内容的磁盘块列表。
//
// inode 从 sb.startinode 指定的位置开始，在磁盘上连续排列。
// 每个 inode 都有一个编号，用于表示它在磁盘上的位置。
//
// 内核会在内存中缓存正在使用的 inode，
// 为多个进程访问同一个 inode 提供同步机制。
// 缓存中的 inode 还包含一些不会存储在磁盘上的管理信息：
// ip->ref 和 ip->valid。
//
// 一个 inode 及其内存表示，在能够被文件系统其他代码使用之前，
// 会经历一系列状态。
//
// * 分配（Allocation）：如果 inode 在磁盘上的 type 不为 0，
//   就表示该 inode 已经被分配。
//   ialloc() 负责分配 inode；当引用计数和链接计数都降为 0 时，
//   iput() 负责释放 inode。
//
// * 缓存引用（Referencing in cache）：
//   如果 ip->ref 为 0，表示 inode Cache 中的这个缓存条目是空闲的。
//   否则，ip->ref 记录指向该缓存条目的内存指针数量
//   （例如打开的文件和当前目录）。
//   iget() 查找或创建一个缓存条目，并增加其引用计数；
//   iput() 则减少引用计数。
//
// * 有效性（Valid）：
//   只有当 ip->valid 为 1 时，inode 缓存条目中的信息
//   （如 type、size 等）才是正确的。
//   ilock() 从磁盘读取 inode，并设置 ip->valid；
//   当 ip->ref 降为 0 时，iput() 会清除 ip->valid。
//
// * 加锁（Locked）：
//   文件系统代码只有先锁定 inode，才能检查和修改
//   inode 中的信息及其文件内容。
//
// 因此，一个典型的操作流程是：
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... 检查和修改 ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() 与 iget() 分开实现，是为了让系统调用能够
// 长期持有 inode 的引用（例如打开的文件），
// 而只在短时间内对其加锁（例如执行 read() 时）。
//
// 这种分离也有助于避免路径名查找过程中出现死锁和竞态条件。
// iget() 会增加 ip->ref，使 inode 保留在缓存中，
// 并确保指向它的指针保持有效。
//
// 许多文件系统内部函数要求调用者事先锁定相关 inode，
// 这样调用者就可以组织多个步骤构成的原子操作。
//
// icache.lock 自旋锁用于保护 inode Cache 条目的分配。
// 由于 ip->ref 表示一个缓存条目是否空闲，
// 而 ip->dev 和 ip->inum 表示该条目缓存的是哪个 inode，
// 因此访问这些字段时必须持有 icache.lock。
//
// ip->lock 睡眠锁用于保护除 ref、dev 和 inum 之外的
// 所有 ip-> 字段。
// 要读取或修改 inode 的 ip->valid、ip->size、ip->type
// 等字段，必须先持有 ip->lock。