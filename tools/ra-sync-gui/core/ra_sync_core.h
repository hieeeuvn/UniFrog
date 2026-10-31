#ifndef UNIFROG_RA_SYNC_CORE_H
#define UNIFROG_RA_SYNC_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Offline RetroAchievements sync, shared by the Windows and Android GUIs.
 *
 * The protocol matches tools/ra_sync.mjs and the files the firmware writes:
 *
 *   achievements/queue/<hash>.req   hashes the device could not identify
 *   achievements/cache/<hash>.json  raw "patch" responses for the device
 *   achievements/queue/unlocks.jsonl unlocks waiting to be awarded
 *
 * All paths given to ra_sync_fs are relative to the achievements folder, so
 * each platform only has to map queue/... and cache/... to its own storage.
 */

#define RA_SYNC_URL "https://retroachievements.org/dorequest.php"
#define RA_SYNC_ERROR_MAX 256
#define RA_SYNC_HASH_MAX 33
#define RA_SYNC_NAME_MAX 128
/* dorequest.php issues 16-character Connect tokens from r=login2. */
#define RA_SYNC_TOKEN_LEN 16
#define RA_SYNC_TOKEN_MAX 64

typedef void (*ra_sync_log_fn)(void *userdata, const char *line);

typedef struct ra_sync_http {
   void *userdata;
   /*
    * POST a form-encoded body. On success returns 0 and stores a malloc'd,
    * NUL-terminated response in *response (the caller frees it). On failure
    * returns -1 and writes a short reason to error.
    */
   int (*post)(void *userdata, const char *body, char **response,
      char *error, size_t error_size);
} ra_sync_http_t;

typedef struct ra_sync_fs {
   void *userdata;
   int (*exists)(void *userdata, const char *path);
   /*
    * List the direct children of dir. The callback receives a bare file name
    * (not a path). Return 0 on success, -1 when the directory is missing.
    */
   int (*list)(void *userdata, const char *dir,
      int (*callback)(void *ud, const char *name), void *callback_ud);
   /* Read a whole file into a malloc'd buffer. Returns 0 on success. */
   int (*read)(void *userdata, const char *path, char **data, size_t *size);
   int (*write)(void *userdata, const char *path, const char *data,
      size_t size);
   int (*remove)(void *userdata, const char *path);
   /* Rename from -> to, replacing to when it exists. */
   int (*rename)(void *userdata, const char *from, const char *to);
} ra_sync_fs_t;

struct ra_sync_options {
   const char *user;
   /*
    * Connect token. dorequest.php only accepts this 16-character value as
    * t=; the 32-character Web API key from the site is for API_*.php and is
    * rejected here with invalid_credentials.
    */
   const char *key;
   /*
    * RetroAchievements account password. It is exchanged for a Connect token
    * with r=login2 and is never sent with any other request.
    */
   const char *password;
   int pull; /* download definitions for queue/<hash>.req */
   int push; /* upload queue/unlocks.jsonl */
   int list; /* report card contents without network */
   int dry_run;
   /* Optional: receives the Connect token that was used or obtained. */
   char *token_out;
   size_t token_out_size;
};

struct ra_sync_stats {
   int requests;
   int cached;
   int unknown;
   int awarded;
   int failed;
   int pending;
   char error[RA_SYNC_ERROR_MAX];
};

/*
 * Run one sync pass. Returns 0 when the pass completed, -1 when it stopped
 * early (missing credentials, login failure, ...). Individual game or award
 * failures do not abort the pass; they are counted and logged.
 */
int ra_sync_run(const ra_sync_http_t *http, const ra_sync_fs_t *fs,
   const struct ra_sync_options *options, ra_sync_log_fn log, void *log_ud,
   struct ra_sync_stats *stats);

/* md5(id + user + hardcore [+ id + seconds]) for the RA award signature. */
void ra_sync_signature(char out[33], unsigned id, const char *user,
   int hardcore, long long seconds);

/* 32 lowercase hex digits. */
int ra_sync_hash_valid(const char *hash);

#ifdef __cplusplus
}
#endif

#endif
