/*
 * Copyright (C) 2015 IT University of Copenhagen
 * Initial release: Matias Bjorling <m@bjorling.me>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Implementation of a Round-robin page-based Hybrid FTL for Open-channel SSDs.
 */

#ifndef RRPC_DEBUG_H_
#define RRPC_DEBUG_H_

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>

#include <linux/lightnvm.h>

/* Run only GC if less than 1/X blocks are free */
#define GC_LIMIT_INVERSE 10
#define GC_TIME_SECS 100

#define RRPC_DEBUG_SECTOR (512)
#define RRPC_DEBUG_EXPOSED_PAGE_SIZE (4096)

#define NR_PHY_IN_LOG (RRPC_DEBUG_EXPOSED_PAGE_SIZE / RRPC_DEBUG_SECTOR)

struct rrpc_debug_inflight {
	struct list_head reqs;
	spinlock_t lock;
};

struct rrpc_debug_inflight_rq {
	struct list_head list;
	sector_t l_start;
	sector_t l_end;
};

struct rrpc_debug_rq {
	struct rrpc_debug_inflight_rq inflight_rq;
	struct rrpc_debug_addr *addr;
	unsigned long flags;
};

struct rrpc_debug_block {
	struct nvm_block *parent;
	struct list_head prio;

#define MAX_INVALID_PAGES_STORAGE 8
	/* Bitmap for invalid page intries */
	unsigned long invalid_pages[MAX_INVALID_PAGES_STORAGE];
	/* points to the next writable page within a block */
	unsigned int next_page;
	/* number of pages that are invalid, wrt host page size */
	unsigned int nr_invalid_pages;

	spinlock_t lock;
	atomic_t data_cmnt_size; /* data pages committed to stable storage */
};

struct rrpc_debug_lun {
	struct rrpc_debug *rrpc_debug;
	struct nvm_lun *parent;
	struct rrpc_debug_block *cur, *gc_cur;
	struct rrpc_debug_block *blocks;	/* Reference to block allocation */
	struct list_head prio_list;		/* Blocks that may be GC'ed */
	struct work_struct ws_gc;

	spinlock_t lock;
};

struct rrpc_debug {
	/* instance must be kept in top to resolve rrpc_debug in unprep */
	struct nvm_tgt_instance instance;

	struct nvm_dev *dev;
	struct gendisk *disk;

	u64 poffset; /* physical page offset */
	int lun_offset;

	int nr_luns;
	struct rrpc_debug_lun *luns;

	/* calculated values */
	unsigned long long nr_pages;
	unsigned long total_blocks;

	/* Write strategy variables. Move these into each for structure for each
	 * strategy
	 */
	atomic_t next_lun; /* Whenever a page is written, this is updated
			    * to point to the next write lun
			    */

	spinlock_t bio_lock;
	struct bio_list requeue_bios;
	struct work_struct ws_requeue;

	/* Simple translation map of logical addresses to physical addresses.
	 * The logical addresses is known by the host system, while the physical
	 * addresses are used when writing to the disk block device.
	 */
	struct rrpc_debug_addr *trans_map;
	/* also store a reverse map for garbage collection */
	struct rrpc_debug_rev_addr *rev_trans_map;
	spinlock_t rev_lock;

	struct rrpc_debug_inflight inflights;

	mempool_t *addr_pool;
	mempool_t *page_pool;
	mempool_t *gcb_pool;
	mempool_t *rq_pool;

	struct timer_list gc_timer;
	struct workqueue_struct *krqd_wq;
	struct workqueue_struct *kgc_wq;
};

struct rrpc_debug_block_gc {
	struct rrpc_debug *rrpc_debug;
	struct rrpc_debug_block *rblk;
	struct work_struct ws_gc;
};

/* Logical to physical mapping */
struct rrpc_debug_addr {
	u64 addr;
	struct rrpc_debug_block *rblk;
};

/* Physical to logical mapping */
struct rrpc_debug_rev_addr {
	u64 addr;
};

static inline sector_t rrpc_debug_get_laddr(struct bio *bio)
{
	return bio->bi_iter.bi_sector / NR_PHY_IN_LOG;
}

static inline unsigned int rrpc_debug_get_pages(struct bio *bio)
{
	return  bio->bi_iter.bi_size / RRPC_DEBUG_EXPOSED_PAGE_SIZE;
}

static inline sector_t rrpc_debug_get_sector(sector_t laddr)
{
	return laddr * NR_PHY_IN_LOG;
}

static inline int request_intersects(struct rrpc_debug_inflight_rq *r,
				sector_t laddr_start, sector_t laddr_end)
{
	return (laddr_end >= r->l_start && laddr_end <= r->l_end) &&
		(laddr_start >= r->l_start && laddr_start <= r->l_end);
}

static int __rrpc_debug_lock_laddr(struct rrpc_debug *rrpc_debug, sector_t laddr,
			     unsigned pages, struct rrpc_debug_inflight_rq *r)
{
	sector_t laddr_end = laddr + pages - 1;
	struct rrpc_debug_inflight_rq *rtmp;

	spin_lock_irq(&rrpc_debug->inflights.lock);
	list_for_each_entry(rtmp, &rrpc_debug->inflights.reqs, list) {
		if (unlikely(request_intersects(rtmp, laddr, laddr_end))) {
			/* existing, overlapping request, come back later */
			spin_unlock_irq(&rrpc_debug->inflights.lock);
			return 1;
		}
	}

	r->l_start = laddr;
	r->l_end = laddr_end;

	list_add_tail(&r->list, &rrpc_debug->inflights.reqs);
	spin_unlock_irq(&rrpc_debug->inflights.lock);
	return 0;
}

static inline int rrpc_debug_lock_laddr(struct rrpc_debug *rrpc_debug, sector_t laddr,
				 unsigned pages,
				 struct rrpc_debug_inflight_rq *r)
{
	BUG_ON((laddr + pages) > rrpc_debug->nr_pages);

	return __rrpc_debug_lock_laddr(rrpc_debug, laddr, pages, r);
}

static inline struct rrpc_debug_inflight_rq *rrpc_debug_get_inflight_rq(struct nvm_rq *rqd)
{
	struct rrpc_debug_rq *rrqd = nvm_rq_to_pdu(rqd);

	return &rrqd->inflight_rq;
}

static inline int rrpc_debug_lock_rq(struct rrpc_debug *rrpc_debug, struct bio *bio,
							struct nvm_rq *rqd)
{
	sector_t laddr = rrpc_debug_get_laddr(bio);
	unsigned int pages = rrpc_debug_get_pages(bio);
	struct rrpc_debug_inflight_rq *r = rrpc_debug_get_inflight_rq(rqd);

	return rrpc_debug_lock_laddr(rrpc_debug, laddr, pages, r);
}

static inline void rrpc_debug_unlock_laddr(struct rrpc_debug *rrpc_debug,
						struct rrpc_debug_inflight_rq *r)
{
	unsigned long flags;

	spin_lock_irqsave(&rrpc_debug->inflights.lock, flags);
	list_del_init(&r->list);
	spin_unlock_irqrestore(&rrpc_debug->inflights.lock, flags);
}

static inline void rrpc_debug_unlock_rq(struct rrpc_debug *rrpc_debug, struct nvm_rq *rqd)
{
	struct rrpc_debug_inflight_rq *r = rrpc_debug_get_inflight_rq(rqd);
	uint8_t pages = rqd->nr_pages;

	BUG_ON((r->l_start + pages) > rrpc_debug->nr_pages);

	rrpc_debug_unlock_laddr(rrpc_debug, r);
}

#endif /* RRPC_DEBUG_H_ */
