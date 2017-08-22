/*
 * Copyright (C) 2015 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/xattr.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sched.h>

#include "format.h"
#include "super.h"
#include "key.h"
#include "inode.h"
#include "dir.h"
#include "data.h"
#include "scoutfs_trace.h"
#include "xattr.h"
#include "trans.h"
#include "msg.h"
#include "kvec.h"
#include "item.h"
#include "client.h"

/*
 * XXX
 *  - worry about i_ino trunctation, not sure if we do anything
 *  - use inode item value lengths for forward/back compat
 */

struct free_ino_pool {
	wait_queue_head_t waitq;
	spinlock_t lock;
	u64 ino;
	u64 nr;
	bool in_flight;
};

struct inode_sb_info {
	struct free_ino_pool pool;

	spinlock_t writeback_lock;
	struct rb_root writeback_inodes;
};

#define DECLARE_INODE_SB_INFO(sb, name) \
	struct inode_sb_info *name = SCOUTFS_SB(sb)->inode_sb_info

static struct kmem_cache *scoutfs_inode_cachep;
static int scoutfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			   struct kstat *stat);

/*
 * This is called once before all the allocations and frees of a inode
 * object within a slab.  It's for inode fields that don't need to be
 * initialized for a given instance of an inode.
 */
static void scoutfs_inode_ctor(void *obj)
{
	struct scoutfs_inode_info *ci = obj;

	mutex_init(&ci->item_mutex);
	seqcount_init(&ci->seqcount);
	ci->staging = false;
	init_rwsem(&ci->xattr_rwsem);
	RB_CLEAR_NODE(&ci->writeback_node);

	inode_init_once(&ci->inode);
}

struct inode *scoutfs_alloc_inode(struct super_block *sb)
{
	struct scoutfs_inode_info *ci;

	ci = kmem_cache_alloc(scoutfs_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	return &ci->inode;
}

static void scoutfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	trace_printk("freeing inode %p\n", inode);
	kmem_cache_free(scoutfs_inode_cachep, SCOUTFS_I(inode));
}

static void insert_writeback_inode(struct inode_sb_info *inf,
				   struct scoutfs_inode_info *ins)
{
	struct rb_root *root = &inf->writeback_inodes;
	struct rb_node **node = &root->rb_node;
	struct rb_node *parent = NULL;
	struct scoutfs_inode_info *si;

	while (*node) {
		parent = *node;
		si = container_of(*node, struct scoutfs_inode_info,
				  writeback_node);

		if (ins->ino < si->ino)
			node = &(*node)->rb_left;
		else if (ins->ino > si->ino)
			node = &(*node)->rb_right;
		else
			BUG();
	}

	rb_link_node(&ins->writeback_node, parent, node);
	rb_insert_color(&ins->writeback_node, root);
}

static void remove_writeback_inode(struct inode_sb_info *inf,
			       struct scoutfs_inode_info *si)
{
	if (!RB_EMPTY_NODE(&si->writeback_node)) {
		rb_erase(&si->writeback_node, &inf->writeback_inodes);
		RB_CLEAR_NODE(&si->writeback_node);
	}
}

void scoutfs_destroy_inode(struct inode *inode)
{
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);

	spin_lock(&inf->writeback_lock);
	remove_writeback_inode(inf, SCOUTFS_I(inode));
	spin_unlock(&inf->writeback_lock);

	call_rcu(&inode->i_rcu, scoutfs_i_callback);
}

static const struct inode_operations scoutfs_file_iops = {
	.getattr	= scoutfs_getattr,
	.setxattr	= scoutfs_setxattr,
	.getxattr	= scoutfs_getxattr,
	.listxattr	= scoutfs_listxattr,
	.removexattr	= scoutfs_removexattr,
	.fiemap		= scoutfs_data_fiemap,
};

static const struct inode_operations scoutfs_special_iops = {
	.setxattr	= scoutfs_setxattr,
	.getxattr	= scoutfs_getxattr,
	.listxattr	= scoutfs_listxattr,
	.removexattr	= scoutfs_removexattr,
};

/*
 * Called once new inode allocation or inode reading has initialized
 * enough of the inode for us to set the ops based on the mode.
 */
static void set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_mapping->a_ops = &scoutfs_file_aops;
		inode->i_op = &scoutfs_file_iops;
		inode->i_fop = &scoutfs_file_fops;
		break;
	case S_IFDIR:
		inode->i_op = &scoutfs_dir_iops;
		inode->i_fop = &scoutfs_dir_fops;
		break;
	case S_IFLNK:
		inode->i_op = &scoutfs_symlink_iops;
		break;
	default:
		inode->i_op = &scoutfs_special_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}

	/* ephemeral data items avoid kmap for pointers to page contents */
	mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
}

/*
 * The caller has ensured that the fields in the incoming scoutfs inode
 * reflect both the inode item and the inode index items.  This happens
 * when reading, refreshing, or updating the inodes.  We set the inode
 * info fields to match so that next time we try to update the inode we
 * can tell which fields have changed.
 */
static void set_item_info(struct scoutfs_inode_info *si,
			  struct scoutfs_inode *sinode)
{
	BUG_ON(!mutex_is_locked(&si->item_mutex));

	si->have_item = true;
	si->item_size = le64_to_cpu(sinode->size);
	si->item_ctime.tv_sec = le64_to_cpu(sinode->ctime.sec);
	si->item_ctime.tv_nsec = le32_to_cpu(sinode->ctime.nsec);
	si->item_mtime.tv_sec = le64_to_cpu(sinode->mtime.sec);
	si->item_mtime.tv_nsec = le32_to_cpu(sinode->mtime.nsec);
	si->item_meta_seq = le64_to_cpu(sinode->meta_seq);
	si->item_data_seq = le64_to_cpu(sinode->data_seq);
}

static void load_inode(struct inode *inode, struct scoutfs_inode *cinode)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);

	i_size_write(inode, le64_to_cpu(cinode->size));
	set_nlink(inode, le32_to_cpu(cinode->nlink));
	i_uid_write(inode, le32_to_cpu(cinode->uid));
	i_gid_write(inode, le32_to_cpu(cinode->gid));
	inode->i_mode = le32_to_cpu(cinode->mode);
	inode->i_rdev = le32_to_cpu(cinode->rdev);
	inode->i_atime.tv_sec = le64_to_cpu(cinode->atime.sec);
	inode->i_atime.tv_nsec = le32_to_cpu(cinode->atime.nsec);
	inode->i_mtime.tv_sec = le64_to_cpu(cinode->mtime.sec);
	inode->i_mtime.tv_nsec = le32_to_cpu(cinode->mtime.nsec);
	inode->i_ctime.tv_sec = le64_to_cpu(cinode->ctime.sec);
	inode->i_ctime.tv_nsec = le32_to_cpu(cinode->ctime.nsec);

	ci->meta_seq = le64_to_cpu(cinode->meta_seq);
	ci->data_seq = le64_to_cpu(cinode->data_seq);
	ci->data_version = le64_to_cpu(cinode->data_version);
	ci->next_readdir_pos = le64_to_cpu(cinode->next_readdir_pos);

	set_item_info(ci, cinode);
}

static int refresh_inode(struct inode *inode, struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_key_buf key;
	struct scoutfs_inode_key ikey;
	struct scoutfs_inode sinode;
	SCOUTFS_DECLARE_KVEC(val);
	int ret;

	scoutfs_inode_init_key(&key, &ikey, scoutfs_ino(inode));
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	mutex_lock(&si->item_mutex);
	ret = scoutfs_item_lookup_exact(sb, &key, val, sizeof(sinode), lock->end);
	if (ret == 0)
		load_inode(inode, &sinode);
	mutex_unlock(&si->item_mutex);

	return ret;
}

void scoutfs_inode_init_key(struct scoutfs_key_buf *key,
			    struct scoutfs_inode_key *ikey, u64 ino)
{
	ikey->zone = SCOUTFS_FS_ZONE;
	ikey->ino = cpu_to_be64(ino);
	ikey->type = SCOUTFS_INODE_TYPE;

	scoutfs_key_init(key, ikey, sizeof(struct scoutfs_inode_key));
}

static int scoutfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			   struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct scoutfs_lock *lock = NULL;
	int ret;

	ret = scoutfs_lock_ino_group(sb, DLM_LOCK_PR, scoutfs_ino(inode),
				     &lock);
	if (ret)
		return ret;

	ret = refresh_inode(inode, lock);
	if (ret == 0)
		generic_fillattr(inode, stat);

	scoutfs_unlock(sb, lock);
	return ret;
}

/*
 * Set a given seq to the current trans seq if it differs.  The caller
 * holds locks and a transaction which prevents the transaction from
 * committing and refreshing the seq.
 */
static void set_trans_seq(struct inode *inode, u64 *seq)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);

	if (*seq != sbi->trans_seq) {
		preempt_disable();
		write_seqcount_begin(&si->seqcount);
		*seq = sbi->trans_seq;
		write_seqcount_end(&si->seqcount);
		preempt_enable();
	}
}

void scoutfs_inode_set_meta_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	set_trans_seq(inode, &si->meta_seq);
}

void scoutfs_inode_set_data_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	set_trans_seq(inode, &si->data_seq);
}

void scoutfs_inode_inc_data_version(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	preempt_disable();
	write_seqcount_begin(&si->seqcount);
	si->data_version++;
	write_seqcount_end(&si->seqcount);
	preempt_enable();
}

static u64 read_seqcount_u64(struct inode *inode, u64 *val)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	unsigned int seq;
	u64 v;

	do {
		seq = read_seqcount_begin(&si->seqcount);
		v = *val;
	} while (read_seqcount_retry(&si->seqcount, seq));

	return v;
}

u64 scoutfs_inode_meta_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->meta_seq);
}

u64 scoutfs_inode_data_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->data_seq);
}

u64 scoutfs_inode_data_version(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->data_version);
}

static int scoutfs_iget_test(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 *ino = arg;

	return ci->ino == *ino;
}

static int scoutfs_iget_set(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 *ino = arg;

	inode->i_ino = *ino;
	ci->ino = *ino;

	return 0;
}

struct inode *scoutfs_iget(struct super_block *sb, u64 ino)
{
	struct inode *inode;
	struct scoutfs_lock *lock = NULL;
	int ret;

	ret = scoutfs_lock_ino_group(sb, DLM_LOCK_PR, ino, &lock);
	if (ret)
		return ERR_PTR(ret);

	inode = iget5_locked(sb, ino, scoutfs_iget_test, scoutfs_iget_set,
			     &ino);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}

	if (inode->i_state & I_NEW) {
		ret = refresh_inode(inode, lock);
		if (ret) {
			iget_failed(inode);
			inode = ERR_PTR(ret);
		} else {
			set_inode_ops(inode);
			unlock_new_inode(inode);
		}
	}

out:
	scoutfs_unlock(sb, lock);
	return inode;
}

static void store_inode(struct scoutfs_inode *cinode, struct inode *inode)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);

	cinode->size = cpu_to_le64(i_size_read(inode));
	cinode->nlink = cpu_to_le32(inode->i_nlink);
	cinode->uid = cpu_to_le32(i_uid_read(inode));
	cinode->gid = cpu_to_le32(i_gid_read(inode));
	cinode->mode = cpu_to_le32(inode->i_mode);
	cinode->rdev = cpu_to_le32(inode->i_rdev);
	cinode->atime.sec = cpu_to_le64(inode->i_atime.tv_sec);
	cinode->atime.nsec = cpu_to_le32(inode->i_atime.tv_nsec);
	cinode->ctime.sec = cpu_to_le64(inode->i_ctime.tv_sec);
	cinode->ctime.nsec = cpu_to_le32(inode->i_ctime.tv_nsec);
	cinode->mtime.sec = cpu_to_le64(inode->i_mtime.tv_sec);
	cinode->mtime.nsec = cpu_to_le32(inode->i_mtime.tv_nsec);

	cinode->meta_seq = cpu_to_le64(scoutfs_inode_meta_seq(inode));
	cinode->data_seq = cpu_to_le64(scoutfs_inode_data_seq(inode));
	cinode->data_version = cpu_to_le64(scoutfs_inode_data_version(inode));
	cinode->next_readdir_pos = cpu_to_le64(ci->next_readdir_pos);
}

/*
 * Create a pinned dirty inode item so that we can later update the
 * inode item without risking failure.  We often wouldn't want to have
 * to unwind inode modifcations (perhaps by shared vfs code!) if our
 * item update failed.  This is our chance to return errors for enospc
 * for lack of space for new logged dirty inode items.
 *
 * This dirty inode item will be found by lookups in the interim so we
 * have to update it now with the current inode contents.
 *
 * Callers don't delete these dirty items on errors.  They're still
 * valid and will be merged with the current item eventually.  They can
 * be found in the dirty block to avoid future dirtying (say repeated
 * creations in a directory).
 *
 * The caller has to prevent sync between dirtying and updating the
 * inodes.
 *
 * XXX this will have to do something about variable length inodes
 */
int scoutfs_dirty_inode_item(struct inode *inode, struct scoutfs_key_buf *end)
{
	struct super_block *sb = inode->i_sb;
	struct scoutfs_inode_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_inode sinode;
	int ret;

	store_inode(&sinode, inode);

	scoutfs_inode_init_key(&key, &ikey, scoutfs_ino(inode));

	ret = scoutfs_item_dirty(sb, &key, end);
	if (!ret)
		trace_scoutfs_dirty_inode(inode);
	return ret;
}

/*
 * Make sure inode index items are kept in sync with the fields that are
 * set in the inode items.  This must be called any time the contents of
 * the inode items are updated.
 *
 * This is effectively a RMW on the inode fields so the caller needs to
 * lock the inode so that it's the only one working with the index items
 * for a given set of fields in the inode.
 *
 * But it doesn't need to lock the index item keys.  By locking the
 * inode we've ensured that we can safely log deletion and insertion
 * items in our log.  The indexes are eventually consistent so we don't
 * need to wrap them locks.
 *
 * XXX this needs more supporting work from the rest of the
 * infrastructure:
 *
 * - Deleting and creating the items needs to forcefully set those dirty
 * items in the cache without first trying to read them from segments.
 * - the reading ioctl needs to forcefully invalidate the index items
 * as it walks.
 * - maybe the reading ioctl needs to verify fields with inodes?
 * - final inode deletion needs to invalidate the index items for
 * each inode as it deletes items based on the locked inode fields.
 * - make sure deletion items safely vanish w/o finding existing item
 * - ... error handling :(
 */
static int update_index(struct super_block *sb, struct scoutfs_inode_info *si,
			u64 ino, u8 type, u64 now_major, u32 now_minor,
			u64 then_major, u32 then_minor)
{
	struct scoutfs_inode_index_key ins_ikey;
	struct scoutfs_inode_index_key del_ikey;
	struct scoutfs_key_buf ins;
	struct scoutfs_key_buf del;
	int ret;
	int err;

	trace_printk("ino %llu have %u now %llu.%u then %llu.%u \n",
		     ino, si->have_item, now_major, now_minor, then_major,
		     then_minor);

	if (si->have_item && now_major == then_major && now_minor == then_minor)
		return 0;

	ins_ikey.zone = SCOUTFS_INODE_INDEX_ZONE;
	ins_ikey.type = type;
	ins_ikey.major = cpu_to_be64(now_major);
	ins_ikey.minor = cpu_to_be32(now_minor);
	ins_ikey.ino = cpu_to_be64(ino);
	scoutfs_key_init(&ins, &ins_ikey, sizeof(ins_ikey));

	ret = scoutfs_item_create(sb, &ins, NULL);
	if (ret || !si->have_item)
		return ret;

	del_ikey.zone = SCOUTFS_INODE_INDEX_ZONE;
	del_ikey.type = type;
	del_ikey.major = cpu_to_be64(then_major);
	del_ikey.minor = cpu_to_be32(then_minor);
	del_ikey.ino = cpu_to_be64(ino);
	scoutfs_key_init(&del, &del_ikey, sizeof(del_ikey));

	ret = scoutfs_item_delete(sb, &del, NULL);
	if (ret) {
		err = scoutfs_item_delete(sb, &ins, NULL);
		BUG_ON(err);
	}

	return ret;
}

/*
 * Every time we modify the inode in memory we copy it to its inode
 * item.  This lets us write out items without having to track down
 * dirty vfs inodes.
 *
 * The caller makes sure that the item is dirty and pinned so they don't
 * have to deal with errors and unwinding after they've modified the
 * vfs inode and get here.
 */
void scoutfs_update_inode_item(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	const u64 ino = scoutfs_ino(inode);
	struct scoutfs_inode_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_inode sinode;
	SCOUTFS_DECLARE_KVEC(val);
	int ret;
	int err;

	mutex_lock(&si->item_mutex);

	/* set the meta version once per trans for any inode updates */
	scoutfs_inode_set_meta_seq(inode);

	/* only race with other inode field stores once */
	store_inode(&sinode, inode);

	ret = update_index(sb, si, ino, SCOUTFS_INODE_INDEX_SIZE_TYPE,
			   le64_to_cpu(sinode.size), 0, si->item_size, 0) ?:
	      update_index(sb, si, ino, SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			   le64_to_cpu(sinode.meta_seq), 0,
			   si->item_meta_seq, 0);
	if (ret == 0 && S_ISREG(inode->i_mode))
		ret = update_index(sb, si, ino,
				   SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
				   le64_to_cpu(sinode.data_seq), 0,
				   si->item_data_seq, 0);
	BUG_ON(ret);

	scoutfs_inode_init_key(&key, &ikey, ino);
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	err = scoutfs_item_update(sb, &key, val, NULL);
	if (err) {
		scoutfs_err(sb, "inode %llu update err %d", ino, err);
		BUG_ON(err);
	}

	set_item_info(si, &sinode);
	trace_scoutfs_update_inode(inode);

	mutex_unlock(&si->item_mutex);
}

/* this is called on final inode cleanup so enoent is fine */
static int remove_index(struct super_block *sb, u64 ino, u8 type, u64 major,
			u32 minor)
{
	struct scoutfs_inode_index_key ikey;
	struct scoutfs_key_buf key;
	int ret;

	ikey.zone = SCOUTFS_INODE_INDEX_ZONE;
	ikey.type = type;
	ikey.major = cpu_to_be64(major);
	ikey.minor = cpu_to_be32(minor);
	ikey.ino = cpu_to_be64(ino);
	scoutfs_key_init(&key, &ikey, sizeof(ikey));

	/* XXX would be deletion under CW that doesn't need to read */
	ret = scoutfs_item_delete(sb, &key, NULL);
	if (ret == -ENOENT)
		ret = 0;
	return ret;
}

/*
 * Remove all the inode's index items.  The caller has ensured that
 * there are no more active users of the inode.  This can be racing with
 * users of the inode index items.  Once we can use them we'll get CW
 * locks around the index items to invalidate remote caches.  Racing
 * users of the index items already have to deal with the possibility
 * that the inodes returned by the index queries can go out of sync by
 * the time they get to it, including being deleted.
 */
static int remove_index_items(struct super_block *sb, u64 ino,
			      struct scoutfs_inode *sinode)
{
	umode_t mode = le32_to_cpu(sinode->mode);
	int ret;

	ret = remove_index(sb, ino, SCOUTFS_INODE_INDEX_SIZE_TYPE,
			   le64_to_cpu(sinode->size), 0) ?:
	      remove_index(sb, ino, SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			   le64_to_cpu(sinode->meta_seq), 0);
	if (ret == 0 && S_ISREG(mode))
		ret = remove_index(sb, ino, SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
				   le64_to_cpu(sinode->data_seq), 0);
	return ret;
}

/*
 * A quick atomic sample of the last inode number that's been allocated.
 */
u64 scoutfs_last_ino(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	u64 last;

	spin_lock(&sbi->next_ino_lock);
	last = le64_to_cpu(super->next_ino);
	spin_unlock(&sbi->next_ino_lock);

	return last;
}

/*
 * Network replies refill the pool, providing ino = ~0ULL nr = 0 when
 * there's no more inodes (which should never happen in practice.)
 */
void scoutfs_inode_fill_pool(struct super_block *sb, u64 ino, u64 nr)
{
	struct free_ino_pool *pool = &SCOUTFS_SB(sb)->inode_sb_info->pool;

	trace_printk("filling ino %llu nr %llu\n", ino, nr);

	spin_lock(&pool->lock);

	pool->ino = ino;
	pool->nr = nr;
	pool->in_flight = false;

	spin_unlock(&pool->lock);

	wake_up(&pool->waitq);
}

static bool pool_in_flight(struct free_ino_pool *pool)
{
	bool in_flight;

	spin_lock(&pool->lock);
	in_flight = pool->in_flight;
	spin_unlock(&pool->lock);

	return in_flight;
}

/*
 * We have a pool of free inodes given to us by the server.  If it
 * empties we only ever have one request for new inodes in flight.  The
 * net layer calls us when it gets a reply.  If there's no more inodes
 * we'll get ino == ~0 and nr == 0.
 */
static int alloc_ino(struct super_block *sb, u64 *ino)
{
	struct free_ino_pool *pool = &SCOUTFS_SB(sb)->inode_sb_info->pool;
	bool request;
	int ret;

	*ino = 0;

	spin_lock(&pool->lock);

	while (pool->nr == 0 && pool->ino != ~0ULL) {
		if (pool->in_flight) {
			request = false;
		} else {
			pool->in_flight = true;
			request = true;
		}

		spin_unlock(&pool->lock);

		if (request) {
			ret = scoutfs_client_alloc_inodes(sb);
			if (ret) {
				spin_lock(&pool->lock);
				pool->in_flight = false;
				spin_unlock(&pool->lock);
				wake_up(&pool->waitq);
				goto out;
			}
		}

		ret = wait_event_interruptible(pool->waitq,
					       !pool_in_flight(pool));
		if (ret)
			goto out;

		spin_lock(&pool->lock);
	}

	if (pool->nr == 0) {
		*ino = 0;
		ret = -ENOSPC;
	} else {
		*ino = pool->ino++;
		pool->nr--;
		ret = 0;

	}

	spin_unlock(&pool->lock);

out:
	trace_printk("ret %d ino %llu pool ino %llu nr %llu req %u (racey)\n",
		     ret, *ino, pool->ino, pool->nr, pool->in_flight);
	return ret;
}

/*
 * Allocate and initialize a new inode.  The caller is responsible for
 * creating links to it and updating it.  @dir can be null.
 */
struct inode *scoutfs_new_inode(struct super_block *sb, struct inode *dir,
				umode_t mode, dev_t rdev)
{
	struct scoutfs_inode_info *ci;
	struct scoutfs_inode_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_inode sinode;
	SCOUTFS_DECLARE_KVEC(val);
	struct inode *inode;
	u64 ino;
	int ret;

	ret = alloc_ino(sb, &ino);
	if (ret)
		return ERR_PTR(ret);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ci = SCOUTFS_I(inode);
	ci->ino = ino;
	ci->meta_seq = 0;
	ci->data_seq = 0;
	ci->data_version = 0;
	ci->next_readdir_pos = SCOUTFS_DIRENT_FIRST_POS;
	ci->have_item = false;

	inode->i_ino = ino; /* XXX overflow */
	inode_init_owner(inode, dir, mode);
	inode_set_bytes(inode, 0);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_rdev = rdev;
	set_inode_ops(inode);

	store_inode(&sinode, inode);
	scoutfs_inode_init_key(&key, &ikey, scoutfs_ino(inode));
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	ret = scoutfs_item_create(sb, &key, val);
	if (ret) {
		iput(inode);
		return ERR_PTR(ret);
	}

	return inode;
}

static void init_orphan_key(struct scoutfs_key_buf *key,
			    struct scoutfs_orphan_key *okey, u64 node_id, u64 ino)
{
	okey->zone = SCOUTFS_NODE_ZONE;
	okey->node_id = cpu_to_be64(node_id);
	okey->type = SCOUTFS_ORPHAN_TYPE;
	okey->ino = cpu_to_be64(ino);

	scoutfs_key_init(key, okey, sizeof(struct scoutfs_orphan_key));
}

static int remove_orphan_item(struct super_block *sb, u64 ino)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_orphan_key okey;
	struct scoutfs_key_buf key;
	int ret;

	init_orphan_key(&key, &okey, sbi->node_id, ino);

	ret = scoutfs_item_delete(sb, &key, NULL);
	if (ret == -ENOENT)
		ret = 0;

	return ret;
}

/*
 * Remove all the items associated with a given inode.  This is only
 * called once nlink has dropped to zero so we don't have to worry about
 * dirents referencing the inode or link backrefs.  Dropping nlink to 0
 * also created an orphan item.  That orphan item will continue
 * triggering attempts to finish previous partial deletion until all
 * deletion is complete and the orphan item is removed.
 */
static int delete_inode_items(struct super_block *sb, u64 ino)
{
	struct scoutfs_inode_key ikey;
	struct scoutfs_inode sinode;
	struct scoutfs_key_buf key;
	SCOUTFS_DECLARE_KVEC(val);
	DECLARE_ITEM_COUNT(cnt);
	bool release = false;
	umode_t mode;
	int ret;

	scoutfs_inode_init_key(&key, &ikey, ino);
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	ret = scoutfs_item_lookup_exact(sb, &key, val, sizeof(sinode), NULL);
	if (ret < 0) {
		if (ret == -ENOENT)
			ret = 0;
		return ret;
	}

	/* XXX corruption, inode probably won't be freed without repair */
	if (le32_to_cpu(sinode.nlink)) {
		scoutfs_warn(sb, "Dangling orphan item for inode %llu.", ino);
		return -EIO;
	}

	mode = le32_to_cpu(sinode.mode);
	trace_delete_inode(sb, ino, mode);

	/* XXX this is obviously not done yet :) */
	scoutfs_count_dirty_inode(&cnt);
	ret = scoutfs_hold_trans(sb, &cnt);
	if (ret)
		goto out;
	release = true;

	/* first remove index items to try to avoid indexing partial deletion */
	ret = remove_index_items(sb, ino, &sinode);
	if (ret)
		goto out;

#if 0
	ret = scoutfs_xattr_drop(sb, ino);
	if (ret)
		goto out;

	if (S_ISLNK(mode))
		ret = scoutfs_symlink_drop(sb, ino, i_size);
	else if (S_ISREG(mode))
		ret = scoutfs_truncate_extent_items(sb, ino, 0, ~0ULL, false);
	if (ret)
		goto out;

#endif
	ret = scoutfs_item_delete(sb, &key, NULL);
	if (ret)
		goto out;

	ret = remove_orphan_item(sb, ino);
out:
	if (release)
		scoutfs_release_trans(sb);
	return ret;
}

/*
 * iput_final has already written out the dirty pages to the inode
 * before we get here.  We're left with a clean inode that we have to
 * tear down.  If there are no more links to the inode then we also
 * remove all its persistent structures.
 */
void scoutfs_evict_inode(struct inode *inode)
{
	trace_printk("ino %llu nlink %d bad %d\n",
		     scoutfs_ino(inode), inode->i_nlink, is_bad_inode(inode));

	if (is_bad_inode(inode))
		goto clear;

	truncate_inode_pages_final(&inode->i_data);

	if (inode->i_nlink == 0)
		delete_inode_items(inode->i_sb, scoutfs_ino(inode));
clear:
	clear_inode(inode);
}

int scoutfs_drop_inode(struct inode *inode)
{
	int ret = generic_drop_inode(inode);

	trace_printk("ret %d nlink %d unhashed %d\n",
		     ret, inode->i_nlink, inode_unhashed(inode));
	return ret;
}

/*
 * Find orphan items and process each one.
 *
 * Runtime of this will be bounded by the number of orphans, which could
 * theoretically be very large. If that becomes a problem we might want to push
 * this work off to a thread.
 *
 * This only scans orphans for this node.  This will need to be covered by
 * the rest of node zone cleanup.
 */
int scoutfs_scan_orphans(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_orphan_key okey;
	struct scoutfs_orphan_key last_okey;
	struct scoutfs_key_buf key;
	struct scoutfs_key_buf last;
	int err = 0;
	int ret;

	trace_scoutfs_scan_orphans(sb);

	init_orphan_key(&key, &okey, sbi->node_id, 0);
	init_orphan_key(&last, &last_okey, sbi->node_id, ~0ULL);

	while (1) {
		ret = scoutfs_item_next_same(sb, &key, &last, NULL, NULL);
		if (ret == -ENOENT) /* No more orphan items */
			break;
		if (ret < 0)
			goto out;

		ret = delete_inode_items(sb, be64_to_cpu(okey.ino));
		if (ret && ret != -ENOENT && !err)
			err = ret;

		scoutfs_key_inc_cur_len(&key);
	}

	ret = 0;
out:
	return err ? err : ret;
}

int scoutfs_orphan_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_orphan_key okey;
	struct scoutfs_key_buf key;
	int ret;

	trace_scoutfs_orphan_inode(sb, inode);

	init_orphan_key(&key, &okey, sbi->node_id, scoutfs_ino(inode));

	ret = scoutfs_item_create(sb, &key, NULL);

	return ret;
}

/*
 * Track an inode that could have dirty pages.  Used to kick off writeback
 * on all dirty pages during transaction commit without tying ourselves in
 * knots trying to call through the high level vfs sync methods.
 */
void scoutfs_inode_queue_writeback(struct inode *inode)
{
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	spin_lock(&inf->writeback_lock);
	if (RB_EMPTY_NODE(&si->writeback_node))
		insert_writeback_inode(inf, si);
	spin_unlock(&inf->writeback_lock);
}

/*
 * Walk our dirty inodes in ino order and either start dirty page
 * writeback or wait for writeback to complete.
 *
 * This is called by transaction commiting so other writers are
 * excluded.  We're still very careful to iterate over the tree while it
 * and the inodes could be changing.
 *
 * Because writes are excluded we know that there's no remaining dirty
 * pages once waiting returns successfully.
 *
 * XXX not sure what to do about retrying io errors.
 */
int scoutfs_inode_walk_writeback(struct super_block *sb, bool write)
{
	DECLARE_INODE_SB_INFO(sb, inf);
	struct scoutfs_inode_info *si;
	struct rb_node *node;
	struct inode *inode;
	struct inode *defer_iput = NULL;
	int ret;

	spin_lock(&inf->writeback_lock);

	node = rb_first(&inf->writeback_inodes);
	while (node) {
		si = container_of(node, struct scoutfs_inode_info,
				  writeback_node);
		node = rb_next(node);
		inode = igrab(&si->inode);
		if (!inode)
			continue;

		spin_unlock(&inf->writeback_lock);

		if (defer_iput) {
			iput(defer_iput);
			defer_iput = NULL;
		}

		if (write)
			ret = filemap_fdatawrite(inode->i_mapping);
		else
			ret = filemap_fdatawait(inode->i_mapping);
		trace_printk("ino %llu write %d ret %d\n",
			     scoutfs_ino(inode), write, ret);
		if (ret) {
			iput(inode);
			goto out;
		}

		spin_lock(&inf->writeback_lock);

		if (WARN_ON_ONCE(RB_EMPTY_NODE(&si->writeback_node)))
			node = rb_first(&inf->writeback_inodes);
		else
			node = rb_next(&si->writeback_node);

		if (!write)
			remove_writeback_inode(inf, si);

		/* avoid iput->destroy lock deadlock */
		defer_iput = inode;
	}

	spin_unlock(&inf->writeback_lock);
out:
	if (defer_iput)
		iput(defer_iput);
	return ret;
}

int scoutfs_inode_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct free_ino_pool *pool;
	struct inode_sb_info *inf;

	inf = kzalloc(sizeof(struct inode_sb_info), GFP_KERNEL);
	if (!inf)
		return -ENOMEM;

	pool = &inf->pool;
	init_waitqueue_head(&pool->waitq);
	spin_lock_init(&pool->lock);

	spin_lock_init(&inf->writeback_lock);
	inf->writeback_inodes = RB_ROOT;

	sbi->inode_sb_info = inf;

	return 0;
}

void scoutfs_inode_destroy(struct super_block *sb)
{
	struct inode_sb_info *inf = SCOUTFS_SB(sb)->inode_sb_info;

	kfree(inf);
}

void scoutfs_inode_exit(void)
{
	if (scoutfs_inode_cachep) {
		rcu_barrier();
		kmem_cache_destroy(scoutfs_inode_cachep);
		scoutfs_inode_cachep = NULL;
	}
}

int scoutfs_inode_init(void)
{
	scoutfs_inode_cachep = kmem_cache_create("scoutfs_inode_info",
					sizeof(struct scoutfs_inode_info), 0,
					SLAB_RECLAIM_ACCOUNT,
					scoutfs_inode_ctor);
	if (!scoutfs_inode_cachep)
		return -ENOMEM;

	return 0;
}
