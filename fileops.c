#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>

static const char *file_name = "/tmp/test";
static const char *file_content = "Evil file.";

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
