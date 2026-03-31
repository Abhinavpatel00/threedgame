#ifndef FS_H
#define FS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize PhysFS and configure read/write roots.
// - argv0: pass argv[0] when available, or NULL.
// - write_dir: host path for writable data; if NULL, uses a pref dir.
// - read_dir: host path to mount for reads; if NULL, uses base dir.
bool fs_init(const char* argv0, const char* write_dir, const char* read_dir);

// Deinitialize PhysFS.
void fs_shutdown(void);

// Read an entire file into memory (caller owns buffer, free with fs_free).
bool fs_read_all(const char* path, void** out_data, size_t* out_size);

// Write an entire buffer to a file.
bool fs_write_all(const char* path, const void* data, size_t size);

// Check if a file or directory exists.
bool fs_exists(const char* path);

// Create a directory (recursively) in the write area.
bool fs_mkdirs(const char* path);

// Free memory returned by fs_read_all.
void fs_free(void* data);

#ifdef __cplusplus
}
#endif

#endif
