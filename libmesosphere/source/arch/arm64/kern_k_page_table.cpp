/*
 * Copyright (c) 2018-2020 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <mesosphere.hpp>

namespace ams::kern::arch::arm64 {

    void KPageTable::Initialize(s32 core_id) {
        /* Nothing actually needed here. */
    }

    Result KPageTable::InitializeForKernel(void *table, KVirtualAddress start, KVirtualAddress end) {
        /* Initialize basic fields. */
        this->asid = 0;
        this->manager = std::addressof(Kernel::GetPageTableManager());

        /* Allocate a page for ttbr. */
        const u64 asid_tag = (static_cast<u64>(this->asid) << 48ul);
        const KVirtualAddress page = this->manager->Allocate();
        MESOSPHERE_ASSERT(page != Null<KVirtualAddress>);
        cpu::ClearPageToZero(GetVoidPointer(page));
        this->ttbr = GetInteger(KPageTableBase::GetLinearPhysicalAddress(page)) | asid_tag;

        /* Initialize the base page table. */
        MESOSPHERE_R_ABORT_UNLESS(KPageTableBase::InitializeForKernel(true, table, start, end));

        return ResultSuccess();
    }

    Result KPageTable::Finalize() {
        MESOSPHERE_TODO_IMPLEMENT();
    }

    Result KPageTable::Operate(PageLinkedList *page_list, KProcessAddress virt_addr, size_t num_pages, KPhysicalAddress phys_addr, bool is_pa_valid, const KPageProperties properties, OperationType operation, bool reuse_ll) {
        /* Check validity of parameters. */
        MESOSPHERE_ASSERT(this->IsLockedByCurrentThread());
        MESOSPHERE_ASSERT(num_pages > 0);
        MESOSPHERE_ASSERT(util::IsAligned(GetInteger(virt_addr), PageSize));
        MESOSPHERE_ASSERT(this->ContainsPages(virt_addr, num_pages));

        if (operation == OperationType_Map) {
            MESOSPHERE_ABORT_UNLESS(is_pa_valid);
            MESOSPHERE_ASSERT(util::IsAligned(GetInteger(phys_addr), PageSize));
        } else {
            MESOSPHERE_ABORT_UNLESS(!is_pa_valid);
        }

        if (operation == OperationType_Unmap) {
            MESOSPHERE_TODO("operation == OperationType_Unmap");
        } else {
            auto entry_template = this->GetEntryTemplate(properties);

            switch (operation) {
                case OperationType_Map:
                    return this->MapContiguous(virt_addr, phys_addr, num_pages, entry_template, page_list, reuse_ll);
                MESOSPHERE_UNREACHABLE_DEFAULT_CASE();
            }
        }
    }

    Result KPageTable::Operate(PageLinkedList *page_list, KProcessAddress virt_addr, size_t num_pages, const KPageGroup *page_group, const KPageProperties properties, OperationType operation, bool reuse_ll) {
        MESOSPHERE_TODO_IMPLEMENT();
    }

    Result KPageTable::Map(KProcessAddress virt_addr, KPhysicalAddress phys_addr, size_t num_pages, PageTableEntry entry_template, PageLinkedList *page_list, bool reuse_ll) {
        MESOSPHERE_ASSERT(this->IsLockedByCurrentThread());
        MESOSPHERE_ASSERT(util::IsAligned(GetInteger(virt_addr), PageSize));
        MESOSPHERE_ASSERT(util::IsAligned(GetInteger(phys_addr), PageSize));

        auto &impl = this->GetImpl();
        KVirtualAddress l2_virt = Null<KVirtualAddress>;
        KVirtualAddress l3_virt = Null<KVirtualAddress>;
        int l2_open_count = 0;
        int l3_open_count = 0;

        /* Iterate, mapping each page. */
        for (size_t i = 0; i < num_pages; i++) {
            KPhysicalAddress l3_phys = Null<KPhysicalAddress>;
            bool l2_allocated = false;

            /* If we have no L3 table, we should get or allocate one. */
            if (l3_virt == Null<KVirtualAddress>) {
                KPhysicalAddress l2_phys = Null<KPhysicalAddress>;

                /* If we have no L2 table, we should get or allocate one. */
                if (l2_virt == Null<KVirtualAddress>) {
                    if (L1PageTableEntry *l1_entry = impl.GetL1Entry(virt_addr); !l1_entry->GetTable(l2_phys)) {
                        /* Allocate table. */
                        l2_virt = AllocatePageTable(page_list, reuse_ll);
                        R_UNLESS(l2_virt != Null<KVirtualAddress>, svc::ResultOutOfResource());

                        /* Set the entry. */
                        l2_phys = GetPageTablePhysicalAddress(l2_virt);
                        PteDataSynchronizationBarrier();
                        *l1_entry = L1PageTableEntry(l2_phys, this->IsKernel(), true);
                        PteDataSynchronizationBarrier();
                        l2_allocated = true;
                    } else {
                        l2_virt = GetPageTableVirtualAddress(l2_phys);
                    }
                }
                MESOSPHERE_ASSERT(l2_virt != Null<KVirtualAddress>);

                if (L2PageTableEntry *l2_entry = impl.GetL2EntryFromTable(l2_virt, virt_addr); !l2_entry->GetTable(l3_phys)) {
                        /* Allocate table. */
                        l3_virt = AllocatePageTable(page_list, reuse_ll);
                        if (l3_virt == Null<KVirtualAddress>) {
                            /* Cleanup the L2 entry. */
                            if (l2_allocated) {
                                *impl.GetL1Entry(virt_addr) = InvalidL1PageTableEntry;
                                this->NoteUpdated();
                                FreePageTable(page_list, l2_virt);
                            } else if (this->GetPageTableManager().IsInPageTableHeap(l2_virt) && l2_open_count > 0) {
                                this->GetPageTableManager().Open(l2_virt, l2_open_count);
                            }
                            return svc::ResultOutOfResource();
                        }

                        /* Set the entry. */
                        l3_phys = GetPageTablePhysicalAddress(l3_virt);
                        PteDataSynchronizationBarrier();
                        *l2_entry = L2PageTableEntry(l3_phys, this->IsKernel(), true);
                        PteDataSynchronizationBarrier();
                        l2_open_count++;
                } else {
                    l3_virt = GetPageTableVirtualAddress(l3_phys);
                }
            }
            MESOSPHERE_ASSERT(l3_virt != Null<KVirtualAddress>);

            /* Map the page. */
            *impl.GetL3EntryFromTable(l3_virt, virt_addr) = L3PageTableEntry(phys_addr, entry_template, false);
            l3_open_count++;
            virt_addr += PageSize;
            phys_addr += PageSize;

            /* Account for hitting end of table. */
            if (util::IsAligned(GetInteger(virt_addr), L2BlockSize)) {
                if (this->GetPageTableManager().IsInPageTableHeap(l3_virt)) {
                    this->GetPageTableManager().Open(l3_virt, l3_open_count);
                }
                l3_virt = Null<KVirtualAddress>;
                l3_open_count = 0;

                if (util::IsAligned(GetInteger(virt_addr), L1BlockSize)) {
                    if (this->GetPageTableManager().IsInPageTableHeap(l2_virt) && l2_open_count > 0) {
                        this->GetPageTableManager().Open(l2_virt, l2_open_count);
                    }
                    l2_virt = Null<KVirtualAddress>;
                    l2_open_count = 0;
                }
            }
        }

        /* Perform any remaining opens. */
        if (l2_open_count > 0 && this->GetPageTableManager().IsInPageTableHeap(l2_virt)) {
            this->GetPageTableManager().Open(l2_virt, l2_open_count);
        }
        if (l3_open_count > 0 && this->GetPageTableManager().IsInPageTableHeap(l3_virt)) {
            this->GetPageTableManager().Open(l3_virt, l3_open_count);
        }

        return ResultSuccess();
    }

    Result KPageTable::Unmap(KProcessAddress virt_addr, size_t num_pages, KPageGroup *pg, PageLinkedList *page_list, bool force, bool reuse_ll) {
        MESOSPHERE_TODO_IMPLEMENT();
    }

    Result KPageTable::MapContiguous(KProcessAddress virt_addr, KPhysicalAddress phys_addr, size_t num_pages, PageTableEntry entry_template, PageLinkedList *page_list, bool reuse_ll) {
        MESOSPHERE_ASSERT(this->IsLockedByCurrentThread());

        /* Cache initial addresses for use on cleanup. */
        const KProcessAddress  orig_virt_addr = virt_addr;
        const KPhysicalAddress orig_phys_addr = phys_addr;

        size_t remaining_pages = num_pages;

        if (num_pages < ContiguousPageSize / PageSize) {
            auto guard = SCOPE_GUARD { MESOSPHERE_R_ABORT_UNLESS(this->Unmap(orig_virt_addr, num_pages, nullptr, page_list, true, true)); };
            R_TRY(this->Map(virt_addr, phys_addr, num_pages, entry_template, page_list, reuse_ll));
            guard.Cancel();
        } else {
            MESOSPHERE_TODO("Contiguous mapping");
            (void)remaining_pages;
        }

        /* Perform what coalescing we can. */
        this->MergePages(orig_virt_addr, page_list);
        if (num_pages > 1) {
            this->MergePages(orig_virt_addr + (num_pages - 1) * PageSize, page_list);
        }

        /* Open references to the pages, if we should. */
        if (IsHeapPhysicalAddress(orig_phys_addr)) {
            Kernel::GetMemoryManager().Open(GetHeapVirtualAddress(orig_phys_addr), num_pages);
        }

        return ResultSuccess();
    }

    bool KPageTable::MergePages(KProcessAddress virt_addr, PageLinkedList *page_list) {
        MESOSPHERE_ASSERT(this->IsLockedByCurrentThread());

        auto &impl = this->GetImpl();
        bool merged = false;

        /* If there's no L1 table, don't bother. */
        L1PageTableEntry *l1_entry = impl.GetL1Entry(virt_addr);
        if (!l1_entry->IsTable()) {
            return merged;
        }

        /* Examine and try to merge the L2 table. */
        L2PageTableEntry *l2_entry = impl.GetL2Entry(l1_entry, virt_addr);
        if (l2_entry->IsTable()) {
            /* We have an L3 entry. */
            L3PageTableEntry *l3_entry = impl.GetL3Entry(l2_entry, virt_addr);
            if (!l3_entry->IsBlock() || !l3_entry->IsContiguousAllowed()) {
                return merged;
            }

            /* If it's not contiguous, try to make it so. */
            if (!l3_entry->IsContiguous()) {
                virt_addr = util::AlignDown(GetInteger(virt_addr), L3ContiguousBlockSize);
                KPhysicalAddress phys_addr = util::AlignDown(GetInteger(l3_entry->GetBlock()), L3ContiguousBlockSize);
                const u64 entry_template = l3_entry->GetEntryTemplate();

                /* Validate that we can merge. */
                for (size_t i = 0; i < L3ContiguousBlockSize / L3BlockSize; i++) {
                    if (!impl.GetL3Entry(l2_entry, virt_addr + L3BlockSize * i)->Is(entry_template | GetInteger(phys_addr + PageSize * i) | PageTableEntry::Type_L3Block)) {
                        return merged;
                    }
                }

                /* Merge! */
                for (size_t i = 0; i < L3ContiguousBlockSize / L3BlockSize; i++) {
                    impl.GetL3Entry(l2_entry, virt_addr + L3BlockSize * i)->SetContiguous(true);
                }

                /* Note that we updated. */
                this->NoteUpdated();
                merged = true;
            }

            /* We might be able to upgrade a contiguous set of L3 entries into an L2 block. */
            virt_addr = util::AlignDown(GetInteger(virt_addr), L2BlockSize);
            KPhysicalAddress phys_addr = util::AlignDown(GetInteger(l3_entry->GetBlock()), L2BlockSize);
            const u64 entry_template = l3_entry->GetEntryTemplate();

            /* Validate that we can merge. */
            for (size_t i = 0; i < L2BlockSize / L3ContiguousBlockSize; i++) {
                if (!impl.GetL3Entry(l2_entry, virt_addr + L3BlockSize * i)->Is(entry_template | GetInteger(phys_addr + L3ContiguousBlockSize * i) | PageTableEntry::ContigType_Contiguous)) {
                    return merged;
                }
            }

            /* Merge! */
            PteDataSynchronizationBarrier();
            *l2_entry = L2PageTableEntry(phys_addr, entry_template, false);

            /* Note that we updated. */
            this->NoteUpdated();
            merged = true;

            /* Free the L3 table. */
            KVirtualAddress l3_table = util::AlignDown(reinterpret_cast<uintptr_t>(l3_entry), PageSize);
            if (this->GetPageTableManager().IsInPageTableHeap(l3_table)) {
                this->GetPageTableManager().Close(l3_table, L2BlockSize / L3BlockSize);
                this->FreePageTable(page_list, l3_table);
            }
        }
        if (l2_entry->IsBlock()) {
            /* If it's not contiguous, try to make it so. */
            if (!l2_entry->IsContiguous()) {
                virt_addr = util::AlignDown(GetInteger(virt_addr), L2ContiguousBlockSize);
                KPhysicalAddress phys_addr = util::AlignDown(GetInteger(l2_entry->GetBlock()), L2ContiguousBlockSize);
                const u64 entry_template = l2_entry->GetEntryTemplate();

                /* Validate that we can merge. */
                for (size_t i = 0; i < L2ContiguousBlockSize / L2BlockSize; i++) {
                    if (!impl.GetL2Entry(l1_entry, virt_addr + L2BlockSize * i)->Is(entry_template | GetInteger(phys_addr + PageSize * i) | PageTableEntry::Type_L2Block)) {
                        return merged;
                    }
                }

                /* Merge! */
                for (size_t i = 0; i < L2ContiguousBlockSize / L2BlockSize; i++) {
                    impl.GetL2Entry(l1_entry, virt_addr + L2BlockSize * i)->SetContiguous(true);
                }

                /* Note that we updated. */
                this->NoteUpdated();
                merged = true;
            }

            /* We might be able to upgrade a contiguous set of L2 entries into an L1 block. */
            virt_addr = util::AlignDown(GetInteger(virt_addr), L1BlockSize);
            KPhysicalAddress phys_addr = util::AlignDown(GetInteger(l2_entry->GetBlock()), L1BlockSize);
            const u64 entry_template = l2_entry->GetEntryTemplate();

            /* Validate that we can merge. */
            for (size_t i = 0; i < L1BlockSize / L2ContiguousBlockSize; i++) {
                if (!impl.GetL2Entry(l1_entry, virt_addr + L3BlockSize * i)->Is(entry_template | GetInteger(phys_addr + L2ContiguousBlockSize * i) | PageTableEntry::ContigType_Contiguous)) {
                    return merged;
                }
            }

            /* Merge! */
            PteDataSynchronizationBarrier();
            *l1_entry = L1PageTableEntry(phys_addr, entry_template, false);

            /* Note that we updated. */
            this->NoteUpdated();
            merged = true;

            /* Free the L2 table. */
            KVirtualAddress l2_table = util::AlignDown(reinterpret_cast<uintptr_t>(l2_entry), PageSize);
            if (this->GetPageTableManager().IsInPageTableHeap(l2_table)) {
                this->GetPageTableManager().Close(l2_table, L1BlockSize / L2BlockSize);
                this->FreePageTable(page_list, l2_table);
            }
        }

        return merged;
    }

    void KPageTable::FinalizeUpdate(PageLinkedList *page_list) {
        while (page_list->Peek()) {
            KVirtualAddress page = KVirtualAddress(page_list->Pop());
            MESOSPHERE_ASSERT(this->GetPageTableManager().IsInPageTableHeap(page));
            MESOSPHERE_ASSERT(this->GetPageTableManager().GetRefCount(page) == 0);
            this->GetPageTableManager().Free(page);
        }
    }

}