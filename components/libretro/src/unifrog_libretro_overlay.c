/*
 * Game overlay paks.
 *
 * A pak normally runs a JS2300 script, but the script runtime is either
 * standalone, the frontend, or the core itself, so a third party cannot own a
 * frame while somebody else's core runs. An overlay pak is therefore a folder
 * under /unifrog_data/apps that only *describes* the panel, and the libretro
 * host draws it natively over the presented game frame:
 *
 *   title=Stats
 *   position=top-right
 *   line1=clock
 *   line2=battery,achievements
 *
 * Tokens: clock, battery, achievements, game, points. Up to four lines, three
 * tokens per line, one space between tokens. The active pak is chosen on the
 * device (Custom -> Game Overlay) and stored as overlay_pak in unifrog.ini;
 * "off" or a missing pak leaves the game untouched.
 *
 * See docs/app-paks.md.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <unifrog/achievements.h>
#include <unifrog/battery.h>
#include <unifrog/clock.h>
#include <unifrog/config.h>
#include <unifrog/gfx.h>
#include <unifrog/log.h>
#include <unifrog/paths.h>
#include <unifrog/perf.h>
#include <unifrog/text.h>

#define OVERLAY_MAX_LINES 4u
#define OVERLAY_TOKENS_PER_LINE 3u
#define OVERLAY_LINE_MAX 48u
#define OVERLAY_ID_MAX 32u
#define OVERLAY_PATH_MAX 256u
#define OVERLAY_REFRESH_MS 500u
#define OVERLAY_MARGIN 6
#define OVERLAY_PAD 5

enum overlay_token {
   OVERLAY_TOKEN_NONE = 0,
   OVERLAY_TOKEN_CLOCK,
   OVERLAY_TOKEN_BATTERY,
   OVERLAY_TOKEN_ACHIEVEMENTS,
   OVERLAY_TOKEN_GAME,
   OVERLAY_TOKEN_POINTS,
};

enum overlay_position {
   OVERLAY_POSITION_TOP_LEFT = 0,
   OVERLAY_POSITION_TOP_RIGHT,
   OVERLAY_POSITION_BOTTOM_LEFT,
   OVERLAY_POSITION_BOTTOM_RIGHT,
};

struct overlay_line {
   unsigned token_count;
   enum overlay_token tokens[OVERLAY_TOKENS_PER_LINE];
};

static struct {
   int need_load;
   int active;
   enum overlay_position position;
   unsigned line_count;
   struct overlay_line lines[OVERLAY_MAX_LINES];
   char id[OVERLAY_ID_MAX];
   uint32_t built_ms;
   char text[OVERLAY_MAX_LINES][OVERLAY_LINE_MAX];
} overlay;

static const char *overlay_trim(const char *value)
{
   static char buffer[OVERLAY_LINE_MAX];

   if (!value)
      return "";
   while (*value == ' ' || *value == '\t')
      value++;
   unifrog_text_copy(buffer, sizeof(buffer), value);
   while (buffer[0] &&
      buffer[strlen(buffer) - 1u] == ' ')
      buffer[strlen(buffer) - 1u] = '\0';
   return buffer;
}

/* The pak folder name becomes a path component, so keep it a plain name. */
static int overlay_id_ok(const char *id)
{
   if (!id || !id[0] || id[0] == '.' || strlen(id) >= OVERLAY_ID_MAX)
      return 0;
   if (strstr(id, "..") || strchr(id, '/') || strchr(id, '\\') ||
       strchr(id, ':'))
      return 0;
   return 1;
}

static enum overlay_token overlay_token_parse(const char *name)
{
   if (strcasecmp(name, "clock") == 0)
      return OVERLAY_TOKEN_CLOCK;
   if (strcasecmp(name, "battery") == 0)
      return OVERLAY_TOKEN_BATTERY;
   if (strcasecmp(name, "achievements") == 0 ||
       strcasecmp(name, "ach") == 0)
      return OVERLAY_TOKEN_ACHIEVEMENTS;
   if (strcasecmp(name, "game") == 0)
      return OVERLAY_TOKEN_GAME;
   if (strcasecmp(name, "points") == 0)
      return OVERLAY_TOKEN_POINTS;
   return OVERLAY_TOKEN_NONE;
}

static void overlay_parse_tokens(struct overlay_line *line, const char *value)
{
   char piece[OVERLAY_LINE_MAX];
   unsigned index = 0;
   unsigned start = 0;
   unsigned length = 0;
   enum overlay_token token;

   line->token_count = 0;
   if (!value)
      return;
   for (;;) {
      char ch = value[start + length];

      if (ch == ',' || ch == '\0') {
         if (length > 0 && length < sizeof(piece) &&
             index < OVERLAY_TOKENS_PER_LINE) {
            memcpy(piece, value + start, length);
            piece[length] = '\0';
            token = overlay_token_parse(overlay_trim(piece));
            if (token != OVERLAY_TOKEN_NONE)
               line->tokens[line->token_count++] = token;
            index++;
         }
         if (ch == '\0')
            break;
         start += length + 1u;
         length = 0;
      } else {
         length++;
         if (length >= sizeof(piece)) {
            /* Overlong token: skip to the next separator. */
            while (value[start + length] && value[start + length] != ',')
               length++;
         }
      }
   }
}

static int overlay_pak_key(void *userdata, const char *section, const char *key,
   const char *value, unsigned line_number)
{
   char *id = userdata;

   (void)section;
   (void)line_number;
   if (strcasecmp(key, "overlay_pak") == 0)
      unifrog_text_copy(id, OVERLAY_ID_MAX, overlay_trim(value));
   return 0;
}

static int overlay_manifest_key(void *userdata, const char *section,
   const char *key, const char *value, unsigned line_number)
{
   (void)userdata;
   (void)section;
   (void)line_number;

   if (strcasecmp(key, "position") == 0) {
      const char *position = overlay_trim(value);

      if (strcasecmp(position, "top-left") == 0 ||
          strcasecmp(position, "tl") == 0)
         overlay.position = OVERLAY_POSITION_TOP_LEFT;
      else if (strcasecmp(position, "bottom-left") == 0 ||
          strcasecmp(position, "bl") == 0)
         overlay.position = OVERLAY_POSITION_BOTTOM_LEFT;
      else if (strcasecmp(position, "bottom-right") == 0 ||
          strcasecmp(position, "br") == 0)
         overlay.position = OVERLAY_POSITION_BOTTOM_RIGHT;
      else
         overlay.position = OVERLAY_POSITION_TOP_RIGHT;
      return 0;
   }
   if (strncasecmp(key, "line", 4) == 0 && key[4] >= '1' &&
       key[4] <= '4' && key[5] == '\0') {
      unsigned index = (unsigned)(key[4] - '1');

      overlay_parse_tokens(&overlay.lines[index], value);
      if (index + 1u > overlay.line_count)
         overlay.line_count = index + 1u;
   }
   return 0;
}

static void overlay_load(void)
{
   char id[OVERLAY_ID_MAX];
   char path[OVERLAY_PATH_MAX];
   unsigned index;

   overlay.need_load = 0;
   overlay.active = 0;
   overlay.line_count = 0;
   overlay.built_ms = 0;
   overlay.id[0] = '\0';
   id[0] = '\0';
   if (unifrog_config_read(UNIFROG_CONFIG_PATH, overlay_pak_key, id, NULL) != 0)
      return;
   if (!id[0] || strcasecmp(id, "off") == 0 || !overlay_id_ok(id))
      return;
   if (snprintf(path, sizeof(path), "%s/%s/overlay.ini", UNIFROG_APP_ROOT, id)
       >= (int)sizeof(path))
      return;
   if (unifrog_config_read(path, overlay_manifest_key, NULL, NULL) != 0) {
      unifrog_log("libretro overlay pak skipped name=%s reason=no_manifest\n",
         id);
      return;
   }
   if (overlay.line_count == 0u) {
      unifrog_log("libretro overlay pak skipped name=%s reason=no_lines\n", id);
      return;
   }
   unifrog_text_copy(overlay.id, sizeof(overlay.id), id);
   overlay.active = 1;
   unifrog_log("libretro overlay pak name=%s lines=%u position=%u\n", overlay.id,
      overlay.line_count, (unsigned)overlay.position);
}

static void overlay_append(char *dst, size_t size, const char *text)
{
   size_t length;

   if (!text || !text[0])
      return;
   length = strlen(dst);
   if (length + 1u >= size)
      return;
   snprintf(dst + length, size - length, length ? " %s" : "%s", text);
}

static void overlay_token_text(enum overlay_token token, char *out, size_t size,
   const struct unifrog_battery_status *battery,
   const struct unifrog_achievements_summary *summary)
{
   struct tm clock;

   out[0] = '\0';
   switch (token) {
   case OVERLAY_TOKEN_CLOCK:
      if (unifrog_clock_get_local(&clock) == 0)
         snprintf(out, size, "%02d:%02d", clock.tm_hour, clock.tm_min);
      else
         snprintf(out, size, "--:--");
      break;
   case OVERLAY_TOKEN_BATTERY:
      if (battery && battery->available)
         snprintf(out, size, "%u%%", battery->percent);
      else
         snprintf(out, size, "--%%");
      break;
   case OVERLAY_TOKEN_ACHIEVEMENTS:
      if (summary->total)
         snprintf(out, size, "%u/%u", summary->unlocked, summary->total);
      else
         snprintf(out, size, "no ACH");
      break;
   case OVERLAY_TOKEN_POINTS:
      snprintf(out, size, "%upt", summary->points_unlocked);
      break;
   case OVERLAY_TOKEN_GAME:
      if (summary->game_title[0])
         snprintf(out, size, "%s", summary->game_title);
      break;
   default:
      break;
   }
}

static void overlay_build(const struct unifrog_battery_status *battery)
{
   struct unifrog_achievements_summary summary;
   char piece[OVERLAY_LINE_MAX];
   unsigned line;
   unsigned token;

   unifrog_achievements_summary(&summary);
   for (line = 0; line < overlay.line_count; line++) {
      overlay.text[line][0] = '\0';
      for (token = 0; token < overlay.lines[line].token_count; token++) {
         overlay_token_text(overlay.lines[line].tokens[token], piece,
            sizeof(piece), battery, &summary);
         overlay_append(overlay.text[line], sizeof(overlay.text[line]), piece);
      }
   }
}

void unifrog_libretro_overlay_begin(void)
{
   overlay.need_load = 1;
   overlay.built_ms = 0;
}

int unifrog_libretro_overlay_draw(struct unifrog_surface *surface,
   const struct unifrog_battery_status *battery)
{
   uint16_t background = UNIFROG_RGB565(10, 14, 20);
   uint16_t accent = UNIFROG_RGB565(70, 150, 170);
   uint16_t color = UNIFROG_RGB565(235, 245, 250);
   uint32_t now;
   int advance;
   int line_height;
   int text_width = 0;
   int box_width;
   int box_height;
   int x;
   int y;
   int line;
   int length;

   if (!surface || !surface->pixels || !surface->stride)
      return 0;
   if (overlay.need_load)
      overlay_load();
   if (!overlay.active || overlay.line_count == 0u)
      return 0;
   now = unifrog_perf_time_ms();
   if (overlay.built_ms == 0u ||
       (uint32_t)(now - overlay.built_ms) >= OVERLAY_REFRESH_MS) {
      overlay_build(battery);
      overlay.built_ms = now ? now : 1u;
   }
   advance = unifrog_gfx_font_advance();
   line_height = unifrog_gfx_font_height() + 3;
   for (line = 0; line < (int)overlay.line_count; line++) {
      length = (int)strlen(overlay.text[line]);
      if (length * advance > text_width)
         text_width = length * advance;
   }
   if (text_width <= 0)
      return 0;
   box_width = text_width + OVERLAY_PAD * 2;
   box_height = (int)overlay.line_count * line_height + OVERLAY_PAD * 2 - 3;
   if (box_width > (int)surface->width - OVERLAY_MARGIN * 2)
      box_width = (int)surface->width - OVERLAY_MARGIN * 2;
   if (box_height <= 0 || box_height > (int)surface->height - 2)
      return 0;
   switch (overlay.position) {
   case OVERLAY_POSITION_TOP_LEFT:
      x = OVERLAY_MARGIN;
      y = OVERLAY_MARGIN;
      break;
   case OVERLAY_POSITION_BOTTOM_LEFT:
      x = OVERLAY_MARGIN;
      y = (int)surface->height - OVERLAY_MARGIN - box_height;
      break;
   case OVERLAY_POSITION_BOTTOM_RIGHT:
      x = (int)surface->width - OVERLAY_MARGIN - box_width;
      y = (int)surface->height - OVERLAY_MARGIN - box_height;
      break;
   case OVERLAY_POSITION_TOP_RIGHT:
   default:
      x = (int)surface->width - OVERLAY_MARGIN - box_width;
      y = OVERLAY_MARGIN;
      break;
   }
   if (x < 0)
      x = 0;
   if (y < 0)
      y = 0;
   unifrog_gfx_fill_rect(surface, x, y, box_width, box_height, background);
   unifrog_gfx_draw_hline(surface, x, y, box_width, accent);
   for (line = 0; line < (int)overlay.line_count; line++)
      unifrog_gfx_draw_text(surface, x + OVERLAY_PAD,
         y + OVERLAY_PAD + line * line_height, overlay.text[line], color, 1);
   /*
    * The core already flushed the game frame, so only the panel is dirty. A row
    * at a time keeps the writeback at a few kilobytes instead of the 150KB the
    * whole framebuffer would cost every frame.
    */
   for (line = 0; line < box_height; line++)
      unifrog_perf_cache_flush(surface->pixels +
         (size_t)(y + line) * surface->stride + (size_t)x,
         (size_t)box_width * 2u);
   return 1;
}
