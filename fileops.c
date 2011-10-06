/*
 * fileops: filesystem operations from kernel space
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 *
 * Copyright (C) 2011 Andrea Righi <andrea@betterlinux.com>
 *
 * === WARNING ===
 *
 * Sit down and THINK before using this approach in your code!
 *
 * Reading and writing a file within the kernel is a bad, bad, BAD thing to do.
 *
 * You should never write a module that requires reading or writing to a file.
 * There are well-designed interfaces to exchange informations between kernel
 * and userspace: procfs, sysfs, block/char devices, etc...
 *
 * That said, it is actually possible to do file I/O in the kernel, and this is
 * an example, but doing so is a severe violation of standard practice and it
 * can also lead to races and crashes.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>

/* Support file operations for up to 16 pages of data */
#define MAX_HELD_PAGES 16

static const char *file_name = "/tmp/test.txt";
static const char *file_content = "Evil file. Created from kernel space...";

/*
 * Pool to get page cache pages in advance to provide NOFS memory allocation.
 */
struct pagecache_pool {
	struct page *held_pages[MAX_HELD_PAGES];
	int held_cnt;
};
static struct pagecache_pool page_pool;

/* Release page cache pages from the page pool */
static void put_pages(struct pagecache_pool *pp)
{
	int i;

	for (i = 0; i < pp->held_cnt; i++)
		page_cache_release(pp->held_pages[i]);
}

/* Pre-allocate page pool pages */
static int get_pages(struct pagecache_pool *pp,
			struct file *file, size_t count, loff_t pos)
{
	pgoff_t index, start_index, end_index;
	struct page *page;
	struct address_space *mapping = file->f_mapping;

	start_index = pos >> PAGE_CACHE_SHIFT;
	end_index = (pos + count - 1) >> PAGE_CACHE_SHIFT;
	if (end_index - start_index + 1 > MAX_HELD_PAGES)
		return -EFBIG;
	pp->held_cnt = 0;
	for (index = start_index; index <= end_index; index++) {
		page = find_get_page(mapping, index);
		if (page == NULL) {
			page = find_or_create_page(mapping, index, GFP_NOFS);
			if (page == NULL) {
				write_inode_now(mapping->host, 1);
				page = find_or_create_page(mapping, index, GFP_NOFS);
			}
			if (page == NULL) {
				put_pages(pp);
				return -ENOMEM;
			}
			unlock_page(page);
		}
		pp->held_pages[pp->held_cnt++] = page;
	}
	return 0;
}

/* Current task should never get caught in the normal page freeing logic */
static int set_memalloc(void)
{
	if (current->flags & PF_MEMALLOC)
		return 0;
	current->flags |= PF_MEMALLOC;
	return 1;
}

/* Restore old PF_MEMALLOC value */
static void clear_memalloc(int memalloc)
{
	if (memalloc)
		current->flags &= ~PF_MEMALLOC;
}

/* Open a file from kernel space. Yay! */
static struct file *file_open(const char *filename, int flags, int mode)
{
	struct file *file;

	file = filp_open(filename, flags, mode);
	if (IS_ERR(file))
		return file;
	if (!file->f_op ||
			(!file->f_op->read && !file->f_op->aio_read) ||
			(!file->f_op->write && !file->f_op->aio_write)) {
		filp_close(file, NULL);
		file = ERR_PTR(-EINVAL);
	}

	return file;
}

/* Close a file handle */
static void file_close(struct file *file)
{
	if (file)
		filp_close(file, NULL);
}

/* Read some data from a opened file handle */
static ssize_t
file_read(struct file *file, void *data, size_t count, loff_t *pos)
{
	mm_segment_t oldfs;
	ssize_t size;
	int memalloc;

	size = get_pages(&page_pool, file, count, *pos);
	if (size < 0)
		return size;
	oldfs = get_fs();
	set_fs(get_ds());
        memalloc = set_memalloc();
	size = vfs_read(file, (char __user *)data, count, pos);
        clear_memalloc(memalloc);
	set_fs(oldfs);
	put_pages(&page_pool);

	return size;
}

/* Write some data to a opened file handle */
static ssize_t
file_write(struct file *file, const void *data, size_t count, loff_t *pos)
{
	mm_segment_t old_fs;
	ssize_t size;
	int memalloc;

	size = get_pages(&page_pool, file, count, *pos);
	if (size < 0)
		return size;
        old_fs = get_fs();
        set_fs(get_ds());
        memalloc = set_memalloc();
        size = vfs_write(file, (const char __user *)data, count, pos);
        clear_memalloc(memalloc);
        set_fs(old_fs);
	put_pages(&page_pool);

        return size;
}

/* Be sure cached data are written to the device */
static int file_sync(struct file *file)
{
	return vfs_fsync(file, 0);
}

/* Test routine: create a file and write some data */
static int test_write(void)
{
	struct file *file;
	loff_t pos = 0;
	int ret;

	file = file_open(file_name, O_WRONLY | O_CREAT, 0600);
	if (IS_ERR(file))
		return PTR_ERR(file);
	ret = file_write(file, file_content, strlen(file_content), &pos);
	if (!ret)
		file_sync(file);
	file_close(file);

	return 0;
}

/* Test routine: read some data from a previously created file */
static int test_read(void)
{
	char *buf;
	struct file *file;
	loff_t pos = 0;
	int ret;

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;
	file = file_open(file_name, O_RDONLY, 0600);
	if (IS_ERR(file)) {
		printk(KERN_INFO "couldn't open file %s\n", file_name);
		ret = PTR_ERR(file);
		goto out;
	}
	ret = file_read(file, buf, PAGE_SIZE, &pos);
	if (ret > 0)
		printk(KERN_INFO "%s\n", buf);
	file_close(file);
out:
	kfree(buf);

	return ret;
}

static int __init fileops_init(void)
{
	return test_write();
}

static void __exit fileops_exit(void)
{
	test_read();
}

module_init(fileops_init);
module_exit(fileops_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrea Righi <andrea@betterlinux.com>");
MODULE_DESCRIPTION("Example of file operations from kernel space");
