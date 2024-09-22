#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "devices/timer.h"
#include "threads/synch.h"

#define CACHE_MAX_SIZE 64 // 最大Cache数组大小
// Cache块
struct disk_cache
{

    uint8_t block[BLOCK_SECTOR_SIZE]; // Cache块大小为磁盘中的块区大小即BLOCK_SECTOR_SIZE大小
    block_sector_t disk_sector;       // Cache块在磁盘中扇区编号

    bool is_free;  // 该Cache块是否空闲
    int open_cnt;  // 该Cache块被访问/打开的次数
    bool accessed; // Cache块是否被访问
    bool dirty;    // 该Cache块是否被写入
};

struct lock cache_lock;            // Cache的同步锁
struct disk_cache cache_array[64]; // Cache块组成的数组

// 初始化Cache数组中的每一块
void init_entry(int idx);
// 初始化Cache数组同时初始化Cache的同步锁
void init_cache(void);
// 根据磁盘中的扇区号获取Cache数组中对应块的索引
int get_cache_entry(block_sector_t disk_sector);
// 获取Cache数组中当前空闲的块
int get_free_entry(void);
// 访问指定扇区的Cache块并修改其对应的属性，如果不存在该扇区的Cache块那么就替换一个Cache块再修改
int access_cache_entry(block_sector_t disk_sector, bool dirty);
// Cache块的替换算法，基于访问位和修改位的时钟算法，性能最接近LRU
int replace_cache_entry(block_sector_t disk_sector, bool dirty);
// 每隔四个TIMER_FREQ将缓冲区中的数据写回磁盘
void func_periodic_writer(void *aux);
// 将Cache数组中所有dirty为true即被修改过的Cache块写回磁盘并更新dirty为false，根据是否clear来确定是否将该缓存快初始化
void write_back(bool clear);
// 预取策略，如果Cache数组中没有指定扇区块那么替换一个Cache块
void func_read_ahead(void *aux);
// 预取当前块的下一块到缓存区中
void ahead_reader(block_sector_t);

#endif
