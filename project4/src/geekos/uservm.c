/*
 * Paging-based user mode implementation
 * Copyright (c) 2003,2004 David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.50 $
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/int.h>
#include <geekos/mem.h>
#include <geekos/paging.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/argblock.h>
#include <geekos/kthread.h>
#include <geekos/range.h>
#include <geekos/vfs.h>
#include <geekos/user.h>

int userDebug = 0;
extern pde_t *g_kernel_pde;

/* ----------------------------------------------------------------------
 * Private functions
 * ---------------------------------------------------------------------- */

// TODO: Add private functions
struct User_Context *Create_User_Context()
{
    struct User_Context *user_context;
    user_context = (struct User_Context *)Malloc(sizeof(struct
                                                        User_Context));
    if (user_context == NULL)
    {
        return NULL;
    }
    user_context->ldtDescriptor = NULL;
    user_context->memory = NULL;
    user_context->size = 0;
    user_context->ldtSelector = 0;
    user_context->csSelector = 0;
    user_context->dsSelector = 0;
    user_context->pageDir = NULL;
    user_context->entryAddr = 0;
    user_context->argBlockAddr = 0;
    user_context->stackPointerAddr = 0;
    user_context->refCount = 0;
    return user_context;
}

void Free_User_Pages(struct User_Context *userContext)
{
    pde_t *pageDirectory = userContext->pageDir;
    int i, j;
    for (i = 512; i <= 1018; i++)
    {
        pde_t *cur_pde = pageDirectory + i;
        if (cur_pde->present == 1)
        {
            pte_t *pageTable = (pte_t *)(cur_pde->pageTableBaseAddr << 12);
            for (j = 0; j < 1024; j++)
            {
                pte_t *cur_pte = pageTable + j;
                if (cur_pte->present == 1)
                {
                    if (cur_pte->kernelInfo != KINFO_PAGE_ON_DISK)
                        Free_Page((void *)((uint_t)cur_pte->pageBaseAddr << 12));
                    else
                        Free_Space_On_Paging_File(cur_pte->pageBaseAddr);
                }
            }
            Free_Page(pageTable);
        }
    }
    Free_Page(pageDirectory);
}

uint_t lin_to_phyaddr(pde_t *page_dir, uint_t lin_address)
{
    uint_t pagedir_index = lin_address >> 22;
    uint_t page_index = (lin_address << 10) >> 22;
    uint_t offset_address = lin_address & 0xfff;
    pde_t *pagedir_entry = page_dir + pagedir_index;
    pte_t *page_entry = 0;
    if (pagedir_entry->present)
    {
        page_entry = (pte_t *)((uint_t)pagedir_entry->pageTableBaseAddr << 12);
        page_entry += page_index;
        return (page_entry->pageBaseAddr << 12) + offset_address;
    }
    else
    {
        return 0;
    }
}

bool Copy_User_Page(pde_t *page_dir, uint_t dest_user, char *src, uint_t byte_num)
{
    uint_t phy_start;
    uint_t temp_len;
    int page_nums;
    struct Page *page;
    if (Round_Down_To_Page(dest_user + byte_num) ==
        Round_Down_To_Page(dest_user))
    {
        temp_len = byte_num;
        page_nums = 1;
    }
    else
    {
        temp_len = Round_Up_To_Page(dest_user) - dest_user;
        byte_num -= temp_len;
        page_nums = 0;
    }
    phy_start = lin_to_phyaddr(page_dir, dest_user);
    if (phy_start == 0)
    {
        return false;
    }
    page = Get_Page(phy_start);
    Disable_Interrupts();
    page->flags &= ~PAGE_PAGEABLE;
    Enable_Interrupts();
    memcpy((char *)phy_start, src, temp_len);
    page->flags |= PAGE_PAGEABLE;
    if (page_nums == 1)
    {
        return true;
    }
    temp_len = Round_Up_To_Page(dest_user) - dest_user;
    dest_user += temp_len;
    src += temp_len;
    byte_num -= temp_len;
    while (dest_user != Round_Down_To_Page(dest_user + byte_num))
    {
        phy_start = lin_to_phyaddr(page_dir, dest_user);
        if (phy_start == 0)
        {
            return false;
        }
        page = Get_Page(phy_start);
        Disable_Interrupts();
        page->flags &= ~PAGE_PAGEABLE;
        Enable_Interrupts();
        memcpy((char *)phy_start, src, PAGE_SIZE);
        page->flags |= PAGE_PAGEABLE;
        dest_user += PAGE_SIZE;
        byte_num -= PAGE_SIZE;
        src += PAGE_SIZE;
    }
    phy_start = lin_to_phyaddr(page_dir, dest_user);
    if (phy_start == 0)
    {
        return false;
    }
    Disable_Interrupts();
    page->flags &= ~PAGE_PAGEABLE;
    Enable_Interrupts();
    memcpy((char *)phy_start, src, byte_num);
    page->flags |= PAGE_PAGEABLE;
    return true;
}

/* ----------------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------------- */

/*
 * Destroy a User_Context object, including all memory
 * and other resources allocated within it.
 */
void Destroy_User_Context(struct User_Context *context)
{
    KASSERT(context->refCount == 0);
    /* Free the context's LDT descriptor */
    Free_Segment_Descriptor(context->ldtDescriptor);
    bool iflag;
    iflag = Begin_Int_Atomic();
    //--destroy page table, page dir，free all pages
    Free_User_Pages(context);
    Free(context);
    End_Int_Atomic(iflag);
}

/*
 * Load a user executable into memory by creating a User_Context
 * data structure.
 * Params:
 * exeFileData - a buffer containing the executable to load
 * exeFileLength - number of bytes in exeFileData
 * exeFormat - parsed ELF segment information describing how to
 *   load the executable's text and data segments, and the
 *   code entry point address
 * command - string containing the complete command to be executed:
 *   this should be used to create the argument block for the
 *   process
 * pUserContext - reference to the pointer where the User_Context
 *   should be stored
 *
 * Returns:
 *   0 if successful, or an error code (< 0) if unsuccessful
 */
int Load_User_Program(char *exeFileData, ulong_t exeFileLength,
                      struct Exe_Format *exeFormat, const char *command,
                      struct User_Context **pUserContext)
{
    struct User_Context *uContext;
    uContext = Create_User_Context();
    //----先处理pUserContext中涉及分段机制的选择子，描述符等结构-----
    uContext->ldtDescriptor = Allocate_Segment_Descriptor();
    if (uContext->ldtDescriptor == NULL)
    {
        Print("allocate segment descriptor fail/n");
        return -1;
    }
    Init_LDT_Descriptor(uContext->ldtDescriptor, uContext->ldt,
                        NUM_USER_LDT_ENTRIES);
    uContext->ldtSelector = Selector(USER_PRIVILEGE, true,
                                     Get_Descriptor_Index(uContext->ldtDescriptor));
    // 注意，在GeekOS的分页机制下，用户地址空间默认从线性地址2G开始
    Init_Code_Segment_Descriptor(&uContext->ldt[0], USER_VM_START,
                                 USER_VM_LEN / PAGE_SIZE, USER_PRIVILEGE);
    Init_Data_Segment_Descriptor(&uContext->ldt[1], USER_VM_START,
                                 USER_VM_LEN / PAGE_SIZE, USER_PRIVILEGE);
    uContext->csSelector = Selector(USER_PRIVILEGE, false, 0);
    uContext->dsSelector = Selector(USER_PRIVILEGE, false, 1);
    //---------处理分页涉及的数据--------------------------------------
    pde_t *pageDirectory;
    pageDirectory = (pde_t *)Alloc_Page();
    if (pageDirectory == NULL)
    {
        return -1;
    }
    memset(pageDirectory, '\0', PAGE_SIZE);
    // 将内核页目录复制到用户态进程的页目录中
    memcpy(pageDirectory, g_kernel_pde, PAGE_SIZE);
    uContext->pageDir = pageDirectory;
    int i, res;
    uint_t startAddress = 0;
    uint_t sizeInMemory = 0;
    uint_t offsetInFile = 0;
    uint_t lengthInFile = 0;
    for (i = 0; i < exeFormat->numSegments; i++)
    {
        startAddress = exeFormat->segmentList[i].startAddress;
        sizeInMemory = exeFormat->segmentList[i].sizeInMemory;
        offsetInFile = exeFormat->segmentList[i].offsetInFile;
        lengthInFile = exeFormat->segmentList[i].lengthInFile;
        if (startAddress + sizeInMemory < USER_VM_LEN)
        {
            res = Alloc_User_Page(pageDirectory, startAddress + USER_VM_START,
                                  sizeInMemory);
            if (res != 0)
            {
                return -1;
            }
            if ((sizeInMemory == 0) && (lengthInFile == 0))
                continue;
            res = Copy_User_Page(pageDirectory, startAddress + USER_VM_START,
                                 exeFileData + offsetInFile, lengthInFile);
            if (res != true)
            {
                return -1;
            }
        }
        else
        {
            return -1;
        }
    }
    //----------处理参数块与堆栈块---------------------------------
    uint_t args_num, stack_addr, arg_addr;
    ulong_t arg_size;
    Get_Argument_Block_Size(command, &args_num, &arg_size);
    if (arg_size > PAGE_SIZE)
    {
        return -1;
    }
    // 分配参数块所需页
    arg_addr = Round_Down_To_Page(USER_VM_LEN - arg_size);
    char *block_buffer = Malloc(arg_size);
    KASSERT(block_buffer != NULL);
    Format_Argument_Block(block_buffer, args_num, arg_addr, command);
    res = Alloc_User_Page(pageDirectory, arg_addr + USER_VM_START, arg_size);
    if (res != 0)
    {
        return -1;
    }
    res = Copy_User_Page(pageDirectory, arg_addr + USER_VM_START,
                         block_buffer, arg_size);
    if (res != true)
    {
        return -1;
    }
    Free(block_buffer);
    // 分配堆栈所需页
    stack_addr = USER_VM_LEN - Round_Up_To_Page(arg_size) -
                 DEFAULT_STACK_SIZE;
    res = Alloc_User_Page(pageDirectory, stack_addr + USER_VM_START,
                          DEFAULT_STACK_SIZE);
    if (res != 0)
    {
        return -1;
    }
    // 最后处理UserContext的信息
    uContext->entryAddr = exeFormat->entryAddr;
    uContext->argBlockAddr = arg_addr;
    uContext->size = USER_VM_LEN;
    uContext->stackPointerAddr = arg_addr;
    *pUserContext = uContext;
    return 0;
}

/*
 * Copy data from user buffer into kernel buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_From_User(void *destInKernel, ulong_t srcInUser, ulong_t numBytes)
{
    void *kaddr = destInKernel;
    ulong_t userVA = srcInUser + USER_VM_START;
    ulong_t numCopied = 0;
    struct User_Context *userContext = g_currentThread->userContext;
    while (numCopied < numBytes)
    {
        struct Page *cur_page = Get_Page(lin_to_phyaddr(userContext->pageDir,
                                                        userVA));
        bool iflag = Begin_Int_Atomic();
        cur_page->flags &= ~(PAGE_PAGEABLE);
        End_Int_Atomic(iflag);
        ulong_t toCopy = PAGE_SIZE;
        if (!Is_Page_Multiple(userVA))
            toCopy -= (userVA & (PAGE_SIZE - 1));
        if (toCopy > numBytes)
            toCopy = numBytes;
        memcpy(kaddr, (void *)userVA, toCopy);
        userVA = Round_Down_To_Page(userVA + PAGE_SIZE);
        kaddr = (void *)((char *)kaddr + toCopy);
        numCopied += toCopy;
        iflag = Begin_Int_Atomic();
        cur_page->flags |= PAGE_PAGEABLE;
        End_Int_Atomic(iflag);
    }
    return true;
}

/*
 * Copy data from kernel buffer into user buffer.
 * Returns true if successful, false otherwise.
 */
bool Copy_To_User(ulong_t destInUser, void *srcInKernel, ulong_t numBytes)
{
    ulong_t userVA = destInUser + USER_VM_START;
    ulong_t numCopied = 0;
    struct User_Context *userContext = g_currentThread->userContext;
    while (numCopied < numBytes)
    {
        struct Page *cur_page = Get_Page(lin_to_phyaddr(userContext->pageDir, userVA));
        cur_page->flags &= ~(PAGE_PAGEABLE);
        ulong_t pageVA = Round_Down_To_Page(userVA);
        ulong_t toCopy = PAGE_SIZE;
        if (!Is_Page_Multiple(userVA))
            toCopy -= (userVA & (PAGE_SIZE - 1));
        if (toCopy > numBytes)
            toCopy = numBytes;
        memcpy((void *)pageVA, srcInKernel, toCopy);
        userVA = Round_Down_To_Page(userVA + PAGE_SIZE);
        srcInKernel = (void *)((char *)srcInKernel + toCopy);
        numCopied += toCopy;
        cur_page->flags |= PAGE_PAGEABLE;
    }
    return true;
}

/*
 * Switch to user address space.
 */
void Switch_To_Address_Space(struct User_Context *userContext)
{
    ushort_t ldtSelector;
    Set_PDBR(userContext->pageDir);
    ldtSelector = userContext->ldtSelector;
    __asm__ __volatile__(
        "lldt %0"
        :
        : "a"(ldtSelector));
}

