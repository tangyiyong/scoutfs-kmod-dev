#include <linux/kernel.h>
#include <linux/fs.h>

#include "msg.h"

/*
 * This can be called with pre-emption disabled if the caller is printing
 * the contents of formated per-cpu key string buffers.
 */
void scoutfs_msg(struct super_block *sb, const char *prefix, const char *str,
		 const char *fmt, ...)
{
        struct va_format vaf;
        va_list args;

        va_start(args, fmt);

        vaf.fmt = fmt;
        vaf.va = &args;

        printk("%sscoutfs (%s %u:%u)%s: %pV\n", prefix,
	       sb->s_id, MAJOR(sb->s_bdev->bd_dev), MINOR(sb->s_bdev->bd_dev),
	       str, &vaf);

        va_end(args);
}
