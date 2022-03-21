/*
    Driver to convert input temperature to output temperature
    (e.g. Fahrenheit --> Celcius, Celcius --> Fahrenheit)
*/

#define pr_fmt(fmt) "%s:%s(): " fmt, KBUILD_MODNAME, __func__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>		// k[m|z]alloc(), k[z]free(), ...
#include <linux/mm.h>		// kvmalloc()
#include <linux/fs.h>		// the fops
#include <linux/sched.h>	// get_task_comm()

// copy_[to|from]_user()
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 11, 0)
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif

#define OURMODNAME "convert"
MODULE_AUTHOR("Omar Naffaa");
MODULE_DESCRIPTION("Converts given input temperature and writes output to kernel");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1");

struct drv_ctx {
	struct device *dev;
    int conversion_read_cnt;
    int conversion_write_cnt;
#define MAXBYTES 5 	// Input from userspace should not be bigger than 4 characters + NULL char (e.g. 100F)

	char converted_temp[MAXBYTES];
};
static struct drv_ctx *ctx;

static int open_miscdrv_rdwr(struct inode *inode, struct file *filp)
{
    struct device *dev = ctx->dev;
	char *buf = kzalloc(PATH_MAX, GFP_KERNEL);

	if (unlikely(!buf))
		return -ENOMEM;

    dev_info(dev, " opening \"%s\" now; wrt open file: f_flags = 0x%x\n", file_path(filp, buf, PATH_MAX), filp->f_flags);
	kfree(buf);

	return nonseekable_open(inode, filp); // do not support lseek() system call
}

static ssize_t read_miscdrv_rdwr(struct file *filp, char __user *ubuf,
				 size_t count, loff_t *off)
{
    int temp_len = strnlen(ctx->converted_temp, MAXBYTES);
	struct device *dev = ctx->dev;
	char tasknm[TASK_COMM_LEN];

    dev_info(dev, "%s wants to read (upto) %zu bytes\n", get_task_comm(tasknm, current), count);

	if (temp_len <= 0) {
		dev_warn(dev, "No temperature available, returning...\n");
		return -EINVAL;
	}

	if (copy_to_user(ubuf, ctx->converted_temp, temp_len)) {
		dev_warn(dev, "copy_to_user() failed\n");
		return -EFAULT;
	}

	// Update stats
	ctx->conversion_read_cnt += 1;
	dev_info(dev, " %d bytes read, returning... (stats: reads performed = %d, writes performed = %d)\n",
		temp_len, ctx->conversion_read_cnt, ctx->conversion_write_cnt);

	return temp_len; // return number of bytes written, per POSIX standards
}

static ssize_t write_miscdrv_rdwr(struct file *filp, const char __user *ubuf,
				  size_t count, loff_t *off)
{
	int ret;
	long new_temp;
	char unit;
	void *kbuf = NULL;
	struct device *dev = ctx->dev;
	char tasknm[TASK_COMM_LEN];

	if (unlikely(count > MAXBYTES)) {
		dev_warn(dev, "count %zu exceeds max # of bytes allowed, "
			"aborting write\n", count);
		return -ENOMEM;
	}
	dev_info(dev, "%s wants to write %zu bytes\n", get_task_comm(tasknm, current), count);

	kbuf = kzalloc(count, GFP_KERNEL);
	if (unlikely(!kbuf))
		return -ENOMEM;

	if (copy_from_user(kbuf, ubuf, count)) {
		dev_warn(dev, "copy_from_user() failed\n");
		kfree(kbuf);
        return -EFAULT;
	}
	strlcpy(ctx->converted_temp, kbuf, (count > MAXBYTES ? MAXBYTES : count));
	
	unit = ctx->converted_temp[3];
	ctx->converted_temp[3] = '\0'; // remove unit to allow char[] to int conversion

	ret = kstrtol(ctx->converted_temp, 10, &new_temp);
	if (ret) {
		dev_warn(dev, "Could not parse entered value into integer");
		return ret;
	}

	if(unit == 'F') {
		new_temp = (new_temp - 32) * 5/9;
		pr_info("%s Fahrenheit = approximately %ld Celsius", ctx->converted_temp, new_temp);
	} else if(unit == 'C') {
		new_temp = (new_temp * 9) / 5 + 32;
		pr_info("%s Celsius = approximately %ld Fahrenheit", ctx->converted_temp, new_temp);
	} else {
		pr_info("Could not convert temperature; inappropriate unit \"%c\" specified...", unit);
	}

	// Update stats
	ctx->conversion_write_cnt += 1;

	dev_info(dev, " %zu bytes written, returning... (stats: reads performed = %d, writes performed = %d)\n",
		count, ctx->conversion_read_cnt, ctx->conversion_write_cnt);

    return count; // return the number of bytes written, per POSIX standards
}

static int close_miscdrv_rdwr(struct inode *inode, struct file *filp)
{
    struct device *dev = ctx->dev;
	char *buf = kzalloc(PATH_MAX, GFP_KERNEL);

	if (unlikely(!buf))
		return -ENOMEM;

	dev_info(dev, " filename: \"%s\"\n", file_path(filp, buf, PATH_MAX));
	kfree(buf);

	return 0;
}

static const struct file_operations convert_misc_fops = {
    .open = open_miscdrv_rdwr,
	.read = read_miscdrv_rdwr,
	.write = write_miscdrv_rdwr,
	.llseek = no_llseek,	// dummy, we don't support lseek(2)
	.release = close_miscdrv_rdwr,
};

static struct miscdevice convert_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,	/* kernel dynamically assigns a free minor# */
	.name = "convertdrv",	        /* name to add driver as in /dev when misc_register() is invoked */
	.mode = 0666,		            /* dev node perms set as specified here */
	.fops = &convert_misc_fops,	    /* connect to this driver's 'functionality' */
};

static int __init convertdrv_init(void)
{
	int ret = 0;
	struct device *dev;

	ret = misc_register(&convert_miscdev);
	if (ret) {
		pr_notice("%s: misc device registration failed, aborting\n", OURMODNAME);
		return ret;
	}
	/* Retrieve the device pointer for this device */
	dev = convert_miscdev.this_device;

	pr_info("Temperature converter misc driver (major # 10) registered, minor# = %d,"
		" dev node is /dev/%s\n", convert_miscdev.minor, convert_miscdev.name);

	ctx = devm_kzalloc(dev, sizeof(struct drv_ctx), GFP_KERNEL);
	if (unlikely(!ctx))
		return -ENOMEM;
		
	ctx->dev = dev;

	// Initialize temperature buffer
	strlcpy(ctx->converted_temp, "None", 5);

	return 0;
}

static void __exit convertdrv_exit(void)
{
	misc_deregister(&convert_miscdev);
	pr_info("Temperature Converter driver successfully deregistered\n");
}

module_init(convertdrv_init);
module_exit(convertdrv_exit);