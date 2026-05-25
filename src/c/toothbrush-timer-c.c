#include <pebble.h>

#define BRUSH_DURATION 120
#define WAIT_DURATION 1800
#define SIDEBAR_WIDTH 30
#define ICON_SLOT_COUNT 3
#define AUTO_ADVANCE_DELAY_MS 4200
#define REMAINING_FONT_SIZE 42
#define DURATION_FONT_SIZE 20
#define PERSIST_KEY_STATE 0
#define PERSIST_KEY_WAKEUP_ID 1

typedef enum { TIMER_STATE_PAUSED, TIMER_STATE_PLAYING } TimerState;

typedef struct {
  TimerState timer_state;
  int remaining_sec;
  int duration_sec;
  time_t end_epoch;
} AppState;

static AppState s_state;
static Window *s_main_window;
static TextLayer *s_remaining_label;
static TextLayer *s_duration_label;
static Layer *s_sidebar_layer;
static BitmapLayer *s_toggle_icon_layer;
static BitmapLayer *s_play_pause_icon_layer;
static GBitmap *s_play_bitmap;
static GBitmap *s_pause_bitmap;
static GBitmap *s_reset_bitmap;
static GBitmap *s_toggle_bitmap;
static AppTimer *s_play_start_delay_timer;
static AppTimer *s_auto_advance_timer;
static char s_remaining_text[8];
static char s_duration_text[8];
static WakeupId s_wakeup_id = -1;
static Layer *s_background_layer;

static void play_timer(void);
static void toggle_timer(void);
static void handle_timer_expired(bool should_vibrate);
static GRect centered_icon_rect_in_slot(int slot_index, GRect sidebar_bounds,
                                        GSize icon_size);

static void format_time(int sec, char *text_ptr, size_t text_size) {
  int minutes = sec / 60;
  int seconds = sec % 60;
  snprintf(text_ptr, text_size, "%d:%02d", minutes, seconds);
}

static void sync_display(void) {
  format_time(s_state.remaining_sec, s_remaining_text,
              sizeof(s_remaining_text));
  text_layer_set_text(s_remaining_label, s_remaining_text);

  format_time(s_state.duration_sec, s_duration_text, sizeof(s_duration_text));
  text_layer_set_text(s_duration_label, s_duration_text);
  layer_mark_dirty(s_background_layer);
}

static void set_toggle_reset_icon_visible(bool visible) {
  layer_set_hidden(bitmap_layer_get_layer(s_toggle_icon_layer), !visible);
}

static void set_play_pause_icon_visible(bool visible) {
  layer_set_hidden(bitmap_layer_get_layer(s_play_pause_icon_layer), !visible);
}

static void set_sidebar_icon(BitmapLayer *icon_layer, int slot_index,
                             GBitmap *bitmap) {
  GRect sidebar_bounds = layer_get_bounds(s_sidebar_layer);
  layer_set_frame(bitmap_layer_get_layer(icon_layer),
                  centered_icon_rect_in_slot(slot_index, sidebar_bounds,
                                             gbitmap_get_bounds(bitmap).size));
  bitmap_layer_set_bitmap(icon_layer, bitmap);
}

static void show_toggle_icon(void) {
  set_sidebar_icon(s_toggle_icon_layer, 0, s_toggle_bitmap);
}

static void show_reset_icon(void) {
  set_sidebar_icon(s_toggle_icon_layer, 0, s_reset_bitmap);
}

static void show_play_icon(void) {
  set_sidebar_icon(s_play_pause_icon_layer, 2, s_play_bitmap);
}

static void show_pause_icon(void) {
  set_sidebar_icon(s_play_pause_icon_layer, 2, s_pause_bitmap);
}

static void cancel_wakeup(void) {
  if (s_wakeup_id >= 0) {
    wakeup_cancel(s_wakeup_id);
    s_wakeup_id = -1;
  }
  persist_delete(PERSIST_KEY_WAKEUP_ID);
}

static void schedule_wakeup(void) {
  cancel_wakeup();
  s_state.end_epoch = time(NULL) + s_state.remaining_sec;
  s_wakeup_id = wakeup_schedule(s_state.end_epoch, 0, false);
  if (s_wakeup_id >= 0) {
    persist_write_int(PERSIST_KEY_WAKEUP_ID, s_wakeup_id);
  }
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

static void handle_timer_expired(bool should_vibrate) {
  if (should_vibrate) {
    static const uint32_t strong_triple_segments[] = {
        400, 200, 400, 200, 400, 800, 400, 200, 400, 200, 400};
    static const VibePattern strong_triple_pattern = {
        .durations = strong_triple_segments,
        .num_segments = ARRAY_LENGTH(strong_triple_segments),
    };
    vibes_enqueue_custom_pattern(strong_triple_pattern);
  }

  tick_timer_service_unsubscribe();

  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
    s_play_start_delay_timer = NULL;
  }

  s_state.timer_state = TIMER_STATE_PAUSED;
  set_toggle_reset_icon_visible(false);
  set_play_pause_icon_visible(false);

  if (s_state.duration_sec == BRUSH_DURATION) {
    s_auto_advance_timer = app_timer_register(
        AUTO_ADVANCE_DELAY_MS, auto_advance_after_brush_callback, NULL);
  } else {
    s_auto_advance_timer = app_timer_register(
        AUTO_ADVANCE_DELAY_MS, auto_advance_after_wait_callback, NULL);
  }
}

static void on_second_tick(struct tm *tick_time, TimeUnits units_changed) {
  s_state.remaining_sec -= 1;
  sync_display();

  if (s_state.duration_sec == BRUSH_DURATION) {
    if (s_state.remaining_sec == 90 || s_state.remaining_sec == 60 ||
        s_state.remaining_sec == 30) {

      static const uint32_t tight_triple_segments[] = {200, 100, 200, 100, 200};
      static const VibePattern tight_triple_pattern = {
          .durations = tight_triple_segments,
          .num_segments = ARRAY_LENGTH(tight_triple_segments),
      };
      vibes_enqueue_custom_pattern(tight_triple_pattern);
    }
  }

  if (s_state.remaining_sec > 0) {
    return;
  }

  cancel_wakeup();
  handle_timer_expired(true);
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
  set_toggle_reset_icon_visible(false);

  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
  }

  schedule_wakeup();

  uint16_t current_ms_into_second;
  time_ms(NULL, &current_ms_into_second);
  uint32_t ms_until_next_second = 1000 - current_ms_into_second;
  s_play_start_delay_timer = app_timer_register(
      ms_until_next_second, begin_second_tick_callback, NULL);
}

static void pause_timer(void) {
  cancel_wakeup();

  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
    s_play_start_delay_timer = NULL;
  }

  s_state.timer_state = TIMER_STATE_PAUSED;
  sync_display();
  tick_timer_service_unsubscribe();

  if (s_state.duration_sec == s_state.remaining_sec) {
    show_toggle_icon();
  } else {
    show_reset_icon();
  }
  set_toggle_reset_icon_visible(true);
  show_play_icon();
  set_play_pause_icon_visible(true);
}

static void reset_timer(void) {
  s_state.remaining_sec = s_state.duration_sec;
  s_state.timer_state = TIMER_STATE_PAUSED;
  sync_display();

  show_play_icon();
  set_play_pause_icon_visible(true);
  show_toggle_icon();
  set_toggle_reset_icon_visible(true);
}

static void toggle_timer(void) {
  int new_duration =
      s_state.duration_sec == BRUSH_DURATION ? WAIT_DURATION : BRUSH_DURATION;
  s_state.duration_sec = new_duration;
  s_state.remaining_sec = new_duration;
  sync_display();

  show_play_icon();
  set_play_pause_icon_visible(true);
  show_toggle_icon();
  set_toggle_reset_icon_visible(true);
}

static void toggle_reset_click_handler(ClickRecognizerRef recognizer,
                                       void *context) {
  if (layer_get_hidden(bitmap_layer_get_layer(s_toggle_icon_layer))) {
    return;
  }

  if (s_state.duration_sec == s_state.remaining_sec) {
    toggle_timer();
  } else {
    reset_timer();
  }
}

static void play_pause_click_handler(ClickRecognizerRef recognizer,
                                     void *context) {
  if (layer_get_hidden(bitmap_layer_get_layer(s_play_pause_icon_layer))) {
    return;
  }

  if (s_state.timer_state == TIMER_STATE_PLAYING) {
    pause_timer();
  } else {
    play_timer();
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, toggle_reset_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, play_pause_click_handler);
}

// TODO: swap to a text layer?
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

static void background_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int16_t fill_height =
      bounds.size.h * s_state.remaining_sec / s_state.duration_sec;
  int16_t fill_top = bounds.size.h - fill_height;
  graphics_context_set_fill_color(ctx, GColorYellow);
  graphics_fill_rect(ctx, GRect(0, fill_top, bounds.size.w, fill_height), 0,
                     GCornerNone);
}

static void main_window_load(Window *window) {
  Layer *root_layer = window_get_root_layer(window);
  GRect root_bounds = layer_get_bounds(root_layer);

  window_set_background_color(window, GColorLightGray);

  s_background_layer = layer_create(
      GRect(0, 0, root_bounds.size.w - SIDEBAR_WIDTH, root_bounds.size.h));
  layer_set_update_proc(s_background_layer, background_layer_update_proc);
  layer_add_child(root_layer, s_background_layer);

  int slot_height = root_bounds.size.h / ICON_SLOT_COUNT;
  int timer_label_top = (slot_height - REMAINING_FONT_SIZE) / 2;
  GRect remaining_label_rect =
      GRect(0, timer_label_top, root_bounds.size.w - SIDEBAR_WIDTH,
            REMAINING_FONT_SIZE);

  s_remaining_label = text_layer_create(remaining_label_rect);
  text_layer_set_background_color(s_remaining_label, GColorClear);
  text_layer_set_text_color(s_remaining_label, GColorBlack);
  text_layer_set_font(s_remaining_label,
                      fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS));
  text_layer_set_text_alignment(s_remaining_label, GTextAlignmentCenter);
  layer_add_child(root_layer, text_layer_get_layer(s_remaining_label));

  int duration_label_padding = 10;
  GRect duration_label_rect =
      GRect(duration_label_padding,
            root_bounds.size.h - DURATION_FONT_SIZE - duration_label_padding,
            root_bounds.size.w - SIDEBAR_WIDTH, DURATION_FONT_SIZE);

  s_duration_label = text_layer_create(duration_label_rect);
  text_layer_set_background_color(s_duration_label, GColorClear);
  text_layer_set_text_color(s_duration_label, GColorBlack);
  text_layer_set_font(s_duration_label,
                      fonts_get_system_font(FONT_KEY_LECO_20_BOLD_NUMBERS));
  text_layer_set_text_alignment(s_duration_label, GTextAlignmentLeft);
  layer_add_child(root_layer, text_layer_get_layer(s_duration_label));

  GRect sidebar_rect = GRect(root_bounds.size.w - SIDEBAR_WIDTH, 0,
                             SIDEBAR_WIDTH, root_bounds.size.h);
  s_sidebar_layer = layer_create(sidebar_rect);
  layer_set_update_proc(s_sidebar_layer, sidebar_layer_update_proc);
  layer_add_child(root_layer, s_sidebar_layer);

  GRect sidebar_bounds = layer_get_bounds(s_sidebar_layer);

  bool show_reset_icon = s_state.timer_state == TIMER_STATE_PAUSED &&
                         s_state.duration_sec != s_state.remaining_sec;
  GBitmap *toggle_reset_bitmap =
      show_reset_icon ? s_reset_bitmap : s_toggle_bitmap;

  s_toggle_icon_layer = bitmap_layer_create(GRectZero);
  bitmap_layer_set_compositing_mode(s_toggle_icon_layer, GCompOpSet);
  set_sidebar_icon(s_toggle_icon_layer, 0, toggle_reset_bitmap);
  set_toggle_reset_icon_visible(s_state.timer_state == TIMER_STATE_PAUSED);
  layer_add_child(s_sidebar_layer, bitmap_layer_get_layer(s_toggle_icon_layer));

  GBitmap *play_pause_bitmap = s_state.timer_state == TIMER_STATE_PAUSED
                                   ? s_play_bitmap
                                   : s_pause_bitmap;

  s_play_pause_icon_layer = bitmap_layer_create(GRectZero);
  bitmap_layer_set_compositing_mode(s_play_pause_icon_layer, GCompOpSet);
  set_sidebar_icon(s_play_pause_icon_layer, 2, play_pause_bitmap);
  set_play_pause_icon_visible(s_state.remaining_sec != 0);
  layer_add_child(s_sidebar_layer,
                  bitmap_layer_get_layer(s_play_pause_icon_layer));

  window_set_click_config_provider(window, click_config_provider);

  sync_display();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_remaining_label);
  bitmap_layer_destroy(s_toggle_icon_layer);
  bitmap_layer_destroy(s_play_pause_icon_layer);
  layer_destroy(s_sidebar_layer);
  layer_destroy(s_background_layer);
}

static void save_state(void) {
  persist_write_data(PERSIST_KEY_STATE, &s_state, sizeof(AppState));
}

static void load_state(void) {
  if (!persist_exists(PERSIST_KEY_STATE)) {
    return;
  }

  AppState saved_state;
  persist_read_data(PERSIST_KEY_STATE, &saved_state, sizeof(AppState));

  if (persist_exists(PERSIST_KEY_WAKEUP_ID)) {
    s_wakeup_id = persist_read_int(PERSIST_KEY_WAKEUP_ID);
  }

  if (saved_state.timer_state == TIMER_STATE_PLAYING) {
    int recalculated_remaining = (int)(saved_state.end_epoch - time(NULL));
    if (recalculated_remaining > 0) {
      s_state = saved_state;
      s_state.remaining_sec = recalculated_remaining;
    } else {
      s_state.duration_sec = saved_state.duration_sec;
      s_state.timer_state = TIMER_STATE_PAUSED;
      s_state.remaining_sec = 0;
    }
    return;
  }

  if (saved_state.timer_state == TIMER_STATE_PAUSED) {
    s_state = saved_state;
  }
}

static void init(void) {
  s_state = (AppState){
      .timer_state = TIMER_STATE_PAUSED,
      .remaining_sec = BRUSH_DURATION,
      .duration_sec = BRUSH_DURATION,
  };

  load_state();

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

  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    cancel_wakeup();
    handle_timer_expired(true);
  }

  if (s_state.timer_state == TIMER_STATE_PLAYING) {
    play_timer();
  }
}

static void deinit(void) {
  save_state();

  if (s_play_start_delay_timer) {
    app_timer_cancel(s_play_start_delay_timer);
  }
  if (s_auto_advance_timer) {
    app_timer_cancel(s_auto_advance_timer);
  }
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
