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

#include <linux/kernel.h>
#include "rrpc_debug.h"

static struct kmem_cache *rrpc_debug_gcb_cache, *rrpc_debug_rq_cache;
static DECLARE_RWSEM(rrpc_debug_lock);

static int rrpc_debug_submit_io(struct rrpc_debug *rrpc_debug, struct bio *bio,
				struct nvm_rq *rqd, unsigned long flags);

#define rrpc_debug_for_each_lun(rrpc_debug, rlun, i) \
		for ((i) = 0, rlun = &(rrpc_debug)->luns[0]; \
			(i) < (rrpc_debug)->nr_luns; (i)++, rlun = &(rrpc_debug)->luns[(i)])

static void rrpc_debug_page_invalidate(struct rrpc_debug *rrpc_debug, struct rrpc_debug_addr *a)
{
	struct rrpc_debug_block *rblk = a->rblk;
	unsigned int pg_offset;

	lockdep_assert_held(&rrpc_debug->rev_lock);

	if (a->addr == ADDR_EMPTY || !rblk)
		return;

	spin_lock(&rblk->lock);

	div_u64_rem(a->addr, rrpc_debug->dev->pgs_per_blk, &pg_offset);
	WARN_ON(test_and_set_bit(pg_offset, rblk->invalid_pages));
	rblk->nr_invalid_pages++;

	spin_unlock(&rblk->lock);

	rrpc_debug->rev_trans_map[a->addr - rrpc_debug->poffset].addr = ADDR_EMPTY;
}

static void rrpc_debug_invalidate_range(struct rrpc_debug *rrpc_debug, sector_t slba,
								unsigned len)
{
	sector_t i;

	spin_lock(&rrpc_debug->rev_lock);
	for (i = slba; i < slba + len; i++) {
		struct rrpc_debug_addr *gp = &rrpc_debug->trans_map[i];

		rrpc_debug_page_invalidate(rrpc_debug, gp);
		gp->rblk = NULL;
	}
	spin_unlock(&rrpc_debug->rev_lock);
}

static struct nvm_rq *rrpc_debug_inflight_laddr_acquire(struct rrpc_debug *rrpc_debug,
					sector_t laddr, unsigned int pages)
{
	struct nvm_rq *rqd;
	struct rrpc_debug_inflight_rq *inf;

	rqd = mempool_alloc(rrpc_debug->rq_pool, GFP_ATOMIC);
	if (!rqd)
		return ERR_PTR(-ENOMEM);

	inf = rrpc_debug_get_inflight_rq(rqd);
	if (rrpc_debug_lock_laddr(rrpc_debug, laddr, pages, inf)) {
		mempool_free(rqd, rrpc_debug->rq_pool);
		return NULL;
	}

	return rqd;
}

static void rrpc_debug_inflight_laddr_release(struct rrpc_debug *rrpc_debug, struct nvm_rq *rqd)
{
	struct rrpc_debug_inflight_rq *inf = rrpc_debug_get_inflight_rq(rqd);

	rrpc_debug_unlock_laddr(rrpc_debug, inf);

	mempool_free(rqd, rrpc_debug->rq_pool);
}

static void rrpc_debug_discard(struct rrpc_debug *rrpc_debug, struct bio *bio)
{
	sector_t slba = bio->bi_iter.bi_sector / NR_PHY_IN_LOG;
	sector_t len = bio->bi_iter.bi_size / RRPC_DEBUG_EXPOSED_PAGE_SIZE;
	struct nvm_rq *rqd;

	do {
		rqd = rrpc_debug_inflight_laddr_acquire(rrpc_debug, slba, len);
		schedule();
	} while (!rqd);

	if (IS_ERR(rqd)) {
		pr_err("rrpc_debug: unable to acquire inflight IO\n");
		bio_io_error(bio);
		return;
	}

	rrpc_debug_invalidate_range(rrpc_debug, slba, len);
	rrpc_debug_inflight_laddr_release(rrpc_debug, rqd);
}

static int block_is_full(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	return (rblk->next_page == rrpc_debug->dev->pgs_per_blk);
}

static u64 block_to_addr(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	struct nvm_block *blk = rblk->parent;

	return blk->id * rrpc_debug->dev->pgs_per_blk;
}

static struct ppa_addr linear_to_generic_addr(struct nvm_dev *dev,
							struct ppa_addr r)
{
	struct ppa_addr l;
	int secs, pgs, blks, luns;
	sector_t ppa = r.ppa;

	l.ppa = 0;

	div_u64_rem(ppa, dev->sec_per_pg, &secs);
	l.g.sec = secs;

	sector_div(ppa, dev->sec_per_pg);
	div_u64_rem(ppa, dev->sec_per_blk, &pgs);
	l.g.pg = pgs;

	sector_div(ppa, dev->pgs_per_blk);
	div_u64_rem(ppa, dev->blks_per_lun, &blks);
	l.g.blk = blks;

	sector_div(ppa, dev->blks_per_lun);
	div_u64_rem(ppa, dev->luns_per_chnl, &luns);
	l.g.lun = luns;

	sector_div(ppa, dev->luns_per_chnl);
	l.g.ch = ppa;

	return l;
}

static struct ppa_addr rrpc_debug_ppa_to_gaddr(struct nvm_dev *dev, u64 addr)
{
	struct ppa_addr paddr;

	paddr.ppa = addr;
	return linear_to_generic_addr(dev, paddr);
}

/* requires lun->lock taken */
static void rrpc_debug_set_lun_cur(struct rrpc_debug_lun *rlun, struct rrpc_debug_block *rblk)
{
	struct rrpc_debug *rrpc_debug = rlun->rrpc_debug;

	BUG_ON(!rblk);

	if (rlun->cur) {
		spin_lock(&rlun->cur->lock);
		WARN_ON(!block_is_full(rrpc_debug, rlun->cur));
		spin_unlock(&rlun->cur->lock);
	}
	rlun->cur = rblk;
}

static struct rrpc_debug_block *rrpc_debug_get_blk(struct rrpc_debug *rrpc_debug, struct rrpc_debug_lun *rlun,
							unsigned long flags)
{
	struct nvm_block *blk;
	struct rrpc_debug_block *rblk;

	printk(KERN_INFO "target_get_blk\n");

	blk = nvm_get_blk(rrpc_debug->dev, rlun->parent, 0);
	if (!blk)
		return NULL;

	rblk = &rlun->blocks[blk->id];
	blk->priv = rblk;

	bitmap_zero(rblk->invalid_pages, rrpc_debug->dev->pgs_per_blk);
	rblk->next_page = 0;
	rblk->nr_invalid_pages = 0;
	atomic_set(&rblk->data_cmnt_size, 0);

	return rblk;
}

static void rrpc_debug_put_blk(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	printk(KERN_INFO "target_put_blk\n");

	nvm_put_blk(rrpc_debug->dev, rblk->parent);
}

static struct rrpc_debug_lun *get_next_lun(struct rrpc_debug *rrpc_debug)
{
	int next = atomic_inc_return(&rrpc_debug->next_lun);

	return &rrpc_debug->luns[next % rrpc_debug->nr_luns];
}

static void rrpc_debug_gc_kick(struct rrpc_debug *rrpc_debug)
{
	struct rrpc_debug_lun *rlun;
	unsigned int i;

	for (i = 0; i < rrpc_debug->nr_luns; i++) {
		rlun = &rrpc_debug->luns[i];
		queue_work(rrpc_debug->krqd_wq, &rlun->ws_gc);
	}
}

/*
 * timed GC every interval.
 */
static void rrpc_debug_gc_timer(unsigned long data)
{
	struct rrpc_debug *rrpc_debug = (struct rrpc_debug *)data;

	rrpc_debug_gc_kick(rrpc_debug);
	mod_timer(&rrpc_debug->gc_timer, jiffies + msecs_to_jiffies(10));
}

static void rrpc_debug_end_sync_bio(struct bio *bio)
{
	struct completion *waiting = bio->bi_private;

	if (bio->bi_error)
		pr_err("nvm: gc request failed (%u).\n", bio->bi_error);

	complete(waiting);
}

/*
 * rrpc_debug_move_valid_pages -- migrate live data off the block
 * @rrpc_debug: the 'rrpc_debug' structure
 * @block: the block from which to migrate live pages
 *
 * Description:
 *   GC algorithms may call this function to migrate remaining live
 *   pages off the block prior to erasing it. This function blocks
 *   further execution until the operation is complete.
 */
static int rrpc_debug_move_valid_pages(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	struct request_queue *q = rrpc_debug->dev->q;
	struct rrpc_debug_rev_addr *rev;
	struct nvm_rq *rqd;
	struct bio *bio;
	struct page *page;
	int slot;
	int nr_pgs_per_blk = rrpc_debug->dev->pgs_per_blk;
	u64 phys_addr;
	DECLARE_COMPLETION_ONSTACK(wait);

	if (bitmap_full(rblk->invalid_pages, nr_pgs_per_blk))
		return 0;

	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio) {
		pr_err("nvm: could not alloc bio to gc\n");
		return -ENOMEM;
	}

	page = mempool_alloc(rrpc_debug->page_pool, GFP_NOIO);

	while ((slot = find_first_zero_bit(rblk->invalid_pages,
					    nr_pgs_per_blk)) < nr_pgs_per_blk) {

		/* Lock laddr */
		phys_addr = (rblk->parent->id * nr_pgs_per_blk) + slot;

try:
		spin_lock(&rrpc_debug->rev_lock);
		/* Get logical address from physical to logical table */
		rev = &rrpc_debug->rev_trans_map[phys_addr - rrpc_debug->poffset];
		/* already updated by previous regular write */
		if (rev->addr == ADDR_EMPTY) {
			spin_unlock(&rrpc_debug->rev_lock);
			continue;
		}

		rqd = rrpc_debug_inflight_laddr_acquire(rrpc_debug, rev->addr, 1);
		if (IS_ERR_OR_NULL(rqd)) {
			spin_unlock(&rrpc_debug->rev_lock);
			schedule();
			goto try;
		}

		spin_unlock(&rrpc_debug->rev_lock);

		/* Perform read to do GC */
		bio->bi_iter.bi_sector = rrpc_debug_get_sector(rev->addr);
		bio->bi_rw = READ;
		bio->bi_private = &wait;
		bio->bi_end_io = rrpc_debug_end_sync_bio;

		/* TODO: may fail when EXP_PG_SIZE > PAGE_SIZE */
		bio_add_pc_page(q, bio, page, RRPC_DEBUG_EXPOSED_PAGE_SIZE, 0);

		if (rrpc_debug_submit_io(rrpc_debug, bio, rqd, NVM_IOTYPE_GC)) {
			pr_err("rrpc_debug: gc read failed.\n");
			rrpc_debug_inflight_laddr_release(rrpc_debug, rqd);
			goto finished;
		}
		wait_for_completion_io(&wait);

		bio_reset(bio);
		reinit_completion(&wait);

		bio->bi_iter.bi_sector = rrpc_debug_get_sector(rev->addr);
		bio->bi_rw = WRITE;
		bio->bi_private = &wait;
		bio->bi_end_io = rrpc_debug_end_sync_bio;

		bio_add_pc_page(q, bio, page, RRPC_DEBUG_EXPOSED_PAGE_SIZE, 0);

		/* turn the command around and write the data back to a new
		 * address
		 */
		if (rrpc_debug_submit_io(rrpc_debug, bio, rqd, NVM_IOTYPE_GC)) {
			pr_err("rrpc_debug: gc write failed.\n");
			rrpc_debug_inflight_laddr_release(rrpc_debug, rqd);
			goto finished;
		}
		wait_for_completion_io(&wait);

		rrpc_debug_inflight_laddr_release(rrpc_debug, rqd);

		bio_reset(bio);
	}

finished:
	mempool_free(page, rrpc_debug->page_pool);
	bio_put(bio);

	if (!bitmap_full(rblk->invalid_pages, nr_pgs_per_blk)) {
		pr_err("nvm: failed to garbage collect block\n");
		return -EIO;
	}

	return 0;
}

static void rrpc_debug_block_gc(struct work_struct *work)
{
	struct rrpc_debug_block_gc *gcb = container_of(work, struct rrpc_debug_block_gc,
									ws_gc);
	struct rrpc_debug *rrpc_debug = gcb->rrpc_debug;
	struct rrpc_debug_block *rblk = gcb->rblk;
	struct nvm_dev *dev = rrpc_debug->dev;

	pr_debug("nvm: block '%lu' being reclaimed\n", rblk->parent->id);

	if (rrpc_debug_move_valid_pages(rrpc_debug, rblk))
		goto done;

	nvm_erase_blk(dev, rblk->parent);
	rrpc_debug_put_blk(rrpc_debug, rblk);
done:
	mempool_free(gcb, rrpc_debug->gcb_pool);
}

/* the block with highest number of invalid pages, will be in the beginning
 * of the list
 */
static struct rrpc_debug_block *rblock_max_invalid(struct rrpc_debug_block *ra,
							struct rrpc_debug_block *rb)
{
	if (ra->nr_invalid_pages == rb->nr_invalid_pages)
		return ra;

	return (ra->nr_invalid_pages < rb->nr_invalid_pages) ? rb : ra;
}

/* linearly find the block with highest number of invalid pages
 * requires lun->lock
 */
static struct rrpc_debug_block *block_prio_find_max(struct rrpc_debug_lun *rlun)
{
	struct list_head *prio_list = &rlun->prio_list;
	struct rrpc_debug_block *rblock, *max;

	BUG_ON(list_empty(prio_list));

	max = list_first_entry(prio_list, struct rrpc_debug_block, prio);
	list_for_each_entry(rblock, prio_list, prio)
		max = rblock_max_invalid(max, rblock);

	return max;
}

static void rrpc_debug_lun_gc(struct work_struct *work)
{
	struct rrpc_debug_lun *rlun = container_of(work, struct rrpc_debug_lun, ws_gc);
	struct rrpc_debug *rrpc_debug = rlun->rrpc_debug;
	struct nvm_lun *lun = rlun->parent;
	struct rrpc_debug_block_gc *gcb;
	unsigned int nr_blocks_need;

	nr_blocks_need = rrpc_debug->dev->blks_per_lun / GC_LIMIT_INVERSE;

	if (nr_blocks_need < rrpc_debug->nr_luns)
		nr_blocks_need = rrpc_debug->nr_luns;

	spin_lock(&lun->lock);
	while (nr_blocks_need > lun->nr_free_blocks &&
					!list_empty(&rlun->prio_list)) {
		struct rrpc_debug_block *rblock = block_prio_find_max(rlun);
		struct nvm_block *block = rblock->parent;

		if (!rblock->nr_invalid_pages)
			break;

		list_del_init(&rblock->prio);

		BUG_ON(!block_is_full(rrpc_debug, rblock));

		pr_debug("rrpc_debug: selected block '%lu' for GC\n", block->id);

		gcb = mempool_alloc(rrpc_debug->gcb_pool, GFP_ATOMIC);
		if (!gcb)
			break;

		gcb->rrpc_debug = rrpc_debug;
		gcb->rblk = rblock;
		INIT_WORK(&gcb->ws_gc, rrpc_debug_block_gc);

		queue_work(rrpc_debug->kgc_wq, &gcb->ws_gc);

		nr_blocks_need--;
	}
	spin_unlock(&lun->lock);

	/* TODO: Hint that request queue can be started again */
}

static void rrpc_debug_gc_queue(struct work_struct *work)
{
	struct rrpc_debug_block_gc *gcb = container_of(work, struct rrpc_debug_block_gc,
									ws_gc);
	struct rrpc_debug *rrpc_debug = gcb->rrpc_debug;
	struct rrpc_debug_block *rblk = gcb->rblk;
	struct nvm_lun *lun = rblk->parent->lun;
	struct rrpc_debug_lun *rlun = &rrpc_debug->luns[lun->id - rrpc_debug->lun_offset];

	spin_lock(&rlun->lock);
	list_add_tail(&rblk->prio, &rlun->prio_list);
	spin_unlock(&rlun->lock);

	mempool_free(gcb, rrpc_debug->gcb_pool);
	pr_debug("nvm: block '%lu' is full, allow GC (sched)\n",
							rblk->parent->id);
}

static const struct block_device_operations rrpc_debug_fops = {
	.owner		= THIS_MODULE,
};

static struct rrpc_debug_lun *rrpc_debug_get_lun_rr(struct rrpc_debug *rrpc_debug, int is_gc)
{
	unsigned int i;
	struct rrpc_debug_lun *rlun, *max_free;

	if (!is_gc)
		return get_next_lun(rrpc_debug);

	/* during GC, we don't care about RR, instead we want to make
	 * sure that we maintain evenness between the block luns.
	 */
	max_free = &rrpc_debug->luns[0];
	/* prevent GC-ing lun from devouring pages of a lun with
	 * little free blocks. We don't take the lock as we only need an
	 * estimate.
	 */
	rrpc_debug_for_each_lun(rrpc_debug, rlun, i) {
		if (rlun->parent->nr_free_blocks >
					max_free->parent->nr_free_blocks)
			max_free = rlun;
	}

	return max_free;
}

static struct rrpc_debug_addr *rrpc_debug_update_map(struct rrpc_debug *rrpc_debug, sector_t laddr,
					struct rrpc_debug_block *rblk, u64 paddr)
{
	struct rrpc_debug_addr *gp;
	struct rrpc_debug_rev_addr *rev;

	printk(KERN_INFO "target_update_map\n");

	BUG_ON(laddr >= rrpc_debug->nr_pages);

	gp = &rrpc_debug->trans_map[laddr];
	spin_lock(&rrpc_debug->rev_lock);
	if (gp->rblk)
		rrpc_debug_page_invalidate(rrpc_debug, gp);

	gp->addr = paddr;
	gp->rblk = rblk;

	rev = &rrpc_debug->rev_trans_map[gp->addr - rrpc_debug->poffset];
	rev->addr = laddr;
	spin_unlock(&rrpc_debug->rev_lock);

	return gp;
}

static u64 rrpc_debug_alloc_addr(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	u64 addr = ADDR_EMPTY;

	spin_lock(&rblk->lock);
	if (block_is_full(rrpc_debug, rblk))
		goto out;

	addr = block_to_addr(rrpc_debug, rblk) + rblk->next_page;

	rblk->next_page++;
out:
	spin_unlock(&rblk->lock);
	return addr;
}

/* Simple round-robin Logical to physical address translation.
 *
 * Retrieve the mapping using the active append point. Then update the ap for
 * the next write to the disk.
 *
 * Returns rrpc_debug_addr with the physical address and block. Remember to return to
 * rrpc_debug->addr_cache when request is finished.
 */
static struct rrpc_debug_addr *rrpc_debug_map_page(struct rrpc_debug *rrpc_debug, sector_t laddr,
								int is_gc)
{
	struct rrpc_debug_lun *rlun;
	struct rrpc_debug_block *rblk;
	struct nvm_lun *lun;
	u64 paddr;

	printk(KERN_INFO "target_map_page\n");

	rlun = rrpc_debug_get_lun_rr(rrpc_debug, is_gc);
	lun = rlun->parent;

	if (!is_gc && lun->nr_free_blocks < rrpc_debug->nr_luns * 4)
		return NULL;

	spin_lock(&rlun->lock);

	rblk = rlun->cur;
retry:
	paddr = rrpc_debug_alloc_addr(rrpc_debug, rblk);

	if (paddr == ADDR_EMPTY) {
		rblk = rrpc_debug_get_blk(rrpc_debug, rlun, 0);
		if (rblk) {
			rrpc_debug_set_lun_cur(rlun, rblk);
			goto retry;
		}

		if (is_gc) {
			/* retry from emergency gc block */
			paddr = rrpc_debug_alloc_addr(rrpc_debug, rlun->gc_cur);
			if (paddr == ADDR_EMPTY) {
				rblk = rrpc_debug_get_blk(rrpc_debug, rlun, 1);
				if (!rblk) {
					pr_err("rrpc_debug: no more blocks");
					goto err;
				}

				rlun->gc_cur = rblk;
				paddr = rrpc_debug_alloc_addr(rrpc_debug, rlun->gc_cur);
			}
			rblk = rlun->gc_cur;
		}
	}

	spin_unlock(&rlun->lock);
	return rrpc_debug_update_map(rrpc_debug, laddr, rblk, paddr);
err:
	spin_unlock(&rlun->lock);
	return NULL;
}

static void rrpc_debug_run_gc(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	struct rrpc_debug_block_gc *gcb;

	printk(KERN_INFO "target_run_gc\n");

	gcb = mempool_alloc(rrpc_debug->gcb_pool, GFP_ATOMIC);
	if (!gcb) {
		pr_err("rrpc_debug: unable to queue block for gc.");
		return;
	}

	gcb->rrpc_debug = rrpc_debug;
	gcb->rblk = rblk;

	INIT_WORK(&gcb->ws_gc, rrpc_debug_gc_queue);
	queue_work(rrpc_debug->kgc_wq, &gcb->ws_gc);
}

static void rrpc_debug_end_io_write(struct rrpc_debug *rrpc_debug, struct rrpc_debug_rq *rrqd,
						sector_t laddr, uint8_t npages)
{
	struct rrpc_debug_addr *p;
	struct rrpc_debug_block *rblk;
	struct nvm_lun *lun;
	int cmnt_size, i;

	for (i = 0; i < npages; i++) {
		p = &rrpc_debug->trans_map[laddr + i];
		rblk = p->rblk;
		lun = rblk->parent->lun;

		cmnt_size = atomic_inc_return(&rblk->data_cmnt_size);
		if (unlikely(cmnt_size == rrpc_debug->dev->pgs_per_blk))
			rrpc_debug_run_gc(rrpc_debug, rblk);
	}
}

static int rrpc_debug_end_io(struct nvm_rq *rqd, int error)
{
	struct rrpc_debug *rrpc_debug = container_of(rqd->ins, struct rrpc_debug, instance);
	struct rrpc_debug_rq *rrqd = nvm_rq_to_pdu(rqd);
	uint8_t npages = rqd->nr_pages;
	sector_t laddr = rrpc_debug_get_laddr(rqd->bio) - npages;

	printk(KERN_INFO "target_end_io\n");

	if (bio_data_dir(rqd->bio) == WRITE)
		rrpc_debug_end_io_write(rrpc_debug, rrqd, laddr, npages);

	if (rrqd->flags & NVM_IOTYPE_GC)
		return 0;

	rrpc_debug_unlock_rq(rrpc_debug, rqd);
	bio_put(rqd->bio);

	if (npages > 1)
		nvm_dev_dma_free(rrpc_debug->dev, rqd->ppa_list, rqd->dma_ppa_list);
	if (rqd->metadata)
		nvm_dev_dma_free(rrpc_debug->dev, rqd->metadata, rqd->dma_metadata);

	mempool_free(rqd, rrpc_debug->rq_pool);

	return 0;
}

static int rrpc_debug_read_ppalist_rq(struct rrpc_debug *rrpc_debug, struct bio *bio,
			struct nvm_rq *rqd, unsigned long flags, int npages)
{
	struct rrpc_debug_inflight_rq *r = rrpc_debug_get_inflight_rq(rqd);
	struct rrpc_debug_addr *gp;
	sector_t laddr = rrpc_debug_get_laddr(bio);
	int is_gc = flags & NVM_IOTYPE_GC;
	int i;

	if (!is_gc && rrpc_debug_lock_rq(rrpc_debug, bio, rqd)) {
		nvm_dev_dma_free(rrpc_debug->dev, rqd->ppa_list, rqd->dma_ppa_list);
		return NVM_IO_REQUEUE;
	}

	for (i = 0; i < npages; i++) {
		/* We assume that mapping occurs at 4KB granularity */
		BUG_ON(!(laddr + i >= 0 && laddr + i < rrpc_debug->nr_pages));
		gp = &rrpc_debug->trans_map[laddr + i];

		if (gp->rblk) {
			rqd->ppa_list[i] = rrpc_debug_ppa_to_gaddr(rrpc_debug->dev,
								gp->addr);
		} else {
			BUG_ON(is_gc);
			rrpc_debug_unlock_laddr(rrpc_debug, r);
			nvm_dev_dma_free(rrpc_debug->dev, rqd->ppa_list,
							rqd->dma_ppa_list);
			return NVM_IO_DONE;
		}
	}

	rqd->opcode = NVM_OP_HBREAD;

	return NVM_IO_OK;
}

static int rrpc_debug_read_rq(struct rrpc_debug *rrpc_debug, struct bio *bio, struct nvm_rq *rqd,
							unsigned long flags)
{
	struct rrpc_debug_rq *rrqd = nvm_rq_to_pdu(rqd);
	int is_gc = flags & NVM_IOTYPE_GC;
	sector_t laddr = rrpc_debug_get_laddr(bio);
	struct rrpc_debug_addr *gp;

	printk(KERN_INFO "target_read_rq\n");

	if (!is_gc && rrpc_debug_lock_rq(rrpc_debug, bio, rqd))
		return NVM_IO_REQUEUE;

	BUG_ON(!(laddr >= 0 && laddr < rrpc_debug->nr_pages));
	gp = &rrpc_debug->trans_map[laddr];

	if (gp->rblk) {
		rqd->ppa_addr = rrpc_debug_ppa_to_gaddr(rrpc_debug->dev, gp->addr);
	} else {
		BUG_ON(is_gc);
		rrpc_debug_unlock_rq(rrpc_debug, rqd);
		return NVM_IO_DONE;
	}

	rqd->opcode = NVM_OP_HBREAD;
	rrqd->addr = gp;

	return NVM_IO_OK;
}

static int rrpc_debug_write_ppalist_rq(struct rrpc_debug *rrpc_debug, struct bio *bio,
			struct nvm_rq *rqd, unsigned long flags, int npages)
{
	struct rrpc_debug_inflight_rq *r = rrpc_debug_get_inflight_rq(rqd);
	struct rrpc_debug_addr *p;
	sector_t laddr = rrpc_debug_get_laddr(bio);
	int is_gc = flags & NVM_IOTYPE_GC;
	int i;

	if (!is_gc && rrpc_debug_lock_rq(rrpc_debug, bio, rqd)) {
		nvm_dev_dma_free(rrpc_debug->dev, rqd->ppa_list, rqd->dma_ppa_list);
		return NVM_IO_REQUEUE;
	}

	for (i = 0; i < npages; i++) {
		/* We assume that mapping occurs at 4KB granularity */
		p = rrpc_debug_map_page(rrpc_debug, laddr + i, is_gc);
		if (!p) {
			BUG_ON(is_gc);
			rrpc_debug_unlock_laddr(rrpc_debug, r);
			nvm_dev_dma_free(rrpc_debug->dev, rqd->ppa_list,
							rqd->dma_ppa_list);
			rrpc_debug_gc_kick(rrpc_debug);
			return NVM_IO_REQUEUE;
		}

		rqd->ppa_list[i] = rrpc_debug_ppa_to_gaddr(rrpc_debug->dev,
								p->addr);
	}

	rqd->opcode = NVM_OP_HBWRITE;

	return NVM_IO_OK;
}

static int rrpc_debug_write_rq(struct rrpc_debug *rrpc_debug, struct bio *bio,
				struct nvm_rq *rqd, unsigned long flags)
{
	struct rrpc_debug_rq *rrqd = nvm_rq_to_pdu(rqd);
	struct rrpc_debug_addr *p;
	int is_gc = flags & NVM_IOTYPE_GC;
	sector_t laddr = rrpc_debug_get_laddr(bio);

	printk(KERN_INFO "target_write_rq\n");

	if (!is_gc && rrpc_debug_lock_rq(rrpc_debug, bio, rqd))
		return NVM_IO_REQUEUE;

	p = rrpc_debug_map_page(rrpc_debug, laddr, is_gc);
	if (!p) {
		BUG_ON(is_gc);
		rrpc_debug_unlock_rq(rrpc_debug, rqd);
		rrpc_debug_gc_kick(rrpc_debug);
		return NVM_IO_REQUEUE;
	}

	rqd->ppa_addr = rrpc_debug_ppa_to_gaddr(rrpc_debug->dev, p->addr);
	rqd->opcode = NVM_OP_HBWRITE;
	rrqd->addr = p;

	return NVM_IO_OK;
}

static int rrpc_debug_setup_rq(struct rrpc_debug *rrpc_debug, struct bio *bio,
			struct nvm_rq *rqd, unsigned long flags, uint8_t npages)
{
	printk(KERN_INFO "target_setup_rq\n");

	if (npages > 1) {
		rqd->ppa_list = nvm_dev_dma_alloc(rrpc_debug->dev, GFP_KERNEL,
							&rqd->dma_ppa_list);
		if (!rqd->ppa_list) {
			pr_err("rrpc_debug: not able to allocate ppa list\n");
			return NVM_IO_ERR;
		}

		if (bio_rw(bio) == WRITE)
			return rrpc_debug_write_ppalist_rq(rrpc_debug, bio, rqd, flags,
									npages);

		return rrpc_debug_read_ppalist_rq(rrpc_debug, bio, rqd, flags, npages);
	}

	if (bio_rw(bio) == WRITE)
		return rrpc_debug_write_rq(rrpc_debug, bio, rqd, flags);

	return rrpc_debug_read_rq(rrpc_debug, bio, rqd, flags);
}

static int rrpc_debug_submit_io(struct rrpc_debug *rrpc_debug, struct bio *bio,
				struct nvm_rq *rqd, unsigned long flags)
{
	int err;
	struct rrpc_debug_rq *rrq = nvm_rq_to_pdu(rqd);
	uint8_t nr_pages = rrpc_debug_get_pages(bio);
	int bio_size = bio_sectors(bio) << 9;

	printk(KERN_INFO "target_submit_io\n");

	if (bio_size < rrpc_debug->dev->sec_size)
		return NVM_IO_ERR;
	else if (bio_size > rrpc_debug->dev->max_rq_size)
		return NVM_IO_ERR;

	err = rrpc_debug_setup_rq(rrpc_debug, bio, rqd, flags, nr_pages);
	if (err)
		return err;

	bio_get(bio);
	rqd->bio = bio;
	rqd->ins = &rrpc_debug->instance;
	rqd->nr_pages = nr_pages;
	rrq->flags = flags;

	err = nvm_submit_io(rrpc_debug->dev, rqd);
        err = nvm_submit_io(rrpc_debug->dev, rqd);
	if (err) {
		pr_err("rrpc_debug: I/O submission failed: %d\n", err);
		return NVM_IO_ERR;
	}

	return NVM_IO_OK;
}

static blk_qc_t rrpc_debug_make_rq(struct request_queue *q, struct bio *bio)
{
	struct rrpc_debug *rrpc_debug = q->queuedata;
	struct nvm_rq *rqd;
	int err;

	printk(KERN_INFO "target_make_rq\n");

	if (bio->bi_rw & REQ_DISCARD) {
		rrpc_debug_discard(rrpc_debug, bio);
		return BLK_QC_T_NONE;
	}

	rqd = mempool_alloc(rrpc_debug->rq_pool, GFP_KERNEL);
	if (!rqd) {
		pr_err_ratelimited("rrpc_debug: not able to queue bio.");
		bio_io_error(bio);
		return BLK_QC_T_NONE;
	}
	memset(rqd, 0, sizeof(struct nvm_rq));

	err = rrpc_debug_submit_io(rrpc_debug, bio, rqd, NVM_IOTYPE_NONE);
	switch (err) {
	case NVM_IO_OK:
		return BLK_QC_T_NONE;
	case NVM_IO_ERR:
		bio_io_error(bio);
		break;
	case NVM_IO_DONE:
		bio_endio(bio);
		break;
	case NVM_IO_REQUEUE:
		spin_lock(&rrpc_debug->bio_lock);
		bio_list_add(&rrpc_debug->requeue_bios, bio);
		spin_unlock(&rrpc_debug->bio_lock);
		queue_work(rrpc_debug->kgc_wq, &rrpc_debug->ws_requeue);
		break;
	}

	mempool_free(rqd, rrpc_debug->rq_pool);
	return BLK_QC_T_NONE;
}

static void rrpc_debug_requeue(struct work_struct *work)
{
	struct rrpc_debug *rrpc_debug = container_of(work, struct rrpc_debug, ws_requeue);
	struct bio_list bios;
	struct bio *bio;

	bio_list_init(&bios);

	spin_lock(&rrpc_debug->bio_lock);
	bio_list_merge(&bios, &rrpc_debug->requeue_bios);
	bio_list_init(&rrpc_debug->requeue_bios);
	spin_unlock(&rrpc_debug->bio_lock);

	while ((bio = bio_list_pop(&bios)))
		rrpc_debug_make_rq(rrpc_debug->disk->queue, bio);
}

static void rrpc_debug_gc_free(struct rrpc_debug *rrpc_debug)
{
	struct rrpc_debug_lun *rlun;
	int i;

	if (rrpc_debug->krqd_wq)
		destroy_workqueue(rrpc_debug->krqd_wq);

	if (rrpc_debug->kgc_wq)
		destroy_workqueue(rrpc_debug->kgc_wq);

	if (!rrpc_debug->luns)
		return;

	for (i = 0; i < rrpc_debug->nr_luns; i++) {
		rlun = &rrpc_debug->luns[i];

		if (!rlun->blocks)
			break;
		vfree(rlun->blocks);
	}
}

static int rrpc_debug_gc_init(struct rrpc_debug *rrpc_debug)
{
	rrpc_debug->krqd_wq = alloc_workqueue("rrpc_debug-lun", WQ_MEM_RECLAIM|WQ_UNBOUND,
								rrpc_debug->nr_luns);
	if (!rrpc_debug->krqd_wq)
		return -ENOMEM;

	rrpc_debug->kgc_wq = alloc_workqueue("rrpc_debug-bg", WQ_MEM_RECLAIM, 1);
	if (!rrpc_debug->kgc_wq)
		return -ENOMEM;

	setup_timer(&rrpc_debug->gc_timer, rrpc_debug_gc_timer, (unsigned long)rrpc_debug);

	return 0;
}

static void rrpc_debug_map_free(struct rrpc_debug *rrpc_debug)
{
	vfree(rrpc_debug->rev_trans_map);
	vfree(rrpc_debug->trans_map);
}

static int rrpc_debug_l2p_update(u64 slba, u32 nlb, __le64 *entries, void *private)
{
	struct rrpc_debug *rrpc_debug = (struct rrpc_debug *)private;
	struct nvm_dev *dev = rrpc_debug->dev;
	struct rrpc_debug_addr *addr = rrpc_debug->trans_map + slba;
	struct rrpc_debug_rev_addr *raddr = rrpc_debug->rev_trans_map;
	sector_t max_pages = dev->total_pages * (dev->sec_size >> 9);
	u64 elba = slba + nlb;
	u64 i;

	if (unlikely(elba > dev->total_pages)) {
		pr_err("nvm: L2P data from device is out of bounds!\n");
		return -EINVAL;
	}

	for (i = 0; i < nlb; i++) {
		u64 pba = le64_to_cpu(entries[i]);
		/* LNVM treats address-spaces as silos, LBA and PBA are
		 * equally large and zero-indexed.
		 */
		if (unlikely(pba >= max_pages && pba != U64_MAX)) {
			pr_err("nvm: L2P data entry is out of bounds!\n");
			return -EINVAL;
		}

		/* Address zero is a special one. The first page on a disk is
		 * protected. As it often holds internal device boot
		 * information.
		 */
		if (!pba)
			continue;

		addr[i].addr = pba;
		raddr[pba].addr = slba + i;
	}

	return 0;
}

static int rrpc_debug_map_init(struct rrpc_debug *rrpc_debug)
{
	struct nvm_dev *dev = rrpc_debug->dev;
	sector_t i;
	int ret;

	rrpc_debug->trans_map = vzalloc(sizeof(struct rrpc_debug_addr) * rrpc_debug->nr_pages);
	if (!rrpc_debug->trans_map)
		return -ENOMEM;

	rrpc_debug->rev_trans_map = vmalloc(sizeof(struct rrpc_debug_rev_addr)
							* rrpc_debug->nr_pages);
	if (!rrpc_debug->rev_trans_map)
		return -ENOMEM;

	for (i = 0; i < rrpc_debug->nr_pages; i++) {
		struct rrpc_debug_addr *p = &rrpc_debug->trans_map[i];
		struct rrpc_debug_rev_addr *r = &rrpc_debug->rev_trans_map[i];

		p->addr = ADDR_EMPTY;
		r->addr = ADDR_EMPTY;
	}

	if (!dev->ops->get_l2p_tbl)
		return 0;

	/* Bring up the mapping table from device */
	ret = dev->ops->get_l2p_tbl(dev, 0, dev->total_pages,
							rrpc_debug_l2p_update, rrpc_debug);
	if (ret) {
		pr_err("nvm: rrpc_debug: could not read L2P table.\n");
		return -EINVAL;
	}

	return 0;
}


/* Minimum pages needed within a lun */
#define PAGE_POOL_SIZE 16
#define ADDR_POOL_SIZE 64

static int rrpc_debug_core_init(struct rrpc_debug *rrpc_debug)
{
	down_write(&rrpc_debug_lock);
	if (!rrpc_debug_gcb_cache) {
		rrpc_debug_gcb_cache = kmem_cache_create("rrpc_debug_gcb",
				sizeof(struct rrpc_debug_block_gc), 0, 0, NULL);
		if (!rrpc_debug_gcb_cache) {
			up_write(&rrpc_debug_lock);
			return -ENOMEM;
		}

		rrpc_debug_rq_cache = kmem_cache_create("rrpc_debug_rq",
				sizeof(struct nvm_rq) + sizeof(struct rrpc_debug_rq),
				0, 0, NULL);
		if (!rrpc_debug_rq_cache) {
			kmem_cache_destroy(rrpc_debug_gcb_cache);
			up_write(&rrpc_debug_lock);
			return -ENOMEM;
		}
	}
	up_write(&rrpc_debug_lock);

	rrpc_debug->page_pool = mempool_create_page_pool(PAGE_POOL_SIZE, 0);
	if (!rrpc_debug->page_pool)
		return -ENOMEM;

	rrpc_debug->gcb_pool = mempool_create_slab_pool(rrpc_debug->dev->nr_luns,
								rrpc_debug_gcb_cache);
	if (!rrpc_debug->gcb_pool)
		return -ENOMEM;

	rrpc_debug->rq_pool = mempool_create_slab_pool(64, rrpc_debug_rq_cache);
	if (!rrpc_debug->rq_pool)
		return -ENOMEM;

	spin_lock_init(&rrpc_debug->inflights.lock);
	INIT_LIST_HEAD(&rrpc_debug->inflights.reqs);

	return 0;
}

static void rrpc_debug_core_free(struct rrpc_debug *rrpc_debug)
{
	mempool_destroy(rrpc_debug->page_pool);
	mempool_destroy(rrpc_debug->gcb_pool);
	mempool_destroy(rrpc_debug->rq_pool);
}

static void rrpc_debug_luns_free(struct rrpc_debug *rrpc_debug)
{
	kfree(rrpc_debug->luns);
}

static int rrpc_debug_luns_init(struct rrpc_debug *rrpc_debug, int lun_begin, int lun_end)
{
	struct nvm_dev *dev = rrpc_debug->dev;
	struct rrpc_debug_lun *rlun;
	int i, j;

	spin_lock_init(&rrpc_debug->rev_lock);

	rrpc_debug->luns = kcalloc(rrpc_debug->nr_luns, sizeof(struct rrpc_debug_lun),
								GFP_KERNEL);
	if (!rrpc_debug->luns)
		return -ENOMEM;

	/* 1:1 mapping */
	for (i = 0; i < rrpc_debug->nr_luns; i++) {
		struct nvm_lun *lun = dev->mt->get_lun(dev, lun_begin + i);

		if (dev->pgs_per_blk >
				MAX_INVALID_PAGES_STORAGE * BITS_PER_LONG) {
			pr_err("rrpc_debug: number of pages per block too high.");
			goto err;
		}

		rlun = &rrpc_debug->luns[i];
		rlun->rrpc_debug = rrpc_debug;
		rlun->parent = lun;
		INIT_LIST_HEAD(&rlun->prio_list);
		INIT_WORK(&rlun->ws_gc, rrpc_debug_lun_gc);
		spin_lock_init(&rlun->lock);

		rrpc_debug->total_blocks += dev->blks_per_lun;
		rrpc_debug->nr_pages += dev->sec_per_lun;

		rlun->blocks = vzalloc(sizeof(struct rrpc_debug_block) *
						rrpc_debug->dev->blks_per_lun);
		if (!rlun->blocks)
			goto err;

		for (j = 0; j < rrpc_debug->dev->blks_per_lun; j++) {
			struct rrpc_debug_block *rblk = &rlun->blocks[j];
			struct nvm_block *blk = &lun->blocks[j];

			rblk->parent = blk;
			INIT_LIST_HEAD(&rblk->prio);
			spin_lock_init(&rblk->lock);
		}
	}

	return 0;
err:
	return -ENOMEM;
}

static void rrpc_debug_free(struct rrpc_debug *rrpc_debug)
{
	rrpc_debug_gc_free(rrpc_debug);
	rrpc_debug_map_free(rrpc_debug);
	rrpc_debug_core_free(rrpc_debug);
	rrpc_debug_luns_free(rrpc_debug);

	kfree(rrpc_debug);
}

static void rrpc_debug_exit(void *private)
{
	struct rrpc_debug *rrpc_debug = private;

	del_timer(&rrpc_debug->gc_timer);

	flush_workqueue(rrpc_debug->krqd_wq);
	flush_workqueue(rrpc_debug->kgc_wq);

	rrpc_debug_free(rrpc_debug);
}

static sector_t rrpc_debug_capacity(void *private)
{
	struct rrpc_debug *rrpc_debug = private;
	
	sector_t total = rrpc_debug->nr_pages * NR_PHY_IN_LOG;

	printk(KERN_INFO "pages: %llu, sectors: %llu",(unsigned long long)rrpc_debug->nr_pages,(unsigned long long)total); 

	return total;
}

/*
 * Looks up the logical address from reverse trans map and check if its valid by
 * comparing the logical to physical address with the physical address.
 * Returns 0 on free, otherwise 1 if in use
 */
static void rrpc_debug_block_map_update(struct rrpc_debug *rrpc_debug, struct rrpc_debug_block *rblk)
{
	struct nvm_dev *dev = rrpc_debug->dev;
	int offset;
	struct rrpc_debug_addr *laddr;
	u64 paddr, pladdr;

	for (offset = 0; offset < dev->pgs_per_blk; offset++) {
		paddr = block_to_addr(rrpc_debug, rblk) + offset;

		pladdr = rrpc_debug->rev_trans_map[paddr].addr;
		if (pladdr == ADDR_EMPTY)
			continue;

		laddr = &rrpc_debug->trans_map[pladdr];

		if (paddr == laddr->addr) {
			laddr->rblk = rblk;
		} else {
			set_bit(offset, rblk->invalid_pages);
			rblk->nr_invalid_pages++;
		}
	}
}

static int rrpc_debug_blocks_init(struct rrpc_debug *rrpc_debug)
{
	struct rrpc_debug_lun *rlun;
	struct rrpc_debug_block *rblk;
	int lun_iter, blk_iter;

	for (lun_iter = 0; lun_iter < rrpc_debug->nr_luns; lun_iter++) {
		rlun = &rrpc_debug->luns[lun_iter];

		for (blk_iter = 0; blk_iter < rrpc_debug->dev->blks_per_lun;
								blk_iter++) {
			rblk = &rlun->blocks[blk_iter];
			rrpc_debug_block_map_update(rrpc_debug, rblk);
		}
	}

	return 0;
}

static int rrpc_debug_luns_configure(struct rrpc_debug *rrpc_debug)
{
	struct rrpc_debug_lun *rlun;
	struct rrpc_debug_block *rblk;
	int i;

	for (i = 0; i < rrpc_debug->nr_luns; i++) {
		rlun = &rrpc_debug->luns[i];

		rblk = rrpc_debug_get_blk(rrpc_debug, rlun, 0);
		if (!rblk)
			return -EINVAL;

		rrpc_debug_set_lun_cur(rlun, rblk);

		/* Emergency gc block */
		rblk = rrpc_debug_get_blk(rrpc_debug, rlun, 1);
		if (!rblk)
			return -EINVAL;
		rlun->gc_cur = rblk;
	}

	return 0;
}

static struct nvm_tgt_type tt_rrpc_debug;

static void *rrpc_debug_init(struct nvm_dev *dev, struct gendisk *tdisk,
						int lun_begin, int lun_end)
{
	struct request_queue *bqueue = dev->q;
	struct request_queue *tqueue = tdisk->queue;
	struct rrpc_debug *rrpc_debug;
	int ret;

	printk(KERN_INFO "target_init\n");

	if (!(dev->identity.dom & NVM_RSP_L2P)) {
		pr_err("nvm: rrpc_debug: device does not support l2p (%x)\n",
							dev->identity.dom);
		return ERR_PTR(-EINVAL);
	}

	rrpc_debug = kzalloc(sizeof(struct rrpc_debug), GFP_KERNEL);
	if (!rrpc_debug)
		return ERR_PTR(-ENOMEM);

	rrpc_debug->instance.tt = &tt_rrpc_debug;
	rrpc_debug->dev = dev;
	rrpc_debug->disk = tdisk;

	bio_list_init(&rrpc_debug->requeue_bios);
	spin_lock_init(&rrpc_debug->bio_lock);
	INIT_WORK(&rrpc_debug->ws_requeue, rrpc_debug_requeue);

	rrpc_debug->nr_luns = lun_end - lun_begin + 1;

	/* simple round-robin strategy */
	atomic_set(&rrpc_debug->next_lun, -1);

	ret = rrpc_debug_luns_init(rrpc_debug, lun_begin, lun_end);
	if (ret) {
		pr_err("nvm: rrpc_debug: could not initialize luns\n");
		goto err;
	}

	rrpc_debug->poffset = dev->sec_per_lun * lun_begin;
	rrpc_debug->lun_offset = lun_begin;

	ret = rrpc_debug_core_init(rrpc_debug);
	if (ret) {
		pr_err("nvm: rrpc_debug: could not initialize core\n");
		goto err;
	}

	ret = rrpc_debug_map_init(rrpc_debug);
	if (ret) {
		pr_err("nvm: rrpc_debug: could not initialize maps\n");
		goto err;
	}

	ret = rrpc_debug_blocks_init(rrpc_debug);
	if (ret) {
		pr_err("nvm: rrpc_debug: could not initialize state for blocks\n");
		goto err;
	}

	ret = rrpc_debug_luns_configure(rrpc_debug);
	if (ret) {
		pr_err("nvm: rrpc_debug: not enough blocks available in LUNs.\n");
		goto err;
	}

	ret = rrpc_debug_gc_init(rrpc_debug);
	if (ret) {
		pr_err("nvm: rrpc_debug: could not initialize gc\n");
		goto err;
	}

	/* inherit the size from the underlying device */
	blk_queue_logical_block_size(tqueue, queue_physical_block_size(bqueue));
	blk_queue_max_hw_sectors(tqueue, queue_max_hw_sectors(bqueue));

	pr_info("nvm: rrpc_debug initialized with %u luns and %llu pages.\n",
			rrpc_debug->nr_luns, (unsigned long long)rrpc_debug->nr_pages);

	mod_timer(&rrpc_debug->gc_timer, jiffies + msecs_to_jiffies(10));

	return rrpc_debug;
err:
	rrpc_debug_free(rrpc_debug);
	return ERR_PTR(ret);
}

/* round robin, page-based FTL, and cost-based GC */
static struct nvm_tgt_type tt_rrpc_debug = {
	.name		= "rrpc_debug",
	.version	= {1, 0, 0},

	.make_rq	= rrpc_debug_make_rq,
	.capacity	= rrpc_debug_capacity,
	.end_io		= rrpc_debug_end_io,

	.init		= rrpc_debug_init,
	.exit		= rrpc_debug_exit,
};

static int __init rrpc_debug_module_init(void)
{
	printk(KERN_INFO "init");
	return nvm_register_target(&tt_rrpc_debug);
	printk(KERN_INFO "init_ok");
}

static void rrpc_debug_module_exit(void)
{
	nvm_unregister_target(&tt_rrpc_debug);
}

module_init(rrpc_debug_module_init);
module_exit(rrpc_debug_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Block-Device Target for Open-Channel SSDs");
