/**
 * @file nullfs.cpp
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

#undef _DEBUG
#include "nullfs.h"
#include <sddl.h>
#include <VersionHelpers.h>
#include <cassert>
#include <map>
#include <unordered_map>

/* SLOWIO */
#include <thread>

#define NULLFS_MAX_PATH                  512
FSP_FSCTL_STATIC_ASSERT(NULLFS_MAX_PATH > MAX_PATH,
    "NULLFS_MAX_PATH must be greater than MAX_PATH.");

/*
 * Define the NULLFS_STANDALONE macro when building NULLFS as a standalone file system.
 * This macro should be defined in the Visual Studio project settings, Makefile, etc.
 */
//#define NULLFS_STANDALONE

/*
 * Define the NULLFS_DISPATCHER_STOPPED macro to include DispatcherStopped support.
 */
#define NULLFS_DISPATCHER_STOPPED

/*
 * Define the NULLFS_NAME_NORMALIZATION macro to include name normalization support.
 */
#define NULLFS_NAME_NORMALIZATION

/*
 * Define the NULLFS_REPARSE_POINTS macro to include reparse points support.
 */
#define NULLFS_REPARSE_POINTS

/*
 * Define the NULLFS_NAMED_STREAMS macro to include named streams support.
 */
#define NULLFS_NAMED_STREAMS

/*
 * Define the NULLFS_DIRINFO_BY_NAME macro to include GetDirInfoByName.
 */
#define NULLFS_DIRINFO_BY_NAME

/*
 * Define the NULLFS_SLOWIO macro to include delayed I/O response support.
 */
#define NULLFS_SLOWIO

/*
 * Define the NULLFS_CONTROL macro to include DeviceControl support.
 */
#define NULLFS_CONTROL

/*
 * Define the NULLFS_EA macro to include extended attributes support.
 */
#define NULLFS_EA

/*
 * Define the NULLFS_WSL macro to include WSLinux support.
 */
#define NULLFS_WSL

/*
 * Define the NULLFS_REJECT_EARLY_IRP macro to reject IRP's sent
 * to the file system prior to the dispatcher being started.
 */
#if defined(NULLFS_STANDALONE)
#define NULLFS_REJECT_EARLY_IRP
#endif

/*
 * Define the DEBUG_BUFFER_CHECK macro on Windows 8 or above. This includes
 * a check for the Write buffer to ensure that it is read-only.
 *
 * Since ProcessBuffer support in the FSD, this is no longer a guarantee.
 */
#if !defined(NDEBUG)
//#define DEBUG_BUFFER_CHECK
#endif

#define NULLFS_SECTOR_SIZE               512
#define NULLFS_SECTORS_PER_ALLOCATION_UNIT 1

/* NULLFS: fixed, fake volume size reported to the OS (1 PiB) */
#define NULLFS_VOLUME_SIZE              (1ULL << 50)

/*
 * Large Heap Support
 */

typedef struct
{
    DWORD Options;
    SIZE_T InitialSize;
    SIZE_T MaximumSize;
    SIZE_T Alignment;
} LARGE_HEAP_INITIALIZE_PARAMS;
static INIT_ONCE LargeHeapInitOnce = INIT_ONCE_STATIC_INIT;
static HANDLE LargeHeap;
static SIZE_T LargeHeapAlignment;
static BOOL WINAPI LargeHeapInitOnceF(
    PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context)
{
    LARGE_HEAP_INITIALIZE_PARAMS *Params = (LARGE_HEAP_INITIALIZE_PARAMS *)Parameter;
    LargeHeap = HeapCreate(Params->Options, Params->InitialSize, Params->MaximumSize);
    LargeHeapAlignment = 0 != Params->Alignment ?
        FSP_FSCTL_ALIGN_UP(Params->Alignment, 4096) :
        16 * 4096;
    return TRUE;
}
static inline
BOOLEAN LargeHeapInitialize(
    DWORD Options,
    SIZE_T InitialSize,
    SIZE_T MaximumSize,
    SIZE_T Alignment)
{
    LARGE_HEAP_INITIALIZE_PARAMS Params;
    Params.Options = Options;
    Params.InitialSize = InitialSize;
    Params.MaximumSize = MaximumSize;
    Params.Alignment = Alignment;
    InitOnceExecuteOnce(&LargeHeapInitOnce, LargeHeapInitOnceF, &Params, 0);
    return 0 != LargeHeap;
}
static inline
PVOID LargeHeapAlloc(SIZE_T Size)
{
    return HeapAlloc(LargeHeap, 0, FSP_FSCTL_ALIGN_UP(Size, LargeHeapAlignment));
}
static inline
PVOID LargeHeapRealloc(PVOID Pointer, SIZE_T Size)
{
    if (0 != Pointer)
    {
        if (0 != Size)
            return HeapReAlloc(LargeHeap, 0, Pointer, FSP_FSCTL_ALIGN_UP(Size, LargeHeapAlignment));
        else
            return HeapFree(LargeHeap, 0, Pointer), 0;
    }
    else
    {
        if (0 != Size)
            return HeapAlloc(LargeHeap, 0, FSP_FSCTL_ALIGN_UP(Size, LargeHeapAlignment));
        else
            return 0;
    }
}
static inline
VOID LargeHeapFree(PVOID Pointer)
{
    if (0 != Pointer)
        HeapFree(LargeHeap, 0, Pointer);
}

/*
 * NULLFS
 */

static inline
UINT64 NullfsGetSystemTime(VOID)
{
    FILETIME FileTime;
    GetSystemTimeAsFileTime(&FileTime);
    return ((PLARGE_INTEGER)&FileTime)->QuadPart;
}

static inline
unsigned NullfsUpperChar(unsigned c)
{
    /*
     * Bit-twiddling upper case char:
     *
     * - Let signbit(x) = x & 0x100 (treat bit 0x100 as "signbit").
     * - 'A' <= c && c <= 'Z' <=> s = signbit(c - 'A') ^ signbit(c - ('Z' + 1)) == 1
     *     - c >= 'A' <=> c - 'A' >= 0      <=> signbit(c - 'A') = 0
     *     - c <= 'Z' <=> c - ('Z' + 1) < 0 <=> signbit(c - ('Z' + 1)) = 1
     * - Bit 0x20 = 0x100 >> 3 toggles uppercase to lowercase and vice-versa.
     *
     * This is actually faster than `(c - 'a' <= 'z' - 'a') ? (c & ~0x20) : c`, even
     * when compiled using cmov conditional moves at least on this system (i7-1065G7).
     *
     * See https://godbolt.org/z/ebv131Wrh
     */
    unsigned s = ((c - 'a') ^ (c - ('z' + 1))) & 0x100;
    return c & ~(s >> 3);
}

static inline
int NullfsWcsnicmp(const wchar_t *s0, const wchar_t *t0, int n)
{
    /* Use fast loop for ASCII and fall back to CompareStringW for general case. */
    const wchar_t *s = s0;
    const wchar_t *t = t0;
    int v = 0;
    for (const void *e = t + n; e > (const void *)t; ++s, ++t)
    {
        unsigned sc = *s, tc = *t;
        if (0xffffff80 & (sc | tc))
        {
            v = CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, s0, n, t0, n);
            if (0 != v)
                return v - 2;
            else
                return _wcsnicmp(s, t, n);
        }
        if (0 != (v = NullfsUpperChar(sc) - NullfsUpperChar(tc)) || !tc)
            break;
    }
    return v;/*(0 < v) - (0 > v);*/
}

static inline
int NullfsFileNameCompare(PWSTR a, int alen, PWSTR b, int blen, BOOLEAN CaseInsensitive)
{
    PWSTR p, endp, partp, q, endq, partq;
    WCHAR c, d;
    int plen, qlen, len, res;

    if (-1 == alen)
        alen = lstrlenW(a);
    if (-1 == blen)
        blen = lstrlenW(b);

    for (p = a, endp = p + alen, q = b, endq = q + blen; endp > p && endq > q;)
    {
        c = d = 0;
        for (; endp > p && (L':' == *p || L'\\' == *p); p++)
            c = *p;
        for (; endq > q && (L':' == *q || L'\\' == *q); q++)
            d = *q;

        if (L':' == c)
            c = 1;
        else if (L'\\' == c)
            c = 2;
        if (L':' == d)
            d = 1;
        else if (L'\\' == d)
            d = 2;

        res = c - d;
        if (0 != res)
            return res;

        for (partp = p; endp > p && L':' != *p && L'\\' != *p; p++)
            ;
        for (partq = q; endq > q && L':' != *q && L'\\' != *q; q++)
            ;

        plen = (int)(p - partp);
        qlen = (int)(q - partq);

        len = plen < qlen ? plen : qlen;

        if (CaseInsensitive)
            res = NullfsWcsnicmp(partp, partq, len);
        else
            res = wcsncmp(partp, partq, len);

        if (0 == res)
            res = plen - qlen;

        if (0 != res)
            return res;
    }

    return -(endp <= p) + (endq <= q);
}

static inline
BOOLEAN NullfsFileNameHasPrefix(PWSTR a, PWSTR b, BOOLEAN CaseInsensitive)
{
    int alen = (int)wcslen(a);
    int blen = (int)wcslen(b);

    return alen >= blen && 0 == NullfsFileNameCompare(a, blen, b, blen, CaseInsensitive) &&
        (alen == blen || (1 == blen && L'\\' == b[0]) ||
#if defined(NULLFS_NAMED_STREAMS)
            (L'\\' == a[blen] || L':' == a[blen]));
#else
            (L'\\' == a[blen]));
#endif
}

#if defined(NULLFS_EA)
static inline
int NullfsEaNameCompare(PSTR a, PSTR b)
{
    /* EA names are always case-insensitive in NULLFS (to be inline with NTFS) */

    int res;

    res = CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, a, -1, b, -1);
    if (0 != res)
        res -= 2;
    else
        res = _stricmp(a, b);

    return res;
}

struct NULLFS_FILE_NODE_EA_LESS
{
    NULLFS_FILE_NODE_EA_LESS()
    {
    }
    bool operator()(PSTR a, PSTR b) const
    {
        return 0 > NullfsEaNameCompare(a, b);
    }
};
typedef std::map<PSTR, FILE_FULL_EA_INFORMATION *, NULLFS_FILE_NODE_EA_LESS> NULLFS_FILE_NODE_EA_MAP;
#endif

typedef struct _NULLFS_FILE_NODE
{
    WCHAR FileName[NULLFS_MAX_PATH];
    FSP_FSCTL_FILE_INFO FileInfo;
    SIZE_T FileSecuritySize;
    PVOID FileSecurity;
#if defined(NULLFS_REPARSE_POINTS)
    SIZE_T ReparseDataSize;
    PVOID ReparseData;
#endif
#if defined(NULLFS_EA)
    NULLFS_FILE_NODE_EA_MAP *EaMap;
#endif
    volatile LONG RefCount;
#if defined(NULLFS_NAMED_STREAMS)
    struct _NULLFS_FILE_NODE *MainFileNode;
#endif
} NULLFS_FILE_NODE;

struct NULLFS_FILE_NODE_LESS
{
    NULLFS_FILE_NODE_LESS(BOOLEAN CaseInsensitive) : CaseInsensitive(CaseInsensitive)
    {
    }
    bool operator()(PWSTR a, PWSTR b) const
    {
        return 0 > NullfsFileNameCompare(a, -1, b, -1, CaseInsensitive);
    }
    BOOLEAN CaseInsensitive;
};
typedef std::map<PWSTR, NULLFS_FILE_NODE *, NULLFS_FILE_NODE_LESS> NULLFS_FILE_NODE_MAP;

typedef struct _NULLFS
{
    FSP_FILE_SYSTEM *FileSystem;
    NULLFS_FILE_NODE_MAP *FileNodeMap;
    ULONG MaxFileNodes;
    ULONG MaxFileSize;
#ifdef NULLFS_SLOWIO
    ULONG SlowioMaxDelay;
    ULONG SlowioPercentDelay;
    ULONG SlowioRarefyDelay;
    volatile LONG SlowioThreadsRunning;
#endif
    UINT16 VolumeLabelLength;
    WCHAR VolumeLabel[32];
} NULLFS;

static inline
NTSTATUS NullfsFileNodeCreate(PWSTR FileName, NULLFS_FILE_NODE **PFileNode)
{
    static UINT64 IndexNumber = 1;
    NULLFS_FILE_NODE *FileNode;

    *PFileNode = 0;

    FileNode = (NULLFS_FILE_NODE *)malloc(sizeof *FileNode);
    if (0 == FileNode)
        return STATUS_INSUFFICIENT_RESOURCES;

    memset(FileNode, 0, sizeof *FileNode);
    wcscpy_s(FileNode->FileName, sizeof FileNode->FileName / sizeof(WCHAR), FileName);
    FileNode->FileInfo.CreationTime =
    FileNode->FileInfo.LastAccessTime =
    FileNode->FileInfo.LastWriteTime =
    FileNode->FileInfo.ChangeTime = NullfsGetSystemTime();
    FileNode->FileInfo.IndexNumber = IndexNumber++;

    *PFileNode = FileNode;

    return STATUS_SUCCESS;
}

#if defined(NULLFS_EA)
static inline
VOID NullfsFileNodeDeleteEaMap(NULLFS_FILE_NODE *FileNode)
{
    if (0 != FileNode->EaMap)
    {
        for (NULLFS_FILE_NODE_EA_MAP::iterator p = FileNode->EaMap->begin(), q = FileNode->EaMap->end();
            p != q; ++p)
            free(p->second);
        delete FileNode->EaMap;
        FileNode->EaMap = 0;
        FileNode->FileInfo.EaSize = 0;
    }
}
#endif

static inline
VOID NullfsFileNodeDelete(NULLFS_FILE_NODE *FileNode)
{
#if defined(NULLFS_EA)
    NullfsFileNodeDeleteEaMap(FileNode);
#endif
#if defined(NULLFS_REPARSE_POINTS)
    free(FileNode->ReparseData);
#endif
    free(FileNode->FileSecurity);
    free(FileNode);
}

static inline
VOID NullfsFileNodeReference(NULLFS_FILE_NODE *FileNode)
{
    InterlockedIncrement(&FileNode->RefCount);
}

static inline
VOID NullfsFileNodeDereference(NULLFS_FILE_NODE *FileNode)
{
    if (0 == InterlockedDecrement(&FileNode->RefCount))
        NullfsFileNodeDelete(FileNode);
}

static inline
VOID NullfsFileNodeGetFileInfo(NULLFS_FILE_NODE *FileNode, FSP_FSCTL_FILE_INFO *FileInfo)
{
#if defined(NULLFS_NAMED_STREAMS)
    if (0 == FileNode->MainFileNode)
        *FileInfo = FileNode->FileInfo;
    else
    {
        *FileInfo = FileNode->MainFileNode->FileInfo;
        FileInfo->FileAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;
            /* named streams cannot be directories */
        FileInfo->AllocationSize = FileNode->FileInfo.AllocationSize;
        FileInfo->FileSize = FileNode->FileInfo.FileSize;
    }
#else
    *FileInfo = FileNode->FileInfo;
#endif
}

#if defined(NULLFS_EA)
static inline
NTSTATUS NullfsFileNodeGetEaMap(NULLFS_FILE_NODE *FileNode, NULLFS_FILE_NODE_EA_MAP **PEaMap)
{
#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    *PEaMap = FileNode->EaMap;
    if (0 != *PEaMap)
        return STATUS_SUCCESS;

    try
    {
        *PEaMap = FileNode->EaMap = new NULLFS_FILE_NODE_EA_MAP(NULLFS_FILE_NODE_EA_LESS());
        return STATUS_SUCCESS;
    }
    catch (...)
    {
        *PEaMap = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

static inline
NTSTATUS NullfsFileNodeSetEa(
    FSP_FILE_SYSTEM *FileSystem, PVOID Context,
    PFILE_FULL_EA_INFORMATION Ea)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)Context;
    NULLFS_FILE_NODE_EA_MAP *EaMap;
    FILE_FULL_EA_INFORMATION *FileNodeEa = 0;
    NULLFS_FILE_NODE_EA_MAP::iterator p;
    ULONG EaSizePlus = 0, EaSizeMinus = 0;
    NTSTATUS Result;

    Result = NullfsFileNodeGetEaMap(FileNode, &EaMap);
    if (!NT_SUCCESS(Result))
        return Result;

    if (0 != Ea->EaValueLength)
    {
        EaSizePlus = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) +
            Ea->EaNameLength + 1 + Ea->EaValueLength;
        FileNodeEa = (FILE_FULL_EA_INFORMATION *)malloc(EaSizePlus);
        if (0 == FileNodeEa)
            return STATUS_INSUFFICIENT_RESOURCES;
        memcpy(FileNodeEa, Ea, EaSizePlus);
        FileNodeEa->NextEntryOffset = 0;

        EaSizePlus = FspFileSystemGetEaPackedSize(Ea);
    }

    p = EaMap->find(Ea->EaName);
    if (p != EaMap->end())
    {
        EaSizeMinus = FspFileSystemGetEaPackedSize(Ea);

        free(p->second);
        EaMap->erase(p);
    }

    if (0 != Ea->EaValueLength)
    {
        try
        {
            EaMap->insert(NULLFS_FILE_NODE_EA_MAP::value_type(FileNodeEa->EaName, FileNodeEa));
        }
        catch (...)
        {
            free(FileNodeEa);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    FileNode->FileInfo.EaSize = FileNode->FileInfo.EaSize + EaSizePlus - EaSizeMinus;

    return STATUS_SUCCESS;
}

static inline
BOOLEAN NullfsFileNodeNeedEa(NULLFS_FILE_NODE *FileNode)
{
#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (0 != FileNode->EaMap)
    {
        for (NULLFS_FILE_NODE_EA_MAP::iterator p = FileNode->EaMap->begin(), q = FileNode->EaMap->end();
            p != q; ++p)
            if (0 != (p->second->Flags & FILE_NEED_EA))
                return TRUE;
    }

    return FALSE;
}

static inline
BOOLEAN NullfsFileNodeEnumerateEa(NULLFS_FILE_NODE *FileNode,
    BOOLEAN (*EnumFn)(PFILE_FULL_EA_INFORMATION Ea, PVOID), PVOID Context)
{
#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (0 != FileNode->EaMap)
    {
        for (NULLFS_FILE_NODE_EA_MAP::iterator p = FileNode->EaMap->begin(), q = FileNode->EaMap->end();
            p != q; ++p)
            if (!EnumFn(p->second, Context))
                return FALSE;
    }

    return TRUE;
}
#endif

static inline
VOID NullfsFileNodeMapDump(NULLFS_FILE_NODE_MAP *FileNodeMap)
{
    for (NULLFS_FILE_NODE_MAP::iterator p = FileNodeMap->begin(), q = FileNodeMap->end(); p != q; ++p)
        FspDebugLog("%c %04lx %6lu %S\n",
            FILE_ATTRIBUTE_DIRECTORY & p->second->FileInfo.FileAttributes ? 'd' : 'f',
            (ULONG)p->second->FileInfo.FileAttributes,
            (ULONG)p->second->FileInfo.FileSize,
            p->second->FileName);
}

static inline
BOOLEAN NullfsFileNodeMapIsCaseInsensitive(NULLFS_FILE_NODE_MAP *FileNodeMap)
{
    return FileNodeMap->key_comp().CaseInsensitive;
}

static inline
NTSTATUS NullfsFileNodeMapCreate(BOOLEAN CaseInsensitive, NULLFS_FILE_NODE_MAP **PFileNodeMap)
{
    *PFileNodeMap = 0;
    try
    {
        *PFileNodeMap = new NULLFS_FILE_NODE_MAP(NULLFS_FILE_NODE_LESS(CaseInsensitive));
        return STATUS_SUCCESS;
    }
    catch (...)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

static inline
VOID NullfsFileNodeMapDelete(NULLFS_FILE_NODE_MAP *FileNodeMap)
{
    for (NULLFS_FILE_NODE_MAP::iterator p = FileNodeMap->begin(), q = FileNodeMap->end(); p != q; ++p)
        NullfsFileNodeDelete(p->second);

    delete FileNodeMap;
}

static inline
SIZE_T NullfsFileNodeMapCount(NULLFS_FILE_NODE_MAP *FileNodeMap)
{
    return FileNodeMap->size();
}

static inline
NULLFS_FILE_NODE *NullfsFileNodeMapGet(NULLFS_FILE_NODE_MAP *FileNodeMap, PWSTR FileName)
{
    NULLFS_FILE_NODE_MAP::iterator iter = FileNodeMap->find(FileName);
    if (iter == FileNodeMap->end())
        return 0;
    return iter->second;
}

#if defined(NULLFS_NAMED_STREAMS)
static inline
NULLFS_FILE_NODE *NullfsFileNodeMapGetMain(NULLFS_FILE_NODE_MAP *FileNodeMap, PWSTR FileName0)
{
    WCHAR FileName[NULLFS_MAX_PATH];
    wcscpy_s(FileName, sizeof FileName / sizeof(WCHAR), FileName0);
    PWSTR StreamName = wcschr(FileName, L':');
    if (0 == StreamName)
        return 0;
    StreamName[0] = L'\0';
    NULLFS_FILE_NODE_MAP::iterator iter = FileNodeMap->find(FileName);
    if (iter == FileNodeMap->end())
        return 0;
    return iter->second;
}
#endif

static inline
NULLFS_FILE_NODE *NullfsFileNodeMapGetParent(NULLFS_FILE_NODE_MAP *FileNodeMap, PWSTR FileName0,
    PNTSTATUS PResult)
{
    WCHAR Root[2] = L"\\";
    PWSTR Remain, Suffix;
    WCHAR FileName[NULLFS_MAX_PATH];
    wcscpy_s(FileName, sizeof FileName / sizeof(WCHAR), FileName0);
    FspPathSuffix(FileName, &Remain, &Suffix, Root);
    NULLFS_FILE_NODE_MAP::iterator iter = FileNodeMap->find(Remain);
    FspPathCombine(FileName, Suffix);
    if (iter == FileNodeMap->end())
    {
        *PResult = STATUS_OBJECT_PATH_NOT_FOUND;
        return 0;
    }
    if (0 == (iter->second->FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        *PResult = STATUS_NOT_A_DIRECTORY;
        return 0;
    }
    return iter->second;
}

static inline
VOID NullfsFileNodeMapTouchParent(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode)
{
    NTSTATUS Result;
    NULLFS_FILE_NODE *Parent;
    if (L'\\' == FileNode->FileName[0] && L'\0' == FileNode->FileName[1])
        return;
    Parent = NullfsFileNodeMapGetParent(FileNodeMap, FileNode->FileName, &Result);
    if (0 == Parent)
        return;
    Parent->FileInfo.LastAccessTime =
    Parent->FileInfo.LastWriteTime =
    Parent->FileInfo.ChangeTime = NullfsGetSystemTime();
}

static inline
NTSTATUS NullfsFileNodeMapInsert(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode,
    PBOOLEAN PInserted)
{
    *PInserted = 0;
    try
    {
        *PInserted = FileNodeMap->insert(NULLFS_FILE_NODE_MAP::value_type(FileNode->FileName, FileNode)).second;
        if (*PInserted)
        {
            NullfsFileNodeReference(FileNode);
            NullfsFileNodeMapTouchParent(FileNodeMap, FileNode);
        }
        return STATUS_SUCCESS;
    }
    catch (...)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

static inline
VOID NullfsFileNodeMapRemove(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode)
{
    if (FileNodeMap->erase(FileNode->FileName))
    {
        NullfsFileNodeMapTouchParent(FileNodeMap, FileNode);
        NullfsFileNodeDereference(FileNode);
    }
}

static inline
BOOLEAN NullfsFileNodeMapHasChild(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode)
{
    BOOLEAN Result = FALSE;
    WCHAR Root[2] = L"\\";
    PWSTR Remain, Suffix;
    NULLFS_FILE_NODE_MAP::iterator iter = FileNodeMap->upper_bound(FileNode->FileName);
    for (; FileNodeMap->end() != iter; ++iter)
    {
#if defined(NULLFS_NAMED_STREAMS)
        if (0 != wcschr(iter->second->FileName, L':'))
            continue;
#endif
        FspPathSuffix(iter->second->FileName, &Remain, &Suffix, Root);
        Result = 0 == NullfsFileNameCompare(Remain, -1, FileNode->FileName, -1,
            NullfsFileNodeMapIsCaseInsensitive(FileNodeMap));
        FspPathCombine(iter->second->FileName, Suffix);
        break;
    }
    return Result;
}

static inline
BOOLEAN NullfsFileNodeMapEnumerateChildren(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode,
    PWSTR PrevFileName0, BOOLEAN (*EnumFn)(NULLFS_FILE_NODE *, PVOID), PVOID Context)
{
    WCHAR Root[2] = L"\\";
    PWSTR Remain, Suffix;
    NULLFS_FILE_NODE_MAP::iterator iter;
    BOOLEAN IsDirectoryChild;
    if (0 != PrevFileName0)
    {
        WCHAR PrevFileName[NULLFS_MAX_PATH + 256];
        size_t Length0 = wcslen(FileNode->FileName);
        size_t Length1 = 1 != Length0 || L'\\' != FileNode->FileName[0];
        size_t Length2 = wcslen(PrevFileName0);
        assert(NULLFS_MAX_PATH + 256 > Length0 + Length1 + Length2);
        memcpy(PrevFileName, FileNode->FileName, Length0 * sizeof(WCHAR));
        memcpy(PrevFileName + Length0, L"\\", Length1 * sizeof(WCHAR));
        memcpy(PrevFileName + Length0 + Length1, PrevFileName0, Length2 * sizeof(WCHAR));
        PrevFileName[Length0 + Length1 + Length2] = L'\0';
        iter = FileNodeMap->upper_bound(PrevFileName);
    }
    else
        iter = FileNodeMap->upper_bound(FileNode->FileName);
    for (; FileNodeMap->end() != iter; ++iter)
    {
        if (!NullfsFileNameHasPrefix(iter->second->FileName, FileNode->FileName,
            NullfsFileNodeMapIsCaseInsensitive(FileNodeMap)))
            break;
        FspPathSuffix(iter->second->FileName, &Remain, &Suffix, Root);
        IsDirectoryChild = 0 == NullfsFileNameCompare(Remain, -1, FileNode->FileName, -1,
            NullfsFileNodeMapIsCaseInsensitive(FileNodeMap));
#if defined(NULLFS_NAMED_STREAMS)
        IsDirectoryChild = IsDirectoryChild && 0 == wcschr(Suffix, L':');
#endif
        FspPathCombine(iter->second->FileName, Suffix);
        if (IsDirectoryChild)
        {
            if (!EnumFn(iter->second, Context))
                return FALSE;
        }
    }
    return TRUE;
}

#if defined(NULLFS_NAMED_STREAMS)
static inline
BOOLEAN NullfsFileNodeMapEnumerateNamedStreams(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode,
    BOOLEAN (*EnumFn)(NULLFS_FILE_NODE *, PVOID), PVOID Context)
{
    NULLFS_FILE_NODE_MAP::iterator iter = FileNodeMap->upper_bound(FileNode->FileName);
    for (; FileNodeMap->end() != iter; ++iter)
    {
        if (!NullfsFileNameHasPrefix(iter->second->FileName, FileNode->FileName,
            NullfsFileNodeMapIsCaseInsensitive(FileNodeMap)))
            break;
        if (L':' != iter->second->FileName[wcslen(FileNode->FileName)])
            break;
        if (!EnumFn(iter->second, Context))
            return FALSE;
    }
    return TRUE;
}
#endif

static inline
BOOLEAN NullfsFileNodeMapEnumerateDescendants(NULLFS_FILE_NODE_MAP *FileNodeMap, NULLFS_FILE_NODE *FileNode,
    BOOLEAN (*EnumFn)(NULLFS_FILE_NODE *, PVOID), PVOID Context)
{
    WCHAR Root[2] = L"\\";
    NULLFS_FILE_NODE_MAP::iterator iter = FileNodeMap->lower_bound(FileNode->FileName);
    for (; FileNodeMap->end() != iter; ++iter)
    {
        if (!NullfsFileNameHasPrefix(iter->second->FileName, FileNode->FileName,
            NullfsFileNodeMapIsCaseInsensitive(FileNodeMap)))
            break;
        if (!EnumFn(iter->second, Context))
            return FALSE;
    }
    return TRUE;
}

typedef struct _NULLFS_FILE_NODE_MAP_ENUM_CONTEXT
{
    BOOLEAN Reference;
    NULLFS_FILE_NODE **FileNodes;
    ULONG Capacity, Count;
} NULLFS_FILE_NODE_MAP_ENUM_CONTEXT;

static inline
BOOLEAN NullfsFileNodeMapEnumerateFn(NULLFS_FILE_NODE *FileNode, PVOID Context0)
{
    NULLFS_FILE_NODE_MAP_ENUM_CONTEXT *Context = (NULLFS_FILE_NODE_MAP_ENUM_CONTEXT *)Context0;

    if (Context->Capacity <= Context->Count)
    {
        ULONG Capacity = 0 != Context->Capacity ? Context->Capacity * 2 : 16;
        PVOID P = realloc(Context->FileNodes, Capacity * sizeof Context->FileNodes[0]);
        if (0 == P)
        {
            FspDebugLog(__FUNCTION__ ": cannot allocate memory; aborting\n");
            abort();
        }

        Context->FileNodes = (NULLFS_FILE_NODE **)P;
        Context->Capacity = Capacity;
    }

    Context->FileNodes[Context->Count++] = FileNode;
    if (Context->Reference)
        NullfsFileNodeReference(FileNode);

    return TRUE;
}

static inline
VOID NullfsFileNodeMapEnumerateFree(NULLFS_FILE_NODE_MAP_ENUM_CONTEXT *Context)
{
    if (Context->Reference)
    {
        for (ULONG Index = 0; Context->Count > Index; Index++)
        {
            NULLFS_FILE_NODE *FileNode = Context->FileNodes[Index];
            NullfsFileNodeDereference(FileNode);
        }
    }
    free(Context->FileNodes);
}

#ifdef NULLFS_SLOWIO
/*
 * SLOWIO
 *
 * This is included for two uses:
 *
 * 1) For testing winfsp, by allowing nullfs to act more like a non-ram file system,
 *    with some IO taking many milliseconds, and some IO completion delayed.
 *
 * 2) As sample code for how to use winfsp's STATUS_PENDING capabilities.
 * 
 */

static inline UINT64 Hash(UINT64 x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x =  x ^ (x >> 31);
    return x;
}

static inline ULONG PseudoRandom(ULONG to)
{
    /* John Oberschelp's PRNG */
    static UINT64 spin = 0;
    InterlockedIncrement(&spin);
    return Hash(spin) % to;
}

static inline BOOLEAN SlowioReturnPending(FSP_FILE_SYSTEM *FileSystem)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    if (0 == Nullfs->SlowioMaxDelay)
        return FALSE;
    return PseudoRandom(100) < Nullfs->SlowioPercentDelay;
}

static inline VOID SlowioSnooze(FSP_FILE_SYSTEM *FileSystem)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    if (0 == Nullfs->SlowioMaxDelay)
        return;
    ULONG millis = PseudoRandom(Nullfs->SlowioMaxDelay + 1) >> PseudoRandom(Nullfs->SlowioRarefyDelay + 1);
    Sleep(millis);
}

void SlowioReadThread(
    FSP_FILE_SYSTEM *FileSystem,
    NULLFS_FILE_NODE *FileNode,
    PVOID Buffer, 
    UINT64 Offset, 
    UINT64 EndOffset,
    UINT64 RequestHint)
{
    SlowioSnooze(FileSystem);

    /* NULLFS: reads behave like /dev/zero */
    memset(Buffer, 0, (size_t)(EndOffset - Offset));
    UINT32 BytesTransferred = (ULONG)(EndOffset - Offset);

    FSP_FSCTL_TRANSACT_RSP ResponseBuf;
    memset(&ResponseBuf, 0, sizeof ResponseBuf);
    ResponseBuf.Size = sizeof ResponseBuf;
    ResponseBuf.Kind = FspFsctlTransactReadKind;
    ResponseBuf.Hint = RequestHint;                         // IRP that is being completed
    ResponseBuf.IoStatus.Status = STATUS_SUCCESS;
    ResponseBuf.IoStatus.Information = BytesTransferred;    // bytes read
    FspFileSystemSendResponse(FileSystem, &ResponseBuf);

    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    InterlockedDecrement(&Nullfs->SlowioThreadsRunning);
}

void SlowioWriteThread(
    FSP_FILE_SYSTEM *FileSystem,
    NULLFS_FILE_NODE *FileNode,
    PVOID Buffer, 
    UINT64 Offset, 
    UINT64 EndOffset,
    UINT64 RequestHint)
{
    SlowioSnooze(FileSystem);

    /* NULLFS: written data is discarded */
    UINT32 BytesTransferred = (ULONG)(EndOffset - Offset);

    FSP_FSCTL_TRANSACT_RSP ResponseBuf;
    memset(&ResponseBuf, 0, sizeof ResponseBuf);
    ResponseBuf.Size = sizeof ResponseBuf;
    ResponseBuf.Kind = FspFsctlTransactWriteKind;
    ResponseBuf.Hint = RequestHint;                         // IRP that is being completed
    ResponseBuf.IoStatus.Status = STATUS_SUCCESS;
    ResponseBuf.IoStatus.Information = BytesTransferred;    // bytes written
    NullfsFileNodeGetFileInfo(FileNode, &ResponseBuf.Rsp.Write.FileInfo);
    FspFileSystemSendResponse(FileSystem, &ResponseBuf);

    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    InterlockedDecrement(&Nullfs->SlowioThreadsRunning);
}

void SlowioReadDirectoryThread(
    FSP_FILE_SYSTEM *FileSystem,
    ULONG BytesTransferred,
    UINT64 RequestHint)
{
    SlowioSnooze(FileSystem);

    FSP_FSCTL_TRANSACT_RSP ResponseBuf;
    memset(&ResponseBuf, 0, sizeof ResponseBuf);
    ResponseBuf.Size = sizeof ResponseBuf;
    ResponseBuf.Kind = FspFsctlTransactQueryDirectoryKind;
    ResponseBuf.Hint = RequestHint;                         // IRP that is being completed
    ResponseBuf.IoStatus.Status = STATUS_SUCCESS;
    ResponseBuf.IoStatus.Information = BytesTransferred;    // bytes of directory info read
    FspFileSystemSendResponse(FileSystem, &ResponseBuf);

    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    InterlockedDecrement(&Nullfs->SlowioThreadsRunning);
}
#endif

/*
 * FSP_FILE_SYSTEM_INTERFACE
 */

#if defined(NULLFS_REPARSE_POINTS)
static NTSTATUS GetReparsePointByName(
    FSP_FILE_SYSTEM *FileSystem, PVOID Context,
    PWSTR FileName, BOOLEAN IsDirectory, PVOID Buffer, PSIZE_T PSize);
#endif

static NTSTATUS SetFileSizeInternal(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, UINT64 NewSize, BOOLEAN SetAllocationSize);

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *FileSystem,
    FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;

    /* NULLFS: nothing is ever stored, so report a fixed volume size */
    VolumeInfo->TotalSize = NULLFS_VOLUME_SIZE;
    VolumeInfo->FreeSize = NULLFS_VOLUME_SIZE;
    VolumeInfo->VolumeLabelLength = Nullfs->VolumeLabelLength;
    memcpy(VolumeInfo->VolumeLabel, Nullfs->VolumeLabel, Nullfs->VolumeLabelLength);

    return STATUS_SUCCESS;
}

static NTSTATUS SetVolumeLabel(FSP_FILE_SYSTEM *FileSystem,
    PWSTR VolumeLabel,
    FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;

    Nullfs->VolumeLabelLength = (UINT16)(wcslen(VolumeLabel) * sizeof(WCHAR));
    if (Nullfs->VolumeLabelLength > sizeof Nullfs->VolumeLabel)
        Nullfs->VolumeLabelLength = sizeof Nullfs->VolumeLabel;
    memcpy(Nullfs->VolumeLabel, VolumeLabel, Nullfs->VolumeLabelLength);

    /* NULLFS: nothing is ever stored, so report a fixed volume size */
    VolumeInfo->TotalSize = NULLFS_VOLUME_SIZE;
    VolumeInfo->FreeSize = NULLFS_VOLUME_SIZE;
    VolumeInfo->VolumeLabelLength = Nullfs->VolumeLabelLength;
    memcpy(VolumeInfo->VolumeLabel, Nullfs->VolumeLabel, Nullfs->VolumeLabelLength);

    return STATUS_SUCCESS;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode;
    NTSTATUS Result;

    FileNode = NullfsFileNodeMapGet(Nullfs->FileNodeMap, FileName);
    if (0 == FileNode)
    {
        Result = STATUS_OBJECT_NAME_NOT_FOUND;

#if defined(NULLFS_REPARSE_POINTS)
        if (FspFileSystemFindReparsePoint(FileSystem, GetReparsePointByName, 0,
            FileName, PFileAttributes))
            Result = STATUS_REPARSE;
        else
#endif
            NullfsFileNodeMapGetParent(Nullfs->FileNodeMap, FileName, &Result);

        return Result;
    }

#if defined(NULLFS_NAMED_STREAMS)
    UINT32 FileAttributesMask = ~(UINT32)0;
    if (0 != FileNode->MainFileNode)
    {
        FileAttributesMask = ~(UINT32)FILE_ATTRIBUTE_DIRECTORY;
        FileNode = FileNode->MainFileNode;
    }

    if (0 != PFileAttributes)
        *PFileAttributes = FileNode->FileInfo.FileAttributes & FileAttributesMask;
#else
    if (0 != PFileAttributes)
        *PFileAttributes = FileNode->FileInfo.FileAttributes;
#endif

    if (0 != PSecurityDescriptorSize)
    {
        if (FileNode->FileSecuritySize > *PSecurityDescriptorSize)
        {
            *PSecurityDescriptorSize = FileNode->FileSecuritySize;
            return STATUS_BUFFER_OVERFLOW;
        }

        *PSecurityDescriptorSize = FileNode->FileSecuritySize;
        if (0 != SecurityDescriptor)
            memcpy(SecurityDescriptor, FileNode->FileSecurity, FileNode->FileSecuritySize);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS Create(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor, UINT64 AllocationSize,
#if defined(NULLFS_EA) || defined(NULLFS_WSL)
    PVOID ExtraBuffer, ULONG ExtraLength, BOOLEAN ExtraBufferIsReparsePoint,
#endif
    PVOID *PFileNode, FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
#if defined(NULLFS_NAME_NORMALIZATION)
    WCHAR FileNameBuf[NULLFS_MAX_PATH];
#endif
    NULLFS_FILE_NODE *FileNode;
    NULLFS_FILE_NODE *ParentNode;
    NTSTATUS Result;
    BOOLEAN Inserted;

    if (NULLFS_MAX_PATH <= wcslen(FileName))
        return STATUS_OBJECT_NAME_INVALID;

    if (CreateOptions & FILE_DIRECTORY_FILE)
        AllocationSize = 0;

    FileNode = NullfsFileNodeMapGet(Nullfs->FileNodeMap, FileName);
    if (0 != FileNode)
        return STATUS_OBJECT_NAME_COLLISION;

    ParentNode = NullfsFileNodeMapGetParent(Nullfs->FileNodeMap, FileName, &Result);
    if (0 == ParentNode)
        return Result;

    if (NullfsFileNodeMapCount(Nullfs->FileNodeMap) >= Nullfs->MaxFileNodes)
        return STATUS_CANNOT_MAKE;

    if (0 != Nullfs->MaxFileSize && AllocationSize > Nullfs->MaxFileSize)
        return STATUS_DISK_FULL;

#if defined(NULLFS_NAME_NORMALIZATION)
    if (NullfsFileNodeMapIsCaseInsensitive(Nullfs->FileNodeMap))
    {
        WCHAR Root[2] = L"\\";
        PWSTR Remain, Suffix;
        size_t RemainLength, BSlashLength, SuffixLength;

        FspPathSuffix(FileName, &Remain, &Suffix, Root);
        assert(0 == NullfsFileNameCompare(Remain, -1, ParentNode->FileName, -1, TRUE));
        FspPathCombine(FileName, Suffix);

        RemainLength = wcslen(ParentNode->FileName);
        BSlashLength = 1 < RemainLength;
        SuffixLength = wcslen(Suffix);
        if (NULLFS_MAX_PATH <= RemainLength + BSlashLength + SuffixLength)
            return STATUS_OBJECT_NAME_INVALID;

        memcpy(FileNameBuf, ParentNode->FileName, RemainLength * sizeof(WCHAR));
        memcpy(FileNameBuf + RemainLength, L"\\", BSlashLength * sizeof(WCHAR));
        memcpy(FileNameBuf + RemainLength + BSlashLength, Suffix, (SuffixLength + 1) * sizeof(WCHAR));

        FileName = FileNameBuf;
    }
#endif

    Result = NullfsFileNodeCreate(FileName, &FileNode);
    if (!NT_SUCCESS(Result))
        return Result;

#if defined(NULLFS_NAMED_STREAMS)
    FileNode->MainFileNode = NullfsFileNodeMapGetMain(Nullfs->FileNodeMap, FileName);
#endif

    FileNode->FileInfo.FileAttributes = (FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
        FileAttributes : FileAttributes | FILE_ATTRIBUTE_ARCHIVE;

    if (0 != SecurityDescriptor)
    {
        FileNode->FileSecuritySize = GetSecurityDescriptorLength(SecurityDescriptor);
        FileNode->FileSecurity = (PSECURITY_DESCRIPTOR)malloc(FileNode->FileSecuritySize);
        if (0 == FileNode->FileSecurity)
        {
            NullfsFileNodeDelete(FileNode);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        memcpy(FileNode->FileSecurity, SecurityDescriptor, FileNode->FileSecuritySize);
    }

#if defined(NULLFS_EA) || defined(NULLFS_WSL)
    if (0 != ExtraBuffer)
    {
#if defined(NULLFS_EA)
        if (!ExtraBufferIsReparsePoint)
        {
            Result = FspFileSystemEnumerateEa(FileSystem, NullfsFileNodeSetEa, FileNode,
                (PFILE_FULL_EA_INFORMATION)ExtraBuffer, ExtraLength);
            if (!NT_SUCCESS(Result))
            {
                NullfsFileNodeDelete(FileNode);
                return Result;
            }
        }
#endif
#if defined(NULLFS_WSL)
        if (ExtraBufferIsReparsePoint)
        {
#if defined(NULLFS_REPARSE_POINTS)
            FileNode->ReparseDataSize = ExtraLength;
            FileNode->ReparseData = malloc(ExtraLength);
            if (0 == FileNode->ReparseData && 0 != ExtraLength)
            {
                NullfsFileNodeDelete(FileNode);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            FileNode->FileInfo.FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
            FileNode->FileInfo.ReparseTag = *(PULONG)ExtraBuffer;
                /* the first field in a reparse buffer is the reparse tag */
            memcpy(FileNode->ReparseData, ExtraBuffer, ExtraLength);
#else
            NullfsFileNodeDelete(FileNode);
            return STATUS_INVALID_PARAMETER;
#endif
        }
#endif
    }
#endif

    /*
     * NULLFS: record the allocation size, but never allocate any storage.
     * Everything written to this file is discarded.
     */
    FileNode->FileInfo.AllocationSize = AllocationSize;

    Result = NullfsFileNodeMapInsert(Nullfs->FileNodeMap, FileNode, &Inserted);
    if (!NT_SUCCESS(Result) || !Inserted)
    {
        NullfsFileNodeDelete(FileNode);
        if (NT_SUCCESS(Result))
            Result = STATUS_OBJECT_NAME_COLLISION; /* should not happen! */
        return Result;
    }

    NullfsFileNodeReference(FileNode);
    *PFileNode = FileNode;
    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

#if defined(NULLFS_NAME_NORMALIZATION)
    if (NullfsFileNodeMapIsCaseInsensitive(Nullfs->FileNodeMap))
    {
        FSP_FSCTL_OPEN_FILE_INFO *OpenFileInfo = FspFileSystemGetOpenFileInfo(FileInfo);

        wcscpy_s(OpenFileInfo->NormalizedName, OpenFileInfo->NormalizedNameSize / sizeof(WCHAR),
            FileNode->FileName);
        OpenFileInfo->NormalizedNameSize = (UINT16)(wcslen(FileNode->FileName) * sizeof(WCHAR));
    }
#endif

    return STATUS_SUCCESS;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID *PFileNode, FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode;
    NTSTATUS Result;

    if (NULLFS_MAX_PATH <= wcslen(FileName))
        return STATUS_OBJECT_NAME_INVALID;

    FileNode = NullfsFileNodeMapGet(Nullfs->FileNodeMap, FileName);
    if (0 == FileNode)
    {
        Result = STATUS_OBJECT_NAME_NOT_FOUND;
        NullfsFileNodeMapGetParent(Nullfs->FileNodeMap, FileName, &Result);
        return Result;
    }

#if defined(NULLFS_EA)
    /* if the OP specified no EA's check the need EA count, but only if accessing main stream */
    if (0 != (CreateOptions & FILE_NO_EA_KNOWLEDGE)
#if defined(NULLFS_NAMED_STREAMS)
        && (0 == FileNode->MainFileNode)
#endif
        )
    {
        if (NullfsFileNodeNeedEa(FileNode))
        {
            Result = STATUS_ACCESS_DENIED;
            return Result;
        }
    }
#endif

    NullfsFileNodeReference(FileNode);
    *PFileNode = FileNode;
    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

#if defined(NULLFS_NAME_NORMALIZATION)
    if (NullfsFileNodeMapIsCaseInsensitive(Nullfs->FileNodeMap))
    {
        FSP_FSCTL_OPEN_FILE_INFO *OpenFileInfo = FspFileSystemGetOpenFileInfo(FileInfo);

        wcscpy_s(OpenFileInfo->NormalizedName, OpenFileInfo->NormalizedNameSize / sizeof(WCHAR),
            FileNode->FileName);
        OpenFileInfo->NormalizedNameSize = (UINT16)(wcslen(FileNode->FileName) * sizeof(WCHAR));
    }
#endif

    return STATUS_SUCCESS;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes, UINT64 AllocationSize,
#if defined(NULLFS_EA)
    PFILE_FULL_EA_INFORMATION Ea, ULONG EaLength,
#endif
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NTSTATUS Result;

#if defined(NULLFS_NAMED_STREAMS)
    NULLFS_FILE_NODE_MAP_ENUM_CONTEXT Context = { TRUE };
    ULONG Index;

    NullfsFileNodeMapEnumerateNamedStreams(Nullfs->FileNodeMap, FileNode,
        NullfsFileNodeMapEnumerateFn, &Context);
    for (Index = 0; Context.Count > Index; Index++)
    {
        LONG RefCount = FspInterlockedLoad32((INT32 *)&Context.FileNodes[Index]->RefCount);
        if (2 >= RefCount)
            NullfsFileNodeMapRemove(Nullfs->FileNodeMap, Context.FileNodes[Index]);
    }
    NullfsFileNodeMapEnumerateFree(&Context);
#endif

#if defined(NULLFS_EA)
    NullfsFileNodeDeleteEaMap(FileNode);
    if (0 != Ea)
    {
        Result = FspFileSystemEnumerateEa(FileSystem, NullfsFileNodeSetEa, FileNode, Ea, EaLength);
        if (!NT_SUCCESS(Result))
            return Result;
    }
#endif

    Result = SetFileSizeInternal(FileSystem, FileNode, AllocationSize, TRUE);
    if (!NT_SUCCESS(Result))
        return Result;

    if (ReplaceFileAttributes)
        FileNode->FileInfo.FileAttributes = FileAttributes | FILE_ATTRIBUTE_ARCHIVE;
    else
        FileNode->FileInfo.FileAttributes |= FileAttributes | FILE_ATTRIBUTE_ARCHIVE;

    FileNode->FileInfo.FileSize = 0;
    FileNode->FileInfo.LastAccessTime =
    FileNode->FileInfo.LastWriteTime =
    FileNode->FileInfo.ChangeTime = NullfsGetSystemTime();

    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

    return STATUS_SUCCESS;
}

static VOID Cleanup(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, PWSTR FileName, ULONG Flags)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
#if defined(NULLFS_NAMED_STREAMS)
    NULLFS_FILE_NODE *MainFileNode = 0 != FileNode->MainFileNode ?
        FileNode->MainFileNode : FileNode;
#else
    NULLFS_FILE_NODE *MainFileNode = FileNode;
#endif

    assert(0 != Flags); /* FSP_FSCTL_VOLUME_PARAMS::PostCleanupWhenModifiedOnly ensures this */

    if (Flags & FspCleanupSetArchiveBit)
    {
        if (0 == (MainFileNode->FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            MainFileNode->FileInfo.FileAttributes |= FILE_ATTRIBUTE_ARCHIVE;
    }

    if (Flags & (FspCleanupSetLastAccessTime | FspCleanupSetLastWriteTime | FspCleanupSetChangeTime))
    {
        UINT64 SystemTime = NullfsGetSystemTime();

        if (Flags & FspCleanupSetLastAccessTime)
            MainFileNode->FileInfo.LastAccessTime = SystemTime;
        if (Flags & FspCleanupSetLastWriteTime)
            MainFileNode->FileInfo.LastWriteTime = SystemTime;
        if (Flags & FspCleanupSetChangeTime)
            MainFileNode->FileInfo.ChangeTime = SystemTime;
    }

    if (Flags & FspCleanupSetAllocationSize)
    {
        UINT64 AllocationUnit = NULLFS_SECTOR_SIZE * NULLFS_SECTORS_PER_ALLOCATION_UNIT;
        UINT64 AllocationSize = (FileNode->FileInfo.FileSize + AllocationUnit - 1) /
            AllocationUnit * AllocationUnit;

        SetFileSizeInternal(FileSystem, FileNode, AllocationSize, TRUE);
    }

    if ((Flags & FspCleanupDelete) && !NullfsFileNodeMapHasChild(Nullfs->FileNodeMap, FileNode))
    {
#if defined(NULLFS_NAMED_STREAMS)
        NULLFS_FILE_NODE_MAP_ENUM_CONTEXT Context = { FALSE };
        ULONG Index;

        NullfsFileNodeMapEnumerateNamedStreams(Nullfs->FileNodeMap, FileNode,
            NullfsFileNodeMapEnumerateFn, &Context);
        for (Index = 0; Context.Count > Index; Index++)
            NullfsFileNodeMapRemove(Nullfs->FileNodeMap, Context.FileNodes[Index]);
        NullfsFileNodeMapEnumerateFree(&Context);
#endif

        NullfsFileNodeMapRemove(Nullfs->FileNodeMap, FileNode);
    }
}

static VOID Close(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

    NullfsFileNodeDereference(FileNode);
}

static NTSTATUS Read(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    UINT64 EndOffset;

    if (Offset >= FileNode->FileInfo.FileSize)
        return STATUS_END_OF_FILE;

    EndOffset = Offset + Length;
    if (EndOffset > FileNode->FileInfo.FileSize)
        EndOffset = FileNode->FileInfo.FileSize;

#ifdef NULLFS_SLOWIO
    if (SlowioReturnPending(FileSystem))
    {
        NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
        try
        {
            InterlockedIncrement(&Nullfs->SlowioThreadsRunning);
            std::thread(SlowioReadThread,
                FileSystem, FileNode, Buffer, Offset, EndOffset,
                FspFileSystemGetOperationContext()->Request->Hint).
                detach();
            return STATUS_PENDING;
        }
        catch (...)
        {
            InterlockedDecrement(&Nullfs->SlowioThreadsRunning);
        }
    }
    SlowioSnooze(FileSystem);
#endif

    /* NULLFS: reads behave like /dev/zero */
    memset(Buffer, 0, (size_t)(EndOffset - Offset));

    *PBytesTransferred = (ULONG)(EndOffset - Offset);

    return STATUS_SUCCESS;
}

static NTSTATUS Write(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, PVOID Buffer, UINT64 Offset, ULONG Length,
    BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO *FileInfo)
{
#if defined(DEBUG_BUFFER_CHECK)
    SYSTEM_INFO SystemInfo;
    GetSystemInfo(&SystemInfo);
    for (PUINT8 P = (PUINT8)Buffer, EndP = P + Length; EndP > P; P += SystemInfo.dwPageSize)
        __try
        {
            *P = *P | 0;
            assert(!IsWindows8OrGreater());
                /* only on Windows 8 we can make the buffer read-only! */
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            /* ignore! */
        }
#endif

    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    UINT64 EndOffset;
    NTSTATUS Result;

    if (ConstrainedIo)
    {
        if (Offset >= FileNode->FileInfo.FileSize)
            return STATUS_SUCCESS;
        EndOffset = Offset + Length;
        if (EndOffset > FileNode->FileInfo.FileSize)
            EndOffset = FileNode->FileInfo.FileSize;
    }
    else
    {
        if (WriteToEndOfFile)
            Offset = FileNode->FileInfo.FileSize;
        EndOffset = Offset + Length;
        if (EndOffset > FileNode->FileInfo.FileSize)
        {
            Result = SetFileSizeInternal(FileSystem, FileNode, EndOffset, FALSE);
            if (!NT_SUCCESS(Result))
                return Result;
        }
    }

#ifdef NULLFS_SLOWIO
    if (SlowioReturnPending(FileSystem))
    {
        NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
        try
        {
            InterlockedIncrement(&Nullfs->SlowioThreadsRunning);
            std::thread(SlowioWriteThread,
                FileSystem, FileNode, Buffer, Offset, EndOffset,
                FspFileSystemGetOperationContext()->Request->Hint).
                detach();
            return STATUS_PENDING;
        }
        catch (...)
        {
            InterlockedDecrement(&Nullfs->SlowioThreadsRunning);
        }
    }
    SlowioSnooze(FileSystem);
#endif

    /* NULLFS: written data is discarded */

    *PBytesTransferred = (ULONG)(EndOffset - Offset);
    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

    return STATUS_SUCCESS;
}

NTSTATUS Flush(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

    /*  nothing to flush, since we do not cache anything */

    if (0 != FileNode)
    {
#if 0
#if defined(NULLFS_NAMED_STREAMS)
        if (0 != FileNode->MainFileNode)
            FileNode->MainFileNode->FileInfo.LastAccessTime =
            FileNode->MainFileNode->FileInfo.LastWriteTime =
            FileNode->MainFileNode->FileInfo.ChangeTime = NullfsGetSystemTime();
        else
#endif
        FileNode->FileInfo.LastAccessTime =
        FileNode->FileInfo.LastWriteTime =
        FileNode->FileInfo.ChangeTime = NullfsGetSystemTime();
#endif

        NullfsFileNodeGetFileInfo(FileNode, FileInfo);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

    return STATUS_SUCCESS;
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (INVALID_FILE_ATTRIBUTES != FileAttributes)
        FileNode->FileInfo.FileAttributes = FileAttributes;
    if (0 != CreationTime)
        FileNode->FileInfo.CreationTime = CreationTime;
    if (0 != LastAccessTime)
        FileNode->FileInfo.LastAccessTime = LastAccessTime;
    if (0 != LastWriteTime)
        FileNode->FileInfo.LastWriteTime = LastWriteTime;
    if (0 != ChangeTime)
        FileNode->FileInfo.ChangeTime = ChangeTime;

    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

    return STATUS_SUCCESS;
}

static NTSTATUS SetFileSizeInternal(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, UINT64 NewSize, BOOLEAN SetAllocationSize)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

    if (SetAllocationSize)
    {
        if (FileNode->FileInfo.AllocationSize != NewSize)
        {
            if (0 != Nullfs->MaxFileSize && NewSize > Nullfs->MaxFileSize)
                return STATUS_DISK_FULL;

            /*
             * NULLFS: the allocation size is a bookkeeping value only;
             * no storage is allocated, resized or freed.
             */
            FileNode->FileInfo.AllocationSize = NewSize;
            if (FileNode->FileInfo.FileSize > NewSize)
                FileNode->FileInfo.FileSize = NewSize;
        }
    }
    else
    {
        if (FileNode->FileInfo.FileSize != NewSize)
        {
            if (FileNode->FileInfo.AllocationSize < NewSize)
            {
                UINT64 AllocationUnit = NULLFS_SECTOR_SIZE * NULLFS_SECTORS_PER_ALLOCATION_UNIT;
                UINT64 AllocationSize = (NewSize + AllocationUnit - 1) / AllocationUnit * AllocationUnit;

                NTSTATUS Result = SetFileSizeInternal(FileSystem, FileNode, AllocationSize, TRUE);
                if (!NT_SUCCESS(Result))
                    return Result;
            }

            /* NULLFS: no storage to zero-fill */
            FileNode->FileInfo.FileSize = NewSize;
        }
    }

    return STATUS_SUCCESS;
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, UINT64 NewSize, BOOLEAN SetAllocationSize,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NTSTATUS Result;

    Result = SetFileSizeInternal(FileSystem, FileNode0, NewSize, SetAllocationSize);
    if (!NT_SUCCESS(Result))
        return Result;

    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

    return STATUS_SUCCESS;
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, PWSTR FileName)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

    if (NullfsFileNodeMapHasChild(Nullfs->FileNodeMap, FileNode))
        return STATUS_DIRECTORY_NOT_EMPTY;

    return STATUS_SUCCESS;
}

static NTSTATUS Rename(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NULLFS_FILE_NODE *NewFileNode, *DescendantFileNode;
    NULLFS_FILE_NODE_MAP_ENUM_CONTEXT Context = { TRUE };
    ULONG Index, FileNameLen, NewFileNameLen;
    BOOLEAN Inserted;
    NTSTATUS Result;

    NewFileNode = NullfsFileNodeMapGet(Nullfs->FileNodeMap, NewFileName);
    if (0 != NewFileNode && FileNode != NewFileNode)
    {
        if (!ReplaceIfExists)
        {
            Result = STATUS_OBJECT_NAME_COLLISION;
            goto exit;
        }

        if (NewFileNode->FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            Result = STATUS_ACCESS_DENIED;
            goto exit;
        }
    }

    NullfsFileNodeMapEnumerateDescendants(Nullfs->FileNodeMap, FileNode,
        NullfsFileNodeMapEnumerateFn, &Context);

    FileNameLen = (ULONG)wcslen(FileNode->FileName);
    NewFileNameLen = (ULONG)wcslen(NewFileName);
    for (Index = 0; Context.Count > Index; Index++)
    {
        DescendantFileNode = Context.FileNodes[Index];
        if (NULLFS_MAX_PATH <= wcslen(DescendantFileNode->FileName) - FileNameLen + NewFileNameLen)
        {
            Result = STATUS_OBJECT_NAME_INVALID;
            goto exit;
        }
    }

    if (0 != NewFileNode)
    {
        NullfsFileNodeReference(NewFileNode);
        NullfsFileNodeMapRemove(Nullfs->FileNodeMap, NewFileNode);
        NullfsFileNodeDereference(NewFileNode);
    }

    for (Index = 0; Context.Count > Index; Index++)
    {
        DescendantFileNode = Context.FileNodes[Index];
        NullfsFileNodeMapRemove(Nullfs->FileNodeMap, DescendantFileNode);
        memmove(DescendantFileNode->FileName + NewFileNameLen,
            DescendantFileNode->FileName + FileNameLen,
            (wcslen(DescendantFileNode->FileName) + 1 - FileNameLen) * sizeof(WCHAR));
        memcpy(DescendantFileNode->FileName, NewFileName, NewFileNameLen * sizeof(WCHAR));
        Result = NullfsFileNodeMapInsert(Nullfs->FileNodeMap, DescendantFileNode, &Inserted);
        if (!NT_SUCCESS(Result))
        {
            FspDebugLog(__FUNCTION__ ": cannot insert into FileNodeMap; aborting\n");
            abort();
        }
        assert(Inserted);
    }

    Result = STATUS_SUCCESS;

exit:
    NullfsFileNodeMapEnumerateFree(&Context);

    return Result;
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PSECURITY_DESCRIPTOR SecurityDescriptor, SIZE_T *PSecurityDescriptorSize)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (FileNode->FileSecuritySize > *PSecurityDescriptorSize)
    {
        *PSecurityDescriptorSize = FileNode->FileSecuritySize;
        return STATUS_BUFFER_OVERFLOW;
    }

    *PSecurityDescriptorSize = FileNode->FileSecuritySize;
    if (0 != SecurityDescriptor)
        memcpy(SecurityDescriptor, FileNode->FileSecurity, FileNode->FileSecuritySize);

    return STATUS_SUCCESS;
}

static NTSTATUS SetSecurity(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    PSECURITY_DESCRIPTOR NewSecurityDescriptor, FileSecurity;
    SIZE_T FileSecuritySize;
    NTSTATUS Result;

#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    Result = FspSetSecurityDescriptor(
        FileNode->FileSecurity,
        SecurityInformation,
        ModificationDescriptor,
        &NewSecurityDescriptor);
    if (!NT_SUCCESS(Result))
        return Result;

    FileSecuritySize = GetSecurityDescriptorLength(NewSecurityDescriptor);
    FileSecurity = (PSECURITY_DESCRIPTOR)malloc(FileSecuritySize);
    if (0 == FileSecurity)
    {
        FspDeleteSecurityDescriptor(NewSecurityDescriptor, (NTSTATUS (*)())FspSetSecurityDescriptor);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memcpy(FileSecurity, NewSecurityDescriptor, FileSecuritySize);
    FspDeleteSecurityDescriptor(NewSecurityDescriptor, (NTSTATUS (*)())FspSetSecurityDescriptor);

    free(FileNode->FileSecurity);
    FileNode->FileSecuritySize = FileSecuritySize;
    FileNode->FileSecurity = FileSecurity;

    return STATUS_SUCCESS;
}

typedef struct _NULLFS_READ_DIRECTORY_CONTEXT
{
    PVOID Buffer;
    ULONG Length;
    PULONG PBytesTransferred;
} NULLFS_READ_DIRECTORY_CONTEXT;

static BOOLEAN AddDirInfo(NULLFS_FILE_NODE *FileNode, PWSTR FileName,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
    UINT8 DirInfoBuf[sizeof(FSP_FSCTL_DIR_INFO) + sizeof FileNode->FileName];
    FSP_FSCTL_DIR_INFO *DirInfo = (FSP_FSCTL_DIR_INFO *)DirInfoBuf;
    WCHAR Root[2] = L"\\";
    PWSTR Remain, Suffix;

    if (0 == FileName)
    {
        FspPathSuffix(FileNode->FileName, &Remain, &Suffix, Root);
        FileName = Suffix;
        FspPathCombine(FileNode->FileName, Suffix);
    }

    memset(DirInfo->Padding, 0, sizeof DirInfo->Padding);
    DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + wcslen(FileName) * sizeof(WCHAR));
    DirInfo->FileInfo = FileNode->FileInfo;
    memcpy(DirInfo->FileNameBuf, FileName, DirInfo->Size - sizeof(FSP_FSCTL_DIR_INFO));

    return FspFileSystemAddDirInfo(DirInfo, Buffer, Length, PBytesTransferred);
}

static BOOLEAN ReadDirectoryEnumFn(NULLFS_FILE_NODE *FileNode, PVOID Context0)
{
    NULLFS_READ_DIRECTORY_CONTEXT *Context = (NULLFS_READ_DIRECTORY_CONTEXT *)Context0;

    return AddDirInfo(FileNode, 0,
        Context->Buffer, Context->Length, Context->PBytesTransferred);
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, PWSTR Pattern, PWSTR Marker,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
    assert(0 == Pattern);

    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NULLFS_FILE_NODE *ParentNode;
    NULLFS_READ_DIRECTORY_CONTEXT Context;
    NTSTATUS Result;

    Context.Buffer = Buffer;
    Context.Length = Length;
    Context.PBytesTransferred = PBytesTransferred;

    if (L'\0' != FileNode->FileName[1])
    {
        /* if this is not the root directory add the dot entries */

        ParentNode = NullfsFileNodeMapGetParent(Nullfs->FileNodeMap, FileNode->FileName, &Result);
        if (0 == ParentNode)
            return Result;

        if (0 == Marker)
        {
            if (!AddDirInfo(FileNode, L".", Buffer, Length, PBytesTransferred))
                return STATUS_SUCCESS;
        }
        if (0 == Marker || (L'.' == Marker[0] && L'\0' == Marker[1]))
        {
            if (!AddDirInfo(ParentNode, L"..", Buffer, Length, PBytesTransferred))
                return STATUS_SUCCESS;
            Marker = 0;
        }
    }

    if (NullfsFileNodeMapEnumerateChildren(Nullfs->FileNodeMap, FileNode, Marker,
        ReadDirectoryEnumFn, &Context))
        FspFileSystemAddDirInfo(0, Buffer, Length, PBytesTransferred);

#ifdef NULLFS_SLOWIO
    if (SlowioReturnPending(FileSystem))
    {
        try
        {
            InterlockedIncrement(&Nullfs->SlowioThreadsRunning);
            std::thread(SlowioReadDirectoryThread,
                FileSystem, *PBytesTransferred,
                FspFileSystemGetOperationContext()->Request->Hint).
                detach();
            return STATUS_PENDING;
        }
        catch (...)
        {
            InterlockedDecrement(&Nullfs->SlowioThreadsRunning);
        }
    }
    SlowioSnooze(FileSystem);
#endif

    return STATUS_SUCCESS;
}

#if defined(NULLFS_DIRINFO_BY_NAME)
static NTSTATUS GetDirInfoByName(FSP_FILE_SYSTEM *FileSystem,
    PVOID ParentNode0, PWSTR FileName,
    FSP_FSCTL_DIR_INFO *DirInfo)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *ParentNode = (NULLFS_FILE_NODE *)ParentNode0;
    NULLFS_FILE_NODE *FileNode;
    WCHAR FileNameBuf[NULLFS_MAX_PATH];
    size_t ParentLength, BSlashLength, FileNameLength;
    WCHAR Root[2] = L"\\";
    PWSTR Remain, Suffix;

    ParentLength = wcslen(ParentNode->FileName);
    BSlashLength = 1 < ParentLength;
    FileNameLength = wcslen(FileName);
    if (NULLFS_MAX_PATH <= ParentLength + BSlashLength + FileNameLength)
        return STATUS_OBJECT_NAME_NOT_FOUND; //STATUS_OBJECT_NAME_INVALID?

    memcpy(FileNameBuf, ParentNode->FileName, ParentLength * sizeof(WCHAR));
    memcpy(FileNameBuf + ParentLength, L"\\", BSlashLength * sizeof(WCHAR));
    memcpy(FileNameBuf + ParentLength + BSlashLength, FileName, (FileNameLength + 1) * sizeof(WCHAR));

    FileName = FileNameBuf;

    FileNode = NullfsFileNodeMapGet(Nullfs->FileNodeMap, FileName);
    if (0 == FileNode)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    FspPathSuffix(FileNode->FileName, &Remain, &Suffix, Root);
    FileName = Suffix;
    FspPathCombine(FileNode->FileName, Suffix);

    //memset(DirInfo->Padding, 0, sizeof DirInfo->Padding);
    DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + wcslen(FileName) * sizeof(WCHAR));
    DirInfo->FileInfo = FileNode->FileInfo;
    memcpy(DirInfo->FileNameBuf, FileName, DirInfo->Size - sizeof(FSP_FSCTL_DIR_INFO));

    return STATUS_SUCCESS;
}
#endif

#if defined(NULLFS_REPARSE_POINTS)
static NTSTATUS ResolveReparsePoints(FSP_FILE_SYSTEM *FileSystem,
    PWSTR FileName, UINT32 ReparsePointIndex, BOOLEAN ResolveLastPathComponent,
    PIO_STATUS_BLOCK PIoStatus, PVOID Buffer, PSIZE_T PSize)
{
    return FspFileSystemResolveReparsePoints(FileSystem, GetReparsePointByName, 0,
        FileName, ReparsePointIndex, ResolveLastPathComponent,
        PIoStatus, Buffer, PSize);
}

static NTSTATUS GetReparsePointByName(
    FSP_FILE_SYSTEM *FileSystem, PVOID Context,
    PWSTR FileName, BOOLEAN IsDirectory, PVOID Buffer, PSIZE_T PSize)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode;

#if defined(NULLFS_NAMED_STREAMS)
    /* GetReparsePointByName will never receive a named stream */
    assert(0 == wcschr(FileName, L':'));
#endif

    FileNode = NullfsFileNodeMapGet(Nullfs->FileNodeMap, FileName);
    if (0 == FileNode)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (0 == (FileNode->FileInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return STATUS_NOT_A_REPARSE_POINT;

    if (0 != Buffer)
    {
        if (FileNode->ReparseDataSize > *PSize)
            return STATUS_BUFFER_TOO_SMALL;

        *PSize = FileNode->ReparseDataSize;
        memcpy(Buffer, FileNode->ReparseData, FileNode->ReparseDataSize);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS GetReparsePoint(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PWSTR FileName, PVOID Buffer, PSIZE_T PSize)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;

#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (0 == (FileNode->FileInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return STATUS_NOT_A_REPARSE_POINT;

    if (FileNode->ReparseDataSize > *PSize)
        return STATUS_BUFFER_TOO_SMALL;

    *PSize = FileNode->ReparseDataSize;
    memcpy(Buffer, FileNode->ReparseData, FileNode->ReparseDataSize);

    return STATUS_SUCCESS;
}

static NTSTATUS SetReparsePoint(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PWSTR FileName, PVOID Buffer, SIZE_T Size)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    PVOID ReparseData;
    NTSTATUS Result;

#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (NullfsFileNodeMapHasChild(Nullfs->FileNodeMap, FileNode))
        return STATUS_DIRECTORY_NOT_EMPTY;

    if (0 != FileNode->ReparseData)
    {
        Result = FspFileSystemCanReplaceReparsePoint(
            FileNode->ReparseData, FileNode->ReparseDataSize,
            Buffer, Size);
        if (!NT_SUCCESS(Result))
            return Result;
    }

    ReparseData = realloc(FileNode->ReparseData, Size);
    if (0 == ReparseData && 0 != Size)
        return STATUS_INSUFFICIENT_RESOURCES;

    FileNode->FileInfo.FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    FileNode->FileInfo.ReparseTag = *(PULONG)Buffer;
        /* the first field in a reparse buffer is the reparse tag */
    FileNode->ReparseDataSize = Size;
    FileNode->ReparseData = ReparseData;
    memcpy(FileNode->ReparseData, Buffer, Size);

    return STATUS_SUCCESS;
}

static NTSTATUS DeleteReparsePoint(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PWSTR FileName, PVOID Buffer, SIZE_T Size)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NTSTATUS Result;

#if defined(NULLFS_NAMED_STREAMS)
    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;
#endif

    if (0 != FileNode->ReparseData)
    {
        Result = FspFileSystemCanReplaceReparsePoint(
            FileNode->ReparseData, FileNode->ReparseDataSize,
            Buffer, Size);
        if (!NT_SUCCESS(Result))
            return Result;
    }
    else
        return STATUS_NOT_A_REPARSE_POINT;

    free(FileNode->ReparseData);

    FileNode->FileInfo.FileAttributes &= ~FILE_ATTRIBUTE_REPARSE_POINT;
    FileNode->FileInfo.ReparseTag = 0;
    FileNode->ReparseDataSize = 0;
    FileNode->ReparseData = 0;

    return STATUS_SUCCESS;
}
#endif

#if defined(NULLFS_NAMED_STREAMS)
typedef struct _NULLFS_GET_STREAM_INFO_CONTEXT
{
    PVOID Buffer;
    ULONG Length;
    PULONG PBytesTransferred;
} NULLFS_GET_STREAM_INFO_CONTEXT;

static BOOLEAN AddStreamInfo(NULLFS_FILE_NODE *FileNode,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
    UINT8 StreamInfoBuf[sizeof(FSP_FSCTL_STREAM_INFO) + sizeof FileNode->FileName];
    FSP_FSCTL_STREAM_INFO *StreamInfo = (FSP_FSCTL_STREAM_INFO *)StreamInfoBuf;
    PWSTR StreamName;

    StreamName = wcschr(FileNode->FileName, L':');
    if (0 != StreamName)
        StreamName++;
    else
        StreamName = L"";

    StreamInfo->Size = (UINT16)(sizeof(FSP_FSCTL_STREAM_INFO) + wcslen(StreamName) * sizeof(WCHAR));
    StreamInfo->StreamSize = FileNode->FileInfo.FileSize;
    StreamInfo->StreamAllocationSize = FileNode->FileInfo.AllocationSize;
    memcpy(StreamInfo->StreamNameBuf, StreamName, StreamInfo->Size - sizeof(FSP_FSCTL_STREAM_INFO));

    return FspFileSystemAddStreamInfo(StreamInfo, Buffer, Length, PBytesTransferred);
}

static BOOLEAN GetStreamInfoEnumFn(NULLFS_FILE_NODE *FileNode, PVOID Context0)
{
    NULLFS_GET_STREAM_INFO_CONTEXT *Context = (NULLFS_GET_STREAM_INFO_CONTEXT *)Context0;

    return AddStreamInfo(FileNode,
        Context->Buffer, Context->Length, Context->PBytesTransferred);
}

static NTSTATUS GetStreamInfo(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0, PVOID Buffer, ULONG Length,
    PULONG PBytesTransferred)
{
    NULLFS *Nullfs = (NULLFS *)FileSystem->UserContext;
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NULLFS_GET_STREAM_INFO_CONTEXT Context;

    if (0 != FileNode->MainFileNode)
        FileNode = FileNode->MainFileNode;

    Context.Buffer = Buffer;
    Context.Length = Length;
    Context.PBytesTransferred = PBytesTransferred;

    if (0 == (FileNode->FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !AddStreamInfo(FileNode, Buffer, Length, PBytesTransferred))
        return STATUS_SUCCESS;

    if (NullfsFileNodeMapEnumerateNamedStreams(Nullfs->FileNodeMap, FileNode, GetStreamInfoEnumFn, &Context))
        FspFileSystemAddStreamInfo(0, Buffer, Length, PBytesTransferred);

    /* ???: how to handle out of response buffer condition? */

    return STATUS_SUCCESS;
}
#endif

#if defined(NULLFS_CONTROL)
static NTSTATUS Control(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode, UINT32 ControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength, PULONG PBytesTransferred)
{
    /* NULLFS also supports encryption! See below :) */
    if (CTL_CODE(0x8000 + 'M', 'R', METHOD_BUFFERED, FILE_ANY_ACCESS) == ControlCode)
    {
        if (OutputBufferLength != InputBufferLength)
            return STATUS_INVALID_PARAMETER;

        for (PUINT8 P = (PUINT8)InputBuffer, Q = (PUINT8)OutputBuffer, EndP = P + InputBufferLength;
            EndP > P; P++, Q++)
        {
            if (('A' <= *P && *P <= 'M') || ('a' <= *P && *P <= 'm'))
                *Q = *P + 13;
            else
            if (('N' <= *P && *P <= 'Z') || ('n' <= *P && *P <= 'z'))
                *Q = *P - 13;
            else
                *Q = *P;
        }

        *PBytesTransferred = InputBufferLength;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}
#endif

#if defined(NULLFS_EA)
typedef struct _NULLFS_GET_EA_CONTEXT
{
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG EaLength;
    PULONG PBytesTransferred;
} NULLFS_GET_EA_CONTEXT;

static BOOLEAN GetEaEnumFn(PFILE_FULL_EA_INFORMATION Ea, PVOID Context0)
{
    NULLFS_GET_EA_CONTEXT *Context = (NULLFS_GET_EA_CONTEXT *)Context0;

    return FspFileSystemAddEa(Ea,
        Context->Ea, Context->EaLength, Context->PBytesTransferred);
}

static NTSTATUS GetEa(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PFILE_FULL_EA_INFORMATION Ea, ULONG EaLength, PULONG PBytesTransferred)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NULLFS_GET_EA_CONTEXT Context;

    Context.Ea = Ea;
    Context.EaLength = EaLength;
    Context.PBytesTransferred = PBytesTransferred;

    if (NullfsFileNodeEnumerateEa(FileNode, GetEaEnumFn, &Context))
        FspFileSystemAddEa(0, Ea, EaLength, PBytesTransferred);

    return STATUS_SUCCESS;
}

static NTSTATUS SetEa(FSP_FILE_SYSTEM *FileSystem,
    PVOID FileNode0,
    PFILE_FULL_EA_INFORMATION Ea, ULONG EaLength,
    FSP_FSCTL_FILE_INFO *FileInfo)
{
    NULLFS_FILE_NODE *FileNode = (NULLFS_FILE_NODE *)FileNode0;
    NTSTATUS Result;

    Result = FspFileSystemEnumerateEa(FileSystem, NullfsFileNodeSetEa, FileNode, Ea, EaLength);
    if (!NT_SUCCESS(Result))
        return Result;

    NullfsFileNodeGetFileInfo(FileNode, FileInfo);

    return STATUS_SUCCESS;
}
#endif

#if defined(NULLFS_DISPATCHER_STOPPED)
static VOID DispatcherStopped(FSP_FILE_SYSTEM *FileSystem,
    BOOLEAN Normally)
{
    FspFileSystemStopServiceIfNecessary(FileSystem, Normally);
}
#endif

static FSP_FILE_SYSTEM_INTERFACE NullfsInterface =
{
    GetVolumeInfo,
    SetVolumeLabel,
    GetSecurityByName,
#if defined(NULLFS_EA) || defined(NULLFS_WSL)
    0,
#else
    Create,
#endif
    Open,
#if defined(NULLFS_EA)
    0,
#else
    Overwrite,
#endif
    Cleanup,
    Close,
    Read,
    Write,
    Flush,
    GetFileInfo,
    SetBasicInfo,
    SetFileSize,
    CanDelete,
    Rename,
    GetSecurity,
    SetSecurity,
    ReadDirectory,
#if defined(NULLFS_REPARSE_POINTS)
    ResolveReparsePoints,
    GetReparsePoint,
    SetReparsePoint,
    DeleteReparsePoint,
#else
    0,
    0,
    0,
    0,
#endif
#if defined(NULLFS_NAMED_STREAMS)
    GetStreamInfo,
#else
    0,
#endif
#if defined(NULLFS_DIRINFO_BY_NAME)
    GetDirInfoByName,
#else
    0,
#endif
#if defined(NULLFS_CONTROL)
    Control,
#else
    0,
#endif
    0,
#if defined(NULLFS_EA) || defined(NULLFS_WSL)
    Create,
#endif
#if defined(NULLFS_EA)
    Overwrite,
    GetEa,
    SetEa,
#else
    0,
    0,
    0,
#endif
    0,
#if defined(NULLFS_DISPATCHER_STOPPED)
    DispatcherStopped,
#else
    0,
#endif
};

/*
 * Public API
 */

NTSTATUS NullfsCreateFunnel(
    ULONG Flags,
    ULONG FileInfoTimeout,
    ULONG MaxFileNodes,
    ULONG MaxFileSize,
    ULONG SlowioMaxDelay,
    ULONG SlowioPercentDelay,
    ULONG SlowioRarefyDelay,
    PWSTR FileSystemName,
    PWSTR VolumePrefix,
    PWSTR RootSddl,
    NULLFS **PNullfs)
{
    NTSTATUS Result;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    BOOLEAN CaseInsensitive = !!(Flags & NullfsCaseInsensitive);
    BOOLEAN FlushAndPurgeOnCleanup = !!(Flags & NullfsFlushAndPurgeOnCleanup);
    BOOLEAN SupportsPosixUnlinkRename = !(Flags & NullfsLegacyUnlinkRename);
    PWSTR DevicePath = NullfsNet == (Flags & NullfsDeviceMask) ?
        L"" FSP_FSCTL_NET_DEVICE_NAME : L"" FSP_FSCTL_DISK_DEVICE_NAME;
    UINT64 AllocationUnit;
    NULLFS *Nullfs;
    NULLFS_FILE_NODE *RootNode;
    PSECURITY_DESCRIPTOR RootSecurity;
    ULONG RootSecuritySize;
    BOOLEAN Inserted;

    *PNullfs = 0;

    Result = NullfsHeapConfigure(0, 0, 0);
    if (!NT_SUCCESS(Result))
        return Result;

    if (0 == RootSddl)
        RootSddl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(RootSddl, SDDL_REVISION_1,
        &RootSecurity, &RootSecuritySize))
        return FspNtStatusFromWin32(GetLastError());

    Nullfs = (NULLFS *)malloc(sizeof *Nullfs);
    if (0 == Nullfs)
    {
        LocalFree(RootSecurity);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    memset(Nullfs, 0, sizeof *Nullfs);
    Nullfs->MaxFileNodes = MaxFileNodes;
    AllocationUnit = NULLFS_SECTOR_SIZE * NULLFS_SECTORS_PER_ALLOCATION_UNIT;
    Nullfs->MaxFileSize = (ULONG)((MaxFileSize + AllocationUnit - 1) / AllocationUnit * AllocationUnit);

#ifdef NULLFS_SLOWIO
    Nullfs->SlowioMaxDelay = SlowioMaxDelay;
    Nullfs->SlowioPercentDelay = SlowioPercentDelay;
    Nullfs->SlowioRarefyDelay = SlowioRarefyDelay;
#endif

    Result = NullfsFileNodeMapCreate(CaseInsensitive, &Nullfs->FileNodeMap);
    if (!NT_SUCCESS(Result))
    {
        free(Nullfs);
        LocalFree(RootSecurity);
        return Result;
    }

    memset(&VolumeParams, 0, sizeof VolumeParams);
    VolumeParams.Version = sizeof FSP_FSCTL_VOLUME_PARAMS;
    VolumeParams.SectorSize = NULLFS_SECTOR_SIZE;
    VolumeParams.SectorsPerAllocationUnit = NULLFS_SECTORS_PER_ALLOCATION_UNIT;
    VolumeParams.VolumeCreationTime = NullfsGetSystemTime();
    VolumeParams.VolumeSerialNumber = (UINT32)(NullfsGetSystemTime() / (10000 * 1000));
    VolumeParams.FileInfoTimeout = FileInfoTimeout;
    VolumeParams.CaseSensitiveSearch = !CaseInsensitive;
    VolumeParams.CasePreservedNames = 1;
    VolumeParams.UnicodeOnDisk = 1;
    VolumeParams.PersistentAcls = 1;
    VolumeParams.ReparsePoints = 1;
    VolumeParams.ReparsePointsAccessCheck = 0;
#if defined(NULLFS_NAMED_STREAMS)
    VolumeParams.NamedStreams = 1;
#endif
    VolumeParams.PostCleanupWhenModifiedOnly = 1;
    VolumeParams.PostDispositionWhenNecessaryOnly = 1;
#if defined(NULLFS_DIRINFO_BY_NAME)
    VolumeParams.PassQueryDirectoryFileName = 1;
#endif
    VolumeParams.FlushAndPurgeOnCleanup = FlushAndPurgeOnCleanup;
#if defined(NULLFS_CONTROL)
    VolumeParams.DeviceControl = 1;
#endif
#if defined(NULLFS_EA)
    VolumeParams.ExtendedAttributes = 1;
#endif
#if defined(NULLFS_WSL)
    VolumeParams.WslFeatures = 1;
#endif
    VolumeParams.AllowOpenInKernelMode = 1;
#if defined(NULLFS_REJECT_EARLY_IRP)
    VolumeParams.RejectIrpPriorToTransact0 = 1;
#endif
    VolumeParams.SupportsPosixUnlinkRename = SupportsPosixUnlinkRename;
    if (0 != VolumePrefix)
        wcscpy_s(VolumeParams.Prefix, sizeof VolumeParams.Prefix / sizeof(WCHAR), VolumePrefix);
    wcscpy_s(VolumeParams.FileSystemName, sizeof VolumeParams.FileSystemName / sizeof(WCHAR),
        0 != FileSystemName ? FileSystemName : L"-NULLFS");

    Result = FspFileSystemCreate(DevicePath, &VolumeParams, &NullfsInterface, &Nullfs->FileSystem);
    if (!NT_SUCCESS(Result))
    {
        NullfsFileNodeMapDelete(Nullfs->FileNodeMap);
        free(Nullfs);
        LocalFree(RootSecurity);
        return Result;
    }

    Nullfs->FileSystem->UserContext = Nullfs;
    Nullfs->VolumeLabelLength = sizeof L"NULLFS" - sizeof(WCHAR);
    memcpy(Nullfs->VolumeLabel, L"NULLFS", Nullfs->VolumeLabelLength);

#if 0
    FspFileSystemSetOperationGuardStrategy(Nullfs->FileSystem,
        FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_COARSE);
#endif

    /*
     * Create root directory.
     */

    Result = NullfsFileNodeCreate(L"\\", &RootNode);
    if (!NT_SUCCESS(Result))
    {
        NullfsDelete(Nullfs);
        LocalFree(RootSecurity);
        return Result;
    }

    RootNode->FileInfo.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;

    RootNode->FileSecurity = malloc(RootSecuritySize);
    if (0 == RootNode->FileSecurity)
    {
        NullfsFileNodeDelete(RootNode);
        NullfsDelete(Nullfs);
        LocalFree(RootSecurity);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RootNode->FileSecuritySize = RootSecuritySize;
    memcpy(RootNode->FileSecurity, RootSecurity, RootSecuritySize);

    Result = NullfsFileNodeMapInsert(Nullfs->FileNodeMap, RootNode, &Inserted);
    if (!NT_SUCCESS(Result))
    {
        NullfsFileNodeDelete(RootNode);
        NullfsDelete(Nullfs);
        LocalFree(RootSecurity);
        return Result;
    }

    LocalFree(RootSecurity);

    *PNullfs = Nullfs;

    return STATUS_SUCCESS;
}

VOID NullfsDelete(NULLFS *Nullfs)
{
    FspFileSystemDelete(Nullfs->FileSystem);

    NullfsFileNodeMapDelete(Nullfs->FileNodeMap);

    free(Nullfs);
}

NTSTATUS NullfsStart(NULLFS *Nullfs)
{
#ifdef NULLFS_SLOWIO
    Nullfs->SlowioThreadsRunning = 0;
#endif

    return FspFileSystemStartDispatcher(Nullfs->FileSystem, 0);
}

VOID NullfsStop(NULLFS *Nullfs)
{
    FspFileSystemStopDispatcher(Nullfs->FileSystem);

#ifdef NULLFS_SLOWIO
    while (Nullfs->SlowioThreadsRunning)
        Sleep(1);
#endif
}

FSP_FILE_SYSTEM *NullfsFileSystem(NULLFS *Nullfs)
{
    return Nullfs->FileSystem;
}

NTSTATUS NullfsHeapConfigure(SIZE_T InitialSize, SIZE_T MaximumSize, SIZE_T Alignment)
{
    return LargeHeapInitialize(0, InitialSize, MaximumSize, LargeHeapAlignment) ?
        STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
