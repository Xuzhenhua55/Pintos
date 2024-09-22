#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;

void inode_init(void);
bool inode_create(block_sector_t, off_t, bool);
struct inode *inode_open(block_sector_t);
struct inode *inode_reopen(struct inode *);
block_sector_t inode_get_inumber(const struct inode *);
void inode_close(struct inode *);
void inode_remove(struct inode *);
off_t inode_read_at(struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at(struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write(struct inode *);
void inode_allow_write(struct inode *);
off_t inode_length(const struct inode *);

// 判断inode是否是目录
bool inode_is_dir(const struct inode *);
// 获取inode的open_cnt值
int inode_get_open_cnt(const struct inode *);
// 获取inode的父目录所在扇区编号
block_sector_t inode_get_parent(const struct inode *);
// 将指定的parent扇区作为child扇区的父文件/目录
bool inode_set_parent(block_sector_t parent, block_sector_t child);
// 为inode加锁
void inode_lock(const struct inode *inode);
// 为inode解锁
void inode_unlock(const struct inode *inode);
#endif /* filesys/inode.h */
