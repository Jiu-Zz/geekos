/*
 * ELF executable loading
 * Copyright (c) 2003, Jeffrey K. Hollingsworth <hollings@cs.umd.edu>
 * Copyright (c) 2003, David H. Hovemeyer <daveho@cs.umd.edu>
 * $Revision: 1.29 $
 * 
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "COPYING".
 */

#include <geekos/errno.h>
#include <geekos/kassert.h>
#include <geekos/ktypes.h>
#include <geekos/screen.h>  /* for debug Print() statements */
#include <geekos/pfat.h>
#include <geekos/malloc.h>
#include <geekos/string.h>
#include <geekos/user.h>
#include <geekos/elf.h>


/**
 * From the data of an ELF executable, determine how its segments
 * need to be loaded into memory.
 * @param exeFileData buffer containing the executable file
 * @param exeFileLength length of the executable file in bytes
 * @param exeFormat structure describing the executable's segments
 *   and entry address; to be filled in
 * @return 0 if successful, < 0 on error
 */
int Parse_ELF_Executable(char *exeFileData, ulong_t exeFileLength,
                         struct Exe_Format *exeFormat)
{
    elfHeader *elfHead = (elfHeader *)exeFileData;
    int i;

    if (elfHead->ident[0] != 0x7F || elfHead->ident[1] != 'E' ||
        elfHead->ident[2] != 'L' || elfHead->ident[3] != 'F')
    {
        return -1;
    }

    exeFormat->numSegments = 0;
    exeFormat->entryAddr = elfHead->entry;

    programHeader *phHead = (programHeader *)(exeFileData + elfHead->phoff);
    for (i = 0; i < elfHead->phnum; i++)
    {
        programHeader *ph = &phHead[i];

        struct Exe_Segment *seg = &exeFormat->segmentList[exeFormat->numSegments];
        seg->offsetInFile = ph->offset;
        seg->lengthInFile = ph->fileSize;
        seg->startAddress = ph->vaddr;
        seg->sizeInMemory = ph->memSize;

        exeFormat->numSegments++;
    }
    return 0;
}

