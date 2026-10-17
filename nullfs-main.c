/**
 * @file nullfs-main.c
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

#include <winfsp/winfsp.h>
#include "nullfs.h"

#define PROGNAME                        "nullfs"

#define info(format, ...)               FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...)               FspServiceLog(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...)               FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

#define argtos(v)                       if (arge > ++argp) v = *argp; else goto usage
#define argtol(v)                       if (arge > ++argp) v = wcstol_deflt(*argp, v); else goto usage

static ULONG wcstol_deflt(wchar_t *w, ULONG deflt)
{
    wchar_t *endp;
    ULONG ul = wcstol(w, &endp, 0);
    return L'\0' != w[0] && L'\0' == *endp ? ul : deflt;
}

NTSTATUS SvcStart(FSP_SERVICE *Service, ULONG argc, PWSTR *argv)
{
    wchar_t **argp, **arge;
    ULONG DebugFlags = 0;
    PWSTR DebugLogFile = 0;
    ULONG Flags = NullfsDisk;
    ULONG OtherFlags = 0;
    ULONG FileInfoTimeout = INFINITE;
    ULONG MaxFileNodes = 65536;
    ULONG MaxFileSize = 0;          /* 0: no per-file size limit */
    ULONG SlowioMaxDelay = 0;       /* -M: maximum slow IO delay in millis */
    ULONG SlowioPercentDelay = 0;   /* -P: percent of slow IO to make pending */
    ULONG SlowioRarefyDelay = 0;    /* -R: adjust the rarity of pending slow IO */
    PWSTR FileSystemName = 0;
    PWSTR MountPoint = 0;
    PWSTR VolumePrefix = 0;
    PWSTR RootSddl = 0;
    HANDLE DebugLogHandle = INVALID_HANDLE_VALUE;
    NULLFS *Nullfs = 0;
    NTSTATUS Result;

    for (argp = argv + 1, arge = argv + argc; arge > argp; argp++)
    {
        if (L'-' != argp[0][0])
            break;
        switch (argp[0][1])
        {
        case L'?':
            goto usage;
        case L'd':
            argtol(DebugFlags);
            break;
        case L'D':
            argtos(DebugLogFile);
            break;
        case L'f':
            OtherFlags = NullfsFlushAndPurgeOnCleanup;
            break;
        case L'F':
            argtos(FileSystemName);
            break;
        case L'i':
            OtherFlags = NullfsCaseInsensitive;
            break;
        case L'm':
            argtos(MountPoint);
            break;
        case L'M':
            argtol(SlowioMaxDelay);
            break;
        case L'n':
            argtol(MaxFileNodes);
            break;
        case L'P':
            argtol(SlowioPercentDelay);
            break;
        case L'R':
            argtol(SlowioRarefyDelay);
            break;
        case L'S':
            argtos(RootSddl);
            break;
        case L's':
            argtol(MaxFileSize);
            break;
        case L't':
            argtol(FileInfoTimeout);
            break;
        case L'u':
            argtos(VolumePrefix);
            if (0 != VolumePrefix && L'\0' != VolumePrefix[0])
                Flags = NullfsNet;
            break;
        default:
            goto usage;
        }
    }

    if (arge > argp)
        goto usage;

    if (NullfsDisk == Flags && 0 == MountPoint)
        goto usage;

    if (0 != DebugLogFile)
    {
        if (0 == wcscmp(L"-", DebugLogFile))
            DebugLogHandle = GetStdHandle(STD_ERROR_HANDLE);
        else
            DebugLogHandle = CreateFileW(
                DebugLogFile,
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                0,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                0);
        if (INVALID_HANDLE_VALUE == DebugLogHandle)
        {
            fail(L"cannot open debug log file");
            goto usage;
        }

        FspDebugLogSetHandle(DebugLogHandle);
    }

    Result = NullfsCreateFunnel(
        Flags | OtherFlags,
        FileInfoTimeout,
        MaxFileNodes,
        MaxFileSize,
        SlowioMaxDelay,
        SlowioPercentDelay,
        SlowioRarefyDelay,
        FileSystemName,
        VolumePrefix,
        RootSddl,
        &Nullfs);
    if (!NT_SUCCESS(Result))
    {
        fail(L"cannot create NULLFS");
        goto exit;
    }

    FspFileSystemSetDebugLog(NullfsFileSystem(Nullfs), DebugFlags);

    if (0 != MountPoint && L'\0' != MountPoint[0])
    {
        Result = FspFileSystemSetMountPoint(NullfsFileSystem(Nullfs),
            L'*' == MountPoint[0] && L'\0' == MountPoint[1] ? 0 : MountPoint);
        if (!NT_SUCCESS(Result))
        {
            fail(L"cannot mount NULLFS");
            goto exit;
        }
    }

    Result = NullfsStart(Nullfs);
    if (!NT_SUCCESS(Result))
    {
        fail(L"cannot start NULLFS");
        goto exit;
    }

    MountPoint = FspFileSystemMountPoint(NullfsFileSystem(Nullfs));

    info(L"%s -t %ld -n %ld -s %ld%s%s%s%s%s%s",
        L"" PROGNAME, FileInfoTimeout, MaxFileNodes, MaxFileSize,
        RootSddl ? L" -S " : L"", RootSddl ? RootSddl : L"",
        0 != VolumePrefix && L'\0' != VolumePrefix[0] ? L" -u " : L"",
            0 != VolumePrefix && L'\0' != VolumePrefix[0] ? VolumePrefix : L"",
        MountPoint ? L" -m " : L"", MountPoint ? MountPoint : L"");

    Service->UserContext = Nullfs;
    Result = STATUS_SUCCESS;

exit:
    if (!NT_SUCCESS(Result) && 0 != Nullfs)
        NullfsDelete(Nullfs);

    return Result;

usage:
    static wchar_t usage[] = L""
        "usage: %s OPTIONS\n"
        "\n"
        "options:\n"
        "    -d DebugFlags       [-1: enable all debug logs]\n"
        "    -D DebugLogFile     [file path; use - for stderr]\n"
        "    -i                  [case insensitive file system]\n"
        "    -f                  [flush and purge cache on cleanup]\n"
        "    -t FileInfoTimeout  [millis]\n"
        "    -n MaxFileNodes     [default 65536]\n"
        "    -s MaxFileSize      [bytes; 0 = unlimited (default)]\n"
        "    -M MaxDelay         [maximum slow IO delay in millis]\n"
        "    -P PercentDelay     [percent of slow IO to make pending]\n"
        "    -R RarefyDelay      [adjust the rarity of pending slow IO]\n"
        "    -F FileSystemName\n"
        "    -S RootSddl         [file rights: FA, etc; NO generic rights: GA, etc.]\n"
        "    -u \\Server\\Share    [UNC prefix (single backslash)]\n"
        "    -m MountPoint       [X:|* (required if no UNC prefix)]\n";

    fail(usage, L"" PROGNAME);

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS SvcStop(FSP_SERVICE *Service)
{
    NULLFS *Nullfs = Service->UserContext;

    NullfsStop(Nullfs);
    NullfsDelete(Nullfs);

    return STATUS_SUCCESS;
}

int wmain(int argc, wchar_t **argv)
{
    /*
     * NULLFS: load winfsp-<arch>.dll. Requires that the executable be linked
     * with /DELAYLOAD:winfsp-<arch>.dll. FspLoad() first tries the DLL by name
     * (e.g. from PATH) and then falls back to the WinFsp install directory as
     * recorded in the registry, so the executable does not need the WinFsp bin
     * directory on its PATH.
     */
    if (!NT_SUCCESS(FspLoad(0)))
        return ERROR_DELAY_LOAD_FAILED;

    return FspServiceRun(L"" PROGNAME, SvcStart, SvcStop, 0);
}
