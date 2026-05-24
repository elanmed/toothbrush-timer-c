#include <pebble.h>

#define BRUSH_DURATION 120
#define WAIT_DURATION 1800
#define SIDEBAR_WIDTH 30
#define ICON_SLOT_COUNT 3
#define AUTO_ADVANCE_DELAY_MS 4000
#define PLAY_START_DELAY_MS 1000

typedef enum { TIMER_STATE_PAUSED, TIMER_STATE_PLAYING } TimerState;

typedef struct {
  TimerState timer_state;
  int remaining_sec;
  int duration_sec;
} AppState;

static AppState s_state;
static Window *s_main_window;
static TextLayer *s_timer_label;
static Layer *s_sidebar_layer;
static BitmapLayer *s_reset_icon_layer;
static BitmapLayer *s_toggle_icon_layer;
static BitmapLayer *s_play_pause_icon_layer;
static GBitmap *s_play_bitmap;
static GBitmap *s_pause_bitmap;
static GBitmap *s_reset_bitmap;
static GBitmap *s_toggle_bitmap;
static AppTimer *s_play_start_delay_timer;
static AppTimer *s_auto_advance_timer;
static char s_timer_text[8];

static void play_timer(void);
static void toggle_timer(void);

static void format_remaining_sec(void) {
  int minutes = s_state.remaining_sec / 60;
  int seconds = s_state.remaining_sec % 60;
  if (minutes > 0) {
    snprintf(s_timer_text, sizeof(s_timer_text), "%d:%02d", minutes, seconds);
  } else {
    snprintf(s_timer_text, sizeof(s_timer_text), "%02d", seconds);
  }
}

static void sync_display(void) {
  format_remaining_sec();
  text_layer_set_text(s_timer_label, s_timer_text);
}

static void set_reset_icon_visible(bool visible) {
  layer_set_hidden(bitmap_layer_get_layer(s_reset_icon_layer), !visible);
}

static void set_toggle_icon_visible(bool visible) {
  layer_set_hidden(bitmap_layer_get_layer(s_toggle_icon_layer), !visible);
}

static void set_play_pause_icon_visible(bool visible) {
  layer_set_hidden(bitmap_layer_get_layer(s_play_pause_icon_layer), !visible);
}

static void show_play_icon(void) {
  bitmap_layer_set_bitmap(s_play_pause_icon_layer, s_play_bitmap);
}

static void show_pause_icon(void) {
  bitmap_layer_set_bitmap(s_play_pause_icon_layer, s_pause_bitmap);
}

static void auto_advance_after_brush_callback(void *context) {
  s_auto_advance_timer = NULL;
  int new_duration =
      s_state.duration_sec == BRUSH_DURATION ? WAIT_DURATION : BRUSH_DURATION;
  s_state.duration_sec = new_duration;
  s_state.remaining_sec = new_duration;
  play_timer();
}

static void auto_advance_after_wait_callback(void *context) {
  s_auto_advance_timer = NULL;
  toggle_timer();
}

static void on_second_tick(struct tm *tick_time, TimeUnits units_changed) {
  s_state.remaining_sec -= 1;
  sync_display();

  if (s_state.remaining_sec > 0)
    return;

  tick_timer_service_unsubscribe();

  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
    s_play_start_delay_timer = NULL;
  }

  s_state.timer_state = TIMER_STATE_PAUSED;
  set_reset_icon_visible(false);
  set_toggle_icon_visible(false);
  set_play_pause_icon_visible(false);

  if (s_state.duration_sec == BRUSH_DURATION) {
    s_auto_advance_timer = app_timer_register(
        AUTO_ADVANCE_DELAY_MS, auto_advance_after_brush_callback, NULL);
  } else {
    s_auto_advance_timer = app_timer_register(
        AUTO_ADVANCE_DELAY_MS, auto_advance_after_wait_callback, NULL);
  }
}

static void begin_second_tick_callback(void *context) {
  s_play_start_delay_timer = NULL;
  tick_timer_service_subscribe(SECOND_UNIT, on_second_tick);
}

static void play_timer(void) {
  s_state.timer_state = TIMER_STATE_PLAYING;
  sync_display();

  show_pause_icon();
  set_play_pause_icon_visible(true);
  set_reset_icon_visible(false);
  set_toggle_icon_visible(false);

  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
  }
  s_play_start_delay_timer =
      app_timer_register(PLAY_START_DELAY_MS, begin_second_tick_callback, NULL);
}

static void pause_timer(void) {
  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
    s_play_start_delay_timer = NULL;
  }

  s_state.timer_state = TIMER_STATE_PAUSED;
  sync_display();
  tick_timer_service_unsubscribe();

  bool is_at_full_duration = s_state.duration_sec == s_state.remaining_sec;
  set_reset_icon_visible(!is_at_full_duration);
  set_toggle_icon_visible(is_at_full_duration);
  show_play_icon();
  set_play_pause_icon_visible(true);
}

static void reset_timer(void) {
  s_state.remaining_sec = s_state.duration_sec;
  s_state.timer_state = TIMER_STATE_PAUSED;
  sync_display();

  show_play_icon();
  set_play_pause_icon_visible(true);
  set_reset_icon_visible(false);
  set_toggle_icon_visible(true);
}

static void toggle_timer(void) {
  int new_duration =
      s_state.duration_sec == BRUSH_DURATION ? WAIT_DURATION : BRUSH_DURATION;
  s_state.duration_sec = new_duration;
  s_state.remaining_sec = new_duration;
  sync_display();

  show_play_icon();
  set_play_pause_icon_visible(true);
  set_reset_icon_visible(false);
  set_toggle_icon_visible(true);
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  bool reset_icon_is_hidden =
      layer_get_hidden(bitmap_layer_get_layer(s_reset_icon_layer));
  if (reset_icon_is_hidden) {
    return;
  }
  reset_timer();
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  bool toggle_icon_is_hidden =
      layer_get_hidden(bitmap_layer_get_layer(s_toggle_icon_layer));
  if (toggle_icon_is_hidden) {
    return;
  }
  toggle_timer();
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  bool play_pause_icon_is_hidden =
      layer_get_hidden(bitmap_layer_get_layer(s_play_pause_icon_layer));
  if (play_pause_icon_is_hidden) {
    return;
  }

  if (s_state.timer_state == TIMER_STATE_PLAYING) {
    pause_timer();
  } else {
    play_timer();
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static void sidebar_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect sidebar_bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, sidebar_bounds, 0, GCornerNone);
}

static GRect centered_icon_rect_in_slot(int slot_index, GRect sidebar_bounds,
                                        GSize icon_size) {
  int slot_height = sidebar_bounds.size.h / ICON_SLOT_COUNT;
  int icon_x = (sidebar_bounds.size.w - icon_size.w) / 2;
  int icon_y = slot_index * slot_height + (slot_height - icon_size.h) / 2;
  return GRect(icon_x, icon_y, icon_size.w, icon_size.h);
}

static void main_window_load(Window *window) {
  Layer *root_layer = window_get_root_layer(window);
  GRect root_bounds = layer_get_bounds(root_layer);

  window_set_background_color(window, GColorYellow);

  int timer_label_top = (root_bounds.size.h - 42) / 2;
  GRect timer_label_rect =
      GRect(0, timer_label_top, root_bounds.size.w - SIDEBAR_WIDTH,
            root_bounds.size.h);
  s_timer_label = text_layer_create(timer_label_rect);
  text_layer_set_background_color(s_timer_label, GColorClear);
  text_layer_set_text_color(s_timer_label, GColorBlack);
  text_layer_set_font(s_timer_label,
                      fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS));
  text_layer_set_text_alignment(s_timer_label, GTextAlignmentCenter);
  layer_add_child(root_layer, text_layer_get_layer(s_timer_label));

  GRect sidebar_rect = GRect(root_bounds.size.w - SIDEBAR_WIDTH, 0,
                             SIDEBAR_WIDTH, root_bounds.size.h);
  s_sidebar_layer = layer_create(sidebar_rect);
  layer_set_update_proc(s_sidebar_layer, sidebar_layer_update_proc);
  layer_add_child(root_layer, s_sidebar_layer);

  GRect sidebar_bounds = layer_get_bounds(s_sidebar_layer);

  s_reset_icon_layer = bitmap_layer_create(centered_icon_rect_in_slot(
      0, sidebar_bounds, gbitmap_get_bounds(s_reset_bitmap).size));
  bitmap_layer_set_bitmap(s_reset_icon_layer, s_reset_bitmap);
  bitmap_layer_set_compositing_mode(s_reset_icon_layer, GCompOpSet);
  layer_set_hidden(bitmap_layer_get_layer(s_reset_icon_layer), true);
  layer_add_child(s_sidebar_layer, bitmap_layer_get_layer(s_reset_icon_layer));

  s_toggle_icon_layer = bitmap_layer_create(centered_icon_rect_in_slot(
      1, sidebar_bounds, gbitmap_get_bounds(s_toggle_bitmap).size));
  bitmap_layer_set_bitmap(s_toggle_icon_layer, s_toggle_bitmap);
  bitmap_layer_set_compositing_mode(s_toggle_icon_layer, GCompOpSet);
  layer_add_child(s_sidebar_layer, bitmap_layer_get_layer(s_toggle_icon_layer));

  s_play_pause_icon_layer = bitmap_layer_create(centered_icon_rect_in_slot(
      2, sidebar_bounds, gbitmap_get_bounds(s_play_bitmap).size));
  bitmap_layer_set_bitmap(s_play_pause_icon_layer, s_play_bitmap);
  bitmap_layer_set_compositing_mode(s_play_pause_icon_layer, GCompOpSet);
  layer_add_child(s_sidebar_layer,
                  bitmap_layer_get_layer(s_play_pause_icon_layer));

  window_set_click_config_provider(window, click_config_provider);

  sync_display();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_timer_label);
  bitmap_layer_destroy(s_reset_icon_layer);
  bitmap_layer_destroy(s_toggle_icon_layer);
  bitmap_layer_destroy(s_play_pause_icon_layer);
  layer_destroy(s_sidebar_layer);
}

static void init(void) {
  s_state = (AppState){
      .timer_state = TIMER_STATE_PAUSED,
      .remaining_sec = BRUSH_DURATION,
      .duration_sec = BRUSH_DURATION,
  };

  s_play_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PLAY);
  s_pause_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PAUSE);
  s_reset_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_RESET);
  s_toggle_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_TOGGLE);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
                                                .load = main_window_load,
                                                .unload = main_window_unload,
                                            });
  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  if (s_play_start_delay_timer)
    app_timer_cancel(s_play_start_delay_timer);
  if (s_auto_advance_timer)
    app_timer_cancel(s_auto_advance_timer);
  tick_timer_service_unsubscribe();

  gbitmap_destroy(s_play_bitmap);
  gbitmap_destroy(s_pause_bitmap);
  gbitmap_destroy(s_reset_bitmap);
  gbitmap_destroy(s_toggle_bitmap);

  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
