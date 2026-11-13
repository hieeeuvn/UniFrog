/*
 * UniFrog app paks.
 *
 * A pak is a folder under /unifrog_data/apps holding an app.ini manifest plus a
 * JS2300 script, so a new tool is one dropped folder instead of a firmware
 * rebuild. The frontend only reads the manifest and hands the entry script to
 * the existing script launcher, which keeps the pak path out of the UI code.
 *
 * See docs/app-paks.md for the format.
 */
#include "frontend_internal.h"

/* Entries come from a user-supplied manifest, so never leave the pak folder. */
static int pak_rel_path_ok(const char *rel)
{
   if (!rel || !rel[0] || rel[0] == '/')
      return 0;
   if (strstr(rel, "..") || strchr(rel, '\\') || strchr(rel, ':'))
      return 0;
   return 1;
}

static int pak_script_name(const char *name)
{
   if (!name)
      return 0;
   return unifrog_text_ends_with_ci(name, ".js") ||
      unifrog_text_ends_with_ci(name, ".mjs");
}

static void pak_remember_icon(struct frontend_state *fe, unsigned item,
   const char *pak_dir, const char *icon_name)
{
   char icon_path[FRONTEND_MAX_PATH];

   if (!fe || fe->pak_icon_count >= FRONTEND_APP_PAK_MAX)
      return;
   if (!pak_rel_path_ok(icon_name))
      return;
   if (frontend_path_join(icon_path, sizeof(icon_path), pak_dir, icon_name) != 0)
      return;
   if (!frontend_file_exists(icon_path))
      return;
   fe->pak_icon_item[fe->pak_icon_count] = item;
   unifrog_text_copy(fe->pak_icon_path[fe->pak_icon_count],
      sizeof(fe->pak_icon_path[0]), icon_path);
   fe->pak_icon_count++;
}

int frontend_paks_icon(const struct frontend_state *fe, unsigned item,
   char *out, size_t size)
{
   if (!fe || !out || size == 0)
      return -1;
   for (unsigned i = 0; i < fe->pak_icon_count; i++) {
      if (fe->pak_icon_item[i] != item)
         continue;
      unifrog_text_copy(out, size, fe->pak_icon_path[i]);
      return 0;
   }
   return -1;
}

static int pak_add(struct frontend_state *fe, const char *pak_name)
{
   char pak_dir[FRONTEND_MAX_PATH];
   char manifest[FRONTEND_MAX_PATH];
   char entry_name[FRONTEND_MAX_PATH];
   char entry_path[FRONTEND_MAX_PATH];
   char icon_name[FRONTEND_MAX_PATH];
   char title[64];
   char info[64];
   struct frontend_item *item;
   struct stat st;

   if (frontend_path_join(pak_dir, sizeof(pak_dir), FRONTEND_APP_ROOT,
       pak_name) != 0)
      return 0;
   if (stat(pak_dir, &st) != 0 || !S_ISDIR(st.st_mode))
      return 0;
   if (frontend_path_join(manifest, sizeof(manifest), pak_dir, "app.ini") != 0 ||
       !frontend_file_exists(manifest)) {
      unifrog_log("frontend app pak skipped name=%s reason=no_manifest\n",
         pak_name);
      return 0;
   }
   unifrog_text_copy(title, sizeof(title), pak_name);
   info[0] = '\0';
   entry_name[0] = '\0';
   icon_name[0] = '\0';
   (void)frontend_read_file_key(title, sizeof(title), manifest, "title");
   (void)frontend_read_file_key(info, sizeof(info), manifest, "info");
   (void)frontend_read_file_key(entry_name, sizeof(entry_name), manifest,
      "entry");
   (void)frontend_read_file_key(icon_name, sizeof(icon_name), manifest, "icon");
   if (!entry_name[0])
      unifrog_text_copy(entry_name, sizeof(entry_name), "main.js");
   if (!pak_rel_path_ok(entry_name) || !pak_script_name(entry_name) ||
       frontend_path_join(entry_path, sizeof(entry_path), pak_dir,
          entry_name) != 0 ||
       !frontend_file_exists(entry_path)) {
      unifrog_log("frontend app pak skipped name=%s entry=%s reason=bad_entry\n",
         pak_name, entry_name);
      return 0;
   }
   if (!title[0])
      unifrog_text_copy(title, sizeof(title), pak_name);
   item = add_item(fe, title, info[0] ? info : "app pak", FRONTEND_ITEM_APP,
      entry_path, NULL);
   if (!item)
      return 0;
   pak_remember_icon(fe, fe->item_count - 1u, pak_dir, icon_name);
   unifrog_log("frontend app pak name=%s title=%s entry=%s\n", pak_name, title,
      entry_name);
   return 1;
}

unsigned frontend_paks_load(struct frontend_state *fe)
{
   DIR *dir;
   struct dirent *entry;
   unsigned seen = 0;
   unsigned loaded = 0;
   uint32_t start_ms = unifrog_perf_time_ms();

   if (!fe)
      return 0;
   fe->pak_icon_count = 0;
   dir = unifrog_storage_opendir_resilient(FRONTEND_APP_ROOT, "app_paks", 4,
      120);
   if (!dir)
      return 0;
   while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] == '.')
         continue;
      if (fe->item_count + 2u >= FRONTEND_MAX_ITEMS)
         break;
      seen++;
      loaded += (unsigned)pak_add(fe, entry->d_name);
   }
   closedir(dir);
   unifrog_log("frontend app paks seen=%u loaded=%u ms=%lu\n", seen, loaded,
      (unsigned long)(unifrog_perf_time_ms() - start_ms));
   return loaded;
}
