#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"

void init_entry(int idx)
{
    cache_array[idx].is_free = true;
    cache_array[idx].open_cnt = 0;
    cache_array[idx].dirty = false;
    cache_array[idx].accessed = false;
}

void init_cache(void)
{
    int i;
    lock_init(&cache_lock); //
    for (i = 0; i < CACHE_MAX_SIZE; i++)
        init_entry(i);
    thread_create("cache_writeback", 0, func_periodic_writer, NULL);
}

int get_cache_entry(block_sector_t disk_sector)
{
    int i;
    for (i = 0; i < CACHE_MAX_SIZE; i++)
    {
        if (cache_array[i].disk_sector == disk_sector)
        {
            if (!cache_array[i].is_free)
            {
                return i;
            }
        }
    }
    return -1;
}

int get_free_entry(void)
{
    int i;
    for (i = 0; i < CACHE_MAX_SIZE; i++)
    {
        if (cache_array[i].is_free == true)
        {
            cache_array[i].is_free = false;
            return i;
        }
    }

    return -1;
}

int access_cache_entry(block_sector_t disk_sector, bool dirty)
{
    lock_acquire(&cache_lock); //

    int idx = get_cache_entry(disk_sector);
    if (idx == -1)
        idx = replace_cache_entry(disk_sector, dirty);
    else
    {
        cache_array[idx].open_cnt++;
        cache_array[idx].accessed = true;
        cache_array[idx].dirty |= dirty;
    }

    lock_release(&cache_lock); //
    return idx;
}

int replace_cache_entry(block_sector_t disk_sector, bool dirty)
{
    int idx = get_free_entry();
    int i = 0;
    if (idx == -1) // 如果Cache数组以及满了那么就执行加强版时钟算法进行Cache块的替换
    {
        for (i = 0;; i = (i + 1) % CACHE_MAX_SIZE)
        {
            // 如果当前块正在被使用那么跳过
            if (cache_array[i].open_cnt > 0)
                continue;

            // 如果当前块的使用位为1意味着最近被使用那么给予第二次机会
            if (cache_array[i].accessed == true)
                cache_array[i].accessed = false;

            // 否则进行替换
            else
            {
                // 如果当前块的dirty位为true意味着被修改那么就将当前块写回磁盘
                if (cache_array[i].dirty == true)
                    block_write(fs_device, cache_array[i].disk_sector,
                                &cache_array[i].block);
                // 重新初始化当前块
                init_entry(i);
                idx = i;
                break;
            }
        }
    }

    cache_array[idx].disk_sector = disk_sector;
    cache_array[idx].is_free = false;
    cache_array[idx].open_cnt++;
    cache_array[idx].accessed = true;
    cache_array[idx].dirty = dirty;
    block_read(fs_device, cache_array[idx].disk_sector, &cache_array[idx].block); //

    return idx;
}

void func_periodic_writer(void *aux UNUSED)
{
    while (true)
    {
        timer_sleep(4 * TIMER_FREQ);
        write_back(false);
    }
}

void write_back(bool clear)
{
    int i;
    lock_acquire(&cache_lock); //

    for (i = 0; i < CACHE_MAX_SIZE; i++)
    {
        if (cache_array[i].dirty == true)
        {
            block_write(fs_device, cache_array[i].disk_sector, &cache_array[i].block);
            cache_array[i].dirty = false;
        }

        // 在filesystem done的时候将缓存块清空
        if (clear)
        {
            init_entry(i);
        }
    }

    lock_release(&cache_lock); //
}

void func_read_ahead(void *aux)
{
    block_sector_t disk_sector = *(block_sector_t *)aux;
    lock_acquire(&cache_lock); //

    int idx = get_cache_entry(disk_sector);

    // 需要置换
    if (idx == -1)
        replace_cache_entry(disk_sector, false);

    lock_release(&cache_lock); //
    free(aux);
}

void ahead_reader(block_sector_t disk_sector)
{
    block_sector_t *arg = malloc(sizeof(block_sector_t));
    *arg = disk_sector + 1; // next block
    thread_create("cache_read_ahead", 0, func_read_ahead, arg);
}
