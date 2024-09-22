#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/synch.h"
/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INDIRECT_PTRS 128 // 每个磁盘扇区指定为128个Block块

// 将文件通过索引来组织其Block块的分配，本来是直接分配一个扇区，这将形成许多外部碎片，现在通过分级索引来分页组织
#define DIRECT_BLOCKS 4        // 直接索引
#define INDIRECT_BLOCKS 9      // 一级索引
#define DOUBLE_DIRECT_BLOCKS 1 // 二级索引
#define INODE_PTRS 14          // 每个文件的索引数组总长度为14由4个直接索引+9个一级索引+1个二级索引组成

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  uint32_t unused[107]; /* Not used. */

  uint32_t direct_index;          // 直接索引
  uint32_t indirect_index;        // 一级索引
  uint32_t double_indirect_index; // 二级索引
  block_sector_t blocks[14];      // 索引数组
  bool is_dir;                    // 是否是目录
  block_sector_t parent;          // 父文件（目录）的扇区编号
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors(off_t size)
{
  return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem; /* Element in inode list. */
  block_sector_t sector; /* Sector number of disk location. */
  int open_cnt;          /* Number of openers. */
  bool removed;          /* True if deleted, false otherwise. */
  int deny_write_cnt;    /* 0: writes ok, >0: deny writes. */

  off_t length;                   // 文件长度
  off_t read_length;              // 文件的可读取长度
  uint32_t direct_index;          // 直接索引
  uint32_t indirect_index;        // 一级索引
  uint32_t double_indirect_index; // 二级索引
  block_sector_t blocks[14];      // 索引数组
  bool is_dir;                    // 是否是目录
  block_sector_t parent;          // 父文件（目录）的扇区编号
  struct lock lock;               // 文件锁
};

// 根据inode_disk的相关信息分配inode空间后将inode和inode_disk的信息同步
bool inode_alloc(struct inode_disk *inode_disk);
// 将指定的inode扩展到length长度
off_t inode_grow(struct inode *inode, off_t length);
// 释放inode所占据的空间
void inode_free(struct inode *inode);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
// 获取length长度的inode文件pos位置所处在的物理块扇区编号
static block_sector_t
byte_to_sector(const struct inode *inode, off_t length, off_t pos)
{
  ASSERT(inode != NULL);
  if (pos < length)
  {
    uint32_t idx;
    uint32_t blocks[INDIRECT_PTRS];

    // 直接索引能直接寻址
    if (pos < DIRECT_BLOCKS * BLOCK_SECTOR_SIZE)
    {
      return inode->blocks[pos / BLOCK_SECTOR_SIZE];
    }

    // 一级索引需要先将直接寻址获取到的物理块赋值给blocks数组后进行二次寻址
    else if (pos < (DIRECT_BLOCKS + INDIRECT_BLOCKS * INDIRECT_PTRS) * BLOCK_SECTOR_SIZE)
    {
      pos -= DIRECT_BLOCKS * BLOCK_SECTOR_SIZE;
      idx = pos / (INDIRECT_PTRS * BLOCK_SECTOR_SIZE) + DIRECT_BLOCKS;
      block_read(fs_device, inode->blocks[idx], &blocks);

      pos %= INDIRECT_PTRS * BLOCK_SECTOR_SIZE;
      return blocks[pos / BLOCK_SECTOR_SIZE];
    }

    // 二级索引需要先将直接寻址获取到的物理块赋值给blocks数组后再通过偏移量二次寻址后将该物理块再赋值给blocks数组进行三次寻址
    else
    {
      block_read(fs_device, inode->blocks[INODE_PTRS - 1], &blocks);

      pos -= (DIRECT_BLOCKS + INDIRECT_BLOCKS * INDIRECT_PTRS) * BLOCK_SECTOR_SIZE;
      idx = pos / (INDIRECT_PTRS * BLOCK_SECTOR_SIZE);
      block_read(fs_device, blocks[idx], &blocks);

      pos %= INDIRECT_PTRS * BLOCK_SECTOR_SIZE;
      return blocks[pos / BLOCK_SECTOR_SIZE];
    }
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void)
{
  list_init(&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
// 根据指定扇区号创建一个length长度的文件包括disk_inode和inode同时指定其是否是目录
bool inode_create(block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->is_dir = is_dir;          // 设置是否是目录
    disk_inode->parent = ROOT_DIR_SECTOR; // 设置其父目录为根目录
    // 根据disk_inode来生成inode并将inode的属性回调赋值给disk_inode
    if (inode_alloc(disk_inode))
    {
      block_write(fs_device, sector, disk_inode);
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open(block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e))
  {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector)
    {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  struct inode_disk inode_disk; // 定义一个inode_dick来获取指定扇区的内容

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);                           // 初始化该文件的文件锁
  block_read(fs_device, inode->sector, &inode_disk); // 将指定扇区的内容读取到inode_disk中
  // 将inode_disk的属性赋值给inode对应的属性
  inode->length = inode_disk.length;
  inode->read_length = inode_disk.length;
  inode->direct_index = inode_disk.direct_index;
  inode->indirect_index = inode_disk.indirect_index;
  inode->double_indirect_index = inode_disk.double_indirect_index;
  inode->is_dir = inode_disk.is_dir;
  inode->parent = inode_disk.parent;
  // 将inode_disk的索引数组拷贝给inode的索引数组
  memcpy(&inode->blocks, &inode_disk.blocks, INODE_PTRS * sizeof(block_sector_t));
  return inode; // 完成了属性转移后返回该inode
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen(struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber(const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode)
{
  struct inode_disk inode_disk;
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed)
    {
      free_map_release(inode->sector, 1);
      inode_free(inode);
    }
    else // inode的内容可能发生了更改，首先将其写入到inode_disk中此后写回到磁盘指定扇区
    {
      inode_disk.length = inode->length;
      inode_disk.magic = INODE_MAGIC;
      inode_disk.direct_index = inode->direct_index;
      inode_disk.indirect_index = inode->indirect_index;
      inode_disk.double_indirect_index = inode->double_indirect_index;
      inode_disk.is_dir = inode->is_dir;
      inode_disk.parent = inode->parent;
      memcpy(&inode_disk.blocks, &inode->blocks, INODE_PTRS * sizeof(block_sector_t));
      block_write(fs_device, inode->sector, &inode_disk);
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode)
{
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
// 从inode的offset开始读取size个buffer中的内容，需要考虑到offset+size超出inode的总长度这种情况
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  off_t length = inode->read_length; // 获取到inode的可读取总长度

  if (offset >= length)
    return 0;

  while (size > 0)
  {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, length, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = length - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;
    // 将本来需要通过系统调用实现的读取转换为从缓冲区中进行读取
    int cache_idx = access_cache_entry(sector_idx, false);
    memcpy(buffer + bytes_read, cache_array[cache_idx].block + sector_ofs,
           chunk_size);
    cache_array[cache_idx].accessed = true;
    cache_array[cache_idx].open_cnt--;
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  inode->read_length = inode_length(inode);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
// 从inode的offset位置开始将buffer缓冲区中的size个byte写入扇区，需要考虑offset+size大于inode的总长度
// 在扩容的时候需要加锁
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;
  inode_deny_write(inode);
  // 如果offset+size大于inode的总长度那么就将inode进行扩容
  if (offset + size > inode_length(inode))
  {
    if (!inode->is_dir)
      lock_acquire(&inode->lock);
    inode->length = inode_grow(inode, offset + size);
    if (!inode->is_dir)
      lock_release(&inode->lock);
  }

  while (size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, inode_length(inode), offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    // 将本来需要通过系统调用实现的写入转换为写入缓冲区
    int cache_idx = access_cache_entry(sector_idx, true);
    memcpy(cache_array[cache_idx].block + sector_ofs, buffer + bytes_written, chunk_size);
    cache_array[cache_idx].accessed = true;
    cache_array[cache_idx].dirty = true;
    cache_array[cache_idx].open_cnt--;
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  inode_allow_write(inode);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode)
{
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
// 直接返回inode的长度
off_t inode_length(const struct inode *inode)
{
  return inode->length;
}

// 根据inode_disk来生成一个inode并将inode的数据回调给inode_dick
bool inode_alloc(struct inode_disk *inode_disk)
{
  struct inode inode;
  inode.length = 0;
  inode.direct_index = 0;
  inode.indirect_index = 0;
  inode.double_indirect_index = 0;

  inode_grow(&inode, inode_disk->length);
  inode_disk->direct_index = inode.direct_index;
  inode_disk->indirect_index = inode.indirect_index;
  inode_disk->double_indirect_index = inode.double_indirect_index;
  memcpy(&inode_disk->blocks, &inode.blocks, INODE_PTRS * sizeof(block_sector_t));
  return true;
}
// 将inode的长度扩展到指定的length长度
off_t inode_grow(struct inode *inode, off_t length)
{
  static char zeros[BLOCK_SECTOR_SIZE];

  size_t grow_sectors = bytes_to_sectors(length) - bytes_to_sectors(inode->length);

  if (grow_sectors == 0)
  {
    return length;
  }

  // 直接索引
  while (inode->direct_index < DIRECT_BLOCKS && grow_sectors != 0)
  {
    free_map_allocate(1, &inode->blocks[inode->direct_index]);
    block_write(fs_device, inode->blocks[inode->direct_index], zeros);
    inode->direct_index++;
    grow_sectors--;
  }

  // 一级索引
  while (inode->direct_index < DIRECT_BLOCKS + INDIRECT_BLOCKS && grow_sectors != 0)
  {
    block_sector_t blocks[128];

    // 为一级索引本身分配扇区编号
    if (inode->indirect_index == 0)
      free_map_allocate(1, &inode->blocks[inode->direct_index]);
    else
      block_read(fs_device, inode->blocks[inode->direct_index], &blocks);

    // 为一级索引对应的扇区中的每一个分区分配扇区编号
    while (inode->indirect_index < INDIRECT_PTRS && grow_sectors != 0)
    {
      free_map_allocate(1, &blocks[inode->indirect_index]);
      block_write(fs_device, blocks[inode->indirect_index], zeros);
      inode->indirect_index++;
      grow_sectors--;
    }

    // 将blocks数组写入扇区
    block_write(fs_device, inode->blocks[inode->direct_index], &blocks);

    // 下一轮一级索引的循环
    if (inode->indirect_index == INDIRECT_PTRS)
    {
      inode->indirect_index = 0;
      inode->direct_index++;
    }
  }

  // 二级索引
  if (inode->direct_index == INODE_PTRS - 1 && grow_sectors != 0)
  {
    block_sector_t level_one[128];
    block_sector_t level_two[128];

    // 为二级索引本身分配扇区编号
    if (inode->double_indirect_index == 0 && inode->indirect_index == 0)
      free_map_allocate(1, &inode->blocks[inode->direct_index]);
    else
      block_read(fs_device, inode->blocks[inode->direct_index], &level_one);

    // 为二级索引对应的扇区中的每一个分区分配扇区编号
    while (inode->indirect_index < INDIRECT_PTRS && grow_sectors != 0)
    {
      if (inode->double_indirect_index == 0)
        free_map_allocate(1, &level_one[inode->indirect_index]);
      else
        block_read(fs_device, level_one[inode->indirect_index], &level_two);

      while (inode->double_indirect_index < INDIRECT_PTRS && grow_sectors != 0)
      {
        free_map_allocate(1, &level_two[inode->double_indirect_index]);
        block_write(fs_device, level_two[inode->double_indirect_index], zeros);
        inode->double_indirect_index++;
        grow_sectors--;
      }

      // 将level_two数组写入扇区
      block_write(fs_device, level_one[inode->indirect_index], &level_two);

      // 下一轮二级索引的循环
      if (inode->double_indirect_index == INDIRECT_PTRS)
      {
        inode->double_indirect_index = 0;
        inode->indirect_index++;
      }
    }

    // 将level_one数组写入扇区
    block_write(fs_device, inode->blocks[inode->direct_index], &level_one);
  }

  return length;
}

// 将指定的inode中所占据的所有扇区内容释放
void inode_free(struct inode *inode)
{
  size_t sector_num = bytes_to_sectors(inode->length);
  size_t idx = 0;

  if (sector_num == 0)
  {
    return;
  }

  // 释放直接索引
  while (idx < DIRECT_BLOCKS && sector_num != 0)
  {
    free_map_release(inode->blocks[idx], 1);
    sector_num--;
    idx++;
  }

  // 释放一级索引
  while (inode->direct_index >= DIRECT_BLOCKS &&
         idx < DIRECT_BLOCKS + INDIRECT_BLOCKS && sector_num != 0)
  {
    size_t free_blocks = sector_num < INDIRECT_PTRS ? sector_num : INDIRECT_PTRS;

    size_t i;
    block_sector_t block[128];
    block_read(fs_device, inode->blocks[idx], &block);

    for (i = 0; i < free_blocks; i++)
    {
      free_map_release(block[i], 1);
      sector_num--;
    }

    free_map_release(inode->blocks[idx], 1);
    idx++;
  }

  // 释放二级索引
  if (inode->direct_index == INODE_PTRS - 1)
  {
    size_t i, j;
    block_sector_t level_one[128], level_two[128];

    // 读取level_one索引数组
    block_read(fs_device, inode->blocks[INODE_PTRS - 1], &level_one);

    size_t indirect_blocks = DIV_ROUND_UP(sector_num, INDIRECT_PTRS * BLOCK_SECTOR_SIZE);

    for (i = 0; i < indirect_blocks; i++)
    {
      size_t free_blocks = sector_num < INDIRECT_PTRS ? sector_num : INDIRECT_PTRS;

      // 读取level_two索引数组
      block_read(fs_device, level_one[i], &level_two);

      for (j = 0; j < free_blocks; j++)
      {
        free_map_release(level_two[j], 1);
        sector_num--;
      }

      free_map_release(level_one[i], 1);
    }

    free_map_release(inode->blocks[INODE_PTRS - 1], 1);
  }
}

bool inode_is_dir(const struct inode *inode)
{
  return inode->is_dir;
}

int inode_get_open_cnt(const struct inode *inode)
{
  return inode->open_cnt;
}

block_sector_t
inode_get_parent(const struct inode *inode)
{
  return inode->parent;
}

bool inode_set_parent(block_sector_t parent, block_sector_t child)
{
  struct inode *inode = inode_open(child);

  if (!inode)
    return false;

  inode->parent = parent;
  inode_close(inode);
  return true;
}

void inode_lock(const struct inode *inode)
{
  lock_acquire(&((struct inode *)inode)->lock);
}

void inode_unlock(const struct inode *inode)
{
  lock_release(&((struct inode *)inode)->lock);
}