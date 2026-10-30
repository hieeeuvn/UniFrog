/*
 * Win32 filesystem backend for the RA sync core.
 *
 * The user picks the SD card folder that contains unifrog_data (or the
 * achievements folder itself). Everything the core touches is relative to
 * <root>/achievements, so the selected path is normalised once at startup.
 */
#include "ra_fs_win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char fs_base[MAX_PATH * 2];

static int path_join(char *out, size_t size, const char *path)
{
   int written = snprintf(out, size, "%s\\%s", fs_base, path ? path : "");

   return written > 0 && (size_t)written < size ? 0 : -1;
}

int ra_fs_win32_resolve_root(const char *selected, char *out, size_t size)
{
   char probe[MAX_PATH * 2];
   DWORD attributes;

   if (!selected || !selected[0])
      return -1;
   snprintf(probe, sizeof(probe), "%s\\achievements", selected);
   attributes = GetFileAttributesA(probe);
   if (attributes != INVALID_FILE_ATTRIBUTES &&
       (attributes & FILE_ATTRIBUTE_DIRECTORY))
      snprintf(out, size, "%s", probe);
   else
      snprintf(out, size, "%s", selected);
   return 0;
}

int ra_fs_win32_init(const char *selected)
{
   return ra_fs_win32_resolve_root(selected, fs_base, sizeof(fs_base));
}

static int fs_exists(void *userdata, const char *path)
{
   char full[MAX_PATH * 2];
   DWORD attributes;

   (void)userdata;
   if (path_join(full, sizeof(full), path) != 0)
      return 0;
   attributes = GetFileAttributesA(full);
   return attributes != INVALID_FILE_ATTRIBUTES;
}

static int fs_list(void *userdata, const char *dir,
   int (*callback)(void *ud, const char *name), void *callback_ud)
{
   char pattern[MAX_PATH * 2];
   WIN32_FIND_DATAA entry;
   HANDLE handle;

   (void)userdata;
   if (!callback)
      return 0;
   if (path_join(pattern, sizeof(pattern), dir) != 0)
      return -1;
   snprintf(pattern + strlen(pattern), sizeof(pattern) - strlen(pattern),
      "\\*");
   handle = FindFirstFileA(pattern, &entry);
   if (handle == INVALID_HANDLE_VALUE)
      return -1;
   do {
      if (strcmp(entry.cFileName, ".") == 0 ||
          strcmp(entry.cFileName, "..") == 0)
         continue;
      if (callback(callback_ud, entry.cFileName) != 0)
         break;
   } while (FindNextFileA(handle, &entry));
   FindClose(handle);
   return 0;
}

static int fs_read(void *userdata, const char *path, char **data, size_t *size)
{
   char full[MAX_PATH * 2];
   FILE *file;
   long length;
   char *buffer;

   (void)userdata;
   if (path_join(full, sizeof(full), path) != 0)
      return -1;
   file = fopen(full, "rb");
   if (!file)
      return -1;
   if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0) {
      fclose(file);
      return -1;
   }
   rewind(file);
   buffer = (char *)malloc((size_t)length + 1u);
   if (!buffer) {
      fclose(file);
      return -1;
   }
   if (length > 0 && fread(buffer, 1, (size_t)length, file) !=
       (size_t)length) {
      free(buffer);
      fclose(file);
      return -1;
   }
   buffer[length] = '\0';
   fclose(file);
   *data = buffer;
   if (size)
      *size = (size_t)length;
   return 0;
}

static int fs_write(void *userdata, const char *path, const char *data,
   size_t size)
{
   char full[MAX_PATH * 2];
   FILE *file;

   (void)userdata;
   if (path_join(full, sizeof(full), path) != 0)
      return -1;
   file = fopen(full, "wb");
   if (!file)
      return -1;
   if (size > 0 && fwrite(data, 1, size, file) != size) {
      fclose(file);
      return -1;
   }
   return fclose(file) == 0 ? 0 : -1;
}

static int fs_remove(void *userdata, const char *path)
{
   char full[MAX_PATH * 2];

   (void)userdata;
   if (path_join(full, sizeof(full), path) != 0)
      return -1;
   return DeleteFileA(full) ? 0 : -1;
}

static int fs_rename(void *userdata, const char *from, const char *to)
{
   char full_from[MAX_PATH * 2];
   char full_to[MAX_PATH * 2];

   (void)userdata;
   if (path_join(full_from, sizeof(full_from), from) != 0 ||
       path_join(full_to, sizeof(full_to), to) != 0)
      return -1;
   return MoveFileExA(full_from, full_to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

const ra_sync_fs_t ra_fs_win32 = {
   NULL, fs_exists, fs_list, fs_read, fs_write, fs_remove, fs_rename,
};
