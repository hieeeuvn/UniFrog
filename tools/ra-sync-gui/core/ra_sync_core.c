/*
 * Shared RetroAchievements sync core for the Windows and Android clients.
 *
 * This mirrors tools/ra_sync.mjs: same endpoints, same queue files, same
 * award signature. Keeping the protocol in one C file lets the host test
 * drive pull/push against a fake transport and a temporary card folder.
 */
#include "ra_sync_core.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ra_md5.h"

#define RA_SYNC_QUEUE_DIR "queue"
#define RA_SYNC_CACHE_DIR "cache"
#define RA_SYNC_UNLOCK_PATH "queue/unlocks.jsonl"
#define RA_SYNC_UNLOCK_TEMP "queue/unlocks.jsonl.tmp"
#define RA_SYNC_EARNED_DIR "earned"
#define RA_SYNC_LINE_MAX 512
#define RA_SYNC_PULL_MAX 128
#define RA_SYNC_EARNED_MAX 64
#define RA_SYNC_SEEN_MAX 512
#define RA_SYNC_SEEN_KEY_MAX 64

struct ra_buf {
   char *data;
   size_t length;
   size_t capacity;
};

static void buf_init(struct ra_buf *buffer)
{
   buffer->data = NULL;
   buffer->length = 0;
   buffer->capacity = 0;
}

static void buf_free(struct ra_buf *buffer)
{
   free(buffer->data);
   buffer->data = NULL;
   buffer->length = 0;
   buffer->capacity = 0;
}

static int buf_reserve(struct ra_buf *buffer, size_t extra)
{
   size_t needed = buffer->length + extra + 1u;
   char *grown;
   size_t capacity;

   if (needed <= buffer->capacity)
      return 0;
   capacity = buffer->capacity ? buffer->capacity : 256u;
   while (capacity < needed)
      capacity *= 2u;
   grown = (char *)realloc(buffer->data, capacity);
   if (!grown)
      return -1;
   buffer->data = grown;
   buffer->capacity = capacity;
   return 0;
}

static int buf_add_bytes(struct ra_buf *buffer, const char *data, size_t size)
{
   if (buf_reserve(buffer, size) != 0)
      return -1;
   if (size > 0)
      memcpy(buffer->data + buffer->length, data, size);
   buffer->length += size;
   buffer->data[buffer->length] = '\0';
   return 0;
}

static int buf_add(struct ra_buf *buffer, const char *text)
{
   return buf_add_bytes(buffer, text ? text : "",
      text ? strlen(text) : 0);
}

static int buf_add_format(struct ra_buf *buffer, const char *format, ...)
{
   char text[256];
   va_list arguments;
   int length;

   va_start(arguments, format);
   length = vsnprintf(text, sizeof(text), format, arguments);
   va_end(arguments);
   if (length < 0)
      return -1;
   if ((size_t)length >= sizeof(text))
      length = (int)sizeof(text) - 1;
   return buf_add_bytes(buffer, text, (size_t)length);
}

static int buf_add_encoded(struct ra_buf *buffer, const char *text)
{
   static const char hex[] = "0123456789ABCDEF";
   const unsigned char *cursor = (const unsigned char *)(text ? text : "");

   for (; *cursor; cursor++) {
      unsigned char value = *cursor;
      char encoded[4];

      if ((value >= 'A' && value <= 'Z') ||
          (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') ||
          value == '-' || value == '_' || value == '.' || value == '~') {
         encoded[0] = (char)value;
         encoded[1] = '\0';
      } else if (value == ' ') {
         encoded[0] = '+';
         encoded[1] = '\0';
      } else {
         encoded[0] = '%';
         encoded[1] = hex[value >> 4];
         encoded[2] = hex[value & 15u];
         encoded[3] = '\0';
      }
      if (buf_add(buffer, encoded) != 0)
         return -1;
   }
   return 0;
}

static void safe_copy(char *destination, size_t size, const char *source)
{
   if (!destination || size == 0)
      return;
   if (!source)
      source = "";
   while (*source && size > 1) {
      *destination++ = *source++;
      size--;
   }
   *destination = '\0';
}

static void log_line(ra_sync_log_fn log, void *userdata, const char *line)
{
   if (log && line)
      log(userdata, line);
}

static int ascii_lower(int value)
{
   return (value >= 'A' && value <= 'Z') ? value + 32 : value;
}

static int contains_case_insensitive(const char *text, const char *needle)
{
   size_t needle_length;

   if (!text || !needle || !needle[0])
      return 0;
   needle_length = strlen(needle);
   for (; *text; text++) {
      size_t index = 0;

      while (index < needle_length &&
             ascii_lower((unsigned char)text[index]) ==
             ascii_lower((unsigned char)needle[index]))
         index++;
      if (index == needle_length)
         return 1;
   }
   return 0;
}

/*
 * Minimal JSON field reader for the flat responses we consume (login and
 * award), plus the PatchData title. A full parser would be overkill here and
 * would duplicate what the device engine already validates.
 */
static const char *json_find_value(const char *json, const char *key)
{
   const char *cursor;
   size_t key_length = strlen(key);

   if (!json || !key)
      return NULL;
   for (cursor = json; (cursor = strchr(cursor, '"')) != NULL; cursor++) {
      if (strncmp(cursor + 1, key, key_length) == 0 &&
          cursor[1 + key_length] == '"') {
         const char *value = cursor + key_length + 2;

         while (*value == ' ' || *value == '\t' || *value == '\n' ||
                *value == '\r')
            value++;
         if (*value == ':') {
            value++;
            while (*value == ' ' || *value == '\t' || *value == '\n' ||
                   *value == '\r')
               value++;
            return value;
         }
      }
   }
   return NULL;
}

static int json_bool(const char *json, const char *key, int fallback)
{
   const char *value = json_find_value(json, key);

   if (!value)
      return fallback;
   if (strncmp(value, "true", 4) == 0)
      return 1;
   if (strncmp(value, "false", 5) == 0)
      return 0;
   return fallback;
}

static int json_string(const char *json, const char *key, char *out,
   size_t out_size)
{
   const char *value = json_find_value(json, key);
   size_t used = 0;

   if (!out || out_size == 0)
      return -1;
   out[0] = '\0';
   if (!value || *value != '"')
      return -1;
   value++;
   while (*value && *value != '"' && used + 1 < out_size) {
      char character = *value++;

      if (character == '\\' && *value) {
         char escaped = *value++;

         switch (escaped) {
         case 'n': character = '\n'; break;
         case 'r': character = '\r'; break;
         case 't': character = '\t'; break;
         case 'u':
            while (*value && isxdigit((unsigned char)*value) && used < 4)
               value++;
            character = '?';
            break;
         default: character = escaped; break;
         }
      }
      out[used++] = character;
   }
   out[used] = '\0';
   return 0;
}

static int json_uint(const char *json, const char *key,
   unsigned long long *out)
{
   const char *value = json_find_value(json, key);
   char *end;
   unsigned long long parsed;

   if (!value)
      return -1;
   if (*value == '"')
      value++;
   if (!isdigit((unsigned char)*value))
      return -1;
   parsed = strtoull(value, &end, 10);
   if (end == value)
      return -1;
   if (out)
      *out = parsed;
   return 0;
}

static void server_message(const char *json, char *out, size_t out_size)
{
   static const char *keys[] = { "Error", "error", "Message", "message" };
   unsigned index;
   unsigned long long code;

   for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
      if (json_string(json, keys[index], out, out_size) == 0 && out[0])
         return;
   }
   if (json_uint(json, "Code", &code) == 0) {
      snprintf(out, out_size, "code %llu", code);
      return;
   }
   safe_copy(out, out_size, "request rejected");
}

void ra_sync_signature(char out[33], unsigned id, const char *user,
   int hardcore, long long seconds)
{
   char input[256];

   snprintf(input, sizeof(input), "%u%s%d", id, user ? user : "",
      hardcore ? 1 : 0);
   if (seconds > 0) {
      size_t used = strlen(input);

      snprintf(input + used, sizeof(input) - used, "%u%lld", id, seconds);
   }
   ra_md5_hex(input, strlen(input), out);
}

int ra_sync_hash_valid(const char *hash)
{
   unsigned index;

   if (!hash)
      return 0;
   for (index = 0; index < 32; index++) {
      if (!isxdigit((unsigned char)hash[index]))
         return 0;
   }
   return hash[32] == '\0';
}

static void lowercase_hash(char *hash)
{
   for (; hash && *hash; hash++)
      *hash = (char)tolower((unsigned char)*hash);
}

static long long current_seconds(void)
{
   time_t now = time(NULL);

   return now > 0 ? (long long)now : 0;
}

static int http_post_json(const ra_sync_http_t *http, struct ra_buf *form,
   char **response, char *error, size_t error_size, ra_sync_log_fn log,
   void *log_ud)
{
   int attempt;

   if (!http || !http->post) {
      safe_copy(error, error_size, "no HTTP transport configured");
      return -1;
   }
   for (attempt = 0; attempt < 2; attempt++) {
      char reason[RA_SYNC_ERROR_MAX];

      reason[0] = '\0';
      *response = NULL;
      if (http->post(http->userdata, form->data ? form->data : "",
          response, reason, sizeof(reason)) == 0 && *response)
         return 0;
      safe_copy(error, error_size, reason[0] ? reason : "network error");
      free(*response);
      *response = NULL;
      if (attempt == 0)
         log_line(log, log_ud, "Network error, retrying once");
   }
   return -1;
}

static int pull_one(const ra_sync_http_t *http, const ra_sync_fs_t *fs,
   const struct ra_sync_options *options, const char *user, const char *key,
   const char *hash, ra_sync_log_fn log, void *log_ud,
   struct ra_sync_stats *stats)
{
   char cache_path[RA_SYNC_NAME_MAX + 16];
   char request_path[RA_SYNC_NAME_MAX + 16];
   char error[RA_SYNC_ERROR_MAX];
   char message[RA_SYNC_ERROR_MAX];
   char line[RA_SYNC_LINE_MAX];
   struct ra_buf form;
   char *response = NULL;

   snprintf(cache_path, sizeof(cache_path), RA_SYNC_CACHE_DIR "/%s.json",
      hash);
   snprintf(request_path, sizeof(request_path), RA_SYNC_QUEUE_DIR "/%s.req",
      hash);

   if (fs->exists && fs->exists(fs->userdata, cache_path)) {
      snprintf(line, sizeof(line), "%s: already cached", hash);
      log_line(log, log_ud, line);
      if (!options->dry_run && fs->remove)
         (void)fs->remove(fs->userdata, request_path);
      return 0;
   }
   if (options->dry_run) {
      snprintf(line, sizeof(line), "%s: would download definitions", hash);
      log_line(log, log_ud, line);
      return 0;
   }

   stats->requests++;
   buf_init(&form);
   buf_add(&form, "r=patch&u=");
   buf_add_encoded(&form, user);
   buf_add(&form, "&t=");
   buf_add_encoded(&form, key);
   buf_add(&form, "&m=");
   buf_add_encoded(&form, hash);

   if (http_post_json(http, &form, &response, error, sizeof(error), log,
       log_ud) != 0) {
      buf_free(&form);
      snprintf(line, sizeof(line), "%s: %s", hash, error);
      log_line(log, log_ud, line);
      stats->failed++;
      return -1;
   }
   buf_free(&form);

   if (!json_bool(response, "Success", 0)) {
      char title[96];

      server_message(response, message, sizeof(message));
      json_string(response, "Title", title, sizeof(title));
      stats->unknown++;
      snprintf(line, sizeof(line), "%s: %s", hash, message);
      log_line(log, log_ud, line);
      log_line(log, log_ud, "  (request kept for the next sync)");
      free(response);
      return -1;
   }

   if (fs->write(fs->userdata, cache_path, response, strlen(response)) != 0) {
      snprintf(line, sizeof(line), "%s: could not write %s", hash,
         cache_path);
      log_line(log, log_ud, line);
      stats->failed++;
      free(response);
      return -1;
   }
   stats->cached++;
   {
      char title[96];

      json_string(response, "Title", title, sizeof(title));
      snprintf(line, sizeof(line), "%s: cached \"%s\"", hash,
         title[0] ? title : "unknown game");
      log_line(log, log_ud, line);
   }
   free(response);

   if (fs->remove && fs->remove(fs->userdata, request_path) != 0) {
      snprintf(line, sizeof(line), "%s: could not remove %s", hash,
         request_path);
      log_line(log, log_ud, line);
   }
   return 0;
}

struct pull_scan {
   char hashes[RA_SYNC_PULL_MAX][RA_SYNC_HASH_MAX];
   unsigned count;
   int overflow;
};

static int pull_scan_callback(void *userdata, const char *name)
{
   struct pull_scan *scan = (struct pull_scan *)userdata;
   size_t length;

   if (!name)
      return 0;
   length = strlen(name);
   if (length != 36 || strcmp(name + 32, ".req") != 0)
      return 0;
   if (scan->count >= RA_SYNC_PULL_MAX) {
      scan->overflow = 1;
      return 0;
   }
   memcpy(scan->hashes[scan->count], name, 32);
   scan->hashes[scan->count][32] = '\0';
   lowercase_hash(scan->hashes[scan->count]);
   if (!ra_sync_hash_valid(scan->hashes[scan->count]))
      return 0;
   scan->count++;
   return 0;
}

static void sync_pull(const ra_sync_http_t *http, const ra_sync_fs_t *fs,
   const struct ra_sync_options *options, const char *user, const char *key,
   ra_sync_log_fn log, void *log_ud, struct ra_sync_stats *stats)
{
   struct pull_scan scan;
   char line[RA_SYNC_LINE_MAX];
   unsigned index;

   memset(&scan, 0, sizeof(scan));
   if (!fs->list || fs->list(fs->userdata, RA_SYNC_QUEUE_DIR,
       pull_scan_callback, &scan) != 0) {
      log_line(log, log_ud, "No queue folder on the card");
      return;
   }
   if (scan.overflow) {
      snprintf(line, sizeof(line),
         "More than %u pending games; sync again after this pass",
         RA_SYNC_PULL_MAX);
      log_line(log, log_ud, line);
   }
   for (index = 0; index < scan.count; index++) {
      char request_path[RA_SYNC_NAME_MAX + 16];
      char *data = NULL;
      size_t size = 0;

      snprintf(request_path, sizeof(request_path),
         RA_SYNC_QUEUE_DIR "/%s.req", scan.hashes[index]);
      if (fs->read &&
          fs->read(fs->userdata, request_path, &data, &size) == 0) {
         char content[160];

         if (json_string(data, "content", content, sizeof(content)) == 0 &&
             content[0]) {
            snprintf(line, sizeof(line), "%s: requested by %s",
               scan.hashes[index], content);
            log_line(log, log_ud, line);
         }
         free(data);
      }
      (void)pull_one(http, fs, options, user, key, scan.hashes[index], log,
         log_ud, stats);
   }
   if (scan.count == 0)
      log_line(log, log_ud, "No pending game requests");
}

static int push_seen_add(char seen[RA_SYNC_SEEN_MAX][RA_SYNC_SEEN_KEY_MAX],
   unsigned *count, unsigned id, int hardcore, const char *hash)
{
   char key[RA_SYNC_SEEN_KEY_MAX];
   unsigned index;

   snprintf(key, sizeof(key), "%u:%d:%s", id, hardcore ? 1 : 0,
      hash ? hash : "");
   for (index = 0; index < *count; index++) {
      if (strcmp(seen[index], key) == 0)
         return 0;
   }
   if (*count < RA_SYNC_SEEN_MAX) {
      safe_copy(seen[*count], sizeof(seen[0]), key);
      (*count)++;
   }
   return 1;
}

/* Returns 1 when the raw line must stay queued, 0 when it is done. */
static int push_line(const ra_sync_http_t *http,
   const struct ra_sync_options *options, const char *user, const char *key,
   const char *line, ra_sync_log_fn log, void *log_ud,
   struct ra_sync_stats *stats)
{
   unsigned long long id = 0;
   unsigned long long value = 0;
   unsigned long long seconds = 0;
   char hash[RA_SYNC_HASH_MAX];
   char signature[33];
   char message[RA_SYNC_ERROR_MAX];
   char error[RA_SYNC_ERROR_MAX];
   char output[RA_SYNC_LINE_MAX];
   struct ra_buf form;
   char *response = NULL;
   int hardcore = 0;

   memset(hash, 0, sizeof(hash));
   if (json_uint(line, "a", &id) != 0 || id == 0 || id > 0xFFFFFFFFull) {
      log_line(log, log_ud, "Dropping malformed unlock");
      return 0;
   }
   (void)json_uint(line, "m", &value);
   hardcore = value ? 1 : 0;
   if (json_string(line, "h", hash, sizeof(hash)) != 0)
      hash[0] = '\0';
   if (json_uint(line, "t", &value) == 0 && value > 0) {
      long long now = current_seconds();

      if (now > (long long)value)
         seconds = (unsigned long long)(now - (long long)value);
   }

   if (options->dry_run) {
      snprintf(output, sizeof(output),
         "achievement %llu: would upload (%llu s late)", id, seconds);
      log_line(log, log_ud, output);
      return 1;
   }

   ra_sync_signature(signature, (unsigned)id, user, hardcore,
      (long long)seconds);
   buf_init(&form);
   buf_add(&form, "r=awardachievement&u=");
   buf_add_encoded(&form, user);
   buf_add(&form, "&t=");
   buf_add_encoded(&form, key);
   buf_add_format(&form, "&a=%llu&h=%d&v=", id, hardcore ? 1 : 0);
   buf_add(&form, signature);
   if (hash[0] && ra_sync_hash_valid(hash)) {
      buf_add(&form, "&m=");
      buf_add_encoded(&form, hash);
   }
   if (seconds > 0)
      buf_add_format(&form, "&o=%llu", seconds);

   if (http_post_json(http, &form, &response, error, sizeof(error), log,
       log_ud) != 0) {
      stats->failed++;
      snprintf(output, sizeof(output), "achievement %llu: %s", id, error);
      log_line(log, log_ud, output);
      buf_free(&form);
      return 1;
   }
   buf_free(&form);

   if (json_bool(response, "Success", 0)) {
      stats->awarded++;
      snprintf(output, sizeof(output), "achievement %llu: awarded", id);
      log_line(log, log_ud, output);
      free(response);
      return 0;
   }

   server_message(response, message, sizeof(message));
   if (contains_case_insensitive(message, "already") ||
       contains_case_insensitive(message, "previously") ||
       contains_case_insensitive(message, "duplicate")) {
      stats->awarded++;
      snprintf(output, sizeof(output),
         "achievement %llu: awarded (already unlocked)", id);
      log_line(log, log_ud, output);
      free(response);
      return 0;
   }

   stats->failed++;
   snprintf(output, sizeof(output), "achievement %llu (%s): %s", id,
      hash[0] ? hash : "no hash", message);
   log_line(log, log_ud, output);
   free(response);
   return 1;
}

static void sync_push(const ra_sync_http_t *http, const ra_sync_fs_t *fs,
   const struct ra_sync_options *options, const char *user, const char *key,
   ra_sync_log_fn log, void *log_ud, struct ra_sync_stats *stats)
{
   char *data = NULL;
   size_t size = 0;
   struct ra_buf remaining;
   char seen[RA_SYNC_SEEN_MAX][RA_SYNC_SEEN_KEY_MAX];
   unsigned seen_count = 0;
   unsigned line_number = 0;
   size_t offset = 0;
   unsigned kept = 0;

   if (!fs->read ||
       fs->read(fs->userdata, RA_SYNC_UNLOCK_PATH, &data, &size) != 0) {
      log_line(log, log_ud, "No queued unlocks");
      stats->pending = 0;
      return;
   }

   buf_init(&remaining);
   while (offset < size) {
      size_t start = offset;
      size_t length;
      char line[RA_SYNC_LINE_MAX];

      while (offset < size && data[offset] != '\n')
         offset++;
      length = offset - start;
      if (offset < size)
         offset++;
      if (length == 0)
         continue;
      if (length >= sizeof(line))
         length = sizeof(line) - 1;
      memcpy(line, data + start, length);
      line[length] = '\0';
      while (length > 0 && (line[length - 1] == '\r' ||
             line[length - 1] == ' ' || line[length - 1] == '\t'))
         line[--length] = '\0';
      if (line[0] == '\0')
         continue;
      line_number++;

      {
         unsigned long long id = 0;
         unsigned long long hardcore_value = 0;
         char hash[RA_SYNC_HASH_MAX];

         memset(hash, 0, sizeof(hash));
         if (json_uint(line, "a", &id) != 0 || id == 0 ||
             id > 0xFFFFFFFFull) {
            char detail[64];

            snprintf(detail, sizeof(detail),
               "Dropping malformed unlock line %u", line_number);
            log_line(log, log_ud, detail);
            continue;
         }
         (void)json_uint(line, "m", &hardcore_value);
         (void)json_string(line, "h", hash, sizeof(hash));
         if (!push_seen_add(seen, &seen_count, (unsigned)id,
             hardcore_value ? 1 : 0, hash)) {
            char detail[64];

            snprintf(detail, sizeof(detail), "Dropping duplicate unlock %u",
               (unsigned)id);
            log_line(log, log_ud, detail);
            continue;
         }
      }

      if (push_line(http, options, user, key, line, log, log_ud, stats) != 0) {
         if (buf_add(&remaining, line) != 0 ||
             buf_add(&remaining, "\n") != 0)
            log_line(log, log_ud, "Out of memory while keeping failed unlock");
         kept++;
      }
   }
   free(data);

   stats->pending = (int)kept;
   if (options->dry_run) {
      buf_free(&remaining);
      return;
   }
   if (kept == 0) {
      if (fs->remove)
         (void)fs->remove(fs->userdata, RA_SYNC_UNLOCK_PATH);
      buf_free(&remaining);
      return;
   }
   if (fs->write(fs->userdata, RA_SYNC_UNLOCK_TEMP,
       remaining.data ? remaining.data : "", remaining.length) != 0) {
      log_line(log, log_ud,
         "Could not write the queue; unlocks stay on the card");
      buf_free(&remaining);
      return;
   }
   if (fs->rename && fs->rename(fs->userdata, RA_SYNC_UNLOCK_TEMP,
       RA_SYNC_UNLOCK_PATH) == 0) {
      buf_free(&remaining);
      return;
   }
   /* Some cards reject rename; rewrite the original file instead. */
   (void)fs->remove(fs->userdata, RA_SYNC_UNLOCK_PATH);
   if (fs->write(fs->userdata, RA_SYNC_UNLOCK_PATH,
       remaining.data ? remaining.data : "", remaining.length) != 0)
      log_line(log, log_ud, "Could not rewrite the unlock queue");
   buf_free(&remaining);
}

struct list_counts {
   unsigned caches;
   unsigned requests;
   unsigned unlocks;
   unsigned earned_games;
};

static int list_cache_cb(void *userdata, const char *name)
{
   struct list_counts *counts = (struct list_counts *)userdata;
   size_t length = name ? strlen(name) : 0;

   if (length > 5 && strcmp(name + length - 5, ".json") == 0)
      counts->caches++;
   return 0;
}

static int list_request_cb(void *userdata, const char *name)
{
   struct list_counts *counts = (struct list_counts *)userdata;
   size_t length = name ? strlen(name) : 0;

   if (length > 4 && strcmp(name + length - 4, ".req") == 0)
      counts->requests++;
   return 0;
}

static int list_earned_cb(void *userdata, const char *name)
{
   struct list_counts *counts = (struct list_counts *)userdata;
   size_t length = name ? strlen(name) : 0;

   if (length > 4 && strcmp(name + length - 4, ".txt") == 0)
      counts->earned_games++;
   return 0;
}

static void sync_list(const ra_sync_fs_t *fs, ra_sync_log_fn log,
   void *log_ud, struct ra_sync_stats *stats)
{
   struct list_counts counts;
   char line[RA_SYNC_LINE_MAX];
   char *data = NULL;
   size_t size = 0;

   memset(&counts, 0, sizeof(counts));
   if (fs->list) {
      (void)fs->list(fs->userdata, RA_SYNC_CACHE_DIR, list_cache_cb, &counts);
      (void)fs->list(fs->userdata, RA_SYNC_QUEUE_DIR, list_request_cb,
         &counts);
      (void)fs->list(fs->userdata, RA_SYNC_EARNED_DIR, list_earned_cb,
         &counts);
   }
   if (fs->read && fs->read(fs->userdata, RA_SYNC_UNLOCK_PATH, &data,
       &size) == 0) {
      size_t offset = 0;

      while (offset < size) {
         if (data[offset] == '\n')
            counts.unlocks++;
         offset++;
      }
      if (size > 0 && data[size - 1] != '\n')
         counts.unlocks++;
   }
   free(data);

   snprintf(line, sizeof(line), "Cached games: %u", counts.caches);
   log_line(log, log_ud, line);
   snprintf(line, sizeof(line), "Pending requests: %u", counts.requests);
   log_line(log, log_ud, line);
   snprintf(line, sizeof(line), "Queued unlocks: %u", counts.unlocks);
   log_line(log, log_ud, line);
   snprintf(line, sizeof(line), "Earned on card: %u game%s",
      counts.earned_games, counts.earned_games == 1 ? "" : "s");
   log_line(log, log_ud, line);
   stats->pending = (int)counts.unlocks;
}

/*
 * The firmware records every id it unlocked in earned/<hash>.txt, one id per
 * line, before or after the queue line reaches the card. A failed queue write
 * (or a queue the tool already consumed) leaves the id in that list only, so
 * the award never reaches the server. Push those ids as well; the card keeps
 * the list, and the server answers "already unlocked" once an id is there.
 *
 * The file holds ids only, so the award goes out with the casual flag and no
 * seconds. The game hash comes from the file name, which is what the server
 * uses to attach the award to the right game.
 */
struct earned_scan {
   char hashes[RA_SYNC_EARNED_MAX][RA_SYNC_HASH_MAX];
   unsigned count;
   int overflow;
};

static int earned_scan_callback(void *userdata, const char *name)
{
   struct earned_scan *scan = (struct earned_scan *)userdata;
   size_t length;

   if (!name)
      return 0;
   length = strlen(name);
   if (length != 36 || strcmp(name + 32, ".txt") != 0)
      return 0;
   if (scan->count >= RA_SYNC_EARNED_MAX) {
      scan->overflow = 1;
      return 0;
   }
   memcpy(scan->hashes[scan->count], name, 32);
   scan->hashes[scan->count][32] = '\0';
   lowercase_hash(scan->hashes[scan->count]);
   if (!ra_sync_hash_valid(scan->hashes[scan->count]))
      return 0;
   scan->count++;
   return 0;
}

static void sync_earned(const ra_sync_http_t *http, const ra_sync_fs_t *fs,
   const struct ra_sync_options *options, const char *user, const char *key,
   ra_sync_log_fn log, void *log_ud, struct ra_sync_stats *stats)
{
   struct earned_scan scan;
   char seen[RA_SYNC_SEEN_MAX][RA_SYNC_SEEN_KEY_MAX];
   unsigned seen_count = 0;
   char line[RA_SYNC_LINE_MAX];
   unsigned index;

   memset(&scan, 0, sizeof(scan));
   if (!fs->list || fs->list(fs->userdata, RA_SYNC_EARNED_DIR,
       earned_scan_callback, &scan) != 0)
      return;

   for (index = 0; index < scan.count; index++) {
      char path[RA_SYNC_NAME_MAX + 16];
      char *data = NULL;
      size_t size = 0;
      size_t offset = 0;
      unsigned ids = 0;

      snprintf(path, sizeof(path), RA_SYNC_EARNED_DIR "/%s.txt",
         scan.hashes[index]);
      if (!fs->read || fs->read(fs->userdata, path, &data, &size) != 0)
         continue;
      while (offset < size) {
         unsigned long value = 0;
         int digits = 0;

         while (offset < size && (data[offset] == ' ' ||
                data[offset] == '\t' || data[offset] == '\r' ||
                data[offset] == '\n'))
            offset++;
         while (offset < size && data[offset] >= '0' &&
                data[offset] <= '9' && digits < 9) {
            value = value * 10ul + (unsigned long)(data[offset] - '0');
            digits++;
            offset++;
         }
         while (offset < size && data[offset] != '\n')
            offset++;
         if (offset < size)
            offset++;
         if (digits == 0 || value == 0)
            continue;
         if (!push_seen_add(seen, &seen_count, (unsigned)value, 0,
             scan.hashes[index]))
            continue;
         ids++;
         snprintf(line, sizeof(line),
            "{\"a\":%lu,\"h\":\"%s\",\"m\":0,\"t\":0}", value,
            scan.hashes[index]);
         (void)push_line(http, options, user, key, line, log, log_ud, stats);
      }
      free(data);
      if (ids > 0) {
         snprintf(line, sizeof(line),
            "%s: %u achievement%s from the earned list (casual, no "
            "timestamp)", scan.hashes[index], ids, ids == 1 ? "" : "s");
         log_line(log, log_ud, line);
      }
   }
   if (scan.overflow)
      log_line(log, log_ud,
         "More earned games than one pass handles; sync again for the rest");
}

/*
 * Work out which credential goes into t=. A 16-character value in the key
 * field is already a Connect token; otherwise the account password is
 * exchanged for one with r=login2. The 32-character Web API key is never
 * sent to dorequest.php because the server rejects it.
 */
static int resolve_token(const ra_sync_http_t *http,
   const struct ra_sync_options *options, char *token, size_t token_size,
   ra_sync_log_fn log, void *log_ud, struct ra_sync_stats *stats)
{
   const char *key = options->key ? options->key : "";
   const char *password = options->password ? options->password : "";
   char line[RA_SYNC_LINE_MAX];
   char *response = NULL;
   char error[RA_SYNC_ERROR_MAX];
   char message[RA_SYNC_ERROR_MAX];
   struct ra_buf form;

   token[0] = '\0';
   if (strlen(key) == RA_SYNC_TOKEN_LEN) {
      safe_copy(token, token_size, key);
      snprintf(line, sizeof(line), "Using the Connect token for %s",
         options->user);
      log_line(log, log_ud, line);
      return 0;
   }
   if (key[0]) {
      snprintf(line, sizeof(line),
         "Ignoring the %u-character Web API key: dorequest.php needs a "
         "Connect token", (unsigned)strlen(key));
      log_line(log, log_ud, line);
   }
   if (!password[0]) {
      safe_copy(stats->error, sizeof(stats->error),
         "Enter your RetroAchievements password once, or a 16-character "
         "Connect token: the 32-character Web API key only works with "
         "API_*.php");
      log_line(log, log_ud, stats->error);
      return -1;
   }
   if (options->dry_run) {
      log_line(log, log_ud, "Would log in to obtain a Connect token");
      return 0;
   }

   buf_init(&form);
   buf_add(&form, "r=login2&u=");
   buf_add_encoded(&form, options->user);
   buf_add(&form, "&p=");
   buf_add_encoded(&form, password);
   if (http_post_json(http, &form, &response, error, sizeof(error), log,
       log_ud) != 0) {
      safe_copy(stats->error, sizeof(stats->error), error);
      log_line(log, log_ud, error);
      buf_free(&form);
      return -1;
   }
   buf_free(&form);

   if (!json_bool(response, "Success", 0) ||
       json_string(response, "Token", token, token_size) != 0 || !token[0]) {
      server_message(response, message, sizeof(message));
      if (!message[0])
         snprintf(message, sizeof(message),
            "empty error message from the server");
      if (contains_case_insensitive(message, "password"))
         snprintf(stats->error, sizeof(stats->error),
            "Login failed: %.180s (check the account password)", message);
      else
         snprintf(stats->error, sizeof(stats->error),
            "Login failed: %.160s (user %s, password %u chars)", message,
            options->user, (unsigned)strlen(password));
      log_line(log, log_ud, stats->error);
      free(response);
      return -1;
   }
   free(response);
   snprintf(line, sizeof(line),
      "Logged in as %s (Connect token: %u characters)", options->user,
      (unsigned)strlen(token));
   log_line(log, log_ud, line);
   return 0;
}

int ra_sync_run(const ra_sync_http_t *http, const ra_sync_fs_t *fs,
   const struct ra_sync_options *options, ra_sync_log_fn log, void *log_ud,
   struct ra_sync_stats *stats)
{
   struct ra_sync_stats local;
   char token[RA_SYNC_TOKEN_MAX];

   if (!stats)
      stats = &local;
   memset(stats, 0, sizeof(*stats));
   if (!fs || !options) {
      safe_copy(stats->error, sizeof(stats->error), "invalid sync request");
      return -1;
   }
   if (!options->list &&
       (!options->user || !options->user[0] ||
        ((!options->key || !options->key[0]) &&
         (!options->password || !options->password[0])))) {
      safe_copy(stats->error, sizeof(stats->error),
         "Missing credentials: enter your RetroAchievements username and "
         "password (or a 16-character Connect token)");
      log_line(log, log_ud, stats->error);
      return -1;
   }

   if (options->list) {
      sync_list(fs, log, log_ud, stats);
      return 0;
   }

   if (resolve_token(http, options, token, sizeof(token), log, log_ud,
       stats) != 0)
      return -1;
   if (options->token_out && options->token_out_size)
      safe_copy(options->token_out, options->token_out_size, token);

   if (options->pull)
      sync_pull(http, fs, options, options->user, token, log, log_ud, stats);
   if (options->push) {
      sync_push(http, fs, options, options->user, token, log, log_ud, stats);
      sync_earned(http, fs, options, options->user, token, log, log_ud,
         stats);
   }
   return 0;
}
