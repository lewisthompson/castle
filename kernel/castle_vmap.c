#undef CONFIG_TRACK_DIRTY_PAGES

#include <linux/sched.h>
#include <linux/list.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>
#include "castle.h"
#include "castle_debug.h"
#include "castle_utils.h"
#include "castle_vmap.h"

//#define DEBUG
#ifndef DEBUG
#define debug(_f, ...)           ((void)0)
#else
#define debug(_f, _a...)         (castle_printk(LOG_DEBUG, "%s:%.4d: " _f, __FILE__, __LINE__ , ##_a))
#endif

#define CASTLE_VMAP_FREELIST_INITIAL    2       /* Initial number of per-bucket freelist slots */
#define CASTLE_VMAP_FREELIST_MULTI      2       /* Freelist grow multiplier */

#define CASTLE_SLOT_INVALID             0xFAFAFAFA

#define SLOT_SIZE(idx)                 (1 << idx) /* Convert slot (order no) to slot size */

#define get_freelist_head(bucket_index)                                                            \
            (list_first_entry(castle_vmap_fast_maps_ptr+bucket_index, castle_vmap_freelist_t, list))

/* One struct per freelist, multiple freelists per bucket */
typedef struct castle_vmap_freelist {
    struct list_head    list;                   /* List of freelists for this bucket */
    uint32_t            *freelist;              /* The freelist */
    void                *vstart;                /* Start vaddr */
    void                *vend;                  /* End vaddr */
    int                 nr_slots;               /* No of slots in this freelist */
    int                 slots_free;             /* No of free slots in this freelist */
} castle_vmap_freelist_t;

/* This is the bucket of linked lists of freelists. Array is oversized,
so that we can index by allocation order number, e.g. index 8 = size 256 (2^8) */
struct list_head                    castle_vmap_fast_maps[CASTLE_VMAP_MAX_ORDER+1];
struct list_head                    *castle_vmap_fast_maps_ptr = castle_vmap_fast_maps;

static union {
    spinlock_t lock;
    char cacheline_pad[L1_CACHE_BYTES]; /* places each spinlock on its own cacheline */
} castle_vmap_lock[CASTLE_VMAP_MAX_ORDER + 1];

static castle_vmap_freelist_t       *castle_vmap_freelist_init(int slot_size, int slots);
static void                         castle_vmap_freelist_delete(castle_vmap_freelist_t
                                                                *castle_vmap_freelist);
static void                         castle_vmap_freelist_add(castle_vmap_freelist_t
                                                             *castle_vmap_freelist, uint32_t id);
static uint32_t                     castle_vmap_freelist_get(castle_vmap_freelist_t
                                                             *castle_vmap_freelist);
static void                         castle_vmap_freelist_grow(int freelist_bucket_idx, int slots);

/**********************************************************************************************
 * Utilities for vmapping/vunmapping pages. Assumes that virtual address to map/unmap is known.
 * Copied from RHEL mm/vmalloc.c.
 */

/* For Xen, reimplement the hypercall macros with a hardcoded hypercall_page address. */

#ifdef CONFIG_XEN
//char hypercall_page[PAGE_SIZE]; /* this is here solely to prevent external references */

#define CASTLE_HYPERCALL_STR(name)                              \
    "call 0xffffffff80206000 + ("STR(__HYPERVISOR_##name)" * 32)"

#define _castle_hypercall3(type, name, a1, a2, a3)              \
({                                                              \
    long __res, __ign1, __ign2, __ign3;                         \
    asm volatile (                                              \
        CASTLE_HYPERCALL_STR(name)                              \
        : "=a" (__res), "=D" (__ign1), "=S" (__ign2),           \
        "=d" (__ign3)                                           \
        : "1" ((long)(a1)), "2" ((long)(a2)),                   \
        "3" ((long)(a3))                                        \
        : "memory" );                                           \
    (type)__res;                                                \
})

#define _castle_hypercall4(type, name, a1, a2, a3, a4)          \
({                                                              \
    long __res, __ign1, __ign2, __ign3;                         \
    asm volatile (                                              \
        "movq %7,%%r10; "                                       \
        CASTLE_HYPERCALL_STR(name)                              \
        : "=a" (__res), "=D" (__ign1), "=S" (__ign2),           \
        "=d" (__ign3)                                           \
        : "1" ((long)(a1)), "2" ((long)(a2)),                   \
        "3" ((long)(a3)), "g" ((long)(a4))                      \
        : "memory", "r10" );                                    \
    (type)__res;                                                \
})

static inline int
castle_HYPERVISOR_update_va_mapping(unsigned long va, pte_t new_val, unsigned long flags)
{
    return _castle_hypercall3(int, update_va_mapping, va, new_val.pte, flags);
}

static inline int
castle_HYPERVISOR_mmu_update(mmu_update_t *req, int count, int *success_count, domid_t domid)
{
    return _castle_hypercall4(int, mmu_update, req, count, success_count, domid);
}

static inline int
castle_HYPERVISOR_mmuext_op(struct mmuext_op *op, int count, int *success_count, domid_t domid)
{
    return _castle_hypercall4(int, mmuext_op, op, count, success_count, domid);
}
#endif

/* Our version of flush_tlb_kernel_range(), needed by unmap_vm_area(). */

#ifdef CONFIG_XEN
static void castle_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_TLB_FLUSH_ALL;
    BUG_ON(castle_HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}
#else
static void castle_do_flush_tlb_all(void* info)
{
    unsigned long cpu = smp_processor_id();

    __flush_tlb_all();
    if (read_pda(mmu_state) == TLBSTATE_LAZY) {
        cpu_clear(cpu, read_pda(active_mm)->cpu_vm_mask);
        load_cr3(init_mm.pgd);
    }
}

static void castle_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
    on_each_cpu(castle_do_flush_tlb_all, NULL, 1, 1);
}
#endif

/* The following three functions are needed under Xen by set_{pgd,pud,pmd}(), which are in
 * turn needed by {pgd,pud,pmd}_clear(). */

#ifdef CONFIG_XEN
void xen_l2_entry_update(pmd_t *ptr, pmd_t val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val.pmd;
    BUG_ON(castle_HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

void xen_l3_entry_update(pud_t *ptr, pud_t val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val.pud;
    BUG_ON(castle_HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

void xen_l4_entry_update(pgd_t *ptr, pgd_t val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val.pgd;
    BUG_ON(castle_HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}
#endif

/* The following three functions are needed by {pgd,pud,pmd}_none_or_clear_bad(). */

void pgd_clear_bad(pgd_t *pgd)
{
    pgd_ERROR(*pgd);
    pgd_clear(pgd);
}

void pud_clear_bad(pud_t *pud)
{
    pud_ERROR(*pud);
    pud_clear(pud);
}

void pmd_clear_bad(pmd_t *pmd)
{
    pmd_ERROR(*pmd);
    pmd_clear(pmd);
}

/* Our version of unmap_vm_area() and friends. */

static void castle_vunmap_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end)
{
    pte_t *pte;

    pte = pte_offset_kernel(pmd, addr);
    do {
        pte_t ptent = ptep_get_and_clear(&init_mm, addr, pte);
        WARN_ON(!pte_none(ptent) && !pte_present(ptent));
    } while (pte++, addr += PAGE_SIZE, addr != end);
}

static inline void castle_vunmap_pmd_range(pud_t *pud, unsigned long addr, unsigned long end)
{
    pmd_t *pmd;
    unsigned long next;

    pmd = pmd_offset(pud, addr);
    do {
        next = pmd_addr_end(addr, end);
        if (pmd_none_or_clear_bad(pmd))
            BUG();
        castle_vunmap_pte_range(pmd, addr, next);
    } while (pmd++, addr = next, addr != end);
}

static void castle_vunmap_pud_range(pgd_t *pgd, unsigned long addr, unsigned long end)
{
    pud_t *pud;
    unsigned long next;

    pud = pud_offset(pgd, addr);
    do {
        next = pud_addr_end(addr, end);
        if (pud_none_or_clear_bad(pud))
            BUG();
        castle_vunmap_pmd_range(pud, addr, next);
    } while (pud++, addr = next, addr != end);
}

static void castle_unmap_vm_area(void *addr_p, int nr_pages)
{
    pgd_t *pgd;
    unsigned long next;
    unsigned long addr = (unsigned long) addr_p;
    unsigned long end = addr + nr_pages * PAGE_SIZE;

    BUG_ON(addr >= end);
    pgd = pgd_offset_k(addr);
    flush_cache_vunmap(addr, end);
    do {
        next = pgd_addr_end(addr, end);
        if (pgd_none_or_clear_bad(pgd))
            BUG();
        castle_vunmap_pud_range(pgd, addr, next);
    } while (pgd++, addr = next, addr != end);
    castle_flush_tlb_kernel_range((unsigned long) addr_p, end);
}

/* Our version of map_vm_area() and friends. */

#ifdef CONFIG_XEN
#undef set_pte_at
#define set_pte_at(_mm,addr,ptep,pteval) do {                           \
    if (((_mm) != current->mm && (_mm) != &init_mm) ||                  \
        castle_HYPERVISOR_update_va_mapping((addr), (pteval), 0))       \
            set_pte((ptep), (pteval));                                  \
} while (0)
#endif

static inline pud_t *castle_pud_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long address)
{
    BUG_ON(pgd_none(*pgd));
    return pud_offset(pgd, address);
}

static inline pmd_t *castle_pmd_alloc(struct mm_struct *mm, pud_t *pud, unsigned long address)
{
    BUG_ON(pud_none(*pud));
    return pmd_offset(pud, address);
}

static inline pte_t *castle_pte_alloc_kernel(pmd_t *pmd, unsigned long address)
{
    BUG_ON(!pmd_present(*pmd));
    return pte_offset_kernel(pmd, address);
}

static int castle_vmap_pte_range(pmd_t *pmd, unsigned long addr,
                                 unsigned long end, pgprot_t prot, struct page ***pages)
{
    pte_t *pte;

    pte = castle_pte_alloc_kernel(pmd, addr);
    if (!pte)
        return -ENOMEM;
    do {
        struct page *page = **pages;
        WARN_ON(!pte_none(*pte));
        if (!page)
            return -ENOMEM;
        set_pte_at(&init_mm, addr, pte, mk_pte(page, prot));
        (*pages)++;
    } while (pte++, addr += PAGE_SIZE, addr != end);
    return 0;
}

static inline int castle_vmap_pmd_range(pud_t *pud, unsigned long addr,
                                        unsigned long end, pgprot_t prot, struct page ***pages)
{
    pmd_t *pmd;
    unsigned long next;

    pmd = castle_pmd_alloc(&init_mm, pud, addr);
    if (!pmd)
        return -ENOMEM;
    do {
        next = pmd_addr_end(addr, end);
        if (castle_vmap_pte_range(pmd, addr, next, prot, pages))
            return -ENOMEM;
    } while (pmd++, addr = next, addr != end);
    return 0;
}

static int castle_vmap_pud_range(pgd_t *pgd, unsigned long addr,
                                 unsigned long end, pgprot_t prot, struct page ***pages)
{
    pud_t *pud;
    unsigned long next;

    pud = castle_pud_alloc(&init_mm, pgd, addr);
    if (!pud)
        return -ENOMEM;
    do {
        next = pud_addr_end(addr, end);
        if (castle_vmap_pmd_range(pud, addr, next, prot, pages))
            return -ENOMEM;
    } while (pud++, addr = next, addr != end);
    return 0;
}

static int castle_map_vm_area(void *addr_p, struct page **pages, int nr_pages, pgprot_t prot)
{
    pgd_t *pgd;
    unsigned long next;
    unsigned long addr = (unsigned long) addr_p;
    unsigned long end = addr + nr_pages * PAGE_SIZE;
    int err;

    BUG_ON(addr >= end);
    pgd = pgd_offset_k(addr);
    do {
        next = pgd_addr_end(addr, end);
        err = castle_vmap_pud_range(pgd, addr, next, prot, &pages);
        if (err)
            break;
    } while (pgd++, addr = next, addr != end);
    flush_cache_vmap((unsigned long) addr_p, end);
    return err;
}

/**********************************************************************************************
 * Castle fast vmap implementation.
 */

int castle_vmap_fast_map_init(void)
{
    int     freelist_bucket_idx;

    /* The fast vmap unit sizes are 2^1 through 2^8 */
    for (freelist_bucket_idx=1; freelist_bucket_idx<=CASTLE_VMAP_MAX_ORDER; freelist_bucket_idx++)
    {
        castle_vmap_freelist_t * castle_vmap_freelist;

        castle_vmap_freelist = castle_vmap_freelist_init(SLOT_SIZE(freelist_bucket_idx),
                                                         CASTLE_VMAP_FREELIST_INITIAL);

        if (!castle_vmap_freelist)
        {
            debug("castle_vmap_fast_map_init freelist vmalloc/vmap failure\n");
            /* Before we error out, delete the freelists successfully allocated so far, if any */
            while (--freelist_bucket_idx)
            {
                castle_vmap_freelist_t *freelist;

                freelist = get_freelist_head (freelist_bucket_idx);

                castle_vmap_freelist_delete(freelist);
                castle_free(freelist);
            }
            return -ENOMEM;
        }

        /* Put at the head of the bucket for this size mapper */
        INIT_LIST_HEAD(castle_vmap_fast_maps_ptr+freelist_bucket_idx);
        list_add(&castle_vmap_freelist->list, castle_vmap_fast_maps_ptr+freelist_bucket_idx);

        /* Init the spinlock for this bucket */
        castle_vmap_lock[freelist_bucket_idx].lock = __SPIN_LOCK_UNLOCKED(castle_vmap_lock[freelist_bucket_idx].lock);
    }

    return EXIT_SUCCESS;
}

void castle_vmap_fast_map_fini(void)
{
    int                     freelist_bucket_idx;
    castle_vmap_freelist_t  *castle_vmap_freelist;

    /* Index through each of the freelist buckets. For each bucket, delete the list of freelists */
    for (freelist_bucket_idx=1; freelist_bucket_idx<=CASTLE_VMAP_MAX_ORDER; freelist_bucket_idx++)
    {
        castle_vmap_freelist = get_freelist_head(freelist_bucket_idx);
        BUG_ON(!(list_is_singular(castle_vmap_fast_maps_ptr+freelist_bucket_idx)));
        castle_vmap_freelist_delete(castle_vmap_freelist);
        castle_free(castle_vmap_freelist);
    }
}

#define DUMMY_PAGE_SHIFT 15 /* Max number of vmap pages we can back per dummy page */

static castle_vmap_freelist_t *castle_vmap_freelist_init(int slot_size, int slots)
{
    int                     nr_vmap_array_pages, nr_dummy_pages;
    struct page             **pgs_array;
    struct page             **dummy_pages;
    castle_vmap_freelist_t  *castle_vmap_freelist;
    int i;

    castle_vmap_freelist = castle_alloc(sizeof(castle_vmap_freelist_t));

    if (!castle_vmap_freelist)
        goto errout_1;

    /* Each array is sized to include 'slots' entries, plus a canary page between
       each entry. For n entries of size p this is (n * p) + n - 1 pages */
    nr_vmap_array_pages = (slots * slot_size + slots - 1);
    pgs_array = castle_alloc(nr_vmap_array_pages * sizeof(struct page *));
    if(!pgs_array)
        goto errout_2;

    /* Due to a restriction on early versions of xen, there is a limit to the number of times
       a page can be used as backing for a vmap (dummy pages). We will use each dummy page a
       maximum of 1<<DUMMY_PAGE_SHIFT times to workaround this limit */
    nr_dummy_pages = (nr_vmap_array_pages>>DUMMY_PAGE_SHIFT) + 1;
    dummy_pages = castle_zalloc(nr_dummy_pages * sizeof(struct page *));
    if(!dummy_pages)
        goto errout_3;

    /* Freelist contains one slot per mapping, plus one extra as an end-of-freelist marker */
    castle_vmap_freelist->freelist = castle_alloc((slots + 1) * sizeof(uint32_t));

    if(!castle_vmap_freelist->freelist)
        goto errout_4;

    memset(castle_vmap_freelist->freelist, 0xFA, (slots + 1) * sizeof(uint32_t));

    castle_vmap_freelist->slots_free = 0;

    /* Populate the pages in pgs_array. This can be any arbitrary 'dummy' page - they are only
       used to allow vmap() to create us a vm area, and we subsequently unmap them all anyway. */
    for (i=0;i<nr_dummy_pages;i++)
    {
        dummy_pages[i] = alloc_page(GFP_KERNEL);
        if (!dummy_pages[i])
            goto errout_5;
    }
    for (i=0; i<nr_vmap_array_pages; i++)
        pgs_array[i] = dummy_pages[i>>DUMMY_PAGE_SHIFT];

    castle_vmap_freelist->vstart = vmap(pgs_array, nr_vmap_array_pages,
                    VM_READ|VM_WRITE, PAGE_KERNEL);

    if (!castle_vmap_freelist->vstart)
    {
        castle_printk(LOG_ERROR, "Failed to vmap %d pages (freelist slot_size %d)\n",
                      nr_vmap_array_pages, slot_size);
        goto errout_5;
    }

    castle_vmap_freelist->vend = castle_vmap_freelist->vstart + nr_vmap_array_pages * PAGE_SIZE;
    castle_vmap_freelist->nr_slots = slots;

    /* This gives as an area in virtual memory in which we'll keep mapping multi-page objects. In
       order for this to work we need to unmap all the pages, but trick the kernel vmalloc code into
       not deallocating the vm_area_struct describing our virtual memory region. */
    castle_unmap_vm_area(castle_vmap_freelist->vstart, nr_vmap_array_pages);

    /* Init the actual freelist. This needs to contain ids which will always put us within the vmap
       area created above. */
    for(i=0; i<slots; i++)
        castle_vmap_freelist_add(castle_vmap_freelist, i);

    for (i=0;i<nr_dummy_pages;i++)
        __free_page(dummy_pages[i]);

    castle_free(dummy_pages);

    castle_free(pgs_array);

    return castle_vmap_freelist;

errout_5:
    for (i=0;i<nr_dummy_pages;i++)
        if (dummy_pages[i])
            __free_page(dummy_pages[i]);
    castle_free(castle_vmap_freelist->freelist);
errout_4:
    castle_free(dummy_pages);
errout_3:
    castle_free(pgs_array);
errout_2:
    castle_free(castle_vmap_freelist);
errout_1:
    return NULL;
}

static void castle_vmap_freelist_delete(castle_vmap_freelist_t *castle_vmap_freelist)
{
#ifdef CASTLE_DEBUG
{
    /* At this point there should be nothing mapped in the fast vmap areas for this freelist.
       When in debug mode, verify that the freelist contains the correct number of items */
    int i = 0;
    while(castle_vmap_freelist->freelist[0] < castle_vmap_freelist->nr_slots)
    {
        castle_vmap_freelist_get(castle_vmap_freelist);
        i++;
    }
    BUG_ON(i != castle_vmap_freelist->nr_slots);
}
#endif
    castle_free(castle_vmap_freelist->freelist);
    /* Let vmalloc.c destroy vm_area_struct by vmunmping it. */
    vunmap(castle_vmap_freelist->vstart);
}

static void castle_vmap_freelist_add(castle_vmap_freelist_t *castle_vmap_freelist, uint32_t id)
{
    BUG_ON(castle_vmap_freelist->freelist[id+1] != CASTLE_SLOT_INVALID);
    castle_vmap_freelist->freelist[id+1] = castle_vmap_freelist->freelist[0];
    castle_vmap_freelist->freelist[0]    = id;
    castle_vmap_freelist->slots_free++;
}

/* Should be called with the vmap lock held */
static uint32_t castle_vmap_freelist_get(castle_vmap_freelist_t *castle_vmap_freelist)
{
    uint32_t id;

    id = castle_vmap_freelist->freelist[0];
    if (id >= castle_vmap_freelist->nr_slots)
    {
        /* Last slot has been used. Return CASTLE_SLOT_INVALID. Caller will grow freelist and retry */
        BUG_ON(id != CASTLE_SLOT_INVALID);
        goto out;
    }

    castle_vmap_freelist->freelist[0] = castle_vmap_freelist->freelist[id+1];
    /* Invalidate the slot we've just allocated, so that we can test for double frees */
    castle_vmap_freelist->freelist[id+1] = CASTLE_SLOT_INVALID;
    castle_vmap_freelist->slots_free--;

out:
    return id;
}

/* This used to require the vmap lock to be held. Now locking is done internally. Caller should
   lock the pages, though. */
void *castle_vmap_fast_map(struct page **pgs, int nr_pages)
{
    uint32_t                vmap_slot;
    void                    *vaddr;
    int                     freelist_bucket_idx=0;
    castle_vmap_freelist_t  *castle_vmap_freelist;
    int need_slots;

    freelist_bucket_idx = order_base_2(nr_pages);
    while(1)
    {

        /* grab the lock for the bucket */
        spin_lock(&castle_vmap_lock[freelist_bucket_idx].lock);

        /* We always map from the freelist at the head of the bucket */
        castle_vmap_freelist = get_freelist_head(freelist_bucket_idx);
        vmap_slot = castle_vmap_freelist_get(castle_vmap_freelist);

        if (vmap_slot == CASTLE_SLOT_INVALID)
        {
            /* need to grow the freelist */
            need_slots = castle_vmap_freelist->nr_slots;
            /* 1. drop the lock on the bucket */
            spin_unlock(&castle_vmap_lock[freelist_bucket_idx].lock);
            /* 2. grow the freelist */
            castle_vmap_freelist_grow(freelist_bucket_idx, need_slots);
            /* 3. and retry */
            continue;
        }
        vaddr = castle_vmap_freelist->vstart + vmap_slot * PAGE_SIZE * (SLOT_SIZE(freelist_bucket_idx)+1);
        /* release the lock for the bucket */
        spin_unlock(&castle_vmap_lock[freelist_bucket_idx].lock);
        break; /* we have what we want, break out of the loop */
    }

#ifdef CASTLE_DEBUG
    BUG_ON((unsigned long)vaddr <  (unsigned long)castle_vmap_freelist->vstart);
    BUG_ON((unsigned long)vaddr >= (unsigned long)castle_vmap_freelist->vend);
#endif

    if(castle_map_vm_area(vaddr, pgs, nr_pages, PAGE_KERNEL))
    {
        debug("ERROR: failed to vmap!\n");

        /* put the vaddr range back */
        spin_lock(&castle_vmap_lock[freelist_bucket_idx].lock);
        castle_vmap_freelist_add(castle_vmap_freelist, vmap_slot);
        spin_unlock(&castle_vmap_lock[freelist_bucket_idx].lock);

        vaddr = NULL;
    }

    return vaddr;
}

void castle_vmap_fast_unmap(void *vaddr, int nr_pages)
{
    castle_vmap_freelist_t  *castle_vmap_freelist;
    int                     freelist_bucket_idx=0;
    uint32_t                vmap_slot;
    struct list_head        *pos;
    int need_release_list = 0;

    freelist_bucket_idx = order_base_2(nr_pages);

    /* first unmap the vm area since we are putting this back */
    castle_unmap_vm_area(vaddr, nr_pages);
    spin_lock(&castle_vmap_lock[freelist_bucket_idx].lock);

    /* We could be unmapping from any freelist in the bucket */
    list_for_each(pos, castle_vmap_fast_maps_ptr+freelist_bucket_idx)
    {
        castle_vmap_freelist = list_entry(pos, castle_vmap_freelist_t, list);
        /* Is it in this freelist? */
        if ((vaddr >= castle_vmap_freelist->vstart) && (vaddr < castle_vmap_freelist->vend))
        {
            vmap_slot = (vaddr - castle_vmap_freelist->vstart) /
                        ((SLOT_SIZE(freelist_bucket_idx)+1) * PAGE_SIZE);
            castle_vmap_freelist_add(castle_vmap_freelist, vmap_slot);

            /* If the add made this freelist completely free, and this freelist is not at the head
               of the bucket (i.e. not active for gets, then delete this freelist. */
            if ((castle_vmap_freelist->slots_free == castle_vmap_freelist->nr_slots) &&
                (castle_vmap_freelist != get_freelist_head(freelist_bucket_idx)))
            {
                list_del(pos);
                need_release_list = 1;
            }
            spin_unlock(&castle_vmap_lock[freelist_bucket_idx].lock);
            debug("Released fast vmap slot: %d\n", vmap_slot);

            if(need_release_list)
            {
                castle_vmap_freelist_delete(castle_vmap_freelist);
                castle_free(castle_vmap_freelist);
            }

            return;
        }
    }
    debug("Unmap: could not find freelist\n");
    BUG();
}

/**
 * Grow a vmap freelist, by inserting a new, larger freelist at the head of the bucket.
 *
 * @param castle_vmap_freelist  The old freelist to be re-checked under the bucket lock.
 * @param freelist_bucket_idx   The freelist bucket index to be grown.
 * @param slots                 The number of slots to grow to.
 */
static void castle_vmap_freelist_grow(int freelist_bucket_idx, int slots)
{
    castle_vmap_freelist_t  *new;
    castle_vmap_freelist_t *castle_vmap_freelist;
    int want_slots;

    debug("Adding new freelist of %d slots for bucket size %d\n",
          slots * CASTLE_VMAP_FREELIST_MULTI, SLOT_SIZE(freelist_bucket_idx));

    want_slots = slots * CASTLE_VMAP_FREELIST_MULTI;
    new = castle_vmap_freelist_init(SLOT_SIZE(freelist_bucket_idx), want_slots);

    if (!new)
        BUG(); /* failure to get vmem area is fatal */

    /* Drop new map if we raced or if the active map is empty.  Only the active
     * map (at the head of the list) handles allocations.  Maps get freed up
     * when all of their slots become free so we must not install a new active
     * map over an existing empty map, or it will not be freed. */
    spin_lock(&castle_vmap_lock[freelist_bucket_idx].lock);
    castle_vmap_freelist = get_freelist_head(freelist_bucket_idx);
    if (castle_vmap_freelist->nr_slots >= want_slots
            || castle_vmap_freelist->nr_slots == castle_vmap_freelist->slots_free)
    {
        debug("Dropping new list for freelist bucket index %d\n", freelist_bucket_idx);
        spin_unlock(&castle_vmap_lock[freelist_bucket_idx].lock);
        castle_vmap_freelist_delete(new);
        castle_free(new);
        return;
    }
    list_add(&new->list, castle_vmap_fast_maps_ptr+freelist_bucket_idx);
    spin_unlock(&castle_vmap_lock[freelist_bucket_idx].lock);
}
