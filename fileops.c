#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>

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
	ssize_t ret;
	int memalloc;

	oldfs = get_fs();
	set_fs(get_ds());
        memalloc = set_memalloc();
	ret = vfs_read(file, (char __user *)data, count, pos);
        clear_memalloc(memalloc);
	set_fs(oldfs);

	return ret;
}

static ssize_t
file_write(struct file *file, const void *data, size_t count, loff_t *pos)
{
	mm_segment_t old_fs;
	ssize_t size;
	int memalloc;

        old_fs = get_fs();
        set_fs(get_ds());
        memalloc = set_memalloc();
        size = vfs_write(file, (const char __user *)data, count, pos);
        clear_memalloc(memalloc);
        set_fs(old_fs);

        return size;
}

static int file_sync(struct file *file)
{
	return vfs_fsync(file, 0);
}

static const char *string = "Evil file.";

static int __init write_init(void)
{
	struct file *file;
	loff_t pos = 0;
	int ret;

	file = file_open("/tmp/test", O_WRONLY | O_CREAT, 0600);
	if (IS_ERR(file))
		return PTR_ERR(file);
	ret = file_write(file, string, strlen(string), &pos);
	if (!ret)
		file_sync(file);
	file_close(file);

	return 0;
}

static void __exit write_exit(void)
{
}

module_init(write_init);
module_exit(write_exit);

MODULE_LICENSE("GPL");
