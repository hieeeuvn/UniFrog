#ifndef UNIFROG_RA_FS_WIN32_H
#define UNIFROG_RA_FS_WIN32_H

#include "../core/ra_sync_core.h"

/* Accept either unifrog_data or its achievements folder. */
int ra_fs_win32_resolve_root(const char *selected, char *out, size_t size);
int ra_fs_win32_init(const char *selected);

extern const ra_sync_fs_t ra_fs_win32;

#endif
