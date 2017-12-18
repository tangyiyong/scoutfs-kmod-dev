/*
 * Copyright (C) 2016 Versity Software, Inc.  All rights reserved.
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
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/list_sort.h>

#include "super.h"
#include "format.h"
#include "kvec.h"
#include "seg.h"
#include "item.h"
#include "btree.h"
#include "cmp.h"
#include "compact.h"
#include "manifest.h"
#include "trans.h"
#include "counters.h"
#include "triggers.h"
#include "client.h"
#include "scoutfs_trace.h"

/*
 * Manifest entries are stored in the cow btrees in the persistently
 * allocated ring of blocks in the shared device.  This lets clients
 * read consistent old versions of the manifest when it's safe to do so.
 *
 * Manifest entries are sorted first by level then by their first key.
 * This enables the primary searches based on key value for looking up
 * items in segments via the manifest.
 */

struct manifest {
	struct rw_semaphore rwsem;
	u8 nr_levels;

	/* calculated on mount, const thereafter */
	u64 level_limits[SCOUTFS_MANIFEST_MAX_LEVEL + 1];

	unsigned long flags;

	struct scoutfs_key_buf *compact_keys[SCOUTFS_MANIFEST_MAX_LEVEL + 1];
};

#define MANI_FLAG_LEVEL0_FULL (1 << 0)

#define DECLARE_MANIFEST(sb, name) \
	struct manifest *name = SCOUTFS_SB(sb)->manifest

/*
 * A reader uses references to segments copied from a walk of the
 * manifest.  The references are a point in time sample of the manifest.
 * The manifest and segments can change while the reader uses their
 * references.  Locking ensures that the items they're reading will be
 * stable while the manifest and segments change, and the segment
 * allocator gives readers time to use immutable stale segments before
 * their reallocated and reused.
 */
struct manifest_ref {
	struct list_head entry;

	u64 segno;
	u64 seq;
	struct scoutfs_segment *seg;
	int found_ctr;
	int off;
	u8 level;

	struct scoutfs_key_buf *first;
	struct scoutfs_key_buf *last;
};

/*
 * Change the level count under the manifest lock.  We then maintain a
 * bit that can be tested outside the lock to determine if the caller
 * should wait for level 0 segments to drain.
 */
static void add_level_count(struct super_block *sb, int level, s64 val)
{
	DECLARE_MANIFEST(sb, mani);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	__le64 count;
	int full;

	le64_add_cpu(&super->manifest.level_counts[level], val);

	if (level == 0) {
		count = super->manifest.level_counts[level];
		full = test_bit(MANI_FLAG_LEVEL0_FULL, &mani->flags);
		if (count && !full)
			set_bit(MANI_FLAG_LEVEL0_FULL, &mani->flags);
		else if (!count && full)
			clear_bit(MANI_FLAG_LEVEL0_FULL, &mani->flags);
	}
}

/*
 * Return whether or not level 0 segments are full.  It's safe to use
 * this as a wait_event condition because it doesn't block.
 *
 * Callers rely on on the spin locks in wait queues to synchronize
 * testing this as a sleeping condition with addition to the wait queue
 * and waking of the waitqueue.
 */
bool scoutfs_manifest_level0_full(struct super_block *sb)
{
	DECLARE_MANIFEST(sb, mani);

	return test_bit(MANI_FLAG_LEVEL0_FULL, &mani->flags);
}

void scoutfs_manifest_init_entry(struct scoutfs_manifest_entry *ment,
				 u64 level, u64 segno, u64 seq,
				 struct scoutfs_key_buf *first,
				 struct scoutfs_key_buf *last)
{
	ment->level = level;
	ment->segno = segno;
	ment->seq = seq;

	if (first)
		scoutfs_key_clone(&ment->first, first);
	else
		scoutfs_key_init(&ment->first, NULL, 0);

	if (last)
		scoutfs_key_clone(&ment->last, last);
	else
		scoutfs_key_init(&ment->last, NULL, 0);
}

/*
 * level 0 segments have the extra seq up in the btree key.
 */
static struct scoutfs_manifest_btree_key *
alloc_btree_key_val_lens(unsigned first_len, unsigned last_len)
{
	return kmalloc(sizeof(struct scoutfs_manifest_btree_key) +
		       sizeof(u64) +
		       sizeof(struct scoutfs_manifest_btree_val) +
		       first_len + last_len, GFP_NOFS);
}

/*
 * Initialize the btree key and value for a manifest entry in one contiguous
 * allocation.
 */
static struct scoutfs_manifest_btree_key *
alloc_btree_key_val(struct scoutfs_manifest_entry *ment, unsigned *mkey_len,
		    struct scoutfs_manifest_btree_val **mval_ret,
		    unsigned *mval_len_ret)
{
	struct scoutfs_manifest_btree_key *mkey;
	struct scoutfs_manifest_btree_val *mval;
	struct scoutfs_key_buf b_first;
	struct scoutfs_key_buf b_last;
	unsigned bkey_len;
	unsigned mval_len;
	__be64 seq;

	mkey = alloc_btree_key_val_lens(ment->first.key_len, ment->last.key_len);
	if (!mkey)
		return NULL;

	if (ment->level == 0) {
		seq = cpu_to_be64(ment->seq);
		bkey_len = sizeof(seq);
		memcpy(mkey->bkey, &seq, bkey_len);
	} else {
		bkey_len = ment->first.key_len;
	}

	*mkey_len = offsetof(struct scoutfs_manifest_btree_key, bkey[bkey_len]);
	mval = (void *)mkey + *mkey_len;

	if (ment->level == 0) {
		scoutfs_key_init(&b_first, mval->keys, ment->first.key_len);
		scoutfs_key_init(&b_last, mval->keys + ment->first.key_len,
				 ment->last.key_len);
		mval_len = sizeof(struct scoutfs_manifest_btree_val) +
			   ment->first.key_len + ment->last.key_len;
	} else {
		scoutfs_key_init(&b_first, mkey->bkey, ment->first.key_len);
		scoutfs_key_init(&b_last, mval->keys, ment->last.key_len);
		mval_len = sizeof(struct scoutfs_manifest_btree_val) +
			   ment->last.key_len;
	}

	mkey->level = ment->level;
	mval->segno = cpu_to_le64(ment->segno);
	mval->seq = cpu_to_le64(ment->seq);
	mval->first_key_len = cpu_to_le16(ment->first.key_len);
	mval->last_key_len = cpu_to_le16(ment->last.key_len);

	scoutfs_key_copy(&b_first, &ment->first);
	scoutfs_key_copy(&b_last, &ment->last);

	if (mval_ret) {
		*mval_ret = mval;
		*mval_len_ret = mval_len;
	}
	return mkey;
}

/* initialize a native manifest entry to point to the btree key and value */
static void init_ment_iref(struct scoutfs_manifest_entry *ment,
			   struct scoutfs_btree_item_ref *iref)
{
	struct scoutfs_manifest_btree_key *mkey = iref->key;
	struct scoutfs_manifest_btree_val *mval = iref->val;

	ment->level = mkey->level;
	ment->segno = le64_to_cpu(mval->segno);
	ment->seq = le64_to_cpu(mval->seq);

	if (ment->level == 0) {
		scoutfs_key_init(&ment->first, mval->keys,
				 le16_to_cpu(mval->first_key_len));
		scoutfs_key_init(&ment->last, mval->keys +
				 le16_to_cpu(mval->first_key_len),
				 le16_to_cpu(mval->last_key_len));
	} else {
		scoutfs_key_init(&ment->first, mkey->bkey,
				 le16_to_cpu(mval->first_key_len));
		scoutfs_key_init(&ment->last, mval->keys,
				 le16_to_cpu(mval->last_key_len));
	}
}

/*
 * Fill the callers max-size btree key with the given values and return
 * its length.
 */
static unsigned init_btree_key(struct scoutfs_manifest_btree_key *mkey,
			       u8 level, u64 seq, struct scoutfs_key_buf *first)
{
	struct scoutfs_key_buf b_first;
	unsigned bkey_len;
	__be64 bseq;

	mkey->level = level;

	if (level == 0) {
		bseq = cpu_to_be64(seq);
		bkey_len = sizeof(bseq);
		memcpy(mkey->bkey, &bseq, bkey_len);
	} else if (first) {
		scoutfs_key_init(&b_first, mkey->bkey, first->key_len);
		scoutfs_key_copy(&b_first, first);
		bkey_len = first->key_len;
	} else {
		bkey_len = 0;
	}

	return offsetof(struct scoutfs_manifest_btree_key, bkey[bkey_len]);
}

/*
 * Insert a new manifest entry in the ring.  The ring allocates a new
 * node for us and we fill it.
 *
 * This must be called with the manifest lock held.
 */
int scoutfs_manifest_add(struct super_block *sb,
			 struct scoutfs_manifest_entry *ment)
{
	DECLARE_MANIFEST(sb, mani);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_manifest_btree_key *mkey;
	struct scoutfs_manifest_btree_val *mval;
	unsigned mkey_len;
	unsigned mval_len;
	int ret;

	lockdep_assert_held(&mani->rwsem);

	mkey = alloc_btree_key_val(ment, &mkey_len, &mval, &mval_len);
	if (!mkey)
		return -ENOMEM;

	trace_scoutfs_manifest_add(sb, ment->level, ment->segno, ment->seq,
				   &ment->first, &ment->last);

	ret = scoutfs_btree_insert(sb, &super->manifest.root, mkey, mkey_len,
				   mval, mval_len);
	if (ret == 0) {
		mani->nr_levels = max_t(u8, mani->nr_levels, ment->level + 1);
		add_level_count(sb, ment->level, 1);
	}

	kfree(mkey);
	return ret;
}

/*
 * This must be called with the manifest lock held.
 *
 * When this is called from the network we can take the keys directly as
 * they were sent from the clients.
 */
int scoutfs_manifest_del(struct super_block *sb,
			 struct scoutfs_manifest_entry *ment)
{
	DECLARE_MANIFEST(sb, mani);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_manifest_btree_key *mkey;
	unsigned mkey_len;
	int ret;

	trace_scoutfs_manifest_delete(sb, ment->level, ment->segno, ment->seq,
				      &ment->first, &ment->last);

	lockdep_assert_held(&mani->rwsem);

	mkey = alloc_btree_key_val(ment, &mkey_len, NULL, NULL);
	if (!mkey)
		return -ENOMEM;

	ret = scoutfs_btree_delete(sb, &super->manifest.root, mkey, mkey_len);
	if (ret == 0)
		add_level_count(sb, ment->level, -1ULL);

	kfree(mkey);
	return ret;
}

/*
 * XXX This feels pretty gross, but it's a simple way to give compaction
 * atomic updates.  It'll go away once compactions go to the trouble of
 * communicating their atomic results in a message instead of a series
 * of function calls.
 */
int scoutfs_manifest_lock(struct super_block *sb)
{
	DECLARE_MANIFEST(sb, mani);

	down_write(&mani->rwsem);

	return 0;
}

int scoutfs_manifest_unlock(struct super_block *sb)
{
	DECLARE_MANIFEST(sb, mani);

	up_write(&mani->rwsem);

	return 0;
}

static void free_ref(struct super_block *sb, struct manifest_ref *ref)
{
	if (!IS_ERR_OR_NULL(ref)) {
		WARN_ON_ONCE(!list_empty(&ref->entry));
		scoutfs_seg_put(ref->seg);
		scoutfs_key_free(sb, ref->first);
		scoutfs_key_free(sb, ref->last);
		kfree(ref);
	}
}

/*
 * Allocate a reading manifest ref so that we can work with segments
 * described by the callers manifest entry.
 */
static int alloc_manifest_ref(struct super_block *sb, struct list_head *ref_list,
			      struct scoutfs_manifest_entry *ment)
{
	struct manifest_ref *ref;

	ref = kzalloc(sizeof(struct manifest_ref), GFP_NOFS);
	if (ref) {
		ref->first = scoutfs_key_dup(sb, &ment->first);
		ref->last = scoutfs_key_dup(sb, &ment->last);
	}
	if (!ref || !ref->first || !ref->last) {
		free_ref(sb, ref);
		return -ENOMEM;
	}

	ref->level = ment->level;
	ref->segno = ment->segno;
	ref->seq = ment->seq;

	list_add_tail(&ref->entry, ref_list);

	return 0;
}

/*
 * Return the previous entry if it's in the right level and it overlaps
 * with the start key by having a last key that's >=.  If no such entry
 * exists it just returns the next entry after the key and doesn't test
 * it at all.  If this returns 0 then the caller has to put the iref.
 */
static int btree_prev_overlap_or_next(struct super_block *sb,
				      struct scoutfs_btree_root *root,
				      void *key, unsigned key_len,
				      struct scoutfs_key_buf *start, u8 level,
				      struct scoutfs_btree_item_ref *iref)
{
	struct scoutfs_manifest_entry ment;
	int ret;

	ret = scoutfs_btree_prev(sb, root, key, key_len, iref);
	if (ret < 0 && ret != -ENOENT)
		return ret;

	if (ret == 0) {
		init_ment_iref(&ment, iref);
		if (ment.level != level ||
		    scoutfs_key_compare(&ment.last, start) < 0)
			ret = -ENOENT;
	}
	if (ret == -ENOENT) {
		scoutfs_btree_put_iref(iref);
		ret = scoutfs_btree_next(sb, root, key, key_len, iref);
	}

	return ret;
}

/*
 * Get references to all the level 0 segments whose item ranges
 * intersect with the callers range.  We walk the manifest backwards so
 * that we end up adding refs to the caller's list reverse sorted by
 * sequence, which is what they want to be able to use the segment with
 * the newest item.
 *
 * This can return -ESTALE if it reads through stale btree blocks.
 */
static int get_zero_refs(struct super_block *sb,
			 struct scoutfs_btree_root *root,
			 struct scoutfs_key_buf *start,
			 struct scoutfs_key_buf *end,
			 struct list_head *ref_list)
{
	struct scoutfs_manifest_btree_key *mkey;
	struct scoutfs_manifest_entry ment;
	SCOUTFS_BTREE_ITEM_REF(iref);
	SCOUTFS_BTREE_ITEM_REF(prev);
	unsigned mkey_len;
	int ret;

	scoutfs_manifest_init_entry(&ment, 0, 0, 0, start, NULL);
	mkey = alloc_btree_key_val(&ment, &mkey_len, NULL, NULL);
	if (!mkey)
		return -ENOMEM;

	/* get level 0 segments that overlap with the missing range */
	mkey_len = init_btree_key(mkey, 0, ~0ULL, NULL);
	ret = scoutfs_btree_prev(sb, root, mkey, mkey_len, &iref);
	while (ret == 0) {
		init_ment_iref(&ment, &iref);

		if (scoutfs_key_compare_ranges(start, end, &ment.first,
					       &ment.last) == 0) {
			ret = alloc_manifest_ref(sb, ref_list, &ment);
			if (ret)
				break;
		}

		swap(prev, iref);
		ret = scoutfs_btree_before(sb, root, prev.key, prev.key_len,
					   &iref);
		scoutfs_btree_put_iref(&prev);
	}
	if (ret == -ENOENT)
		ret = 0;

	scoutfs_btree_put_iref(&iref);
	scoutfs_btree_put_iref(&prev);
	kfree(mkey);
	return ret;
}

/*
 * Get references to all segments in non-zero levels that contain the
 * caller's search key.   The item ranges of segments at each non-zero
 * level don't overlap so we can iterate through the key space in each
 * segment starting with the search key.  In each level we need the
 * first existing segment that intersects with the range, even if it
 * doesn't contain the key.  The key might fall between segments at that
 * level.  If a segment is entirely outside of the caller's range then
 * we can't trust its contents.
 *
 * This can return -ESTALE if it reads through stale btree blocks.
 */
static int get_nonzero_refs(struct super_block *sb,
			    struct scoutfs_btree_root *root,
			    struct scoutfs_key_buf *key,
			    struct scoutfs_key_buf *end,
			    struct list_head *ref_list)
{
	struct scoutfs_manifest_btree_key *mkey;
	struct scoutfs_manifest_entry ment;
	SCOUTFS_BTREE_ITEM_REF(iref);
	SCOUTFS_BTREE_ITEM_REF(prev);
	unsigned mkey_len;
	int ret;
	int i;

	scoutfs_manifest_init_entry(&ment, 0, 0, 0, key, NULL);
	mkey = alloc_btree_key_val(&ment, &mkey_len, NULL, NULL);
	if (!mkey)
		return -ENOMEM;

	mkey_len = init_btree_key(mkey, 1, 0, key);
	for (i = 1; ; i++) {
		mkey->level = i;

		scoutfs_btree_put_iref(&iref);
		ret = btree_prev_overlap_or_next(sb, root, mkey, mkey_len, key,
						 i, &iref);
		if (ret < 0) {
			if (ret == -ENOENT)
				ret = 0;
			goto out;
		}

		init_ment_iref(&ment, &iref);

		if (ment.level != i ||
		    scoutfs_key_compare(&ment.first, end) > 0)
			continue;

		ret = alloc_manifest_ref(sb, ref_list, &ment);
		if (ret)
			goto out;
	}
	ret = 0;

out:
	scoutfs_btree_put_iref(&iref);
	scoutfs_btree_put_iref(&prev);
	kfree(mkey);
	return ret;
}

/*
 * See if the caller is a remote btree reader who has read a stale btree
 * block and should keep trying.  If they see repeated errors on the
 * same root then we assume that it's persistent corruption.
 */
static int handle_stale_btree(struct super_block *sb,
			      struct scoutfs_btree_root *root,
			      __le64 last_root_seq, int ret)
{
	bool force_hard = scoutfs_trigger(sb, HARD_STALE_ERROR);

	if (ret == -ESTALE || force_hard) {
		if ((last_root_seq != root->ref.seq) && !force_hard)
			return -EAGAIN;

		scoutfs_inc_counter(sb, manifest_hard_stale_error);
		return -EIO;
	}

	return 0;
}

static int cmp_ment_ref_segno(void *priv, struct list_head *A,
			      struct list_head *B)
{
	struct manifest_ref *a = list_entry(A, struct manifest_ref, entry);
	struct manifest_ref *b = list_entry(B, struct manifest_ref, entry);

	return scoutfs_cmp_u64s(a->segno, b->segno);
}

/*
 * Sort by from most to least recent item contents.. from lowest to higest
 * level and from highest to loweset seq in level 0.
 */
static int cmp_ment_ref_level_seq(void *priv, struct list_head *A,
				  struct list_head *B)
{
	struct manifest_ref *a = list_entry(A, struct manifest_ref, entry);
	struct manifest_ref *b = list_entry(B, struct manifest_ref, entry);

	if (a->level == 0 && b->level == 0)
		return -scoutfs_cmp_u64s(a->seq, b->seq);

	return a->level < b->level ? -1 : a->level > b->level ? 1 : 0;
}

/*
 * The caller found a hole in the item cache that they'd like populated.
 * We can only trust items in the segments within their range (they hold
 * a lock) and they're going to keep calling ("He'll keep calling me,
 * he'll keep calling me") until we insert a range into the cache that
 * contains the search key.
 *
 * We search the manifest for all the non-zero segments that contain the
 * key.  We adjust the search range if the segments don't cover the
 * whole locked range.  We have to be careful not to shrink the range
 * past the key, it could be outside the segments and we still want to
 * negatively cache it.  Once we have the search range we get the level
 * zero segments that overlap.
 *
 * Once we have the segments we iterate over them and allocate the items
 * to insert into the cache.  We find the next item in each segment,
 * ignore deletion items, prefer more recent segments, and advance past
 * the items that we used.
 *
 * Returns 0 if we successfully inserted items.
 *
 * Returns -errno if we failed to make any change in the cache.
 *
 * This is asking the seg code to read each entire segment.  The seg
 * code could give it it helpers to submit and wait on blocks within the
 * segment so that we don't have wild bandwidth amplification for cold
 * random reads.
 *
 * The segments are immutable at this point so we can use their contents
 * as long as we hold refs.
 */
int scoutfs_manifest_read_items(struct super_block *sb,
				struct scoutfs_key_buf *key,
				struct scoutfs_key_buf *start,
				struct scoutfs_key_buf *end)
{
	struct scoutfs_key_buf item_key;
	struct scoutfs_key_buf found_key;
	struct scoutfs_key_buf batch_end;
	struct scoutfs_key_buf seg_start;
	struct scoutfs_key_buf seg_end;
	struct scoutfs_btree_root root;
	struct scoutfs_segment *seg;
	struct manifest_ref *ref;
	struct manifest_ref *tmp;
	__le64 last_root_seq;
	struct kvec found_val;
	struct kvec item_val;
	LIST_HEAD(ref_list);
	LIST_HEAD(batch);
	u8 found_flags = 0;
	u8 item_flags;
	int found_ctr;
	bool found;
	bool added;
	int ret = 0;
	int err;
	int cmp;

	/*
	 * Ask the manifest server which manifest root to read from.  Lock
	 * holding callers will be responsible for this in the future.  They'll
	 * either get a manifest ref in the lvb of their lock or they'll
	 * ask the server the first time the system sees the lock.
	 */
	last_root_seq = 0;
retry_stale:

	scoutfs_key_clone(&seg_start, start);
	scoutfs_key_clone(&seg_end, end);

	ret = scoutfs_client_get_manifest_root(sb, &root);
	if (ret)
		goto out;

	/* get non-zero segments that intersect with the missed key */
	ret = get_nonzero_refs(sb, &root, key, &seg_end, &ref_list);
	if (ret)
		goto out;

	/* clamp start and end to the segment boundaries, including key */
	list_for_each_entry(ref, &ref_list, entry) {

		if (scoutfs_key_compare(ref->first, &seg_start) > 0 &&
		    scoutfs_key_compare(ref->first, key) <= 0)
			scoutfs_key_clone(&seg_start, ref->first);

		if (scoutfs_key_compare(ref->last, &seg_end) < 0 &&
		    scoutfs_key_compare(ref->last, key) >= 0)
			scoutfs_key_clone(&seg_end, ref->last);
	}

	trace_scoutfs_read_item_keys(sb, key, start, end, &seg_start, &seg_end);

	/* then get level 0s that intersect with our search range */
	ret = get_zero_refs(sb, &root, &seg_start, &seg_end, &ref_list);
	if (ret)
		goto out;

	/* sort by segment to issue advancing reads */
	list_sort(NULL, &ref_list, cmp_ment_ref_segno);

	/* submit reads for all the segments */
	list_for_each_entry(ref, &ref_list, entry) {

		trace_scoutfs_read_item_segment(sb, ref->level,  ref->segno,
						ref->seq, ref->first, ref->last);

		seg = scoutfs_seg_submit_read(sb, ref->segno);
		if (IS_ERR(seg)) {
			ret = PTR_ERR(seg);
			break;
		}

		ref->seg = seg;
	}

	/* always wait for submitted segments */
	list_for_each_entry(ref, &ref_list, entry) {
		if (!ref->seg)
			break;

		err = scoutfs_seg_wait(sb, ref->seg, ref->segno, ref->seq);
		if (err && !ret)
			ret = err;
	}
	if (ret)
		goto out;

	/* now sort refs by item age */
	list_sort(NULL, &ref_list, cmp_ment_ref_level_seq);

	/* walk items from the start of our range */
	list_for_each_entry(ref, &ref_list, entry)
		ref->off = scoutfs_seg_find_off(ref->seg, &seg_start);

	found_ctr = 0;

	added = false;
	for (;;) {
		found = false;
		found_ctr++;

		/* find the next least key from the off in each segment */
		list_for_each_entry_safe(ref, tmp, &ref_list, entry) {
			if (ref->off < 0)
				continue;

			/*
			 * Check the next item in the segment.  We're
			 * done with the segment if there are no more
			 * items or if the next item is past the keys
			 * that our segments can see.
			 */
			ret = scoutfs_seg_item_ptrs(ref->seg, ref->off,
						    &item_key, &item_val,
						    &item_flags);
			if (ret < 0 ||
			    scoutfs_key_compare(&item_key, &seg_end) > 0) {
				ref->off = -1;
				continue;
			}

			/* see if it's the new least item */
			if (found) {
				cmp = scoutfs_key_compare(&item_key,
							  &found_key);
				if (cmp >= 0) {
					if (cmp == 0)
						ref->found_ctr = found_ctr;
					continue;
				}
			}

			/* remember new least key */
			scoutfs_key_clone(&found_key, &item_key);
			found_val = item_val;
			found_flags = item_flags;
			ref->found_ctr = ++found_ctr;
			found = true;
		}

		/* ran out of keys in segs, range extends to seg end */
		if (!found) {
			scoutfs_key_clone(&batch_end, &seg_end);
			ret = 0;
			break;
		}

		/*
		 * Add the next found item to the batch if it's not a
		 * deletion item.  We still need to use their key to
		 * remember the end of the batch for negative caching.
		 *
		 * If we fail to add an item we're done.  If we already
		 * have items it's not a failure and the end of the
		 * cached range is the last successfully added item.
		 */
		if (!(found_flags & SCOUTFS_ITEM_FLAG_DELETION)) {
			ret = scoutfs_item_add_batch(sb, &batch, &found_key,
						     &found_val);
			if (ret) {
				if (added)
					ret = 0;
				break;
			}
			added = true;
		}

		/* the last successful key determines range end until run out */
		scoutfs_key_clone(&batch_end, &found_key);

		/* if we just saw the end key then we're done */
		if (scoutfs_key_compare(&found_key, &seg_end) == 0) {
			ret = 0;
			break;
		}

		/* advance all the positions that had the found key */
		list_for_each_entry(ref, &ref_list, entry) {
			if (ref->found_ctr == found_ctr)
				ref->off = scoutfs_seg_next_off(ref->seg,
								ref->off);
		}

		ret = 0;
	}

	if (ret < 0) {
		scoutfs_item_free_batch(sb, &batch);
	} else {
		if (scoutfs_key_compare(key, &batch_end) > 0)
			scoutfs_inc_counter(sb, manifest_read_excluded_key);
		ret = scoutfs_item_insert_batch(sb, &batch, &seg_start,
						&batch_end);
	}
out:
	list_for_each_entry_safe(ref, tmp, &ref_list, entry) {
		list_del_init(&ref->entry);
		free_ref(sb, ref);
	}

	ret = handle_stale_btree(sb, &root, last_root_seq, ret);
	if (ret == -EAGAIN) {
		last_root_seq = root.ref.seq;
		goto retry_stale;
	}

	return ret;
}

/*
 * Give the caller a hint to the next key that they'll find after their
 * search key.
 *
 * We read the segments that intersect the key and return either the
 * next item we see or the nearest segment limit.
 *
 * This is a hint because we can return deleted items or the next
 * nearest segment limit can be well before the next items in the next
 * segments.  The caller needs to very carefully iterate using the next
 * key we return.
 *
 * Returns 0 if it set next_key and -ENOENT if the key was after all the
 * segments in the manifest.
 */
int scoutfs_manifest_next_key(struct super_block *sb,
			      struct scoutfs_key_buf *key,
			      struct scoutfs_key_buf *next_key)
{
	struct scoutfs_key_buf item_key;
	struct scoutfs_key_buf end;
	struct scoutfs_btree_root root;
	struct scoutfs_inode_key end_key;
	struct scoutfs_segment *seg;
	struct manifest_ref *ref;
	struct manifest_ref *tmp;
	__le64 last_root_seq;
	LIST_HEAD(ref_list);
	bool found;
	int ret;
	int err;

	last_root_seq = 0;
retry_stale:
	ret = scoutfs_client_get_manifest_root(sb, &root);
	if (ret)
		goto out;

	scoutfs_key_init(&end, &end_key, sizeof(end_key));
	scoutfs_key_set_max(&end);

	ret = get_zero_refs(sb, &root, key, &end, &ref_list) ?:
	      get_nonzero_refs(sb, &root, key, &end, &ref_list);
	if (ret)
		goto out;

	if (list_empty(&ref_list)) {
		ret = -ENOENT;
		goto out;
	}

	list_sort(NULL, &ref_list, cmp_ment_ref_segno);

	list_for_each_entry(ref, &ref_list, entry) {
		seg = scoutfs_seg_submit_read(sb, ref->segno);
		if (IS_ERR(seg)) {
			ret = PTR_ERR(seg);
			break;
		}

		ref->seg = seg;
	}

	list_for_each_entry(ref, &ref_list, entry) {
		if (!ref->seg)
			break;

		err = scoutfs_seg_wait(sb, ref->seg, ref->segno, ref->seq);
		if (err && !ret)
			ret = err;
	}
	if (ret)
		goto out;

	list_sort(NULL, &ref_list, cmp_ment_ref_level_seq);

	/* default to returning the nearest segment limit and find offsets */
	found = false;
	list_for_each_entry(ref, &ref_list, entry) {
		if (ref->level > 0 &&
		    (!found || scoutfs_key_compare(ref->last, next_key) < 0)) {
			scoutfs_key_copy(next_key, ref->last);
			found = true;
		}

		ref->off = scoutfs_seg_find_off(ref->seg, key);
	}

	/* return the nearest item in the segments */
	list_for_each_entry_safe(ref, tmp, &ref_list, entry) {
		if (ref->off < 0)
			continue;

		ret = scoutfs_seg_item_ptrs(ref->seg, ref->off, &item_key,
					    NULL, NULL);
		if (ret < 0)
			continue;

		if (!found || scoutfs_key_compare(&item_key, next_key) < 0) {
			scoutfs_key_copy(next_key, &item_key);
			found = true;
		}
	}

	ret = 0;
out:
	list_for_each_entry_safe(ref, tmp, &ref_list, entry) {
		list_del_init(&ref->entry);
		free_ref(sb, ref);
	}

	ret = handle_stale_btree(sb, &root, last_root_seq, ret);
	if (ret == -EAGAIN) {
		last_root_seq = root.ref.seq;
		goto retry_stale;
	}

	return ret;
}

/*
 * Give the caller the segments that will be involved in the next
 * compaction.
 *
 * For now we have a simple candidate search.  We only initiate
 * compaction when a level has exceeded its exponentially increasing
 * limit on the number of segments.  Once we have a level we use keys at
 * each level to chose the next segment.  This results in a pattern
 * where clock hands sweep through each level.  The hands wrap much
 * faster on the higher levels.
 *
 * We add all the segments to the compaction caller's data and let it do
 * its thing.  It'll allocate and free segments and update the manifest.
 *
 * Returns the number of input segments or -errno.
 *
 * XXX this will get a lot more clever:
 *  - ensuring concurrent compactions don't overlap
 *  - prioritize segments with deletion or incremental records
 *  - prioritize partial segments
 *  - maybe compact segments by age in a given level
 */
int scoutfs_manifest_next_compact(struct super_block *sb, void *data)
{
	DECLARE_MANIFEST(sb, mani);
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct scoutfs_manifest_entry ment;
	struct scoutfs_manifest_entry over;
	struct scoutfs_manifest_btree_key *mkey = NULL;
	SCOUTFS_BTREE_ITEM_REF(iref);
	SCOUTFS_BTREE_ITEM_REF(over_iref);
	SCOUTFS_BTREE_ITEM_REF(prev);
	unsigned mkey_len;
	bool sticky;
	int level;
	int ret;
	int nr = 0;
	int i;

	down_write(&mani->rwsem);

	for (level = mani->nr_levels - 1; level >= 0; level--) {
		if (le64_to_cpu(super->manifest.level_counts[level]) >
	            mani->level_limits[level])
			break;
	}

	trace_scoutfs_manifest_next_compact(sb, level);

	if (level < 0) {
		ret = 0;
		goto out;
	}

	/* alloc a full size mkey, fill it with whatever search key */

	mkey = alloc_btree_key_val_lens(SCOUTFS_MAX_KEY_SIZE, 0);
	if (!mkey) {
		ret = -ENOMEM;
		goto out;
	}

	/* find the oldest level 0 or the next higher order level by key */
	if (level == 0) {
		/* find the oldest level 0 */
		mkey_len = init_btree_key(mkey, 0, 0, NULL);
		ret = scoutfs_btree_next(sb, &super->manifest.root,
					 mkey, mkey_len, &iref);
	} else {
		/* find the next segment after the compaction at this level */
		mkey_len = init_btree_key(mkey, level, 0,
					  mani->compact_keys[level]);

		ret = scoutfs_btree_next(sb, &super->manifest.root,
					 mkey, mkey_len, &iref);
		if (ret == 0) {
			init_ment_iref(&ment, &iref);
			if (ment.level != level)
				ret = -ENOENT;
		}
		if (ret == -ENOENT) {
			/* .. possibly wrapping to the first key in level */
			mkey_len = init_btree_key(mkey, level, 0, NULL);
			scoutfs_btree_put_iref(&iref);
			ret = scoutfs_btree_next(sb, &super->manifest.root,
						 mkey, mkey_len, &iref);
		}
	}
	if (ret == 0) {
		init_ment_iref(&ment, &iref);
		if (ment.level != level)
			goto out;
	}
	if (ret < 0) {
		if (ret == -ENOENT)
			ret = 0;
		goto out;
	}

	/* add the upper input segment */
	ret = scoutfs_compact_add(sb, data, &ment);
	if (ret)
		goto out;
	nr++;

	/* and add a fanout's worth of lower overlapping segments */
	mkey_len = init_btree_key(mkey, level + 1, 0, &ment.first);
	ret = btree_prev_overlap_or_next(sb, &super->manifest.root,
					 mkey, mkey_len,
					 &ment.first, level + 1, &over_iref);
	sticky = false;
	for (i = 0; ret == 0 && i < SCOUTFS_MANIFEST_FANOUT + 1; i++) {
		init_ment_iref(&over, &over_iref);
		if (over.level != level + 1)
			break;

		if (scoutfs_key_compare_ranges(&ment.first, &ment.last,
					       &over.first, &over.last) != 0)
			break;

		/* upper level has to stay around when more than fanout */
		if (i == SCOUTFS_MANIFEST_FANOUT) {
			sticky = true;
			break;
		}

		ret = scoutfs_compact_add(sb, data, &over);
		if (ret)
			goto out;
		nr++;

		swap(prev, over_iref);
		ret = scoutfs_btree_after(sb, &super->manifest.root,
					  prev.key, prev.key_len, &over_iref);
		scoutfs_btree_put_iref(&prev);
	}
	if (ret < 0 && ret != -ENOENT)
		goto out;

	scoutfs_compact_describe(sb, data, level, mani->nr_levels - 1, sticky);

	/* record the next key to start from */
	scoutfs_key_copy(mani->compact_keys[level], &ment.last);
	scoutfs_key_inc(mani->compact_keys[level]);

	ret = 0;
out:
	up_write(&mani->rwsem);

	kfree(mkey);
	scoutfs_btree_put_iref(&iref);
	scoutfs_btree_put_iref(&over_iref);
	scoutfs_btree_put_iref(&prev);

	return ret ?: nr;
}

int scoutfs_manifest_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	struct manifest *mani;
	int i;

	mani = kzalloc(sizeof(struct manifest), GFP_KERNEL);
	if (!mani)
		return -ENOMEM;

	init_rwsem(&mani->rwsem);

	for (i = 0; i < ARRAY_SIZE(mani->compact_keys); i++) {
		mani->compact_keys[i] = scoutfs_key_alloc(sb,
							  SCOUTFS_MAX_KEY_SIZE);
		if (!mani->compact_keys[i]) {
			while (--i >= 0)
				scoutfs_key_free(sb, mani->compact_keys[i]);
			kfree(mani);
			return -ENOMEM;
		}

		scoutfs_key_set_min(mani->compact_keys[i]);
	}

	for (i = ARRAY_SIZE(super->manifest.level_counts) - 1; i >= 0; i--) {
		if (super->manifest.level_counts[i]) {
			mani->nr_levels = i + 1;
			break;
		}
	}

	/* always trigger a compaction if there's a single l0 segment? */
	mani->level_limits[0] = 0;
	mani->level_limits[1] = SCOUTFS_MANIFEST_FANOUT;
	for (i = 2; i < ARRAY_SIZE(mani->level_limits); i++) {
		mani->level_limits[i] = mani->level_limits[i - 1] *
					SCOUTFS_MANIFEST_FANOUT;
	}

	sbi->manifest = mani;

	return 0;
}

void scoutfs_manifest_destroy(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct manifest *mani = sbi->manifest;
	int i;

	if (mani) {
		for (i = 0; i < ARRAY_SIZE(mani->compact_keys); i++)
			scoutfs_key_free(sb, mani->compact_keys[i]);
		kfree(mani);
		sbi->manifest = NULL;
	}
}
