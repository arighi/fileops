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

static void put_pages(struct pagecache_pool *pp)
{
	int i;

	for (i = 0; i < pp->held_cnt; i++)
		page_cache_release(pp->held_pages[i]);
}

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

static int set_memalloc(void)
{
	if (current->flags & PF_MEMALLOC)
		return 0;
	current->flags |= PF_MEMALLOC;
	return 1;
}

static void clear_memalloc(int memalloc)
{
	if (memalloc)
		current->flags &= ~PF_MEMALLOC;
}

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

static void file_close(struct file *file)
{
	if (file)
		filp_close(file, NULL);
}

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

static int file_sync(struct file *file)
{
	return vfs_fsync(file, 0);
}

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
