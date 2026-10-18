/**
 * @file nullfs.h
 *
 * @copyright 2015-2025 Bill Zissimopoulos
 *
 * NULLFS: this file is a modified copy of WinFsp v2.1 tst/memfs.
 * Modifications: file payload data is never stored -- writes are discarded and
 * reads return zeroes. See README.md for the full list of changes.
 */
/*
 * This file is part of WinFsp.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 3 as published by the Free Software
 * Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the GPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#ifndef NULLFS_H_INCLUDED
#define NULLFS_H_INCLUDED

#include <winfsp/winfsp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NULLFS NULLFS;

enum
{
    NullfsDisk                           = 0x00000000,
    NullfsNet                            = 0x00000001,
    NullfsDeviceMask                     = 0x0000000f,
    NullfsCaseInsensitive                = 0x80000000,
    NullfsFlushAndPurgeOnCleanup         = 0x40000000,
    NullfsLegacyUnlinkRename             = 0x20000000,
    NullfsNoSlowio                       = 0x10000000,
};

#define NullfsCreate(Flags, FileInfoTimeout, MaxFileNodes, MaxFileSize, FileMetadataTimeout, Stateless, VolumePrefix, RootSddl, PNullfs)\
    NullfsCreateFunnel(\
        Flags,\
        FileInfoTimeout,\
        MaxFileNodes,\
        MaxFileSize,\
        FileMetadataTimeout,\
        Stateless,\
        0/*SlowioMaxDelay*/,\
        0/*SlowioPercentDelay*/,\
        0/*SlowioRarefyDelay*/,\
        0/*FileSystemName*/,\
        VolumePrefix,\
        RootSddl,\
        PNullfs)
NTSTATUS NullfsCreateFunnel(
    ULONG Flags,
    ULONG FileInfoTimeout,
    ULONG MaxFileNodes,
    ULONG MaxFileSize,
    ULONG FileMetadataTimeout,
    BOOLEAN Stateless,
    ULONG SlowioMaxDelay,
    ULONG SlowioPercentDelay,
    ULONG SlowioRarefyDelay,
    PWSTR FileSystemName,
    PWSTR VolumePrefix,
    PWSTR RootSddl,
    NULLFS **PNullfs);
VOID NullfsDelete(NULLFS *Nullfs);
NTSTATUS NullfsStart(NULLFS *Nullfs);
VOID NullfsStop(NULLFS *Nullfs);
FSP_FILE_SYSTEM *NullfsFileSystem(NULLFS *Nullfs);

NTSTATUS NullfsHeapConfigure(SIZE_T InitialSize, SIZE_T MaximumSize, SIZE_T Alignment);

#ifdef __cplusplus
}
#endif

#endif
