#!/usr/bin/env python3
"""
Derive the NullFS sources (nullfs.h / nullfs.cpp / nullfs-main.c) from a pristine
copy of the WinFsp v2.1 MEMFS sample (memfs.h / memfs.cpp / memfs-main.c).

Usage:  python3 nullfs-from-memfs.py [srcdir] [dstdir] [--force]

This script produces GENERATION 1 of NullFS only (the memfs -> null file system
transform). The checked-in sources have since gained metadata eviction/expiry, so
the script refuses to overwrite them unless --force is given.

The script is deliberately a *textual* transformation: every edit is an exact
string replacement that must match an expected number of times, so a mismatch
aborts instead of silently producing a wrong file. Run `diff -u memfs.cpp
nullfs.cpp` afterwards to review the complete delta.

Verified against WinFsp v2.1 (sha256 847dc5f5... memfs.cpp, 72db7a92... memfs.h,
f54eca0e... memfs-main.c).
"""

import pathlib
import sys

NOTICE = """ * @copyright 2015-2025 Bill Zissimopoulos
 *
 * NULLFS: this file is a modified copy of WinFsp v2.1 tst/memfs.
 * Modifications: file payload data is never stored -- writes are discarded and
 * reads return zeroes. See README.md for the full list of changes.
 */"""

FILES = ["memfs.h", "memfs.cpp", "memfs-main.c"]


def rename(text):
    # identifier prefix rename; done first so the edits below can be written
    # against the final spelling
    for old, new in (("MEMFS", "NULLFS"), ("Memfs", "Nullfs"), ("memfs", "nullfs")):
        text = text.replace(old, new)
    return text


def sub(text, old, new, count=1):
    found = text.count(old)
    if found != count:
        raise SystemExit(
            "FATAL: expected %d occurrence(s), found %d for:\n%s" % (count, found, old)
        )
    return text.replace(old, new)


def edit_nullfs_cpp(s):
    # --- header notice -----------------------------------------------------
    s = sub(s, " * @copyright 2015-2025 Bill Zissimopoulos\n */", NOTICE)

    # --- 1. the file node no longer has a data buffer ----------------------
    s = sub(s, "    PVOID FileSecurity;\n    PVOID FileData;\n", "    PVOID FileSecurity;\n")

    # --- 2. nothing to free on delete -------------------------------------
    s = sub(s, "    LargeHeapFree(FileNode->FileData);\n", "")

    # --- 3. Create(): record AllocationSize, allocate nothing -------------
    s = sub(
        s,
        """    FileNode->FileInfo.AllocationSize = AllocationSize;
    if (0 != FileNode->FileInfo.AllocationSize)
    {
        FileNode->FileData = LargeHeapAlloc((size_t)FileNode->FileInfo.AllocationSize);
        if (0 == FileNode->FileData)
        {
            NullfsFileNodeDelete(FileNode);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
""",
        """    /*
     * NULLFS: record the allocation size, but never allocate any storage.
     * Everything written to this file is discarded.
     */
    FileNode->FileInfo.AllocationSize = AllocationSize;
""",
    )

    # --- 4. Create(): 0 means "no per-file size limit" ---------------------
    s = sub(
        s,
        "    if (AllocationSize > Nullfs->MaxFileSize)\n",
        "    if (0 != Nullfs->MaxFileSize && AllocationSize > Nullfs->MaxFileSize)\n",
    )

    # --- 5. SetFileSizeInternal(): allocation is bookkeeping only ----------
    s = sub(
        s,
        """            if (NewSize > Nullfs->MaxFileSize)
                return STATUS_DISK_FULL;

            PVOID FileData = LargeHeapRealloc(FileNode->FileData, (size_t)NewSize);
            if (0 == FileData && 0 != NewSize)
                return STATUS_INSUFFICIENT_RESOURCES;

            FileNode->FileData = FileData;

            FileNode->FileInfo.AllocationSize = NewSize;
""",
        """            if (0 != Nullfs->MaxFileSize && NewSize > Nullfs->MaxFileSize)
                return STATUS_DISK_FULL;

            /*
             * NULLFS: the allocation size is a bookkeeping value only;
             * no storage is allocated, resized or freed.
             */
            FileNode->FileInfo.AllocationSize = NewSize;
""",
    )

    # --- 6. SetFileSizeInternal(): nothing to zero-fill --------------------
    s = sub(
        s,
        """            if (FileNode->FileInfo.FileSize < NewSize)
                memset((PUINT8)FileNode->FileData + FileNode->FileInfo.FileSize, 0,
                    (size_t)(NewSize - FileNode->FileInfo.FileSize));
            FileNode->FileInfo.FileSize = NewSize;
""",
        """            /* NULLFS: no storage to zero-fill */
            FileNode->FileInfo.FileSize = NewSize;
""",
    )

    # --- 7. Read(): return zeroes (this text occurs twice: with and without
    #        the SLOWIO pending-I/O path) -----------------------------------
    s = sub(
        s,
        "    memcpy(Buffer, (PUINT8)FileNode->FileData + Offset, (size_t)(EndOffset - Offset));\n",
        "    /* NULLFS: reads behave like /dev/zero */\n"
        "    memset(Buffer, 0, (size_t)(EndOffset - Offset));\n",
        count=2,
    )

    # --- 8. Write(): discard (also twice, same reason) ---------------------
    s = sub(
        s,
        "    memcpy((PUINT8)FileNode->FileData + Offset, Buffer, (size_t)(EndOffset - Offset));\n",
        "    /* NULLFS: written data is discarded */\n",
        count=2,
    )

    # --- 9. a fixed, fake volume size --------------------------------------
    s = sub(
        s,
        "#define NULLFS_SECTORS_PER_ALLOCATION_UNIT 1\n",
        "#define NULLFS_SECTORS_PER_ALLOCATION_UNIT 1\n"
        "\n"
        "/* NULLFS: fixed, fake volume size reported to the OS (1 PiB) */\n"
        "#define NULLFS_VOLUME_SIZE              (1ULL << 50)\n",
    )

    # --- 10/11. GetVolumeInfo() and SetVolumeLabel() ------------------------
    s = sub(
        s,
        """    VolumeInfo->TotalSize = Nullfs->MaxFileNodes * (UINT64)Nullfs->MaxFileSize;
    VolumeInfo->FreeSize = (Nullfs->MaxFileNodes - NullfsFileNodeMapCount(Nullfs->FileNodeMap)) *
        (UINT64)Nullfs->MaxFileSize;
""",
        """    /* NULLFS: nothing is ever stored, so report a fixed volume size */
    VolumeInfo->TotalSize = NULLFS_VOLUME_SIZE;
    VolumeInfo->FreeSize = NULLFS_VOLUME_SIZE;
""",
    )
    s = sub(
        s,
        """    VolumeInfo->TotalSize = Nullfs->MaxFileNodes * Nullfs->MaxFileSize;
    VolumeInfo->FreeSize =
        (Nullfs->MaxFileNodes - NullfsFileNodeMapCount(Nullfs->FileNodeMap)) * Nullfs->MaxFileSize;
""",
        """    /* NULLFS: nothing is ever stored, so report a fixed volume size */
    VolumeInfo->TotalSize = NULLFS_VOLUME_SIZE;
    VolumeInfo->FreeSize = NULLFS_VOLUME_SIZE;
""",
    )

    return s


def edit_nullfs_main_c(s):
    s = sub(s, " * @copyright 2015-2025 Bill Zissimopoulos\n */", NOTICE)

    # a recursive backup generator can create a lot of files
    s = sub(s, "    ULONG MaxFileNodes = 1024;\n", "    ULONG MaxFileNodes = 65536;\n")

    # 0 == no per-file size limit (MEMFS capped files at 16 MiB)
    s = sub(
        s,
        "    ULONG MaxFileSize = 16 * 1024 * 1024;\n",
        "    ULONG MaxFileSize = 0;          /* 0: no per-file size limit */\n",
    )

    s = sub(
        s,
        '        "    -n MaxFileNodes\\n"\n        "    -s MaxFileSize      [bytes]\\n"\n',
        '        "    -n MaxFileNodes     [default 65536]\\n"\n'
        '        "    -s MaxFileSize      [bytes; 0 = unlimited (default)]\\n"\n',
    )

    # locate and load winfsp-<arch>.dll (pairs with /DELAYLOAD at link time)
    s = sub(
        s,
        'int wmain(int argc, wchar_t **argv)\n'
        '{\n'
        '    return FspServiceRun(L"" PROGNAME, SvcStart, SvcStop, 0);\n'
        '}\n',
        'int wmain(int argc, wchar_t **argv)\n'
        '{\n'
        '    /*\n'
        '     * NULLFS: load winfsp-<arch>.dll. Requires that the executable be linked\n'
        '     * with /DELAYLOAD:winfsp-<arch>.dll. FspLoad() first tries the DLL by name\n'
        '     * (e.g. from PATH) and then falls back to the WinFsp install directory as\n'
        '     * recorded in the registry, so the executable does not need the WinFsp bin\n'
        '     * directory on its PATH.\n'
        '     */\n'
        '    if (!NT_SUCCESS(FspLoad(0)))\n'
        '        return ERROR_DELAY_LOAD_FAILED;\n'
        '\n'
        '    return FspServiceRun(L"" PROGNAME, SvcStart, SvcStop, 0);\n'
        '}\n',
    )

    return s


def edit_nullfs_h(s):
    return sub(s, " * @copyright 2015-2025 Bill Zissimopoulos\n */", NOTICE)


def main():
    args = [a for a in sys.argv[1:] if a != "--force"]
    srcdir = pathlib.Path(args[0] if len(args) > 0 else ".")
    dstdir = pathlib.Path(args[1] if len(args) > 1 else ".")

    if "--force" not in sys.argv:
        for name in FILES:
            out = dstdir / name.replace("memfs", "nullfs")
            if out.exists():
                raise SystemExit(
                    "FATAL: %s already exists. This script only regenerates the initial\n"
                    "NullFS and would discard the metadata-eviction work. Pass --force to\n"
                    "overwrite it anyway." % out)

    for name in FILES:
        text = (srcdir / name).read_text(encoding="utf-8")
        text = rename(text)
        if name.endswith(".cpp"):
            text = edit_nullfs_cpp(text)
        elif name.endswith("-main.c"):
            text = edit_nullfs_main_c(text)
        else:
            text = edit_nullfs_h(text)
        out = dstdir / name.replace("memfs", "nullfs")
        out.write_text(text, encoding="utf-8")
        print("wrote %s (%d lines)" % (out, text.count("\n")))


if __name__ == "__main__":
    main()
