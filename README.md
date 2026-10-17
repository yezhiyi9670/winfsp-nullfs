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
| 9 | Default `-n MaxFileNodes` raised from 1024 to 65536. |
| 10 | `wmain` calls `FspLoad(0)` first, so the executable locates `winfsp-<arch>.dll` via the registry and does not need WinFsp's `bin` directory on the `PATH`. |

Everything else — EA, reparse points, named streams, SLOWIO, name normalisation,
SDDL/security, `Control`, the SLOWIO options, the large-heap code — is untouched.
The large-heap functions and the SLOWIO machinery are now unused/dead weight; see
*Optional further slimming* below. The complete delta is reviewable with:

```
python3 nullfs-from-memfs.py . .      # regenerate (will abort if memfs.* changed)
diff -u memfs.cpp nullfs.cpp
```

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
cleanly). Other options: `-m *` (auto-assign), `-i` (case-insensitive names),
`-n <nodes>`, `-s <bytes|0>`, `-F <name>`, `-d -1` (debug logging).

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
| RAM per file/directory | roughly 1.2 KB (a 512-`WCHAR` name buffer plus metadata and a copy of the security descriptor); `-n` caps the number of nodes (default 65536, so on the order of 100 MB if the cap is ever reached) |

Things to be aware of for this use case:

* **Read-back returns zeroes.** If the application ever reads a backup back and
  validates it (checksum, size-by-content, "does my backup restore"), it will see a
  sparse zero-filled file. You said yours never reads them; that assumption is what
  makes NullFS safe here. A write-only application is fine; a verifying one is not.
* **The only resource that grows is the directory tree.** A generator that writes and
  never deletes will accumulate one in-memory node per file. Raise or lower `-n`
  accordingly; deleting files frees the nodes.
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

## Optional further slimming

If you want a smaller file rather than the minimal-diff version, comment out the
`#define NULLFS_…` feature macros near the top of `nullfs.cpp`
(`NULLFS_SLOWIO`, `NULLFS_EA`, `NULLFS_REPARSE_POINTS`, `NULLFS_NAMED_STREAMS`,
`NULLFS_DIRINFO_BY_NAME`, `NULLFS_CONTROL`, `NULLFS_WSL`,
`NULLFS_DISPATCHER_STOPPED`, `NULLFS_NAME_NORMALIZATION`) and delete the now-unused
large-heap block. Each macro is a real compile-time toggle and the surrounding code is
`#if`-guarded, but **I did not do this and could not compile-test those combinations**,
so it is left as an exercise rather than presented as working. Note that dropping
`NULLFS_SLOWIO` makes the `-M`/`-P`/`-R` options no-ops.

## Provenance and licence

`nullfs.cpp`, `nullfs.h` and `nullfs-main.c` are modified copies of the MEMFS sample
that ships with WinFsp (`tst/memfs`, tag `v2.1`), which is
**GPLv3** (with a commercial-licence alternative from the WinFsp author). The original
copyright notice is retained and each file carries a note describing the
modification. If you plan to redistribute this, the GPLv3 obligations apply — for
local, internal use there is nothing to worry about. WinFsp's own driver and DLL are
separately installed and licensed by the WinFsp project.

## What I could not verify

I wrote and reviewed this in a Linux environment with no Windows toolchain, so the
code has **not been compiled or run**. What I did verify mechanically: the
rename-normalised diff is exactly the changes in the table above (plus the per-file
notice comment and two usage-string tweaks), the brace/paren/`#if` structure is
unchanged apart from the one removed block, no reference to `FileData` survives, and
your sources are byte-identical to the upstream v2.1 tag. Anything in the *Run*,
*junction* and *Verify* sections is untested advice from WinFsp's documented
behaviour, not something I observed.
