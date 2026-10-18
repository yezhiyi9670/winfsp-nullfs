# NullFS

A "null" file system for Windows, built on [WinFsp](https://github.com/winfsp/winfsp).

It presents a normal drive (`X:`) with a real, writable directory tree, but **no file
payload is ever stored**:

* creating files and directories succeeds, at any path and any depth;
* `Write` succeeds, and the bytes are discarded;
* `Read` succeeds and returns zeroes (like `/dev/zero`);
* the *logical* file size is remembered, so `dir`, `FileSize` and `AllocationSize`
  look plausible to the application;
* the volume always reports ~1 PiB free.

This is intended for the case where a piece of software insists on writing large
files that you know will never be read back. Since nothing is written to disk or
kept in memory, the only cost is the in-memory directory entries.

## Files

| File | What it is |
| --- | --- |
| `nullfs.cpp`, `nullfs.h`, `nullfs-main.c` | the file system (derived from WinFsp v2.1's `memfs` sample) |
| `nullfs-from-memfs.py` | the script that derived them; documents every change |
| `CMakeLists.txt`, `build.bat` | two ways to build |
| `memfs.cpp`, `memfs.h`, `memfs-main.c` | your pristine copy of the sample, kept for reference/diffing |

## What changed relative to MEMFS

The internal symbol names are renamed (`memfs` → `nullfs`, `Memfs` → `Nullfs`,
`MEMFS` → `NULLFS`); apart from that the delta is small and deliberate, so that the
resulting code is a known-good upstream sample with one behaviour changed rather than
a rewrite:

| # | Change |
| --- | --- |
| 1 | `NULLFS_FILE_NODE` no longer has a `FileData` field. |
| 2 | `NullfsFileNodeDelete` no longer frees file data. |
| 3 | `Create` records `AllocationSize` but allocates no storage. |
| 4 | `SetFileSizeInternal` records `AllocationSize`/`FileSize` but never allocates, resizes or zero-fills. |
| 5 | `Read` `memset`s the caller's buffer to zero instead of copying stored data (both the normal and the SLOWIO path). |
| 6 | `Write` throws the buffer away (both paths). |
| 7 | `GetVolumeInfo`/`SetVolumeLabel` report a fixed `NULLFS_VOLUME_SIZE` (1 PiB) instead of `MaxFileNodes * MaxFileSize`. |
| 8 | `-s MaxFileSize` now means "per-file limit", and **`0` = unlimited** (the default; MEMFS capped files at 16 MiB). |
| 9 | Default `-n MaxFileNodes` raised from 1024 to 65536, and it now means "evict above this many nodes" rather than "fail at this many" (see below). |
| 10 | `wmain` calls `FspLoad(0)` first, so the executable locates `winfsp-<arch>.dll` via the registry and does not need WinFsp's `bin` directory on the `PATH`. |

Everything else — EA, reparse points, named streams, SLOWIO, name normalisation,
SDDL/security, `Control`, the SLOWIO options, the large-heap code — is untouched.
The large-heap functions and the SLOWIO machinery are now unused/dead weight.
The complete delta is reviewable with:

```
python3 nullfs-from-memfs.py . .      # regenerate (will abort if memfs.* changed)
diff -u memfs.cpp nullfs.cpp
```

Note that this script produces only the *initial* NullFS; it refuses to overwrite the
current sources, because they have since gained the eviction work described below.

## Infinite metadata: eviction and expiry

MEMFS stops accepting new files once its node table fills up, which made NullFS feel
finite. NullFS now **retires metadata into the void** instead:

* file nodes are queued in creation order; when a `Create` would push the node count
  past `-n`, the oldest regular-file metadata is dropped and the create proceeds, so
  the table never grows past `-n` and creation does not fail just because the
  bookkeeping is full;
* only nodes with **no open handles** are candidates (`RefCount == 1`: only the
  directory table references them), so an eviction can never pull the rug from under an
  open file — a pinned node is simply skipped;
* when the oldest closed file is gone it moves on to **directories**, in two steps:
  the oldest closed *empty* directory first, and — as a last resort — the oldest closed
  directory **together with its whole subtree**. A directory is never dropped while
  children stay behind, because that would silently break every path below it;
* the **root directory** and the **parent directory of the entry being created** are
  never evicted;
* `-n 0` turns eviction off entirely (unbounded growth, your RAM is the ceiling);
  otherwise `-n` must be at least 2, because the root directory always occupies one of
  the slots;
* `-T <seconds>` additionally expires *file* metadata that has not been created or opened for
  that long (default `0` = never expire). The sweep is lazy: it runs from `Create`, at
  most once every `T/4` seconds (clamped to 1–60 s), so a quiet file system keeps its
  idle nodes until the next create.

The consequence to be aware of: an evicted path behaves as though it never existed
(`STATUS_OBJECT_NAME_NOT_FOUND`). If the application recreates it, it comes back as a
new, empty file. A generator that writes each backup once and never revisits it — the
case this file system exists for — never notices. An application holding a *live*
handle keeps working, because the handle pins the metadata.

## Stateless mode (`-B`): nothing remembered at all

`-B` removes the last piece of state, the namespace. Every `Create` and every `Open`
succeeds for any path and hands out a throw-away node that exists only for the
lifetime of the handle:

| | metadata mode (default) | stateless mode (`-B`) |
| --- | --- | --- |
| create file/dir at any path | yes, remembered | yes, but only while a handle is open |
| name collisions | real | impossible |
| file size after close | remembered | forgotten (reopening gives 0) |
| directory listing | real | always empty |
| delete / rename | real | report success, do nothing |
| memory | bounded by `-n` (or by `-T`) | O(open handles) only |
| can creation ever fail | only if every node is held open | no |

**Why it works.** WinFsp's dispatcher only ever asks the file system whether something
exists through `GetSecurityByName`, and `FspAccessCheckEx` (`src/dll/security.c` in
WinFsp 2.1) short-circuits to "access granted" when that operation is not implemented:

```c
if (0 == FileSystem->Interface->GetSecurityByName || ...)
{
    *PGrantedAccess = ...;
    return STATUS_SUCCESS;
}
```

`-B` gives each instance its own copy of the interface table with `GetSecurityByName`
cleared. The FSD therefore performs no name lookup at all, never needs a *parent*
directory to exist, and simply calls `Create`/`Open` and believes the answer. `Create`
never returns `STATUS_OBJECT_NAME_COLLISION` in this mode, so even `CREATE_NEW` on an
"existing" path succeeds.

**What you give up:**

* Sizes are per handle now. An application that closes a file and reopens it to check
  the size sees 0 (this is the assumption the mode is built on: the software does not
  check back).
* Directory enumeration is always empty. If the generator prunes old backups by listing
  the directory, it finds nothing to prune.
* `Rename` is a no-op that reports success.
* Nothing enforces type consistency: a path can be a file in one open and a directory in
  the next.

**Trying it:**

```bat
nullfs-x64.exe -m X: -B

rem any path, any depth, always succeeds:
mkdir X:\a\b\c\d
echo x > X:\a\b\c\d\f.bak
dir X:\                       rem empty: nothing is remembered
type X:\a\b\c\d\f.bak     rem empty: reopening synthesizes a 0-byte file
```

If the generator is happy with that, the file system can no longer run out of anything
except memory for open handles — no `-n`, no eviction, no `-T`. If it is not, drop `-B`
and use the metadata mode above.

## Prerequisites

* **WinFsp v2.1**, installed **with the "Developer" feature** (headers + import
  libraries). The default install root is `C:\Program Files (x86)\WinFsp`, containing
  `inc\winfsp\winfsp.h` and `lib\winfsp-x64.lib` / `winfsp-x86.lib`. Any recent WinFsp
  2.x developer install will do; your `memfs.*` copy is byte-identical to the v2.1 tag,
  so v2.1 matches it exactly.
* **Visual Studio 2019 or 2022** with the C++ toolchain (the sample is C++ that uses
  `std::map` and `std::thread`).
* *Optionally* CMake ≥ 3.20, if you want to use the CMake route.

## Build

### Option A — CMake (recommended)

From any prompt (CMake finds MSVC itself):

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Result: `build\Release\nullfs-x64.exe`. Use `-A Win32` for a 32-bit build, and add
`-DWINFSP_ROOT="D:\tools\WinFsp"` if WinFsp is somewhere else. CMake prints the
include directory and library it settled on, and fails early with a readable message
if the developer files are missing.

### Option B — `build.bat`

Open an **x64 Native Tools Command Prompt for VS** (so that `cl.exe` is on `PATH`)
and run:

```bat
build.bat
```

Result: `nullfs-x64.exe` in the current directory. `build.bat x86` builds the 32-bit
variant. Set `WINFSP_ROOT` first if WinFsp is not in the default location.

### Option C — by hand in Visual Studio

Create an empty **Console Application** (C++), add the three source files, then set —
these are the settings WinFsp's own tutorial prescribes, plus `advapi32.lib`:

* `C/C++ → General → Additional Include Directories`: `$(MSBuildProgramFiles32)\WinFsp\inc`
* `Linker → Input → Additional Dependencies`:
  `$(MSBuildProgramFiles32)\WinFsp\lib\winfsp-$(PlatformTarget).lib`, `advapi32.lib`
  and `delayimp.lib`
* `Linker → Input → Delay Loaded DLLs`: `winfsp-$(PlatformTarget).dll`
* `C/C++ → Preprocessor → Preprocessor Definitions`: add `UNICODE;_UNICODE`

The delay-load setting matters: `wmain` calls `FspLoad(0)`, which finds the WinFsp
DLL through the registry. If you skip delay-loading you must instead copy
`winfsp-x64.dll` next to the executable or put `C:\Program Files (x86)\WinFsp\bin`
on the `PATH`, otherwise Windows will refuse to start the program with a
"missing DLL" error before `wmain` ever runs.

### If the link fails

`LNK2019: unresolved external symbol` for any of `GetSecurityDescriptorLength`,
`ConvertStringSecurityDescriptorToSecurityDescriptorW`, `RegGetValueW`,
`SetSecurityInfo`, `GetSecurityInfo` and friends means `advapi32.lib` is not on the
link line (those are the ACL/SDDL calls, and `RegGetValueW` comes from the `FspLoad()`
helper in `winfsp.h`). Both `CMakeLists.txt` and `build.bat` link it; a hand-made
Visual Studio project from the tutorial alone may not, because the tutorial's
"Win32 Console Application" template silently supplies it while a bare `cl`
invocation does not.

## Run

```bat
nullfs-x64.exe -m X:
```

Leave it running; the mount disappears when the process exits (Ctrl-C stops it
cleanly). Other options: `-B` (stateless black hole, see below), `-m *` (auto-assign), `-i`
(case-insensitive names), `-n <nodes|0>` (eviction threshold; `0` = never evict),
`-s <bytes|0>`, `-T <seconds|0>` (expire idle metadata; `0` = never), `-F <name>`,
`-d -1` (debug logging).

If mounting the drive letter fails with an access-denied error, re-run from an
elevated prompt. For a file system that should be available to all users, WinFsp's
launcher can manage it — see `fsreg.bat` and the *Service Architecture* document in
the WinFsp docs.

## Pointing the defective application at it

Mount NullFS, create a directory inside it, and redirect the application's backup
directory to that mount point with a directory junction, so the application keeps
using the path it already has configured:

```bat
mkdir X:\backups
ren  D:\App\Backups Backups.real
mklink /J D:\App\Backups X:\backups
```

* Do this while the application is stopped; `mklink` requires the link path not to
  exist.
* `rmdir D:\App\Backups` later removes only the junction, not the real directory
  (which is still there as `Backups.real`).
* If the application lets you configure its backup directory, you can of course skip
  the junction and point it straight at `X:\something`.

## Behaviour and limits

| | |
| --- | --- |
| Directories at arbitrary depth | yes (kept in memory) |
| `Write`, `SetEndOfFile`, preallocation | succeed; sizes are bookkeeping only |
| Data written | discarded; never stored on disk or in memory |
| `Read` | returns zeroes up to the logical size, then end-of-file |
| Free space reported | 1 PiB, always |
| `Delete`, `Rename`, attributes, ACLs | work, in memory |
| Persistence | none — the whole tree disappears when the process exits |
| RAM per file/directory | roughly 1.2 KB (a 512-`WCHAR` name buffer plus metadata and a copy of the security descriptor); bounded by `-n` (default 65536, so on the order of 100 MB), which is the eviction threshold rather than a hard limit |
| Metadata growth | with eviction on (the default) the table stays at `-n` nodes: oldest closed file first, then oldest empty directory, then a directory with its subtree; `-T` can prune idle files sooner; `-B` keeps no metadata at all |
| Evicted paths | stop existing (`not found`); recreating one works and starts empty |

Things to be aware of for this use case:

* **Read-back returns zeroes.** If the application ever reads a backup back and
  validates it (checksum, size-by-content, "does my backup restore"), it will see a
  sparse zero-filled file. You said yours never reads them; that assumption is what
  makes NullFS safe here. A write-only application is fine; a verifying one is not.
* **The directory tree is the only thing that grows, and it is bounded.** A generator
  that writes and never deletes stays at `-n` nodes by dropping the oldest file
  metadata; the price is that those paths stop existing. `-n 0` trades that price for
  unbounded memory use. Deleting or renaming files frees nodes immediately.
* **`-s` is parsed as a 32-bit signed value**, so a per-file cap given on the command
  line is only reliable up to 2 GiB. The default (`-s 0`) imposes no cap at all, since
  nothing is allocated — leave it alone and file size is limited only by what the
  application asks for.

## Verify it works

```bat
echo hello > X:\t.txt
dir X:\t.txt
type X:\t.txt
rem type prints nothing meaningful: the 6 bytes read back are all zeroes

powershell -c "$f=[IO.File]::Create('X:\big.bin'); $f.SetLength(1GB); $f.Close()"
dir X:\big.bin
rem 1,073,741,824 bytes, and nothing was written anywhere

powershell -c "Get-PSDrive X | Select-Object Used,Free"
```

(`fsutil file createnew X:\big.bin 1073741824` does the same thing but usually wants
an elevated prompt.)

To watch the eviction work, mount with a tiny table and write more files than it holds —
only the most recent `-n` should survive:

```bat
nullfs-x64.exe -m X: -n 8
rem in another window:
for /L %i in (1,1,20) do @echo x > X:\f%i.txt
dir /b X:\
rem the 7 newest files survive (f14.txt .. f20.txt); the root directory takes the
rem 8th slot, and f1.txt .. f13.txt were dropped into the void
```
