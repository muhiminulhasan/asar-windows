<div align="center">

<img src="resources/asar.ico" width="96" alt="asar-windows logo">

# asar-windows

**Open, extract, create and modify Electron ASAR archives directly from
Windows Explorer and 7-Zip.**

Free and open source (MIT) · No Electron or Node.js required at run time ·
Static CRT, zero external dependencies

[![CI](https://github.com/muhiminulhasan/asar-windows/actions/workflows/ci.yml/badge.svg)](https://github.com/muhiminulhasan/asar-windows/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue)](https://github.com/muhiminulhasan/asar-windows)
[![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org)
[![7-Zip](https://img.shields.io/badge/7--Zip-23%2B%20plugin-E8730C)](https://www.7-zip.org)

</div>

---

## Table of contents

- [Why](#why)
- [Features](#features)
- [Components](#components)
- [Installation](#installation)
- [Usage](#usage)
  - [Windows Explorer](#windows-explorer)
  - [7-Zip](#7-zip)
  - [Command line](#command-line)
  - [Viewer](#viewer)
- [Compatibility](#compatibility)
- [FAQ](#faq)
- [Building from source](#building-from-source)
- [Project structure](#project-structure)
- [How ASAR works](#how-asar-works)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)
- [Author](#author)

## Why

Electron apps ship their source and assets inside `.asar` archives. On
Windows, peeking inside one used to mean installing Node.js just to run the
official `asar` CLI: there was no free, native way to browse one.
**asar-windows** makes `.asar` a first-class archive type on Windows: native
shell integration, a real 7-Zip plugin, a viewer and a CLI. All MIT
licensed, all statically linked, none of it needing Electron.

## Features

- 📂 **Explorer integration**: right-click any `.asar` file to extract or
  open it; right-click any folder to pack it into an `.asar` archive
- 🗂️ **Browse inside Explorer**: double-click an `.asar` and Explorer opens
  it *as a folder*, exactly like a `.zip` (shell namespace extension);
  drag files out of the archive, or double-click an entry to open it
- 🗜️ **7-Zip plugin**: `.asar` becomes a native archive type in 7-Zip File
  Manager and every `7z.exe` invocation (`l`, `x`, `a`, `t`)
- 👁️ **Viewer app**: browse contents, extract selected entries or everything,
  double-click to preview a file, add files to the archive, drag & drop
- ⌨️ **CLI**: scriptable list / extract / pack / add / verify
- 🔐 **Integrity aware**: reads and writes the official SHA-256 whole-file +
  4 MiB block digests; `asar t` verifies an archive's payloads
- ➕ **Efficient adds**: appending to an archive rewrites only the header;
  existing payloads and offsets stay untouched
- 🖇️ **Format complete**: nested/empty directories, symlinks, `unpacked`
  sidecar entries, executables flag
- 🧩 **Self-contained**: statically linked CRT; the only third-party files
  are LGPL interface *headers* from the 7-Zip SDK (no 7-Zip code is linked)
- ⚖️ **x64 · x86 · ARM64**: every component builds for all three
  architectures; the installer picks the right `7z-asar.dll` for your 7-Zip
  (read from its PE header) and the right shell binaries for your Explorer
- 👤 **Per-user install**: Explorer registration writes only to `HKCU`;
  no administrator rights required

## Components

| Binary | Description |
|---|---|
| `7z-asar.dll` | 7-Zip archive format plugin (`IInArchive` + `IOutArchive`) |
| `asarshell.dll` | Explorer context menu extension (COM, per-user self-registration) |
| `asarview.exe` | Win32 viewer application |
| `asar.exe` | Command line tool |
| `libasar` | The C++17 ASAR library all of the above share |

## Installation

### Release binaries

Each release ships **separate zips per architecture** (x64, x86, ARM64):

| Zip | Contents | Use |
|---|---|---|
| `7z-plugin-<ver>-<arch>.zip` | `7z-asar.dll` | Copy into `<7-Zip>\Formats\` — .asar becomes a native archive type in 7-Zip |
| `asarfolder-<ver>-<arch>.zip` | `asarfolder.dll`, `asar.ico` | Standalone: double-click .asar browses it inside Explorer (regsvr32 the dll) |
| `explorer-tools-<ver>-<arch>.zip` | `asarshell.dll`, `asarview.exe`, `asar.exe`, `asar.ico` | Right-click Extract/Pack, viewer, CLI (pairs with asarfolder) |
| `asar-lib-<ver>-<arch>.zip` | `asar.dll`, `asar.lib`, `asar_c.h` | For developers: use libasar from your own C/C++ programs via the C API |

`install.ps1` (inside the explorer zip on releases; at `scripts\` in the repo)
reads the architecture of your OS and of your 7-Zip installation and picks the
right files automatically. The installer restarts Explorer for you when it has
to replace a DLL the shell still has loaded.

Install everything:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1
```

| What | Where it goes | Admin needed |
|---|---|---|
| Explorer integration + viewer + CLI | `%LOCALAPPDATA%\AsarTools` (registered in `HKCU`, matching your OS architecture) | No |
| 7-Zip plugin | `<7-Zip>\Formats\7z-asar.dll` (matching your 7-Zip's architecture) | Yes (7-Zip lives in Program Files) |

For a portable 7-Zip, point the script at it:

```powershell
powershell -File scripts\install.ps1 -SevenZipPath "D:\Tools\7-Zip"
```

Uninstall everything with:

```powershell
powershell -File scripts\uninstall.ps1
```

### Using the library from your own code (`asar.dll`)

`asar.dll` exposes a small C API — link `asar.lib` and include
[`libasar/include/asar/asar_c.h`](libasar/include/asar/asar_c.h):

```c
asar_archive_t* a = asar_open(L"app.asar");
asar_extract(a, L"C:\\temp\\app");       // extract everything
asar_close(a);
```

See `asar_c.h` for the full surface: list (callback), read-file, pack, add,
remove, verify (SHA-256), and thread-local error strings via
`asar_last_error()`.

### Build from source

Requirements: Windows 10/11 and Visual Studio 2022 Build Tools with the
**C++ workload** (add the **C++ ARM64 build tools** component for the ARM64
target) and **C++ CMake tools**. Nothing else: the 7-Zip SDK headers are
vendored under [`vendor/7zip`](vendor/7zip/README.md).

```bat
scripts\build.cmd              build all architectures: x64, Win32 (x86), ARM64
scripts\build.cmd x64          build just one
scripts\build.cmd --clean x64  wipe and rebuild one
```

The script locates MSVC and the Windows SDK on its own and runs the test
suite for every architecture the host can execute (x64 and Win32 on an x64
machine; ARM64 is cross-compiled and tested on ARM64 hosts). Outputs land in
`build\<arch>\<component>\Release\`.

## Usage

### Windows Explorer

Right-click an `.asar` file:

- **Extract to "app\"**: unpacks into a subfolder next to the archive
- **Extract here**: unpacks into the archive's own folder
- **Open in ASAR viewer**: launches `asarview.exe`

Right-click a folder:

- **Pack to "folder.asar"**: creates an archive from the whole tree

> Windows 11: the commands appear in the classic menu
> (*Show more options*). Native integration into the new menu is on the
> [roadmap](#roadmap).

### 7-Zip

After the plugin is installed, `.asar` is just another archive format:

```bat
7z l app.asar               rem list contents
7z x app.asar -oout         rem extract
7z a app.asar newfile.txt   rem add / update entries
7z t app.asar               rem test archive
```

> Note: if 7-Zip reports an `.asar` file as an unknown format, the file is
> not a standard ASAR archive (see [FAQ](#faq)). The ASAR format itself has
> no encryption, so there is nothing to enter or unlock.

### Command line

```
asar l <archive>                            list contents
asar x <archive> [destdir]                  extract (default: <archive dir>\<stem>)
asar p <srcdir> <archive>                   pack a directory
asar a <archive> <file...> [--to <subdir>]  add files or directories
asar t <archive>                            verify payload integrity (SHA256)
```

Examples:

```bat
asar p my-app\src my-app.asar
asar x my-app.asar C:\temp\app
asar a my-app.asar banner.png --to assets
asar t my-app.asar
```

Use `--no-integrity` with `p`/`a` to skip SHA-256 computation.

### Viewer

`asarview.exe` opens archives via drag & drop, the *File → Open* dialog, or
the Explorer *Open* association:

- browse the tree with size / type / unpacked columns
- extract selected entries or the whole archive to any folder
- double-click an entry to preview it (extracted to a temp file and opened
  with the default application)
- *File → Add files…* appends files to the archive in place

<!-- TODO: add screenshots here
<p align="center">
  <img src="docs/images/explorer-menu.png" width="420">
  <img src="docs/images/viewer.png" width="420">
</p>
-->

## Compatibility

Verified in CI against the official
[`@electron/asar`](https://github.com/electron/asar) npm package, in both
directions:

- ✅ official `asar pack` → `asar-windows x` produces byte-identical files
- ✅ `asar-windows p` → official `asar extract` produces byte-identical files
- ✅ listings agree
- ✅ `asar-windows t` verifies the official archives' SHA-256 integrity
  metadata (whole file + 4 MiB blocks, including the empty-file edge case)
- ✅ `asar-windows a` on an official archive keeps all original entries
  intact, and the official tool reads the added entries

Known limitation: `unpacked` entries (Electron's sidecar files living in
`<archive>.unpacked/`) resolve correctly in Explorer, the viewer and the
CLI, but the 7-Zip plugin reports them as unavailable: a plugin only
receives a stream and cannot locate the sidecar file.

## FAQ

**Why won't my `.asar` file open?**

The ASAR format has no encryption. If asar-windows rejects a file, it isn't
a standard ASAR archive: some third-party "protector" tools produce modified
archives that only their own patched Electron builds can read. Standard
archives created with `@electron/asar`, electron-builder, or asar-windows
always work.

## Building from source

```bat
git clone https://github.com/muhiminulhasan/asar-windows.git
cd asar-windows
scripts\build.cmd
```

Run the full verification locally:

```bat
ctest --test-dir build\x64 -C Release --output-on-failure
node scripts\crosscheck.mjs build\x64\tools\asar\Release\asar.exe
```

`crosscheck.mjs` needs Node.js (it drives `npx @electron/asar`); everything
else builds and tests with only the VS toolchain. To regenerate the icon:
`powershell -File scripts\make-icon.ps1`.

## Project structure

```
libasar/      ASAR format library (C++17 core + C API DLL)
asar_c.h      C header for asar.dll consumers (in libasar/include/asar)
7z-asar/      7-Zip format plugin (IInArchive + IOutArchive)
asarshell/    Explorer context menu extension (COM, per-user registration)
asarview/     Win32 viewer
tools/asar/   CLI
tests/        round-trip test + in-process 7-Zip COM harness
vendor/7zip/  7-Zip SDK interface headers (LGPL, see vendor/7zip/README.md)
docs/         ASAR container format specification
scripts/      build, install/uninstall, icon generator, cross-check
```

## How ASAR works

An `.asar` file is a JSON header describing the file tree, followed by the
concatenated payload of every file. Offsets in the header are decimal
strings relative to the end of the header block, so *adding* a file only
requires appending its payload and rewriting the header; existing entries
keep their offsets. The full format, including the Chromium pickle framing
and the integrity metadata layout, is documented in
[`docs/asar-format.md`](docs/asar-format.md).

## Roadmap

- [ ] Windows 11 modern context menu (`IExplorerCommand` + sparse MSIX package)
- [ ] Progress dialog / background extraction in the shell extension
- [ ] Localized menu strings
- [ ] Icon overlay handler for `.asar` files

## Contributing

Issues and pull requests are welcome! For larger changes, please open an
issue first to discuss what you would like to change.

1. Fork the repository
2. Create your branch (`git checkout -b feature/my-feature`)
3. Make sure `scripts\build.cmd` and the cross-check pass
4. Open a pull request

## License

[MIT](LICENSE) © [A. S. M. Muhiminul Hasan](https://github.com/muhiminulhasan).

The vendored 7-Zip SDK headers under `vendor/7zip/` remain under their own
[LGPL-2.1+](vendor/7zip/README.md) terms. No 7-Zip code is linked into any
binary; the plugin is loaded by your existing 7-Zip installation.

## Author

<div align="center">

**A. S. M. Muhiminul Hasan**

[![GitHub](https://img.shields.io/badge/GitHub-muhiminulhasan-181717?logo=github)](https://github.com/muhiminulhasan)
[![Website](https://img.shields.io/badge/Web-muhiminulhasan.com-0A66C2)](https://muhiminulhasan.com)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-in/muhiminulhasan-0A66C2?logo=linkedin)](https://www.linkedin.com/in/muhiminulhasan)
[![X](https://img.shields.io/badge/X-@muhiminulhasan-000000?logo=x)](https://x.com/muhiminulhasan)

If this project saved you a headache, consider leaving a ⭐ on the repo!

</div>

## Acknowledgments

- [Igor Pavlov](https://www.7-zip.org): 7-Zip and its excellent plugin
  architecture (SDK headers used under LGPL)
- [Electron](https://www.electronjs.org/docs/latest/tutorial/asar-archives):
  the ASAR format and the reference `@electron/asar` implementation used for
  compatibility testing
