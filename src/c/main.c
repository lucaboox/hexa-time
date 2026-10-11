#include <pebble.h>

// ---------------------------------------------------------------------------
// Hexa Time: big centered time, AM/PM, up to 6 configurable complications.
// Performance notes: one canvas layer, minute ticks only, all strings are
// formatted when their data changes (never inside the draw call), and icons
// are vectors: either primitives drawn in code ("solid") or tiny Pebble Draw
// Command images ("line art", from TimeStyle by freakified, MIT license) that
// are loaded once, scaled once, and cached.
// ---------------------------------------------------------------------------

#define KEY_SETTINGS 1
#define KEY_WEATHER  2
#define SETTINGS_VERSION 8
#define NUM_SLOTS 6
#define NA -1
#define DEG "\xc2\xb0"  // UTF-8 degree sign, escaped so file encoding can't break it

#if PBL_DISPLAY_HEIGHT >= 220
#define LARGE_SCREEN 1
#define GAP 3
#else
#define GAP 2
#endif

// Icon sizes: [solid, line art]. Line art always uses TimeStyle's native
// 25px, so the icons render exactly as designed (no scaling).
#define PDC_SIZE 25
#ifdef LARGE_SCREEN
static const uint8_t ICON_SIZES[2] = { 22, PDC_SIZE };
static const uint8_t ICON_BIG_SIZES[2] = { 30, PDC_SIZE };
#else
static const uint8_t ICON_SIZES[2] = { 16, PDC_SIZE };
static const uint8_t ICON_BIG_SIZES[2] = { 22, PDC_SIZE };
#endif

// Complication types (must match the option values in src/pkjs/config.js)
enum {
  C_NONE = 0,
  C_DATE_STACK,     // SAT / 10 / OCT
  C_DAY_DATE,       // SAT / 10
  C_WEATHER,        // icon + temp
  C_WEATHER_ICON,   // icon only
  C_WEATHER_TEMP,   // temp only
  C_WEATHER_HILO,   // H 21 / L 12
  C_BATTERY,        // icon + %
  C_BATTERY_ICON,   // icon only
  C_BATTERY_PCT,    // % only
  C_STEPS,
  C_DISTANCE,
  C_SLEEP,
  C_CALORIES,
  C_HEART,
  C_ACTIVE,
  C_BLUETOOTH,
  C_DATE_NUM,       // 10 OCT (single line)
  C_COUNT
};

// Layout kinds
enum { K_NONE, K_ICON_TEXT, K_ICON, K_TEXT, K_STACK2, K_STACK3 };

// Icons (weather icon ids match src/pkjs/index.js)
enum {
  I_CLEAR_DAY = 0, I_CLEAR_NIGHT, I_PARTLY_DAY, I_PARTLY_NIGHT, I_CLOUDY,
  I_RAIN, I_SNOW, I_THUNDER, I_FOG, I_UNKNOWN,
  I_BATTERY, I_STEPS, I_PIN, I_SLEEP, I_FLAME, I_HEART, I_TIMER, I_BT,
  I_HEAVY_RAIN, I_HEAVY_SNOW, I_SLEET,
  I_BATT_CHARGE,  // line-art charging bolt overlay
  I_COUNT
};

typedef struct {
  uint8_t version;
  GColor bg, time, ampm, text, icon;
  bool leading_zero, show_ampm, bt_vibe, temp_f, dist_mi, big_time, batt_text_color, wx_colors, center_group;
  uint8_t bar_style;         // 0 none, 1 background, 2 line, 3 background + line
  uint8_t icon_back;         // 0 none, 1 circle badge, 2 rounded square badge, 3 outline
  GColor top_bar, bot_bar, line, icon_back_color;
  uint8_t batt_colors;       // 0 = icon color, 1 = red when low, 2 = green/yellow/red by level
  uint8_t icon_style;        // 0 = line art, 1 = solid
  uint8_t icon_fill;         // line art centers: 0 = auto (like TimeStyle), 1 = custom color, 2 = see-through
  GColor icon_fill_color;
  uint8_t time_font;
  uint8_t weather_interval;  // minutes
  uint8_t slots[NUM_SLOTS];
} Settings;

typedef struct {
  int16_t temp10, hi10, lo10;  // tenths of a degree Celsius
  uint8_t icon;
  time_t ts;                   // 0 = never fetched
} Weather;

typedef struct {
  uint8_t type;
  char l1[12], l2[12], l3[8];
} Slot;

typedef struct { GFont font; int8_t off; uint8_t h; } FontM;

static Window *s_window;
static Layer *s_canvas;
static Settings s;
static Weather s_wx;
static Slot s_slots[NUM_SLOTS];
static FontM s_f_time, s_f_comp, s_f_small, s_f_ampm;

static char s_time_buf[8];
static const char *s_ampm = "AM";
static struct tm s_tm;
static bool s_show_ampm;

static BatteryChargeState s_batt;
static bool s_bt = true;
static int32_t s_steps = NA, s_dist = NA, s_sleep = NA, s_kcal = NA, s_active = NA, s_hr = NA;
static time_t s_last_wx_req;

// Cached layout
static int16_t s_time_y, s_ampm_y;
static GRect s_region[2];
static GRect s_bar[2];       // top/bottom bar areas (background fill + divider line)
static int8_t s_ibp;         // padding around icons for the badge/outline backing
static bool s_mono;          // true while drawing an icon's outline silhouette
static bool s_big_row;       // a single row is used, shown as a taller bar
static int16_t s_icon_s, s_icon_big;

static GDrawCommandImage *s_pdc[I_COUNT];
static uint8_t s_pdc_size[I_COUNT];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool slot_uses(uint8_t lo, uint8_t hi) {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (s.slots[i] >= lo && s.slots[i] <= hi) return true;
  }
  return false;
}
static bool uses_weather(void) { return slot_uses(C_WEATHER, C_WEATHER_HILO); }
static bool uses_health(void) { return slot_uses(C_STEPS, C_ACTIVE); }

static int div_round(int n, int d) { return n >= 0 ? (n + d / 2) / d : (n - d / 2) / d; }

static int temp_display(int16_t c10) {
  return s.temp_f ? div_round(c10 * 9 + 1600, 50) : div_round(c10, 10);
}

static void upcase(char *p) {
  for (; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

static void fmt_duration(char *buf, size_t n, int32_t secs) {
  if (secs < 0) { snprintf(buf, n, "--"); return; }
  int mins = secs / 60;
  snprintf(buf, n, "%dh%02d", mins / 60, mins % 60);
}

static int32_t tuple_int(Tuple *t) {
  if (t->type == TUPLE_CSTRING) return atoi(t->value->cstring);
  switch (t->length) {
    case 1: return t->type == TUPLE_INT ? t->value->int8 : t->value->uint8;
    case 2: return t->type == TUPLE_INT ? t->value->int16 : t->value->uint16;
    default: return t->value->int32;
  }
}

// ---------------------------------------------------------------------------
// Slot text (formatted only when data changes)
// ---------------------------------------------------------------------------

static uint8_t kind_of(uint8_t type) {
  switch (type) {
    case C_NONE: return K_NONE;
    case C_DATE_STACK: return K_STACK3;
    case C_DAY_DATE:
    case C_WEATHER_HILO: return K_STACK2;
    case C_WEATHER_ICON:
    case C_BATTERY_ICON:
    case C_BLUETOOTH: return K_ICON;
    case C_WEATHER_TEMP:
    case C_BATTERY_PCT:
    case C_DATE_NUM: return K_TEXT;
    default: return K_ICON_TEXT;
  }
}

static void update_slot(int i) {
  Slot *sl = &s_slots[i];
  sl->l1[0] = sl->l2[0] = sl->l3[0] = '\0';
  bool wx = s_wx.ts != 0;

  switch (sl->type) {
    case C_DATE_STACK:
      strftime(sl->l1, sizeof(sl->l1), "%a", &s_tm);
      snprintf(sl->l2, sizeof(sl->l2), "%d", s_tm.tm_mday);
      strftime(sl->l3, sizeof(sl->l3), "%b", &s_tm);
      upcase(sl->l1);
      upcase(sl->l3);
      break;
    case C_DAY_DATE:
      strftime(sl->l1, sizeof(sl->l1), "%a", &s_tm);
      snprintf(sl->l2, sizeof(sl->l2), "%d", s_tm.tm_mday);
      upcase(sl->l1);
      break;
    case C_DATE_NUM:
      strftime(sl->l1, sizeof(sl->l1), "%d %b", &s_tm);
      if (sl->l1[0] == '0') memmove(sl->l1, sl->l1 + 1, strlen(sl->l1));
      upcase(sl->l1);
      break;
    case C_WEATHER:
    case C_WEATHER_TEMP:
      if (wx) snprintf(sl->l1, sizeof(sl->l1), "%d" DEG, temp_display(s_wx.temp10));
      else snprintf(sl->l1, sizeof(sl->l1), "--");
      break;
    case C_WEATHER_HILO:
      if (wx) {
        snprintf(sl->l1, sizeof(sl->l1), "H %d" DEG, temp_display(s_wx.hi10));
        snprintf(sl->l2, sizeof(sl->l2), "L %d" DEG, temp_display(s_wx.lo10));
      } else {
        snprintf(sl->l1, sizeof(sl->l1), "H --");
        snprintf(sl->l2, sizeof(sl->l2), "L --");
      }
      break;
    case C_BATTERY:
    case C_BATTERY_PCT:
      snprintf(sl->l1, sizeof(sl->l1), "%d%%", s_batt.charge_percent);
      break;
    case C_STEPS:
      if (s_steps < 0) snprintf(sl->l1, sizeof(sl->l1), "--");
      else if (s_steps < 10000) snprintf(sl->l1, sizeof(sl->l1), "%d", (int)s_steps);
      else snprintf(sl->l1, sizeof(sl->l1), "%d.%dk", (int)(s_steps / 1000), (int)((s_steps % 1000) / 100));
      break;
    case C_DISTANCE:
      if (s_dist < 0) {
        snprintf(sl->l1, sizeof(sl->l1), "--");
      } else {
        int tenths = s.dist_mi ? (s_dist * 10) / 1609 : s_dist / 100;
        const char *u = s.dist_mi ? "mi" : "km";
        if (tenths >= 1000) snprintf(sl->l1, sizeof(sl->l1), "%d%s", tenths / 10, u);
        else snprintf(sl->l1, sizeof(sl->l1), "%d.%d%s", tenths / 10, tenths % 10, u);
      }
      break;
    case C_SLEEP:
      fmt_duration(sl->l1, sizeof(sl->l1), s_sleep);
      break;
    case C_ACTIVE:
      fmt_duration(sl->l1, sizeof(sl->l1), s_active);
      break;
    case C_CALORIES:
      if (s_kcal < 0) snprintf(sl->l1, sizeof(sl->l1), "--");
      else snprintf(sl->l1, sizeof(sl->l1), "%d", (int)s_kcal);
      break;
    case C_HEART:
      if (s_hr <= 0) snprintf(sl->l1, sizeof(sl->l1), "--");
      else snprintf(sl->l1, sizeof(sl->l1), "%d", (int)s_hr);
      break;
    default:
      break;
  }
}

static void update_slots_of(uint8_t lo, uint8_t hi) {
  bool any = false;
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (s_slots[i].type >= lo && s_slots[i].type <= hi) {
      update_slot(i);
      any = true;
    }
  }
  if (any && s_canvas) layer_mark_dirty(s_canvas);
}

static void update_all_slots(void) {
  for (int i = 0; i < NUM_SLOTS; i++) {
    s_slots[i].type = s.slots[i] < C_COUNT ? s.slots[i] : C_NONE;
    update_slot(i);
  }
}

// ---------------------------------------------------------------------------
// Icons: vector primitives on a 16-unit design grid scaled to size `sz`
// ---------------------------------------------------------------------------

#define U(v) ((v) * sz / 16)

static GColor s_ic;  // current icon color
static GColor s_bgc; // current background color (for cut-outs)

static void fill_poly(GContext *ctx, int x, int y, int sz, const int8_t *pts, int n) {
  GPoint p[10];
  for (int i = 0; i < n; i++) p[i] = GPoint(x + U(pts[i * 2]), y + U(pts[i * 2 + 1]));
  GPath path = { .num_points = n, .points = p, .rotation = 0, .offset = GPointZero };
  gpath_draw_filled(ctx, &path);
}

static void draw_cloud(GContext *ctx, int x, int y, int sz, int grow) {
  graphics_fill_circle(ctx, GPoint(x + U(9), y + U(7)), U(4) + grow);
  graphics_fill_circle(ctx, GPoint(x + U(5), y + U(10)), U(3) + grow);
  graphics_fill_circle(ctx, GPoint(x + U(12), y + U(10)), U(3) + grow);
  graphics_fill_rect(ctx, GRect(x + U(5) - grow, y + U(9) - grow, U(7) + 2 * grow, U(4) + 1 + 2 * grow), 0, GCornerNone);
}

static void draw_sun(GContext *ctx, GPoint c, int r) {
  graphics_fill_circle(ctx, c, r * 5 / 11);
  int r1 = r * 7 / 10;
  for (int i = 0; i < 8; i++) {
    int32_t a = TRIG_MAX_ANGLE * i / 8;
    int32_t sn = sin_lookup(a), cs = cos_lookup(a);
    graphics_draw_line(ctx,
      GPoint(c.x + sn * r1 / TRIG_MAX_RATIO, c.y - cs * r1 / TRIG_MAX_RATIO),
      GPoint(c.x + sn * r / TRIG_MAX_RATIO, c.y - cs * r / TRIG_MAX_RATIO));
  }
}

static void draw_moon(GContext *ctx, GPoint c, int r, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_fill_color(ctx, s_bgc);
  graphics_fill_circle(ctx, GPoint(c.x + r * 2 / 3, c.y - r / 3), r * 3 / 4);
  graphics_context_set_fill_color(ctx, col);
}

// Cloud with a background-colored halo so it reads cleanly over the sun/moon
static void draw_cloud_over(GContext *ctx, int x, int y, int sz, GColor col) {
  graphics_context_set_fill_color(ctx, s_bgc);
  draw_cloud(ctx, x, y, sz, sz >= 20 ? 2 : 1);
  graphics_context_set_fill_color(ctx, col);
  draw_cloud(ctx, x, y, sz, 0);
}

// Weather palette: real-world colors when enabled, otherwise the icon color
#ifdef PBL_COLOR
#define WXC(color) (s.wx_colors && !s_mono ? (color) : s_ic)
#else
#define WXC(color) s_ic
#endif

static void set_colors(GContext *ctx, GColor c) {
  graphics_context_set_fill_color(ctx, c);
  graphics_context_set_stroke_color(ctx, c);
}

// Battery level color: green > 50%, yellow 21-50%, red <= 20%
static GColor batt_level_color(void) {
#ifdef PBL_COLOR
  if (s_mono) return s_ic;
  uint8_t pct = s_batt.charge_percent;
  if (s.batt_colors == 2) return pct <= 20 ? GColorRed : pct <= 50 ? GColorYellow : GColorGreen;
  if (s.batt_colors == 1 && pct <= 20) return GColorRed;
#endif
  return s_ic;
}

static void draw_battery(GContext *ctx, int x, int y, int sz) {
  int t = sz >= 20 ? 2 : 1;
  GColor level = batt_level_color();
  GColor outline = s.batt_colors == 2 ? level : s_ic;
  graphics_context_set_fill_color(ctx, outline);
  GRect body = GRect(x, y + U(4), U(14), U(9));
  graphics_fill_rect(ctx, body, 1, GCornersAll);
  graphics_fill_rect(ctx, GRect(x + U(14), y + U(6), U(2), U(5)), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, s_bgc);
  graphics_fill_rect(ctx, grect_inset(body, GEdgeInsets(t)), 0, GCornerNone);

  if (s_batt.is_charging || s_batt.is_plugged) {
    static const int8_t bolt[] = { 8,5, 3,9, 7,9, 6,12, 11,8, 7,8 };
    graphics_context_set_fill_color(ctx, outline);
    fill_poly(ctx, x, y, sz, bolt, 6);
    return;
  }
  GRect inner = grect_inset(body, GEdgeInsets(t + 1));
  int w = inner.size.w * s_batt.charge_percent / 100;
  if (w < 1 && s_batt.charge_percent > 0) w = 1;
  graphics_context_set_fill_color(ctx, level);
  graphics_fill_rect(ctx, GRect(inner.origin.x, inner.origin.y, w, inner.size.h), 0, GCornerNone);
}

static void draw_icon_solid(GContext *ctx, uint8_t icon, int x, int y, int sz) {
  if (icon == I_HEAVY_RAIN) icon = I_RAIN;
  else if (icon == I_HEAVY_SNOW || icon == I_SLEET) icon = I_SNOW;
  graphics_context_set_fill_color(ctx, s_ic);
  graphics_context_set_stroke_color(ctx, s_ic);
  int sw = sz >= 20 ? 2 : PBL_IF_COLOR_ELSE(2, 1);
  graphics_context_set_stroke_width(ctx, sw);

  switch (icon) {
    case I_CLEAR_DAY:
      set_colors(ctx, WXC(GColorYellow));
      draw_sun(ctx, GPoint(x + sz / 2, y + sz / 2), sz / 2 - 1);
      break;
    case I_CLEAR_NIGHT:
      // The visible crescent sits left of the full circle, so offset it to look centered
      draw_moon(ctx, GPoint(x + U(9), y + U(8)), U(6), WXC(GColorLightGray));
      break;
    case I_PARTLY_DAY:
      set_colors(ctx, WXC(GColorYellow));
      draw_sun(ctx, GPoint(x + U(6), y + U(6)), U(6));
      draw_cloud_over(ctx, x + U(1), y + U(3), sz, WXC(GColorWhite));
      break;
    case I_PARTLY_NIGHT:
      draw_moon(ctx, GPoint(x + U(6), y + U(6)), U(5), WXC(GColorLightGray));
      draw_cloud_over(ctx, x + U(1), y + U(3), sz, WXC(GColorWhite));
      break;
    case I_CLOUDY:
      set_colors(ctx, WXC(GColorLightGray));
      draw_cloud(ctx, x, y, sz, 0);
      break;
    case I_UNKNOWN:
      set_colors(ctx, WXC(GColorLightGray));
      draw_cloud(ctx, x, y, sz, 0);
      graphics_context_set_fill_color(ctx, s_bgc);
      graphics_fill_circle(ctx, GPoint(x + U(9), y + U(9)), U(1) + 1);
      break;
    case I_RAIN:
      set_colors(ctx, WXC(GColorLightGray));
      draw_cloud(ctx, x, y - U(3), sz, 0);
      set_colors(ctx, WXC(GColorVividCerulean));
      graphics_draw_line(ctx, GPoint(x + U(6), y + U(12)), GPoint(x + U(5), y + U(15)));
      graphics_draw_line(ctx, GPoint(x + U(9), y + U(12)), GPoint(x + U(8), y + U(15)));
      graphics_draw_line(ctx, GPoint(x + U(12), y + U(12)), GPoint(x + U(11), y + U(15)));
      break;
    case I_SNOW:
      set_colors(ctx, WXC(GColorLightGray));
      draw_cloud(ctx, x, y - U(3), sz, 0);
      set_colors(ctx, WXC(GColorWhite));
      graphics_fill_circle(ctx, GPoint(x + U(5), y + U(13)), U(1));
      graphics_fill_circle(ctx, GPoint(x + U(9), y + U(15)), U(1));
      graphics_fill_circle(ctx, GPoint(x + U(12), y + U(12)), U(1));
      break;
    case I_THUNDER: {
      static const int8_t bolt[] = { 10,9, 5,13, 8,13, 6,16, 12,11, 9,11 };
      set_colors(ctx, WXC(GColorLightGray));
      draw_cloud(ctx, x, y - U(3), sz, 0);
      set_colors(ctx, WXC(GColorYellow));
      fill_poly(ctx, x, y, sz, bolt, 6);
      break;
    }
    case I_FOG:
      set_colors(ctx, WXC(GColorLightGray));
      graphics_draw_line(ctx, GPoint(x + U(2), y + U(4)), GPoint(x + U(12), y + U(4)));
      graphics_draw_line(ctx, GPoint(x + U(4), y + U(8)), GPoint(x + U(14), y + U(8)));
      graphics_draw_line(ctx, GPoint(x + U(2), y + U(12)), GPoint(x + U(12), y + U(12)));
      break;
    case I_BATTERY:
      draw_battery(ctx, x, y, sz);
      break;
    case I_STEPS:
      graphics_fill_rect(ctx, GRect(x + U(2), y + U(1), U(5), U(8)), U(2), GCornersAll);
      graphics_fill_rect(ctx, GRect(x + U(2), y + U(10), U(5), U(3)), 1, GCornersAll);
      graphics_fill_rect(ctx, GRect(x + U(9), y + U(4), U(5), U(8)), U(2), GCornersAll);
      graphics_fill_rect(ctx, GRect(x + U(9), y + U(13), U(5), U(3)), 1, GCornersAll);
      break;
    case I_PIN: {
      static const int8_t tip[] = { 3,8, 13,8, 8,16 };
      graphics_fill_circle(ctx, GPoint(x + U(8), y + U(6)), U(5));
      fill_poly(ctx, x, y, sz, tip, 3);
      graphics_context_set_fill_color(ctx, s_bgc);
      graphics_fill_circle(ctx, GPoint(x + U(8), y + U(6)), U(2));
      break;
    }
    case I_SLEEP:
      draw_moon(ctx, GPoint(x + U(6), y + U(10)), U(5), s_ic);
      graphics_context_set_stroke_width(ctx, sz >= 20 ? 2 : 1);
      graphics_draw_line(ctx, GPoint(x + U(10), y + U(1)), GPoint(x + U(15), y + U(1)));
      graphics_draw_line(ctx, GPoint(x + U(15), y + U(1)), GPoint(x + U(10), y + U(6)));
      graphics_draw_line(ctx, GPoint(x + U(10), y + U(6)), GPoint(x + U(15), y + U(6)));
      break;
    case I_FLAME: {
      static const int8_t outer[] = { 8,0, 12,5, 14,10, 12,14, 8,16, 4,14, 2,10, 4,5, 6,8 };
      static const int8_t inner[] = { 8,8, 10,11, 10,14, 6,14, 6,11 };
      fill_poly(ctx, x, y, sz, outer, 9);
      graphics_context_set_fill_color(ctx, s_bgc);
      fill_poly(ctx, x, y, sz, inner, 5);
      break;
    }
    case I_HEART: {
      static const int8_t tri[] = { 1,7, 15,7, 8,15 };
      graphics_fill_circle(ctx, GPoint(x + U(5), y + U(5)), U(4) - 1);
      graphics_fill_circle(ctx, GPoint(x + U(11), y + U(5)), U(4) - 1);
      fill_poly(ctx, x, y, sz, tri, 3);
      break;
    }
    case I_TIMER:
      graphics_draw_circle(ctx, GPoint(x + U(8), y + U(9)), U(6));
      graphics_fill_rect(ctx, GRect(x + U(6), y, U(4), U(2)), 0, GCornerNone);
      graphics_draw_line(ctx, GPoint(x + U(8), y + U(9)), GPoint(x + U(8), y + U(5)));
      graphics_draw_line(ctx, GPoint(x + U(8), y + U(9)), GPoint(x + U(11), y + U(9)));
      break;
    case I_BT:
      if (!s_bt && !s_mono) {
#ifdef PBL_COLOR
        graphics_context_set_stroke_color(ctx, GColorRed);
#endif
      }
      graphics_draw_line(ctx, GPoint(x + U(4), y + U(4)), GPoint(x + U(12), y + U(11)));
      graphics_draw_line(ctx, GPoint(x + U(12), y + U(11)), GPoint(x + U(8), y + U(15)));
      graphics_draw_line(ctx, GPoint(x + U(8), y + U(15)), GPoint(x + U(8), y + U(1)));
      graphics_draw_line(ctx, GPoint(x + U(8), y + U(1)), GPoint(x + U(12), y + U(5)));
      graphics_draw_line(ctx, GPoint(x + U(12), y + U(5)), GPoint(x + U(4), y + U(12)));
      if (!s_bt) graphics_draw_line(ctx, GPoint(x + U(1), y + U(15)), GPoint(x + U(15), y + U(1)));
      break;
  }
}

// ---------------------------------------------------------------------------
// Line-art icons (Pebble Draw Command images from TimeStyle, MIT license)
// ---------------------------------------------------------------------------

static uint32_t pdc_resource(uint8_t icon) {
  switch (icon) {
    case I_CLEAR_DAY: return RESOURCE_ID_PDC_CLEAR_DAY;
    case I_CLEAR_NIGHT: return RESOURCE_ID_PDC_CLEAR_NIGHT;
    case I_PARTLY_DAY: return RESOURCE_ID_PDC_PARTLY_CLOUDY;
    case I_PARTLY_NIGHT: return RESOURCE_ID_PDC_PARTLY_CLOUDY_NIGHT;
    case I_CLOUDY: return RESOURCE_ID_PDC_CLOUDY_DAY;
    case I_RAIN: return RESOURCE_ID_PDC_LIGHT_RAIN;
    case I_HEAVY_RAIN: return RESOURCE_ID_PDC_HEAVY_RAIN;
    case I_SNOW: return RESOURCE_ID_PDC_LIGHT_SNOW;
    case I_HEAVY_SNOW: return RESOURCE_ID_PDC_HEAVY_SNOW;
    case I_SLEET: return RESOURCE_ID_PDC_RAINING_AND_SNOWING;
    case I_THUNDER: return RESOURCE_ID_PDC_THUNDERSTORM;
    case I_UNKNOWN: return RESOURCE_ID_PDC_WEATHER_GENERIC;
    case I_BATTERY: return RESOURCE_ID_PDC_BATTERY_BG;
    case I_BATT_CHARGE: return RESOURCE_ID_PDC_BATTERY_CHARGE;
    case I_STEPS: return RESOURCE_ID_PDC_HEALTH_STEPS;
    case I_SLEEP: return RESOURCE_ID_PDC_HEALTH_SLEEP;
    case I_HEART: return RESOURCE_ID_PDC_HEALTH_HEART;
    case I_BT: return RESOURCE_ID_PDC_DISCONNECTED;
    default: return 0;
  }
}

typedef struct { int num, den; } Scale;

static bool pdc_scale_cb(GDrawCommand *cmd, uint32_t index, void *context) {
  Scale *sc = context;
  uint16_t n = gdraw_command_get_num_points(cmd);
  for (uint16_t i = 0; i < n; i++) {
    GPoint p = gdraw_command_get_point(cmd, i);
    gdraw_command_set_point(cmd, i, GPoint(p.x * sc->num / sc->den, p.y * sc->num / sc->den));
  }
  if (gdraw_command_get_type(cmd) == GDrawCommandTypeCircle) {
    gdraw_command_set_radius(cmd, gdraw_command_get_radius(cmd) * sc->num / sc->den);
  }
  uint8_t sw = gdraw_command_get_stroke_width(cmd);
  if (sw) {
    int nsw = (sw * sc->num + sc->den / 2) / sc->den;
    gdraw_command_set_stroke_width(cmd, nsw < 1 ? 1 : nsw);
  }
  return true;
}

// Loads (and scales, once) the icon at the requested size; cached until the size changes
static GDrawCommandImage *pdc_get(uint8_t icon, int sz) {
  if (s_pdc[icon] && s_pdc_size[icon] == sz) return s_pdc[icon];
  if (s_pdc[icon]) gdraw_command_image_destroy(s_pdc[icon]);
  s_pdc[icon] = NULL;
  uint32_t res = pdc_resource(icon);
  if (!res) return NULL;
  GDrawCommandImage *img = gdraw_command_image_create_with_resource(res);
  if (!img) return NULL;
  if (sz != PDC_SIZE) {
    Scale sc = { sz, PDC_SIZE };
    gdraw_command_list_iterate(gdraw_command_image_get_command_list(img), pdc_scale_cb, &sc);
    gdraw_command_image_set_bounds_size(img, GSize(sz, sz));
  }
  s_pdc[icon] = img;
  s_pdc_size[icon] = sz;
  return img;
}

static bool pdc_color_cb(GDrawCommand *cmd, uint32_t index, void *context) {
  GColor *colors = context;
  gdraw_command_set_fill_color(cmd, colors[0]);
  gdraw_command_set_stroke_color(cmd, colors[1]);
  return true;
}

static bool draw_pdc(GContext *ctx, uint8_t icon, int x, int y, int sz, GColor fill, GColor stroke) {
  GDrawCommandImage *img = pdc_get(icon, sz);
  if (!img) return false;
  GColor colors[2] = { fill, stroke };
  gdraw_command_list_iterate(gdraw_command_image_get_command_list(img), pdc_color_cb, colors);
  gdraw_command_image_draw(ctx, img, GPoint(x, y));
  return true;
}

static GColor wx_color(uint8_t icon) {
  switch (icon) {
    case I_CLEAR_DAY:
    case I_THUNDER: return WXC(GColorYellow);
    case I_PARTLY_DAY: return WXC(GColorIcterine);
    case I_RAIN:
    case I_HEAVY_RAIN: return WXC(GColorVividCerulean);
    case I_SNOW:
    case I_HEAVY_SNOW: return WXC(GColorWhite);
    case I_SLEET: return WXC(GColorCeleste);
    default: return WXC(GColorLightGray);
  }
}

// Fill (icon centers) and forced stroke for line-art icons drawn on `bg`.
// Auto matches TimeStyle: black bar -> black centers with colored strokes,
// any other bar -> white centers with black strokes.
static GColor s_lfill;
static bool s_lblack;  // strokes forced to black (auto mode on a non-black bar)

static void line_colors(GColor bg) {
  s_lfill = bg;
  s_lblack = false;
  if (s_mono) return;
  if (s.icon_fill == 0) {
    if (gcolor_equal(bg, GColorBlack)) {
      s_lfill = GColorBlack;
    } else {
      s_lfill = GColorWhite;
      s_lblack = true;
    }
  } else if (s.icon_fill == 1) {
    s_lfill = s.icon_fill_color;
  }
}

#define LSTROKE(c) (s_lblack ? GColorBlack : (c))

static void draw_icon_line(GContext *ctx, uint8_t icon, int x, int y, int sz) {
  graphics_context_set_stroke_width(ctx, sz >= 30 ? 3 : 2);
  line_colors(s_bgc);
  GColor ic = s_ic, bgc = s_bgc;
  s_ic = LSTROKE(s_ic);

  if (icon <= I_UNKNOWN || (icon >= I_HEAVY_RAIN && icon <= I_SLEET)) {
    GColor col = LSTROKE(wx_color(icon));
    if (icon == I_FOG) {
      // No line-art fog icon; the solid one is already made of lines
      s_ic = col;
      draw_icon_solid(ctx, I_FOG, x + sz / 8, y + sz / 8, sz * 3 / 4);
    } else {
      draw_pdc(ctx, icon, x, y, sz, s_lfill, col);
    }
    s_ic = ic;
    s_bgc = bgc;
    return;
  }

  switch (icon) {
    case I_BATTERY: {
      s_ic = ic;
      GColor level = batt_level_color();
      GColor outline = LSTROKE(s.batt_colors == 2 ? level : s_ic);
      if (s_lblack && s.batt_colors == 0) level = GColorBlack;
      draw_pdc(ctx, I_BATTERY, x, y, sz, s_lfill, outline);
      if (s_batt.is_charging || s_batt.is_plugged) {
        draw_pdc(ctx, I_BATT_CHARGE, x, y, sz, outline, s_lfill);  // bolt uses inverted colors
      } else {
        int pct = s_batt.charge_percent > 0 ? s_batt.charge_percent : 5;
        int w = (18 * sz / PDC_SIZE) * pct / 100;
        graphics_context_set_fill_color(ctx, level);
        graphics_fill_rect(ctx, GRect(x + 3 * sz / PDC_SIZE, y + 8 * sz / PDC_SIZE, w < 1 ? 1 : w,
                                      8 * sz / PDC_SIZE), 0, GCornerNone);
      }
      break;
    }
    case I_STEPS:
    case I_SLEEP:
    case I_HEART:
      draw_pdc(ctx, icon, x, y, sz, s_lfill, s_ic);
      break;
    case I_BT:
      if (!s_bt) {
        draw_pdc(ctx, I_BT, x, y, sz, s_lfill, s_mono ? s_ic : PBL_IF_COLOR_ELSE(GColorRed, s_ic));
      } else {
        draw_icon_solid(ctx, I_BT, x + sz / 8, y + sz / 8, sz * 3 / 4);
      }
      break;
    case I_PIN: {
      // Outline map pin to match the line-art set
      int x0 = x + sz / 8, y0 = y + sz / 16, s2 = sz * 3 / 4;
      graphics_context_set_stroke_color(ctx, s_ic);
      graphics_context_set_fill_color(ctx, s_lfill);
      GPoint c = GPoint(x0 + s2 / 2, y0 + s2 * 3 / 8);
      int r = s2 * 3 / 8;
      GPoint tip = GPoint(c.x, y0 + s2 + sz / 8);
      graphics_draw_line(ctx, GPoint(c.x - r * 4 / 5, c.y + r * 3 / 5), tip);
      graphics_draw_line(ctx, GPoint(c.x + r * 4 / 5, c.y + r * 3 / 5), tip);
      graphics_fill_circle(ctx, c, r);
      graphics_draw_circle(ctx, c, r);
      graphics_draw_circle(ctx, c, r / 3);
      break;
    }
    case I_FLAME: {
      static const int8_t outer[] = { 8,1, 12,5, 14,10, 12,14, 8,15, 4,14, 2,10, 4,5, 6,8 };
      int x0 = x + sz / 8, y0 = y + sz / 8, s2 = sz * 3 / 4;
      GPoint p[9];
      for (int i = 0; i < 9; i++) p[i] = GPoint(x0 + outer[i * 2] * s2 / 16, y0 + outer[i * 2 + 1] * s2 / 16);
      GPath path = { .num_points = 9, .points = p, .rotation = 0, .offset = GPointZero };
      graphics_context_set_fill_color(ctx, s_lfill);
      gpath_draw_filled(ctx, &path);
      graphics_context_set_stroke_color(ctx, s_ic);
      gpath_draw_outline(ctx, &path);
      break;
    }
    case I_TIMER:
      graphics_context_set_fill_color(ctx, s_lfill);
      graphics_fill_circle(ctx, GPoint(x + sz / 8 + (sz * 3 / 4) / 2, y + sz / 8 + (sz * 3 / 4) * 9 / 16),
                           (sz * 3 / 4) * 6 / 16);
      draw_icon_solid(ctx, icon, x + sz / 8, y + sz / 8, sz * 3 / 4);
      break;
    default:
      // Anything else: the solid versions are already outlines
      draw_icon_solid(ctx, icon, x + sz / 8, y + sz / 8, sz * 3 / 4);
      break;
  }
  s_ic = ic;
  s_bgc = bgc;
}

static void draw_icon(GContext *ctx, uint8_t icon, int x, int y, int sz) {
  if (s.icon_style == 0) draw_icon_line(ctx, icon, x, y, sz);
  else draw_icon_solid(ctx, icon, x, y, sz);
}

static uint8_t icon_of(uint8_t type) {
  switch (type) {
    case C_WEATHER:
    case C_WEATHER_ICON: return s_wx.ts ? s_wx.icon : I_UNKNOWN;
    case C_BATTERY:
    case C_BATTERY_ICON: return I_BATTERY;
    case C_STEPS: return I_STEPS;
    case C_DISTANCE: return I_PIN;
    case C_SLEEP: return I_SLEEP;
    case C_CALORIES: return I_FLAME;
    case C_HEART: return I_HEART;
    case C_ACTIVE: return I_TIMER;
    case C_BLUETOOTH: return I_BT;
    default: return I_UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void text_at(GContext *ctx, const char *str, const FontM *f, int cx, int y, int w) {
  graphics_draw_text(ctx, str, f->font, GRect(cx - w / 2, y - f->off, w, f->h + f->off + 8),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Draws an icon whose box (including backing padding) starts at x,y.
// `bg` is the color behind the slot, used for the icon's cut-outs.
static void draw_icon_backed(GContext *ctx, uint8_t icon, int x, int y, int sz, GColor bg) {
  int full = sz + s_ibp * 2;
  int ix = x + s_ibp, iy = y + s_ibp;
  s_ic = s.icon;
  s_bgc = bg;
  switch (s.icon_back) {
    case 1:
      graphics_context_set_fill_color(ctx, s.icon_back_color);
      graphics_fill_circle(ctx, GPoint(x + full / 2, y + full / 2), full / 2);
      s_bgc = s.icon_back_color;
      break;
    case 2:
      graphics_context_set_fill_color(ctx, s.icon_back_color);
      graphics_fill_rect(ctx, GRect(x, y, full, full), full / 4, GCornersAll);
      s_bgc = s.icon_back_color;
      break;
    case 3: {
      // Silhouette in the outline color, stamped around the icon
      static const int8_t d[8][2] = { {-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1} };
      s_mono = true;
      s_ic = s_bgc = s.icon_back_color;
      for (int i = 0; i < 8; i++) draw_icon(ctx, icon, ix + d[i][0], iy + d[i][1], sz);
      s_mono = false;
      s_ic = s.icon;
      s_bgc = bg;
      break;
    }
  }
  draw_icon(ctx, icon, ix, iy, sz);
}

// Line-art mode draws the stacked date's day number inside an outlined box
static bool date_boxed(uint8_t type) { return type == C_DATE_STACK && s.icon_style == 0; }
#define BOX_PAD 3

static int slot_height(uint8_t type) {
  switch (kind_of(type)) {
    case K_ICON_TEXT: return s_icon_s + s_ibp * 2 + GAP + s_f_comp.h;
    case K_ICON: return s_icon_big + s_ibp * 2;
    case K_TEXT: return s_f_comp.h;
    case K_STACK2: return (type == C_WEATHER_HILO ? s_f_comp.h : s_f_small.h) + GAP + 1 + s_f_comp.h;
    case K_STACK3: return s_f_small.h * 2 + s_f_comp.h + (GAP + 1 + (date_boxed(type) ? BOX_PAD + 1 : 0)) * 2;
    default: return 0;
  }
}

static int row_height(int row) {
  int h = 0;
  for (int c = 0; c < 3; c++) {
    int sh = slot_height(s.slots[row * 3 + c] < C_COUNT ? s.slots[row * 3 + c] : C_NONE);
    if (sh > h) h = sh;
  }
  return h;
}

static void draw_slot(GContext *ctx, const Slot *sl, int cx, int col_w, GRect reg, int row, bool side, GColor bg) {
  uint8_t k = kind_of(sl->type);
  int ch = slot_height(sl->type);
  if (!ch) return;

  int y = reg.origin.y + (reg.size.h - ch) / 2;
#ifdef PBL_ROUND
  // Side columns on round screens hug the time where the display is wider
  if (side) y = row == 0 ? reg.origin.y + reg.size.h - ch - 2 : reg.origin.y + 2;
#endif
  int tw = col_w + 10;
  bool batt_text = s.batt_text_color && (sl->type == C_BATTERY || sl->type == C_BATTERY_PCT);
  if (batt_text) graphics_context_set_text_color(ctx, batt_level_color());

  switch (k) {
    case K_ICON_TEXT:
      draw_icon_backed(ctx, icon_of(sl->type), cx - s_icon_s / 2 - s_ibp, y, s_icon_s, bg);
      text_at(ctx, sl->l1, &s_f_comp, cx, y + s_icon_s + s_ibp * 2 + GAP, tw);
      break;
    case K_ICON:
      draw_icon_backed(ctx, icon_of(sl->type), cx - s_icon_big / 2 - s_ibp, y, s_icon_big, bg);
      break;
    case K_TEXT:
      text_at(ctx, sl->l1, &s_f_comp, cx, y, tw);
      break;
    case K_STACK2: {
      const FontM *f1 = sl->type == C_WEATHER_HILO ? &s_f_comp : &s_f_small;
      text_at(ctx, sl->l1, f1, cx, y, tw);
      text_at(ctx, sl->l2, &s_f_comp, cx, y + f1->h + GAP + 1, tw);
      break;
    }
    case K_STACK3: {
      int sp = GAP + 1 + (date_boxed(sl->type) ? BOX_PAD + 1 : 0);
      text_at(ctx, sl->l1, &s_f_small, cx, y, tw);
      y += s_f_small.h + sp;
      bool boxed = date_boxed(sl->type);
      if (boxed) {
        int bw = s_icon_s + 2, bh = s_f_comp.h + BOX_PAD * 2;
        GRect box = GRect(cx - bw / 2, y - BOX_PAD, bw, bh);
        line_colors(bg);
        graphics_context_set_fill_color(ctx, s_lfill);
        graphics_fill_rect(ctx, box, 3, GCornersAll);
        graphics_context_set_stroke_color(ctx, LSTROKE(s.icon));
        graphics_context_set_stroke_width(ctx, 2);
        graphics_draw_round_rect(ctx, box, 3);
        if (s.icon_fill != 2) graphics_context_set_text_color(ctx, LSTROKE(s.icon));
      }
      text_at(ctx, sl->l2, &s_f_comp, cx, y, tw);
      if (boxed) graphics_context_set_text_color(ctx, s.text);
      y += s_f_comp.h + sp;
      text_at(ctx, sl->l3, &s_f_small, cx, y, tw);
      break;
    }
  }
  if (batt_text) graphics_context_set_text_color(ctx, s.text);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  s_ic = s.icon;
  s_bgc = s.bg;

  graphics_context_set_fill_color(ctx, s.bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Bars: only rows that have complications get one
  for (int row = 0; row < 2; row++) {
    if (s_bar[row].size.h <= 0) continue;
    if (s.bar_style & 1) {
      graphics_context_set_fill_color(ctx, row ? s.bot_bar : s.top_bar);
      graphics_fill_rect(ctx, s_bar[row], 0, GCornerNone);
    }
    if (s.bar_style & 2) {
      int lw = PBL_IF_COLOR_ELSE(2, 1);
      int ly = row ? s_bar[1].origin.y : s_bar[0].origin.y + s_bar[0].size.h - lw;
      graphics_context_set_fill_color(ctx, s.line);
      graphics_fill_rect(ctx, GRect(0, ly, b.size.w, lw), 0, GCornerNone);
    }
  }

  // Time
  graphics_context_set_text_color(ctx, s.time);
  graphics_draw_text(ctx, s_time_buf, s_f_time.font,
                     GRect(0, s_time_y - s_f_time.off, b.size.w, s_f_time.h + s_f_time.off + 14),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  if (s_show_ampm) {
    graphics_context_set_text_color(ctx, s.ampm);
    text_at(ctx, s_ampm, &s_f_ampm, b.size.w / 2, s_ampm_y, b.size.w);
  }

  // Complications: active slots in each row share the row width evenly
  graphics_context_set_text_color(ctx, s.text);
  int inset = PBL_IF_ROUND_ELSE(b.size.w / 8, 0);
  int row_w = b.size.w - inset * 2;
  for (int row = 0; row < 2; row++) {
    const Slot *active[3];
    int n = 0;
    for (int c = 0; c < 3; c++) {
      const Slot *sl = &s_slots[row * 3 + c];
      if (sl->type != C_NONE) active[n++] = sl;
    }
    if (!n) continue;
    GColor row_bg = (s.bar_style & 1) ? (row ? s.bot_bar : s.top_bar) : s.bg;
    int col_w = row_w / n;
    for (int c = 0; c < n; c++) {
      int cx = inset + col_w * c + col_w / 2;
      bool side = n > 1 && (c == 0 || c == n - 1);
      draw_slot(ctx, active[c], cx, col_w, s_region[row], row, side, row_bg);
    }
  }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// Big custom font used when a whole complication row is empty
#if defined(PBL_PLATFORM_GABBRO)
#define BIG_FONT RESOURCE_ID_FONT_BIG_82
#define BIG_OFF 25
#define BIG_H 70
#elif defined(LARGE_SCREEN)
#define BIG_FONT RESOURCE_ID_FONT_BIG_80
#define BIG_OFF 21
#define BIG_H 59
#else
#define BIG_FONT RESOURCE_ID_FONT_BIG_58
#define BIG_OFF 14
#define BIG_H 40
#endif

static GFont s_big_font;

static void load_time_font(bool big) {
  if (big) {
    if (!s_big_font) s_big_font = fonts_load_custom_font(resource_get_handle(BIG_FONT));
    s_f_time = (FontM){ s_big_font, BIG_OFF, BIG_H };
    return;
  }
  if (s_big_font) {
    fonts_unload_custom_font(s_big_font);
    s_big_font = NULL;
  }
  switch (s.time_font) {
    case 1: s_f_time = (FontM){ fonts_get_system_font(FONT_KEY_BITHAM_42_LIGHT), 11, 30 }; break;
    case 2: s_f_time = (FontM){ fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS), 7, 30 }; break;
    case 3: s_f_time = (FontM){ fonts_get_system_font(FONT_KEY_ROBOTO_BOLD_SUBSET_49), 13, 35 }; break;
    case 4: s_f_time = (FontM){ fonts_get_system_font(FONT_KEY_LECO_36_BOLD_NUMBERS), 6, 26 }; break;
    default: s_f_time = (FontM){ fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD), 11, 30 }; break;
  }
}

static bool row_used(int row) {
  for (int c = 0; c < 3; c++) {
    uint8_t t = s.slots[row * 3 + c];
    if (t != C_NONE && t < C_COUNT) return true;
  }
  return false;
}

// Complication fonts (TimeStyle's sidebar sizes) and icon sizes.
// `big` = the single-row layout: a taller bar with the time moved aside.
static void set_sizes(bool big) {
  s_big_row = big;
  int line = s.icon_style == 0;
  s_icon_s = ICON_SIZES[line];
  s_icon_big = ICON_BIG_SIZES[line];
#ifdef LARGE_SCREEN
  s_f_comp = (FontM){ fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), 9, 16 };
  s_f_small = (FontM){ fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), 6, 12 };
#else
  s_f_comp = (FontM){ fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), 6, 12 };
  s_f_small = (FontM){ fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), 4, 10 };
#endif
}

static void compute_layout(void) {
  if (!s_canvas) return;
  GRect b = layer_get_bounds(s_canvas);
  s_show_ampm = s.show_ampm && !clock_is_24h_style();

  // One row on a rectangular screen becomes a taller bar with bigger
  // complications; the time moves into the remaining space
  bool single_row = row_used(0) != row_used(1);
  (void)single_row;
  set_sizes(PBL_IF_ROUND_ELSE(false, single_row && !s.center_group));
  s_ibp = s.icon_back == 3 ? 1 : s.icon_back ? (s_icon_s >= 20 ? 4 : 3) : 0;
  int top_h = row_height(0), bot_h = row_height(1);
  load_time_font(s.big_time && !(top_h && bot_h));

  int block_h = s_f_time.h + (s_show_ampm ? GAP * 3 + s_f_ampm.h : 0);
  int pad = GAP * 2;
  int edge = PBL_IF_ROUND_ELSE(b.size.h / 12, 2);

  if (s_big_row) {
    int m = GAP * 3;
    int top;
    if (bot_h) {
      s_region[1] = GRect(0, b.size.h - edge - m - bot_h, b.size.w, bot_h);
      int bar_y = s_region[1].origin.y - m;
      s_bar[0] = GRect(0, 0, 0, 0);
      s_bar[1] = GRect(0, bar_y, b.size.w, b.size.h - bar_y);
      top = (bar_y - block_h) / 2;
    } else {
      s_region[0] = GRect(0, edge + m, b.size.w, top_h);
      int bar_e = s_region[0].origin.y + top_h + m;
      s_bar[0] = GRect(0, 0, b.size.w, bar_e);
      s_bar[1] = GRect(0, 0, 0, 0);
      top = bar_e + (b.size.h - bar_e - block_h) / 2;
    }
    s_time_y = top;
    s_ampm_y = top + s_f_time.h + GAP * 3;
    return;
  }

  int top;
  if ((top_h && bot_h) || ((top_h || bot_h) && !s.center_group)) {
    // Time sits dead center; used rows go above/below it
    top = (b.size.h - block_h) / 2;
    s_region[0] = GRect(0, edge, b.size.w, top - pad - edge);
    int bot = top + block_h + pad * 2;
    s_region[1] = GRect(0, bot, b.size.w, b.size.h - edge - bot);
  } else if (top_h) {
    // Only the top row: center the row + time together as one group
    int gap = pad * 3;
    int y = (b.size.h - (top_h + gap + block_h)) / 2;
    s_region[0] = GRect(0, y, b.size.w, top_h);
    top = y + top_h + gap;
  } else if (bot_h) {
    int gap = pad * 3;
    top = (b.size.h - (block_h + gap + bot_h)) / 2;
    s_region[1] = GRect(0, top + block_h + gap, b.size.w, bot_h);
  } else {
    top = (b.size.h - block_h) / 2;
  }
  s_time_y = top;
  s_ampm_y = top + s_f_time.h + GAP * 3;

  // Bars extend from the screen edge to just past their row's content,
  // never closer than GAP*2 to the time block
  int gap2 = GAP * 2, block_bot = top + block_h;
  if (top_h) {
    int e = s_region[0].origin.y + s_region[0].size.h + gap2;
    if (e > top - gap2) e = top - gap2;
    s_bar[0] = GRect(0, 0, b.size.w, e);
  } else {
    s_bar[0] = GRect(0, 0, 0, 0);
  }
  if (bot_h) {
    int e = s_region[1].origin.y - gap2;
    if (e < block_bot + gap2) e = block_bot + gap2;
    s_bar[1] = GRect(0, e, b.size.w, b.size.h - e);
  } else {
    s_bar[1] = GRect(0, 0, 0, 0);
  }
}

// ---------------------------------------------------------------------------
// Data sources
// ---------------------------------------------------------------------------

static void update_time(void) {
  int hour = s_tm.tm_hour;
  if (!clock_is_24h_style()) {
    s_ampm = hour < 12 ? "AM" : "PM";
    hour %= 12;
    if (hour == 0) hour = 12;
  }
  snprintf(s_time_buf, sizeof(s_time_buf), s.leading_zero ? "%02d:%02d" : "%d:%02d", hour, s_tm.tm_min);
}

static void request_weather(void) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) return;
  dict_write_uint8(it, MESSAGE_KEY_RequestWeather, 1);
  app_message_outbox_send();
  s_last_wx_req = time(NULL);
}

static void maybe_request_weather(void) {
  if (!uses_weather() || !s_bt) return;
  time_t now = time(NULL);
  int interval = (s.weather_interval ? s.weather_interval : 30) * 60;
  if (now - s_wx.ts >= interval && now - s_last_wx_req >= 600) request_weather();
}

#if defined(PBL_HEALTH)
static int32_t health_sum(HealthMetric m) {
  time_t start = time_start_of_today(), end = time(NULL);
  if (!(health_service_metric_accessible(m, start, end) & HealthServiceAccessibilityMaskAvailable)) return NA;
  return health_service_sum_today(m);
}

static void refresh_health(void) {
  if (!uses_health()) return;
  int32_t steps = slot_uses(C_STEPS, C_STEPS) ? health_sum(HealthMetricStepCount) : NA;
  int32_t dist = slot_uses(C_DISTANCE, C_DISTANCE) ? health_sum(HealthMetricWalkedDistanceMeters) : NA;
  int32_t sleep = slot_uses(C_SLEEP, C_SLEEP) ? health_sum(HealthMetricSleepSeconds) : NA;
  int32_t kcal = slot_uses(C_CALORIES, C_CALORIES) ? health_sum(HealthMetricActiveKCalories) : NA;
  int32_t active = slot_uses(C_ACTIVE, C_ACTIVE) ? health_sum(HealthMetricActiveSeconds) : NA;
  int32_t hr = NA;
#if PBL_API_EXISTS(health_service_peek_current_value)
  if (slot_uses(C_HEART, C_HEART)) hr = (int32_t)health_service_peek_current_value(HealthMetricHeartRateBPM);
#endif
  if (steps != s_steps) { s_steps = steps; update_slots_of(C_STEPS, C_STEPS); }
  if (dist != s_dist) { s_dist = dist; update_slots_of(C_DISTANCE, C_DISTANCE); }
  if (sleep != s_sleep) { s_sleep = sleep; update_slots_of(C_SLEEP, C_SLEEP); }
  if (kcal != s_kcal) { s_kcal = kcal; update_slots_of(C_CALORIES, C_CALORIES); }
  if (active != s_active) { s_active = active; update_slots_of(C_ACTIVE, C_ACTIVE); }
  if (hr != s_hr) { s_hr = hr; update_slots_of(C_HEART, C_HEART); }
}

static void health_handler(HealthEventType event, void *context) {
  if (event != HealthEventMetricAlert) refresh_health();
}

static void subscribe_health(void) {
  static bool subscribed;
  bool want = uses_health();
  if (want && !subscribed) subscribed = health_service_events_subscribe(health_handler, NULL);
  else if (!want && subscribed) { health_service_events_unsubscribe(); subscribed = false; }
}
#else
static void refresh_health(void) {}
static void subscribe_health(void) {}
#endif

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  s_tm = *tick_time;
  if (s_show_ampm != (s.show_ampm && !clock_is_24h_style())) compute_layout();  // 12/24h changed
  update_time();
  if (units_changed & DAY_UNIT) {
    update_slots_of(C_DATE_STACK, C_DAY_DATE);
    update_slots_of(C_DATE_NUM, C_DATE_NUM);
    refresh_health();  // daily totals reset at midnight
  }
  maybe_request_weather();
  layer_mark_dirty(s_canvas);
}

static void battery_handler(BatteryChargeState state) {
  s_batt = state;
  update_slots_of(C_BATTERY, C_BATTERY_PCT);
}

static void bt_handler(bool connected) {
  if (!connected && s_bt && s.bt_vibe) vibes_double_pulse();
  s_bt = connected;
  update_slots_of(C_BLUETOOTH, C_BLUETOOTH);
  if (connected) maybe_request_weather();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static void default_settings(void) {
  s = (Settings){
    .version = SETTINGS_VERSION,
    .bg = GColorBlack,
    .time = GColorWhite,
    .ampm = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite),
    .text = GColorWhite,
    .icon = PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite),
    .leading_zero = false,
    .show_ampm = true,
    .bt_vibe = true,
    .temp_f = false,
    .dist_mi = false,
    .batt_colors = 2,
    .batt_text_color = false,
    .wx_colors = true,
    .center_group = false,
    .bar_style = 0,
    .icon_style = 0,
    .icon_fill = 0,
    .icon_fill_color = GColorWhite,
    .icon_back = 0,
    .top_bar = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack),
    .bot_bar = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack),
    .line = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite),
    .icon_back_color = GColorBlack,
    .big_time = true,
#ifdef LARGE_SCREEN
    .time_font = 3,
#else
    .time_font = 0,
#endif
    .weather_interval = 30,
    .slots = { C_WEATHER, C_DATE_STACK, C_BATTERY, C_STEPS, C_SLEEP, C_HEART },
  };
}

static void load_settings(void) {
  default_settings();
  Settings tmp;
  if (persist_read_data(KEY_SETTINGS, &tmp, sizeof(tmp)) == (int)sizeof(tmp) && tmp.version == SETTINGS_VERSION) {
    s = tmp;
  }
  if (persist_read_data(KEY_WEATHER, &s_wx, sizeof(s_wx)) != (int)sizeof(s_wx)) {
    memset(&s_wx, 0, sizeof(s_wx));
  }
}

static void apply_settings(void) {
  window_set_background_color(s_window, s.bg);
  compute_layout();
  update_time();
  update_all_slots();
  subscribe_health();
  refresh_health();
  maybe_request_weather();
  layer_mark_dirty(s_canvas);
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool changed = false;

#define READ_COLOR(key, field) \
  if ((t = dict_find(iter, MESSAGE_KEY_##key))) { s.field = GColorFromHEX(tuple_int(t)); changed = true; }
#define READ_BOOL(key, field) \
  if ((t = dict_find(iter, MESSAGE_KEY_##key))) { s.field = tuple_int(t) != 0; changed = true; }
#define READ_U8(key, field) \
  if ((t = dict_find(iter, MESSAGE_KEY_##key))) { s.field = (uint8_t)tuple_int(t); changed = true; }

  READ_COLOR(BgColor, bg)
  READ_COLOR(TimeColor, time)
  READ_COLOR(AmPmColor, ampm)
  READ_COLOR(CompTextColor, text)
  READ_COLOR(CompIconColor, icon)
  READ_BOOL(LeadingZero, leading_zero)
  READ_BOOL(ShowAmPm, show_ampm)
  READ_BOOL(BtVibe, bt_vibe)
  READ_BOOL(TempUnit, temp_f)
  READ_BOOL(DistUnit, dist_mi)
  READ_U8(BattColors, batt_colors)
  READ_BOOL(BattTextColor, batt_text_color)
  READ_BOOL(WeatherColors, wx_colors)
  READ_BOOL(BigTime, big_time)
  READ_BOOL(CenterGroup, center_group)
  READ_U8(BarStyle, bar_style)
  READ_U8(IconBack, icon_back)
  READ_U8(IconStyle, icon_style)
  READ_U8(IconFill, icon_fill)
  READ_COLOR(IconFillColor, icon_fill_color)
  READ_COLOR(TopBarColor, top_bar)
  READ_COLOR(BottomBarColor, bot_bar)
  READ_COLOR(LineColor, line)
  READ_COLOR(IconBackColor, icon_back_color)
  READ_U8(TimeFont, time_font)
  READ_U8(WeatherInterval, weather_interval)
  READ_U8(Slot0, slots[0])
  READ_U8(Slot1, slots[1])
  READ_U8(Slot2, slots[2])
  READ_U8(Slot3, slots[3])
  READ_U8(Slot4, slots[4])
  READ_U8(Slot5, slots[5])

  if ((t = dict_find(iter, MESSAGE_KEY_WeatherTemp))) {
    s_wx.temp10 = tuple_int(t);
    if ((t = dict_find(iter, MESSAGE_KEY_WeatherHi))) s_wx.hi10 = tuple_int(t);
    if ((t = dict_find(iter, MESSAGE_KEY_WeatherLo))) s_wx.lo10 = tuple_int(t);
    if ((t = dict_find(iter, MESSAGE_KEY_WeatherIcon))) s_wx.icon = tuple_int(t);
    s_wx.ts = time(NULL);
    persist_write_data(KEY_WEATHER, &s_wx, sizeof(s_wx));
    update_slots_of(C_WEATHER, C_WEATHER_HILO);
  }

  if (changed) {
    persist_write_data(KEY_SETTINGS, &s, sizeof(s));
    apply_settings();
  }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

static void init(void) {
  load_settings();

#ifdef LARGE_SCREEN
  s_f_ampm = (FontM){ fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), 6, 12 };
#else
  s_f_ampm = (FontM){ fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), 4, 10 };
#endif
  set_sizes(false);

  time_t now = time(NULL);
  s_tm = *localtime(&now);
  s_batt = battery_state_service_peek();
  s_bt = connection_service_peek_pebble_app_connection();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, false);

  app_message_register_inbox_received(inbox_received);
  app_message_open(512, 64);

  apply_settings();

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  connection_service_subscribe((ConnectionHandlers){ .pebble_app_connection_handler = bt_handler });
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
#if defined(PBL_HEALTH)
  health_service_events_unsubscribe();
#endif
  window_destroy(s_window);
  if (s_big_font) fonts_unload_custom_font(s_big_font);
  for (int i = 0; i < I_COUNT; i++) {
    if (s_pdc[i]) gdraw_command_image_destroy(s_pdc[i]);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
