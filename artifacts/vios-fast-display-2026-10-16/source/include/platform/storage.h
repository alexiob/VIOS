#ifndef PLATFORM_STORAGE_H
#define PLATFORM_STORAGE_H
#include <stddef.h>
/* Synchronous, single-volume FAT32 service. No heap or concurrent handles.
 * Paths use FAT short names (8.3), at most 127 printable ASCII bytes.
 * Whole-file reads set *size=0 on every failure; oversized files are rejected
 * before copying. An I/O failure may leave partial buffer bytes untrusted.
 * Writes replace/truncate the named file and are NOT power-fail atomic.
 * Use a new temporary name plus explicit rename for staged save workflows.
 */
int storage_init(void);
int storage_read_file(const char *path, void *buffer, size_t capacity, size_t *size);
int storage_write_file(const char *path, const void *buffer, size_t size);
int storage_rename(const char *old_path, const char *new_path);
int storage_remove(const char *path);
int storage_mkdir(const char *path);
int storage_sync(void);
const char *storage_error(void);
#endif
