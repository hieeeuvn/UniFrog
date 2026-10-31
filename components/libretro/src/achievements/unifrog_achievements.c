/*
 * Offline-first RetroAchievements engine for UniFrog.
 *
 * Definitions live on the SD card as the raw response of the RetroAchievements
 * "patch" endpoint. The engine identifies content with the RetroAchievements
 * hash, activates the cached achievements with rcheevos, evaluates them once
 * per emulated frame, keeps progress in a compact binary file, records earned
 * ids per game, and appends unlocks to a durable queue that tools/ra_sync.mjs
 * uploads later.
 */
#include <unifrog/achievements.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <unifrog/config.h>
#include <unifrog/log.h>
#include <unifrog/path.h>
#include <unifrog/paths.h>
#include <unifrog/perf.h>

#include <rc_api_runtime.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#include <rc_libretro.h>
#include <rc_runtime.h>
#include <rc_runtime_types.h>

#define ACHIEVEMENTS_CACHE_MAX (768u * 1024u)
#define ACHIEVEMENTS_PROGRESS_MAX (128u * 1024u)
#define ACHIEVEMENTS_CONTENT_PATH_MAX 256u
#define ACHIEVEMENTS_PATH_MAX 320u
#define ACHIEVEMENTS_CORE_ID_MAX 32u
#define ACHIEVEMENTS_TOAST_MAX 96u
#define ACHIEVEMENTS_PROGRESS_SAVE_MS 5000u
#define ACHIEVEMENTS_EARNED_MAX 512u
#define ACHIEVEMENTS_QUEUE_LINE_MAX 256u
#define ACHIEVEMENTS_PENDING_MAX 16u
#define ACHIEVEMENTS_PENDING_LINE_MAX 112u
#define ACHIEVEMENTS_QUEUE_RETRY_MS 2000u

struct achievements_pending_unlock {
   unsigned id;
   unsigned points;
   int reported;
   char line[ACHIEVEMENTS_PENDING_LINE_MAX];
};

struct achievements_state {
   int initialized;
   int active;
   int enabled;
   int hardcore;
   unsigned toast_ms;
   char username[UNIFROG_ACHIEVEMENTS_NAME_MAX];

   enum unifrog_achievements_state state;
   char status[UNIFROG_ACHIEVEMENTS_STATUS_MAX];
   char hash[UNIFROG_ACHIEVEMENTS_HASH_MAX];
   char title[UNIFROG_ACHIEVEMENTS_NAME_MAX];
   char core_id[ACHIEVEMENTS_CORE_ID_MAX];
   char content_path[ACHIEVEMENTS_CONTENT_PATH_MAX];
   char cache_path[ACHIEVEMENTS_PATH_MAX];
   char progress_path[ACHIEVEMENTS_PATH_MAX];
   char earned_path[ACHIEVEMENTS_PATH_MAX];
   char request_path[ACHIEVEMENTS_PATH_MAX];
   unsigned console_id;

   rc_runtime_t runtime;
   int runtime_ready;
   rc_api_fetch_game_data_response_t game;
   int game_loaded;
   rc_libretro_memory_regions_t regions;
   int regions_ready;
   unsigned unlock_count;
   int progress_dirty;
   uint32_t progress_saved_ms;
   unsigned earned[ACHIEVEMENTS_EARNED_MAX];
   unsigned earned_count;

   /*
    * Unlocks that are not on the card yet. The handheld can drop a write
    * while a core runs, so the queue line is kept here until it is stored.
    */
   struct achievements_pending_unlock pending[ACHIEVEMENTS_PENDING_MAX];
   unsigned pending_count;
   uint32_t queue_retry_ms;

   char toast[ACHIEVEMENTS_TOAST_MAX];
   uint32_t toast_until_ms;
};

static struct achievements_state achievements;

static void *(*achievements_memory_data)(unsigned id);
static size_t (*achievements_memory_size)(unsigned id);

void unifrog_achievements_set_memory_access(
   void *(*data)(unsigned id), size_t (*size)(unsigned id))
{
   achievements_memory_data = data;
   achievements_memory_size = size;
}

static void achievements_copy(char *dst, size_t size, const char *src)
{
   if (!dst || size == 0)
      return;
   if (!src)
      src = "";
   while (*src && size > 1) {
      *dst++ = *src++;
      size--;
   }
   *dst = '\0';
}

static void achievements_set_status(enum unifrog_achievements_state state,
   const char *status)
{
   achievements.state = state;
   achievements_copy(achievements.status, sizeof(achievements.status),
      status);
   UF_LOG_INFO("achievements", "event=state state=%d status=%s",
      (int)state, achievements.status);
}

static void achievements_config_write(void)
{
   char temporary[sizeof(UNIFROG_ACHIEVEMENTS_CONFIG_PATH) + 8];
   FILE *file;

   snprintf(temporary, sizeof(temporary), "%s.tmp",
      UNIFROG_ACHIEVEMENTS_CONFIG_PATH);
   file = fopen(temporary, "wb");
   if (!file)
      return;
   fprintf(file, "# RetroAchievements settings written by UniFrog.\n");
   fprintf(file, "# enabled: evaluate cached achievements during play.\n");
   fprintf(file, "# hardcore: reserved for future no-savestate enforcement.\n");
   fprintf(file, "# toast_ms: how long an unlock banner stays on screen.\n");
   fprintf(file, "[retroachievements]\n");
   fprintf(file, "enabled=%d\n", achievements.enabled ? 1 : 0);
   fprintf(file, "hardcore=%d\n", achievements.hardcore ? 1 : 0);
   fprintf(file, "toast_ms=%u\n", achievements.toast_ms);
   fprintf(file, "username=%s\n", achievements.username);
   fflush(file);
   fclose(file);
   if (rename(temporary, UNIFROG_ACHIEVEMENTS_CONFIG_PATH) != 0)
      remove(temporary);
}

static int achievements_config_entry(void *userdata, const char *section,
   const char *key, const char *value, unsigned line_number)
{
   (void)userdata;
   (void)line_number;
   if (!section || strcmp(section, "retroachievements") != 0 || !key || !value)
      return 0;
   if (strcmp(key, "enabled") == 0)
      achievements.enabled = atoi(value) != 0;
   else if (strcmp(key, "hardcore") == 0)
      achievements.hardcore = atoi(value) != 0;
   else if (strcmp(key, "toast_ms") == 0) {
      int toast_ms = atoi(value);
      if (toast_ms < 500)
         toast_ms = 500;
      if (toast_ms > 15000)
         toast_ms = 15000;
      achievements.toast_ms = (unsigned)toast_ms;
   } else if (strcmp(key, "username") == 0)
      achievements_copy(achievements.username, sizeof(achievements.username),
         value);
   return 0;
}

static void achievements_config_load(void)
{
   achievements.enabled = 1;
   achievements.hardcore = 0;
   achievements.toast_ms = 4000;
   achievements.username[0] = '\0';
   if (unifrog_config_read(UNIFROG_ACHIEVEMENTS_CONFIG_PATH,
       achievements_config_entry, NULL, NULL) != 0) {
      achievements_config_write();
      return;
   }
}

static void achievements_ensure_dirs(void)
{
   (void)mkdir(UNIFROG_ACHIEVEMENTS_ROOT, 0777);
   (void)mkdir(UNIFROG_ACHIEVEMENTS_QUEUE_ROOT, 0777);
   (void)mkdir(UNIFROG_ACHIEVEMENTS_CACHE_ROOT, 0777);
   (void)mkdir(UNIFROG_ACHIEVEMENTS_PROGRESS_ROOT, 0777);
   (void)mkdir(UNIFROG_ACHIEVEMENTS_EARNED_ROOT, 0777);
}

static int achievements_hash_valid(const char *hash)
{
   unsigned i;

   if (!hash)
      return 0;
   for (i = 0; i < 32; i++) {
      if (!isxdigit((unsigned char)hash[i]))
         return 0;
   }
   return hash[32] == '\0';
}

static void achievements_paths(void)
{
   snprintf(achievements.cache_path, sizeof(achievements.cache_path),
      "%s/%s.json", UNIFROG_ACHIEVEMENTS_CACHE_ROOT, achievements.hash);
   snprintf(achievements.progress_path, sizeof(achievements.progress_path),
      "%s/%s.bin", UNIFROG_ACHIEVEMENTS_PROGRESS_ROOT, achievements.hash);
   snprintf(achievements.earned_path, sizeof(achievements.earned_path),
      "%s/%s.txt", UNIFROG_ACHIEVEMENTS_EARNED_ROOT, achievements.hash);
   snprintf(achievements.request_path, sizeof(achievements.request_path),
      "%s/%s.req", UNIFROG_ACHIEVEMENTS_QUEUE_ROOT, achievements.hash);
}

/*
 * Console mapping for the cores UniFrog ships. The hash algorithm is console
 * specific, so an unknown core must not guess: a wrong console produces a
 * valid MD5 that RetroAchievements will never recognise.
 *
 * PicoDrive serves several Sega systems, so it is resolved from the content
 * extension instead of the core id.
 */
static unsigned achievements_console_for_core(const char *core_id,
   const char *content_path)
{
   static const struct {
      const char *core;
      unsigned console;
   } map[] = {
      { "fceumm", RC_CONSOLE_NINTENDO },
      { "quicknes", RC_CONSOLE_NINTENDO },
      { "snes9x2002", RC_CONSOLE_SUPER_NINTENDO },
      { "snes9x2005", RC_CONSOLE_SUPER_NINTENDO },
      { "snes9x", RC_CONSOLE_SUPER_NINTENDO },
      { "gambatte", RC_CONSOLE_GAMEBOY },
      { "gearboy", RC_CONSOLE_GAMEBOY },
      { "gpsp", RC_CONSOLE_GAMEBOY_ADVANCE },
      { "pce-fast", RC_CONSOLE_PC_ENGINE },
      { "pce_fast", RC_CONSOLE_PC_ENGINE },
      { "prosystem", RC_CONSOLE_ATARI_7800 },
      { "stella2014", RC_CONSOLE_ATARI_2600 },
      { "a5200", RC_CONSOLE_ATARI_5200 },
      { "handy", RC_CONSOLE_ATARI_LYNX },
      { "beetle-cygne", RC_CONSOLE_WONDERSWAN },
      { "race", RC_CONSOLE_NEOGEO_POCKET },
      { "gearcoleco", RC_CONSOLE_COLECOVISION },
      { "bluemsx", RC_CONSOLE_MSX },
      { "bluemsx-prosty", RC_CONSOLE_MSX },
      { "frodo", RC_CONSOLE_COMMODORE_64 },
      { "frodo-prosty", RC_CONSOLE_COMMODORE_64 },
      { "qpsx", RC_CONSOLE_PLAYSTATION },
   };
   const char *extension;
   unsigned i;

   if (!core_id)
      return 0;
   if (strcmp(core_id, "picodrive") == 0) {
      extension = content_path ? strrchr(content_path, '.') : NULL;
      if (extension) {
         if (strcasecmp(extension, ".sms") == 0)
            return RC_CONSOLE_MASTER_SYSTEM;
         if (strcasecmp(extension, ".gg") == 0)
            return RC_CONSOLE_GAME_GEAR;
         if (strcasecmp(extension, ".32x") == 0)
            return RC_CONSOLE_SEGA_32X;
         if (strcasecmp(extension, ".cue") == 0 ||
             strcasecmp(extension, ".chd") == 0)
            return RC_CONSOLE_SEGA_CD;
      }
      return RC_CONSOLE_MEGA_DRIVE;
   }
   for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
      if (strcmp(core_id, map[i].core) == 0)
         return map[i].console;
   }
   return 0;
}

static void achievements_write_request(void)
{
   FILE *file;

   if (!achievements_hash_valid(achievements.hash))
      return;
   if (access(achievements.cache_path, F_OK) == 0)
      return;
   file = fopen(achievements.request_path, "wb");
   if (!file)
      return;
   fprintf(file, "{\"hash\":\"%s\",\"console\":%u,\"content\":\"%s\"}\n",
      achievements.hash, achievements.console_id, achievements.content_path);
   fclose(file);
}

static int achievements_load_cache(void)
{
   FILE *file;
   long length;
   char *buffer;
   int ret;

   if (!achievements_hash_valid(achievements.hash))
      return -1;
   file = fopen(achievements.cache_path, "rb");
   if (!file) {
      achievements_write_request();
      achievements_set_status(UNIFROG_ACHIEVEMENTS_WAITING_CACHE,
         "No achievement data - run ra_sync");
      return -1;
   }
   if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
       (unsigned long)length > ACHIEVEMENTS_CACHE_MAX) {
      fclose(file);
      achievements_set_status(UNIFROG_ACHIEVEMENTS_ERROR,
         "Achievement cache is unreadable");
      return -1;
   }
   buffer = (char *)malloc((size_t)length + 1u);
   if (!buffer) {
      fclose(file);
      achievements_set_status(UNIFROG_ACHIEVEMENTS_ERROR,
         "Not enough memory for achievements");
      return -1;
   }
   rewind(file);
   if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
      fclose(file);
      free(buffer);
      achievements_set_status(UNIFROG_ACHIEVEMENTS_ERROR,
         "Achievement cache read failed");
      return -1;
   }
   fclose(file);
   buffer[length] = '\0';

   memset(&achievements.game, 0, sizeof(achievements.game));
   ret = rc_api_process_fetch_game_data_response(&achievements.game, buffer);
   free(buffer);
   if (ret != RC_OK) {
      rc_api_destroy_fetch_game_data_response(&achievements.game);
      memset(&achievements.game, 0, sizeof(achievements.game));
      achievements_set_status(UNIFROG_ACHIEVEMENTS_ERROR,
         "Achievement data rejected");
      return -1;
   }
   achievements.game_loaded = 1;
   achievements_copy(achievements.title, sizeof(achievements.title),
      achievements.game.title ? achievements.game.title : "Unknown game");
   return 0;
}

static void achievements_activate(void)
{
   unsigned i;

   rc_runtime_init(&achievements.runtime);
   achievements.runtime_ready = 1;
   for (i = 0; i < achievements.game.num_achievements; i++) {
      const rc_api_achievement_definition_t *definition =
         &achievements.game.achievements[i];
      if (definition->category != RC_ACHIEVEMENT_CATEGORY_CORE)
         continue;
      if (!definition->definition || !definition->definition[0])
         continue;
      (void)rc_runtime_activate_achievement(&achievements.runtime,
         definition->id, definition->definition, NULL, 0);
   }
}

static void achievements_load_progress(void)
{
   FILE *file;
   long length;
   uint8_t *buffer;

   if (!achievements.runtime_ready)
      return;
   file = fopen(achievements.progress_path, "rb");
   if (!file)
      return;
   if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
       (unsigned long)length > ACHIEVEMENTS_PROGRESS_MAX) {
      fclose(file);
      return;
   }
   buffer = (uint8_t *)malloc((size_t)length);
   if (!buffer) {
      fclose(file);
      return;
   }
   rewind(file);
   if (fread(buffer, 1, (size_t)length, file) == (size_t)length)
      (void)rc_runtime_deserialize_progress_sized(&achievements.runtime,
         buffer, (uint32_t)length, NULL);
   fclose(file);
   free(buffer);
}

/*
 * Earned achievements need their own record. rcheevos only serializes
 * *in-progress* triggers: once a trigger fires it is skipped by
 * rc_runtime_serialize_progress(), so without this list a reboot would re-fire
 * every achievement and queue it a second time. One decimal id per line, per
 * game, keeps the file tiny and trivial to append.
 */
static int achievements_is_earned(unsigned id)
{
   unsigned i;

   for (i = 0; i < achievements.earned_count; i++) {
      if (achievements.earned[i] == id)
         return 1;
   }
   return 0;
}

static void achievements_earned_add(unsigned id)
{
   if (id == 0 || achievements_is_earned(id) ||
       achievements.earned_count >= ACHIEVEMENTS_EARNED_MAX)
      return;
   achievements.earned[achievements.earned_count++] = id;
}

static void achievements_mark_earned(unsigned id)
{
   FILE *file;

   if (achievements_is_earned(id) ||
       achievements.earned_count >= ACHIEVEMENTS_EARNED_MAX)
      return;
   achievements_earned_add(id);
   file = fopen(achievements.earned_path, "ab");
   if (!file) {
      UF_LOG_WARN("achievements", "event=earned_write_failed id=%u", id);
      return;
   }
   fprintf(file, "%u\n", id);
   fclose(file);
}

/* Seed the earned list from unlocks queued by firmware that predates it. */
static void achievements_import_queue(void)
{
   FILE *file;
   char line[ACHIEVEMENTS_QUEUE_LINE_MAX];

   file = fopen(UNIFROG_ACHIEVEMENTS_UNLOCK_PATH, "rb");
   if (!file)
      return;
   while (fgets(line, sizeof(line), file)) {
      const char *entry = strstr(line, "\"a\":");
      const char *hash = strstr(line, "\"h\":\"");
      char *end;
      unsigned long value;

      if (!entry || !hash)
         continue;
      hash += 5;
      if (strncmp(hash, achievements.hash, 32) != 0)
         continue;
      value = strtoul(entry + 4, &end, 10);
      if (end != entry + 4 && value > 0 && value <= 0xFFFFFFFFul)
         achievements_earned_add((unsigned)value);
   }
   fclose(file);
}

static void achievements_load_earned(void)
{
   FILE *file;
   char line[32];
   unsigned i;

   achievements.earned_count = 0;
   if (!achievements_hash_valid(achievements.hash))
      return;
   file = fopen(achievements.earned_path, "rb");
   if (!file) {
      achievements_import_queue();
      if (achievements.earned_count == 0)
         return;
   } else {
      while (fgets(line, sizeof(line), file)) {
         char *end;
         unsigned long value = strtoul(line, &end, 10);
         if (end != line && value > 0 && value <= 0xFFFFFFFFul)
            achievements_earned_add((unsigned)value);
      }
      fclose(file);
      return;
   }

   /* Imported from the queue: write the list so the migration happens once. */
   file = fopen(achievements.earned_path, "wb");
   if (!file)
      return;
   for (i = 0; i < achievements.earned_count; i++)
      fprintf(file, "%u\n", achievements.earned[i]);
   fclose(file);
}

static void achievements_save_progress(void)
{
   uint32_t size;
   uint8_t *buffer;
   FILE *file;
   char temporary[sizeof(achievements.progress_path) + 8];

   if (!achievements.runtime_ready || !achievements.progress_dirty)
      return;
   size = rc_runtime_progress_size(&achievements.runtime, NULL);
   if (size == 0 || size > ACHIEVEMENTS_PROGRESS_MAX)
      return;
   buffer = (uint8_t *)malloc(size);
   if (!buffer)
      return;
   if (rc_runtime_serialize_progress_sized(buffer, size,
       &achievements.runtime, NULL) == RC_OK) {
      snprintf(temporary, sizeof(temporary), "%s.tmp",
         achievements.progress_path);
      file = fopen(temporary, "wb");
      if (file) {
         if (fwrite(buffer, 1, size, file) == size) {
            fflush(file);
            fclose(file);
            if (rename(temporary, achievements.progress_path) == 0)
               achievements.progress_dirty = 0;
            else
               remove(temporary);
         } else {
            fclose(file);
            remove(temporary);
         }
      }
   }
   free(buffer);
}

static int achievements_append_line(const char *path, const char *line)
{
   size_t length;
   FILE *file;

   if (!path || !line || !line[0])
      return -1;
   length = strlen(line);
   file = fopen(path, "ab");
   if (!file)
      return -1;
   if (fwrite(line, 1, length, file) != length || fflush(file) != 0) {
      fclose(file);
      return -1;
   }
   return fclose(file) == 0 ? 0 : -1;
}

static int achievements_pending_find(unsigned id)
{
   unsigned i;

   for (i = 0; i < achievements.pending_count; i++) {
      if (achievements.pending[i].id == id)
         return (int)i;
   }
   return -1;
}

static void achievements_flush_pending(void);

/*
 * Queue an award in memory. The line captures the game hash and unlock time
 * now so it stays valid if the retry happens after the next game starts.
 */
static void achievements_queue_unlock(unsigned id, unsigned points)
{
   struct achievements_pending_unlock *entry;
   time_t now;

   if (id == 0 || achievements_is_earned(id) ||
       achievements_pending_find(id) >= 0)
      return;
   if (achievements.pending_count >= ACHIEVEMENTS_PENDING_MAX) {
      /* The card may have recovered since the oldest line was queued. */
      achievements_flush_pending();
      if (achievements.pending_count >= ACHIEVEMENTS_PENDING_MAX) {
         UF_LOG_WARN("achievements", "event=unlock_queue_full id=%u", id);
         return;
      }
   }
   now = time(NULL);
   entry = &achievements.pending[achievements.pending_count++];
   entry->id = id;
   entry->points = points;
   entry->reported = 0;
   snprintf(entry->line, sizeof(entry->line),
      "{\"a\":%u,\"p\":%u,\"h\":\"%s\",\"m\":%d,\"t\":%ld}\n",
      id, points, achievements.hash, achievements.hardcore ? 1 : 0,
      (long)now);
}

/*
 * Store queued awards. An id is only recorded as earned after its queue line
 * is on the card: writing the earned list first would make a failed append
 * permanent, because later sessions would skip the trigger as a duplicate.
 */
static void achievements_flush_pending(void)
{
   unsigned i = 0;

   achievements.queue_retry_ms = unifrog_perf_time_ms();
   while (i < achievements.pending_count) {
      struct achievements_pending_unlock *entry = &achievements.pending[i];

      if (achievements_append_line(UNIFROG_ACHIEVEMENTS_UNLOCK_PATH,
          entry->line) != 0) {
         /* One diagnosis per award: retries every two seconds are silent. */
         if (!entry->reported) {
            entry->reported = 1;
            UF_LOG_WARN("achievements", "event=unlock_queue_failed id=%u",
               entry->id);
         }
         i++;
         continue;
      }
      achievements_mark_earned(entry->id);
      achievements.unlock_count++;
      UF_LOG_INFO("achievements", "event=unlock id=%u points=%u",
         entry->id, entry->points);
      achievements.pending_count--;
      if (i != achievements.pending_count) {
         achievements.pending[i] =
            achievements.pending[achievements.pending_count];
      }
   }
}

static void achievements_show_toast(const char *title, unsigned points)
{
   if (!title || !title[0])
      return;
   snprintf(achievements.toast, sizeof(achievements.toast),
      "%.64s  %uG", title, points);
   achievements.toast_until_ms = unifrog_perf_time_ms() + achievements.toast_ms;
}

static const rc_api_achievement_definition_t *achievements_definition(unsigned id)
{
   unsigned i;

   if (!achievements.game_loaded)
      return NULL;
   for (i = 0; i < achievements.game.num_achievements; i++) {
      if (achievements.game.achievements[i].id == id)
         return &achievements.game.achievements[i];
   }
   return NULL;
}

static void achievements_event(const rc_runtime_event_t *event)
{
   const rc_api_achievement_definition_t *definition;

   if (!event)
      return;
   switch (event->type) {
   case RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED:
      /* Already earned in an earlier session: the queue or the server has it,
       * so do not append a duplicate award. */
      if (achievements_is_earned(event->id))
         break;
      definition = achievements_definition(event->id);
      achievements.progress_dirty = 1;
      achievements_queue_unlock(event->id,
         definition ? definition->points : 0);
      /* The award is real even if the card is busy; show it immediately. */
      if (definition)
         achievements_show_toast(definition->title, definition->points);
      achievements_flush_pending();
      break;
   case RC_RUNTIME_EVENT_ACHIEVEMENT_PROGRESS_UPDATED:
      achievements.progress_dirty = 1;
      break;
   default:
      break;
   }
}

static void achievements_core_memory_info(uint32_t id,
   rc_libretro_core_memory_info_t *info);

/* The core's SET_MEMORY_MAPS layout, owned by the libretro host. */
static const struct retro_memory_map *achievements_memory_map;

void unifrog_achievements_set_memory_map(const struct retro_memory_map *map)
{
   achievements_memory_map = map;
}

static uint32_t achievements_peek(uint32_t address, uint32_t num_bytes,
   void *userdata)
{
   uint8_t buffer[4];
   uint32_t value = 0;
   uint32_t i;

   (void)userdata;
   if (num_bytes == 0)
      return 0;
   if (num_bytes > sizeof(buffer))
      num_bytes = sizeof(buffer);
   if (!achievements.regions_ready)
      return 0;
   if (rc_libretro_memory_read(&achievements.regions, address, buffer,
       num_bytes) != num_bytes)
      return 0;
   for (i = 0; i < num_bytes; i++)
      value |= (uint32_t)buffer[i] << (i * 8u);
   return value;
}

static int achievements_init_memory(void)
{
   unsigned index;
   size_t filler = 0;

   memset(&achievements.regions, 0, sizeof(achievements.regions));
   if (!achievements_memory_data || !achievements_memory_size)
      return -1;
   if (!rc_libretro_memory_init(&achievements.regions, achievements_memory_map,
       achievements_core_memory_info, achievements.console_id))
      return -1;
   for (index = 0; index < achievements.regions.count; index++) {
      if (!achievements.regions.data[index])
         filler += achievements.regions.size[index];
   }
   UF_LOG_INFO("achievements",
      "event=memory_regions console=%u mapped=%u regions=%u unmapped=%u",
      achievements.console_id,
      achievements_memory_map ? achievements_memory_map->num_descriptors : 0,
      achievements.regions.count, (unsigned)filler);
   /*
    * Nothing readable at the base of this console's address space means every
    * trigger sees zero, and conditions such as "value = 0" fire on their own.
    * That is how a whole achievement set unlocks at once, so refuse to track
    * instead of uploading awards the player never earned.
    */
   if (achievements.regions.count == 0 || !achievements.regions.data[0]) {
      UF_LOG_WARN("achievements", "event=memory_unmapped console=%u",
         achievements.console_id);
      achievements.regions_ready = 0;
      return -1;
   }
   achievements.regions_ready = 1;
   return 0;
}

static void achievements_core_memory_info(uint32_t id,
   rc_libretro_core_memory_info_t *info)
{
   if (!info)
      return;
   info->data = NULL;
   info->size = 0;
   if (!achievements_memory_data || !achievements_memory_size)
      return;
   /* rcheevos asks for the libretro memory ids (system/save/video RAM). */
   info->data = (uint8_t *)achievements_memory_data(id);
   info->size = achievements_memory_size(id);
}

static void achievements_clear_game(void)
{
   achievements.regions_ready = 0;
   memset(&achievements.regions, 0, sizeof(achievements.regions));
   if (achievements.runtime_ready) {
      rc_runtime_destroy(&achievements.runtime);
      achievements.runtime_ready = 0;
   }
   if (achievements.game_loaded) {
      rc_api_destroy_fetch_game_data_response(&achievements.game);
      achievements.game_loaded = 0;
   }
   achievements.active = 0;
   achievements.unlock_count = 0;
   achievements.progress_dirty = 0;
   achievements.toast[0] = '\0';
   achievements.toast_until_ms = 0;
}

int unifrog_achievements_begin(const char *core_id, const char *content_path)
{
   if (!achievements.initialized) {
      memset(&achievements, 0, sizeof(achievements));
      achievements.initialized = 1;
      achievements_config_load();
      achievements_ensure_dirs();
   }
   achievements_clear_game();
   achievements_copy(achievements.core_id, sizeof(achievements.core_id),
      core_id);
   achievements_copy(achievements.content_path,
      sizeof(achievements.content_path), content_path);
   achievements.hash[0] = '\0';
   achievements.title[0] = '\0';
   achievements.cache_path[0] = '\0';
   achievements.progress_path[0] = '\0';
   achievements.earned_path[0] = '\0';
   achievements.request_path[0] = '\0';
   achievements.console_id = 0;

   if (!achievements.enabled) {
      achievements.state = UNIFROG_ACHIEVEMENTS_OFF;
      achievements.status[0] = '\0';
      return 0;
   }
   achievements.console_id = achievements_console_for_core(core_id,
      content_path);
   if (achievements.console_id == 0) {
      achievements_set_status(UNIFROG_ACHIEVEMENTS_OFF,
         "No achievements for this core");
      return 0;
   }
   if (!content_path || !content_path[0]) {
      achievements_set_status(UNIFROG_ACHIEVEMENTS_WAITING_HASH,
         "No content to identify");
      return 0;
   }
   if (!rc_hash_generate_from_file(achievements.hash, achievements.console_id,
       content_path)) {
      achievements_set_status(UNIFROG_ACHIEVEMENTS_WAITING_HASH,
         "Content hash failed");
      return 0;
   }
   achievements_paths();
   achievements_set_status(UNIFROG_ACHIEVEMENTS_WAITING_HASH, "Identifying");
   if (achievements_load_cache() != 0)
      return 0;
   achievements_activate();
   achievements_load_progress();
   achievements_load_earned();
   if (achievements_init_memory() != 0) {
      achievements_set_status(UNIFROG_ACHIEVEMENTS_ERROR,
         "Core memory not available");
      achievements_clear_game();
      return 0;
   }
   achievements.active = 1;
   achievements.progress_saved_ms = unifrog_perf_time_ms();
   achievements_set_status(UNIFROG_ACHIEVEMENTS_READY, "Tracking");
   UF_LOG_INFO("achievements",
      "event=begin core=%s console=%u hash=%s achievements=%u",
      achievements.core_id, achievements.console_id, achievements.hash,
      achievements.game.num_achievements);
   return 0;
}

void unifrog_achievements_end(void)
{
   if (!achievements.initialized)
      return;
   achievements_flush_pending();
   achievements_save_progress();
   achievements_clear_game();
   achievements.state = UNIFROG_ACHIEVEMENTS_OFF;
}

void unifrog_achievements_frame(void)
{
   uint32_t now_ms;

   /*
    * Retry queued awards at a low rate. This keeps a transient SD write
    * failure from losing an unlock without touching the card every frame.
    */
   now_ms = unifrog_perf_time_ms();
   if (achievements.pending_count &&
       (uint32_t)(now_ms - achievements.queue_retry_ms) >=
       ACHIEVEMENTS_QUEUE_RETRY_MS)
      achievements_flush_pending();
   if (!achievements.active || !achievements.runtime_ready)
      return;
   rc_runtime_do_frame(&achievements.runtime, achievements_event,
      achievements_peek, NULL, NULL);
   if (!achievements.progress_dirty)
      return;
   /*
    * A handheld can be switched off at any time, so do not wait for a clean
    * teardown to persist progress. Five seconds bounds the loss to a handful
    * of frames while keeping SD writes rare.
    */
   now_ms = unifrog_perf_time_ms();
   if ((uint32_t)(now_ms - achievements.progress_saved_ms) >=
       ACHIEVEMENTS_PROGRESS_SAVE_MS) {
      achievements_save_progress();
      achievements.progress_saved_ms = now_ms;
   }
}

int unifrog_achievements_draw_overlay(struct unifrog_surface *surface)
{
   uint32_t now_ms;
   int width;
   int y;
   int x;

   if (!surface || !surface->pixels || !achievements.toast[0])
      return 0;
   now_ms = unifrog_perf_time_ms();
   if ((int32_t)(achievements.toast_until_ms - now_ms) <= 0) {
      achievements.toast[0] = '\0';
      return 0;
   }
   width = unifrog_gfx_text_width_limit(achievements.toast, 292);
   if (width < 0)
      width = 292;
   x = ((int)surface->width - (width + 24)) / 2;
   if (x < 4)
      x = 4;
   y = 8;
   unifrog_gfx_fill_rect(surface, x, y, width + 24, 38,
      UNIFROG_RGB565(20, 32, 28));
   unifrog_gfx_fill_rect(surface, x, y, 4, 38,
      UNIFROG_RGB565(120, 214, 189));
   unifrog_gfx_draw_text(surface, x + 12, y + 8,
      "ACHIEVEMENT UNLOCKED", UNIFROG_RGB565(120, 214, 189), 1);
   unifrog_gfx_draw_text(surface, x + 12, y + 22,
      achievements.toast, UNIFROG_RGB565(240, 250, 245), 1);
   return 1;
}

void unifrog_achievements_summary(struct unifrog_achievements_summary *summary)
{
   unsigned i;
   unsigned pending = 0;
   FILE *file;

   if (!summary)
      return;
   memset(summary, 0, sizeof(*summary));
   if (!achievements.initialized) {
      summary->state = UNIFROG_ACHIEVEMENTS_OFF;
      return;
   }
   summary->state = achievements.enabled ? achievements.state :
      UNIFROG_ACHIEVEMENTS_OFF;
   summary->hardcore = achievements.hardcore;
   achievements_copy(summary->game_title, sizeof(summary->game_title),
      achievements.title);
   achievements_copy(summary->hash, sizeof(summary->hash), achievements.hash);
   achievements_copy(summary->status, sizeof(summary->status),
      achievements.status);

   if (achievements.game_loaded) {
      const rc_trigger_t *trigger;

      for (i = 0; i < achievements.game.num_achievements; i++) {
         const rc_api_achievement_definition_t *definition =
            &achievements.game.achievements[i];
         if (definition->category != RC_ACHIEVEMENT_CATEGORY_CORE)
            continue;
         summary->total++;
         summary->points_total += definition->points;
         trigger = achievements.runtime_ready ?
            rc_runtime_get_achievement(&achievements.runtime,
               definition->id) : NULL;
         if (trigger && trigger->state == RC_TRIGGER_STATE_TRIGGERED) {
            summary->unlocked++;
            summary->points_unlocked += definition->points;
         } else if (achievements_is_earned(definition->id)) {
            summary->unlocked++;
            summary->points_unlocked += definition->points;
         }
      }
   }
   file = fopen(UNIFROG_ACHIEVEMENTS_UNLOCK_PATH, "rb");
   if (file) {
      int ch;
      while ((ch = fgetc(file)) != EOF) {
         if (ch == '\n')
            pending++;
      }
      fclose(file);
   }
   summary->pending_uploads = pending;
}

unsigned unifrog_achievements_count(void)
{
   unsigned i;
   unsigned count = 0;

   if (!achievements.game_loaded)
      return 0;
   for (i = 0; i < achievements.game.num_achievements; i++) {
      if (achievements.game.achievements[i].category ==
          RC_ACHIEVEMENT_CATEGORY_CORE)
         count++;
   }
   return count;
}

int unifrog_achievements_entry(unsigned index,
   struct unifrog_achievements_entry *entry)
{
   unsigned i;
   unsigned visible = 0;

   if (!entry || !achievements.game_loaded)
      return -1;
   for (i = 0; i < achievements.game.num_achievements; i++) {
      const rc_api_achievement_definition_t *definition =
         &achievements.game.achievements[i];
      const rc_trigger_t *trigger;
      unsigned measured = 0;
      unsigned target = 0;

      if (definition->category != RC_ACHIEVEMENT_CATEGORY_CORE)
         continue;
      if (visible++ != index)
         continue;
      memset(entry, 0, sizeof(*entry));
      entry->id = definition->id;
      entry->points = definition->points;
      entry->progress_percent = -1;
      achievements_copy(entry->title, sizeof(entry->title), definition->title);
      achievements_copy(entry->description, sizeof(entry->description),
         definition->description);
      if (achievements.runtime_ready) {
         trigger = rc_runtime_get_achievement(&achievements.runtime,
            definition->id);
         if (trigger && trigger->state == RC_TRIGGER_STATE_TRIGGERED)
            entry->unlocked = 1;
         else if (achievements_is_earned(definition->id))
            entry->unlocked = 1;
         if (rc_runtime_get_achievement_measured(&achievements.runtime,
             definition->id, &measured, &target) && target > 0 &&
             target <= 1000000u) {
            unsigned percent = (unsigned)(((uint64_t)measured * 100u) / target);
            entry->progress_percent = percent > 100u ? 100 : (int)percent;
         }
      }
      return 0;
   }
   return -1;
}

const char *unifrog_achievements_toast(void)
{
   return achievements.toast[0] ? achievements.toast : NULL;
}

int unifrog_achievements_enabled(void)
{
   if (!achievements.initialized) {
      memset(&achievements, 0, sizeof(achievements));
      achievements.initialized = 1;
      achievements_config_load();
   }
   return achievements.enabled;
}

int unifrog_achievements_set_enabled(int enabled)
{
   (void)unifrog_achievements_enabled();
   achievements.enabled = enabled ? 1 : 0;
   achievements_config_write();
   if (!achievements.enabled) {
      achievements_save_progress();
      achievements_clear_game();
      achievements.state = UNIFROG_ACHIEVEMENTS_OFF;
   } else if (achievements.state == UNIFROG_ACHIEVEMENTS_OFF &&
       achievements.content_path[0]) {
      (void)unifrog_achievements_begin(achievements.core_id,
         achievements.content_path);
   }
   return achievements.enabled;
}

int unifrog_achievements_hardcore(void)
{
   (void)unifrog_achievements_enabled();
   return achievements.hardcore;
}

int unifrog_achievements_set_hardcore(int hardcore)
{
   (void)unifrog_achievements_enabled();
   achievements.hardcore = hardcore ? 1 : 0;
   achievements_config_write();
   return achievements.hardcore;
}

void unifrog_achievements_refresh(void)
{
   /* Summary and entry views are computed on demand; nothing to cache. */
}
