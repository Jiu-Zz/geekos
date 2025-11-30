/*
 * Paging (virtual memory) support
 * Copyright (c) 2003, Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 * Copyright (c) 2003,2004 David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.55 $
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/string.h>
#include <geekos/int.h>
#include <geekos/idt.h>
#include <geekos/kthread.h>
#include <geekos/kassert.h>
#include <geekos/screen.h>
#include <geekos/mem.h>
#include <geekos/malloc.h>
#include <geekos/gdt.h>
#include <geekos/segment.h>
#include <geekos/user.h>
#include <geekos/vfs.h>
#include <geekos/blockdev.h>
#include <geekos/crc32.h>
#include <geekos/paging.h>

/* ----------------------------------------------------------------------
 * Public data
 * ---------------------------------------------------------------------- */

pde_t *g_kernel_pde;
void *BitmapPaging = NULL;
struct Paging_Device *pagingDevice;
static int numOfPagingPages;

/* ----------------------------------------------------------------------
 * Private functions/data
 * ---------------------------------------------------------------------- */

#define SECTORS_PER_PAGE (PAGE_SIZE / SECTOR_SIZE)

/*
 * flag to indicate if debugging paging code
 */
int debugFaults = 0;
#define Debug(args...) \
    if (debugFaults)   \
    Print(args)

void checkPaging()
{
    unsigned long reg = 0;
    __asm__ __volatile__("movl %%cr0, %0" : "=a"(reg));
    Print("Paging on ? : %d\n", (reg & (1 << 31)) != 0);
}

/*
 * Print diagnostic information for a page fault.
 */
static void Print_Fault_Info(uint_t address, faultcode_t faultCode)
{
    extern uint_t g_freePageCount;

    Print("Pid %d, Page Fault received, at address %x (%d pages free)\n",
          g_currentThread->pid, address, g_freePageCount);
    if (faultCode.protectionViolation)
        Print("   Protection Violation, ");
    else
        Print("   Non-present page, ");
    if (faultCode.writeFault)
        Print("Write Fault, ");
    else
        Print("Read Fault, ");
    if (faultCode.userModeFault)
        Print("in User Mode\n");
    else
        Print("in Supervisor Mode\n");
}

/*
 * Handler for page faults.
 * You should call the Install_Interrupt_Handler() function to
 * register this function as the handler for interrupt 14.
 */
void Page_Fault_Handler(struct Interrupt_State *state)
{
    ulong_t address;
    faultcode_t faultCode;
    extern uint_t g_freePageCount;
    KASSERT(!Interrupts_Enabled());
    address = Get_Page_Fault_Address();
    Debug("Page fault @%lx\n", address);
    faultCode = *((faultcode_t *)&(state->errorCode)); /* 错误码 */
    struct User_Context *userContext = g_currentThread->userContext;
    if (faultCode.writeFault)
    { // 写错误，缺页情况为堆栈生长到新页
        int res;
        res = Alloc_User_Page(userContext->pageDir,
                              Round_Down_To_Page(address), PAGE_SIZE);
        if (res == -1)
            Exit(-1);
        return;
    }
    else
    { ////读错误，分两种缺页情况
        ulong_t page_dir_addr = address >> 22;
        ulong_t page_addr = (address << 10) >> 22;
        pde_t *page_dir_entry = (pde_t *)userContext->pageDir +
                                page_dir_addr;
        pte_t *page_entry = NULL;
        if (page_dir_entry->present)
        { // 页目录项
            page_entry = (pte_t *)((page_dir_entry->pageTableBaseAddr) << 12);
            page_entry += page_addr;
        }
        else
        { ////非法地址访问的缺页情况
            Print_Fault_Info(address, faultCode);
            Exit(-1);
        }
        if (page_entry->kernelInfo != KINFO_PAGE_ON_DISK)
        { // 页不在磁盘上
            Print_Fault_Info(address, faultCode);
            Exit(-1);
        }
        // 以下处理因为页保存在磁盘pagefile引起的缺页
        int pagefile_index = page_entry->pageBaseAddr;
        void *paddr = Alloc_Pageable_Page(page_entry,
                                          Round_Down_To_Page(address));
        if (paddr == NULL)
            Exit(-1);
        *((uint_t *)page_entry) = 0;
        page_entry->present = 1;
        page_entry->flags = VM_WRITE | VM_READ | VM_USER;
        page_entry->globalPage = 0;
        page_entry->pageBaseAddr = (ulong_t)paddr >> 12;
        Enable_Interrupts();
        Read_From_Paging_File(paddr, Round_Down_To_Page(address),
                              pagefile_index);
        Disable_Interrupts();
        Free_Space_On_Paging_File(pagefile_index);
        return;
    }
}

int Alloc_User_Page(pde_t *pageDir, uint_t startAddress, uint_t sizeInMemory)
{
    uint_t pagedir_index = startAddress >> 22;
    uint_t page_index = (startAddress << 10) >> 22;
    pde_t *pagedir_entry = pageDir + pagedir_index;
    pte_t *page_entry;
    // 第一步，建立startAddress对应的页目录表项与页
    if (pagedir_entry->present)
    { // startAddress对应的页目录表项已经建立的情况
        page_entry = (pte_t *)(pagedir_entry->pageTableBaseAddr << 12);
    }
    else
    { // startAddress对应页目录表项没有建立的情况（对应的页表没有建立）
        // 分配一个页
        page_entry = (pte_t *)Alloc_Page();
        if (page_entry == NULL)
        {
            return -1;
        }
        memset(page_entry, 0, PAGE_SIZE);
        // 设置对应的页目录表项
        *((uint_t *)pagedir_entry) = 0;
        pagedir_entry->present = 1;
        pagedir_entry->flags = VM_WRITE | VM_READ | VM_USER;
        pagedir_entry->pageTableBaseAddr = (ulong_t)page_entry >> 12;
    }
    // 找到页表中对应于startAddress的页表项
    page_entry += page_index;
    // 第二步，建立startAddress对应的页表项与页
    int num_pages;
    void *page_addr;
    // 这里算所需页数时，注意要对齐页边界
    num_pages = Round_Up_To_Page(startAddress -
                                 Round_Down_To_Page(startAddress) + sizeInMemory) /
                PAGE_SIZE;
    int i;
    uint_t first_page_addr = 0;
    for (i = 0; i < num_pages; i++)
    {
        // 对应的页表项没有建立的情况（此时意味着对应的页没有建立）
        if (!page_entry->present)
        {
            page_addr = Alloc_Pageable_Page(page_entry,
                                            Round_Down_To_Page(startAddress));
            if (page_addr == NULL)
            {
                return -1;
            }
            // 设置页表项
            *((uint_t *)page_entry) = 0;
            page_entry->present = 1;
            page_entry->flags = VM_WRITE | VM_READ | VM_USER;
            page_entry->globalPage = 0;
            page_entry->pageBaseAddr = (ulong_t)page_addr >> 12;
            KASSERT(page_addr != 0);
            if (i == 0)
            {
                first_page_addr = (uint_t)page_addr;
            }
        }
        page_entry++;
        startAddress += PAGE_SIZE;
    }
    return 0;
}

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

/*
 * Initialize virtual memory by building page tables
 * for the kernel and physical memory.
 */
void Init_VM(struct Boot_Info *bootInfo)
{
    // 计算内核页目录中要多少个目录项
    int num_dir_entries = (bootInfo->memSizeKB / 4) / NUM_PAGE_TABLE_ENTRIES + 1;
    g_kernel_pde = Alloc_Page(); // 为内核页目录分配一页空间
    if (g_kernel_pde == NULL)
        KASSERT(0);
    memset(g_kernel_pde, '\0', PAGE_SIZE);
    pte_t *first_pte;
    int i = 0, j;
    uint_t mem;
    for (i = 0; i < num_dir_entries; i++)
    {
        g_kernel_pde[i].flags = VM_WRITE | VM_USER;
        g_kernel_pde[i].present = 1;
        first_pte = Alloc_Page(); ////为页表分配一页空间
        if (first_pte == NULL)
            KASSERT(0);
        memset(first_pte, '\0', PAGE_SIZE);
        g_kernel_pde[i].pageTableBaseAddr = ((uint_t)first_pte) >> 12;
        int j;
        mem = i * NUM_PAGE_TABLE_ENTRIES * PAGE_SIZE;
        for (j = 0; j < NUM_PAGE_TABLE_ENTRIES; j++)
        {
            first_pte[j].present = 1;
            first_pte[j].flags = VM_WRITE;
            first_pte[j].pageBaseAddr = mem >> 12;
            mem += PAGE_SIZE;
        }
    }
    i = 1019;
    g_kernel_pde[i].present = 1;
    g_kernel_pde[i].flags = VM_WRITE;
    first_pte = Alloc_Page();
    if (first_pte == NULL)
        KASSERT(0);
    g_kernel_pde[i].pageTableBaseAddr = ((uint_t)first_pte) >> 12;
    memset(first_pte, '\0', PAGE_SIZE);
    mem = i * NUM_PAGE_TABLE_ENTRIES * (long long)PAGE_SIZE;
    for (j = 0; j < NUM_PAGE_TABLE_ENTRIES; j++)
    {
        first_pte[j].present = 1;
        first_pte[j].flags = VM_WRITE | VM_USER;
        first_pte[j].pageBaseAddr = mem >> 12;
        mem += PAGE_SIZE;
    }
    Enable_Paging(g_kernel_pde);
    Install_Interrupt_Handler(14, Page_Fault_Handler);
    Install_Interrupt_Handler(46, Page_Fault_Handler);
}

/**
 * Initialize paging file data structures.
 * All filesystems should be mounted before this function
 * is called, to ensure that the paging file is available.
 */
void Init_Paging(void)
{
    pagingDevice = Get_Paging_Device();
    if (pagingDevice == NULL)
        KASSERT(0);
    numOfPagingPages = pagingDevice->numSectors / SECTORS_PER_PAGE;
    BitmapPaging = Create_Bit_Set(numOfPagingPages);
}

/**
 * Find a free bit of disk on the paging file for this page.
 * Interrupts must be disabled.
 * @return index of free page sized chunk of disk space in
 *   the paging file, or -1 if the paging file is full
 */
int Find_Space_On_Paging_File(void)
{
    KASSERT(!Interrupts_Enabled());
    return Find_First_Free_Bit(BitmapPaging, numOfPagingPages);
}

/**
 * Free a page-sized chunk of disk space in the paging file.
 * Interrupts must be disabled.
 * @param pagefileIndex index of the chunk of disk space
 */
void Free_Space_On_Paging_File(int pagefileIndex)
{
    KASSERT(!Interrupts_Enabled());
    KASSERT(0 <= pagefileIndex && pagefileIndex < numOfPagingPages);
    Clear_Bit(BitmapPaging, pagefileIndex);
}

/**
 * Write the contents of given page to the indicated block
 * of space in the paging file.
 * @param paddr a pointer to the physical memory of the page
 * @param vaddr virtual address where page is mapped in user memory
 * @param pagefileIndex the index of the page sized chunk of space
 *   in the paging file
 */
void Write_To_Paging_File(void *paddr, ulong_t vaddr, int pagefileIndex)
{
    struct Page *page = Get_Page((ulong_t)paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE)); // 必须锁定
    KASSERT((page->flags & PAGE_LOCKED));
    int i;
    if (0 <= pagefileIndex && pagefileIndex < numOfPagingPages)
    {
        for (i = 0; i < SECTORS_PER_PAGE; i++)
        {
            Block_Write(pagingDevice->dev,
                        pagefileIndex * SECTORS_PER_PAGE + i + (pagingDevice->startSector),
                        paddr + i * SECTOR_SIZE);
        }
        Set_Bit(BitmapPaging, pagefileIndex);
    }
    else
        KASSERT(0);
}

/**
 * Read the contents of the indicated block
 * of space in the paging file into the given page.
 * @param paddr a pointer to the physical memory of the page
 * @param vaddr virtual address where page will be re-mapped in
 *   user memory
 * @param pagefileIndex the index of the page sized chunk of space
 *   in the paging file
 */
void Read_From_Paging_File(void *paddr, ulong_t vaddr, int pagefileIndex)
{
    struct Page *page = Get_Page((ulong_t)paddr);
    KASSERT(!(page->flags & PAGE_PAGEABLE)); /* Page must be locked! */
    int i;
    if (0 <= pagefileIndex && pagefileIndex < numOfPagingPages)
        for (i = 0; i < SECTORS_PER_PAGE; i++)
        {
            Block_Read(pagingDevice->dev,
                       pagefileIndex * SECTORS_PER_PAGE + i + (pagingDevice->startSector),
                       paddr + i * SECTOR_SIZE);
        }
    else
        KASSERT(0);
}

