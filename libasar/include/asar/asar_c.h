// asar_c.h - C API for the asar.dll library (libasar).
//
// Stable, C-linkable surface for other languages and runtimes. All strings
// are UTF-16 (wchar_t). All returned buffers are allocated by the DLL and
// must be released with asar_free(). Errors: functions return 0 on success,
// nonzero on failure, with asar_last_error() describing the problem.
//
// Usage (C/C++):
//   link asar.lib (or LoadLibrary("asar.dll") + GetProcAddress)

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#if defined(_WIN32)
#  ifdef ASAR_BUILDING_DLL
#    define ASAR_CAPI __declspec(dllexport)
#  else
#    define ASAR_CAPI __declspec(dllimport)
#  endif
#else
#  define ASAR_CAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct asar_archive_s* asar_archive_t;

// Entry callback for asar_list(). Return nonzero to stop enumeration.
// `path` is UTF-16, '/'-separated, relative to the archive root.
typedef int (*asar_list_cb)(void* user, const wchar_t* path, int is_dir,
                            int is_link, unsigned long long size);

// Opens an archive. Returns NULL on failure (see asar_last_error).
ASAR_CAPI asar_archive_t asar_open(const wchar_t* archive_path);

// Closes a handle. NULL is allowed.
ASAR_CAPI void asar_close(asar_archive_t h);

// Enumerates every entry (files, directories, links). Returns 0 on success.
// Stops early (returns 1) when the callback returns nonzero.
ASAR_CAPI int asar_list(asar_archive_t h, asar_list_cb cb, void* user);

// Reads a file payload into *data (allocated by the DLL). *size = byte count.
// Free with asar_free(). Returns 0 on success.
ASAR_CAPI int asar_read_file(asar_archive_t h, const wchar_t* path_in_archive,
                             unsigned char** data, uint64_t* size);

// Extracts every entry below dest_dir (created if missing).
// Returns 0 on success.
ASAR_CAPI int asar_extract(asar_archive_t h, const wchar_t* dest_dir);

// Extracts one file to an exact destination path. Returns 0 on success.
ASAR_CAPI int asar_extract_file(asar_archive_t h, const wchar_t* path_in_archive,
                                const wchar_t* dest_file);

// Packs src_dir into a new archive. integrity: nonzero computes SHA-256
// whole-file + 4 MiB block digests. Returns 0 on success.
ASAR_CAPI int asar_pack(const wchar_t* src_dir, const wchar_t* out_archive,
                        int integrity);

// Adds files or directory trees into an existing archive.
// sources/n: array of file-or-directory paths. dest_dir_in_archive: L"" for
// root. Returns the number of file payloads added (>= 0), -1 on error.
ASAR_CAPI int asar_add(const wchar_t* archive_path,
                       const wchar_t* const* sources, size_t source_count,
                       const wchar_t* dest_dir_in_archive, int integrity);

// Removes in-archive paths (files or subtrees). Missing paths are counted
// in *not_removed_count when that pointer is non-NULL. Returns the number
// removed, or -1 on error.
ASAR_CAPI int asar_remove(const wchar_t* archive_path,
                          const wchar_t* const* paths, size_t path_count,
                          size_t* not_found_count);

// Verifies payload integrity (SHA-256, when the header carries digests).
// missing_sidecar_count: unpacked entries whose sidecar file is absent.
// Returns 0 when every present payload verifies, 1 when any mismatch, -1 on
// error.
ASAR_CAPI int asar_verify(asar_archive_t h, uint64_t* verified_bytes,
                          size_t* missing_sidecar_count);

// Frees buffers returned by this DLL (asar_read_file data, ...).
ASAR_CAPI void asar_free(void* p);

// Last error message for the calling thread (UTF-16; valid until the next
// call into the DLL on the same thread). NULL when no error occurred.
ASAR_CAPI const wchar_t* asar_last_error(void);

// Version of the DLL, e.g. L"0.1.0".
ASAR_CAPI const wchar_t* asar_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
