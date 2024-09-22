#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"
/* Partition that contains the file system. */
struct block *fs_device;

static void do_format(void);

char *path_to_name(const char *path_name);
struct dir *path_to_dir(const char *path);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format)
{
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");
  init_cache(); // 初始化缓冲区
  inode_init();

  free_map_init();

  if (format)
    do_format();

  free_map_open();
  // 初始化文件系统的访问锁
  lock_init(&filesys_lock);
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void)
{
  write_back(true); // 将缓冲区的内容写回到磁盘中
  free_map_close();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
// 加入is_dir的判断
// 修改为父子目录条件下的文件的创建
bool filesys_create(const char *name, off_t initial_size, bool is_dir)
{
  block_sector_t inode_sector = 0;
  struct dir *dir = path_to_dir(name);  // 获取到name的最底层目录
  char *file_name = path_to_name(name); // 获取到name指定的文件名
  bool success = false;
  // 如果文件名不为.或者..那么就分配扇区、创建inode同时将该扇区添加到dir目录下
  if (strcmp(file_name, ".") != 0 && strcmp(file_name, "..") != 0)
  {
    success = (dir != NULL && free_map_allocate(1, &inode_sector) && inode_create(inode_sector, initial_size, is_dir) && dir_add(dir, file_name, inode_sector));
  }
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
  dir_close(dir);
  free(file_name);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
// 修改为父子目录条件下指定名称文件的打开
struct file *
filesys_open(const char *name)
{
  if (strlen(name) == 0)
    return NULL;
  struct dir *dir = path_to_dir(name);  // 获取到name的最底层目录
  char *file_name = path_to_name(name); // 获取到name指定的文件名
  struct inode *inode = NULL;

  if (dir != NULL)
  {
    // 如果文件名是..那么就获取当前目录的父目录
    if (strcmp(file_name, "..") == 0)
    {
      inode = dir_parent_inode(dir);
      if (!inode)
      {
        free(file_name);
        return NULL;
      }
    } // 如果当前目录是根目录同时文件名是空或者文件名是.都直接返回当前目录
    else if ((dir_is_root(dir) && strlen(file_name) == 0) ||
             strcmp(file_name, ".") == 0)
    {
      free(file_name);
      return (struct file *)dir;
    }
    else // 否则就在当前目录dir下查找名为filename的文件并存储到inode中
      dir_lookup(dir, file_name, &inode);
  }
  free(file_name);
  dir_close(dir);
  if (!inode)
    return NULL;

  if (inode_is_dir(inode))
    return (struct file *)dir_open(inode);
  return file_open(inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
// 修改为父子目录条件下文件的删除
bool filesys_remove(const char *name)
{
  struct dir *dir = path_to_dir(name);  // 获取到name的最底层目录
  char *file_name = path_to_name(name); // 获取到name指定的文件名
  bool success = dir != NULL && dir_remove(dir, file_name);
  dir_close(dir);
  free(file_name);
  return success;
}

/* Formats the file system. */
static void
do_format(void)
{
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}
// 将当前正在运行的线程的目录更改为指定目录
bool filesys_chdir(const char *path)
{
  struct dir *dir = path_to_dir(path); // 获取到name的最底层目录
  char *name = path_to_name(path);     // 获取到name指定的文件名
  struct inode *inode = NULL;
  // 如果根据path得到的dir为null那么直接返回false
  if (dir == NULL)
  {
    free(name);
    return false;
  }
  // 如果文件名是..那么就获取当前目录的父目录
  else if (strcmp(name, "..") == 0)
  {
    inode = dir_parent_inode(dir);
    if (inode == NULL)
    {
      free(name);
      return false;
    }
  }
  // 如果文件名是.或者文件名长度为0同时为根目录那么就将当前运行的线程的目录改为dir即可
  else if (strcmp(name, ".") == 0 || (strlen(name) == 0 && dir_is_root(dir)))
  {
    thread_current()->dir = dir;
    free(name);
    return true;
  }
  else // 否则从dir目录中查找name名称的目录
    dir_lookup(dir, name, &inode);

  dir_close(dir);

  // 打开指定目录，这一步和上面filesys_open先判断is_dir再open效果是一样的因为这里还加入了null的判断
  dir = dir_open(inode);

  if (dir == NULL)
  {
    free(name);
    return false;
  }
  else
  {
    dir_close(thread_current()->dir);
    thread_current()->dir = dir;
    free(name);
    return true;
  }
}
// 根据path_name获取到该路径结尾的文件/目录名称
char *
path_to_name(const char *path_name)
{
  int length = strlen(path_name);
  char path[length + 1];
  memcpy(path, path_name, length + 1);
  // 获取到path_name最后一个/之后的内容并赋值给prev
  char *cur, *ptr, *prev = "";
  for (cur = strtok_r(path, "/", &ptr); cur != NULL; cur = strtok_r(NULL, "/", &ptr))
    prev = cur;

  char *name = malloc(strlen(prev) + 1);
  memcpy(name, prev, strlen(prev) + 1);
  return name;
}

// 根据path_name获取最底层的目录
struct dir *
path_to_dir(const char *path_name)
{
  int length = strlen(path_name);
  char path[length + 1];
  memcpy(path, path_name, length + 1);

  struct dir *dir;
  // 如果是绝对路径那么将dir初始化为根目录
  if (path[0] == '/' || !thread_current()->dir)
    dir = dir_open_root();
  else // 如果是相对路径那么将dir初始化为当前线程的目录
    dir = dir_reopen(thread_current()->dir);

  char *cur, *ptr, *prev;
  prev = strtok_r(path, "/", &ptr);
  for (cur = strtok_r(NULL, "/", &ptr); cur != NULL;
       prev = cur, cur = strtok_r(NULL, "/", &ptr))
  {
    struct inode *inode;
    // 一个.代表的是当前目录因此直接跳过
    if (strcmp(prev, ".") == 0)
      continue;
    else if (strcmp(prev, "..") == 0) // 两个.代表的是当前目录的父目录
    {
      inode = dir_parent_inode(dir);
      if (inode == NULL)
        return NULL;
    }
    else if (dir_lookup(dir, prev, &inode) == false) // 否则判断dir下是否存在名为prev的文件如果不存在则直接返回
      return NULL;
    // 如果存在那么判断该文件是否是目录，如果是目录那么将dir指向该目录
    if (inode_is_dir(inode))
    {
      dir_close(dir);
      dir = dir_open(inode);
    } // 否则继续
    else
      inode_close(inode);
  }

  return dir;
}
