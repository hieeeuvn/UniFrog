#ifndef UNIFROG_ACHIEVEMENTS_H
#define UNIFROG_ACHIEVEMENTS_H

#include <stddef.h>
#include <stdint.h>

#include <unifrog/gfx.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Offline-first RetroAchievements support.
 *
 * The device has no network stack, so achievement definitions and unlock
 * uploads travel through the SD card. The emulator identifies the game with
 * the same hashes RetroAchievements uses, evaluates the cached definitions
 * every frame, stores unlocks in a durable queue, and the companion tool
 * (tools/ra_sync.mjs) exchanges those files with retroachievements.org.
 */

#define UNIFROG_ACHIEVEMENTS_TITLE_MAX 48
#define UNIFROG_ACHIEVEMENTS_DESCRIPTION_MAX 112
#define UNIFROG_ACHIEVEMENTS_STATUS_MAX 96
#define UNIFROG_ACHIEVEMENTS_NAME_MAX 64
#define UNIFROG_ACHIEVEMENTS_HASH_MAX 33

enum unifrog_achievements_state {
   UNIFROG_ACHIEVEMENTS_OFF = 0,
   UNIFROG_ACHIEVEMENTS_WAITING_HASH,
   UNIFROG_ACHIEVEMENTS_WAITING_CACHE,
   UNIFROG_ACHIEVEMENTS_READY,
   UNIFROG_ACHIEVEMENTS_ERROR,
};

struct unifrog_achievements_summary {
   enum unifrog_achievements_state state;
   int hardcore;
   unsigned total;
   unsigned unlocked;
   unsigned points_total;
   unsigned points_unlocked;
   unsigned pending_uploads;
   char game_title[UNIFROG_ACHIEVEMENTS_NAME_MAX];
   char hash[UNIFROG_ACHIEVEMENTS_HASH_MAX];
   char status[UNIFROG_ACHIEVEMENTS_STATUS_MAX];
};

struct unifrog_achievements_entry {
   unsigned id;
   unsigned points;
   int unlocked;
   /* -1 when the achievement has no measured progress. */
   int progress_percent;
   char title[UNIFROG_ACHIEVEMENTS_TITLE_MAX];
   char description[UNIFROG_ACHIEVEMENTS_DESCRIPTION_MAX];
};

/*
 * Register the core memory accessors so the engine can read emulated RAM.
 * The libretro host calls this once per core load, before begin().
 */
void unifrog_achievements_set_memory_access(
   void *(*data)(unsigned id), size_t (*size)(unsigned id));

/*
 * Start tracking the content that is about to run. Called by the libretro
 * session after retro_load_game() so core memory is available. Returns 0 when
 * the subsystem is disabled or the core has no RetroAchievements console.
 */
int unifrog_achievements_begin(const char *core_id, const char *content_path);

/* Release runtime/cache resources. Safe to call without a matching begin. */
void unifrog_achievements_end(void);

/* Evaluate triggers and advance unlock toasts. Called once per core frame. */
void unifrog_achievements_frame(void);

/* Draw the unlock toast on the active presenter buffer. Returns 1 if the
 * buffer changed, so the host can flush only when needed. */
int unifrog_achievements_draw_overlay(struct unifrog_surface *surface);

/* Current game summary, list access, and the transient toast text. */
void unifrog_achievements_summary(struct unifrog_achievements_summary *summary);
unsigned unifrog_achievements_count(void);
int unifrog_achievements_entry(unsigned index,
   struct unifrog_achievements_entry *entry);
const char *unifrog_achievements_toast(void);

/* In-game menu toggles. Persisted to unifrog_data/achievements/ra.ini. */
int unifrog_achievements_enabled(void);
int unifrog_achievements_set_enabled(int enabled);
int unifrog_achievements_hardcore(void);
int unifrog_achievements_set_hardcore(int hardcore);
/* Rewrite the summary after a menu toggle so the UI reflects it immediately. */
void unifrog_achievements_refresh(void);

#ifdef __cplusplus
}
#endif

#endif
