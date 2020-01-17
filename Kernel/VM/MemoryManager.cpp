#include "CMOS.h"
#include "Process.h"
#include "StdLib.h"
#include <AK/Assertions.h>
#include <AK/kstdio.h>
#include <Kernel/Arch/i386/CPU.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/Multiboot.h>
#include <Kernel/VM/AnonymousVMObject.h>
#include <Kernel/VM/InodeVMObject.h>
#include <Kernel/VM/MemoryManager.h>
#include <Kernel/VM/PurgeableVMObject.h>

//#define MM_DEBUG
//#define PAGE_FAULT_DEBUG

static MemoryManager* s_the;

MemoryManager& MM
{
    return *s_the;
}

MemoryManager::MemoryManager()
{
    m_kernel_page_directory = PageDirectory::create_kernel_page_directory();

    initialize_paging();

    kprintf("MM initialized.\n");
}

MemoryManager::~MemoryManager()
{
}

void MemoryManager::initialize_paging()
{
    if (!g_cpu_supports_pae) {
        kprintf("x86: Cannot boot on machines without PAE support.\n");
        hang();
    }

#ifdef MM_DEBUG
    dbgprintf("MM: Kernel page directory @ %p\n", kernel_page_directory().cr3());
#endif

#if 0
    // Disable writing to the kernel text and rodata segments.
    extern u32 start_of_kernel_text;
    extern u32 start_of_kernel_data;
    for (size_t i = (u32)&start_of_kernel_text; i < (u32)&start_of_kernel_data; i += PAGE_SIZE) {
        auto& pte = ensure_pte(kernel_page_directory(), VirtualAddress(i));
        pte.set_writable(false);
    }

    if (g_cpu_supports_nx) {
        // Disable execution of the kernel data and bss segments.
        extern u32 end_of_kernel_bss;
        for (size_t i = (u32)&start_of_kernel_data; i < (u32)&end_of_kernel_bss; i += PAGE_SIZE) {
            auto& pte = ensure_pte(kernel_page_directory(), VirtualAddress(i));
            pte.set_execute_disabled(true);
        }
    }
#endif

    m_quickmap_addr = VirtualAddress(0xffe00000);
#ifdef MM_DEBUG
    dbgprintf("MM: Quickmap will use %p\n", m_quickmap_addr.get());
#endif

    parse_memory_map();

#ifdef MM_DEBUG
    dbgprintf("MM: Installing page directory\n");
#endif

    // Turn on CR4.PAE
    asm volatile(
        "mov %cr4, %eax\n"
        "orl $0x20, %eax\n"
        "mov %eax, %cr4\n");

    if (g_cpu_supports_pge) {
        // Turn on CR4.PGE so the CPU will respect the G bit in page tables.
        asm volatile(
            "mov %cr4, %eax\n"
            "orl $0x80, %eax\n"
            "mov %eax, %cr4\n");
        kprintf("x86: PGE support enabled\n");
    } else {
        kprintf("x86: PGE support not detected\n");
    }

    if (g_cpu_supports_smep) {
        // Turn on CR4.SMEP
        asm volatile(
            "mov %cr4, %eax\n"
            "orl $0x100000, %eax\n"
            "mov %eax, %cr4\n");
        kprintf("x86: SMEP support enabled\n");
    } else {
        kprintf("x86: SMEP support not detected\n");
    }

    if (g_cpu_supports_smap) {
        // Turn on CR4.SMAP
        kprintf("x86: Enabling SMAP\n");
        asm volatile(
            "mov %cr4, %eax\n"
            "orl $0x200000, %eax\n"
            "mov %eax, %cr4\n");
        kprintf("x86: SMAP support enabled\n");
    } else {
        kprintf("x86: SMAP support not detected\n");
    }

    if (g_cpu_supports_nx) {
        // Turn on IA32_EFER.NXE
        asm volatile(
            "movl $0xc0000080, %ecx\n"
            "rdmsr\n"
            "orl $0x800, %eax\n"
            "wrmsr\n");
        kprintf("x86: NX support enabled\n");
    } else {
        kprintf("x86: NX support not detected\n");
    }

    asm volatile("movl %%eax, %%cr3" ::"a"(kernel_page_directory().cr3()));
    asm volatile(
        "movl %%cr0, %%eax\n"
        "orl $0x80010001, %%eax\n"
        "movl %%eax, %%cr0\n" ::
            : "%eax", "memory");

    setup_low_1mb();

#ifdef MM_DEBUG
    dbgprintf("MM: Paging initialized.\n");
#endif
}

void MemoryManager::setup_low_1mb()
{
    m_low_page_table = allocate_supervisor_physical_page();

    auto* pd_zero = quickmap_pd(kernel_page_directory(), 0);
    pd_zero[1].set_present(false);
    pd_zero[2].set_present(false);
    pd_zero[3].set_present(false);

    auto& pde_zero = pd_zero[0];
    pde_zero.set_page_table_base(m_low_page_table->paddr().get());
    pde_zero.set_present(true);
    pde_zero.set_huge(false);
    pde_zero.set_writable(true);
    pde_zero.set_user_allowed(false);
    if (g_cpu_supports_nx)
        pde_zero.set_execute_disabled(true);

    for (u32 offset = 0; offset < (2 * MB); offset += PAGE_SIZE) {
        auto& page_table_page = m_low_page_table;
        auto& pte = quickmap_pt(page_table_page->paddr())[offset / PAGE_SIZE];
        pte.set_physical_page_base(offset);
        pte.set_user_allowed(false);
        pte.set_present(offset != 0);
        pte.set_writable(offset < (1 * MB));
    }
}

void MemoryManager::parse_memory_map()
{
    RefPtr<PhysicalRegion> region;
    bool region_is_super = false;

    auto* mmap = (multiboot_memory_map_t*)(low_physical_to_virtual(multiboot_info_ptr->mmap_addr));
    for (; (unsigned long)mmap < (low_physical_to_virtual(multiboot_info_ptr->mmap_addr)) + (multiboot_info_ptr->mmap_length); mmap = (multiboot_memory_map_t*)((unsigned long)mmap + mmap->size + sizeof(mmap->size))) {
        kprintf("MM: Multiboot mmap: base_addr = 0x%x%08x, length = 0x%x%08x, type = 0x%x\n",
            (u32)(mmap->addr >> 32),
            (u32)(mmap->addr & 0xffffffff),
            (u32)(mmap->len >> 32),
            (u32)(mmap->len & 0xffffffff),
            (u32)mmap->type);

        if (mmap->type != MULTIBOOT_MEMORY_AVAILABLE)
            continue;

        // FIXME: Maybe make use of stuff below the 1MB mark?
        if (mmap->addr < (1 * MB))
            continue;

        if ((mmap->addr + mmap->len) > 0xffffffff)
            continue;

        auto diff = (u32)mmap->addr % PAGE_SIZE;
        if (diff != 0) {
            kprintf("MM: got an unaligned region base from the bootloader; correcting %p by %d bytes\n", mmap->addr, diff);
            diff = PAGE_SIZE - diff;
            mmap->addr += diff;
            mmap->len -= diff;
        }
        if ((mmap->len % PAGE_SIZE) != 0) {
            kprintf("MM: got an unaligned region length from the bootloader; correcting %d by %d bytes\n", mmap->len, mmap->len % PAGE_SIZE);
            mmap->len -= mmap->len % PAGE_SIZE;
        }
        if (mmap->len < PAGE_SIZE) {
            kprintf("MM: memory region from bootloader is too small; we want >= %d bytes, but got %d bytes\n", PAGE_SIZE, mmap->len);
            continue;
        }

#ifdef MM_DEBUG
        kprintf("MM: considering memory at %p - %p\n",
            (u32)mmap->addr, (u32)(mmap->addr + mmap->len));
#endif

        for (size_t page_base = mmap->addr; page_base < (mmap->addr + mmap->len); page_base += PAGE_SIZE) {
            auto addr = PhysicalAddress(page_base);

            if (page_base < 7 * MB) {
                // nothing
            } else if (page_base >= 7 * MB && page_base < 8 * MB) {
                if (region.is_null() || !region_is_super || region->upper().offset(PAGE_SIZE) != addr) {
                    m_super_physical_regions.append(PhysicalRegion::create(addr, addr));
                    region = m_super_physical_regions.last();
                    region_is_super = true;
                } else {
                    region->expand(region->lower(), addr);
                }
            } else {
                if (region.is_null() || region_is_super || region->upper().offset(PAGE_SIZE) != addr) {
                    m_user_physical_regions.append(PhysicalRegion::create(addr, addr));
                    region = m_user_physical_regions.last();
                    region_is_super = false;
                } else {
                    region->expand(region->lower(), addr);
                }
            }
        }
    }

    for (auto& region : m_super_physical_regions)
        m_super_physical_pages += region.finalize_capacity();

    for (auto& region : m_user_physical_regions)
        m_user_physical_pages += region.finalize_capacity();
}

PageTableEntry& MemoryManager::ensure_pte(PageDirectory& page_directory, VirtualAddress vaddr)
{
    ASSERT_INTERRUPTS_DISABLED();
    u32 page_directory_table_index = (vaddr.get() >> 30) & 0x3;
    u32 page_directory_index = (vaddr.get() >> 21) & 0x1ff;
    u32 page_table_index = (vaddr.get() >> 12) & 0x1ff;

    auto* pd = quickmap_pd(page_directory, page_directory_table_index);
    PageDirectoryEntry& pde = pd[page_directory_index];
    if (!pde.is_present()) {
#ifdef MM_DEBUG
        dbgprintf("MM: PDE %u not present (requested for V%p), allocating\n", page_directory_index, vaddr.get());
#endif
        auto page_table = allocate_supervisor_physical_page();
#ifdef MM_DEBUG
        dbgprintf("MM: PD K%p (%s) at P%p allocated page table #%u (for V%p) at P%p\n",
            &page_directory,
            &page_directory == m_kernel_page_directory ? "Kernel" : "User",
            page_directory.cr3(),
            page_directory_index,
            vaddr.get(),
            page_table->paddr().get());
#endif
        pde.set_page_table_base(page_table->paddr().get());
        pde.set_user_allowed(true);
        pde.set_present(true);
        pde.set_writable(true);
        pde.set_global(&page_directory == m_kernel_page_directory.ptr());
        page_directory.m_physical_pages.set(page_directory_index, move(page_table));
    }

    return quickmap_pt(PhysicalAddress((u32)pde.page_table_base()))[page_table_index];
}

void MemoryManager::map_protected(VirtualAddress vaddr, size_t length)
{
    InterruptDisabler disabler;
    ASSERT(vaddr.is_page_aligned());
    for (u32 offset = 0; offset < length; offset += PAGE_SIZE) {
        auto pte_address = vaddr.offset(offset);
        auto& pte = ensure_pte(kernel_page_directory(), pte_address);
        pte.set_physical_page_base(pte_address.get());
        pte.set_user_allowed(false);
        pte.set_present(false);
        pte.set_writable(false);
        flush_tlb(pte_address);
    }
}

void MemoryManager::create_identity_mapping(PageDirectory& page_directory, VirtualAddress vaddr, size_t size)
{
    InterruptDisabler disabler;
    ASSERT((vaddr.get() & ~PAGE_MASK) == 0);
    for (u32 offset = 0; offset < size; offset += PAGE_SIZE) {
        auto pte_address = vaddr.offset(offset);
        auto& pte = ensure_pte(page_directory, pte_address);
        pte.set_physical_page_base(pte_address.get());
        pte.set_user_allowed(false);
        pte.set_present(true);
        pte.set_writable(true);
        flush_tlb(pte_address);
    }
}

void MemoryManager::initialize()
{
    s_the = new MemoryManager;
}

Region* MemoryManager::kernel_region_from_vaddr(VirtualAddress vaddr)
{
    if (vaddr.get() < 0xc0000000)
        return nullptr;
    for (auto& region : MM.m_kernel_regions) {
        if (region.contains(vaddr))
            return &region;
    }
    return nullptr;
}

Region* MemoryManager::user_region_from_vaddr(Process& process, VirtualAddress vaddr)
{
    // FIXME: Use a binary search tree (maybe red/black?) or some other more appropriate data structure!
    for (auto& region : process.m_regions) {
        if (region.contains(vaddr))
            return &region;
    }
    dbg() << process << " Couldn't find user region for " << vaddr;
    if (auto* kreg = kernel_region_from_vaddr(vaddr)) {
        dbg() << process << "  OTOH, there is a kernel region: " << kreg->range() << ": " << kreg->name();
    } else {
        dbg() << process << "  AND no kernel region either";
    }

    process.dump_regions();

    kprintf("Kernel regions:\n");
    kprintf("BEGIN       END         SIZE        ACCESS  NAME\n");
    for (auto& region : MM.m_kernel_regions) {
        kprintf("%08x -- %08x    %08x    %c%c%c%c%c%c    %s\n",
            region.vaddr().get(),
            region.vaddr().offset(region.size() - 1).get(),
            region.size(),
            region.is_readable() ? 'R' : ' ',
            region.is_writable() ? 'W' : ' ',
            region.is_executable() ? 'X' : ' ',
            region.is_shared() ? 'S' : ' ',
            region.is_stack() ? 'T' : ' ',
            region.vmobject().is_purgeable() ? 'P' : ' ',
            region.name().characters());
    }
    return nullptr;
}

Region* MemoryManager::region_from_vaddr(Process& process, VirtualAddress vaddr)
{
    if (auto* region = kernel_region_from_vaddr(vaddr))
        return region;
    return user_region_from_vaddr(process, vaddr);
}

const Region* MemoryManager::region_from_vaddr(const Process& process, VirtualAddress vaddr)
{
    if (auto* region = kernel_region_from_vaddr(vaddr))
        return region;
    return user_region_from_vaddr(const_cast<Process&>(process), vaddr);
}

Region* MemoryManager::region_from_vaddr(VirtualAddress vaddr)
{
    if (auto* region = kernel_region_from_vaddr(vaddr))
        return region;
    auto page_directory = PageDirectory::find_by_cr3(cpu_cr3());
    if (!page_directory)
        return nullptr;
    ASSERT(page_directory->process());
    return user_region_from_vaddr(*page_directory->process(), vaddr);
}

PageFaultResponse MemoryManager::handle_page_fault(const PageFault& fault)
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(current);
#ifdef PAGE_FAULT_DEBUG
    dbgprintf("MM: handle_page_fault(%w) at V%p\n", fault.code(), fault.vaddr().get());
#endif
    ASSERT(fault.vaddr() != m_quickmap_addr);
    auto* region = region_from_vaddr(fault.vaddr());
    if (!region) {
        kprintf("NP(error) fault at invalid address V%p\n", fault.vaddr().get());
        return PageFaultResponse::ShouldCrash;
    }

    return region->handle_fault(fault);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region(size_t size, const StringView& name, u8 access, bool user_accessible, bool should_commit, bool cacheable)
{
    InterruptDisabler disabler;
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    ASSERT(range.is_valid());
    OwnPtr<Region> region;
    if (user_accessible)
        region = Region::create_user_accessible(range, name, access, cacheable);
    else
        region = Region::create_kernel_only(range, name, access, cacheable);
    region->set_page_directory(kernel_page_directory());
    // FIXME: It would be cool if these could zero-fill on demand instead.
    if (should_commit)
        region->commit();
    return region;
}

OwnPtr<Region> MemoryManager::allocate_kernel_region(PhysicalAddress paddr, size_t size, const StringView& name, u8 access, bool user_accessible, bool cacheable)
{
    InterruptDisabler disabler;
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    ASSERT(range.is_valid());
    OwnPtr<Region> region;
    if (user_accessible)
        region = Region::create_user_accessible(range, AnonymousVMObject::create_for_physical_range(paddr, size), 0, name, access, cacheable);
    else
        region = Region::create_kernel_only(range, AnonymousVMObject::create_for_physical_range(paddr, size), 0, name, access, cacheable);
    region->map(kernel_page_directory());
    return region;
}

OwnPtr<Region> MemoryManager::allocate_user_accessible_kernel_region(size_t size, const StringView& name, u8 access, bool cacheable)
{
    return allocate_kernel_region(size, name, access, true, true, cacheable);
}

OwnPtr<Region> MemoryManager::allocate_kernel_region_with_vmobject(VMObject& vmobject, size_t size, const StringView& name, u8 access, bool user_accessible, bool cacheable)
{
    InterruptDisabler disabler;
    ASSERT(!(size % PAGE_SIZE));
    auto range = kernel_page_directory().range_allocator().allocate_anywhere(size);
    ASSERT(range.is_valid());
    OwnPtr<Region> region;
    if (user_accessible)
        region = Region::create_user_accessible(range, vmobject, 0, name, access, cacheable);
    else
        region = Region::create_kernel_only(range, vmobject, 0, name, access, cacheable);
    region->map(kernel_page_directory());
    return region;
}

void MemoryManager::deallocate_user_physical_page(PhysicalPage&& page)
{
    for (auto& region : m_user_physical_regions) {
        if (!region.contains(page)) {
            kprintf(
                "MM: deallocate_user_physical_page: %p not in %p -> %p\n",
                page.paddr().get(), region.lower().get(), region.upper().get());
            continue;
        }

        region.return_page(move(page));
        --m_user_physical_pages_used;

        return;
    }

    kprintf("MM: deallocate_user_physical_page couldn't figure out region for user page @ %p\n", page.paddr().get());
    ASSERT_NOT_REACHED();
}

RefPtr<PhysicalPage> MemoryManager::find_free_user_physical_page()
{
    RefPtr<PhysicalPage> page;
    for (auto& region : m_user_physical_regions) {
        page = region.take_free_page(false);
        if (!page.is_null())
            break;
    }
    return page;
}

RefPtr<PhysicalPage> MemoryManager::allocate_user_physical_page(ShouldZeroFill should_zero_fill)
{
    InterruptDisabler disabler;
    RefPtr<PhysicalPage> page = find_free_user_physical_page();

    if (!page) {
        if (m_user_physical_regions.is_empty()) {
            kprintf("MM: no user physical regions available (?)\n");
        }

        for_each_vmobject([&](auto& vmobject) {
            if (vmobject.is_purgeable()) {
                auto& purgeable_vmobject = static_cast<PurgeableVMObject&>(vmobject);
                int purged_page_count = purgeable_vmobject.purge_with_interrupts_disabled({});
                if (purged_page_count) {
                    kprintf("MM: Purge saved the day! Purged %d pages from PurgeableVMObject{%p}\n", purged_page_count, &purgeable_vmobject);
                    page = find_free_user_physical_page();
                    ASSERT(page);
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        });

        if (!page) {
            kprintf("MM: no user physical pages available\n");
            ASSERT_NOT_REACHED();
            return {};
        }
    }

#ifdef MM_DEBUG
    dbgprintf("MM: allocate_user_physical_page vending P%p\n", page->paddr().get());
#endif

    if (should_zero_fill == ShouldZeroFill::Yes) {
        auto* ptr = (u32*)quickmap_page(*page);
        memset(ptr, 0, PAGE_SIZE);
        unquickmap_page();
    }

    ++m_user_physical_pages_used;
    return page;
}

void MemoryManager::deallocate_supervisor_physical_page(PhysicalPage&& page)
{
    for (auto& region : m_super_physical_regions) {
        if (!region.contains(page)) {
            kprintf(
                "MM: deallocate_supervisor_physical_page: %p not in %p -> %p\n",
                page.paddr().get(), region.lower().get(), region.upper().get());
            continue;
        }

        region.return_page(move(page));
        --m_super_physical_pages_used;
        return;
    }

    kprintf("MM: deallocate_supervisor_physical_page couldn't figure out region for super page @ %p\n", page.paddr().get());
    ASSERT_NOT_REACHED();
}

RefPtr<PhysicalPage> MemoryManager::allocate_supervisor_physical_page()
{
    InterruptDisabler disabler;
    RefPtr<PhysicalPage> page;

    for (auto& region : m_super_physical_regions) {
        page = region.take_free_page(true);
        if (page.is_null())
            continue;
    }

    if (!page) {
        if (m_super_physical_regions.is_empty()) {
            kprintf("MM: no super physical regions available (?)\n");
        }

        kprintf("MM: no super physical pages available\n");
        ASSERT_NOT_REACHED();
        return {};
    }

#ifdef MM_DEBUG
    dbgprintf("MM: allocate_supervisor_physical_page vending P%p\n", page->paddr().get());
#endif

    fast_u32_fill((u32*)page->paddr().offset(0xc0000000).as_ptr(), 0, PAGE_SIZE / sizeof(u32));
    ++m_super_physical_pages_used;
    return page;
}

void MemoryManager::enter_process_paging_scope(Process& process)
{
    ASSERT(current);
    InterruptDisabler disabler;

    current->tss().cr3 = process.page_directory().cr3();
    asm volatile("movl %%eax, %%cr3" ::"a"(process.page_directory().cr3())
                 : "memory");
}

void MemoryManager::flush_entire_tlb()
{
    asm volatile(
        "mov %%cr3, %%eax\n"
        "mov %%eax, %%cr3\n" ::
            : "%eax", "memory");
}

void MemoryManager::flush_tlb(VirtualAddress vaddr)
{
#ifdef MM_DEBUG
    dbgprintf("MM: Flush page V%p\n", vaddr.get());
#endif
    asm volatile("invlpg %0"
                 :
                 : "m"(*(char*)vaddr.get())
                 : "memory");
}

extern "C" PageTableEntry boot_pd3_pde1023_pt[1024];

PageDirectoryEntry* MemoryManager::quickmap_pd(PageDirectory& directory, size_t pdpt_index)
{
    auto& pte = boot_pd3_pde1023_pt[4];
    auto pd_paddr = directory.m_directory_pages[pdpt_index]->paddr();
    if (pte.physical_page_base() != pd_paddr.as_ptr()) {
#ifdef MM_DEBUG
        dbgprintf("quickmap_pd: Mapping P%p at 0xffe04000 in pte @ %p\n", directory.m_directory_pages[pdpt_index]->paddr().as_ptr(), &pte);
#endif
        pte.set_physical_page_base(pd_paddr.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);
        flush_tlb(VirtualAddress(0xffe04000));
    }
    return (PageDirectoryEntry*)0xffe04000;
}

PageTableEntry* MemoryManager::quickmap_pt(PhysicalAddress pt_paddr)
{
    auto& pte = boot_pd3_pde1023_pt[8];
    if (pte.physical_page_base() != pt_paddr.as_ptr()) {
#ifdef MM_DEBUG
        dbgprintf("quickmap_pt: Mapping P%p at 0xffe08000 in pte @ %p\n", pt_paddr.as_ptr(), &pte);
#endif
        pte.set_physical_page_base(pt_paddr.get());
        pte.set_present(true);
        pte.set_writable(true);
        pte.set_user_allowed(false);
        flush_tlb(VirtualAddress(0xffe08000));
    }
    return (PageTableEntry*)0xffe08000;
}

void MemoryManager::map_for_kernel(VirtualAddress vaddr, PhysicalAddress paddr, bool cache_disabled)
{
    auto& pte = ensure_pte(kernel_page_directory(), vaddr);
    pte.set_physical_page_base(paddr.get());
    pte.set_present(true);
    pte.set_writable(true);
    pte.set_user_allowed(false);
    pte.set_cache_disabled(cache_disabled);
    flush_tlb(vaddr);
}

u8* MemoryManager::quickmap_page(PhysicalPage& physical_page)
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(!m_quickmap_in_use);
    m_quickmap_in_use = true;
    auto page_vaddr = m_quickmap_addr;
    auto& pte = ensure_pte(kernel_page_directory(), page_vaddr);
    pte.set_physical_page_base(physical_page.paddr().get());
    pte.set_present(true);
    pte.set_writable(true);
    pte.set_user_allowed(false);
    flush_tlb(page_vaddr);
    ASSERT((u32)pte.physical_page_base() == physical_page.paddr().get());
#ifdef MM_DEBUG
    dbg() << "MM: >> quickmap_page " << page_vaddr << " => " << physical_page.paddr() << " @ PTE=" << (void*)pte.raw() << " {" << &pte << "}";
#endif
    return page_vaddr.as_ptr();
}

void MemoryManager::unquickmap_page()
{
    ASSERT_INTERRUPTS_DISABLED();
    ASSERT(m_quickmap_in_use);
    auto page_vaddr = m_quickmap_addr;
    auto& pte = ensure_pte(kernel_page_directory(), page_vaddr);
#ifdef MM_DEBUG
    auto old_physical_address = pte.physical_page_base();
#endif
    pte.set_physical_page_base(0);
    pte.set_present(false);
    pte.set_writable(false);
    flush_tlb(page_vaddr);
#ifdef MM_DEBUG
    dbg() << "MM: >> unquickmap_page " << page_vaddr << " =/> " << old_physical_address;
#endif
    m_quickmap_in_use = false;
}

template<MemoryManager::AccessSpace space, MemoryManager::AccessType access_type>
bool MemoryManager::validate_range(const Process& process, VirtualAddress base_vaddr, size_t size) const
{
    ASSERT(size);
    VirtualAddress vaddr = base_vaddr.page_base();
    VirtualAddress end_vaddr = base_vaddr.offset(size - 1).page_base();
    if (end_vaddr < vaddr) {
        dbg() << *current << " Shenanigans! Asked to validate " << base_vaddr << " size=" << size;
        return false;
    }
    const Region* region = nullptr;
    while (vaddr <= end_vaddr) {
        if (!region || !region->contains(vaddr)) {
            if (space == AccessSpace::Kernel)
                region = kernel_region_from_vaddr(vaddr);
            if (!region || !region->contains(vaddr))
                region = user_region_from_vaddr(const_cast<Process&>(process), vaddr);
            if (!region
                || (space == AccessSpace::User && !region->is_user_accessible())
                || (access_type == AccessType::Read && !region->is_readable())
                || (access_type == AccessType::Write && !region->is_writable())) {
                return false;
            }
        }
        vaddr = vaddr.offset(PAGE_SIZE);
    }
    return true;
}

bool MemoryManager::validate_user_stack(const Process& process, VirtualAddress vaddr) const
{
    if (!is_user_address(vaddr))
        return false;
    auto* region = user_region_from_vaddr(const_cast<Process&>(process), vaddr);
    return region && region->is_user_accessible() && region->is_stack();
}

bool MemoryManager::validate_kernel_read(const Process& process, VirtualAddress vaddr, size_t size) const
{
    return validate_range<AccessSpace::Kernel, AccessType::Read>(process, vaddr, size);
}

bool MemoryManager::validate_user_read(const Process& process, VirtualAddress vaddr, size_t size) const
{
    if (!is_user_address(vaddr))
        return false;
    return validate_range<AccessSpace::User, AccessType::Read>(process, vaddr, size);
}

bool MemoryManager::validate_user_write(const Process& process, VirtualAddress vaddr, size_t size) const
{
    if (!is_user_address(vaddr))
        return false;
    return validate_range<AccessSpace::User, AccessType::Write>(process, vaddr, size);
}

void MemoryManager::register_vmobject(VMObject& vmobject)
{
    InterruptDisabler disabler;
    m_vmobjects.append(&vmobject);
}

void MemoryManager::unregister_vmobject(VMObject& vmobject)
{
    InterruptDisabler disabler;
    m_vmobjects.remove(&vmobject);
}

void MemoryManager::register_region(Region& region)
{
    InterruptDisabler disabler;
    if (region.vaddr().get() >= 0xc0000000)
        m_kernel_regions.append(&region);
    else
        m_user_regions.append(&region);
}

void MemoryManager::unregister_region(Region& region)
{
    InterruptDisabler disabler;
    if (region.vaddr().get() >= 0xc0000000)
        m_kernel_regions.remove(&region);
    else
        m_user_regions.remove(&region);
}

ProcessPagingScope::ProcessPagingScope(Process& process)
{
    ASSERT(current);
    MM.enter_process_paging_scope(process);
}

ProcessPagingScope::~ProcessPagingScope()
{
    MM.enter_process_paging_scope(current->process());
}
