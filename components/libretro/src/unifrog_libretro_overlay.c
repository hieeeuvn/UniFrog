/*
 * Game overlay paks.
 *
 * A pak normally runs a JS2300 script, but the script runtime is either
 * standalone, the frontend, or the core itself, so a third party cannot own a
 * frame while somebody else's core runs. An overlay pak is therefore a folder
 * under /unifrog_data/apps that only *describes* what to draw, and the libretro
 * host draws it natively over the presented game frame.
 *
 * Two shapes, both optional and combinable:
 *
 *   image=frame.png        a bezel: a full-screen PNG whose transparent middle
 *                          lets the game show through, like a GBA or GB border
 *   position=top-right     a small text panel
 *   line1=clock
 *   line2=battery,achievements
 *
 * Tokens: clock, battery, achievements, game, points. Up to four lines, three
 * tokens per line, one space between tokens.
 *
 * The active pak is chosen on the device (Custom -> Game Overlay) and stored as
 * overlay_pak in unifrog.ini; "off" or a missing pak leaves the game untouched.
 *
 * The bezel is pre-scaled once at load into opaque pixel runs, because a full
 * PNG blend per frame costs more than the frame budget on this hardware. Only
 * runs are copied each frame, and only the touched rows are written back.
 *
 * See docs/app-paks.md.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <unifrog/png.h>
#include <unifrog/text.h>

#define OVERLAY_MAX_LINES 4u
#define OVERLAY_TOKENS_PER_LINE 3u
#define OVERLAY_LINE_MAX 48u
#define OVERLAY_ID_MAX 32u
#define OVERLAY_PATH_MAX 256u
#define OVERLAY_REFRESH_MS 500u
#define OVERLAY_MARGIN 6
#define OVERLAY_PAD 5
/* Below this alpha a bezel pixel is a hole; blending per frame is too slow. */
#define OVERLAY_ALPHA_VISIBLE 128u

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

/* One horizontal run of bezel pixels, in screen coordinates. */
struct overlay_span {
   uint16_t y;
   uint16_t x;
   uint16_t length;
};

static struct {
   int need_load;
   int active;
   enum overlay_position position;
   unsigned line_count;
   struct overlay_line lines[OVERLAY_MAX_LINES];
   char id[OVERLAY_ID_MAX];
   char image_name[OVERLAY_PATH_MAX];
   uint32_t built_ms;
   char text[OVERLAY_MAX_LINES][OVERLAY_LINE_MAX];
   /* Bezel, built once for one screen size. */
   uint16_t *frame_pixels;
   struct overlay_span *frame_spans;
   unsigned frame_span_count;
   unsigned frame_pixel_count;
   unsigned frame_width;
   unsigned frame_height;
   int frame_box_x;
   int frame_box_y;
   int frame_box_w;
   int frame_box_h;
} overlay;

static const char *overlay_trim(const char *value)
{
   static char buffer[OVERLAY_PATH_MAX];

   if (!value)
      return "";
   while (*value == ' ' || *value == '\t')
      value++;
   unifrog_text_copy(buffer, sizeof(buffer), value);
   while (buffer[0] && buffer[strlen(buffer) - 1u] == ' ')
      buffer[strlen(buffer) - 1u] = '\0';
   return buffer;
}

/* Pak folder names and pak-relative paths must stay inside the pak. */
static int overlay_rel_ok(const char *path)
{
   if (!path || !path[0] || path[0] == '/')
      return 0;
   if (strstr(path, "..") || strchr(path, '\\') || strchr(path, ':'))
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
             line->token_count < OVERLAY_TOKENS_PER_LINE) {
            memcpy(piece, value + start, length);
            piece[length] = '\0';
            token = overlay_token_parse(overlay_trim(piece));
            if (token != OVERLAY_TOKEN_NONE)
               line->tokens[line->token_count++] = token;
         }
         if (ch == '\0')
            break;
         start += length + 1u;
         length = 0;
      } else {
         length++;
         if (length >= sizeof(piece)) {
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

   if (strcasecmp(key, "image") == 0) {
      unifrog_text_copy(overlay.image_name, sizeof(overlay.image_name),
         overlay_trim(value));
      return 0;
   }
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

static void overlay_release_frame(void)
{
   free(overlay.frame_pixels);
   free(overlay.frame_spans);
   overlay.frame_pixels = NULL;
   overlay.frame_spans = NULL;
   overlay.frame_span_count = 0;
   overlay.frame_pixel_count = 0;
   overlay.frame_box_x = 0;
   overlay.frame_box_y = 0;
   overlay.frame_box_w = 0;
   overlay.frame_box_h = 0;
}

static unsigned overlay_image_alpha(const struct unifrog_png_image *image,
   unsigned sy, unsigned sx)
{
   size_t index;

   if (!image->alpha)
      return 255;
   index = (size_t)sy * image->width + sx;
   return image->alpha[index];
}

/*
 * Flatten the bezel into spans of opaque pixels for the current screen size.
 * A transparent pixel stays game, which is what makes the middle a window.
 */
static int overlay_build_frame(const struct unifrog_surface *surface,
   const struct unifrog_png_image *image)
{
   struct overlay_span *spans;
   uint16_t *pixels;
   unsigned span_capacity;
   unsigned span_count = 0;
   unsigned pixel_count = 0;
   unsigned y;
   unsigned x;
   int box_x = (int)surface->width;
   int box_y = (int)surface->height;
   int box_right = 0;
   int box_bottom = 0;

   if (!surface || !image || !image->pixels || image->width == 0 ||
       image->height == 0 || surface->width == 0 || surface->height == 0)
      return -1;
   span_capacity = surface->height * 4u + 16u;
   spans = calloc(span_capacity, sizeof(*spans));
   if (!spans)
      return -1;
   for (y = 0; y < surface->height; y++) {
      unsigned sy = (unsigned)(((uint64_t)y * image->height) /
         surface->height);

      if (sy >= image->height)
         sy = image->height - 1u;
      for (x = 0; x < surface->width; x++) {
         unsigned sx = (unsigned)(((uint64_t)x * image->width) /
            surface->width);
         unsigned start;

         if (sx >= image->width)
            sx = image->width - 1u;
         if (overlay_image_alpha(image, sy, sx) < OVERLAY_ALPHA_VISIBLE)
            continue;
         start = x;
         while (x + 1u < surface->width) {
            unsigned next = (unsigned)(((uint64_t)(x + 1u) * image->width) /
               surface->width);

            if (next >= image->width)
               next = image->width - 1u;
            if (overlay_image_alpha(image, sy, next) < OVERLAY_ALPHA_VISIBLE)
               break;
            x++;
         }
         pixel_count += x - start + 1u;
         if (span_count < span_capacity) {
            spans[span_count].y = (uint16_t)y;
            spans[span_count].x = (uint16_t)start;
            spans[span_count].length = (uint16_t)(x - start + 1u);
            span_count++;
         }
         if ((int)start < box_x)
            box_x = (int)start;
         if ((int)(x + 1u) > box_right)
            box_right = (int)(x + 1u);
         if ((int)y < box_y)
            box_y = (int)y;
         box_bottom = (int)y + 1;
      }
   }
   if (span_count == 0) {
      free(spans);
      return -1;
   }
   pixels = malloc((size_t)pixel_count * sizeof(*pixels));
   if (!pixels) {
      free(spans);
      return -1;
   }
   {
      unsigned cursor = 0;
      unsigned index;

      for (index = 0; index < span_count; index++) {
         unsigned sy = (unsigned)(((uint64_t)spans[index].y * image->height) /
            surface->height);

         if (sy >= image->height)
            sy = image->height - 1u;
         for (x = 0; x < spans[index].length; x++) {
            unsigned sx = (unsigned)(((uint64_t)(spans[index].x + x) *
               image->width) / surface->width);

            if (sx >= image->width)
               sx = image->width - 1u;
            pixels[cursor++] = image->pixels[(size_t)sy * image->width + sx];
         }
      }
   }
   overlay.frame_spans = spans;
   overlay.frame_pixels = pixels;
   overlay.frame_span_count = span_count;
   overlay.frame_pixel_count = pixel_count;
   overlay.frame_width = surface->width;
   overlay.frame_height = surface->height;
   overlay.frame_box_x = box_x;
   overlay.frame_box_y = box_y;
   overlay.frame_box_w = box_right - box_x;
   overlay.frame_box_h = box_bottom - box_y;
   return 0;
}

static void overlay_load(const struct unifrog_surface *surface)
{
   char id[OVERLAY_ID_MAX];
   char manifest[OVERLAY_PATH_MAX];
   char image_path[OVERLAY_PATH_MAX];
   struct unifrog_png_image image;

   overlay.need_load = 0;
   overlay.active = 0;
   overlay.line_count = 0;
   overlay.built_ms = 0;
   overlay.id[0] = '\0';
   overlay.image_name[0] = '\0';
   overlay_release_frame();
   id[0] = '\0';
   if (unifrog_config_read(UNIFROG_CONFIG_PATH, overlay_pak_key, id, NULL) != 0)
      return;
   if (!id[0] || strcasecmp(id, "off") == 0 || !overlay_rel_ok(id))
      return;
   if (snprintf(manifest, sizeof(manifest), "%s/%s/overlay.ini",
       UNIFROG_APP_ROOT, id) >= (int)sizeof(manifest))
      return;
   if (unifrog_config_read(manifest, overlay_manifest_key, NULL, NULL) != 0) {
      unifrog_log("libretro overlay pak skipped name=%s reason=no_manifest\n",
         id);
      return;
   }

   if (overlay.image_name[0]) {
      memset(&image, 0, sizeof(image));
      if (!overlay_rel_ok(overlay.image_name) ||
          snprintf(image_path, sizeof(image_path), "%s/%s/%s",
             UNIFROG_APP_ROOT, id, overlay.image_name) >=
             (int)sizeof(image_path)) {
         unifrog_log("libretro overlay pak name=%s reason=bad_image_path\n",
            id);
         overlay.image_name[0] = '\0';
      } else if (unifrog_png_load_file(image_path, &image) != 0) {
         unifrog_log("libretro overlay pak name=%s image=%s reason=no_image\n",
            id, overlay.image_name);
         overlay.image_name[0] = '\0';
      } else {
         if (overlay_build_frame(surface, &image) != 0)
            unifrog_log("libretro overlay pak name=%s image=%s "
               "reason=empty_image\n", id, overlay.image_name);
         unifrog_png_free(&image);
      }
   }

   if (!overlay.frame_span_count && overlay.line_count == 0u) {
      unifrog_log("libretro overlay pak skipped name=%s reason=no_content\n",
         id);
      return;
   }
   unifrog_text_copy(overlay.id, sizeof(overlay.id), id);
   overlay.active = 1;
   unifrog_log("libretro overlay pak name=%s image=%s spans=%u lines=%u "
      "position=%u\n", overlay.id,
      overlay.image_name[0] ? overlay.image_name : "none",
      overlay.frame_span_count, overlay.line_count,
      (unsigned)overlay.position);
}

void unifrog_libretro_overlay_begin(void)
{
   overlay.need_load = 1;
   overlay.built_ms = 0;
}

void unifrog_libretro_overlay_end(void)
{
   overlay_release_frame();
   overlay.active = 0;
   overlay.need_load = 1;
   overlay.id[0] = '\0';
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

   if (overlay.line_count == 0u)
      return;
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

/* Write back only the rectangle we painted; the core already flushed the frame. */
static void overlay_flush_rows(const struct unifrog_surface *surface, int x,
   int y, int w, int h)
{
   int row;

   for (row = 0; row < h; row++)
      unifrog_perf_cache_flush(surface->pixels +
         (size_t)(y + row) * surface->stride + (size_t)x, (size_t)w * 2u);
}

static void overlay_draw_frame(struct unifrog_surface *surface)
{
   unsigned index;
   unsigned cursor = 0;

   for (index = 0; index < overlay.frame_span_count; index++) {
      const struct overlay_span *span = &overlay.frame_spans[index];
      uint16_t *row;

      if (span->y >= surface->height || span->x >= surface->width)
         continue;
      row = surface->pixels + (size_t)span->y * surface->stride + span->x;
      memcpy(row, overlay.frame_pixels + cursor,
         (size_t)span->length * sizeof(uint16_t));
      /* Writes back exactly what was painted, not the bezel bounding box. */
      unifrog_perf_cache_flush(row,
         (size_t)span->length * sizeof(uint16_t));
      cursor += span->length;
   }
}

static void overlay_draw_panel(struct unifrog_surface *surface)
{
   uint16_t background = UNIFROG_RGB565(10, 14, 20);
   uint16_t accent = UNIFROG_RGB565(70, 150, 170);
   uint16_t color = UNIFROG_RGB565(235, 245, 250);
   int advance;
   int line_height;
   int text_width = 0;
   int box_width;
   int box_height;
   int x;
   int y;
   int line;
   int length;

   advance = unifrog_gfx_font_advance();
   line_height = unifrog_gfx_font_height() + 3;
   for (line = 0; line < (int)overlay.line_count; line++) {
      length = (int)strlen(overlay.text[line]);
      if (length * advance > text_width)
         text_width = length * advance;
   }
   if (text_width <= 0)
      return;
   box_width = text_width + OVERLAY_PAD * 2;
   box_height = (int)overlay.line_count * line_height + OVERLAY_PAD * 2 - 3;
   if (box_width > (int)surface->width - OVERLAY_MARGIN * 2)
      box_width = (int)surface->width - OVERLAY_MARGIN * 2;
   if (box_height <= 0 || box_height > (int)surface->height - 2)
      return;
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
   overlay_flush_rows(surface, x, y, box_width, box_height);
}

int unifrog_libretro_overlay_draw(struct unifrog_surface *surface,
   const struct unifrog_battery_status *battery)
{
   uint32_t now;

   if (!surface || !surface->pixels || !surface->stride)
      return 0;
   if (overlay.need_load || (overlay.frame_span_count &&
       (surface->width != overlay.frame_width ||
        surface->height != overlay.frame_height)))
      overlay_load(surface);
   if (!overlay.active)
      return 0;
   if (overlay.line_count > 0u) {
      now = unifrog_perf_time_ms();
      if (overlay.built_ms == 0u ||
          (uint32_t)(now - overlay.built_ms) >= OVERLAY_REFRESH_MS) {
         overlay_build(battery);
         overlay.built_ms = now ? now : 1u;
      }
   }
   if (overlay.frame_span_count)
      overlay_draw_frame(surface);
   if (overlay.line_count > 0u)
      overlay_draw_panel(surface);
   return 1;
}
