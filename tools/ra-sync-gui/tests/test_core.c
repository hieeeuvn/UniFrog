/*
 * Host test for the RA sync core.
 *
 * The transport is faked: every request is inspected as a form body and the
 * response is selected from a small table. The filesystem is the real one
 * under /tmp so pull/push file handling (cache write, request removal, queue
 * rewrite) is exercised end to end.
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../core/ra_md5.h"

#include "../core/ra_sync_core.h"

static char *dup_text(const char *text)
{
   size_t size = strlen(text) + 1u;
   char *copy = (char *)malloc(size);

   if (copy)
      memcpy(copy, text, size);
   return copy;
}

#define TEST_ROOT "/tmp/unifrog-ra-sync-gui-test"
#define MAX_FORM 2048

static int failures;

#define CHECK(condition)                                                     \
   do {                                                                      \
      if (!(condition)) {                                                    \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,    \
            #condition);                                                     \
         failures++;                                                         \
      }                                                                      \
   } while (0)

#define CHECK_INT(expected, actual)                                          \
   do {                                                                      \
      long long expected_value = (long long)(expected);                      \
      long long actual_value = (long long)(actual);                          \
      if (expected_value != actual_value) {                                  \
         fprintf(stderr, "%s:%d: expected %lld, got %lld\n", __FILE__,       \
            __LINE__, expected_value, actual_value);                         \
         failures++;                                                         \
      }                                                                      \
   } while (0)

#define CHECK_STR(expected, actual)                                          \
   do {                                                                      \
      const char *expected_text = (expected);                                \
      const char *actual_text = (actual);                                    \
      if (!actual_text || strcmp(expected_text, actual_text) != 0) {         \
         fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__,   \
            __LINE__, expected_text, actual_text ? actual_text : "(null)");  \
         failures++;                                                         \
      }                                                                      \
   } while (0)

/* ------------------------------------------------------------------ */
/* Fake HTTP                                                          */
/* ------------------------------------------------------------------ */

static char last_forms[16][MAX_FORM];
static int last_form_count;
static const char *login_response = "{\"Success\":true}";

static struct award_reply {
   unsigned id;
   const char *body;
} award_replies[] = {
   { 111, "{\"Success\":true}" },
   { 222, "{\"Success\":false,\"Error\":\"Already unlocked\"}" },
   { 333, "{\"Success\":false,\"Error\":\"Invalid signature\"}" },
};

static const char *cached_hash = "0123456789abcdef0123456789abcdef";

static char *form_value(const char *form, const char *key)
{
   static char value[128];
   char pattern[32];
   const char *found;
   const char *end;
   size_t length;

   value[0] = '\0';
   snprintf(pattern, sizeof(pattern), "&%s=", key);
   found = strstr(form, pattern);
   if (!found)
      return value;
   found += strlen(pattern);
   end = strchr(found, '&');
   length = end ? (size_t)(end - found) : strlen(found);
   if (length >= sizeof(value))
      length = sizeof(value) - 1;
   memcpy(value, found, length);
   value[length] = '\0';
   return value;
}

static int form_has(const char *form, const char *needle)
{
   return strstr(form, needle) != NULL;
}

static int fake_post(void *userdata, const char *body, char **response,
   char *error, size_t error_size)
{
   (void)userdata;
   (void)error;
   (void)error_size;

   if (last_form_count < (int)(sizeof(last_forms) / sizeof(last_forms[0])))
      snprintf(last_forms[last_form_count++], MAX_FORM, "%s", body);

   if (strncmp(body, "r=login2", 8) == 0) {
      *response = dup_text(login_response);
      return *response ? 0 : -1;
   }
   if (strncmp(body, "r=patch", 7) == 0) {
      char *hash = form_value(body, "m");

      if (strcmp(hash, cached_hash) == 0) {
         *response = dup_text("{\"Success\":true,\"PatchData\":{\"ID\":4242,"
            "\"Title\":\"Test Game\",\"ConsoleID\":7,\"Achievements\":[]}}");
      } else {
         *response = dup_text("{\"Success\":false,\"Error\":\"Unknown game\"}");
      }
      return *response ? 0 : -1;
   }
   if (strncmp(body, "r=awardachievement", 18) == 0) {
      char *id_text = form_value(body, "a");
      unsigned id = (unsigned)strtoul(id_text, NULL, 10);
      unsigned index;

      for (index = 0; index < sizeof(award_replies) /
            sizeof(award_replies[0]); index++) {
         if (award_replies[index].id == id) {
            *response = dup_text(award_replies[index].body);
            return *response ? 0 : -1;
         }
      }
      *response = dup_text("{\"Success\":false,\"Error\":\"No award\"}");
      return *response ? 0 : -1;
   }
   *response = dup_text("{\"Success\":false,\"Error\":\"unexpected request\"}");
   return *response ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* POSIX filesystem under TEST_ROOT                                   */
/* ------------------------------------------------------------------ */

static void make_dir(const char *path)
{
   (void)mkdir(path, 0777);
}

static int fs_join(char *out, size_t size, const char *path)
{
   int written = snprintf(out, size, "%s/%s", TEST_ROOT, path);

   return written > 0 && (size_t)written < size ? 0 : -1;
}

static int fs_exists(void *userdata, const char *path)
{
   char full[512];
   struct stat info;

   (void)userdata;
   return fs_join(full, sizeof(full), path) == 0 &&
      stat(full, &info) == 0;
}

static int fs_list(void *userdata, const char *dir,
   int (*callback)(void *ud, const char *name), void *callback_ud)
{
   char full[512];
   DIR *handle;
   struct dirent *entry;

   (void)userdata;
   if (fs_join(full, sizeof(full), dir) != 0)
      return -1;
   handle = opendir(full);
   if (!handle)
      return -1;
   while ((entry = readdir(handle)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         continue;
      if (callback && callback(callback_ud, entry->d_name) != 0)
         break;
   }
   closedir(handle);
   return 0;
}

static int fs_read(void *userdata, const char *path, char **data, size_t *size)
{
   char full[512];
   FILE *file;
   long length;
   char *buffer;

   (void)userdata;
   if (fs_join(full, sizeof(full), path) != 0)
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
   if (length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
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
   char full[512];
   FILE *file;

   (void)userdata;
   if (fs_join(full, sizeof(full), path) != 0)
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
   char full[512];

   (void)userdata;
   if (fs_join(full, sizeof(full), path) != 0)
      return -1;
   return remove(full) == 0 ? 0 : -1;
}

static int fs_rename(void *userdata, const char *from, const char *to)
{
   char full_from[512];
   char full_to[512];

   (void)userdata;
   if (fs_join(full_from, sizeof(full_from), from) != 0 ||
       fs_join(full_to, sizeof(full_to), to) != 0)
      return -1;
   return rename(full_from, full_to) == 0 ? 0 : -1;
}

static const ra_sync_fs_t test_fs = {
   NULL, fs_exists, fs_list, fs_read, fs_write, fs_remove, fs_rename,
};

static char log_buffer[8192];

static void test_log(void *userdata, const char *line)
{
   size_t used = strlen(log_buffer);
   size_t room = sizeof(log_buffer) - used;

   (void)userdata;
   if (room > 2) {
      snprintf(log_buffer + used, room, "%s\n", line);
   }
}

static int file_text(const char *path, char *out, size_t size)
{
   char full[512];
   FILE *file;
   size_t length;

   if (fs_join(full, sizeof(full), path) != 0)
      return -1;
   file = fopen(full, "rb");
   if (!file)
      return -1;
   length = fread(out, 1, size - 1, file);
   fclose(file);
   out[length] = '\0';
   return 0;
}

static void write_text(const char *path, const char *text)
{
   CHECK_INT(0, fs_write(NULL, path, text, strlen(text)));
}

static void reset_root(void)
{
   char command[128];

   snprintf(command, sizeof(command), "rm -rf %s", TEST_ROOT);
   (void)system(command);
   make_dir(TEST_ROOT);
   make_dir(TEST_ROOT "/queue");
   make_dir(TEST_ROOT "/cache");
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_signature_and_md5(void)
{
   char digest[33];
   char signature[33];

   memset(digest, 0, sizeof(digest));
   ra_md5_hex("", 0, digest);
   CHECK_STR("d41d8cd98f00b204e9800998ecf8427e", digest);
   ra_md5_hex("abc", 3, digest);
   CHECK_STR("900150983cd24fb0d6963f7d28e17f72", digest);
   ra_md5_hex("The quick brown fox jumps over the lazy dog", 43, digest);
   CHECK_STR("9e107d9d372bb6826bd81d3542a419d6", digest);

   /*
    * md5("111Tester0") and md5("111Tester011195"), the exact inputs the RA
    * award endpoint expects. See tools/ra-sync-gui/README.md.
    */
   ra_sync_signature(signature, 111, "Tester", 0, 0);
   CHECK_STR("44b31ac21be11b450f1077e5ea3cc832", signature);
   ra_sync_signature(signature, 111, "Tester", 0, 95);
   CHECK_STR("d6b5ee9c1e37b74fcc46b93723ca21a3", signature);
}

static void test_missing_credentials(void)
{
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http = { NULL, fake_post };

   memset(&options, 0, sizeof(options));
   memset(&stats, 0, sizeof(stats));
   options.user = "";
   options.key = "";
   options.pull = 1;
   CHECK_INT(-1, ra_sync_run(&http, &test_fs, &options, test_log, NULL,
      &stats));
   CHECK(strstr(stats.error, "Missing credentials") != NULL);
}

static void test_login_failure(void)
{
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http = { NULL, fake_post };

   login_response = "{\"Success\":false,\"Error\":\"Invalid API key\"}";
   memset(&options, 0, sizeof(options));
   options.user = "Tester";
   options.key = "bad";
   options.pull = 1;
   CHECK_INT(-1, ra_sync_run(&http, &test_fs, &options, test_log, NULL,
      &stats));
   CHECK(strstr(stats.error, "Login failed: Invalid API key") != NULL);
   login_response = "{\"Success\":true}";
}

static void test_pull(void)
{
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http = { NULL, fake_post };
   char text[1024];

   reset_root();
   write_text("queue/" "0123456789abcdef0123456789abcdef.req",
      "{\"hash\":\"0123456789abcdef0123456789abcdef\",\"console\":7,\"content\":\"/ROMS/game.nes\"}");
   write_text("queue/" "fedcba9876543210fedcba9876543210.req",
      "{\"hash\":\"fedcba9876543210fedcba9876543210\",\"console\":7,\"content\":\"/ROMS/unknown.nes\"}");

   memset(&options, 0, sizeof(options));
   memset(log_buffer, 0, sizeof(log_buffer));
   options.user = "Tester";
   options.key = "key";
   options.pull = 1;
   CHECK_INT(0, ra_sync_run(&http, &test_fs, &options, test_log, NULL,
      &stats));
   CHECK_INT(1, stats.cached);
   CHECK_INT(1, stats.unknown);
   CHECK_INT(0, fs_exists(NULL, "queue/0123456789abcdef0123456789abcdef.req"));
   CHECK(fs_exists(NULL, "queue/fedcba9876543210fedcba9876543210.req"));
   CHECK(fs_exists(NULL, "cache/0123456789abcdef0123456789abcdef.json"));
   CHECK_INT(0, file_text("cache/0123456789abcdef0123456789abcdef.json",
      text, sizeof(text)));
   CHECK(strstr(text, "\"Title\":\"Test Game\"") != NULL);
   CHECK(strstr(log_buffer, "requested by /ROMS/game.nes") != NULL);
   CHECK(strstr(log_buffer, "0123456789abcdef0123456789abcdef: cached") != NULL);
   CHECK(strstr(log_buffer, "fedcba9876543210fedcba9876543210: Unknown game") !=
      NULL);
}

static void test_push(void)
{
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http = { NULL, fake_post };
   char text[2048];
   char expected[33];
   long long now;

   reset_root();
   now = (long long)time(NULL);
   snprintf(text, sizeof(text),
      "{\"a\":111,\"p\":5,\"h\":\"" "0123456789abcdef0123456789abcdef"
      "\",\"m\":0,\"t\":%lld}\n"
      "{\"a\":222,\"p\":10,\"h\":\"" "0123456789abcdef0123456789abcdef"
      "\",\"m\":1,\"t\":%lld}\n"
      "{\"a\":333,\"p\":15,\"h\":\"" "0123456789abcdef0123456789abcdef"
      "\",\"m\":0,\"t\":%lld}\n"
      "{\"a\":111,\"p\":5,\"h\":\"" "0123456789abcdef0123456789abcdef"
      "\",\"m\":0,\"t\":%lld}\n"
      "this is not json\n",
      now - 95, now - 60, now - 30, now - 95);
   write_text("queue/unlocks.jsonl", text);

   memset(&options, 0, sizeof(options));
   memset(log_buffer, 0, sizeof(log_buffer));
   options.user = "Tester";
   options.key = "key";
   options.push = 1;
   last_form_count = 0;
   CHECK_INT(0, ra_sync_run(&http, &test_fs, &options, test_log, NULL,
      &stats));
   CHECK_INT(2, stats.awarded);
   CHECK_INT(1, stats.failed);
   CHECK_INT(1, stats.pending);

   CHECK_INT(0, file_text("queue/unlocks.jsonl", text, sizeof(text)));
   CHECK(strstr(text, "\"a\":333") != NULL);
   CHECK(strstr(text, "\"a\":111") == NULL);
   CHECK(strstr(text, "\"a\":222") == NULL);
   CHECK(strstr(log_buffer, "Dropping duplicate unlock 111") != NULL);
   CHECK(strstr(log_buffer, "Dropping malformed unlock line 5") != NULL);
   CHECK(strstr(log_buffer, "achievement 222: awarded (already unlocked)") !=
      NULL);

   /* Requests: login, award 111, award 222, award 333. */
   CHECK_INT(4, last_form_count);
   {
      const char *form = NULL;
      int index;

      for (index = 0; index < last_form_count; index++) {
         if (form_has(last_forms[index], "&a=111"))
            form = last_forms[index];
      }
      CHECK(form != NULL);
      if (form) {
         char seconds[32];
         long long parsed;

         CHECK(form_has(form, "&h=0"));
         CHECK(form_has(form, "&m=0123456789abcdef0123456789abcdef"));
         CHECK(form_has(form, "&o="));
         snprintf(seconds, sizeof(seconds), "%s", form_value(form, "o"));
         parsed = strtoll(seconds, NULL, 10);
         CHECK(parsed >= 90 && parsed <= 120);
         ra_sync_signature(expected, 111, "Tester", 0, parsed);
         CHECK(form_has(form, expected));
      }
   }
   {
      const char *form = NULL;
      int index;

      for (index = 0; index < last_form_count; index++) {
         if (form_has(last_forms[index], "&a=222"))
            form = last_forms[index];
      }
      CHECK(form != NULL);
      if (form) {
         char seconds[32];
         long long parsed;

         CHECK(form_has(form, "&h=1"));
         snprintf(seconds, sizeof(seconds), "%s", form_value(form, "o"));
         parsed = strtoll(seconds, NULL, 10);
         ra_sync_signature(expected, 222, "Tester", 1, parsed);
         CHECK(form_has(form, expected));
      }
   }

   /* A fully awarded queue removes the file. */
   write_text("queue/unlocks.jsonl",
      "{\"a\":111,\"p\":5,\"h\":\"0123456789abcdef0123456789abcdef\","
      "\"m\":0,\"t\":0}\n");
   memset(&stats, 0, sizeof(stats));
   options.push = 1;
   last_form_count = 0;
   CHECK_INT(0, ra_sync_run(&http, &test_fs, &options, test_log, NULL,
      &stats));
   CHECK_INT(1, stats.awarded);
   CHECK_INT(0, stats.pending);
   CHECK_INT(0, fs_exists(NULL, "queue/unlocks.jsonl"));
}

static void test_rewrite_without_rename(void)
{
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http = { NULL, fake_post };
   ra_sync_fs_t fs = test_fs;
   char text[1024];

   reset_root();
   write_text("queue/unlocks.jsonl",
      "{\"a\":333,\"p\":15,\"h\":\"0123456789abcdef0123456789abcdef\","
      "\"m\":0,\"t\":0}\n");

   fs.rename = NULL;
   memset(&options, 0, sizeof(options));
   options.user = "Tester";
   options.key = "key";
   options.push = 1;
   CHECK_INT(0, ra_sync_run(&http, &fs, &options, test_log, NULL, &stats));
   CHECK_INT(1, stats.failed);
   CHECK_INT(0, file_text("queue/unlocks.jsonl", text, sizeof(text)));
   CHECK(strstr(text, "\"a\":333") != NULL);
}

static void test_list(void)
{
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http = { NULL, fake_post };

   reset_root();
   write_text("cache/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.json", "{}");
   write_text("cache/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.json", "{}");
   write_text("queue/cccccccccccccccccccccccccccccccc.req", "{}");
   write_text("queue/unlocks.jsonl",
      "{\"a\":1}\n{\"a\":2}\n{\"a\":3}");

   memset(&options, 0, sizeof(options));
   memset(log_buffer, 0, sizeof(log_buffer));
   options.list = 1;
   CHECK_INT(0, ra_sync_run(&http, &test_fs, &options, test_log, NULL,
      &stats));
   CHECK(strstr(log_buffer, "Cached games: 2") != NULL);
   CHECK(strstr(log_buffer, "Pending requests: 1") != NULL);
   CHECK(strstr(log_buffer, "Queued unlocks: 3") != NULL);
   CHECK_INT(3, stats.pending);
}

int main(void)
{
   test_signature_and_md5();
   test_missing_credentials();
   test_login_failure();
   test_pull();
   test_push();
   test_rewrite_without_rename();
   test_list();

   if (failures == 0) {
      printf("OK ra-sync-core\n");
      return 0;
   }
   printf("FAIL ra-sync-core (%d checks)\n", failures);
   return 1;
}
