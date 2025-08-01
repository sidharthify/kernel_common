// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2010-2024 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

/**
 * DOC: Base kernel MMU management.
 */

#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/migrate.h>
#include <mali_kbase.h>
#include <mali_kbase_io.h>
#include <gpu/mali_kbase_gpu_fault.h>
#include <hw_access/mali_kbase_hw_access_regmap.h>
#include <tl/mali_kbase_tracepoints.h>
#include <backend/gpu/mali_kbase_instr_defs.h>
#include <mali_kbase_ctx_sched.h>
#include <mali_kbase_debug.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_hw.h>
#include <mmu/mali_kbase_mmu_hw.h>
#include <mali_kbase_mem.h>
#include <mali_kbase_reset_gpu.h>
#include <mmu/mali_kbase_mmu.h>
#include <mmu/mali_kbase_mmu_internal.h>
#include <device/mali_kbase_device.h>
#include <uapi/gpu/arm/midgard/gpu/mali_kbase_gpu_id.h>
#if !MALI_USE_CSF
#include <mali_kbase_hwaccess_jm.h>
#endif
#include <linux/version_compat_defs.h>

#include <mali_kbase_trace_gpu_mem.h>
#include <backend/gpu/mali_kbase_pm_internal.h>

/* Threshold used to decide whether to flush full caches or just a physical range */
#define KBASE_PA_RANGE_THRESHOLD_NR_PAGES 20
#define MGM_DEFAULT_PTE_GROUP (0)

/* Macro to convert updated PDGs to flags indicating levels skip in flush */
#define pgd_level_to_skip_flush(dirty_pgds) (~(dirty_pgds)&0xF)

/**
 * kmap_pgd() - Map a PGD page and return the address of it
 *
 * @p:           Pointer to the PGD page to be mapped.
 * @pgd:         The physical address of the PGD. May not be PAGE_SIZE aligned but shall be
 *               GPU_PAGE_SIZE aligned.
 *
 * Return: The mapped address of the @pgd, adjusted by the offset of @pgd from the start of page.
 */
static inline void *kmap_pgd(struct page *p, phys_addr_t pgd)
{
#if GPU_PAGES_PER_CPU_PAGE > 1
	return kbase_kmap(p) + (pgd & ~PAGE_MASK);
#else
	CSTD_UNUSED(pgd);
	return kbase_kmap(p);
#endif
}

/**
 * kmap_atomic_pgd() - Variant of kmap_pgd for atomic mapping
 *
 * @p:           Pointer to the PGD page to be mapped.
 * @pgd:         The physical address of the PGD. May not be PAGE_SIZE aligned but shall be
 *               GPU_PAGE_SIZE aligned.
 *
 * Return: The mapped address of the @pgd.
 */
static inline void *kmap_atomic_pgd(struct page *p, phys_addr_t pgd)
{
#if GPU_PAGES_PER_CPU_PAGE > 1
	return kbase_kmap_atomic(p) + (pgd & ~PAGE_MASK);
#else
	CSTD_UNUSED(pgd);
	return kbase_kmap_atomic(p);
#endif
}

/**
 * kunmap_pgd() - Unmap a PGD page
 *
 * @p:           Pointer to the PGD page to be unmapped.
 * @pgd_address: The address of the PGD. May not be PAGE_SIZE aligned but shall be
 *               GPU_PAGE_SIZE aligned.
 */
static inline void kunmap_pgd(struct page *p, void *pgd_address)
{
	/* It is okay to not align pgd_address to PAGE_SIZE boundary */
	kbase_kunmap(p, pgd_address);
}

/**
 * kunmap_atomic_pgd() - Variant of kunmap_pgd for atomic unmapping
 *
 * @pgd_address: The address of the PGD. May not be PAGE_SIZE aligned but shall be
 *               GPU_PAGE_SIZE aligned.
 */
static inline void kunmap_atomic_pgd(void *pgd_address)
{
	/* It is okay to not align pgd_address to PAGE_SIZE boundary */
	kbase_kunmap_atomic(pgd_address);
}

/**
 * pgd_dma_addr() - Return dma addr of a PGD
 *
 * @p:       Pointer to the PGD page.
 * @pgd:     The physical address of the PGD.
 *
 * Return:   DMA address of the PGD
 */
static inline dma_addr_t pgd_dma_addr(struct page *p, phys_addr_t pgd)
{
#if GPU_PAGES_PER_CPU_PAGE > 1
	return kbase_page_private(p)->dma_addr + (pgd & ~PAGE_MASK);
#else
	CSTD_UNUSED(pgd);
	return kbase_dma_addr(p);
#endif
}

#if GPU_PAGES_PER_CPU_PAGE > 1
/**
 * get_pgd_sub_page_index() - Return the index of a sub PGD page in the PGD page.
 *
 * @pgd:         The physical address of the PGD.
 *
 * Return:       The index value ranging from 0 to (GPU_PAGES_PER_CPU_PAGE - 1)
 */
static inline u32 get_pgd_sub_page_index(phys_addr_t pgd)
{
	return (pgd & ~PAGE_MASK) / GPU_PAGE_SIZE;
}

/**
 * alloc_pgd_page_metadata() - Allocate page metadata for a PGD.
 *
 * @kbdev:      Pointer to the instance of a kbase device.
 * @mmut:       Structure holding details of the MMU table for a kcontext.
 * @p:          PGD page.
 *
 * The PGD page, @p is linked to &kbase_mmu_table.pgd_pages_list for allocating
 * sub PGD pages from the list.
 *
 * Return:      True on success.
 */
static bool alloc_pgd_page_metadata(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				    struct page *p)
{
	struct kbase_page_metadata *page_md;

	if (!kbase_is_page_migration_enabled()) {
		page_md = kmem_cache_zalloc(kbdev->page_metadata_slab, GFP_KERNEL);
		if (!page_md)
			return false;

		page_md->dma_addr = kbase_dma_addr_as_priv(p);
		set_page_private(p, (unsigned long)page_md);
	} else {
		page_md = kbase_page_private(p);
	}

	page_md->data.pt_mapped.num_allocated_sub_pages = 1;
	set_bit(0, page_md->data.pt_mapped.allocated_sub_pages);
	page_md->data.pt_mapped.pgd_page = p;
	list_add(&page_md->data.pt_mapped.pgd_link, &mmut->pgd_pages_list);

	return true;
}

/**
 * free_pgd_page_metadata() - Free page metadata for a PGD.
 *
 * @kbdev:      Pointer to the instance of a kbase device.
 * @p:          PGD page where the metadata belongs to.
 *
 * The PGD page, @p is removed from &kbase_mmu_table.pgd_pages_list.
 */
static void free_pgd_page_metadata(struct kbase_device *kbdev, struct page *p)
{
	struct kbase_page_metadata *page_md = kbase_page_private(p);

	WARN_ON_ONCE(page_md->data.pt_mapped.num_allocated_sub_pages);
	page_md->data.pt_mapped.pgd_page = NULL;
	list_del_init(&page_md->data.pt_mapped.pgd_link);

	if (kbase_is_page_migration_enabled())
		return;

	set_page_private(p, (unsigned long)page_md->dma_addr);
	kmem_cache_free(kbdev->page_metadata_slab, page_md);
}

/**
 * allocate_pgd_sub_page() - Allocate a PGD sub page
 *
 * @page_md:  Page metadata of a PGD page where a sub page is allocated from.
 *
 * Return:    Physical address of allocated PGD sub page on success.
 *            KBASE_INVALID_PHYSICAL_ADDRESS on failure.
 */
static inline phys_addr_t allocate_pgd_sub_page(struct kbase_page_metadata *page_md)
{
	unsigned long sub_page_index;

	if (page_md->data.pt_mapped.num_allocated_sub_pages == GPU_PAGES_PER_CPU_PAGE)
		return KBASE_INVALID_PHYSICAL_ADDRESS;
	sub_page_index = find_first_zero_bit(page_md->data.pt_mapped.allocated_sub_pages,
					     GPU_PAGES_PER_CPU_PAGE);

#ifdef CONFIG_MALI_DEBUG
	if (WARN_ON_ONCE(sub_page_index >= GPU_PAGES_PER_CPU_PAGE))
		return KBASE_INVALID_PHYSICAL_ADDRESS;
	if (WARN_ON_ONCE(page_md->data.pt_mapped.num_allocated_sub_pages > GPU_PAGES_PER_CPU_PAGE))
		return KBASE_INVALID_PHYSICAL_ADDRESS;
#endif
	set_bit(sub_page_index, page_md->data.pt_mapped.allocated_sub_pages);
	page_md->data.pt_mapped.num_allocated_sub_pages++;

	return (page_to_phys(page_md->data.pt_mapped.pgd_page) + (sub_page_index * GPU_PAGE_SIZE));
}

/**
 * free_pgd_sub_page() - Free a PGD sub page
 *
 * @pgd:      Sub PGD to be freed.
 *
 * Return:    The number of remaining allocated sub pages in the PGD.
 */
static int free_pgd_sub_page(phys_addr_t pgd)
{
	struct page *p = pfn_to_page(PFN_DOWN(pgd));
	struct kbase_page_metadata *page_md = kbase_page_private(p);
	const u32 sub_page_index = get_pgd_sub_page_index(pgd);

#ifdef CONFIG_MALI_DEBUG
	if (WARN_ON_ONCE(!test_bit(sub_page_index, page_md->data.pt_mapped.allocated_sub_pages)))
		return page_md->data.pt_mapped.num_allocated_sub_pages;
#endif
	clear_bit(sub_page_index, page_md->data.pt_mapped.allocated_sub_pages);
	if (!WARN_ON_ONCE(page_md->data.pt_mapped.num_allocated_sub_pages <= 0))
		page_md->data.pt_mapped.num_allocated_sub_pages--;

	if (kbase_is_page_migration_enabled()) {
		spin_lock(&page_md->migrate_lock);
		page_md->data.pt_mapped.pgd_vpfn_level[sub_page_index] = 0;
		spin_unlock(&page_md->migrate_lock);
	}

	return page_md->data.pt_mapped.num_allocated_sub_pages;
}

/**
 * allocate_from_pgd_pages_list() - Allocate a PGD from the PGD pages list
 *
 * @mmut:     Structure holding details of the MMU table for a kcontext.
 *
 * Return:    Physical address of the allocated PGD.
 */
static inline phys_addr_t allocate_from_pgd_pages_list(struct kbase_mmu_table *mmut)
{
	struct list_head *entry;
	phys_addr_t pgd;

	lockdep_assert_held(&mmut->mmu_lock);

	if (unlikely(!mmut->num_free_pgd_sub_pages))
		return KBASE_INVALID_PHYSICAL_ADDRESS;

	if (mmut->last_allocated_pgd_page) {
		pgd = allocate_pgd_sub_page(kbase_page_private(mmut->last_allocated_pgd_page));
		if (pgd != KBASE_INVALID_PHYSICAL_ADDRESS)
			goto success;
	}

	if (mmut->last_freed_pgd_page) {
		pgd = allocate_pgd_sub_page(kbase_page_private(mmut->last_freed_pgd_page));
		if (pgd != KBASE_INVALID_PHYSICAL_ADDRESS)
			goto success;
	}

	list_for_each(entry, &mmut->pgd_pages_list) {
		struct kbase_page_metadata *page_md =
			list_entry(entry, struct kbase_page_metadata, data.pt_mapped.pgd_link);

		pgd = allocate_pgd_sub_page(page_md);
		if (pgd != KBASE_INVALID_PHYSICAL_ADDRESS)
			goto success;
	}

	return KBASE_INVALID_PHYSICAL_ADDRESS;
success:
	mmut->num_free_pgd_sub_pages--;
	return pgd;
}
#endif

static int mmu_insert_pages_no_flush(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				     const u64 start_vpfn, struct tagged_addr *phys, size_t nr,
				     unsigned long flags, int const group_id, u64 *dirty_pgds,
				     struct kbase_va_region *reg, bool ignore_page_migration);

/* Small wrapper function to factor out GPU-dependent context releasing */
static void release_ctx(struct kbase_device *kbdev, struct kbase_context *kctx)
{
#if MALI_USE_CSF
	CSTD_UNUSED(kbdev);
	kbase_ctx_sched_release_ctx_lock(kctx);
#else /* MALI_USE_CSF */
	kbasep_js_runpool_release_ctx(kbdev, kctx);
#endif /* MALI_USE_CSF */
}

/**
 * mmu_flush_cache_on_gpu_ctrl() - Check if cache flush needs to be done
 * through GPU_CONTROL interface.
 *
 * @kbdev:         kbase device to check GPU model ID on.
 *
 * This function returns whether a cache flush for page table update should
 * run through GPU_CONTROL interface or MMU_AS_CONTROL interface.
 *
 * Return: True if cache flush should be done on GPU command.
 */
static bool mmu_flush_cache_on_gpu_ctrl(struct kbase_device *kbdev)
{
	return kbdev->gpu_props.gpu_id.arch_major > 11;
}

/**
 * mmu_flush_pa_range() - Flush physical address range
 *
 * @kbdev:    kbase device to issue the MMU operation on.
 * @phys:     Starting address of the physical range to start the operation on.
 * @nr_bytes: Number of bytes to work on.
 * @op:       Type of cache flush operation to perform.
 *
 * Issue a cache flush physical range command.
 */
#if MALI_USE_CSF
static void mmu_flush_pa_range(struct kbase_device *kbdev, phys_addr_t phys, size_t nr_bytes,
			       enum kbase_mmu_op_type op)
{
	u32 flush_op;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	/* Translate operation to command */
	if (op == KBASE_MMU_OP_FLUSH_PT)
		flush_op = GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_L2;
	else if (op == KBASE_MMU_OP_FLUSH_MEM)
		flush_op = GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_L2_LSC;
	else {
		dev_warn(kbdev->dev, "Invalid flush request (op = %d)", op);
		return;
	}

	if (kbase_gpu_cache_flush_pa_range_and_busy_wait(kbdev, phys, nr_bytes, flush_op))
		dev_err(kbdev->dev, "Flush for physical address range did not complete");
}
#endif

/**
 * mmu_invalidate() - Perform an invalidate operation on MMU caches.
 * @kbdev:      The Kbase device.
 * @kctx:       The Kbase context.
 * @as_nr:      GPU address space number for which invalidate is required.
 * @op_param: Non-NULL pointer to struct containing information about the MMU
 *            operation to perform.
 *
 * Perform an MMU invalidate operation on a particual address space
 * by issuing a UNLOCK command.
 */
static void mmu_invalidate(struct kbase_device *kbdev, struct kbase_context *kctx, int as_nr,
			   const struct kbase_mmu_hw_op_param *op_param)
{
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	if (kbdev->pm.backend.gpu_ready && (!kctx || kctx->as_nr >= 0)) {
		as_nr = kctx ? kctx->as_nr : as_nr;
		if (kbase_mmu_hw_do_unlock(kbdev, &kbdev->as[as_nr], op_param))
			dev_err(kbdev->dev,
				"Invalidate after GPU page table update did not complete");
	}

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
}

/**
 * mmu_invalidate_on_teardown() - Perform an invalidate operation on MMU caches on page
 *                                table teardown.
 * @kbdev:      The Kbase device.
 * @kctx:       The Kbase context.
 * @vpfn:       The virtual page frame number at which teardown is done.
 * @num_pages:  The number of entries that were invalidated in top most level PGD, that
 *              was affected by the teardown operation.
 * @level:      The top most PGD level that was touched on teardown.
 * @as_nr:      GPU address space number for which invalidate is required.
 *
 * Perform an MMU invalidate operation after the teardown of top most level PGD on a
 * particular address space by issuing a UNLOCK command.
 */
static inline void mmu_invalidate_on_teardown(struct kbase_device *kbdev,
					      struct kbase_context *kctx, u64 vpfn,
					      size_t num_pages, int level, int as_nr)
{
	u32 invalidate_range_num_pages = num_pages;
	u64 invalidate_range_start_vpfn = vpfn;
	struct kbase_mmu_hw_op_param op_param;

	if (level != MIDGARD_MMU_BOTTOMLEVEL) {
		invalidate_range_num_pages = 1 << ((3 - level) * 9);
		invalidate_range_start_vpfn = vpfn - (vpfn & (invalidate_range_num_pages - 1));
	}

	op_param = (struct kbase_mmu_hw_op_param){
		.vpfn = invalidate_range_start_vpfn,
		.nr = invalidate_range_num_pages,
		.mmu_sync_info = CALLER_MMU_ASYNC,
		.kctx_id = kctx ? kctx->id : 0xFFFFFFFF,
		.flush_skip_levels = (1ULL << level) - 1,
	};

	mmu_invalidate(kbdev, kctx, as_nr, &op_param);
}

/* Perform a flush/invalidate on a particular address space
 */
static void mmu_flush_invalidate_as(struct kbase_device *kbdev, struct kbase_as *as,
				    const struct kbase_mmu_hw_op_param *op_param)
{
	unsigned long flags;

	/* AS transaction begin */
	mutex_lock(&kbdev->mmu_hw_mutex);
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	if (kbdev->pm.backend.gpu_ready && kbase_mmu_hw_do_flush(kbdev, as, op_param))
		dev_err(kbdev->dev, "Flush for GPU page table update did not complete");

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
	mutex_unlock(&kbdev->mmu_hw_mutex);
	/* AS transaction end */
}

/**
 * mmu_flush_invalidate() - Perform a flush operation on GPU caches.
 * @kbdev:      The Kbase device.
 * @kctx:       The Kbase context.
 * @as_nr:      GPU address space number for which flush + invalidate is required.
 * @op_param: Non-NULL pointer to struct containing information about the MMU
 *            operation to perform.
 *
 * This function performs the cache flush operation described by @op_param.
 * The function retains a reference to the given @kctx and releases it
 * after performing the flush operation.
 *
 * If operation is set to KBASE_MMU_OP_FLUSH_PT then this function will issue
 * a cache flush + invalidate to the L2 caches and invalidate the TLBs.
 *
 * If operation is set to KBASE_MMU_OP_FLUSH_MEM then this function will issue
 * a cache flush + invalidate to the L2 and GPU Load/Store caches as well as
 * invalidating the TLBs.
 */
static void mmu_flush_invalidate(struct kbase_device *kbdev, struct kbase_context *kctx, int as_nr,
				 const struct kbase_mmu_hw_op_param *op_param)
{
	bool ctx_is_in_runpool;

	/* Early out if there is nothing to do */
	if (op_param->nr == 0)
		return;

	/* If no context is provided then MMU operation is performed on address
	 * space which does not belong to user space context. Otherwise, retain
	 * refcount to context provided and release after flush operation.
	 */
	if (!kctx) {
		mmu_flush_invalidate_as(kbdev, &kbdev->as[as_nr], op_param);
	} else {
#if !MALI_USE_CSF
		rt_mutex_lock(&kbdev->js_data.queue_mutex);
		ctx_is_in_runpool = kbase_ctx_sched_inc_refcount(kctx);
		rt_mutex_unlock(&kbdev->js_data.queue_mutex);
#else
		ctx_is_in_runpool = kbase_ctx_sched_inc_refcount_if_as_valid(kctx);
#endif /* !MALI_USE_CSF */

		if (ctx_is_in_runpool) {
			KBASE_DEBUG_ASSERT(kctx->as_nr != KBASEP_AS_NR_INVALID);

			mmu_flush_invalidate_as(kbdev, &kbdev->as[kctx->as_nr], op_param);

			release_ctx(kbdev, kctx);
		}
	}
}

/**
 * mmu_flush_invalidate_on_gpu_ctrl() - Perform a flush operation on GPU caches via
 *                                    the GPU_CONTROL interface
 * @kbdev:      The Kbase device.
 * @kctx:       The Kbase context.
 * @as_nr:      GPU address space number for which flush + invalidate is required.
 * @op_param: Non-NULL pointer to struct containing information about the MMU
 *            operation to perform.
 *
 * Perform a flush/invalidate on a particular address space via the GPU_CONTROL
 * interface.
 */
static void mmu_flush_invalidate_on_gpu_ctrl(struct kbase_device *kbdev, struct kbase_context *kctx,
					     int as_nr,
					     const struct kbase_mmu_hw_op_param *op_param)
{
	unsigned long flags;

	/* AS transaction begin */
	mutex_lock(&kbdev->mmu_hw_mutex);
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	if (kbdev->pm.backend.gpu_ready && (!kctx || kctx->as_nr >= 0)) {
		as_nr = kctx ? kctx->as_nr : as_nr;
		if (kbase_mmu_hw_do_flush_on_gpu_ctrl(kbdev, &kbdev->as[as_nr], op_param))
			dev_err(kbdev->dev, "Flush for GPU page table update did not complete");
	}

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
	mutex_unlock(&kbdev->mmu_hw_mutex);
}

static void kbase_mmu_sync_pgd_gpu(struct kbase_device *kbdev, struct kbase_context *kctx,
				   phys_addr_t phys, size_t size, enum kbase_mmu_op_type flush_op)
{
	kbase_mmu_flush_pa_range(kbdev, kctx, phys, size, flush_op);
}

static void kbase_mmu_sync_pgd_cpu(struct kbase_device *kbdev, dma_addr_t handle, size_t size)
{
	/* Ensure that the GPU can read the pages from memory.
	 *
	 * pixel: b/200555454 requires this sync to happen even if the system
	 * is coherent.
	 */
	dma_sync_single_for_device(kbdev->dev, handle, size, DMA_TO_DEVICE);
}

/**
 * kbase_mmu_sync_pgd() - sync page directory to memory when needed.
 * @kbdev:    Device pointer.
 * @kctx:     Context pointer.
 * @phys:     Starting physical address of the destination region.
 * @handle:   Address of DMA region.
 * @size:     Size of the region to sync.
 * @flush_op: MMU cache flush operation to perform on the physical address
 *            range, if GPU control is available.
 *
 * This function is called whenever the association between a virtual address
 * range and a physical address range changes, because a mapping is created or
 * destroyed.
 * One of the effects of this operation is performing an MMU cache flush
 * operation only on the physical address range affected by this function, if
 * GPU control is available.
 *
 * This should be called after each page directory update.
 */
static void kbase_mmu_sync_pgd(struct kbase_device *kbdev, struct kbase_context *kctx,
			       phys_addr_t phys, dma_addr_t handle, size_t size,
			       enum kbase_mmu_op_type flush_op)
{
	kbase_mmu_sync_pgd_cpu(kbdev, handle, size);
	kbase_mmu_sync_pgd_gpu(kbdev, kctx, phys, size, flush_op);
}

/*
 * Definitions:
 * - PGD: Page Directory.
 * - PTE: Page Table Entry. A 64bit value pointing to the next
 *        level of translation
 * - ATE: Address Translation Entry. A 64bit value pointing to
 *        a 4kB physical page.
 */

/**
 * kbase_mmu_update_and_free_parent_pgds() - Update number of valid entries and
 *                                           free memory of the page directories
 *
 * @kbdev:    Device pointer.
 * @mmut:     GPU MMU page table.
 * @pgds:     Physical addresses of page directories to be freed.
 * @vpfn:     The virtual page frame number.
 * @level:    The level of MMU page table that needs to be updated.
 * @flush_op: The type of MMU flush operation to perform.
 * @dirty_pgds: Flags to track every level where a PGD has been updated.
 * @as_nr:     GPU address space number for which invalidate is required.
 */
static void kbase_mmu_update_and_free_parent_pgds(struct kbase_device *kbdev,
						  struct kbase_mmu_table *mmut, phys_addr_t *pgds,
						  u64 vpfn, int level,
						  enum kbase_mmu_op_type flush_op, u64 *dirty_pgds,
						  int as_nr);

static void kbase_mmu_account_freed_pgd(struct kbase_device *kbdev, struct kbase_mmu_table *mmut)
{
	atomic_sub(1, &kbdev->memdev.used_pages);

	/* If MMU tables belong to a context then pages will have been accounted
	 * against it, so we must decrement the usage counts here.
	 */
	if (mmut->kctx) {
		kbase_process_page_usage_dec(mmut->kctx, 1);
		atomic_sub(1, &mmut->kctx->used_pages);
	}

	kbase_trace_gpu_mem_usage_dec(kbdev, mmut->kctx, 1);
}

static bool kbase_mmu_handle_isolated_pgd_page(struct kbase_device *kbdev,
					       struct kbase_mmu_table *mmut, struct page *p)
{
	struct kbase_page_metadata *page_md = kbase_page_private(p);
	bool page_is_isolated = false;

	lockdep_assert_held(&mmut->mmu_lock);

	if (!kbase_is_page_migration_enabled())
		return false;

	spin_lock(&page_md->migrate_lock);
	if (PAGE_STATUS_GET(page_md->status) == PT_MAPPED) {
		WARN_ON_ONCE(!mmut->kctx);
		if (IS_PAGE_ISOLATED(page_md->status)) {
			page_md->status =
				PAGE_STATUS_SET(page_md->status, FREE_PT_ISOLATED_IN_PROGRESS);
			page_md->data.free_pt_isolated.kbdev = kbdev;
			page_is_isolated = true;
		} else {
			page_md->status = PAGE_STATUS_SET(page_md->status, FREE_IN_PROGRESS);
		}
	} else if ((PAGE_STATUS_GET(page_md->status) == FREE_IN_PROGRESS) ||
		   (PAGE_STATUS_GET(page_md->status) == ALLOCATE_IN_PROGRESS)) {
		/* Nothing to do - fall through */
	} else {
		WARN_ON_ONCE(PAGE_STATUS_GET(page_md->status) != NOT_MOVABLE);
	}
	spin_unlock(&page_md->migrate_lock);

	if (unlikely(page_is_isolated)) {
		/* Do the CPU cache flush and accounting here for the isolated
		 * PGD page, which is done inside kbase_mmu_free_pgd() for the
		 * PGD page that did not get isolated.
		 */
		dma_sync_single_for_device(kbdev->dev, pgd_dma_addr(p, page_to_phys(p)), PAGE_SIZE,
					   DMA_BIDIRECTIONAL);
		kbase_mmu_account_freed_pgd(kbdev, mmut);
	}

	return page_is_isolated;
}

/**
 * kbase_mmu_free_pgd() - Free memory of the page directory
 *
 * @kbdev:   Device pointer.
 * @mmut:    GPU MMU page table.
 * @pgd:     Physical address of page directory to be freed.
 *
 * This function is supposed to be called with mmu_lock held and after
 * ensuring that the GPU won't be able to access the page.
 */
static void kbase_mmu_free_pgd(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
			       phys_addr_t pgd)
{
	struct page *p;
	bool page_is_isolated = false;

	lockdep_assert_held(&mmut->mmu_lock);

	p = pfn_to_page(PFN_DOWN(pgd));
#if GPU_PAGES_PER_CPU_PAGE > 1
	if (free_pgd_sub_page(pgd)) {
		mmut->num_free_pgd_sub_pages++;
		mmut->last_freed_pgd_page = p;
		return;
	}

	mmut->num_free_pgd_sub_pages -= (GPU_PAGES_PER_CPU_PAGE - 1);
	if (p == mmut->last_freed_pgd_page)
		mmut->last_freed_pgd_page = NULL;
	if (p == mmut->last_allocated_pgd_page)
		mmut->last_allocated_pgd_page = NULL;
	free_pgd_page_metadata(kbdev, p);
#endif
	page_is_isolated = kbase_mmu_handle_isolated_pgd_page(kbdev, mmut, p);

	if (likely(!page_is_isolated)) {
		kbase_mem_pool_free(&kbdev->mem_pools.small[mmut->group_id], p, true);
		kbase_mmu_account_freed_pgd(kbdev, mmut);
	}
}

/**
 * kbase_mmu_free_pgds_list() - Free the PGD pages present in the list
 *
 * @kbdev:          Device pointer.
 * @mmut:           GPU MMU page table.
 *
 * This function will call kbase_mmu_free_pgd() on each page directory page
 * present in the list of free PGDs inside @mmut.
 *
 * The function is supposed to be called after the GPU cache and MMU TLB has
 * been invalidated post the teardown loop.
 *
 * The mmu_lock shall be held prior to calling the function.
 */
static void kbase_mmu_free_pgds_list(struct kbase_device *kbdev, struct kbase_mmu_table *mmut)
{
	size_t i;

	lockdep_assert_held(&mmut->mmu_lock);

	for (i = 0; i < mmut->scratch_mem.free_pgds.head_index; i++)
		kbase_mmu_free_pgd(kbdev, mmut, mmut->scratch_mem.free_pgds.pgds[i]);

	mmut->scratch_mem.free_pgds.head_index = 0;
}

static void kbase_mmu_add_to_free_pgds_list(struct kbase_mmu_table *mmut, phys_addr_t pgd)
{
	lockdep_assert_held(&mmut->mmu_lock);

	if (WARN_ON_ONCE(mmut->scratch_mem.free_pgds.head_index > (MAX_FREE_PGDS - 1)))
		return;

	mmut->scratch_mem.free_pgds.pgds[mmut->scratch_mem.free_pgds.head_index++] = pgd;
}

static inline void kbase_mmu_reset_free_pgds_list(struct kbase_mmu_table *mmut)
{
	lockdep_assert_held(&mmut->mmu_lock);

	mmut->scratch_mem.free_pgds.head_index = 0;
}

/**
 * reg_grow_calc_extra_pages() - Calculate the number of backed pages to add to
 *                               a region on a GPU page fault
 * @kbdev:         KBase device
 * @reg:           The region that will be backed with more pages
 * @fault_rel_pfn: PFN of the fault relative to the start of the region
 *
 * This calculates how much to increase the backing of a region by, based on
 * where a GPU page fault occurred and the flags in the region.
 *
 * This can be more than the minimum number of pages that would reach
 * @fault_rel_pfn, for example to reduce the overall rate of page fault
 * interrupts on a region, or to ensure that the end address is aligned.
 *
 * Return: the number of backed pages to increase by
 */
static size_t reg_grow_calc_extra_pages(struct kbase_device *kbdev, struct kbase_va_region *reg,
					size_t fault_rel_pfn)
{
	size_t multiple = reg->extension;
	size_t reg_current_size = kbase_reg_current_backed_size(reg);
	size_t minimum_extra = fault_rel_pfn - reg_current_size + 1;
	size_t remainder;

	if (!multiple) {
		dev_warn(
			kbdev->dev,
			"VA Region 0x%llx extension was 0, allocator needs to set this properly for KBASE_REG_PF_GROW",
			((unsigned long long)reg->start_pfn) << PAGE_SHIFT);
		return minimum_extra;
	}

	/* Calculate the remainder to subtract from minimum_extra to make it
	 * the desired (rounded down) multiple of the extension.
	 * Depending on reg's flags, the base used for calculating multiples is
	 * different
	 */

	/* multiple is based from the current backed size, even if the
	 * current backed size/pfn for end of committed memory are not
	 * themselves aligned to multiple
	 */
	remainder = minimum_extra % multiple;

#if !MALI_USE_CSF
	if (reg->flags & KBASE_REG_TILER_ALIGN_TOP) {
		/* multiple is based from the top of the initial commit, which
		 * has been allocated in such a way that (start_pfn +
		 * initial_commit) is already aligned to multiple. Hence the
		 * pfn for the end of committed memory will also be aligned to
		 * multiple
		 */
		size_t initial_commit = reg->initial_commit;

		if (fault_rel_pfn < initial_commit) {
			/* this case is just to catch in case it's been
			 * recommitted by userspace to be smaller than the
			 * initial commit
			 */
			minimum_extra = initial_commit - reg_current_size;
			remainder = 0;
		} else {
			/* same as calculating
			 * (fault_rel_pfn - initial_commit + 1)
			 */
			size_t pages_after_initial =
				minimum_extra + reg_current_size - initial_commit;

			remainder = pages_after_initial % multiple;
		}
	}
#endif /* !MALI_USE_CSF */

	if (remainder == 0)
		return minimum_extra;

	return minimum_extra + multiple - remainder;
}

#ifdef CONFIG_MALI_CINSTR_GWT
static void kbase_gpu_mmu_handle_write_faulting_as(struct kbase_device *kbdev,
						   struct kbase_as *faulting_as, u64 start_pfn,
						   size_t nr, u32 kctx_id, u64 dirty_pgds)
{
	/* Calls to this function are inherently synchronous, with respect to
	 * MMU operations.
	 */
	const enum kbase_caller_mmu_sync_info mmu_sync_info = CALLER_MMU_SYNC;
	struct kbase_mmu_hw_op_param op_param;
	unsigned long irq_flags;
	int ret = 0;

	kbase_mmu_hw_clear_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);

	/* flush L2 and unlock the VA (resumes the MMU) */
	op_param.vpfn = start_pfn;
	op_param.nr = nr;
	op_param.op = KBASE_MMU_OP_FLUSH_PT;
	op_param.kctx_id = kctx_id;
	op_param.mmu_sync_info = mmu_sync_info;
	spin_lock_irqsave(&kbdev->hwaccess_lock, irq_flags);
	if (mmu_flush_cache_on_gpu_ctrl(kbdev)) {
		op_param.flush_skip_levels = pgd_level_to_skip_flush(dirty_pgds);
		ret = kbase_mmu_hw_do_flush_on_gpu_ctrl(kbdev, faulting_as, &op_param);
	} else {
		ret = kbase_mmu_hw_do_flush(kbdev, faulting_as, &op_param);
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, irq_flags);

	if (ret)
		dev_err(kbdev->dev,
			"Flush for GPU page fault due to write access did not complete");

	kbase_mmu_hw_enable_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);
}

static void set_gwt_element_page_addr_and_size(struct kbasep_gwt_list_element *element,
					       u64 fault_page_addr, struct tagged_addr fault_phys)
{
	u64 fault_pfn = fault_page_addr >> PAGE_SHIFT;
	unsigned int vindex = fault_pfn & (NUM_PAGES_IN_2MB_LARGE_PAGE - 1);

	/* If the fault address lies within a 2MB page, then consider
	 * the whole 2MB page for dumping to avoid incomplete dumps.
	 */
	if (is_huge(fault_phys) && (vindex == index_in_large_page(fault_phys))) {
		element->page_addr = fault_page_addr & ~(SZ_2M - 1UL);
		element->num_pages = NUM_PAGES_IN_2MB_LARGE_PAGE;
	} else {
		element->page_addr = fault_page_addr;
		element->num_pages = 1;
	}
}

static void kbase_gpu_mmu_handle_write_fault(struct kbase_context *kctx,
					     struct kbase_as *faulting_as)
{
	struct kbasep_gwt_list_element *pos;
	struct kbase_va_region *region;
	struct kbase_device *kbdev;
	struct tagged_addr *fault_phys_addr;
	struct kbase_fault *fault;
	u64 fault_pfn, pfn_offset;
	unsigned int as_no;
	u64 dirty_pgds = 0;

	as_no = faulting_as->number;
	kbdev = container_of(faulting_as, struct kbase_device, as[as_no]);
	fault = &faulting_as->pf_data;
	fault_pfn = fault->addr >> PAGE_SHIFT;

	kbase_gpu_vm_lock(kctx);

	/* Find region and check if it should be writable. */
	region = kbase_region_tracker_find_region_enclosing_address(kctx, fault->addr);
	if (kbase_is_region_invalid_or_free(region)) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as,
						"Memory is not mapped on the GPU",
						&faulting_as->pf_data);
		return;
	}

	if (!(region->flags & KBASE_REG_GPU_WR)) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as,
						"Region does not have write permissions",
						&faulting_as->pf_data);
		return;
	}

	if (unlikely(region->gpu_alloc->type == KBASE_MEM_TYPE_ALIAS)) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(
			kctx, faulting_as, "Unexpected write permission fault on an alias region",
			&faulting_as->pf_data);
		return;
	}

	pfn_offset = fault_pfn - region->start_pfn;
	fault_phys_addr = &kbase_get_gpu_phy_pages(region)[pfn_offset];

	/* Capture addresses of faulting write location
	 * for job dumping if write tracking is enabled.
	 */
	if (kctx->gwt_enabled) {
		u64 fault_page_addr = fault->addr & PAGE_MASK;
		bool found = false;
		/* Check if this write was already handled. */
		list_for_each_entry(pos, &kctx->gwt_current_list, link) {
			if (fault_page_addr == pos->page_addr) {
				found = true;
				break;
			}
		}

		if (!found) {
			pos = kmalloc(sizeof(*pos), GFP_KERNEL);
			if (pos) {
				pos->region = region;
				set_gwt_element_page_addr_and_size(pos, fault_page_addr,
								   *fault_phys_addr);
				list_add(&pos->link, &kctx->gwt_current_list);
			} else {
				dev_warn(kbdev->dev, "kmalloc failure");
			}
		}
	}

	/* Now make this faulting page writable to GPU. */
	kbase_mmu_update_pages_no_flush(kbdev, &kctx->mmu, fault_pfn, fault_phys_addr, 1,
					region->flags, region->gpu_alloc->group_id, &dirty_pgds);

	kbase_gpu_mmu_handle_write_faulting_as(kbdev, faulting_as, fault_pfn, 1, kctx->id,
					       dirty_pgds);

	kbase_gpu_vm_unlock(kctx);
}

static void kbase_gpu_mmu_handle_permission_fault(struct kbase_context *kctx,
						  struct kbase_as *faulting_as)
{
	struct kbase_fault *fault = &faulting_as->pf_data;

	switch (AS_FAULTSTATUS_ACCESS_TYPE_GET(fault->status)) {
	case AS_FAULTSTATUS_ACCESS_TYPE_ATOMIC:
	case AS_FAULTSTATUS_ACCESS_TYPE_WRITE:
		kbase_gpu_mmu_handle_write_fault(kctx, faulting_as);
		break;
	case AS_FAULTSTATUS_ACCESS_TYPE_EXECUTE:
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Execute Permission fault",
						fault);
		break;
	case AS_FAULTSTATUS_ACCESS_TYPE_READ:
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Read Permission fault", fault);
		break;
	default:
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Unknown Permission fault",
						fault);
		break;
	}
}
#endif

/**
 * estimate_pool_space_required - Determine how much a pool should be grown by to support a future
 * allocation
 * @pool:           The memory pool to check, including its linked pools
 * @pages_required: Number of small pages require for the pool to support a future allocation
 *
 * The value returned is accounting for the size of @pool and the size of each memory pool linked to
 * @pool. Hence, the caller should use @pool and (if not already satisfied) all its linked pools to
 * allocate from.
 *
 * Note: this is only an estimate, because even during the calculation the memory pool(s) involved
 * can be updated to be larger or smaller. Hence, the result is only a guide as to whether an
 * allocation could succeed, or an estimate of the correct amount to grow the pool by. The caller
 * should keep attempting an allocation and then re-growing with a new value queried form this
 * function until the allocation succeeds.
 *
 * Return: an estimate of the amount of extra small pages in @pool that are required to satisfy an
 * allocation, or 0 if @pool (including its linked pools) is likely to already satisfy the
 * allocation.
 */
static size_t estimate_pool_space_required(struct kbase_mem_pool *pool, const size_t pages_required)
{
	size_t pages_still_required;

	for (pages_still_required = pages_required; pool != NULL && pages_still_required;
	     pool = pool->next_pool) {
		size_t pool_size_small;

		kbase_mem_pool_lock(pool);

		pool_size_small = kbase_mem_pool_size(pool) << pool->order;
		if (pool_size_small >= pages_still_required)
			pages_still_required = 0;
		else
			pages_still_required -= pool_size_small;

		kbase_mem_pool_unlock(pool);
	}
	return pages_still_required;
}

/**
 * page_fault_try_alloc - Try to allocate memory from a context pool
 * @kctx:          Context pointer
 * @region:        Region to grow
 * @new_pages:     Number of small pages to allocate
 * @pages_to_grow: Pointer to variable to store number of outstanding pages on failure. This can be
 *                 either small or 2 MiB pages, depending on the number of pages requested.
 * @grow_2mb_pool: Pointer to variable to store which pool needs to grow - true for 2 MiB, false for
 *                 pool of small pages.
 * @fallback_to_small:  Whether fallback to small pages or not
 * @prealloc_sas:  Pointer to kbase_sub_alloc structures
 *
 * This function will try to allocate as many pages as possible from the context pool, then if
 * required will try to allocate the remaining pages from the device pool.
 *
 * This function will not allocate any new memory beyond that is already present in the context or
 * device pools. This is because it is intended to be called whilst the thread has acquired the
 * region list lock with kbase_gpu_vm_lock(), and a large enough memory allocation whilst that is
 * held could invoke the OoM killer and cause an effective deadlock with kbase_cpu_vm_close().
 *
 * If 2 MiB pages are enabled and new_pages is >= 2 MiB then pages_to_grow will be a count of 2 MiB
 * pages, otherwise it will be a count of small pages.
 *
 * Return: true if successful, false on failure
 */
static bool page_fault_try_alloc(struct kbase_context *kctx, struct kbase_va_region *region,
				 size_t new_pages, size_t *pages_to_grow, bool *grow_2mb_pool,
				 bool fallback_to_small, struct kbase_sub_alloc **prealloc_sas)
{
	size_t total_gpu_pages_alloced = 0;
	size_t total_cpu_pages_alloced = 0;
	struct kbase_mem_pool *pool, *root_pool;
	bool alloc_failed = false;
	size_t pages_still_required;
	size_t total_mempools_free_small = 0;

	lockdep_assert_held(&kctx->reg_lock);
	lockdep_assert_held(&kctx->mem_partials_lock);

	if (WARN_ON(region->gpu_alloc->group_id >= MEMORY_GROUP_MANAGER_NR_GROUPS)) {
		/* Do not try to grow the memory pool */
		*pages_to_grow = 0;
		return false;
	}

	if (kbase_is_large_pages_enabled() && new_pages >= NUM_PAGES_IN_2MB_LARGE_PAGE &&
	    !fallback_to_small) {
		root_pool = &kctx->mem_pools.large[region->gpu_alloc->group_id];
		*grow_2mb_pool = true;
	} else {
		root_pool = &kctx->mem_pools.small[region->gpu_alloc->group_id];
		*grow_2mb_pool = false;
	}

	if (region->gpu_alloc != region->cpu_alloc)
		new_pages *= 2;

	/* Determine how many pages are in the pools before trying to allocate.
	 * Don't attempt to allocate & free if the allocation can't succeed.
	 */
	pages_still_required = estimate_pool_space_required(root_pool, new_pages);

	if (pages_still_required) {
		/* Insufficient pages in pools. Don't try to allocate - just
		 * request a grow.
		 */
		*pages_to_grow = pages_still_required;

		return false;
	}

	/* Since we're not holding any of the mempool locks, the amount of memory in the pools may
	 * change between the above estimate and the actual allocation.
	 */
	pages_still_required = new_pages;
	for (pool = root_pool; pool != NULL && pages_still_required; pool = pool->next_pool) {
		size_t pool_size_small;
		size_t pages_to_alloc_small;
		size_t pages_to_alloc_small_per_alloc;

		kbase_mem_pool_lock(pool);

		/* Allocate as much as possible from this pool*/
		pool_size_small = kbase_mem_pool_size(pool) << pool->order;
		total_mempools_free_small += pool_size_small;
		pages_to_alloc_small = MIN(pages_still_required, pool_size_small);
		if (region->gpu_alloc == region->cpu_alloc)
			pages_to_alloc_small_per_alloc = pages_to_alloc_small;
		else
			pages_to_alloc_small_per_alloc = pages_to_alloc_small >> 1;

		if (pages_to_alloc_small) {
			struct tagged_addr *gpu_pages = kbase_alloc_phy_pages_helper_locked(
				region->gpu_alloc, pool, pages_to_alloc_small_per_alloc,
				&prealloc_sas[0]);

			if (!gpu_pages)
				alloc_failed = true;
			else
				total_gpu_pages_alloced += pages_to_alloc_small_per_alloc;

			if (!alloc_failed && region->gpu_alloc != region->cpu_alloc) {
				struct tagged_addr *cpu_pages = kbase_alloc_phy_pages_helper_locked(
					region->cpu_alloc, pool, pages_to_alloc_small_per_alloc,
					&prealloc_sas[1]);

				if (!cpu_pages)
					alloc_failed = true;
				else
					total_cpu_pages_alloced += pages_to_alloc_small_per_alloc;
			}
		}

		kbase_mem_pool_unlock(pool);

		if (alloc_failed) {
			WARN_ON(!pages_still_required);
			WARN_ON(pages_to_alloc_small >= pages_still_required);
			WARN_ON(pages_to_alloc_small_per_alloc >= pages_still_required);
			break;
		}

		pages_still_required -= pages_to_alloc_small;
	}

	if (pages_still_required) {
		/* Allocation was unsuccessful. We have dropped the mem_pool lock after allocation,
		 * so must in any case use kbase_free_phy_pages_helper() rather than
		 * kbase_free_phy_pages_helper_locked()
		 */
		if (total_gpu_pages_alloced > 0)
			kbase_free_phy_pages_helper(region->gpu_alloc, total_gpu_pages_alloced);
		if (region->gpu_alloc != region->cpu_alloc && total_cpu_pages_alloced > 0)
			kbase_free_phy_pages_helper(region->cpu_alloc, total_cpu_pages_alloced);

		if (alloc_failed) {
			/* Note that in allocating from the above memory pools, we always ensure
			 * never to request more than is available in each pool with the pool's
			 * lock held. Hence failing to allocate in such situations would be unusual
			 * and we should cancel the growth instead (as re-growing the memory pool
			 * might not fix the situation)
			 */
			dev_warn(
				kctx->kbdev->dev,
				"Page allocation failure of %zu pages: managed %zu pages, mempool (inc linked pools) had %zu pages available",
				new_pages, total_gpu_pages_alloced + total_cpu_pages_alloced,
				total_mempools_free_small);
			*pages_to_grow = 0;
		} else {
			/* Tell the caller to try to grow the memory pool
			 *
			 * Freeing pages above may have spilled or returned them to the OS, so we
			 * have to take into account how many are still in the pool before giving a
			 * new estimate for growth required of the pool. We can just re-estimate a
			 * new value.
			 */
			pages_still_required = estimate_pool_space_required(root_pool, new_pages);
			if (pages_still_required) {
				*pages_to_grow = pages_still_required;
			} else {
				/* It's possible another thread could've grown the pool to be just
				 * big enough after we rolled back the allocation. Request at least
				 * one more page to ensure the caller doesn't fail the growth by
				 * conflating it with the alloc_failed case above
				 */
				*pages_to_grow = 1u;
			}
		}

		return false;
	}

	/* Allocation was successful. No pages to grow, return success. */
	*pages_to_grow = 0;

	return true;
}

void kbase_mmu_page_fault_worker(struct work_struct *data)
{
	u64 fault_pfn;
	u32 fault_status;
	size_t new_pages;
	size_t fault_rel_pfn;
	struct kbase_as *faulting_as;
	unsigned int as_no;
	struct kbase_context *kctx;
	struct kbase_device *kbdev;
	struct kbase_va_region *region;
	struct kbase_fault *fault;
	int err;
	bool grown = false;
	size_t pages_to_grow;
	bool grow_2mb_pool = false;
	bool fallback_to_small = false;
	struct kbase_sub_alloc *prealloc_sas[2] = { NULL, NULL };
	int i;
	size_t current_backed_size;
#if MALI_JIT_PRESSURE_LIMIT_BASE
	size_t pages_trimmed = 0;
#endif
	unsigned long hwaccess_flags;

	/* Calls to this function are inherently synchronous, with respect to
	 * MMU operations.
	 */
	const enum kbase_caller_mmu_sync_info mmu_sync_info = CALLER_MMU_SYNC;

	faulting_as = container_of(data, struct kbase_as, work_pagefault);
	fault = &faulting_as->pf_data;
	fault_pfn = fault->addr >> PAGE_SHIFT;
	as_no = faulting_as->number;

	kbdev = container_of(faulting_as, struct kbase_device, as[as_no]);
	dev_dbg(kbdev->dev, "Entering %s %pK, fault_pfn %lld, as_no %u", __func__, (void *)data,
		fault_pfn, as_no);

	/* Grab the context that was already refcounted in kbase_mmu_interrupt()
	 * Therefore, it cannot be scheduled out of this AS until we explicitly
	 * release it
	 */
	kctx = kbase_ctx_sched_as_to_ctx(kbdev, as_no);
	if (!kctx) {
		atomic_dec(&kbdev->faults_pending);
		return;
	}

	KBASE_DEBUG_ASSERT(kctx->kbdev == kbdev);

#if MALI_JIT_PRESSURE_LIMIT_BASE
#if !MALI_USE_CSF
	rt_mutex_lock(&kctx->jctx.lock);
#endif
#endif

	/* check if we still have GPU */
	if (unlikely(!kbase_io_has_gpu(kbdev))) {
		dev_dbg(kbdev->dev, "%s: GPU has been removed", __func__);
		goto fault_done;
	}

	if (unlikely(fault->protected_mode)) {
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Protected mode fault", fault);
		kbase_mmu_hw_clear_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);

		goto fault_done;
	}

	fault_status = fault->status;
	switch (AS_FAULTSTATUS_EXCEPTION_TYPE_GET(fault_status)) {
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSLATION_FAULT_0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSLATION_FAULT_1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSLATION_FAULT_2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSLATION_FAULT_3:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSLATION_FAULT_4:
#if !MALI_USE_CSF
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSLATION_FAULT_IDENTITY:
#endif
		/* need to check against the region to handle this one */
		break;

	case AS_FAULTSTATUS_EXCEPTION_TYPE_PERMISSION_FAULT_0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_PERMISSION_FAULT_1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_PERMISSION_FAULT_2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_PERMISSION_FAULT_3:
#ifdef CONFIG_MALI_CINSTR_GWT
		/* If GWT was ever enabled then we need to handle
		 * write fault pages even if the feature was disabled later.
		 */
		if (kctx->gwt_was_enabled) {
			kbase_gpu_mmu_handle_permission_fault(kctx, faulting_as);
			goto fault_done;
		}
#endif

		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Permission failure", fault);
		goto fault_done;

#if !MALI_USE_CSF
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSTAB_BUS_FAULT_0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSTAB_BUS_FAULT_1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSTAB_BUS_FAULT_2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_TRANSTAB_BUS_FAULT_3:
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Translation table bus fault",
						fault);
		goto fault_done;
#endif

#if !MALI_USE_CSF
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ACCESS_FLAG_0:
		fallthrough;
#endif
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ACCESS_FLAG_1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ACCESS_FLAG_2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ACCESS_FLAG_3:
		/* nothing to do, but we don't expect this fault currently */
		dev_warn(kbdev->dev, "Access flag unexpectedly set");
		goto fault_done;

#if MALI_USE_CSF
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_IN:
		fallthrough;
#else
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_IN0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_IN1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_IN2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_IN3:
		fallthrough;
#endif
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_OUT0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_OUT1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_OUT2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_ADDRESS_SIZE_FAULT_OUT3:
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Address size fault", fault);
		goto fault_done;

	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_FAULT_0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_FAULT_1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_FAULT_2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_FAULT_3:
#if !MALI_USE_CSF
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_NONCACHEABLE_0:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_NONCACHEABLE_1:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_NONCACHEABLE_2:
		fallthrough;
	case AS_FAULTSTATUS_EXCEPTION_TYPE_MEMORY_ATTRIBUTE_NONCACHEABLE_3:
#endif
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Memory attributes fault",
						fault);
		goto fault_done;

	default:
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Unknown fault code", fault);
		goto fault_done;
	}

page_fault_retry:
	if (kbase_is_large_pages_enabled() && !fallback_to_small) {
		/* Preallocate (or re-allocate) memory for the sub-allocation structs if necessary */
		for (i = 0; i != ARRAY_SIZE(prealloc_sas); ++i) {
			if (!prealloc_sas[i]) {
				prealloc_sas[i] = kmalloc(sizeof(*prealloc_sas[i]), GFP_KERNEL);

				if (!prealloc_sas[i]) {
					kbase_mmu_report_fault_and_kill(
						kctx, faulting_as,
						"Failed pre-allocating memory for sub-allocations' metadata",
						fault);
					goto fault_done;
				}
			}
		}
	}

	/* so we have a translation fault,
	 * let's see if it is for growable memory
	 */
	kbase_gpu_vm_lock(kctx);

	region = kbase_region_tracker_find_region_enclosing_address(kctx, fault->addr);
	if (kbase_is_region_invalid_or_free(region)) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as,
						"Memory is not mapped on the GPU", fault);
		goto fault_done;
	}

	if (region->gpu_alloc->type == KBASE_MEM_TYPE_IMPORTED_UMM) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as,
						"DMA-BUF is not mapped on the GPU", fault);
		goto fault_done;
	}

	if (unlikely(region->gpu_alloc->type == KBASE_MEM_TYPE_ALIAS)) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as,
						"Unexpected page fault on an alias region",
						&faulting_as->pf_data);
		goto fault_done;
	}

	if (region->gpu_alloc->group_id >= MEMORY_GROUP_MANAGER_NR_GROUPS) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Bad physical memory group ID",
						fault);
		goto fault_done;
	}

	if ((region->flags & GROWABLE_FLAGS_REQUIRED) != GROWABLE_FLAGS_REQUIRED) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Memory is not growable", fault);
		goto fault_done;
	}

	if ((region->flags & BASEP_MEM_DONT_NEED)) {
		kbase_gpu_vm_unlock(kctx);
		kbase_mmu_report_fault_and_kill(kctx, faulting_as,
						"Don't need memory can't be grown", fault);
		goto fault_done;
	}

	if (AS_FAULTSTATUS_ACCESS_TYPE_GET(fault_status) == AS_FAULTSTATUS_ACCESS_TYPE_READ)
		dev_warn(kbdev->dev, "Grow on pagefault while reading");

	/* find the size we need to grow it by
	 * we know the result fit in a size_t due to
	 * kbase_region_tracker_find_region_enclosing_address
	 * validating the fault_address to be within a size_t from the start_pfn
	 */
	fault_rel_pfn = fault_pfn - region->start_pfn;

	current_backed_size = kbase_reg_current_backed_size(region);

	if (fault_rel_pfn < current_backed_size) {
		struct kbase_mmu_hw_op_param op_param;

		dev_dbg(kbdev->dev,
			"Page fault @ VA 0x%llx in allocated region 0x%llx-0x%llx of growable TMEM: Ignoring",
			fault->addr, region->start_pfn, region->start_pfn + current_backed_size);

		kbase_mmu_hw_clear_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);
		/* [1] in case another page fault occurred while we were
		 * handling the (duplicate) page fault we need to ensure we
		 * don't loose the other page fault as result of us clearing
		 * the MMU IRQ. Therefore, after we clear the MMU IRQ we send
		 * an UNLOCK command that will retry any stalled memory
		 * transaction (which should cause the other page fault to be
		 * raised again).
		 */
		op_param.mmu_sync_info = mmu_sync_info;
		op_param.kctx_id = kctx->id;
		/* Usually it is safe to skip the MMU cache invalidate for all levels
		 * in case of duplicate page faults. But for the pathological scenario
		 * where the faulty VA gets mapped by the time page fault worker runs it
		 * becomes imperative to invalidate MMU cache for all levels, otherwise
		 * there is a possibility of repeated page faults on GPUs which supports
		 * fine grained MMU cache invalidation.
		 */
		op_param.flush_skip_levels = 0x0;
		op_param.vpfn = fault_pfn;
		op_param.nr = 1;
		spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
		err = kbase_mmu_hw_do_unlock(kbdev, faulting_as, &op_param);
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);

		if (err) {
			dev_err(kbdev->dev,
				"Invalidation for MMU did not complete on handling page fault @ VA 0x%llx",
				fault->addr);
		}

		kbase_mmu_hw_enable_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);
		kbase_gpu_vm_unlock(kctx);

		goto fault_done;
	}

	new_pages = reg_grow_calc_extra_pages(kbdev, region, fault_rel_pfn);

	/* cap to max vsize */
	new_pages = min(new_pages, region->nr_pages - current_backed_size);
	dev_dbg(kctx->kbdev->dev, "Allocate %zu pages on page fault", new_pages);

	if (new_pages == 0) {
		struct kbase_mmu_hw_op_param op_param;

		/* Duplicate of a fault we've already handled, nothing to do */
		kbase_mmu_hw_clear_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);

		/* See comment [1] about UNLOCK usage */
		op_param.mmu_sync_info = mmu_sync_info;
		op_param.kctx_id = kctx->id;
		/* Usually it is safe to skip the MMU cache invalidate for all levels
		 * in case of duplicate page faults. But for the pathological scenario
		 * where the faulty VA gets mapped by the time page fault worker runs it
		 * becomes imperative to invalidate MMU cache for all levels, otherwise
		 * there is a possibility of repeated page faults on GPUs which supports
		 * fine grained MMU cache invalidation.
		 */
		op_param.flush_skip_levels = 0x0;
		op_param.vpfn = fault_pfn;
		op_param.nr = 1;
		spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
		err = kbase_mmu_hw_do_unlock(kbdev, faulting_as, &op_param);
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);

		if (err) {
			dev_err(kbdev->dev,
				"Invalidation for MMU did not complete on handling page fault @ VA 0x%llx",
				fault->addr);
		}

		kbase_mmu_hw_enable_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);
		kbase_gpu_vm_unlock(kctx);
		goto fault_done;
	}

	pages_to_grow = 0;

#if MALI_JIT_PRESSURE_LIMIT_BASE
	if ((region->flags & BASEP_MEM_ACTIVE_JIT_ALLOC) && !pages_trimmed) {
		kbase_jit_request_phys_increase(kctx, new_pages);
		pages_trimmed = new_pages;
	}
#endif

	spin_lock(&kctx->mem_partials_lock);
	grown = page_fault_try_alloc(kctx, region, new_pages, &pages_to_grow, &grow_2mb_pool,
				     fallback_to_small, prealloc_sas);
	spin_unlock(&kctx->mem_partials_lock);

	if (grown) {
		u64 dirty_pgds = 0;
		u64 pfn_offset;
		struct kbase_mmu_hw_op_param op_param;

		/* alloc success */
		WARN_ON(kbase_reg_current_backed_size(region) > region->nr_pages);

		/* set up the new pages */
		pfn_offset = kbase_reg_current_backed_size(region) - new_pages;
		/*
		 * Note:
		 * Issuing an MMU operation will unlock the MMU and cause the
		 * translation to be replayed. If the page insertion fails then
		 * rather then trying to continue the context should be killed
		 * so the no_flush version of insert_pages is used which allows
		 * us to unlock the MMU as we see fit.
		 */
		err = mmu_insert_pages_no_flush(kbdev, &kctx->mmu, region->start_pfn + pfn_offset,
						&kbase_get_gpu_phy_pages(region)[pfn_offset],
						new_pages, region->flags,
						region->gpu_alloc->group_id, &dirty_pgds, region,
						false);
		if (err) {
			kbase_free_phy_pages_helper(region->gpu_alloc, new_pages);
			if (region->gpu_alloc != region->cpu_alloc)
				kbase_free_phy_pages_helper(region->cpu_alloc, new_pages);
			kbase_gpu_vm_unlock(kctx);
			/* The locked VA region will be unlocked and the cache
			 * invalidated in here
			 */
			kbase_mmu_report_fault_and_kill(kctx, faulting_as,
							"Page table update failure", fault);
			goto fault_done;
		}
		KBASE_TLSTREAM_AUX_PAGEFAULT(kbdev, kctx->id, as_no, (u64)new_pages);

			if (kbase_reg_is_valid(kbdev, MMU_AS_OFFSET(as_no, FAULTEXTRA)))
				trace_mali_mmu_page_fault_extra_grow(region, fault, new_pages);
			else
				trace_mali_mmu_page_fault_grow(region, fault, new_pages);
		/* AS transaction begin */

		/* clear MMU interrupt - this needs to be done after updating
		 * the page tables but before issuing a FLUSH command. The
		 * FLUSH cmd has a side effect that it restarts stalled memory
		 * transactions in other address spaces which may cause
		 * another fault to occur. If we didn't clear the interrupt at
		 * this stage a new IRQ might not be raised when the GPU finds
		 * a MMU IRQ is already pending.
		 */
		kbase_mmu_hw_clear_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);

		op_param.vpfn = region->start_pfn + pfn_offset;
		op_param.nr = new_pages;
		op_param.op = KBASE_MMU_OP_FLUSH_PT;
		op_param.kctx_id = kctx->id;
		op_param.mmu_sync_info = mmu_sync_info;
		spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
		if (mmu_flush_cache_on_gpu_ctrl(kbdev)) {
			/* Unlock to invalidate the TLB (and resume the MMU) */
			op_param.flush_skip_levels = pgd_level_to_skip_flush(dirty_pgds);
			err = kbase_mmu_hw_do_unlock(kbdev, faulting_as, &op_param);
		} else {
			/* flush L2 and unlock the VA (resumes the MMU) */
			err = kbase_mmu_hw_do_flush(kbdev, faulting_as, &op_param);
		}
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);

		if (err) {
			dev_err(kbdev->dev,
				"Flush for GPU page table update did not complete on handling page fault @ VA 0x%llx",
				fault->addr);
		}

		/* AS transaction end */

		/* reenable this in the mask */
		kbase_mmu_hw_enable_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_PAGE);

#ifdef CONFIG_MALI_CINSTR_GWT
		if (kctx->gwt_enabled) {
			/* GWT also tracks growable regions. */
			struct kbasep_gwt_list_element *pos;

			pos = kmalloc(sizeof(*pos), GFP_KERNEL);
			if (pos) {
				pos->region = region;
				pos->page_addr = (region->start_pfn + pfn_offset) << PAGE_SHIFT;
				pos->num_pages = new_pages;
				list_add(&pos->link, &kctx->gwt_current_list);
			} else {
				dev_warn(kbdev->dev, "kmalloc failure");
			}
		}
#endif

#if MALI_JIT_PRESSURE_LIMIT_BASE
		if (pages_trimmed) {
			kbase_jit_done_phys_increase(kctx, pages_trimmed);
			pages_trimmed = 0;
		}
#endif
		kbase_gpu_vm_unlock(kctx);
	} else {
		int ret = -ENOMEM;
		const u8 group_id = region->gpu_alloc->group_id;

		kbase_gpu_vm_unlock(kctx);

		/* If the memory pool was insufficient then grow it and retry.
		 * Otherwise fail the allocation.
		 */
		if (pages_to_grow > 0) {
			if (kbase_is_large_pages_enabled() && grow_2mb_pool) {
				/* Round page requirement up to nearest 2 MB */
				struct kbase_mem_pool *const lp_mem_pool =
					&kctx->mem_pools.large[group_id];

				pages_to_grow =
					(pages_to_grow + ((1u << lp_mem_pool->order) - 1u)) >>
					lp_mem_pool->order;

				ret = kbase_mem_pool_grow(lp_mem_pool, pages_to_grow, kctx->task);
				/* Retry handling the fault with small pages if required
				 * number of 2MB pages couldn't be allocated.
				 */
				if (ret < 0) {
					fallback_to_small = true;
					dev_dbg(kbdev->dev,
						"No room for 2MB pages, fallback to small pages");
					goto page_fault_retry;
				}
			} else {
				struct kbase_mem_pool *const mem_pool =
					&kctx->mem_pools.small[group_id];

				ret = kbase_mem_pool_grow(mem_pool, pages_to_grow, kctx->task);
			}
		}
		if (ret < 0) {
			/* failed to extend, handle as a normal PF */
			if (unlikely(ret == -EPERM))
				kbase_ctx_flag_set(kctx, KCTX_PAGE_FAULT_REPORT_SKIP);
			kbase_mmu_report_fault_and_kill(kctx, faulting_as,
							"Page allocation failure", fault);
		} else {
			dev_dbg(kbdev->dev, "Try again after pool_grow");
			goto page_fault_retry;
		}
	}

fault_done:
#if MALI_JIT_PRESSURE_LIMIT_BASE
	if (pages_trimmed) {
		kbase_gpu_vm_lock(kctx);
		kbase_jit_done_phys_increase(kctx, pages_trimmed);
		kbase_gpu_vm_unlock(kctx);
	}
#if !MALI_USE_CSF
	rt_mutex_unlock(&kctx->jctx.lock);
#endif
#endif

	for (i = 0; i != ARRAY_SIZE(prealloc_sas); ++i)
		kfree(prealloc_sas[i]);

	/*
	 * By this point, the fault was handled in some way,
	 * so release the ctx refcount
	 */
	release_ctx(kbdev, kctx);

	atomic_dec(&kbdev->faults_pending);
	dev_dbg(kbdev->dev, "Leaving page_fault_worker %pK", (void *)data);
}

/**
 * kbase_mmu_alloc_pgd() - Allocate a PGD
 *
 * @kbdev:    Pointer to the instance of a kbase device.
 * @mmut:     Structure holding details of the MMU table for a kcontext.
 *
 * A 4KB sized PGD page is allocated for the PGD from the memory pool if PAGE_SIZE is 4KB.
 * Otherwise PGD is sub-allocated from a page that is allocated from the memory pool or
 * from one of the pages earlier allocated for the PGD of @mmut.
 *
 * Return:    Physical address of the allocated PGD.
 */
static phys_addr_t kbase_mmu_alloc_pgd(struct kbase_device *kbdev, struct kbase_mmu_table *mmut)
{
	u64 *page;
	struct page *p;
	phys_addr_t pgd;

	lockdep_assert_held(&mmut->mmu_lock);

#if GPU_PAGES_PER_CPU_PAGE > 1
	pgd = allocate_from_pgd_pages_list(mmut);
	if (pgd != KBASE_INVALID_PHYSICAL_ADDRESS)
		return pgd;
#endif

	p = kbase_mem_pool_alloc(&kbdev->mem_pools.small[mmut->group_id]);
	if (!p)
		return KBASE_INVALID_PHYSICAL_ADDRESS;

	page = kbase_kmap(p);

	if (page == NULL)
		goto alloc_free;

#if GPU_PAGES_PER_CPU_PAGE > 1
	if (!alloc_pgd_page_metadata(kbdev, mmut, p)) {
		kbase_kunmap(p, page);
		goto alloc_free;
	}
	mmut->num_free_pgd_sub_pages += (GPU_PAGES_PER_CPU_PAGE - 1);
	mmut->last_allocated_pgd_page = p;
#endif

	pgd = page_to_phys(p);

	/* If the MMU tables belong to a context then account the memory usage
	 * to that context, otherwise the MMU tables are device wide and are
	 * only accounted to the device.
	 */
	if (mmut->kctx) {
		int new_page_count;

		new_page_count = atomic_add_return(1, &mmut->kctx->used_pages);
		KBASE_TLSTREAM_AUX_PAGESALLOC(kbdev, mmut->kctx->id, (u64)new_page_count);
		kbase_process_page_usage_inc(mmut->kctx, 1);
	}

	atomic_add(1, &kbdev->memdev.used_pages);

	kbase_trace_gpu_mem_usage_inc(kbdev, mmut->kctx, 1);

	kbdev->mmu_mode->entries_invalidate(page, KBASE_MMU_PAGE_ENTRIES * GPU_PAGES_PER_CPU_PAGE);

	/* As this page is newly created, therefore there is no content to
	 * clean or invalidate in the GPU caches.
	 */
	kbase_mmu_sync_pgd_cpu(kbdev, pgd_dma_addr(p, pgd), PAGE_SIZE);

	kbase_kunmap(p, page);
	return pgd;

alloc_free:
	kbase_mem_pool_free(&kbdev->mem_pools.small[mmut->group_id], p, false);

	return KBASE_INVALID_PHYSICAL_ADDRESS;
}

/**
 * mmu_get_next_pgd() - Given PGD PFN for level N, return PGD PFN for level N+1
 *
 * @kbdev:    Device pointer.
 * @mmut:     GPU MMU page table.
 * @pgd:      Physical addresse of level N page directory.
 * @vpfn:     The virtual page frame number, in GPU_PAGE_SIZE units.
 * @level:    The level of MMU page table (N).
 *
 * Return:
 * * 0 - OK
 * * -EFAULT - level N+1 PGD does not exist
 * * -EINVAL - kmap() failed for level N PGD PFN
 */
static int mmu_get_next_pgd(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
			    phys_addr_t *pgd, u64 vpfn, int level)
{
	u64 *page;
	phys_addr_t target_pgd;
	struct page *p;

	lockdep_assert_held(&mmut->mmu_lock);

	/*
	 * Architecture spec defines level-0 as being the top-most.
	 * This is a bit unfortunate here, but we keep the same convention.
	 */
	vpfn >>= (3 - level) * 9;
	vpfn &= 0x1FF;

	p = pfn_to_page(PFN_DOWN(*pgd));
	page = kmap_pgd(p, *pgd);
	if (page == NULL) {
		dev_err(kbdev->dev, "%s: kmap failure", __func__);
		return -EINVAL;
	}

	if (!kbdev->mmu_mode->pte_is_valid(page[vpfn], level)) {
		dev_dbg(kbdev->dev, "%s: invalid PTE at level %d vpfn 0x%llx", __func__, level,
			vpfn);
		kunmap_pgd(p, page);
		return -EFAULT;
	} else {
		target_pgd = kbdev->mmu_mode->pte_to_phy_addr(
			kbdev->mgm_dev->ops.mgm_pte_to_original_pte(
				kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, level, page[vpfn]));
	}

	kunmap_pgd(p, page);
	*pgd = target_pgd;

	return 0;
}

/**
 * mmu_get_lowest_valid_pgd() - Find a valid PGD at or closest to in_level
 *
 * @kbdev:    Device pointer.
 * @mmut:     GPU MMU page table.
 * @vpfn:     The virtual page frame number, in GPU_PAGE_SIZE units.
 * @in_level:     The level of MMU page table (N).
 * @out_level:    Set to the level of the lowest valid PGD found on success.
 *                Invalid on error.
 * @out_pgd:      Set to the lowest valid PGD found on success.
 *                Invalid on error.
 *
 * Does a page table walk starting from top level (L0) to in_level to find a valid PGD at or
 * closest to in_level
 *
 * Terminology:
 * Level-0 = Top-level = highest
 * Level-3 = Bottom-level = lowest
 *
 * Return:
 * * 0 - OK
 * * -EINVAL - kmap() failed during page table walk.
 */
static int mmu_get_lowest_valid_pgd(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				    u64 vpfn, int in_level, int *out_level, phys_addr_t *out_pgd)
{
	phys_addr_t pgd;
	int l;
	int err = 0;

	lockdep_assert_held(&mmut->mmu_lock);
	pgd = mmut->pgd;

	for (l = MIDGARD_MMU_TOPLEVEL; l < in_level; l++) {
		err = mmu_get_next_pgd(kbdev, mmut, &pgd, vpfn, l);

		/* Handle failure condition */
		if (err) {
			dev_dbg(kbdev->dev,
				"%s: mmu_get_next_pgd() failed to find a valid pgd at level %d",
				__func__, l + 1);
			break;
		}
	}

	*out_pgd = pgd;
	*out_level = l;

	/* -EFAULT indicates that pgd param was valid but the next pgd entry at vpfn was invalid.
	 * This implies that we have found the lowest valid pgd. Reset the error code.
	 */
	if (err == -EFAULT)
		err = 0;

	return err;
}
KBASE_ALLOW_ERROR_INJECTION_TEST_API(mmu_get_lowest_valid_pgd, ERRNO);

/*
 * On success, sets out_pgd to the PGD for the specified level of translation
 * Returns -EFAULT if a valid PGD is not found
 */
static int mmu_get_pgd_at_level(struct kbase_device *kbdev, struct kbase_mmu_table *mmut, u64 vpfn,
				int level, phys_addr_t *out_pgd)
{
	phys_addr_t pgd;
	int l;

	lockdep_assert_held(&mmut->mmu_lock);
	pgd = mmut->pgd;

	for (l = MIDGARD_MMU_TOPLEVEL; l < level; l++) {
		int err = mmu_get_next_pgd(kbdev, mmut, &pgd, vpfn, l);
		/* Handle failure condition */
		if (err) {
			dev_err(kbdev->dev,
				"%s: mmu_get_next_pgd() failed to find a valid pgd at level %d",
				__func__, l + 1);
			return err;
		}
	}

	*out_pgd = pgd;

	return 0;
}

static void mmu_insert_pages_failure_recovery(struct kbase_device *kbdev,
					      struct kbase_mmu_table *mmut, u64 from_vpfn,
					      u64 to_vpfn, u64 *dirty_pgds,
					      struct tagged_addr *phys, bool ignore_page_migration)
{
	u64 vpfn = from_vpfn;
	struct kbase_mmu_mode const *mmu_mode;

	/* Both from_vpfn and to_vpfn are in GPU_PAGE_SIZE units */

	/* 64-bit address range is the max */
	KBASE_DEBUG_ASSERT(vpfn <= (U64_MAX / GPU_PAGE_SIZE));
	KBASE_DEBUG_ASSERT(from_vpfn <= to_vpfn);

	lockdep_assert_held(&mmut->mmu_lock);

	mmu_mode = kbdev->mmu_mode;
	kbase_mmu_reset_free_pgds_list(mmut);

	while (vpfn < to_vpfn) {
		unsigned int idx = vpfn & 0x1FF;
		unsigned int count = KBASE_MMU_PAGE_ENTRIES - idx;
		unsigned int pcount = 0;
		unsigned int left = to_vpfn - vpfn;
		int level;
		u64 *page;
		phys_addr_t pgds[MIDGARD_MMU_BOTTOMLEVEL + 1];
		phys_addr_t pgd = mmut->pgd;
		struct page *p = phys_to_page(pgd);

		register unsigned int num_of_valid_entries;

		if (count > left)
			count = left;

		/* need to check if this is a 2MB page or a small page */
		for (level = MIDGARD_MMU_TOPLEVEL; level <= MIDGARD_MMU_BOTTOMLEVEL; level++) {
			idx = (vpfn >> ((3 - level) * 9)) & 0x1FF;
			pgds[level] = pgd;
			page = kmap_pgd(p, pgd);
			if (mmu_mode->ate_is_valid(page[idx], level))
				break; /* keep the mapping */
			kunmap_pgd(p, page);
			pgd = mmu_mode->pte_to_phy_addr(kbdev->mgm_dev->ops.mgm_pte_to_original_pte(
				kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, level, page[idx]));
			p = phys_to_page(pgd);
		}

		switch (level) {
		case MIDGARD_MMU_LEVEL(2):
			/* remap to single entry to update */
			pcount = 1;
			break;
		case MIDGARD_MMU_BOTTOMLEVEL:
			/* page count is the same as the logical count */
			pcount = count;
			break;
		default:
			dev_warn(kbdev->dev, "%sNo support for ATEs at level %d", __func__, level);
			goto next;
		}

		if (dirty_pgds && pcount > 0)
			*dirty_pgds |= 1ULL << level;

		num_of_valid_entries = mmu_mode->get_num_valid_entries(page);
		if (WARN_ON_ONCE(num_of_valid_entries < pcount))
			num_of_valid_entries = 0;
		else
			num_of_valid_entries -= pcount;

		/* Invalidate the entries we added */
		mmu_mode->entries_invalidate(&page[idx], pcount);

		if (!num_of_valid_entries) {
			mmu_mode->set_num_valid_entries(page, 0);

			kunmap_pgd(p, page);

			kbase_mmu_update_and_free_parent_pgds(kbdev, mmut, pgds, vpfn, level - 1,
							      KBASE_MMU_OP_NONE, dirty_pgds, 0);

			/* No CPU and GPU cache maintenance is done here as caller would do the
			 * complete flush of GPU cache and invalidation of TLB before the PGD
			 * page is freed. CPU cache flush would be done when the PGD page is
			 * returned to the memory pool.
			 */

			kbase_mmu_add_to_free_pgds_list(mmut, pgd);

			vpfn += count;
			continue;
		}

		mmu_mode->set_num_valid_entries(page, num_of_valid_entries);

		/* MMU cache flush strategy is NONE because GPU cache maintenance is
		 * going to be done by the caller
		 */
		kbase_mmu_sync_pgd(kbdev, mmut->kctx, pgd + (idx * sizeof(u64)),
				   pgd_dma_addr(p, pgd) + sizeof(u64) * idx, sizeof(u64) * pcount,
				   KBASE_MMU_OP_NONE);
		kunmap_pgd(p, page);
next:
		vpfn += count;
	}

	/* If page migration is enabled: the only way to recover from failure
	 * is to mark all pages as not movable. It is not predictable what's
	 * going to happen to these pages at this stage. They might return
	 * movable once they are returned to a memory pool.
	 */
	if (kbase_is_page_migration_enabled() && !ignore_page_migration && phys &&
	    !is_huge(*phys) && !is_partial(*phys)) {
		const u64 num_pages = (to_vpfn - from_vpfn) / GPU_PAGES_PER_CPU_PAGE;
		u64 i;

		for (i = 0; i < num_pages; i++) {
			struct page *phys_page = as_page(phys[i]);
			struct kbase_page_metadata *page_md = kbase_page_private(phys_page);

			if (page_md) {
				spin_lock(&page_md->migrate_lock);
				page_md->status = PAGE_STATUS_SET(page_md->status, (u8)NOT_MOVABLE);
				spin_unlock(&page_md->migrate_lock);
			}
		}
	}
}

static void mmu_flush_invalidate_insert_pages(struct kbase_device *kbdev,
					      struct kbase_mmu_table *mmut, const u64 vpfn,
					      size_t nr, u64 dirty_pgds,
					      enum kbase_caller_mmu_sync_info mmu_sync_info,
					      bool insert_pages_failed)
{
	struct kbase_mmu_hw_op_param op_param;
	int as_nr = 0;

	op_param.vpfn = vpfn;
	op_param.nr = nr;
	op_param.op = KBASE_MMU_OP_FLUSH_PT;
	op_param.mmu_sync_info = mmu_sync_info;
	op_param.kctx_id = mmut->kctx ? mmut->kctx->id : 0xFFFFFFFF;
	op_param.flush_skip_levels = pgd_level_to_skip_flush(dirty_pgds);

#if MALI_USE_CSF
	as_nr = mmut->kctx ? mmut->kctx->as_nr : MCU_AS_NR;
#else
	WARN_ON(!mmut->kctx);
#endif

	/* MMU cache flush strategy depends on whether GPU control commands for
	 * flushing physical address ranges are supported. The new physical pages
	 * are not present in GPU caches therefore they don't need any cache
	 * maintenance, but PGDs in the page table may or may not be created anew.
	 *
	 * Operations that affect the whole GPU cache shall only be done if it's
	 * impossible to update physical ranges.
	 *
	 * On GPUs where flushing by physical address range is supported,
	 * full cache flush is done when an error occurs during
	 * insert_pages() to keep the error handling simpler.
	 */
	if (mmu_flush_cache_on_gpu_ctrl(kbdev) && !insert_pages_failed)
		mmu_invalidate(kbdev, mmut->kctx, as_nr, &op_param);
	else
		mmu_flush_invalidate(kbdev, mmut->kctx, as_nr, &op_param);
}

/**
 * update_parent_pgds() - Updates the page table from bottom level towards
 *                        the top level to insert a new ATE
 *
 * @kbdev:    Device pointer.
 * @mmut:     GPU MMU page table.
 * @cur_level:    The level of MMU page table where the ATE needs to be added.
 *                The bottom PGD level.
 * @insert_level: The level of MMU page table where the chain of newly allocated
 *                PGDs needs to be linked-in/inserted.
 * @insert_vpfn:  The virtual page frame number, in GPU_PAGE_SIZE units, for the ATE.
 * @pgds_to_insert: Ptr to an array (size MIDGARD_MMU_BOTTOMLEVEL+1) that contains
 *                  the physical addresses of newly allocated PGDs from index
 *                  insert_level+1 to cur_level, and an existing PGD at index
 *                  insert_level.
 *
 * The newly allocated PGDs are linked from the bottom level up and inserted into the PGD
 * at insert_level which already exists in the MMU Page Tables. Migration status is also
 * updated for all the newly allocated PGD pages.
 *
 * Return:
 * * 0 - OK
 * * -EFAULT - level N+1 PGD does not exist
 * * -EINVAL - kmap() failed for level N PGD PFN
 */
static int update_parent_pgds(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
			      int cur_level, int insert_level, u64 insert_vpfn,
			      phys_addr_t *pgds_to_insert)
{
	int pgd_index;
	int err = 0;

	/* Add a PTE for the new PGD page at pgd_index into the parent PGD at (pgd_index-1)
	 * Loop runs from the bottom-most to the top-most level so that all entries in the chain
	 * are valid when they are inserted into the MMU Page table via the insert_level PGD.
	 */
	for (pgd_index = cur_level; pgd_index > insert_level; pgd_index--) {
		int parent_index = pgd_index - 1;
		phys_addr_t parent_pgd = pgds_to_insert[parent_index];
		unsigned int current_valid_entries;
		u64 pte;
		phys_addr_t target_pgd = pgds_to_insert[pgd_index];
		u64 parent_vpfn = (insert_vpfn >> ((3 - parent_index) * 9)) & 0x1FF;
		struct page *parent_page = pfn_to_page(PFN_DOWN(parent_pgd));
		u64 *parent_page_va;

		if (WARN_ON_ONCE(target_pgd == KBASE_INVALID_PHYSICAL_ADDRESS)) {
			err = -EFAULT;
			goto failure_recovery;
		}

		parent_page_va = kmap_pgd(parent_page, parent_pgd);

		if (unlikely(parent_page_va == NULL)) {
			dev_err(kbdev->dev, "%s: kmap failure", __func__);
			err = -EINVAL;
			goto failure_recovery;
		}

		current_valid_entries = kbdev->mmu_mode->get_num_valid_entries(parent_page_va);

		kbdev->mmu_mode->entry_set_pte(&pte, target_pgd);
		parent_page_va[parent_vpfn] = kbdev->mgm_dev->ops.mgm_update_gpu_pte(
			kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, PBHA_ID_DEFAULT, PTE_FLAGS_NONE,
			parent_index, pte);
		kbdev->mmu_mode->set_num_valid_entries(parent_page_va, current_valid_entries + 1);
		kunmap_pgd(parent_page, parent_page_va);

		if (parent_index != insert_level) {
			/* Newly allocated PGDs */
			kbase_mmu_sync_pgd_cpu(kbdev,
					       pgd_dma_addr(parent_page, parent_pgd) +
						       (parent_vpfn * sizeof(u64)),
					       sizeof(u64));
		} else {
			/* A new valid entry is added to an existing PGD. Perform the
			 * invalidate operation for GPU cache as it could be having a
			 * cacheline that contains the entry (in an invalid form).
			 */
			kbase_mmu_sync_pgd(
				kbdev, mmut->kctx, parent_pgd + (parent_vpfn * sizeof(u64)),
				pgd_dma_addr(parent_page, parent_pgd) + (parent_vpfn * sizeof(u64)),
				sizeof(u64), KBASE_MMU_OP_FLUSH_PT);
		}

		/* Update the new target_pgd page to its stable state */
		if (kbase_is_page_migration_enabled()) {
			struct kbase_page_metadata *page_md =
				kbase_page_private(phys_to_page(target_pgd));

			spin_lock(&page_md->migrate_lock);

#if GPU_PAGES_PER_CPU_PAGE > 1
			if (mmut->kctx) {
				u32 sub_page_index = get_pgd_sub_page_index(target_pgd);

				if (likely(PAGE_STATUS_GET(page_md->status) != NOT_MOVABLE)) {
					if (PAGE_STATUS_GET(page_md->status) != PT_MAPPED) {
						WARN_ON_ONCE(IS_PAGE_ISOLATED(page_md->status));
						WARN_ON_ONCE(PAGE_STATUS_GET(page_md->status) !=
							     ALLOCATE_IN_PROGRESS);

						page_md->status =
							PAGE_STATUS_SET(page_md->status, PT_MAPPED);
						page_md->data.pt_mapped.mmut = mmut;
					} else {
						WARN_ON_ONCE(page_md->data.pt_mapped.mmut != mmut);
					}

					page_md->data.pt_mapped.pgd_vpfn_level[sub_page_index] =
						PGD_VPFN_LEVEL_SET(insert_vpfn, parent_index);
				} else {
					/* First allocated PGD page gets marked as NON_MOVABLE as
					 * it stores Level 0 pgd in its first sub-page.
					 */
					WARN_ON_ONCE(!sub_page_index);
					WARN_ON_ONCE(mmut->pgd != (target_pgd & PAGE_MASK));
				}
			} else if (PAGE_STATUS_GET(page_md->status) != NOT_MOVABLE) {
				WARN_ON_ONCE(IS_PAGE_ISOLATED(page_md->status));
				WARN_ON_ONCE(PAGE_STATUS_GET(page_md->status) !=
					     ALLOCATE_IN_PROGRESS);
				page_md->status = PAGE_STATUS_SET(page_md->status, NOT_MOVABLE);
			}
#else
			WARN_ON_ONCE(PAGE_STATUS_GET(page_md->status) != ALLOCATE_IN_PROGRESS ||
				     IS_PAGE_ISOLATED(page_md->status));

			if (mmut->kctx) {
				page_md->status = PAGE_STATUS_SET(page_md->status, PT_MAPPED);
				page_md->data.pt_mapped.mmut = mmut;
				page_md->data.pt_mapped.pgd_vpfn_level[0] =
					PGD_VPFN_LEVEL_SET(insert_vpfn, parent_index);
			} else {
				page_md->status = PAGE_STATUS_SET(page_md->status, NOT_MOVABLE);
			}
#endif

			spin_unlock(&page_md->migrate_lock);
		}
	}

	return 0;

failure_recovery:
	/* Cleanup PTEs from PGDs. The Parent PGD in the loop above is just "PGD" here */
	for (; pgd_index < cur_level; pgd_index++) {
		phys_addr_t pgd = pgds_to_insert[pgd_index];
		struct page *pgd_page = pfn_to_page(PFN_DOWN(pgd));
		u64 *pgd_page_va = kmap_pgd(pgd_page, pgd);
		u64 vpfn = (insert_vpfn >> ((3 - pgd_index) * 9)) & 0x1FF;

		kbdev->mmu_mode->entries_invalidate(&pgd_page_va[vpfn], 1);
		kunmap_pgd(pgd_page, pgd_page_va);
	}

	return err;
}

/**
 * mmu_insert_alloc_pgds() - allocate memory for PGDs from level_low to
 *                           level_high (inclusive)
 *
 * @kbdev:    Device pointer.
 * @mmut:     GPU MMU page table.
 * @level_low:  The lower bound for the levels for which the PGD allocs are required
 * @level_high: The higher bound for the levels for which the PGD allocs are required
 * @new_pgds:   Ptr to an array (size MIDGARD_MMU_BOTTOMLEVEL+1) to write the
 *              newly allocated PGD addresses to.
 * @pool_grown: True if new PGDs required the memory pool to grow to allocate more pages,
 *              or false otherwise
 *
 * Numerically, level_low < level_high, not to be confused with top level and
 * bottom level concepts for MMU PGDs. They are only used as low and high bounds
 * in an incrementing for-loop.
 *
 * Return:
 * * 0 - OK
 * * -ENOMEM - allocation failed for a PGD.
 */
static int mmu_insert_alloc_pgds(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				 phys_addr_t *new_pgds, int level_low, int level_high,
				 bool *pool_grown)
{
	int err = 0;
	int i;

	lockdep_assert_held(&mmut->mmu_lock);

	*pool_grown = false;
	for (i = level_low; i <= level_high; i++) {
		if (new_pgds[i] != KBASE_INVALID_PHYSICAL_ADDRESS)
			continue;
		do {
			new_pgds[i] = kbase_mmu_alloc_pgd(kbdev, mmut);
			if (new_pgds[i] != KBASE_INVALID_PHYSICAL_ADDRESS)
				break;
			rt_mutex_unlock(&mmut->mmu_lock);
			err = kbase_mem_pool_grow(&kbdev->mem_pools.small[mmut->group_id],
						  (size_t)level_high, NULL);
			rt_mutex_lock(&mmut->mmu_lock);
			if (err) {
				dev_err(kbdev->dev, "%s: kbase_mem_pool_grow() returned error %d",
					__func__, err);
				return err;
			}
			*pool_grown = true;
		} while (1);
	}

	return 0;
}

static int kbase_mmu_insert_single_page(struct kbase_context *kctx, u64 start_vpfn,
					struct tagged_addr phys, size_t nr, unsigned long flags,
					int const group_id,
					enum kbase_caller_mmu_sync_info mmu_sync_info,
					bool ignore_page_migration)
{
	phys_addr_t pgd;
	u64 *pgd_page;
	u64 insert_vpfn = start_vpfn;
	size_t remain = nr;
	int err;
	struct kbase_device *kbdev;
	u64 dirty_pgds = 0;
	unsigned int i;
	phys_addr_t new_pgds[MIDGARD_MMU_BOTTOMLEVEL + 1];
	enum kbase_mmu_op_type flush_op;
	struct kbase_mmu_table *mmut = &kctx->mmu;
	int l, cur_level, insert_level;
	const phys_addr_t base_phys_address = as_phys_addr_t(phys);

	if (WARN_ON(kctx == NULL))
		return -EINVAL;

	lockdep_assert_held(&kctx->reg_lock);

	/* 64-bit address range is the max */
	KBASE_DEBUG_ASSERT(start_vpfn <= (U64_MAX / PAGE_SIZE));

	kbdev = kctx->kbdev;

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	/* Convert to GPU_PAGE_SIZE units. */
	insert_vpfn *= GPU_PAGES_PER_CPU_PAGE;
	remain *= GPU_PAGES_PER_CPU_PAGE;

	/* If page migration is enabled, pages involved in multiple GPU mappings
	 * are always treated as not movable.
	 */
	if (kbase_is_page_migration_enabled() && !ignore_page_migration) {
		struct page *phys_page = as_page(phys);
		struct kbase_page_metadata *page_md = kbase_page_private(phys_page);

		if (page_md) {
			spin_lock(&page_md->migrate_lock);
			page_md->status = PAGE_STATUS_SET(page_md->status, (u8)NOT_MOVABLE);
			spin_unlock(&page_md->migrate_lock);
		}
	}

	rt_mutex_lock(&mmut->mmu_lock);

	while (remain) {
		unsigned int vindex = insert_vpfn & 0x1FF;
		unsigned int count = KBASE_MMU_PAGE_ENTRIES - vindex;
		struct page *p;
		register unsigned int num_of_valid_entries;
		bool newly_created_pgd = false;
		bool pool_grown;

		if (count > remain)
			count = remain;

		cur_level = MIDGARD_MMU_BOTTOMLEVEL;
		insert_level = cur_level;

		for (l = MIDGARD_MMU_TOPLEVEL + 1; l <= cur_level; l++)
			new_pgds[l] = KBASE_INVALID_PHYSICAL_ADDRESS;

repeat_page_table_walk:
		/*
		 * Repeatedly calling mmu_get_lowest_valid_pgd() is clearly
		 * suboptimal. We don't have to re-parse the whole tree
		 * each time (just cache the l0-l2 sequence).
		 * On the other hand, it's only a gain when we map more than
		 * 256 pages at once (on average). Do we really care?
		 */
		/* insert_level < cur_level if there's no valid PGD for cur_level and insert_vpn */
		err = mmu_get_lowest_valid_pgd(kbdev, mmut, insert_vpfn, cur_level, &insert_level,
					       &pgd);

		if (err) {
			dev_err(kbdev->dev, "%s: mmu_get_lowest_valid_pgd() returned error %d",
				__func__, err);
			goto fail_unlock_free_pgds;
		}

		/* No valid pgd at cur_level */
		if (insert_level != cur_level) {
			/* Allocate new pgds for all missing levels from the required level
			 * down to the lowest valid pgd at insert_level
			 */
			err = mmu_insert_alloc_pgds(kbdev, mmut, new_pgds, (insert_level + 1),
						    cur_level, &pool_grown);
			if (err)
				goto fail_unlock_free_pgds;

			if (pool_grown)
				goto repeat_page_table_walk;

			newly_created_pgd = true;

			new_pgds[insert_level] = pgd;

			/* If we didn't find an existing valid pgd at cur_level,
			 * we've now allocated one. The ATE in the next step should
			 * be inserted in this newly allocated pgd.
			 */
			pgd = new_pgds[cur_level];
		}

		p = pfn_to_page(PFN_DOWN(pgd));

		pgd_page = kmap_pgd(p, pgd);
		if (!pgd_page) {
			dev_err(kbdev->dev, "%s: kmap failure", __func__);
			err = -ENOMEM;

			goto fail_unlock_free_pgds;
		}

		num_of_valid_entries = kbdev->mmu_mode->get_num_valid_entries(pgd_page);

		for (i = 0; i < count; i += GPU_PAGES_PER_CPU_PAGE) {
			unsigned int j;

			for (j = 0; j < GPU_PAGES_PER_CPU_PAGE; j++) {
				unsigned int ofs = vindex + i + j;
				phys_addr_t page_address = base_phys_address + (j * GPU_PAGE_SIZE);

				/* Fail if the current page is a valid ATE entry */
				WARN_ON_ONCE((pgd_page[ofs] & 1UL));
				pgd_page[ofs] = kbase_mmu_create_ate(kbdev, as_tagged(page_address),
								     flags, MIDGARD_MMU_BOTTOMLEVEL,
								     group_id);
			}
		}

		kbdev->mmu_mode->set_num_valid_entries(pgd_page, num_of_valid_entries + count);

		dirty_pgds |= 1ULL << (newly_created_pgd ? insert_level : MIDGARD_MMU_BOTTOMLEVEL);

		/* MMU cache flush operation here will depend on whether bottom level
		 * PGD is newly created or not.
		 *
		 * If bottom level PGD is newly created then no GPU cache maintenance is
		 * required as the PGD will not exist in GPU cache. Otherwise GPU cache
		 * maintenance is required for existing PGD.
		 */
		flush_op = newly_created_pgd ? KBASE_MMU_OP_NONE : KBASE_MMU_OP_FLUSH_PT;

		kbase_mmu_sync_pgd(kbdev, kctx, pgd + (vindex * sizeof(u64)),
				   pgd_dma_addr(p, pgd) + (vindex * sizeof(u64)),
				   count * sizeof(u64), flush_op);

		if (newly_created_pgd) {
			err = update_parent_pgds(kbdev, mmut, cur_level, insert_level, insert_vpfn,
						 new_pgds);
			if (err) {
				dev_err(kbdev->dev, "%s: update_parent_pgds() failed (%d)",
					__func__, err);

				kbdev->mmu_mode->entries_invalidate(&pgd_page[vindex], count);

				kunmap_pgd(p, pgd_page);
				goto fail_unlock_free_pgds;
			}
		}

		insert_vpfn += count;
		remain -= count;
		kunmap_pgd(p, pgd_page);
	}

	rt_mutex_unlock(&mmut->mmu_lock);

	mmu_flush_invalidate_insert_pages(kbdev, mmut, start_vpfn, nr, dirty_pgds, mmu_sync_info,
					  false);

	return 0;

fail_unlock_free_pgds:
	/* Free the pgds allocated by us from insert_level+1 to bottom level */
	for (l = cur_level; l > insert_level; l--)
		if (new_pgds[l] != KBASE_INVALID_PHYSICAL_ADDRESS)
			kbase_mmu_free_pgd(kbdev, mmut, new_pgds[l]);

	if (insert_vpfn != (start_vpfn * GPU_PAGES_PER_CPU_PAGE)) {
		/* Invalidate the pages we have partially completed */
		mmu_insert_pages_failure_recovery(kbdev, mmut, start_vpfn * GPU_PAGES_PER_CPU_PAGE,
						  insert_vpfn, &dirty_pgds, NULL, true);
	}

	mmu_flush_invalidate_insert_pages(kbdev, mmut, start_vpfn, nr, dirty_pgds, mmu_sync_info,
					  true);
	kbase_mmu_free_pgds_list(kbdev, mmut);
	rt_mutex_unlock(&mmut->mmu_lock);

	return err;
}

int kbase_mmu_insert_single_imported_page(struct kbase_context *kctx, u64 vpfn,
					  struct tagged_addr phys, size_t nr, unsigned long flags,
					  int const group_id,
					  enum kbase_caller_mmu_sync_info mmu_sync_info)
{
	/* The aliasing sink page has metadata and shall be moved to NOT_MOVABLE. */
	return kbase_mmu_insert_single_page(kctx, vpfn, phys, nr, flags, group_id, mmu_sync_info,
					    false);
}

int kbase_mmu_insert_single_aliased_page(struct kbase_context *kctx, u64 vpfn,
					 struct tagged_addr phys, size_t nr, unsigned long flags,
					 int const group_id,
					 enum kbase_caller_mmu_sync_info mmu_sync_info)
{
	/* The aliasing sink page has metadata and shall be moved to NOT_MOVABLE. */
	return kbase_mmu_insert_single_page(kctx, vpfn, phys, nr, flags, group_id, mmu_sync_info,
					    false);
}

static void kbase_mmu_progress_migration_on_insert(struct tagged_addr phys,
						   struct kbase_va_region *reg,
						   struct kbase_mmu_table *mmut, const u64 vpfn)
{
	struct page *phys_page = as_page(phys);
	struct kbase_page_metadata *page_md = kbase_page_private(phys_page);

	if (!kbase_is_page_migration_enabled())
		return;

	spin_lock(&page_md->migrate_lock);

	/* If no GPU va region is given: the metadata provided are
	 * invalid.
	 *
	 * If the page is already allocated and mapped: this is
	 * an additional GPU mapping, probably to create a memory
	 * alias, which means it is no longer possible to migrate
	 * the page easily because tracking all the GPU mappings
	 * would be too costly.
	 *
	 * In any case: the page becomes not movable. It is kept
	 * alive, but attempts to migrate it will fail. The page
	 * will be freed if it is still not movable when it returns
	 * to a memory pool. Notice that the movable flag is not
	 * cleared because that would require taking the page lock.
	 */
	if (!reg || PAGE_STATUS_GET(page_md->status) == (u8)ALLOCATED_MAPPED) {
		page_md->status = PAGE_STATUS_SET(page_md->status, (u8)NOT_MOVABLE);
	} else if (PAGE_STATUS_GET(page_md->status) == (u8)ALLOCATE_IN_PROGRESS) {
		page_md->status = PAGE_STATUS_SET(page_md->status, (u8)ALLOCATED_MAPPED);
		page_md->data.mapped.reg = reg;
		page_md->data.mapped.mmut = mmut;
		page_md->data.mapped.vpfn = vpfn;
	}

	spin_unlock(&page_md->migrate_lock);
}

static void kbase_mmu_progress_migration_on_teardown(struct kbase_device *kbdev,
						     struct tagged_addr *phys, size_t requested_nr)
{
	size_t i;

	if (!kbase_is_page_migration_enabled())
		return;

	for (i = 0; i < requested_nr; i++) {
		struct page *phys_page = as_page(phys[i]);
		struct kbase_page_metadata *page_md = kbase_page_private(phys_page);

		/* Skip the small page that is part of a large page, as the large page is
		 * excluded from the migration process.
		 */
		if (is_huge(phys[i]) || is_partial(phys[i]))
			continue;

		if (page_md) {
			u8 status;

			spin_lock(&page_md->migrate_lock);
			status = PAGE_STATUS_GET(page_md->status);

			if (status == ALLOCATED_MAPPED) {
				if (IS_PAGE_ISOLATED(page_md->status)) {
					page_md->status = PAGE_STATUS_SET(
						page_md->status, (u8)FREE_ISOLATED_IN_PROGRESS);
					page_md->data.free_isolated.kbdev = kbdev;
					/* At this point, we still have a reference
					 * to the page via its page migration metadata,
					 * and any page with the FREE_ISOLATED_IN_PROGRESS
					 * status will subsequently be freed in either
					 * kbase_page_migrate() or kbase_page_putback()
					 */
					phys[i] = as_tagged(KBASE_INVALID_PHYSICAL_ADDRESS);
				} else
					page_md->status = PAGE_STATUS_SET(page_md->status,
									  (u8)FREE_IN_PROGRESS);
			}

			spin_unlock(&page_md->migrate_lock);
		}
	}
}

u64 kbase_mmu_create_ate(struct kbase_device *const kbdev, struct tagged_addr const phy,
			 unsigned long const flags, int const level, int const group_id)
{
	u64 entry;
	unsigned int pte_flags = 0;

	kbdev->mmu_mode->entry_set_ate(&entry, phy, flags, level);

	if ((flags & KBASE_REG_GPU_CACHED) && !(flags & KBASE_REG_CPU_CACHED))
		pte_flags |= BIT(MMA_VIOLATION);

	return kbdev->mgm_dev->ops.mgm_update_gpu_pte(kbdev->mgm_dev, (unsigned int)group_id,
						      kbdev->mma_wa_id, pte_flags, level, entry);
}

static int mmu_insert_pages_no_flush(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				     u64 start_vpfn, struct tagged_addr *phys, size_t nr,
				     unsigned long flags, int const group_id, u64 *dirty_pgds,
				     struct kbase_va_region *reg, bool ignore_page_migration)
{
	phys_addr_t pgd;
	u64 *pgd_page;
	u64 insert_vpfn = start_vpfn;
	size_t remain = nr;
	int err;
	struct kbase_mmu_mode const *mmu_mode;
	unsigned int i;
	phys_addr_t new_pgds[MIDGARD_MMU_BOTTOMLEVEL + 1];
	int l, cur_level, insert_level;
	struct tagged_addr *start_phys = phys;

	if (mmut->kctx)
		lockdep_assert_held(&mmut->kctx->reg_lock);

	/* Note that 0 is a valid start_vpfn */
	/* 64-bit address range is the max */
	KBASE_DEBUG_ASSERT(start_vpfn <= (U64_MAX / PAGE_SIZE));

	mmu_mode = kbdev->mmu_mode;

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	/* Convert to GPU_PAGE_SIZE units. */
	insert_vpfn *= GPU_PAGES_PER_CPU_PAGE;
	remain *= GPU_PAGES_PER_CPU_PAGE;
	rt_mutex_lock(&mmut->mmu_lock);

	while (remain) {
		unsigned int vindex = insert_vpfn & 0x1FF;
		unsigned int count = KBASE_MMU_PAGE_ENTRIES - vindex;
		struct page *p;
		register unsigned int num_of_valid_entries;
		bool newly_created_pgd = false;
		enum kbase_mmu_op_type flush_op;
		bool pool_grown;

		if (count > remain)
			count = remain;

		/* There are 3 conditions to satisfy in order to create a level 2 ATE:
		 *
		 * - The GPU VA is aligned to 2 MB.
		 * - The physical address is tagged as the head of a 2 MB region,
		 *   which guarantees a contiguous physical address range.
		 * - There are actually 2 MB of virtual and physical pages to map,
		 *   i.e. 512 entries for the MMU page table.
		 */
		if (!vindex && is_huge_head(*phys) && (count == KBASE_MMU_PAGE_ENTRIES))
			cur_level = MIDGARD_MMU_LEVEL(2);
		else
			cur_level = MIDGARD_MMU_BOTTOMLEVEL;

		insert_level = cur_level;

		for (l = MIDGARD_MMU_TOPLEVEL + 1; l <= cur_level; l++)
			new_pgds[l] = KBASE_INVALID_PHYSICAL_ADDRESS;

repeat_page_table_walk:
		/*
		 * Repeatedly calling mmu_get_lowest_valid_pgd() is clearly
		 * suboptimal. We don't have to re-parse the whole tree
		 * each time (just cache the l0-l2 sequence).
		 * On the other hand, it's only a gain when we map more than
		 * 256 pages at once (on average). Do we really care?
		 */
		/* insert_level < cur_level if there's no valid PGD for cur_level and insert_vpn */
		err = mmu_get_lowest_valid_pgd(kbdev, mmut, insert_vpfn, cur_level, &insert_level,
					       &pgd);

		if (err) {
			dev_err(kbdev->dev, "%s: mmu_get_lowest_valid_pgd() returned error %d",
				__func__, err);
			goto fail_unlock_free_pgds;
		}

		/* No valid pgd at cur_level */
		if (insert_level != cur_level) {
			/* Allocate new pgds for all missing levels from the required level
			 * down to the lowest valid pgd at insert_level
			 */
			err = mmu_insert_alloc_pgds(kbdev, mmut, new_pgds, (insert_level + 1),
						    cur_level, &pool_grown);
			if (err)
				goto fail_unlock_free_pgds;

			if (pool_grown)
				goto repeat_page_table_walk;

			newly_created_pgd = true;

			new_pgds[insert_level] = pgd;

			/* If we didn't find an existing valid pgd at cur_level,
			 * we've now allocated one. The ATE in the next step should
			 * be inserted in this newly allocated pgd.
			 */
			pgd = new_pgds[cur_level];
		}

		p = pfn_to_page(PFN_DOWN(pgd));
		pgd_page = kmap_pgd(p, pgd);

		if (!pgd_page) {
			dev_err(kbdev->dev, "%s: kmap failure", __func__);
			err = -ENOMEM;

			goto fail_unlock_free_pgds;
		}

		num_of_valid_entries = mmu_mode->get_num_valid_entries(pgd_page);

		if (cur_level == MIDGARD_MMU_LEVEL(2)) {
			int level_index = (insert_vpfn >> 9) & 0x1FF;
			pgd_page[level_index] =
				kbase_mmu_create_ate(kbdev, *phys, flags, cur_level, group_id);

			num_of_valid_entries++;
		} else {
			for (i = 0; i < count; i += GPU_PAGES_PER_CPU_PAGE) {
				struct tagged_addr base_tagged_addr =
					phys[i / GPU_PAGES_PER_CPU_PAGE];
				phys_addr_t base_phys_address = as_phys_addr_t(base_tagged_addr);
				unsigned int j;

				for (j = 0; j < GPU_PAGES_PER_CPU_PAGE; j++) {
					unsigned int ofs = vindex + i + j;
					u64 *target = &pgd_page[ofs];
					phys_addr_t page_address =
						base_phys_address + (j * GPU_PAGE_SIZE);

					/* Warn if the current page is a valid ATE
					 * entry. The page table shouldn't have anything
					 * in the place where we are trying to put a
					 * new entry. Modification to page table entries
					 * should be performed with
					 * kbase_mmu_update_pages()
					 */
					WARN_ON_ONCE((*target & 1UL) != 0);

					*target = kbase_mmu_create_ate(kbdev,
								       as_tagged(page_address),
								       flags, cur_level, group_id);
				}

				/* If page migration is enabled, this is the right time
				 * to update the status of the page.
				 */
				if (kbase_is_page_migration_enabled() && !ignore_page_migration &&
				    !is_huge(base_tagged_addr) && !is_partial(base_tagged_addr))
					kbase_mmu_progress_migration_on_insert(
						base_tagged_addr, reg, mmut, insert_vpfn + i);
			}
			num_of_valid_entries += count;
		}

		mmu_mode->set_num_valid_entries(pgd_page, num_of_valid_entries);

		if (dirty_pgds)
			*dirty_pgds |= 1ULL << (newly_created_pgd ? insert_level : cur_level);

		/* MMU cache flush operation here will depend on whether bottom level
		 * PGD is newly created or not.
		 *
		 * If bottom level PGD is newly created then no GPU cache maintenance is
		 * required as the PGD will not exist in GPU cache. Otherwise GPU cache
		 * maintenance is required for existing PGD.
		 */
		flush_op = newly_created_pgd ? KBASE_MMU_OP_NONE : KBASE_MMU_OP_FLUSH_PT;

		kbase_mmu_sync_pgd(kbdev, mmut->kctx, pgd + (vindex * sizeof(u64)),
				   pgd_dma_addr(p, pgd) + (vindex * sizeof(u64)),
				   count * sizeof(u64), flush_op);

		if (newly_created_pgd) {
			err = update_parent_pgds(kbdev, mmut, cur_level, insert_level, insert_vpfn,
						 new_pgds);
			if (err) {
				dev_err(kbdev->dev, "%s: update_parent_pgds() failed (%d)",
					__func__, err);

				kbdev->mmu_mode->entries_invalidate(&pgd_page[vindex], count);

				kunmap_pgd(p, pgd_page);
				goto fail_unlock_free_pgds;
			}
		}

		phys += (count / GPU_PAGES_PER_CPU_PAGE);
		insert_vpfn += count;
		remain -= count;
		kunmap_pgd(p, pgd_page);
	}

	rt_mutex_unlock(&mmut->mmu_lock);

	return 0;

fail_unlock_free_pgds:
	/* Free the pgds allocated by us from insert_level+1 to bottom level */
	for (l = cur_level; l > insert_level; l--)
		if (new_pgds[l] != KBASE_INVALID_PHYSICAL_ADDRESS)
			kbase_mmu_free_pgd(kbdev, mmut, new_pgds[l]);

	if (insert_vpfn != (start_vpfn * GPU_PAGES_PER_CPU_PAGE)) {
		/* Invalidate the pages we have partially completed */
		mmu_insert_pages_failure_recovery(kbdev, mmut, start_vpfn * GPU_PAGES_PER_CPU_PAGE,
						  insert_vpfn, dirty_pgds, start_phys,
						  ignore_page_migration);
	}

	mmu_flush_invalidate_insert_pages(kbdev, mmut, start_vpfn, nr,
					  dirty_pgds ? *dirty_pgds : 0xF, CALLER_MMU_ASYNC, true);
	kbase_mmu_free_pgds_list(kbdev, mmut);
	rt_mutex_unlock(&mmut->mmu_lock);

	return err;
}

int kbase_mmu_insert_pages_no_flush(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				    const u64 start_vpfn, struct tagged_addr *phys, size_t nr,
				    unsigned long flags, int const group_id, u64 *dirty_pgds,
				    struct kbase_va_region *reg)
{
	int err;

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	err = mmu_insert_pages_no_flush(kbdev, mmut, start_vpfn, phys, nr, flags, group_id,
					dirty_pgds, reg, false);

	return err;
}

/*
 * Map 'nr' pages pointed to by 'phys' at GPU PFN 'vpfn' for GPU address space
 * number 'as_nr'.
 */
int kbase_mmu_insert_pages(struct kbase_device *kbdev, struct kbase_mmu_table *mmut, u64 vpfn,
			   struct tagged_addr *phys, size_t nr, unsigned long flags, int as_nr,
			   int const group_id, enum kbase_caller_mmu_sync_info mmu_sync_info,
			   struct kbase_va_region *reg)
{
	int err;
	u64 dirty_pgds = 0;

	CSTD_UNUSED(as_nr);

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	err = mmu_insert_pages_no_flush(kbdev, mmut, vpfn, phys, nr, flags, group_id, &dirty_pgds,
					reg, false);
	if (err)
		return err;

	mmu_flush_invalidate_insert_pages(kbdev, mmut, vpfn, nr, dirty_pgds, mmu_sync_info, false);

	return 0;
}

KBASE_EXPORT_TEST_API(kbase_mmu_insert_pages);
KBASE_ALLOW_ERROR_INJECTION_TEST_API(kbase_mmu_insert_pages, ERRNO);

int kbase_mmu_insert_pages_skip_status_update(struct kbase_device *kbdev,
					      struct kbase_mmu_table *mmut, u64 vpfn,
					      struct tagged_addr *phys, size_t nr,
					      unsigned long flags, int as_nr, int const group_id,
					      enum kbase_caller_mmu_sync_info mmu_sync_info,
					      struct kbase_va_region *reg)
{
	int err;
	u64 dirty_pgds = 0;

	CSTD_UNUSED(as_nr);

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	/* Imported allocations don't have metadata and therefore always ignore the
	 * page migration logic.
	 */
	err = mmu_insert_pages_no_flush(kbdev, mmut, vpfn, phys, nr, flags, group_id, &dirty_pgds,
					reg, true);
	if (err)
		return err;

	mmu_flush_invalidate_insert_pages(kbdev, mmut, vpfn, nr, dirty_pgds, mmu_sync_info, false);

	return 0;
}

int kbase_mmu_insert_aliased_pages(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				   u64 vpfn, struct tagged_addr *phys, size_t nr,
				   unsigned long flags, int as_nr, int const group_id,
				   enum kbase_caller_mmu_sync_info mmu_sync_info,
				   struct kbase_va_region *reg)
{
	int err;
	u64 dirty_pgds = 0;

	CSTD_UNUSED(as_nr);

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	/* Memory aliases are always built on top of existing allocations,
	 * therefore the state of physical pages shall be updated.
	 */
	err = mmu_insert_pages_no_flush(kbdev, mmut, vpfn, phys, nr, flags, group_id, &dirty_pgds,
					reg, false);
	if (err)
		return err;

	mmu_flush_invalidate_insert_pages(kbdev, mmut, vpfn, nr, dirty_pgds, mmu_sync_info, false);

	return 0;
}
KBASE_ALLOW_ERROR_INJECTION_TEST_API(kbase_mmu_insert_aliased_pages, ERRNO);

#if !MALI_USE_CSF
/**
 * kbase_mmu_flush_noretain() - Flush and invalidate the GPU caches
 * without retaining the kbase context.
 * @kctx: The KBase context.
 * @vpfn: The virtual page frame number to start the flush on.
 * @nr: The number of pages to flush.
 *
 * As per kbase_mmu_flush_invalidate but doesn't retain the kctx or do any
 * other locking.
 */
static void kbase_mmu_flush_noretain(struct kbase_context *kctx, u64 vpfn, size_t nr)
{
	struct kbase_device *kbdev = kctx->kbdev;
	int err;
	/* Calls to this function are inherently asynchronous, with respect to
	 * MMU operations.
	 */
	const enum kbase_caller_mmu_sync_info mmu_sync_info = CALLER_MMU_ASYNC;
	struct kbase_mmu_hw_op_param op_param;

	lockdep_assert_held(&kctx->kbdev->hwaccess_lock);
	lockdep_assert_held(&kctx->kbdev->mmu_hw_mutex);

	/* Early out if there is nothing to do */
	if (nr == 0)
		return;

	/* flush L2 and unlock the VA (resumes the MMU) */
	op_param.vpfn = vpfn;
	op_param.nr = nr;
	op_param.op = KBASE_MMU_OP_FLUSH_MEM;
	op_param.kctx_id = kctx->id;
	op_param.mmu_sync_info = mmu_sync_info;
	if (mmu_flush_cache_on_gpu_ctrl(kbdev)) {
		/* Value used to prevent skipping of any levels when flushing */
		op_param.flush_skip_levels = pgd_level_to_skip_flush(0xF);
		err = kbase_mmu_hw_do_flush_on_gpu_ctrl(kbdev, &kbdev->as[kctx->as_nr],
							&op_param);
	} else {
		err = kbase_mmu_hw_do_flush(kbdev, &kbdev->as[kctx->as_nr],
						   &op_param);
	}

	if (err) {
		/* Flush failed to complete, assume the
		 * GPU has hung and perform a reset to recover
		 */
		dev_err(kbdev->dev, "Flush for GPU page table update did not complete. Issuing GPU soft-reset to recover");

		if (kbase_prepare_to_reset_gpu_locked(kbdev, RESET_FLAGS_NONE))
			kbase_reset_gpu_locked(kbdev);
	}
}
#endif

void kbase_mmu_update(struct kbase_device *kbdev, struct kbase_mmu_table *mmut, int as_nr)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);
	lockdep_assert_held(&kbdev->mmu_hw_mutex);
	KBASE_DEBUG_ASSERT(as_nr != KBASEP_AS_NR_INVALID);

	kbdev->mmu_mode->update(kbdev, mmut, as_nr);
}
KBASE_EXPORT_TEST_API(kbase_mmu_update);

void kbase_mmu_disable_as(struct kbase_device *kbdev, int as_nr)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);
#if !MALI_USE_CSF
	lockdep_assert_held(&kbdev->mmu_hw_mutex);
#endif

	kbdev->mmu_mode->disable_as(kbdev, as_nr);
}

#if MALI_USE_CSF
void kbase_mmu_disable(struct kbase_context *kctx)
{
	/* Calls to this function are inherently asynchronous, with respect to
	 * MMU operations.
	 */
	const enum kbase_caller_mmu_sync_info mmu_sync_info = CALLER_MMU_ASYNC;
	struct kbase_device *kbdev = kctx->kbdev;
	struct kbase_mmu_hw_op_param op_param = { 0 };
	int lock_err, flush_err;

	/* Assert that the context has a valid as_nr, which is only the case
	 * when it's scheduled in. as_nr can be invalid, for example, when
	 * the ctx was descheduled while the MMU fault IRQ handling was
	 * pending.
	 *
	 * as_nr won't change because the caller has the hwaccess_lock.
	 */
	if (kctx->as_nr == KBASEP_AS_NR_INVALID) {
		dev_dbg(kbdev->dev, "Invalid as_nr for ctx %d_%d", kctx->tgid, kctx->id);
		return;
	}

	lockdep_assert_held(&kctx->kbdev->hwaccess_lock);

	op_param.vpfn = 0;
	op_param.nr = ~0U;
	op_param.op = KBASE_MMU_OP_FLUSH_MEM;
	op_param.kctx_id = kctx->id;
	op_param.mmu_sync_info = mmu_sync_info;

#if MALI_USE_CSF
	/* 0xF value used to prevent skipping of any levels when flushing */
	if (mmu_flush_cache_on_gpu_ctrl(kbdev))
		op_param.flush_skip_levels = pgd_level_to_skip_flush(0xF);
#endif
	/* lock MMU to prevent existing jobs on GPU from executing while the AS is
	 * not yet disabled
	 */
	lock_err = kbase_mmu_hw_do_lock(kbdev, &kbdev->as[kctx->as_nr], &op_param);
	if (lock_err)
		dev_err(kbdev->dev, "Failed to lock AS %d for ctx %d_%d", kctx->as_nr, kctx->tgid,
			kctx->id);

	/* Issue the flush command only when L2 cache is in stable power on state.
	 * Any other state for L2 cache implies that shader cores are powered off,
	 * which in turn implies there is no execution happening on the GPU.
	 */
	if (kbdev->pm.backend.l2_state == KBASE_L2_ON) {
		flush_err = kbase_gpu_cache_flush_and_busy_wait(kbdev,
								GPU_COMMAND_CACHE_CLN_INV_L2_LSC);
		if (flush_err)
			dev_err(kbdev->dev,
				"Failed to flush GPU cache when disabling AS %d for ctx %d_%d",
				kctx->as_nr, kctx->tgid, kctx->id);
	}
	kbdev->mmu_mode->disable_as(kbdev, kctx->as_nr);

	if (!lock_err) {
		/* unlock the MMU to allow it to resume */
		lock_err =
			kbase_mmu_hw_do_unlock_no_addr(kbdev, &kbdev->as[kctx->as_nr], &op_param);
		if (lock_err)
			dev_err(kbdev->dev, "Failed to unlock AS %d for ctx %d_%d", kctx->as_nr,
				kctx->tgid, kctx->id);
	}

#if !MALI_USE_CSF
	/*
	 * JM GPUs has some L1 read only caches that need to be invalidated
	 * with START_FLUSH configuration. Purge the MMU disabled kctx from
	 * the slot_rb tracking field so such invalidation is performed when
	 * a new katom is executed on the affected slots.
	 */
	kbase_backend_slot_kctx_purge_locked(kbdev, kctx);
#endif

	/* kbase_gpu_cache_flush_and_busy_wait() will reset the GPU on timeout. Only
	 * reset the GPU if locking or unlocking fails.
	 */
	if (lock_err)
		if (kbase_prepare_to_reset_gpu_locked(kbdev, RESET_FLAGS_NONE))
			kbase_reset_gpu_locked(kbdev);
}
#else
void kbase_mmu_disable(struct kbase_context *kctx)
{
	/* ASSERT that the context has a valid as_nr, which is only the case
	 * when it's scheduled in.
	 *
	 * as_nr won't change because the caller has the hwaccess_lock
	 */
	KBASE_DEBUG_ASSERT(kctx->as_nr != KBASEP_AS_NR_INVALID);

	lockdep_assert_held(&kctx->kbdev->hwaccess_lock);
	lockdep_assert_held(&kctx->kbdev->mmu_hw_mutex);

	/*
	 * The address space is being disabled, drain all knowledge of it out
	 * from the caches as pages and page tables might be freed after this.
	 *
	 * The job scheduler code will already be holding the locks and context
	 * so just do the flush.
	 */
	kbase_mmu_flush_noretain(kctx, 0, ~0);

	kctx->kbdev->mmu_mode->disable_as(kctx->kbdev, kctx->as_nr);
#if !MALI_USE_CSF
	/*
	 * JM GPUs has some L1 read only caches that need to be invalidated
	 * with START_FLUSH configuration. Purge the MMU disabled kctx from
	 * the slot_rb tracking field so such invalidation is performed when
	 * a new katom is executed on the affected slots.
	 */
	kbase_backend_slot_kctx_purge_locked(kctx->kbdev, kctx);
#endif
}
#endif
KBASE_EXPORT_TEST_API(kbase_mmu_disable);

static void kbase_mmu_update_and_free_parent_pgds(struct kbase_device *kbdev,
						  struct kbase_mmu_table *mmut, phys_addr_t *pgds,
						  u64 vpfn, int level,
						  enum kbase_mmu_op_type flush_op, u64 *dirty_pgds,
						  int as_nr)
{
	phys_addr_t current_pgd = pgds[level];
	struct page *p = phys_to_page(current_pgd);
	u64 *current_page = kmap_pgd(p, current_pgd);
	unsigned int current_valid_entries = kbdev->mmu_mode->get_num_valid_entries(current_page);
	unsigned int index = (vpfn >> ((3 - level) * 9)) & 0x1FFU;

	lockdep_assert_held(&mmut->mmu_lock);

	/* We need to track every level that needs updating */
	if (dirty_pgds)
		*dirty_pgds |= 1ULL << level;

	kbdev->mmu_mode->entries_invalidate(&current_page[index], 1);
	if (current_valid_entries == 1 && level != MIDGARD_MMU_LEVEL(0)) {
		kbdev->mmu_mode->set_num_valid_entries(current_page, 0);

		kunmap_pgd(p, current_page);

		kbase_mmu_update_and_free_parent_pgds(kbdev, mmut, pgds, vpfn, level - 1, flush_op,
						      dirty_pgds, as_nr);

		/* Check if fine grained GPU cache maintenance is being used */
		if (flush_op == KBASE_MMU_OP_FLUSH_PT) {
			/* Ensure the invalidated PTE is visible in memory right away */
			kbase_mmu_sync_pgd_cpu(kbdev,
					       pgd_dma_addr(p, current_pgd) + (index * sizeof(u64)),
					       sizeof(u64));
			/* Invalidate the GPU cache for the whole PGD page and not just for
			 * the cacheline containing the invalidated PTE, as the PGD page is
			 * going to be freed. There is an extremely remote possibility that
			 * other cachelines (containing all invalid PTEs) of PGD page are
			 * also present in the GPU cache.
			 */
			kbase_mmu_sync_pgd_gpu(kbdev, mmut->kctx, current_pgd, 512 * sizeof(u64),
					       KBASE_MMU_OP_FLUSH_PT);
		}

		kbase_mmu_add_to_free_pgds_list(mmut, current_pgd);
	} else {
		current_valid_entries--;

		kbdev->mmu_mode->set_num_valid_entries(current_page, current_valid_entries);

		kunmap_pgd(p, current_page);

		kbase_mmu_sync_pgd(kbdev, mmut->kctx, current_pgd + (index * sizeof(u64)),
				   pgd_dma_addr(p, current_pgd) + (index * sizeof(u64)),
				   sizeof(u64), flush_op);

		/* When fine grained GPU cache maintenance is used then invalidate the MMU caches
		 * now as the top most level PGD entry, affected by the teardown operation, has
		 * been invalidated (both in memory as well as in GPU L2 cache). This is to avoid
		 * the possibility of invalid ATEs being reloaded into the GPU L2 cache whilst the
		 * teardown is happening.
		 */
		if (flush_op == KBASE_MMU_OP_FLUSH_PT)
			mmu_invalidate_on_teardown(kbdev, mmut->kctx, vpfn, 1, level, as_nr);
	}
}

/**
 * mmu_flush_invalidate_teardown_pages() - Perform flush operation after unmapping pages.
 *
 * @kbdev:         Pointer to kbase device.
 * @kctx:          Pointer to kbase context.
 * @as_nr:         Address space number, for GPU cache maintenance operations
 *                 that happen outside a specific kbase context.
 * @phys:          Array of physical pages to flush.
 * @phys_page_nr:  Number of physical pages to flush.
 * @op_param:      Non-NULL pointer to struct containing information about the flush
 *                 operation to perform.
 *
 * This function will do one of three things:
 * 1. Invalidate the MMU caches, followed by a partial GPU cache flush of the
 *    individual pages that were unmapped if feature is supported on GPU.
 * 2. Perform a full GPU cache flush through the GPU_CONTROL interface if feature is
 *    supported on GPU or,
 * 3. Perform a full GPU cache flush through the MMU_CONTROL interface.
 *
 * When performing a partial GPU cache flush, the number of physical
 * pages does not have to be identical to the number of virtual pages on the MMU,
 * to support a single physical address flush for an aliased page.
 */
static void mmu_flush_invalidate_teardown_pages(struct kbase_device *kbdev,
						struct kbase_context *kctx, int as_nr,
						struct tagged_addr *phys, size_t phys_page_nr,
						struct kbase_mmu_hw_op_param *op_param)
{
	if (!mmu_flush_cache_on_gpu_ctrl(kbdev)) {
		/* Full cache flush through the MMU_COMMAND */
		mmu_flush_invalidate(kbdev, kctx, as_nr, op_param);
	} else if (op_param->op == KBASE_MMU_OP_FLUSH_MEM) {
		/* Full cache flush through the GPU_CONTROL */
		mmu_flush_invalidate_on_gpu_ctrl(kbdev, kctx, as_nr, op_param);
	}
#if MALI_USE_CSF
	else {
		/* Partial GPU cache flush of the pages that were unmapped */
		unsigned long irq_flags;
		unsigned int i;
		bool flush_done = false;

		for (i = 0; !flush_done && i < phys_page_nr; i++) {
			spin_lock_irqsave(&kbdev->hwaccess_lock, irq_flags);
			if (kbdev->pm.backend.gpu_ready && (!kctx || kctx->as_nr >= 0))
				mmu_flush_pa_range(kbdev, as_phys_addr_t(phys[i]), PAGE_SIZE,
						   KBASE_MMU_OP_FLUSH_MEM);
			else
				flush_done = true;
			spin_unlock_irqrestore(&kbdev->hwaccess_lock, irq_flags);
		}
	}
#else
	CSTD_UNUSED(phys);
	CSTD_UNUSED(phys_page_nr);
#endif
}

static int kbase_mmu_teardown_pgd_pages(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
					u64 vpfn, size_t nr, u64 *dirty_pgds,
					struct list_head *free_pgds_list,
					enum kbase_mmu_op_type flush_op, int as_nr)
{
	struct kbase_mmu_mode const *mmu_mode = kbdev->mmu_mode;

	CSTD_UNUSED(free_pgds_list);

	lockdep_assert_held(&mmut->mmu_lock);
	kbase_mmu_reset_free_pgds_list(mmut);
	/* Convert to GPU_PAGE_SIZE units. */
	vpfn *= GPU_PAGES_PER_CPU_PAGE;
	nr *= GPU_PAGES_PER_CPU_PAGE;

	while (nr) {
		unsigned int index = vpfn & 0x1FF;
		unsigned int count = KBASE_MMU_PAGE_ENTRIES - index;
		unsigned int pcount;
		int level;
		u64 *page;
		phys_addr_t pgds[MIDGARD_MMU_BOTTOMLEVEL + 1];
		register unsigned int num_of_valid_entries;
		phys_addr_t pgd = mmut->pgd;
		struct page *p = phys_to_page(pgd);

		count = MIN(nr, count);

		/* need to check if this is a 2MB page or a small page */
		for (level = MIDGARD_MMU_TOPLEVEL; level <= MIDGARD_MMU_BOTTOMLEVEL; level++) {
			phys_addr_t next_pgd;

			index = (vpfn >> ((3 - level) * 9)) & 0x1FF;
			page = kmap_pgd(p, pgd);
			if (mmu_mode->ate_is_valid(page[index], level))
				break; /* keep the mapping */
			else if (!mmu_mode->pte_is_valid(page[index], level)) {
				dev_warn(kbdev->dev, "Invalid PTE found @ level %d for VA %llx",
					 level, vpfn << PAGE_SHIFT);
				/* nothing here, advance to the next PTE of the current level */
				count = (1 << ((3 - level) * 9));
				count -= (vpfn & (count - 1));
				count = MIN(nr, count);
				goto next;
			}
			next_pgd = mmu_mode->pte_to_phy_addr(
				kbdev->mgm_dev->ops.mgm_pte_to_original_pte(
					kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, level, page[index]));
			kunmap_pgd(p, page);
			pgds[level] = pgd;
			pgd = next_pgd;
			p = phys_to_page(pgd);
		}

		switch (level) {
		case MIDGARD_MMU_LEVEL(0):
		case MIDGARD_MMU_LEVEL(1):
			dev_warn(kbdev->dev, "%s: No support for ATEs at level %d", __func__,
				 level);
			kunmap_pgd(p, page);
			goto out;
		case MIDGARD_MMU_LEVEL(2):
			/* can only teardown if count >= 512 */
			if (count >= 512) {
				pcount = 1;
			} else {
				dev_warn(
					kbdev->dev,
					"%s: limiting teardown as it tries to do a partial 2MB teardown, need 512, but have %d to tear down",
					__func__, count);
				pcount = 0;
			}
			break;
		case MIDGARD_MMU_BOTTOMLEVEL:
			/* page count is the same as the logical count */
			pcount = count;
			break;
		default:
			dev_err(kbdev->dev, "%s: found non-mapped memory, early out", __func__);
			vpfn += count;
			nr -= count;
			continue;
		}

		if (pcount > 0)
			*dirty_pgds |= 1ULL << level;

		num_of_valid_entries = mmu_mode->get_num_valid_entries(page);
		if (WARN_ON_ONCE(num_of_valid_entries < pcount))
			num_of_valid_entries = 0;
		else
			num_of_valid_entries -= pcount;

		/* Invalidate the entries we added */
		mmu_mode->entries_invalidate(&page[index], pcount);

		if (!num_of_valid_entries) {
			mmu_mode->set_num_valid_entries(page, 0);

			kunmap_pgd(p, page);

			/* To avoid the invalid ATEs from the PGD page (that is going to be freed)
			 * from getting reloaded into the GPU L2 cache whilst the teardown is
			 * happening, the fine grained GPU L2 cache maintenance is done in the top
			 * to bottom level PGD order. MMU cache invalidation is done after
			 * invalidating the entry of top most level PGD, affected by the teardown.
			 */
			kbase_mmu_update_and_free_parent_pgds(kbdev, mmut, pgds, vpfn, level - 1,
							      flush_op, dirty_pgds, as_nr);

			/* Check if fine grained GPU cache maintenance is being used */
			if (flush_op == KBASE_MMU_OP_FLUSH_PT) {
				/* Ensure the invalidated ATEs are visible in memory right away */
				kbase_mmu_sync_pgd_cpu(kbdev,
						       pgd_dma_addr(p, pgd) + (index * sizeof(u64)),
						       pcount * sizeof(u64));
				/* Invalidate the GPU cache for the whole PGD page and not just for
				 * the cachelines containing the invalidated ATEs, as the PGD page
				 * is going to be freed. There is an extremely remote possibility
				 * that other cachelines (containing all invalid ATEs) of PGD page
				 * are also present in the GPU cache.
				 */
				kbase_mmu_sync_pgd_gpu(kbdev, mmut->kctx, pgd, 512 * sizeof(u64),
						       KBASE_MMU_OP_FLUSH_PT);
			}

			kbase_mmu_add_to_free_pgds_list(mmut, pgd);

			vpfn += count;
			nr -= count;
			continue;
		}

		mmu_mode->set_num_valid_entries(page, num_of_valid_entries);

		kbase_mmu_sync_pgd(kbdev, mmut->kctx, pgd + (index * sizeof(u64)),
				   pgd_dma_addr(p, pgd) + (index * sizeof(u64)),
				   pcount * sizeof(u64), flush_op);

		/* When fine grained GPU cache maintenance is used then invalidation of MMU cache
		 * is done inline for every bottom level PGD touched in the teardown.
		 */
		if (flush_op == KBASE_MMU_OP_FLUSH_PT)
			mmu_invalidate_on_teardown(kbdev, mmut->kctx, vpfn, pcount, level, as_nr);
next:
		kunmap_pgd(p, page);
		vpfn += count;
		nr -= count;
	}
out:
	return 0;
}

/**
 * mmu_teardown_pages - Remove GPU virtual addresses from the MMU page table
 *
 * @kbdev:    Pointer to kbase device.
 * @mmut:     Pointer to GPU MMU page table.
 * @vpfn:     Start page frame number (in PAGE_SIZE units) of the GPU virtual pages to unmap.
 * @phys:     Array of physical pages currently mapped to the virtual
 *            pages to unmap, or NULL. This is used for GPU cache maintenance
 *            and page migration support.
 * @nr_phys_pages: Number of physical pages (in PAGE_SIZE units) to flush.
 * @nr_virt_pages: Number of virtual pages (in PAGE_SIZE units) whose PTEs should be destroyed.
 * @as_nr:    Address space number, for GPU cache maintenance operations
 *            that happen outside a specific kbase context.
 * @ignore_page_migration: Whether page migration metadata should be ignored.
 *
 * We actually discard the ATE and free the page table pages if no valid entries
 * exist in the PGD.
 *
 * IMPORTANT: This uses kbasep_js_runpool_release_ctx() when the context is
 * currently scheduled into the runpool, and so potentially uses a lot of locks.
 * These locks must be taken in the correct order with respect to others
 * already held by the caller. Refer to kbasep_js_runpool_release_ctx() for more
 * information.
 *
 * The @p phys pointer to physical pages is not necessary for unmapping virtual memory,
 * but it is used for fine-grained GPU cache maintenance. If @p phys is NULL,
 * GPU cache maintenance will be done as usual; that is, invalidating the whole GPU caches
 * instead of specific physical address ranges.
 *
 * Return: 0 on success, otherwise an error code.
 */
static int mmu_teardown_pages(struct kbase_device *kbdev, struct kbase_mmu_table *mmut, u64 vpfn,
			      struct tagged_addr *phys, size_t nr_phys_pages, size_t nr_virt_pages,
			      int as_nr, bool ignore_page_migration)
{
	u64 start_vpfn = vpfn;
	enum kbase_mmu_op_type flush_op = KBASE_MMU_OP_NONE;
	struct kbase_mmu_hw_op_param op_param;
	int err = -EFAULT;
	u64 dirty_pgds = 0;
	LIST_HEAD(free_pgds_list);

	/* Calls to this function are inherently asynchronous, with respect to
	 * MMU operations.
	 */
	const enum kbase_caller_mmu_sync_info mmu_sync_info = CALLER_MMU_ASYNC;

	/* This function performs two operations: MMU maintenance and flushing
	 * the caches. To ensure internal consistency between the caches and the
	 * MMU, it does not make sense to be able to flush only the physical pages
	 * from the cache and keep the PTE, nor does it make sense to use this
	 * function to remove a PTE and keep the physical pages in the cache.
	 *
	 * However, we have legitimate cases where we can try to tear down a mapping
	 * with zero virtual and zero physical pages, so we must have the following
	 * behaviour:
	 *  - if both physical and virtual page counts are zero, return early
	 *  - if either physical and virtual page counts are zero, return early
	 *  - if there are fewer physical pages than virtual pages, return -EINVAL
	 */
	if (unlikely(nr_virt_pages == 0 || nr_phys_pages == 0))
		return 0;

	if (unlikely(nr_virt_pages < nr_phys_pages))
		return -EINVAL;

	/* MMU cache flush strategy depends on the number of pages to unmap. In both cases
	 * the operation is invalidate but the granularity of cache maintenance may change
	 * according to the situation.
	 *
	 * If GPU control command operations are present and the number of pages is "small",
	 * then the optimal strategy is flushing on the physical address range of the pages
	 * which are affected by the operation. That implies both the PGDs which are modified
	 * or removed from the page table and the physical pages which are freed from memory.
	 *
	 * Otherwise, there's no alternative to invalidating the whole GPU cache.
	 */
	if (mmu_flush_cache_on_gpu_ctrl(kbdev) && phys &&
	    nr_phys_pages <= KBASE_PA_RANGE_THRESHOLD_NR_PAGES)
		flush_op = KBASE_MMU_OP_FLUSH_PT;

	if (!rt_mutex_trylock(&mmut->mmu_lock)) {
		/*
		 * Sometimes, mmu_lock takes long time to be released.
		 * In that case, kswapd is stuck until it can hold
		 * the lock. Instead, just bail out here so kswapd
		 * could reclaim other pages.
		 */
		if (current_is_kswapd())
			return -EBUSY;
		rt_mutex_lock(&mmut->mmu_lock);
	}

	err = kbase_mmu_teardown_pgd_pages(kbdev, mmut, vpfn, nr_virt_pages, &dirty_pgds,
					   &free_pgds_list, flush_op, as_nr);

	/* Set up MMU operation parameters. See above about MMU cache flush strategy. */
	op_param = (struct kbase_mmu_hw_op_param){
		.vpfn = start_vpfn,
		.nr = nr_virt_pages,
		.mmu_sync_info = mmu_sync_info,
		.kctx_id = mmut->kctx ? mmut->kctx->id : 0xFFFFFFFF,
		.op = (flush_op == KBASE_MMU_OP_FLUSH_PT) ? KBASE_MMU_OP_FLUSH_PT :
								  KBASE_MMU_OP_FLUSH_MEM,
		.flush_skip_levels = pgd_level_to_skip_flush(dirty_pgds),
	};
	mmu_flush_invalidate_teardown_pages(kbdev, mmut->kctx, as_nr, phys, nr_phys_pages,
					    &op_param);

	/* If page migration is enabled: the status of all physical pages involved
	 * shall be updated, unless they are not movable. Their status shall be
	 * updated before releasing the lock to protect against concurrent
	 * requests to migrate the pages, if they have been isolated.
	 */
	if (kbase_is_page_migration_enabled() && phys && !ignore_page_migration)
		kbase_mmu_progress_migration_on_teardown(kbdev, phys, nr_phys_pages);

	kbase_mmu_free_pgds_list(kbdev, mmut);

	rt_mutex_unlock(&mmut->mmu_lock);

	return err;
}

int kbase_mmu_teardown_pages(struct kbase_device *kbdev, struct kbase_mmu_table *mmut, u64 vpfn,
			     struct tagged_addr *phys, size_t nr_phys_pages, size_t nr_virt_pages,
			     int as_nr)
{
	return mmu_teardown_pages(kbdev, mmut, vpfn, phys, nr_phys_pages, nr_virt_pages, as_nr,
				  false);
}
KBASE_EXPORT_TEST_API(kbase_mmu_teardown_pages);

int kbase_mmu_teardown_imported_pages(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
				      u64 vpfn, struct tagged_addr *phys, size_t nr_phys_pages,
				      size_t nr_virt_pages, int as_nr)
{
	return mmu_teardown_pages(kbdev, mmut, vpfn, phys, nr_phys_pages, nr_virt_pages, as_nr,
				  true);
}

/**
 * kbase_mmu_update_pages_no_flush() - Update phy pages and attributes data in GPU
 *                                     page table entries
 *
 * @kbdev: Pointer to kbase device.
 * @mmut:  The involved MMU table
 * @vpfn:  Virtual PFN (Page Frame Number), in PAGE_SIZE units, of the first page to update
 * @phys:  Pointer to the array of tagged physical addresses of the physical
 *         pages that are pointed to by the page table entries (that need to
 *         be updated). The pointer should be within the reg->gpu_alloc->pages
 *         array.
 * @nr:    Number of pages (in PAGE_SIZE units) to update
 * @flags: Flags
 * @group_id: The physical memory group in which the page was allocated.
 *            Valid range is 0..(MEMORY_GROUP_MANAGER_NR_GROUPS-1).
 * @dirty_pgds: Flags to track every level where a PGD has been updated.
 *
 * This will update page table entries that already exist on the GPU based on
 * new flags and replace any existing phy pages that are passed (the PGD pages
 * remain unchanged). It is used as a response to the changes of phys as well
 * as the the memory attributes.
 *
 * The caller is responsible for validating the memory attributes.
 *
 * Return: 0 if the attributes data in page table entries were updated
 *         successfully, otherwise an error code.
 */
int kbase_mmu_update_pages_no_flush(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
					   u64 vpfn, struct tagged_addr *phys, size_t nr,
					   unsigned long flags, int const group_id, u64 *dirty_pgds)
{
	phys_addr_t pgd;
	u64 *pgd_page;
	int err;

	KBASE_DEBUG_ASSERT(vpfn <= (U64_MAX / PAGE_SIZE));

	/* Early out if there is nothing to do */
	if (nr == 0)
		return 0;

	/* Convert to GPU_PAGE_SIZE units. */
	vpfn *= GPU_PAGES_PER_CPU_PAGE;
	nr *= GPU_PAGES_PER_CPU_PAGE;
	rt_mutex_lock(&mmut->mmu_lock);

	while (nr) {
		unsigned int i;
		unsigned int index = vpfn & 0x1FF;
		size_t count = KBASE_MMU_PAGE_ENTRIES - index;
		struct page *p;
		register unsigned int num_of_valid_entries;
		int cur_level = MIDGARD_MMU_BOTTOMLEVEL;

		if (count > nr)
			count = nr;

		if (is_huge(*phys) &&
		    (index == (index_in_large_page(*phys) * GPU_PAGES_PER_CPU_PAGE)))
			cur_level = MIDGARD_MMU_LEVEL(2);

		err = mmu_get_pgd_at_level(kbdev, mmut, vpfn, cur_level, &pgd);
		if (WARN_ON(err))
			goto fail_unlock;

		p = pfn_to_page(PFN_DOWN(pgd));
		pgd_page = kmap_pgd(p, pgd);
		if (!pgd_page) {
			dev_warn(kbdev->dev, "kmap failure on update_pages");
			err = -ENOMEM;
			goto fail_unlock;
		}

		num_of_valid_entries = kbdev->mmu_mode->get_num_valid_entries(pgd_page);

		if (cur_level == MIDGARD_MMU_LEVEL(2)) {
			unsigned int level_index = (vpfn >> 9) & 0x1FFU;
			struct tagged_addr *target_phys = phys - index_in_large_page(*phys);

#ifdef CONFIG_MALI_DEBUG
			WARN_ON_ONCE(!kbdev->mmu_mode->ate_is_valid(pgd_page[level_index],
								    MIDGARD_MMU_LEVEL(2)));
#endif
			pgd_page[level_index] = kbase_mmu_create_ate(
				kbdev, *target_phys, flags, MIDGARD_MMU_LEVEL(2), group_id);
			kbase_mmu_sync_pgd(kbdev, mmut->kctx, pgd + (level_index * sizeof(u64)),
					   pgd_dma_addr(p, pgd) + (level_index * sizeof(u64)),
					   sizeof(u64), KBASE_MMU_OP_NONE);
		} else {
			for (i = 0; i < count; i += GPU_PAGES_PER_CPU_PAGE) {
				phys_addr_t base_phys_address =
					as_phys_addr_t(phys[i / GPU_PAGES_PER_CPU_PAGE]);
				unsigned int j;

				for (j = 0; j < GPU_PAGES_PER_CPU_PAGE; j++) {
					phys_addr_t page_address =
						base_phys_address + (j * GPU_PAGE_SIZE);
#ifdef CONFIG_MALI_DEBUG
					WARN_ON_ONCE(!kbdev->mmu_mode->ate_is_valid(
						pgd_page[index + i + j], MIDGARD_MMU_BOTTOMLEVEL));
#endif
					pgd_page[index + i + j] = kbase_mmu_create_ate(
						kbdev, as_tagged(page_address), flags,
						MIDGARD_MMU_BOTTOMLEVEL, group_id);
				}
			}

			/* MMU cache flush strategy is NONE because GPU cache maintenance
			 * will be done by the caller.
			 */
			kbase_mmu_sync_pgd(kbdev, mmut->kctx, pgd + (index * sizeof(u64)),
					   pgd_dma_addr(p, pgd) + (index * sizeof(u64)),
					   count * sizeof(u64), KBASE_MMU_OP_NONE);
		}

		kbdev->mmu_mode->set_num_valid_entries(pgd_page, num_of_valid_entries);

		if (dirty_pgds && count > 0)
			*dirty_pgds |= 1ULL << cur_level;

		phys += (count / GPU_PAGES_PER_CPU_PAGE);
		vpfn += count;
		nr -= count;

		kunmap_pgd(p, pgd_page);
	}

	rt_mutex_unlock(&mmut->mmu_lock);
	return 0;

fail_unlock:
	rt_mutex_unlock(&mmut->mmu_lock);
	return err;
}

static int kbase_mmu_update_pages_common(struct kbase_device *kbdev, struct kbase_context *kctx,
					 u64 vpfn, struct tagged_addr *phys, size_t nr,
					 unsigned long flags, int const group_id)
{
	int err;
	u64 dirty_pgds = 0;
	struct kbase_mmu_table *mmut;

#if !MALI_USE_CSF
	if (unlikely(kctx == NULL))
		return -EINVAL;

	mmut = &kctx->mmu;
#else
	mmut = kctx ? &kctx->mmu : &kbdev->csf.mcu_mmu;
#endif

	err = kbase_mmu_update_pages_no_flush(kbdev, mmut, vpfn, phys, nr, flags, group_id,
					      &dirty_pgds);

	kbase_mmu_flush_invalidate_update_pages(kbdev, kctx, vpfn, nr, dirty_pgds);

	return err;
}

void kbase_mmu_flush_invalidate_update_pages(struct kbase_device *kbdev, struct kbase_context *kctx, u64 vpfn,
					size_t nr, u64 dirty_pgds)
{
	struct kbase_mmu_hw_op_param op_param;
	/* Calls to this function are inherently asynchronous, with respect to
	 * MMU operations.
	 */
	const enum kbase_caller_mmu_sync_info mmu_sync_info = CALLER_MMU_ASYNC;
	int as_nr;

#if !MALI_USE_CSF
	if (unlikely(kctx == NULL))
		return;

	as_nr = kctx->as_nr;
#else
	as_nr = kctx ? kctx->as_nr : MCU_AS_NR;
#endif

	op_param = (const struct kbase_mmu_hw_op_param){
		.vpfn = vpfn,
		.nr = nr,
		.op = KBASE_MMU_OP_FLUSH_MEM,
		.kctx_id = kctx ? kctx->id : 0xFFFFFFFF,
		.mmu_sync_info = mmu_sync_info,
		.flush_skip_levels = pgd_level_to_skip_flush(dirty_pgds),
	};

	if (mmu_flush_cache_on_gpu_ctrl(kbdev))
		mmu_flush_invalidate_on_gpu_ctrl(kbdev, kctx, as_nr, &op_param);
	else
		mmu_flush_invalidate(kbdev, kctx, as_nr, &op_param);
}

int kbase_mmu_update_pages(struct kbase_context *kctx, u64 vpfn, struct tagged_addr *phys,
			   size_t nr, unsigned long flags, int const group_id)
{
	if (unlikely(kctx == NULL))
		return -EINVAL;

	return kbase_mmu_update_pages_common(kctx->kbdev, kctx, vpfn, phys, nr, flags, group_id);
}

#if MALI_USE_CSF
int kbase_mmu_update_csf_mcu_pages(struct kbase_device *kbdev, u64 vpfn, struct tagged_addr *phys,
				   size_t nr, unsigned long flags, int const group_id)
{
	return kbase_mmu_update_pages_common(kbdev, NULL, vpfn, phys, nr, flags, group_id);
}
#endif /* MALI_USE_CSF */

static void mmu_page_migration_transaction_begin(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	WARN_ON_ONCE(kbdev->mmu_page_migrate_in_progress);
	kbdev->mmu_page_migrate_in_progress = true;
}

static void mmu_page_migration_transaction_end(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);
	WARN_ON_ONCE(!kbdev->mmu_page_migrate_in_progress);
	kbdev->mmu_page_migrate_in_progress = false;
	/* Invoke the PM state machine, as the MMU page migration session
	 * may have deferred a transition in L2 state machine.
	 */
	kbase_pm_update_state(kbdev);
}

static void mmu_undo_migrate_pgd_sub_page(struct kbase_mmu_table *mmut, phys_addr_t old_pgd_phys,
					  phys_addr_t new_pgd_phys, dma_addr_t new_pgd_dma_addr,
					  u64 pgd_vpfn_level)
{
	struct kbase_device *kbdev;
	u64 vpfn = PGD_VPFN_LEVEL_GET_VPFN(pgd_vpfn_level);
	int level = PGD_VPFN_LEVEL_GET_LEVEL(pgd_vpfn_level);
	unsigned int index = (vpfn >> ((3 - level) * 9)) & 0x1FFU;
	unsigned int num_of_valid_entries;
	u64 *parent_pgd_page, *new_pgd_page, *target;
	phys_addr_t parent_pgd;
	u64 managed_pte;

	kbdev = mmut->kctx->kbdev;

	lockdep_assert_held(&mmut->kctx->reg_lock);
	lockdep_assert_held(&mmut->mmu_lock);

	if (mmu_get_pgd_at_level(kbdev, mmut, vpfn, level, &parent_pgd)) {
		dev_WARN(kbdev->dev, "Failed to get the PGD at level %u for VA %llx", level, vpfn);
		return;
	}

	parent_pgd_page = kmap_atomic_pgd(phys_to_page(parent_pgd), parent_pgd);
	num_of_valid_entries = kbdev->mmu_mode->get_num_valid_entries(parent_pgd_page);

#ifdef CONFIG_MALI_DEBUG
	/* The PTE should be pointing to the new sub page */
	if (new_pgd_phys !=
	    kbdev->mmu_mode->pte_to_phy_addr(kbdev->mgm_dev->ops.mgm_pte_to_original_pte(
		    kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, level, parent_pgd_page[index]))) {
		dev_WARN(kbdev->dev, "Unexpected PTE value for PGD at level %u for VA %llx", level,
			 vpfn);
	}
#endif

	/* Make PTE point to the old sub page */
	kbdev->mmu_mode->entry_set_pte(&managed_pte, old_pgd_phys);
	target = &parent_pgd_page[index];
	*target = kbdev->mgm_dev->ops.mgm_update_gpu_pte(kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP,
							 level, PBHA_ID_DEFAULT, PTE_FLAGS_NONE,
							 managed_pte);

	kbdev->mmu_mode->set_num_valid_entries(parent_pgd_page, num_of_valid_entries);
	kunmap_atomic_pgd(parent_pgd_page);

	/* Make PTE update visible in memory */
	kbase_mmu_sync_pgd_cpu(
		kbdev, pgd_dma_addr(phys_to_page(parent_pgd), parent_pgd) + (index * sizeof(u64)),
		sizeof(u64));

	/* Invalidate all entries in the new sub page (albeit it may not be really needed) */
	new_pgd_page = kmap_atomic_pgd(phys_to_page(new_pgd_phys), new_pgd_phys);
	kbdev->mmu_mode->entries_invalidate(new_pgd_page, KBASE_MMU_PAGE_ENTRIES);
	kunmap_atomic_pgd(new_pgd_page);
	dma_sync_single_for_device(kbdev->dev, new_pgd_dma_addr, GPU_PAGE_SIZE, DMA_BIDIRECTIONAL);
}

static int mmu_migrate_pgd_sub_page(phys_addr_t old_pgd_phys, phys_addr_t new_pgd_phys,
				    dma_addr_t old_pgd_dma_addr, dma_addr_t new_pgd_dma_addr,
				    u64 pgd_vpfn_level)
{
	struct kbase_page_metadata *page_md = kbase_page_private(phys_to_page(old_pgd_phys));
	struct kbase_mmu_hw_op_param op_param;
	struct kbase_mmu_table *mmut;
	struct kbase_device *kbdev;
	u64 *old_pgd_page, *new_pgd_page, *parent_pgd_page, *target;
	u64 vpfn = PGD_VPFN_LEVEL_GET_VPFN(pgd_vpfn_level);
	int level = PGD_VPFN_LEVEL_GET_LEVEL(pgd_vpfn_level);
	unsigned int index = (vpfn >> ((3 - level) * 9)) & 0x1FFU;
	unsigned long hwaccess_flags = 0;
	unsigned int num_of_valid_entries;
	phys_addr_t parent_pgd;
	u64 managed_pte;
	int ret = 0;

	if (WARN_ONCE(PAGE_STATUS_GET(page_md->status) != PT_MAPPED,
		      "Page metadata status %d does match expected value %d", page_md->status,
		      PT_MAPPED))
		return -EINVAL;

	mmut = page_md->data.pt_mapped.mmut;
	kbdev = mmut->kctx->kbdev;

	lockdep_assert_held(&mmut->kctx->reg_lock);
	lockdep_assert_held(&mmut->mmu_lock);

	/* Create all mappings before copying content.
	 * This is done as early as possible because it is the only operation that may
	 * fail. It is possible to do this before taking any locks because the
	 * pages to migrate are not going to change and even the parent PGD is not
	 * going to be affected by any other concurrent operation, since the page
	 * has been isolated before migration and therefore it cannot disappear in
	 * the middle of this function.
	 */
	old_pgd_page = kmap_pgd(phys_to_page(old_pgd_phys), old_pgd_phys);
	if (!old_pgd_page) {
		dev_warn(kbdev->dev, "%s: kmap failure for old pgd page.", __func__);
		ret = -EINVAL;
		goto old_page_map_error;
	}

	new_pgd_page = kmap_pgd(phys_to_page(new_pgd_phys), new_pgd_phys);
	if (!new_pgd_page) {
		dev_warn(kbdev->dev, "%s: kmap failure for new pgd page.", __func__);
		ret = -EINVAL;
		goto new_page_map_error;
	}

	/* GPU cache maintenance affects both memory content and page table,
	 * but at two different stages. A single virtual memory page is affected
	 * by the migration.
	 *
	 * Notice that the MMU maintenance is done in the following steps:
	 *
	 * 1) The MMU region is locked without performing any other operation.
	 *    This lock must cover the entire migration process, in order to
	 *    prevent any GPU access to the virtual page whose physical page
	 *    is being migrated.
	 * 2) Immediately after locking: the MMU region content is flushed via
	 *    GPU control while the lock is taken and without unlocking.
	 *    The region must stay locked for the duration of the whole page
	 *    migration procedure.
	 *    This is necessary to make sure that pending writes to the old page
	 *    are finalized before copying content to the new page.
	 * 3) Before unlocking: changes to the page table are flushed.
	 *    Finer-grained GPU control operations are used if possible, otherwise
	 *    the whole GPU cache shall be flushed again.
	 *    This is necessary to make sure that the GPU accesses the new page
	 *    after migration.
	 * 4) The MMU region is unlocked.
	 */
#define PGD_VPFN_MASK(level) (~((((u64)1) << ((3 - level) * 9)) - 1))
	op_param.mmu_sync_info = CALLER_MMU_ASYNC;
	op_param.kctx_id = mmut->kctx->id;
	op_param.vpfn = (vpfn / GPU_PAGES_PER_CPU_PAGE) & PGD_VPFN_MASK(level);
	op_param.nr = 1U << ((3 - level) * 9);
	op_param.op = KBASE_MMU_OP_FLUSH_PT;
	op_param.flush_skip_levels = pgd_level_to_skip_flush(3ULL << level);

	ret = mmu_get_pgd_at_level(kbdev, mmut, vpfn, level, &parent_pgd);
	if (ret) {
		dev_err(kbdev->dev, "%s: failed to find parent PGD for old PGD page.", __func__);
		goto get_pgd_at_level_error;
	}

	parent_pgd_page = kmap_pgd(phys_to_page(parent_pgd), parent_pgd);
	if (!parent_pgd_page) {
		dev_warn(kbdev->dev, "%s: kmap failure for parent PGD page.", __func__);
		ret = -EINVAL;
		goto pgd_page_map_error;
	}

	mutex_lock(&kbdev->mmu_hw_mutex);

	/* Lock MMU region and flush GPU cache by using GPU control,
	 * in order to keep MMU region locked.
	 */
	spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
	if (unlikely(!kbase_pm_l2_allow_mmu_page_migration(kbdev))) {
		/* Defer the migration as L2 is in a transitional phase */
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);
		mutex_unlock(&kbdev->mmu_hw_mutex);
		dev_dbg(kbdev->dev, "%s: L2 in transtion, abort PGD page migration", __func__);
		ret = -EAGAIN;
		goto l2_state_defer_out;
	}
	/* Prevent transitional phases in L2 by starting the transaction */
	mmu_page_migration_transaction_begin(kbdev);
	if (kbdev->pm.backend.gpu_ready && mmut->kctx->as_nr >= 0) {
		int as_nr = mmut->kctx->as_nr;
		struct kbase_as *as = &kbdev->as[as_nr];

		ret = kbase_mmu_hw_do_lock(kbdev, as, &op_param);
		if (!ret) {
#if MALI_USE_CSF
			if (mmu_flush_cache_on_gpu_ctrl(kbdev))
				ret = kbase_gpu_cache_flush_pa_range_and_busy_wait(
					kbdev, old_pgd_phys, GPU_PAGE_SIZE,
					GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_L2_LSC);
			else
#endif
				ret = kbase_gpu_cache_flush_and_busy_wait(
					kbdev, GPU_COMMAND_CACHE_CLN_INV_L2_LSC);
		}
		if (ret)
			mmu_page_migration_transaction_end(kbdev);
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);

	if (ret < 0) {
		mutex_unlock(&kbdev->mmu_hw_mutex);
		dev_err(kbdev->dev, "%s: failed to lock MMU region or flush GPU cache", __func__);
		goto l2_state_defer_out;
	}

	/* Copy memory content.
	 *
	 * It is necessary to claim the ownership of the DMA buffer for the old
	 * page before performing the copy, to make sure of reading a consistent
	 * version of its content, before copying. After the copy, ownership of
	 * the DMA buffer for the new page is given to the GPU in order to make
	 * the content visible to potential GPU access that may happen as soon as
	 * this function releases the lock on the MMU region.
	 */
	dma_sync_single_for_cpu(kbdev->dev, old_pgd_dma_addr, GPU_PAGE_SIZE, DMA_BIDIRECTIONAL);
	memcpy(new_pgd_page, old_pgd_page, GPU_PAGE_SIZE);
	dma_sync_single_for_device(kbdev->dev, new_pgd_dma_addr, GPU_PAGE_SIZE, DMA_BIDIRECTIONAL);

	/* Remap GPU PGD page.
	 *
	 * The current implementation doesn't handle the case of a level 0 PGD,
	 * that is: the root PGD of the page table.
	 */
	target = &parent_pgd_page[index];

	/* Certain entries of a page table page encode the count of valid entries
	 * present in that page. So need to save & restore the count information
	 * when updating the PTE/ATE to point to the new page.
	 */
	num_of_valid_entries = kbdev->mmu_mode->get_num_valid_entries(parent_pgd_page);

#ifdef CONFIG_MALI_DEBUG
	/* The PTE should be pointing to the page being migrated */
	WARN_ON_ONCE(
		old_pgd_phys !=
		kbdev->mmu_mode->pte_to_phy_addr(kbdev->mgm_dev->ops.mgm_pte_to_original_pte(
			kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, level, parent_pgd_page[index])));
#endif
	kbdev->mmu_mode->entry_set_pte(&managed_pte, new_pgd_phys);
	*target = kbdev->mgm_dev->ops.mgm_update_gpu_pte(kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP,
							 PBHA_ID_DEFAULT, PTE_FLAGS_NONE, level,
							 managed_pte);

	kbdev->mmu_mode->set_num_valid_entries(parent_pgd_page, num_of_valid_entries);

	/* This function always updates a single entry inside an existing PGD therefore
	 * cache maintenance is necessary.
	 */
	kbase_mmu_sync_pgd(kbdev, mmut->kctx, parent_pgd + (index * sizeof(u64)),
			   pgd_dma_addr(phys_to_page(parent_pgd), parent_pgd) +
				   (index * sizeof(u64)),
			   sizeof(u64), KBASE_MMU_OP_FLUSH_PT);

	/* Unlock MMU region.
	 *
	 * For GPUs without FLUSH_PA_RANGE support, the GPU caches were completely
	 * cleaned and invalidated after locking the virtual address range affected
	 * by the migration. As long as the lock is in place, GPU access to the
	 * locked range would remain blocked. So there is no need to clean and
	 * invalidate the GPU caches again after the copying the page contents
	 * of old page and updating the page table entry to point to new page.
	 *
	 * For GPUs with FLUSH_PA_RANGE support, the contents of old page would
	 * have been evicted from the GPU caches after locking the virtual address
	 * range. The page table entry contents also would have been invalidated
	 * from the GPU's L2 cache by kbase_mmu_sync_pgd() after the page table
	 * update.
	 *
	 * If kbase_mmu_hw_do_unlock_no_addr() fails, GPU reset will be triggered which
	 * would remove the MMU lock and so there is no need to rollback page migration
	 * and the failure can be ignored.
	 */
	spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
	if (kbdev->pm.backend.gpu_ready && mmut->kctx->as_nr >= 0) {
		int as_nr = mmut->kctx->as_nr;
		struct kbase_as *as = &kbdev->as[as_nr];
		int local_ret = kbase_mmu_hw_do_unlock_no_addr(kbdev, as, &op_param);

		CSTD_UNUSED(local_ret);
	}

	/* Release the transition prevention in L2 by ending the transaction */
	mmu_page_migration_transaction_end(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);
	/* Releasing locks before checking the migration transaction error state */
	mutex_unlock(&kbdev->mmu_hw_mutex);

l2_state_defer_out:
	kunmap_pgd(phys_to_page(parent_pgd), parent_pgd_page);
pgd_page_map_error:
get_pgd_at_level_error:
	kunmap_pgd(phys_to_page(new_pgd_phys), new_pgd_page);
new_page_map_error:
	kunmap_pgd(phys_to_page(old_pgd_phys), old_pgd_page);
old_page_map_error:
	return ret;
}

int kbase_mmu_migrate_pgd_page(struct tagged_addr old_pgd_phys, struct tagged_addr new_pgd_phys,
			       dma_addr_t old_pgd_dma_addr, dma_addr_t new_pgd_dma_addr)
{
	struct kbase_page_metadata *page_md = kbase_page_private(as_page(old_pgd_phys));
	struct kbase_mmu_table *mmut;
	struct kbase_device *kbdev;
	u32 sub_page_index;
	u64 old_pgd_phys_addr = as_phys_addr_t(old_pgd_phys);
	u64 new_pgd_phys_addr = as_phys_addr_t(new_pgd_phys);
	int check_state, ret = 0;

	/* If page migration support is not compiled in, return with fault */
	if (!kbase_is_page_migration_enabled())
		return -EINVAL;

	if (WARN_ONCE(PAGE_STATUS_GET(page_md->status) != PT_MAPPED,
		      "Page metadata status %d does match expected value %d", page_md->status,
		      PT_MAPPED))
		return -EINVAL;

	mmut = page_md->data.pt_mapped.mmut;
	/* Due to the hard binding of mmu_command_instr with kctx_id via kbase_mmu_hw_op_param,
	 * here we skip the no kctx case, which is only used with MCU's mmut.
	 */
	if (WARN_ONCE(!mmut->kctx, "Migration failed as kctx is null"))
		return -EINVAL;

	kbdev = mmut->kctx->kbdev;

	if (WARN_ON_ONCE(old_pgd_phys_addr & ~PAGE_MASK))
		return -EINVAL;

	if (WARN_ON_ONCE(new_pgd_phys_addr & ~PAGE_MASK))
		return -EINVAL;

	lockdep_assert_held(&mmut->kctx->reg_lock);

	rt_mutex_lock(&mmut->mmu_lock);

	/* The state was evaluated before entering this function, but it could
	 * have changed before the mmu_lock was taken. However, the state
	 * transitions which are possible at this point are only two, and in both
	 * cases it is a stable state progressing to a "free in progress" state.
	 *
	 * After taking the mmu_lock the state can no longer change: read it again
	 * and make sure that it hasn't changed before continuing.
	 */
	spin_lock(&page_md->migrate_lock);
	check_state = PAGE_STATUS_GET(page_md->status);
	spin_unlock(&page_md->migrate_lock);
	if (check_state != PT_MAPPED) {
		dev_dbg(kbdev->dev, "%s: state changed to %d (was %d), abort PGD page migration",
			__func__, check_state, PT_MAPPED);
		WARN_ON_ONCE(check_state != FREE_PT_ISOLATED_IN_PROGRESS);
		ret = -EAGAIN;
		goto unlock;
	}

	for (sub_page_index = 0; sub_page_index < GPU_PAGES_PER_CPU_PAGE; sub_page_index++) {
		if (!page_md->data.pt_mapped.pgd_vpfn_level[sub_page_index])
			continue;

		ret = mmu_migrate_pgd_sub_page(
			old_pgd_phys_addr + (sub_page_index * GPU_PAGE_SIZE),
			new_pgd_phys_addr + (sub_page_index * GPU_PAGE_SIZE),
			old_pgd_dma_addr + (sub_page_index * GPU_PAGE_SIZE),
			new_pgd_dma_addr + (sub_page_index * GPU_PAGE_SIZE),
			page_md->data.pt_mapped.pgd_vpfn_level[sub_page_index]);
		if (ret)
			break;
	}

	if (ret == 0) {
		/* Undertaking metadata transfer, while we are holding the mmu_lock */
		spin_lock(&page_md->migrate_lock);
		/* Update the new page dma_addr with the transferred metadata from the old_page */
		page_md->dma_addr = new_pgd_dma_addr;
		page_md->status = PAGE_ISOLATE_SET(page_md->status, 0);

#if GPU_PAGES_PER_CPU_PAGE > 1
		page_md->data.pt_mapped.pgd_page = as_page(new_pgd_phys);
		if (mmut->last_allocated_pgd_page == as_page(old_pgd_phys))
			mmut->last_allocated_pgd_page = as_page(new_pgd_phys);
		if (mmut->last_freed_pgd_page == as_page(old_pgd_phys))
			mmut->last_freed_pgd_page = as_page(new_pgd_phys);
#endif
		spin_unlock(&page_md->migrate_lock);

		set_page_private(as_page(new_pgd_phys), (unsigned long)page_md);
		/* Old page metatdata pointer cleared as it now owned by the new page */
		set_page_private(as_page(old_pgd_phys), 0);
	} else {
		unsigned long hwaccess_flags;

		/* Undo the GPU page table updates to remove references to the new page */
		while (sub_page_index-- > 0) {
			if (!page_md->data.pt_mapped.pgd_vpfn_level[sub_page_index])
				continue;

			mmu_undo_migrate_pgd_sub_page(
				mmut, old_pgd_phys_addr + (sub_page_index * GPU_PAGE_SIZE),
				new_pgd_phys_addr + (sub_page_index * GPU_PAGE_SIZE),
				new_pgd_dma_addr + (sub_page_index * GPU_PAGE_SIZE),
				page_md->data.pt_mapped.pgd_vpfn_level[sub_page_index]);
		}

		spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
		if (kbdev->pm.backend.gpu_ready && mmut->kctx->as_nr >= 0) {
			struct kbase_mmu_hw_op_param op_param = {
				.vpfn = 0,
				.nr = ~0U,
				.flush_skip_levels = 0,
				.op = KBASE_MMU_OP_FLUSH_PT,
				.kctx_id = mmut->kctx->id,
				.mmu_sync_info = CALLER_MMU_ASYNC,
			};
			int as_nr = mmut->kctx->as_nr;
			struct kbase_as *as = &kbdev->as[as_nr];
			int local_ret;

			local_ret = kbase_mmu_hw_do_lock(kbdev, as, &op_param);
			CSTD_UNUSED(local_ret);

			local_ret = kbase_gpu_cache_flush_and_busy_wait(
				kbdev, GPU_COMMAND_CACHE_CLN_INV_L2);
			CSTD_UNUSED(local_ret);

			local_ret = kbase_mmu_hw_do_unlock_no_addr(kbdev, as, &op_param);
			CSTD_UNUSED(local_ret);
		}
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);
	}

unlock:
	rt_mutex_unlock(&mmut->mmu_lock);
	return ret;
}

int kbase_mmu_migrate_data_page(struct tagged_addr old_phys, struct tagged_addr new_phys,
				dma_addr_t old_dma_addr, dma_addr_t new_dma_addr)
{
	struct kbase_page_metadata *page_md = kbase_page_private(as_page(old_phys));
	struct kbase_mmu_hw_op_param op_param;
	struct kbase_mmu_table *mmut;
	struct kbase_device *kbdev;
	phys_addr_t pgd;
	u64 *old_page, *new_page, *pgd_page, *target, vpfn;
	unsigned int index;
	int check_state, ret = 0;
	unsigned long hwaccess_flags = 0;
	unsigned int num_of_valid_entries;
	u8 vmap_count = 0;
	phys_addr_t base_phys_address = as_phys_addr_t(new_phys);
	enum kbase_page_status page_status;
	unsigned int i;

	/* If page migration support is not compiled in, return with fault */
	if (!kbase_is_page_migration_enabled())
		return -EINVAL;

	if (WARN_ONCE(PAGE_STATUS_GET(page_md->status) != ALLOCATED_MAPPED,
		      "Page metadata status %d does match expected value %d", page_md->status,
		      ALLOCATED_MAPPED))
		return -EINVAL;

	mmut = page_md->data.mapped.mmut;

	/* Due to the hard binding of mmu_command_instr with kctx_id via kbase_mmu_hw_op_param,
	 * here we skip the no kctx case, which is only used with MCU's mmut.
	 */
	if (WARN_ONCE(!mmut->kctx, "Migration failed as kctx is null"))
		return -EINVAL;

	lockdep_assert_held(&mmut->kctx->reg_lock);

	vpfn = page_md->data.mapped.vpfn;
	kbdev = mmut->kctx->kbdev;
	index = vpfn & 0x1FFU;

	/* Create all mappings before copying content.
	 * This is done as early as possible because it is the only operation that may
	 * fail. It is possible to do this before taking any locks because the
	 * pages to migrate are not going to change and even the parent PGD is not
	 * going to be affected by any other concurrent operation, since the page
	 * has been isolated before migration and therefore it cannot disappear in
	 * the middle of this function.
	 */
	old_page = kbase_kmap(as_page(old_phys));
	if (!old_page) {
		dev_warn(kbdev->dev, "%s: kmap failure for old page.", __func__);
		ret = -EINVAL;
		goto old_page_map_error;
	}

	new_page = kbase_kmap(as_page(new_phys));
	if (!new_page) {
		dev_warn(kbdev->dev, "%s: kmap failure for new page.", __func__);
		ret = -EINVAL;
		goto new_page_map_error;
	}

	/* GPU cache maintenance affects both memory content and page table,
	 * but at two different stages. A single virtual memory page is affected
	 * by the migration.
	 *
	 * Notice that the MMU maintenance is done in the following steps:
	 *
	 * 1) The MMU region is locked without performing any other operation.
	 *    This lock must cover the entire migration process, in order to
	 *    prevent any GPU access to the virtual page whose physical page
	 *    is being migrated.
	 * 2) Immediately after locking: the MMU region content is flushed via
	 *    GPU control while the lock is taken and without unlocking.
	 *    The region must stay locked for the duration of the whole page
	 *    migration procedure.
	 *    This is necessary to make sure that pending writes to the old page
	 *    are finalized before copying content to the new page.
	 * 3) Before unlocking: changes to the page table are flushed.
	 *    Finer-grained GPU control operations are used if possible, otherwise
	 *    the whole GPU cache shall be flushed again.
	 *    This is necessary to make sure that the GPU accesses the new page
	 *    after migration.
	 * 4) The MMU region is unlocked.
	 */
	op_param.mmu_sync_info = CALLER_MMU_ASYNC;
	op_param.kctx_id = mmut->kctx->id;
	op_param.vpfn = (vpfn / GPU_PAGES_PER_CPU_PAGE);
	op_param.nr = 1;
	op_param.op = KBASE_MMU_OP_FLUSH_PT;
	op_param.flush_skip_levels = pgd_level_to_skip_flush(1ULL << MIDGARD_MMU_BOTTOMLEVEL);

	rt_mutex_lock(&mmut->mmu_lock);

	/* The state was evaluated before entering this function, but it could
	 * have changed before the mmu_lock was taken. However, the state
	 * transitions which are possible at this point are only two, and in both
	 * cases it is a stable state progressing to a "free in progress" state.
	 *
	 * After taking the mmu_lock the state can no longer change: read it again
	 * and make sure that it hasn't changed before continuing.
	 */
	spin_lock(&page_md->migrate_lock);
	check_state = PAGE_STATUS_GET(page_md->status);
	vmap_count = page_md->vmap_count;
	spin_unlock(&page_md->migrate_lock);

	if (check_state != ALLOCATED_MAPPED) {
		dev_dbg(kbdev->dev, "%s: state changed to %d (was %d), abort page migration",
			__func__, check_state, ALLOCATED_MAPPED);
		ret = -EAGAIN;
		goto page_state_change_out;
	} else if (vmap_count > 0) {
		dev_dbg(kbdev->dev, "%s: page was multi-mapped, abort page migration", __func__);
		ret = -EAGAIN;
		goto page_state_change_out;
	}

	ret = mmu_get_pgd_at_level(kbdev, mmut, vpfn, MIDGARD_MMU_BOTTOMLEVEL, &pgd);
	if (ret) {
		dev_err(kbdev->dev, "%s: failed to find PGD for old page.", __func__);
		goto get_pgd_at_level_error;
	}

	pgd_page = kmap_pgd(phys_to_page(pgd), pgd);
	if (!pgd_page) {
		dev_warn(kbdev->dev, "%s: kmap failure for PGD page.", __func__);
		ret = -EINVAL;
		goto pgd_page_map_error;
	}

	mutex_lock(&kbdev->mmu_hw_mutex);

	/* Lock MMU region and flush GPU cache by using GPU control,
	 * in order to keep MMU region locked.
	 */
	spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
	if (unlikely(!kbase_pm_l2_allow_mmu_page_migration(kbdev))) {
		/* Defer the migration as L2 is in a transitional phase */
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);
		mutex_unlock(&kbdev->mmu_hw_mutex);
		dev_dbg(kbdev->dev, "%s: L2 in transtion, abort PGD page migration", __func__);
		ret = -EAGAIN;
		goto l2_state_defer_out;
	}
	/* Prevent transitional phases in L2 by starting the transaction */
	mmu_page_migration_transaction_begin(kbdev);
	if (kbdev->pm.backend.gpu_ready && mmut->kctx->as_nr >= 0) {
		int as_nr = mmut->kctx->as_nr;
		struct kbase_as *as = &kbdev->as[as_nr];

		ret = kbase_mmu_hw_do_lock(kbdev, as, &op_param);
		if (!ret) {
#if MALI_USE_CSF
			if (mmu_flush_cache_on_gpu_ctrl(kbdev))
				ret = kbase_gpu_cache_flush_pa_range_and_busy_wait(
					kbdev, as_phys_addr_t(old_phys), PAGE_SIZE,
					GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_L2_LSC);
			else
#endif
				ret = kbase_gpu_cache_flush_and_busy_wait(
					kbdev, GPU_COMMAND_CACHE_CLN_INV_L2_LSC);
		}
		if (ret)
			mmu_page_migration_transaction_end(kbdev);
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);

	if (ret < 0) {
		mutex_unlock(&kbdev->mmu_hw_mutex);
		dev_err(kbdev->dev, "%s: failed to lock MMU region or flush GPU cache", __func__);
		goto undo_mappings;
	}

	/* Copy memory content.
	 *
	 * It is necessary to claim the ownership of the DMA buffer for the old
	 * page before performing the copy, to make sure of reading a consistent
	 * version of its content, before copying. After the copy, ownership of
	 * the DMA buffer for the new page is given to the GPU in order to make
	 * the content visible to potential GPU access that may happen as soon as
	 * this function releases the lock on the MMU region.
	 */
	dma_sync_single_for_cpu(kbdev->dev, old_dma_addr, PAGE_SIZE, DMA_BIDIRECTIONAL);
	memcpy(new_page, old_page, PAGE_SIZE);
	dma_sync_single_for_device(kbdev->dev, new_dma_addr, PAGE_SIZE, DMA_BIDIRECTIONAL);

	/* Remap GPU virtual page.
	 *
	 * This code rests on the assumption that page migration is only enabled
	 * for small pages, that necessarily live in the bottom level of the MMU
	 * page table.
	 */
	target = &pgd_page[index];

	/* Certain entries of a page table page encode the count of valid entries
	 * present in that page. So need to save & restore the count information
	 * when updating the PTE/ATE to point to the new page.
	 */
	num_of_valid_entries = kbdev->mmu_mode->get_num_valid_entries(pgd_page);

	for (i = 0; i < GPU_PAGES_PER_CPU_PAGE; i++) {
		phys_addr_t page_address = base_phys_address + (i * GPU_PAGE_SIZE);

		WARN_ON_ONCE((*target & 1UL) == 0);
		*target = kbase_mmu_create_ate(kbdev, as_tagged(page_address),
					       page_md->data.mapped.reg->flags,
					       MIDGARD_MMU_BOTTOMLEVEL,
					       page_md->data.mapped.reg->gpu_alloc->group_id);
		target++;
	}

	kbdev->mmu_mode->set_num_valid_entries(pgd_page, num_of_valid_entries);

	/* This function always updates a single entry inside an existing PGD when
	 * PAGE_SIZE is 4K, and would update more than one entry when PAGE_SIZE is
	 * not 4K, therefore cache maintenance is necessary.
	 */
	kbase_mmu_sync_pgd(kbdev, mmut->kctx, pgd + (index * sizeof(u64)),
			   pgd_dma_addr(phys_to_page(pgd), pgd) + (index * sizeof(u64)),
			   GPU_PAGES_PER_CPU_PAGE * sizeof(u64), KBASE_MMU_OP_FLUSH_PT);

	/* Unlock MMU region.
	 *
	 * For GPUs without FLUSH_PA_RANGE support, the GPU caches were completely
	 * cleaned and invalidated after locking the virtual address range affected
	 * by the migration. As long as the lock is in place, GPU access to the
	 * locked range would remain blocked. So there is no need to clean and
	 * invalidate the GPU caches again after the copying the page contents
	 * of old page and updating the page table entry to point to new page.
	 *
	 * For GPUs with FLUSH_PA_RANGE support, the contents of old page would
	 * have been evicted from the GPU caches after locking the virtual address
	 * range. The page table entry contents also would have been invalidated
	 * from the GPU's L2 cache by kbase_mmu_sync_pgd() after the page table
	 * update.
	 *
	 * If kbase_mmu_hw_do_unlock_no_addr() fails, GPU reset will be triggered which
	 * would remove the MMU lock and so there is no need to rollback page migration
	 * and the failure can be ignored.
	 */
	spin_lock_irqsave(&kbdev->hwaccess_lock, hwaccess_flags);
	if (kbdev->pm.backend.gpu_ready && mmut->kctx->as_nr >= 0) {
		int as_nr = mmut->kctx->as_nr;
		struct kbase_as *as = &kbdev->as[as_nr];
		int local_ret = kbase_mmu_hw_do_unlock_no_addr(kbdev, as, &op_param);

		CSTD_UNUSED(local_ret);
	}

	/* Release the transition prevention in L2 by ending the transaction */
	mmu_page_migration_transaction_end(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, hwaccess_flags);
	/* Releasing locks before checking the migration transaction error state */
	mutex_unlock(&kbdev->mmu_hw_mutex);

	/* Undertaking metadata transfer, while we are holding the mmu_lock */
	spin_lock(&page_md->migrate_lock);
	page_status = PAGE_STATUS_GET(page_md->status);
	if (page_status == ALLOCATED_MAPPED) {
		/* Replace page in array of pages of the physical allocation. */
		size_t page_array_index = (page_md->data.mapped.vpfn / GPU_PAGES_PER_CPU_PAGE) -
					  page_md->data.mapped.reg->start_pfn;

		page_md->data.mapped.reg->gpu_alloc->pages[page_array_index] = new_phys;
	} else if (page_status == NOT_MOVABLE) {
		dev_dbg(kbdev->dev, "%s: migration completed and page has become NOT_MOVABLE.",
			__func__);
	} else {
		dev_WARN(kbdev->dev, "%s: migration completed but page has moved to status %d.",
			 __func__, page_status);
	}
	/* Update the new page dma_addr with the transferred metadata from the old_page */
	page_md->dma_addr = new_dma_addr;
	page_md->status = PAGE_ISOLATE_SET(page_md->status, 0);
	spin_unlock(&page_md->migrate_lock);
	set_page_private(as_page(new_phys), (unsigned long)page_md);
	/* Old page metatdata pointer cleared as it now owned by the new page */
	set_page_private(as_page(old_phys), 0);

l2_state_defer_out:
	kunmap_pgd(phys_to_page(pgd), pgd_page);
pgd_page_map_error:
get_pgd_at_level_error:
page_state_change_out:
	rt_mutex_unlock(&mmut->mmu_lock);

	kbase_kunmap(as_page(new_phys), new_page);
new_page_map_error:
	kbase_kunmap(as_page(old_phys), old_page);
old_page_map_error:
	return ret;

undo_mappings:
	/* Unlock the MMU table and undo mappings. */
	rt_mutex_unlock(&mmut->mmu_lock);
	kunmap_pgd(phys_to_page(pgd), pgd_page);
	kbase_kunmap(as_page(new_phys), new_page);
	kbase_kunmap(as_page(old_phys), old_page);

	return ret;
}

static void mmu_teardown_level(struct kbase_device *kbdev, struct kbase_mmu_table *mmut,
			       phys_addr_t pgd, int level)
{
	u64 *pgd_page;
	int i;
	struct memory_group_manager_device *mgm_dev = kbdev->mgm_dev;
	struct kbase_mmu_mode const *mmu_mode = kbdev->mmu_mode;
	u64 *pgd_page_buffer = NULL;
	struct page *p = phys_to_page(pgd);

	lockdep_assert_held(&mmut->mmu_lock);

	pgd_page = kmap_atomic_pgd(p, pgd);
	/* kmap_atomic should NEVER fail. */
	if (WARN_ON_ONCE(pgd_page == NULL))
		return;
	if (level < MIDGARD_MMU_BOTTOMLEVEL) {
		/* Copy the page to our preallocated buffer so that we can minimize
		 * kmap_atomic usage
		 */
		pgd_page_buffer = mmut->scratch_mem.teardown_pages.levels[level];
		memcpy(pgd_page_buffer, pgd_page, GPU_PAGE_SIZE);
	}

	/* When page migration is enabled, kbase_region_tracker_term() would ensure
	 * there are no pages left mapped on the GPU for a context. Hence the count
	 * of valid entries is expected to be zero here.
	 */
	if (kbase_is_page_migration_enabled() && mmut->kctx)
		WARN_ON_ONCE(kbdev->mmu_mode->get_num_valid_entries(pgd_page));
	/* Invalidate page after copying */
	mmu_mode->entries_invalidate(pgd_page, KBASE_MMU_PAGE_ENTRIES);
	kunmap_atomic_pgd(pgd_page);
	pgd_page = pgd_page_buffer;

	if (level < MIDGARD_MMU_BOTTOMLEVEL) {
		for (i = 0; i < KBASE_MMU_PAGE_ENTRIES; i++) {
			if (mmu_mode->pte_is_valid(pgd_page[i], level)) {
				phys_addr_t target_pgd = mmu_mode->pte_to_phy_addr(
					mgm_dev->ops.mgm_pte_to_original_pte(mgm_dev,
									     MGM_DEFAULT_PTE_GROUP,
									     level, pgd_page[i]));

				mmu_teardown_level(kbdev, mmut, target_pgd, level + 1);
			}
		}
	}

	kbase_mmu_free_pgd(kbdev, mmut, pgd);
}

static void kbase_mmu_mark_non_movable(struct kbase_device *const kbdev, struct page *page)
{
	struct kbase_page_metadata *page_md;

	if (!kbase_is_page_migration_enabled())
		return;

	/* Composite large-page is excluded from migration, trigger a warn if a development
	 * wrongly leads to it.
	 */
	if (is_huge_head(as_tagged(page_to_phys(page))) ||
	    is_partial(as_tagged(page_to_phys(page))))
		dev_WARN(kbdev->dev, "%s: migration on large-page attempted.", __func__);

	page_md = kbase_page_private(page);

	spin_lock(&page_md->migrate_lock);
	page_md->status = PAGE_STATUS_SET(page_md->status, NOT_MOVABLE);

	if (IS_PAGE_MOVABLE(page_md->status))
		page_md->status = PAGE_MOVABLE_CLEAR(page_md->status);

	spin_unlock(&page_md->migrate_lock);
}

int kbase_mmu_init(struct kbase_device *const kbdev, struct kbase_mmu_table *const mmut,
		   struct kbase_context *const kctx, int const group_id)
{
	if (WARN_ON(group_id >= MEMORY_GROUP_MANAGER_NR_GROUPS) || WARN_ON(group_id < 0))
		return -EINVAL;

	compiletime_assert(KBASE_MEM_ALLOC_MAX_SIZE <= (((8ull << 30) >> PAGE_SHIFT)),
			   "List of free PGDs may not be large enough.");
	compiletime_assert(MAX_PAGES_FOR_FREE_PGDS >= MIDGARD_MMU_BOTTOMLEVEL,
			   "Array of MMU levels is not large enough.");

	mmut->group_id = group_id;
	rt_mutex_init(&mmut->mmu_lock);
	mmut->kctx = kctx;
	mmut->pgd = KBASE_INVALID_PHYSICAL_ADDRESS;

#if GPU_PAGES_PER_CPU_PAGE > 1
	INIT_LIST_HEAD(&mmut->pgd_pages_list);
#endif

	/* We allocate pages into the kbdev memory pool, then
	 * kbase_mmu_alloc_pgd will allocate out of that pool. This is done to
	 * avoid allocations from the kernel happening with the lock held.
	 */
	while (mmut->pgd == KBASE_INVALID_PHYSICAL_ADDRESS) {
		int err;

		err = kbase_mem_pool_grow(&kbdev->mem_pools.small[mmut->group_id],
					  MIDGARD_MMU_BOTTOMLEVEL, kctx ? kctx->task : NULL);
		if (err) {
			kbase_mmu_term(kbdev, mmut);
			return -ENOMEM;
		}

		rt_mutex_lock(&mmut->mmu_lock);
		mmut->pgd = kbase_mmu_alloc_pgd(kbdev, mmut);
		rt_mutex_unlock(&mmut->mmu_lock);
	}

	kbase_mmu_mark_non_movable(kbdev, pfn_to_page(PFN_DOWN(mmut->pgd)));
	return 0;
}

void kbase_mmu_term(struct kbase_device *kbdev, struct kbase_mmu_table *mmut)
{
	WARN((mmut->kctx) && (mmut->kctx->as_nr != KBASEP_AS_NR_INVALID),
	     "kctx-%d_%d must first be scheduled out to flush GPU caches+tlbs before tearing down MMU tables",
	     mmut->kctx->tgid, mmut->kctx->id);

	if (mmut->pgd != KBASE_INVALID_PHYSICAL_ADDRESS) {
		rt_mutex_lock(&mmut->mmu_lock);
		mmu_teardown_level(kbdev, mmut, mmut->pgd, MIDGARD_MMU_TOPLEVEL);
		rt_mutex_unlock(&mmut->mmu_lock);

		if (mmut->kctx)
			KBASE_TLSTREAM_AUX_PAGESALLOC(kbdev, mmut->kctx->id, 0);
	}
}

void kbase_mmu_as_term(struct kbase_device *kbdev, unsigned int i)
{
	destroy_workqueue(kbdev->as[i].pf_wq);
}

void kbase_mmu_flush_pa_range(struct kbase_device *kbdev, struct kbase_context *kctx,
			      phys_addr_t phys, size_t size, enum kbase_mmu_op_type flush_op)
{
#if MALI_USE_CSF
	unsigned long irq_flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, irq_flags);
	if (mmu_flush_cache_on_gpu_ctrl(kbdev) && (flush_op != KBASE_MMU_OP_NONE) &&
	    kbdev->pm.backend.gpu_ready && (!kctx || kctx->as_nr >= 0))
		mmu_flush_pa_range(kbdev, phys, size, KBASE_MMU_OP_FLUSH_PT);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, irq_flags);
#else
	CSTD_UNUSED(kbdev);
	CSTD_UNUSED(kctx);
	CSTD_UNUSED(phys);
	CSTD_UNUSED(size);
	CSTD_UNUSED(flush_op);
#endif
}

#ifdef CONFIG_MALI_VECTOR_DUMP
static size_t kbasep_mmu_dump_level(struct kbase_context *kctx, phys_addr_t pgd, int level,
				    char **const buffer, size_t *size_left)
{
	phys_addr_t target_pgd;
	u64 *pgd_page;
	int i;
	size_t size = KBASE_MMU_PAGE_ENTRIES * sizeof(u64) + sizeof(u64);
	size_t dump_size;
	struct kbase_device *kbdev;
	struct kbase_mmu_mode const *mmu_mode;
	struct page *p;

	if (WARN_ON(kctx == NULL))
		return 0;
	lockdep_assert_held(&kctx->mmu.mmu_lock);

	kbdev = kctx->kbdev;
	mmu_mode = kbdev->mmu_mode;

	p = pfn_to_page(PFN_DOWN(pgd));
	pgd_page = kmap_pgd(p, pgd);
	if (!pgd_page) {
		dev_warn(kbdev->dev, "%s: kmap failure", __func__);
		return 0;
	}

	if (*size_left >= size) {
		/* A modified physical address that contains
		 * the page table level
		 */
		u64 m_pgd = pgd | (u64)level;

		/* Put the modified physical address in the output buffer */
		memcpy(*buffer, &m_pgd, sizeof(m_pgd));
		*buffer += sizeof(m_pgd);

		/* Followed by the page table itself */
		memcpy(*buffer, pgd_page, sizeof(u64) * KBASE_MMU_PAGE_ENTRIES);
		*buffer += sizeof(u64) * KBASE_MMU_PAGE_ENTRIES;

		*size_left -= size;
	}

	if (level < MIDGARD_MMU_BOTTOMLEVEL) {
		for (i = 0; i < KBASE_MMU_PAGE_ENTRIES; i++) {
			if (mmu_mode->pte_is_valid(pgd_page[i], level)) {
				target_pgd = mmu_mode->pte_to_phy_addr(
					kbdev->mgm_dev->ops.mgm_pte_to_original_pte(
						kbdev->mgm_dev, MGM_DEFAULT_PTE_GROUP, level,
						pgd_page[i]));

				dump_size = kbasep_mmu_dump_level(kctx, target_pgd, level + 1,
								  buffer, size_left);
				if (!dump_size) {
					kunmap_pgd(p, pgd_page);
					return 0;
				}
				size += dump_size;
			}
		}
	}

	kunmap_pgd(p, pgd_page);

	return size;
}

void *kbase_mmu_dump(struct kbase_context *kctx, size_t nr_pages)
{
	void *kaddr;
	size_t size_left;

	KBASE_DEBUG_ASSERT(kctx);

	if (nr_pages == 0) {
		/* can't dump in a 0 sized buffer, early out */
		return NULL;
	}

	size_left = nr_pages * PAGE_SIZE;

	if (WARN_ON(size_left == 0))
		return NULL;
	kaddr = vmalloc_user(size_left);

	rt_mutex_lock(&kctx->mmu.mmu_lock);

	if (kaddr) {
		u64 end_marker = 0xFFULL;
		char *buffer;
		char *mmu_dump_buffer;
		u64 config[3];
		size_t dump_size, size = 0;
		struct kbase_mmu_setup as_setup;

		buffer = (char *)kaddr;
		mmu_dump_buffer = buffer;

		kctx->kbdev->mmu_mode->get_as_setup(&kctx->mmu, &as_setup);
		config[0] = as_setup.transtab;
		config[1] = as_setup.memattr;
		config[2] = as_setup.transcfg;
		memcpy(buffer, &config, sizeof(config));
		mmu_dump_buffer += sizeof(config);
		size_left -= sizeof(config);
		size += sizeof(config);

		dump_size = kbasep_mmu_dump_level(kctx, kctx->mmu.pgd, MIDGARD_MMU_TOPLEVEL,
						  &mmu_dump_buffer, &size_left);

		if (!dump_size)
			goto fail_free;

		size += dump_size;

		/* Add on the size for the end marker */
		size += sizeof(u64);

		if (size > (nr_pages * PAGE_SIZE)) {
			/* The buffer isn't big enough - free the memory and
			 * return failure
			 */
			goto fail_free;
		}

		/* Add the end marker */
		memcpy(mmu_dump_buffer, &end_marker, sizeof(u64));
	}

	rt_mutex_unlock(&kctx->mmu.mmu_lock);
	return kaddr;

fail_free:
	vfree(kaddr);
	rt_mutex_unlock(&kctx->mmu.mmu_lock);
	return NULL;
}
KBASE_EXPORT_TEST_API(kbase_mmu_dump);
#endif /* CONFIG_MALI_VECTOR_DUMP */

void kbase_mmu_bus_fault_worker(struct work_struct *data)
{
	struct kbase_as *faulting_as;
	unsigned int as_no;
	struct kbase_context *kctx;
	struct kbase_device *kbdev;
	struct kbase_fault *fault;

	faulting_as = container_of(data, struct kbase_as, work_busfault);
	fault = &faulting_as->bf_data;

	/* Ensure that any pending page fault worker has completed */
	flush_work(&faulting_as->work_pagefault);

	as_no = faulting_as->number;

	kbdev = container_of(faulting_as, struct kbase_device, as[as_no]);

	/* Grab the context, already refcounted in kbase_mmu_interrupt() on
	 * flagging of the bus-fault. Therefore, it cannot be scheduled out of
	 * this AS until we explicitly release it
	 */
	kctx = kbase_ctx_sched_as_to_ctx(kbdev, as_no);
	if (!kctx) {
		atomic_dec(&kbdev->faults_pending);
		return;
	}

	/* check if we still have GPU */
	if (unlikely(!kbase_io_has_gpu(kbdev))) {
		dev_dbg(kbdev->dev, "%s: GPU has been removed", __func__);
		release_ctx(kbdev, kctx);
		atomic_dec(&kbdev->faults_pending);
		return;
	}

	if (unlikely(fault->protected_mode)) {
		kbase_mmu_report_fault_and_kill(kctx, faulting_as, "Permission failure", fault);
		kbase_mmu_hw_clear_fault(kbdev, faulting_as, KBASE_MMU_FAULT_TYPE_BUS_UNEXPECTED);
		release_ctx(kbdev, kctx);
		atomic_dec(&kbdev->faults_pending);
		return;
	}

#if MALI_USE_CSF
	/* Before the GPU power off, wait is done for the completion of
	 * in-flight MMU fault work items. So GPU is expected to remain
	 * powered up whilst the bus fault handling is being done.
	 */
	kbase_gpu_report_bus_fault_and_kill(kctx, faulting_as, fault);
#else
	/* NOTE: If GPU already powered off for suspend,
	 * we don't need to switch to unmapped
	 */
	if (!kbase_pm_context_active_handle_suspend(kbdev,
						    KBASE_PM_SUSPEND_HANDLER_DONT_REACTIVATE)) {
		kbase_gpu_report_bus_fault_and_kill(kctx, faulting_as, fault);
		kbase_pm_context_idle(kbdev);
	}
#endif

	release_ctx(kbdev, kctx);

	atomic_dec(&kbdev->faults_pending);
}

void kbase_flush_mmu_wqs(struct kbase_device *kbdev)
{
	int i;

	for (i = 0; i < kbdev->nr_hw_address_spaces; i++) {
		struct kbase_as *as = &kbdev->as[i];

		flush_workqueue(as->pf_wq);
	}
}
