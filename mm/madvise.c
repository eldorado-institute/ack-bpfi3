// SPDX-License-Identifier: GPL-2.0
/*
 *	linux/mm/madvise.c
 *
 * Copyright (C) 1999  Linus Torvalds
 * Copyright (C) 2002  Christoph Hellwig
 */

#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/mempolicy.h>
#include <linux/page_size_compat.h>
#include <linux/page-isolation.h>
#include <linux/pgsize_migration.h>
#include <linux/page_idle.h>
#include <linux/userfaultfd_k.h>
#include <linux/hugetlb.h>
#include <linux/falloc.h>
#include <linux/fadvise.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm_inline.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/ksm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/pagewalk.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/shmem_fs.h>
#include <linux/mmu_notifier.h>
#include <trace/hooks/mm.h>

#include <asm/tlb.h>

#include "internal.h"
#include "swap.h"

#include <trace/hooks/madvise.h>

struct madvise_walk_private {
	struct mmu_gather *tlb;
	bool pageout;
};

/*
 * Any behaviour which results in changes to the vma->vm_flags needs to
 * take mmap_lock for writing. Others, which simply traverse vmas, need
 * to only take it for reading.
 */
static int madvise_need_mmap_write(int behavior)
{
	switch (behavior) {
	case MADV_REMOVE:
	case MADV_WILLNEED:
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
	case MADV_COLD:
	case MADV_PAGEOUT:
	case MADV_FREE:
	case MADV_POPULATE_READ:
	case MADV_POPULATE_WRITE:
	case MADV_COLLAPSE:
		return 0;
	default:
		/* be safe, default to 1. list exceptions explicitly */
		return 1;
	}
}

#ifdef CONFIG_ANON_VMA_NAME
struct anon_vma_name *anon_vma_name_alloc(const char *name)
{
	struct anon_vma_name *anon_name;
	size_t count;

	/* Add 1 for NUL terminator at the end of the anon_name->name */
	count = strlen(name) + 1;
	anon_name = kmalloc(struct_size(anon_name, name, count), GFP_KERNEL);
	if (anon_name) {
		kref_init(&anon_name->kref);
		memcpy(anon_name->name, name, count);
	}

	return anon_name;
}

void anon_vma_name_free(struct kref *kref)
{
	struct anon_vma_name *anon_name =
			container_of(kref, struct anon_vma_name, kref);
	kfree(anon_name);
}

struct anon_vma_name *anon_vma_name(struct vm_area_struct *vma)
{
	mmap_assert_locked(vma->vm_mm);

	return vma->anon_name;
}
EXPORT_SYMBOL_GPL(anon_vma_name);

/* mmap_lock should be write-locked */
static int replace_anon_vma_name(struct vm_area_struct *vma,
				 struct anon_vma_name *anon_name)
{
	struct anon_vma_name *orig_name = anon_vma_name(vma);

	if (!anon_name) {
		vma->anon_name = NULL;
		anon_vma_name_put(orig_name);
		return 0;
	}

	if (anon_vma_name_eq(orig_name, anon_name))
		return 0;

	vma->anon_name = anon_vma_name_reuse(anon_name);
	anon_vma_name_put(orig_name);

	return 0;
}
#else /* CONFIG_ANON_VMA_NAME */
static int replace_anon_vma_name(struct vm_area_struct *vma,
				 struct anon_vma_name *anon_name)
{
	if (anon_name)
		return -EINVAL;

	return 0;
}
#endif /* CONFIG_ANON_VMA_NAME */
/*
 * Update the vm_flags on region of a vma, splitting it or merging it as
 * necessary.  Must be called with mmap_lock held for writing;
 * Caller should ensure anon_name stability by raising its refcount even when
 * anon_name belongs to a valid vma because this function might free that vma.
 */
static int madvise_update_vma(struct vm_area_struct *vma,
			      struct vm_area_struct **prev, unsigned long start,
			      unsigned long end, unsigned long new_flags,
			      struct anon_vma_name *anon_name)
{
	struct mm_struct *mm = vma->vm_mm;
	int error;
	pgoff_t pgoff;
	VMA_ITERATOR(vmi, mm, start);

	if (new_flags == vma->vm_flags && anon_vma_name_eq(anon_vma_name(vma), anon_name)) {
		*prev = vma;
		return 0;
	}

	pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
	*prev = vma_merge(&vmi, mm, *prev, start, end, new_flags,
			  vma->anon_vma, vma->vm_file, pgoff, vma_policy(vma),
			  vma->vm_userfaultfd_ctx, anon_name);
	if (*prev) {
		vma = *prev;
		goto success;
	}

	*prev = vma;

	if (start != vma->vm_start) {
		error = split_vma(&vmi, vma, start, 1);
		if (error)
			return error;
	}

	if (end != vma->vm_end) {
		error = split_vma(&vmi, vma, end, 0);
		if (error)
			return error;
	}

success:
	/* vm_flags is protected by the mmap_lock held in write mode. */
	vma_start_write(vma);
	vm_flags_reset(vma, new_flags);
	if (!vma->vm_file || vma_is_anon_shmem(vma)) {
		error = replace_anon_vma_name(vma, anon_name);
		if (error)
			return error;
	}

	return 0;
}

#ifdef CONFIG_SWAP
static int swapin_walk_pmd_entry(pmd_t *pmd, unsigned long start,
		unsigned long end, struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->private;
	struct swap_iocb *splug = NULL;
	pte_t *ptep = NULL;
	spinlock_t *ptl;
	unsigned long addr;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		pte_t pte;
		swp_entry_t entry;
		struct page *page;

		if (!ptep++) {
			ptep = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
			if (!ptep)
				break;
		}

		pte = ptep_get(ptep);
		if (!is_swap_pte(pte))
			continue;
		entry = pte_to_swp_entry(pte);
		if (unlikely(non_swap_entry(entry)))
			continue;

		pte_unmap_unlock(ptep, ptl);
		ptep = NULL;
		trace_android_vh_madvise_swapin_walk_pmd_entry(entry);

		page = read_swap_cache_async(entry, GFP_HIGHUSER_MOVABLE,
					     vma, addr, &splug);
		if (page)
			put_page(page);
	}

	if (ptep)
		pte_unmap_unlock(ptep, ptl);
	swap_read_unplug(splug);
	cond_resched();

	return 0;
}

static const struct mm_walk_ops swapin_walk_ops = {
	.pmd_entry		= swapin_walk_pmd_entry,
	.walk_lock		= PGWALK_RDLOCK,
};

static void shmem_swapin_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end,
		struct address_space *mapping)
{
	XA_STATE(xas, &mapping->i_pages, linear_page_index(vma, start));
	pgoff_t end_index = linear_page_index(vma, end) - 1;
	struct page *page;
	struct swap_iocb *splug = NULL;

	rcu_read_lock();
	xas_for_each(&xas, page, end_index) {
		unsigned long addr;
		swp_entry_t entry;

		if (!xa_is_value(page))
			continue;
		entry = radix_to_swp_entry(page);
		/* There might be swapin error entries in shmem mapping. */
		if (non_swap_entry(entry))
			continue;

		addr = vma->vm_start +
			((xas.xa_index - vma->vm_pgoff) << PAGE_SHIFT);
		xas_pause(&xas);
		rcu_read_unlock();

		page = read_swap_cache_async(entry, mapping_gfp_mask(mapping),
					     vma, addr, &splug);
		if (page)
			put_page(page);

		rcu_read_lock();
	}
	rcu_read_unlock();
	swap_read_unplug(splug);
}
#endif		/* CONFIG_SWAP */

/*
 * Schedule all required I/O operations.  Do not wait for completion.
 */
static long madvise_willneed(struct vm_area_struct *vma,
			     struct vm_area_struct **prev,
			     unsigned long start, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	struct file *file = vma->vm_file;
	loff_t offset;

	*prev = vma;
#ifdef CONFIG_SWAP
	if (!file) {
		walk_page_range(vma->vm_mm, start, end, &swapin_walk_ops, vma);
		lru_add_drain(); /* Push any new pages onto the LRU now */
		return 0;
	}

	if (shmem_mapping(file->f_mapping)) {
		shmem_swapin_range(vma, start, end, file->f_mapping);
		lru_add_drain(); /* Push any new pages onto the LRU now */
		return 0;
	}
#else
	if (!file)
		return -EBADF;
#endif

	if (IS_DAX(file_inode(file))) {
		/* no bad return value, but ignore advice */
		return 0;
	}

	/*
	 * Filesystem's fadvise may need to take various locks.  We need to
	 * explicitly grab a reference because the vma (and hence the
	 * vma's reference to the file) can go away as soon as we drop
	 * mmap_lock.
	 */
	*prev = NULL;	/* tell sys_madvise we drop mmap_lock */
	get_file(file);
	offset = (loff_t)(start - vma->vm_start)
			+ ((loff_t)vma->vm_pgoff << PAGE_SHIFT);
	mmap_read_unlock(mm);
	vfs_fadvise(file, offset, end - start, POSIX_FADV_WILLNEED);
	fput(file);
	mmap_read_lock(mm);
	return 0;
}

static inline bool can_do_file_pageout(struct vm_area_struct *vma)
{
	if (!vma->vm_file)
		return false;
	/*
	 * paging out pagecache only for non-anonymous mappings that correspond
	 * to the files the calling process could (if tried) open for writing;
	 * otherwise we'd be including shared non-exclusive mappings, which
	 * opens a side channel.
	 */
	return inode_owner_or_capable(&nop_mnt_idmap,
				      file_inode(vma->vm_file)) ||
	       file_permission(vma->vm_file, MAY_WRITE) == 0;
}

static inline int madvise_folio_pte_batch(unsigned long addr, unsigned long end,
					  struct folio *folio, pte_t *ptep,
					  pte_t pte, bool *any_young,
					  bool *any_dirty)
{
	const fpb_t fpb_flags = FPB_IGNORE_DIRTY | FPB_IGNORE_SOFT_DIRTY;
	int max_nr = (end - addr) / PAGE_SIZE;

	return folio_pte_batch(folio, addr, ptep, pte, max_nr, fpb_flags, NULL,
			       any_young, any_dirty);
}

static int madvise_cold_or_pageout_pte_range(pmd_t *pmd,
				unsigned long addr, unsigned long end,
				struct mm_walk *walk)
{
	struct madvise_walk_private *private = walk->private;
	struct mmu_gather *tlb = private->tlb;
	bool pageout = private->pageout;
	struct mm_struct *mm = tlb->mm;
	struct vm_area_struct *vma = walk->vma;
	pte_t *start_pte, *pte, ptent;
	spinlock_t *ptl;
	struct folio *folio = NULL;
	LIST_HEAD(folio_list);
	bool pageout_anon_only_filter;
	bool abort_madvise = false;
	int nr;
	swp_entry_t entry;
	int ret = 0;

	trace_android_vh_madvise_cold_or_pageout_abort(vma, &abort_madvise);
	if (fatal_signal_pending(current) || abort_madvise)
		return -EINTR;

	trace_android_vh_madvise_pageout_bypass(mm, pageout, &ret);
	if (ret)
		return ret;

	pageout_anon_only_filter = pageout && !vma_is_anonymous(vma) &&
					!can_do_file_pageout(vma);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (pmd_trans_huge(*pmd)) {
		pmd_t orig_pmd;
		unsigned long next = pmd_addr_end(addr, end);

		tlb_change_page_size(tlb, HPAGE_PMD_SIZE);
		ptl = pmd_trans_huge_lock(pmd, vma);
		if (!ptl)
			return 0;

		orig_pmd = *pmd;
		if (is_huge_zero_pmd(orig_pmd))
			goto huge_unlock;

		if (unlikely(!pmd_present(orig_pmd))) {
			VM_BUG_ON(thp_migration_supported() &&
					!is_pmd_migration_entry(orig_pmd));
			goto huge_unlock;
		}

		folio = pfn_folio(pmd_pfn(orig_pmd));

		/* Do not interfere with other mappings of this folio */
		if (folio_likely_mapped_shared(folio))
			goto huge_unlock;

		if (pageout_anon_only_filter && !folio_test_anon(folio))
			goto huge_unlock;

		if (next - addr != HPAGE_PMD_SIZE) {
			int err;

			folio_get(folio);
			spin_unlock(ptl);
			folio_lock(folio);
			err = split_folio(folio);
			folio_unlock(folio);
			folio_put(folio);
			if (err >= 0)
				goto regular_folio;
			return 0;
		}

		if (!pageout && pmd_young(orig_pmd)) {
			pmdp_invalidate(vma, addr, pmd);
			orig_pmd = pmd_mkold(orig_pmd);

			set_pmd_at(mm, addr, pmd, orig_pmd);
			tlb_remove_pmd_tlb_entry(tlb, pmd, addr);
		}

		folio_clear_referenced(folio);
		folio_test_clear_young(folio);
		if (folio_test_active(folio))
			folio_set_workingset(folio);
		if (pageout) {
			if (folio_isolate_lru(folio)) {
				if (folio_test_unevictable(folio))
					folio_putback_lru(folio);
				else
					list_add(&folio->lru, &folio_list);
			}
		} else
			folio_deactivate(folio);
huge_unlock:
		spin_unlock(ptl);
		if (pageout)
			reclaim_pages(&folio_list, true);
		return 0;
	}

regular_folio:
#endif
	tlb_change_page_size(tlb, PAGE_SIZE);
	start_pte = pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	if (!start_pte)
		return 0;
	flush_tlb_batched_pending(mm);
	arch_enter_lazy_mmu_mode();
	for (; addr < end; pte += nr, addr += nr * PAGE_SIZE) {
		bool need_skip = false;
		nr = 1;
		ptent = ptep_get(pte);

		if (pte_none(ptent))
			continue;

		if (!pte_present(ptent)) {
			entry = pte_to_swp_entry(ptent);
			if (!non_swap_entry(entry))
				trace_android_vh_madvise_pageout_swap_entry(entry,
					swp_swapcount(entry), NULL);
			continue;
		}

		folio = vm_normal_folio(vma, addr, ptent);
		if (!folio || folio_is_zone_device(folio))
			continue;

		trace_android_vh_madvise_cold_pageout_skip(vma, folio, pageout,
			&need_skip);

		if (need_skip)
			continue;

		/*
		 * If we encounter a large folio, only split it if it is not
		 * fully mapped within the range we are operating on. Otherwise
		 * leave it as is so that it can be swapped out whole. If we
		 * fail to split a folio, leave it in place and advance to the
		 * next pte in the range.
		 */
		if (folio_test_large(folio)) {
			bool any_young;

			nr = madvise_folio_pte_batch(addr, end, folio, pte,
						     ptent, &any_young, NULL);

			if (any_young)
				ptent = pte_mkyoung(ptent);

			if (nr < folio_nr_pages(folio)) {
				int err;
				bool bypass = false;

				trace_android_vh_split_large_folio_bypass(&bypass);
				if (bypass)
					continue;
				if (folio_likely_mapped_shared(folio))
					continue;
				if (pageout_anon_only_filter && !folio_test_anon(folio))
					continue;
				if (!folio_trylock(folio))
					continue;
				folio_get(folio);
				arch_leave_lazy_mmu_mode();
				pte_unmap_unlock(start_pte, ptl);
				start_pte = NULL;
				err = split_folio(folio);
				folio_unlock(folio);
				folio_put(folio);
				start_pte = pte =
					pte_offset_map_lock(mm, pmd, addr, &ptl);
				if (!start_pte)
					break;
				arch_enter_lazy_mmu_mode();
				if (err >= 0)
					nr = 0;
				continue;
			}
		}

		/*
		 * Do not interfere with other mappings of this folio and
		 * non-LRU folio. If we have a large folio at this point, we
		 * know it is fully mapped so if its mapcount is the same as its
		 * number of pages, it must be exclusive.
		 */
		if (!folio_test_lru(folio) ||
		    folio_mapcount(folio) != folio_nr_pages(folio))
			continue;

		if (pageout_anon_only_filter && !folio_test_anon(folio))
			continue;

		if (!pageout && pte_young(ptent)) {
			clear_young_dirty_ptes(vma, addr, pte, nr,
					       CYDP_CLEAR_YOUNG);
			tlb_remove_tlb_entries(tlb, pte, nr, addr);
		}

		/*
		 * We are deactivating a folio for accelerating reclaiming.
		 * VM couldn't reclaim the folio unless we clear PG_young.
		 * As a side effect, it makes confuse idle-page tracking
		 * because they will miss recent referenced history.
		 */
		folio_clear_referenced(folio);
		folio_test_clear_young(folio);
		if (folio_test_active(folio))
			folio_set_workingset(folio);
		if (pageout) {
			if (folio_isolate_lru(folio)) {
				if (folio_test_unevictable(folio))
					folio_putback_lru(folio);
				else
					list_add(&folio->lru, &folio_list);
			}
		} else
			folio_deactivate(folio);
	}

	if (start_pte) {
		arch_leave_lazy_mmu_mode();
		pte_unmap_unlock(start_pte, ptl);
	}
	if (pageout)
		reclaim_pages(&folio_list, true);
	cond_resched();

	return 0;
}

static const struct mm_walk_ops cold_walk_ops = {
	.pmd_entry = madvise_cold_or_pageout_pte_range,
	.walk_lock = PGWALK_RDLOCK,
};

static void madvise_cold_page_range(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end)
{
	struct madvise_walk_private walk_private = {
		.pageout = false,
		.tlb = tlb,
	};

	tlb_start_vma(tlb, vma);
	walk_page_range(vma->vm_mm, addr, end, &cold_walk_ops, &walk_private);
	tlb_end_vma(tlb, vma);
}

static inline bool can_madv_lru_vma(struct vm_area_struct *vma)
{
	return !(vma->vm_flags & (VM_LOCKED|VM_PFNMAP|VM_HUGETLB));
}

static long madvise_cold(struct vm_area_struct *vma,
			struct vm_area_struct **prev,
			unsigned long start_addr, unsigned long end_addr)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_gather tlb;

	*prev = vma;
	if (!can_madv_lru_vma(vma))
		return -EINVAL;

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm);
	madvise_cold_page_range(&tlb, vma, start_addr, end_addr);
	tlb_finish_mmu(&tlb);

	return 0;
}

static int madvise_pageout_page_range(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end)
{
	struct madvise_walk_private walk_private = {
		.pageout = true,
		.tlb = tlb,
	};
	int ret;

	tlb_start_vma(tlb, vma);
	ret = walk_page_range(vma->vm_mm, addr, end, &cold_walk_ops, &walk_private);
	tlb_end_vma(tlb, vma);

	return ret;
}

static long madvise_pageout(struct vm_area_struct *vma,
			struct vm_area_struct **prev,
			unsigned long start_addr, unsigned long end_addr)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_gather tlb;
	int ret;
	bool return_error = false;

	*prev = vma;
	if (!can_madv_lru_vma(vma))
		return -EINVAL;

	/*
	 * If the VMA belongs to a private file mapping, there can be private
	 * dirty pages which can be paged out if even this process is neither
	 * owner nor write capable of the file. We allow private file mappings
	 * further to pageout dirty anon pages.
	 */
	if (!vma_is_anonymous(vma) && (!can_do_file_pageout(vma) &&
				(vma->vm_flags & VM_MAYSHARE)))
		return 0;

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm);
	ret = madvise_pageout_page_range(&tlb, vma, start_addr, end_addr);
	tlb_finish_mmu(&tlb);

	trace_android_vh_madvise_pageout_return_error(ret, &return_error);
	if (return_error)
		return (long)ret;

	return 0;
}

static int madvise_free_pte_range(pmd_t *pmd, unsigned long addr,
				unsigned long end, struct mm_walk *walk)

{
	const cydp_t cydp_flags = CYDP_CLEAR_YOUNG | CYDP_CLEAR_DIRTY;
	struct mmu_gather *tlb = walk->private;
	struct mm_struct *mm = tlb->mm;
	struct vm_area_struct *vma = walk->vma;
	spinlock_t *ptl;
	pte_t *start_pte, *pte, ptent;
	struct folio *folio;
	int nr_swap = 0;
	unsigned long next;
	int nr, max_nr;

	next = pmd_addr_end(addr, end);
	if (pmd_trans_huge(*pmd))
		if (madvise_free_huge_pmd(tlb, vma, pmd, addr, next))
			return 0;

	tlb_change_page_size(tlb, PAGE_SIZE);
	start_pte = pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	if (!start_pte)
		return 0;
	flush_tlb_batched_pending(mm);
	arch_enter_lazy_mmu_mode();
	for (; addr != end; pte += nr, addr += PAGE_SIZE * nr) {
		nr = 1;
		ptent = ptep_get(pte);

		if (pte_none(ptent))
			continue;
		/*
		 * If the pte has swp_entry, just clear page table to
		 * prevent swap-in which is more expensive rather than
		 * (page allocation + zeroing).
		 */
		if (!pte_present(ptent)) {
			swp_entry_t entry;

			entry = pte_to_swp_entry(ptent);
			if (!non_swap_entry(entry)) {
				max_nr = (end - addr) / PAGE_SIZE;
				nr = swap_pte_batch(pte, max_nr, ptent);
				nr_swap -= nr;
				free_swap_and_cache_nr(entry, nr);
				clear_not_present_full_ptes(mm, addr, pte, nr, tlb->fullmm);
			} else if (is_hwpoison_entry(entry) ||
				   is_poisoned_swp_entry(entry)) {
				pte_clear_not_present_full(mm, addr, pte, tlb->fullmm);
			}
			continue;
		}

		folio = vm_normal_folio(vma, addr, ptent);
		if (!folio || folio_is_zone_device(folio))
			continue;

		/*
		 * If we encounter a large folio, only split it if it is not
		 * fully mapped within the range we are operating on. Otherwise
		 * leave it as is so that it can be marked as lazyfree. If we
		 * fail to split a folio, leave it in place and advance to the
		 * next pte in the range.
		 */
		if (folio_test_large(folio)) {
			bool any_young, any_dirty;

			nr = madvise_folio_pte_batch(addr, end, folio, pte,
						     ptent, &any_young, &any_dirty);

			if (nr < folio_nr_pages(folio)) {
				int err;

				if (folio_likely_mapped_shared(folio))
					continue;
				if (!folio_trylock(folio))
					continue;
				folio_get(folio);
				arch_leave_lazy_mmu_mode();
				pte_unmap_unlock(start_pte, ptl);
				start_pte = NULL;
				err = split_folio(folio);
				folio_unlock(folio);
				folio_put(folio);
				pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
				start_pte = pte;
				if (!start_pte)
					break;
				arch_enter_lazy_mmu_mode();
				if (err >= 0)
					nr = 0;
				continue;
			}

			if (any_young)
				ptent = pte_mkyoung(ptent);
			if (any_dirty)
				ptent = pte_mkdirty(ptent);
		}

		if (folio_test_swapcache(folio) || folio_test_dirty(folio)) {
			if (!folio_trylock(folio))
				continue;
			/*
			 * If we have a large folio at this point, we know it is
			 * fully mapped so if its mapcount is the same as its
			 * number of pages, it must be exclusive.
			 */
			if (folio_mapcount(folio) != folio_nr_pages(folio)) {
				folio_unlock(folio);
				continue;
			}

			if (folio_test_swapcache(folio) &&
			    !folio_free_swap(folio)) {
				folio_unlock(folio);
				continue;
			}

			folio_clear_dirty(folio);
			folio_unlock(folio);
		}

		if (pte_young(ptent) || pte_dirty(ptent)) {
			clear_young_dirty_ptes(vma, addr, pte, nr, cydp_flags);
			tlb_remove_tlb_entries(tlb, pte, nr, addr);
		}
		folio_mark_lazyfree(folio);
	}

	if (nr_swap) {
		if (current->mm == mm)
			sync_mm_rss(mm);
		add_mm_counter(mm, MM_SWAPENTS, nr_swap);
	}
	if (start_pte) {
		arch_leave_lazy_mmu_mode();
		pte_unmap_unlock(start_pte, ptl);
	}
	cond_resched();

	return 0;
}

static const struct mm_walk_ops madvise_free_walk_ops = {
	.pmd_entry		= madvise_free_pte_range,
	.walk_lock		= PGWALK_RDLOCK,
};

static int madvise_free_single_vma(struct vm_area_struct *vma,
			unsigned long start_addr, unsigned long end_addr)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_notifier_range range;
	struct mmu_gather tlb;

	/* MADV_FREE works for only anon vma at the moment */
	if (!vma_is_anonymous(vma))
		return -EINVAL;

	range.start = max(vma->vm_start, start_addr);
	if (range.start >= vma->vm_end)
		return -EINVAL;
	range.end = min(vma->vm_end, end_addr);
	if (range.end <= vma->vm_start)
		return -EINVAL;
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
				range.start, range.end);

	lru_add_drain();
	tlb_gather_mmu(&tlb, mm);
	update_hiwater_rss(mm);

	mmu_notifier_invalidate_range_start(&range);
	tlb_start_vma(&tlb, vma);
	walk_page_range(vma->vm_mm, range.start, range.end,
			&madvise_free_walk_ops, &tlb);
	tlb_end_vma(&tlb, vma);
	mmu_notifier_invalidate_range_end(&range);
	tlb_finish_mmu(&tlb);

	return 0;
}

/*
 * Application no longer needs these pages.  If the pages are dirty,
 * it's OK to just throw them away.  The app will be more careful about
 * data it wants to keep.  Be sure to free swap resources too.  The
 * zap_page_range_single call sets things up for shrink_active_list to actually
 * free these pages later if no one else has touched them in the meantime,
 * although we could add these pages to a global reuse list for
 * shrink_active_list to pick up before reclaiming other pages.
 *
 * NB: This interface discards data rather than pushes it out to swap,
 * as some implementations do.  This has performance implications for
 * applications like large transactional databases which want to discard
 * pages in anonymous maps after committing to backing store the data
 * that was kept in them.  There is no reason to write this data out to
 * the swap area if the application is discarding it.
 *
 * An interface that causes the system to free clean pages and flush
 * dirty pages is already available as msync(MS_INVALIDATE).
 */
static long madvise_dontneed_single_vma(struct vm_area_struct *vma,
					unsigned long start, unsigned long end)
{
	madvise_vma_pad_pages(vma, start, end);

	zap_page_range_single(vma, start, end - start, NULL);
	return 0;
}

static bool madvise_dontneed_free_valid_vma(struct vm_area_struct *vma,
					    unsigned long start,
					    unsigned long *end,
					    int behavior)
{
	if (!is_vm_hugetlb_page(vma)) {
		unsigned int forbidden = VM_PFNMAP;

		if (behavior != MADV_DONTNEED_LOCKED)
			forbidden |= VM_LOCKED;

		return !(vma->vm_flags & forbidden);
	}

	if (behavior != MADV_DONTNEED && behavior != MADV_DONTNEED_LOCKED)
		return false;
	if (start & ~huge_page_mask(hstate_vma(vma)))
		return false;

	/*
	 * Madvise callers expect the length to be rounded up to PAGE_SIZE
	 * boundaries, and may be unaware that this VMA uses huge pages.
	 * Avoid unexpected data loss by rounding down the number of
	 * huge pages freed.
	 */
	*end = ALIGN_DOWN(*end, huge_page_size(hstate_vma(vma)));

	return true;
}

static long madvise_dontneed_free(struct vm_area_struct *vma,
				  struct vm_area_struct **prev,
				  unsigned long start, unsigned long end,
				  int behavior)
{
	struct mm_struct *mm = vma->vm_mm;

	*prev = vma;
	if (!madvise_dontneed_free_valid_vma(vma, start, &end, behavior))
		return -EINVAL;

	if (start == end)
		return 0;

	if (!userfaultfd_remove(vma, start, end)) {
		*prev = NULL; /* mmap_lock has been dropped, prev is stale */

		mmap_read_lock(mm);
		vma = vma_lookup(mm, start);
		if (!vma)
			return -ENOMEM;
		/*
		 * Potential end adjustment for hugetlb vma is OK as
		 * the check below keeps end within vma.
		 */
		if (!madvise_dontneed_free_valid_vma(vma, start, &end,
						     behavior))
			return -EINVAL;
		if (end > vma->vm_end) {
			/*
			 * Don't fail if end > vma->vm_end. If the old
			 * vma was split while the mmap_lock was
			 * released the effect of the concurrent
			 * operation may not cause madvise() to
			 * have an undefined result. There may be an
			 * adjacent next vma that we'll walk
			 * next. userfaultfd_remove() will generate an
			 * UFFD_EVENT_REMOVE repetition on the
			 * end-vma->vm_end range, but the manager can
			 * handle a repetition fine.
			 */
			end = vma->vm_end;
		}
		VM_WARN_ON(start >= end);
	}

	if (behavior == MADV_DONTNEED || behavior == MADV_DONTNEED_LOCKED)
		return madvise_dontneed_single_vma(vma, start, end);
	else if (behavior == MADV_FREE)
		return madvise_free_single_vma(vma, start, end);
	else
		return -EINVAL;
}

static long madvise_populate(struct vm_area_struct *vma,
			     struct vm_area_struct **prev,
			     unsigned long start, unsigned long end,
			     int behavior)
{
	const bool write = behavior == MADV_POPULATE_WRITE;
	struct mm_struct *mm = vma->vm_mm;
	int locked = 1;
	long pages;

	*prev = vma;

	while (start < end) {
		/* Populate (prefault) page tables readable/writable. */
		pages = faultin_page_range(mm, start, end, write, &locked);
		if (!locked) {
			mmap_read_lock(mm);
			locked = 1;
			*prev = NULL;
			vma = NULL;
		}
		if (pages < 0) {
			switch (pages) {
			case -EINTR:
				return -EINTR;
			case -EINVAL: /* Incompatible mappings / permissions. */
				return -EINVAL;
			case -EHWPOISON:
				return -EHWPOISON;
			case -EFAULT: /* VM_FAULT_SIGBUS or VM_FAULT_SIGSEGV */
				return -EFAULT;
			default:
				pr_warn_once("%s: unhandled return value: %ld\n",
					     __func__, pages);
				fallthrough;
			case -ENOMEM: /* No VMA or out of memory. */
				return -ENOMEM;
			}
		}
		start += pages * PAGE_SIZE;
	}
	return 0;
}

/*
 * Application wants to free up the pages and associated backing store.
 * This is effectively punching a hole into the middle of a file.
 */
static long madvise_remove(struct vm_area_struct *vma,
				struct vm_area_struct **prev,
				unsigned long start, unsigned long end)
{
	loff_t offset;
	int error;
	struct file *f;
	struct mm_struct *mm = vma->vm_mm;

	*prev = NULL;	/* tell sys_madvise we drop mmap_lock */

	if (vma->vm_flags & VM_LOCKED)
		return -EINVAL;

	f = vma->vm_file;

	if (!f || !f->f_mapping || !f->f_mapping->host) {
			return -EINVAL;
	}

	if ((vma->vm_flags & (VM_SHARED|VM_WRITE)) != (VM_SHARED|VM_WRITE))
		return -EACCES;

	offset = (loff_t)(start - vma->vm_start)
			+ ((loff_t)vma->vm_pgoff << PAGE_SHIFT);

	/*
	 * Filesystem's fallocate may need to take i_rwsem.  We need to
	 * explicitly grab a reference because the vma (and hence the
	 * vma's reference to the file) can go away as soon as we drop
	 * mmap_lock.
	 */
	get_file(f);
	if (userfaultfd_remove(vma, start, end)) {
		/* mmap_lock was not released by userfaultfd_remove() */
		mmap_read_unlock(mm);
	}
	error = vfs_fallocate(f,
				FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				offset, end - start);
	fput(f);
	mmap_read_lock(mm);
	return error;
}

/*
 * Apply an madvise behavior to a region of a vma.  madvise_update_vma
 * will handle splitting a vm area into separate areas, each area with its own
 * behavior.
 */
static int madvise_vma_behavior(struct vm_area_struct *vma,
				struct vm_area_struct **prev,
				unsigned long start, unsigned long end,
				unsigned long behavior)
{
	int error;
	struct anon_vma_name *anon_name;
	unsigned long new_flags = vma->vm_flags;

	switch (behavior) {
	case MADV_REMOVE:
		return madvise_remove(vma, prev, start, end);
	case MADV_WILLNEED:
		return madvise_willneed(vma, prev, start, end);
	case MADV_COLD:
		return madvise_cold(vma, prev, start, end);
	case MADV_PAGEOUT:
		return madvise_pageout(vma, prev, start, end);
	case MADV_FREE:
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
		return madvise_dontneed_free(vma, prev, start, end, behavior);
	case MADV_POPULATE_READ:
	case MADV_POPULATE_WRITE:
		return madvise_populate(vma, prev, start, end, behavior);
	case MADV_NORMAL:
		new_flags = new_flags & ~VM_RAND_READ & ~VM_SEQ_READ;
		break;
	case MADV_SEQUENTIAL:
		new_flags = (new_flags & ~VM_RAND_READ) | VM_SEQ_READ;
		break;
	case MADV_RANDOM:
		new_flags = (new_flags & ~VM_SEQ_READ) | VM_RAND_READ;
		break;
	case MADV_DONTFORK:
		new_flags |= VM_DONTCOPY;
		break;
	case MADV_DOFORK:
		if (vma->vm_flags & VM_IO)
			return -EINVAL;
		new_flags &= ~VM_DONTCOPY;
		break;
	case MADV_WIPEONFORK:
		/* MADV_WIPEONFORK is only supported on anonymous memory. */
		if (vma->vm_file || vma->vm_flags & VM_SHARED)
			return -EINVAL;
		new_flags |= VM_WIPEONFORK;
		break;
	case MADV_KEEPONFORK:
		new_flags &= ~VM_WIPEONFORK;
		break;
	case MADV_DONTDUMP:
		new_flags |= VM_DONTDUMP;
		break;
	case MADV_DODUMP:
		if (!is_vm_hugetlb_page(vma) && new_flags & VM_SPECIAL)
			return -EINVAL;
		new_flags &= ~VM_DONTDUMP;
		break;
	case MADV_MERGEABLE:
	case MADV_UNMERGEABLE:
		error = ksm_madvise(vma, start, end, behavior, &new_flags);
		if (error)
			goto out;
		break;
	case MADV_HUGEPAGE:
	case MADV_NOHUGEPAGE:
		error = hugepage_madvise(vma, &new_flags, behavior);
		if (error)
			goto out;
		break;
	case MADV_COLLAPSE:
		return madvise_collapse(vma, prev, start, end);
	}

	anon_name = anon_vma_name(vma);
	anon_vma_name_get(anon_name);
	error = madvise_update_vma(vma, prev, start, end, new_flags,
				   anon_name);
	anon_vma_name_put(anon_name);

out:
	/*
	 * madvise() returns EAGAIN if kernel resources, such as
	 * slab, are temporarily unavailable.
	 */
	if (error == -ENOMEM)
		error = -EAGAIN;
	return error;
}

#ifdef CONFIG_MEMORY_FAILURE
/*
 * Error injection support for memory error handling.
 */
static int madvise_inject_error(int behavior,
		unsigned long start, unsigned long end)
{
	unsigned long size;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;


	for (; start < end; start += size) {
		unsigned long pfn;
		struct page *page;
		int ret;

		ret = get_user_pages_fast(start, 1, 0, &page);
		if (ret != 1)
			return ret;
		pfn = page_to_pfn(page);

		/*
		 * When soft offlining hugepages, after migrating the page
		 * we dissolve it, therefore in the second loop "page" will
		 * no longer be a compound page.
		 */
		size = page_size(compound_head(page));

		if (behavior == MADV_SOFT_OFFLINE) {
			pr_info("Soft offlining pfn %#lx at process virtual address %#lx\n",
				 pfn, start);
			ret = soft_offline_page(pfn, MF_COUNT_INCREASED);
		} else {
			pr_info("Injecting memory failure for pfn %#lx at process virtual address %#lx\n",
				 pfn, start);
			ret = memory_failure(pfn, MF_COUNT_INCREASED | MF_SW_SIMULATED);
			if (ret == -EOPNOTSUPP)
				ret = 0;
		}

		if (ret)
			return ret;
	}

	return 0;
}
#endif

static bool
madvise_behavior_valid(int behavior)
{
	switch (behavior) {
	case MADV_DOFORK:
	case MADV_DONTFORK:
	case MADV_NORMAL:
	case MADV_SEQUENTIAL:
	case MADV_RANDOM:
	case MADV_REMOVE:
	case MADV_WILLNEED:
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
	case MADV_FREE:
	case MADV_COLD:
	case MADV_PAGEOUT:
	case MADV_POPULATE_READ:
	case MADV_POPULATE_WRITE:
#ifdef CONFIG_KSM
	case MADV_MERGEABLE:
	case MADV_UNMERGEABLE:
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	case MADV_HUGEPAGE:
	case MADV_NOHUGEPAGE:
	case MADV_COLLAPSE:
#endif
	case MADV_DONTDUMP:
	case MADV_DODUMP:
	case MADV_WIPEONFORK:
	case MADV_KEEPONFORK:
#ifdef CONFIG_MEMORY_FAILURE
	case MADV_SOFT_OFFLINE:
	case MADV_HWPOISON:
#endif
		return true;

	default:
		return false;
	}
}

static bool process_madvise_behavior_valid(int behavior)
{
	switch (behavior) {
	case MADV_COLD:
	case MADV_PAGEOUT:
	case MADV_WILLNEED:
	case MADV_COLLAPSE:
		return true;
	default:
		return false;
	}
}

/*
 * Walk the vmas in range [start,end), and call the visit function on each one.
 * The visit function will get start and end parameters that cover the overlap
 * between the current vma and the original range.  Any unmapped regions in the
 * original range will result in this function returning -ENOMEM while still
 * calling the visit function on all of the existing vmas in the range.
 * Must be called with the mmap_lock held for reading or writing.
 */
static
int madvise_walk_vmas(struct mm_struct *mm, unsigned long start,
		      unsigned long end, unsigned long arg,
		      int (*visit)(struct vm_area_struct *vma,
				   struct vm_area_struct **prev, unsigned long start,
				   unsigned long end, unsigned long arg))
{
	struct vm_area_struct *vma;
	struct vm_area_struct *prev;
	unsigned long tmp;
	int unmapped_error = 0;

	/*
	 * If the interval [start,end) covers some unmapped address
	 * ranges, just ignore them, but return -ENOMEM at the end.
	 * - different from the way of handling in mlock etc.
	 */
	vma = find_vma_prev(mm, start, &prev);
	if (vma && start > vma->vm_start)
		prev = vma;

	for (;;) {
		int error;

		/* Still start < end. */
		if (!vma)
			return -ENOMEM;

		/* Here start < (end|vma->vm_end). */
		if (start < vma->vm_start) {
			unmapped_error = -ENOMEM;
			start = vma->vm_start;
			if (start >= end)
				break;
		}

		/* Here vma->vm_start <= start < (end|vma->vm_end) */
		tmp = vma->vm_end;
		if (end < tmp)
			tmp = end;

		/* Here vma->vm_start <= start < tmp <= (end|vma->vm_end). */
		error = visit(vma, &prev, start, tmp, arg);
		if (error)
			return error;
		start = tmp;
		if (prev && start < prev->vm_end)
			start = prev->vm_end;
		if (start >= end)
			break;
		if (prev)
			vma = find_vma(mm, prev->vm_end);
		else	/* madvise_remove dropped mmap_lock */
			vma = find_vma(mm, start);
	}

	return unmapped_error;
}

#ifdef CONFIG_ANON_VMA_NAME
static int madvise_vma_anon_name(struct vm_area_struct *vma,
				 struct vm_area_struct **prev,
				 unsigned long start, unsigned long end,
				 unsigned long anon_name)
{
	int error;

	/* Only anonymous mappings can be named */
	if (vma->vm_file && !vma_is_anon_shmem(vma))
		return -EBADF;

	trace_android_vh_update_vma_flags(vma);
	error = madvise_update_vma(vma, prev, start, end, vma->vm_flags,
				   (struct anon_vma_name *)anon_name);

	/*
	 * madvise() returns EAGAIN if kernel resources, such as
	 * slab, are temporarily unavailable.
	 */
	if (error == -ENOMEM)
		error = -EAGAIN;
	return error;
}

int madvise_set_anon_name(struct mm_struct *mm, unsigned long start,
			  unsigned long len_in, struct anon_vma_name *anon_name)
{
	unsigned long end;
	unsigned long len;

	if (start & ~__PAGE_MASK)
		return -EINVAL;
	len = (len_in + ~__PAGE_MASK) & __PAGE_MASK;

	/* Check to see whether len was rounded up from small -ve to zero */
	if (len_in && !len)
		return -EINVAL;

	end = start + len;
	if (end < start)
		return -EINVAL;

	if (end == start)
		return 0;

	return madvise_walk_vmas(mm, start, end, (unsigned long)anon_name,
				 madvise_vma_anon_name);
}
#endif /* CONFIG_ANON_VMA_NAME */
/*
 * The madvise(2) system call.
 *
 * Applications can use madvise() to advise the kernel how it should
 * handle paging I/O in this VM area.  The idea is to help the kernel
 * use appropriate read-ahead and caching techniques.  The information
 * provided is advisory only, and can be safely disregarded by the
 * kernel without affecting the correct operation of the application.
 *
 * behavior values:
 *  MADV_NORMAL - the default behavior is to read clusters.  This
 *		results in some read-ahead and read-behind.
 *  MADV_RANDOM - the system should read the minimum amount of data
 *		on any access, since it is unlikely that the appli-
 *		cation will need more than what it asks for.
 *  MADV_SEQUENTIAL - pages in the given range will probably be accessed
 *		once, so they can be aggressively read ahead, and
 *		can be freed soon after they are accessed.
 *  MADV_WILLNEED - the application is notifying the system to read
 *		some pages ahead.
 *  MADV_DONTNEED - the application is finished with the given range,
 *		so the kernel can free resources associated with it.
 *  MADV_FREE - the application marks pages in the given range as lazy free,
 *		where actual purges are postponed until memory pressure happens.
 *  MADV_REMOVE - the application wants to free up the given range of
 *		pages and associated backing store.
 *  MADV_DONTFORK - omit this area from child's address space when forking:
 *		typically, to avoid COWing pages pinned by get_user_pages().
 *  MADV_DOFORK - cancel MADV_DONTFORK: no longer omit this area when forking.
 *  MADV_WIPEONFORK - present the child process with zero-filled memory in this
 *              range after a fork.
 *  MADV_KEEPONFORK - undo the effect of MADV_WIPEONFORK
 *  MADV_HWPOISON - trigger memory error handler as if the given memory range
 *		were corrupted by unrecoverable hardware memory failure.
 *  MADV_SOFT_OFFLINE - try to soft-offline the given range of memory.
 *  MADV_MERGEABLE - the application recommends that KSM try to merge pages in
 *		this area with pages of identical content from other such areas.
 *  MADV_UNMERGEABLE- cancel MADV_MERGEABLE: no longer merge pages with others.
 *  MADV_HUGEPAGE - the application wants to back the given range by transparent
 *		huge pages in the future. Existing pages might be coalesced and
 *		new pages might be allocated as THP.
 *  MADV_NOHUGEPAGE - mark the given range as not worth being backed by
 *		transparent huge pages so the existing pages will not be
 *		coalesced into THP and new pages will not be allocated as THP.
 *  MADV_COLLAPSE - synchronously coalesce pages into new THP.
 *  MADV_DONTDUMP - the application wants to prevent pages in the given range
 *		from being included in its core dump.
 *  MADV_DODUMP - cancel MADV_DONTDUMP: no longer exclude from core dump.
 *  MADV_COLD - the application is not expected to use this memory soon,
 *		deactivate pages in this range so that they can be reclaimed
 *		easily if memory pressure happens.
 *  MADV_PAGEOUT - the application is not expected to use this memory soon,
 *		page out the pages in this range immediately.
 *  MADV_POPULATE_READ - populate (prefault) page tables readable by
 *		triggering read faults if required
 *  MADV_POPULATE_WRITE - populate (prefault) page tables writable by
 *		triggering write faults if required
 *
 * return values:
 *  zero    - success
 *  -EINVAL - start + len < 0, start is not page-aligned,
 *		"behavior" is not a valid value, or application
 *		is attempting to release locked or shared pages,
 *		or the specified address range includes file, Huge TLB,
 *		MAP_SHARED or VMPFNMAP range.
 *  -ENOMEM - addresses in the specified range are not currently
 *		mapped, or are outside the AS of the process.
 *  -EIO    - an I/O error occurred while paging in data.
 *  -EBADF  - map exists, but area maps something that isn't a file.
 *  -EAGAIN - a kernel resource was temporarily unavailable.
 *  -EPERM  - memory is sealed.
 */
int do_madvise(struct mm_struct *mm, unsigned long start, size_t len_in, int behavior)
{
	unsigned long end;
	int error;
	int write;
	size_t len;
	struct blk_plug plug;

	if (!madvise_behavior_valid(behavior))
		return -EINVAL;

	if (!__PAGE_ALIGNED(start))
		return -EINVAL;
	len = __PAGE_ALIGN(len_in);

	/* Check to see whether len was rounded up from small -ve to zero */
	if (len_in && !len)
		return -EINVAL;

	end = start + len;
	if (end < start)
		return -EINVAL;

	if (end == start)
		return 0;

#ifdef CONFIG_MEMORY_FAILURE
	if (behavior == MADV_HWPOISON || behavior == MADV_SOFT_OFFLINE)
		return madvise_inject_error(behavior, start, start + len_in);
#endif

	write = madvise_need_mmap_write(behavior);
	if (write) {
		if (mmap_write_lock_killable(mm))
			return -EINTR;
	} else {
		mmap_read_lock(mm);
	}

	start = untagged_addr_remote(mm, start);
	end = start + len;

	/*
	 * Check if the address range is sealed for do_madvise().
	 * can_modify_mm_madv assumes we have acquired the lock on MM.
	 */
	if (unlikely(!can_modify_mm_madv(mm, start, end, behavior))) {
		error = -EPERM;
		goto out;
	}

	blk_start_plug(&plug);
	error = madvise_walk_vmas(mm, start, end, behavior,
			madvise_vma_behavior);
	blk_finish_plug(&plug);

out:
	if (write)
		mmap_write_unlock(mm);
	else
		mmap_read_unlock(mm);

	return error;
}

SYSCALL_DEFINE3(madvise, unsigned long, start, size_t, len_in, int, behavior)
{
	return do_madvise(current->mm, start, len_in, behavior);
}

SYSCALL_DEFINE5(process_madvise, int, pidfd, const struct iovec __user *, vec,
		size_t, vlen, int, behavior, unsigned int, flags)
{
	ssize_t ret;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov = iovstack;
	struct iov_iter iter;
	struct task_struct *task;
	struct mm_struct *mm;
	size_t total_len;
	unsigned int f_flags;
	bool bypass = false;
	bool return_error = false;

	trace_android_rvh_process_madvise_bypass(pidfd, vec,
			vlen, behavior, flags, &ret, &bypass);
	if (bypass)
		return ret;

	if (flags != 0) {
		ret = -EINVAL;
		goto out;
	}

	ret = import_iovec(ITER_DEST, vec, vlen, ARRAY_SIZE(iovstack), &iov, &iter);
	if (ret < 0)
		goto out;

	task = pidfd_get_task(pidfd, &f_flags);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto free_iov;
	}

	if (!process_madvise_behavior_valid(behavior)) {
		ret = -EINVAL;
		goto release_task;
	}

	/* Require PTRACE_MODE_READ to avoid leaking ASLR metadata. */
	mm = mm_access(task, PTRACE_MODE_READ_FSCREDS);
	if (IS_ERR_OR_NULL(mm)) {
		ret = IS_ERR(mm) ? PTR_ERR(mm) : -ESRCH;
		goto release_task;
	}

	/*
	 * Require CAP_SYS_NICE for influencing process performance. Note that
	 * only non-destructive hints are currently supported.
	 */
	if (!capable(CAP_SYS_NICE)) {
		ret = -EPERM;
		goto release_mm;
	}

	total_len = iov_iter_count(&iter);
	trace_android_vh_process_madvise_begin(task, behavior);

	while (iov_iter_count(&iter)) {
		trace_android_vh_process_madvise_iter(task, behavior, &ret);
		if (ret < 0)
			break;
		ret = do_madvise(mm, (unsigned long)iter_iov_addr(&iter),
					iter_iov_len(&iter), behavior);
		if (ret < 0)
			break;
		iov_iter_advance(&iter, iter_iov_len(&iter));
	}
	trace_android_vh_process_madvise_return_error(behavior, ret, &return_error);
	if (return_error)
		goto release_mm;

	ret = (total_len - iov_iter_count(&iter)) ? : ret;

release_mm:
	mmput(mm);
release_task:
	put_task_struct(task);
free_iov:
	kfree(iov);
out:
	trace_android_vh_process_madvise(behavior, &ret, NULL);
	return ret;
}
