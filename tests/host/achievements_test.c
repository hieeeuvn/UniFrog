/*
 * Host test for the offline RetroAchievements engine.
 *
 * The engine is compiled with UNIFROG_SD_ROOT pointing at a scratch directory
 * so the cache, progress, and queue files never touch a real SD card. A tiny
 * iNES file produces a real RetroAchievements hash; the cached "patch"
 * response defines one achievement that watches byte 0 of NES system RAM.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unifrog/achievements.h>
#include <unifrog/config.h>
#include <unifrog/log.h>
#include <unifrog/paths.h>

#include <rc_hash.h>

#include "test.h"

#define TEST_ROOT "/tmp/unifrog-achievements-test"
#define NES_CONSOLE 7

static uint8_t nes_ram[2048];
static uint32_t fake_clock_ms;

/* Minimal host stubs for the foundation services the engine touches. */

int unifrog_config_read(const char *path, unifrog_config_entry_cb callback,
   void *userdata, unsigned *error_count)
{
   FILE *file;
   char line[128];

   (void)callback;
   (void)userdata;
   if (error_count)
      *error_count = 0;
   file = fopen(path, "rb");
   if (!file)
      return -1;
   while (fgets(line, sizeof(line), file)) {
      /* The test never needs persisted settings. */
   }
   fclose(file);
   return 0;
}

int unifrog_log_at(enum unifrog_log_level level, const char *component,
   const char *format, ...)
{
   (void)level;
   (void)component;
   (void)format;
   return 0;
}

uint32_t unifrog_perf_time_ms(void)
{
   return fake_clock_ms;
}

void unifrog_gfx_fill_rect(const struct unifrog_surface *surface,
   int x, int y, int w, int h, uint16_t color)
{
   (void)surface;
   (void)x;
   (void)y;
   (void)w;
   (void)h;
   (void)color;
}

void unifrog_gfx_draw_text(const struct unifrog_surface *surface,
   int x, int y, const char *text, uint16_t color, int scale)
{
   (void)surface;
   (void)x;
   (void)y;
   (void)text;
   (void)color;
   (void)scale;
}

int unifrog_gfx_text_width_limit(const char *text, int max_px)
{
   int width = text ? (int)strlen(text) * 6 : 0;

   return width > max_px ? max_px : width;
}

/* Host memory callbacks mirror what the libretro frontend provides. */

static void *test_memory_data(unsigned id)
{
   /* libretro.h: RETRO_MEMORY_SYSTEM_RAM is 2, not 0. */
   return id == 2 ? nes_ram : NULL;
}

static size_t test_memory_size(unsigned id)
{
   return id == 2 ? sizeof(nes_ram) : 0;
}

static int write_file(const char *path, const void *data, size_t size)
{
   FILE *file = fopen(path, "wb");
   size_t written;

   if (!file)
      return -1;
   written = fwrite(data, 1, size, file);
   fclose(file);
   return written == size ? 0 : -1;
}

static int read_file(const char *path, char *buffer, size_t size)
{
   FILE *file = fopen(path, "rb");
   size_t length;

   if (!file)
      return -1;
   length = fread(buffer, 1, size - 1, file);
   fclose(file);
   buffer[length] = '\0';
   return 0;
}

static void prepare_directories(void)
{
   system("rm -rf " TEST_ROOT);
   mkdir(TEST_ROOT, 0777);
   mkdir(UNIFROG_DATA_ROOT, 0777);
   mkdir(UNIFROG_ACHIEVEMENTS_ROOT, 0777);
   mkdir(UNIFROG_ACHIEVEMENTS_CACHE_ROOT, 0777);
   mkdir(UNIFROG_ACHIEVEMENTS_PROGRESS_ROOT, 0777);
   mkdir(UNIFROG_ACHIEVEMENTS_QUEUE_ROOT, 0777);
}

int main(void)
{
   static const uint8_t rom[] = {
      'N', 'E', 'S', 0x1a, 1, 1, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0,
      0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
   };
   static const char cache[] =
      "{\"Success\":true,\"PatchData\":{"
      "\"ID\":4242,\"Title\":\"Test Game\",\"ConsoleID\":7,"
      "\"ImageIcon\":\"\",\"RichPresencePatch\":\"\","
      "\"Achievements\":[{"
      "\"ID\":111,\"Title\":\"First Byte\","
      "\"Description\":\"Read the first RAM byte\","
      "\"Flags\":3,\"Points\":5,\"MemAddr\":\"0xH0000=1\","
      "\"Author\":\"tester\",\"BadgeName\":\"00001\","
      "\"Created\":0,\"Modified\":0}],"
      "\"Leaderboards\":[]}}";
   char hash[33];
   char cache_path[160];
   char queue[8192];
   char progress_check[160];
   const char *rom_path = TEST_ROOT "/rom.nes";
   struct unifrog_achievements_summary summary;
   struct unifrog_achievements_entry entry;
   struct stat info;

   prepare_directories();
   TEST_EQ_INT(0, write_file(rom_path, rom, sizeof(rom)));

   TEST_EQ_INT(1, rc_hash_generate_from_file(hash, NES_CONSOLE, rom_path));
   TEST_EQ_INT(32, (int)strlen(hash));
   snprintf(cache_path, sizeof(cache_path), "%s/%s.json",
      UNIFROG_ACHIEVEMENTS_CACHE_ROOT, hash);
   TEST_EQ_INT(0, write_file(cache_path, cache, strlen(cache)));

   unifrog_achievements_set_memory_access(test_memory_data,
      test_memory_size);
   TEST_EQ_INT(0, unifrog_achievements_begin("quicknes", rom_path));

   unifrog_achievements_summary(&summary);
   TEST_EQ_INT(UNIFROG_ACHIEVEMENTS_READY, summary.state);
   TEST_EQ_INT(1, summary.total);
   TEST_EQ_INT(0, summary.unlocked);
   TEST_EQ_STR(hash, summary.hash);
   TEST_EQ_STR("Test Game", summary.game_title);
   TEST_EQ_INT(1, unifrog_achievements_count());
   TEST_EQ_INT(0, unifrog_achievements_entry(0, &entry));
   TEST_EQ_INT(111, entry.id);
   TEST_EQ_INT(5, entry.points);
   TEST_EQ_INT(0, entry.unlocked);

   /* Nothing matches while RAM byte 0 is zero. */
   nes_ram[0] = 0;
   unifrog_achievements_frame();
   unifrog_achievements_summary(&summary);
   TEST_EQ_INT(0, summary.unlocked);
   TEST_EQ_INT(1, unifrog_achievements_toast() == NULL);

   /* The trigger fires on the next frame, queues the unlock, and shows a toast. */
   nes_ram[0] = 1;
   unifrog_achievements_frame();
   unifrog_achievements_summary(&summary);
   TEST_EQ_INT(1, summary.unlocked);
   TEST_EQ_INT(5, summary.points_unlocked);
   TEST_EQ_INT(0, unifrog_achievements_entry(0, &entry));
   TEST_EQ_INT(1, entry.unlocked);
   TEST_EQ_STR("First Byte", entry.title);
   TEST_CHECK(unifrog_achievements_toast() != NULL);

   TEST_EQ_INT(0, read_file(UNIFROG_ACHIEVEMENTS_UNLOCK_PATH, queue,
      sizeof(queue)));
   TEST_CHECK(strstr(queue, "\"a\":111") != NULL);
   TEST_CHECK(strstr(queue, hash) != NULL);

   unifrog_achievements_end();
   snprintf(progress_check, sizeof(progress_check), "%s/%s.bin",
      UNIFROG_ACHIEVEMENTS_PROGRESS_ROOT, hash);
   TEST_EQ_INT(0, stat(progress_check, &info));
   TEST_CHECK(info.st_size > 0);

   /* Earned ids survive without relying on rcheevos progress. */
   snprintf(progress_check, sizeof(progress_check), "%s/%s.txt",
      UNIFROG_ACHIEVEMENTS_EARNED_ROOT, hash);
   TEST_EQ_INT(0, read_file(progress_check, queue, sizeof(queue)));
   TEST_CHECK(strstr(queue, "111") != NULL);

   /* Reloading the game keeps the award and does not queue it twice. */
   TEST_EQ_INT(0, unifrog_achievements_begin("quicknes", rom_path));
   unifrog_achievements_summary(&summary);
   TEST_EQ_INT(UNIFROG_ACHIEVEMENTS_READY, summary.state);
   TEST_EQ_INT(1, summary.unlocked);
   TEST_EQ_INT(1, (int)summary.pending_uploads);
   nes_ram[0] = 1;
   unifrog_achievements_frame();
   unifrog_achievements_summary(&summary);
   TEST_EQ_INT(1, summary.unlocked);
   TEST_EQ_INT(1, (int)summary.pending_uploads);
   unifrog_achievements_end();

   /*
    * A card fault at unlock time must not lose the award. The id is only
    * marked earned once its queue line is stored, and the line is retried.
    */
   {
      static const uint8_t rom2[] = {
         'N', 'E', 'S', 0x1a, 1, 1, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
      };
      static const char cache2[] =
         "{\"Success\":true,\"PatchData\":{"
         "\"ID\":4343,\"Title\":\"Second Game\",\"ConsoleID\":7,"
         "\"ImageIcon\":\"\",\"RichPresencePatch\":\"\","
         "\"Achievements\":[{"
         "\"ID\":222,\"Title\":\"Second Byte\","
         "\"Description\":\"Read the first RAM byte again\","
         "\"Flags\":3,\"Points\":10,\"MemAddr\":\"0xH0000=1\","
         "\"Author\":\"tester\",\"BadgeName\":\"00002\","
         "\"Created\":0,\"Modified\":0}],"
         "\"Leaderboards\":[]}}";
      char hash2[33];
      char path[160];

      TEST_EQ_INT(0, write_file(TEST_ROOT "/rom2.nes", rom2, sizeof(rom2)));
      TEST_EQ_INT(1, rc_hash_generate_from_file(hash2, NES_CONSOLE,
         TEST_ROOT "/rom2.nes"));
      snprintf(path, sizeof(path), "%s/%s.json",
         UNIFROG_ACHIEVEMENTS_CACHE_ROOT, hash2);
      TEST_EQ_INT(0, write_file(path, cache2, strlen(cache2)));

      /* Make the queue unwritable for everyone: a regular file, not a dir. */
      TEST_EQ_INT(0, system("rm -rf " UNIFROG_ACHIEVEMENTS_QUEUE_ROOT));
      TEST_EQ_INT(0, write_file(UNIFROG_ACHIEVEMENTS_QUEUE_ROOT, "x", 1));

      TEST_EQ_INT(0, unifrog_achievements_begin("quicknes",
         TEST_ROOT "/rom2.nes"));
      nes_ram[0] = 0;
      unifrog_achievements_frame();
      nes_ram[0] = 1;
      unifrog_achievements_frame();
      unifrog_achievements_summary(&summary);
      TEST_EQ_INT(1, summary.unlocked);
      snprintf(path, sizeof(path), "%s/%s.txt",
         UNIFROG_ACHIEVEMENTS_EARNED_ROOT, hash2);
      TEST_CHECK(access(path, F_OK) != 0);

      /* Card recovers: the retry stores the line before the earned id. */
      TEST_EQ_INT(0, remove(UNIFROG_ACHIEVEMENTS_QUEUE_ROOT));
      TEST_EQ_INT(0, mkdir(UNIFROG_ACHIEVEMENTS_QUEUE_ROOT, 0777));
      fake_clock_ms += 2100;
      unifrog_achievements_frame();
      TEST_EQ_INT(0, read_file(UNIFROG_ACHIEVEMENTS_UNLOCK_PATH, queue,
         sizeof(queue)));
      TEST_CHECK(strstr(queue, "\"a\":222") != NULL);
      TEST_EQ_INT(0, read_file(path, queue, sizeof(queue)));
      TEST_CHECK(strstr(queue, "222") != NULL);
      unifrog_achievements_end();
   }

   /* An unmapped core must not guess a hash or use a stale game. */
   TEST_EQ_INT(0, unifrog_achievements_begin("unknown-core", rom_path));
   unifrog_achievements_summary(&summary);
   TEST_EQ_INT(UNIFROG_ACHIEVEMENTS_OFF, summary.state);
   TEST_EQ_INT(0, unifrog_achievements_count());
   unifrog_achievements_end();

   system("rm -rf " TEST_ROOT);
   return test_finish("achievements");
}
