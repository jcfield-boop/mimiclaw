#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* ── LED states ───────────────────────────────────────────────
 *
 *  States are prioritised (lower number = higher priority).
 *  Calling led_set_state() with a lower-priority state than the
 *  current one is silently ignored UNLESS the current state is a
 *  timed flash (TELEGRAM_RX, EMAIL) which auto-expires.
 *
 *  Exception: LED_OOM and LED_ERROR always override everything.
 */
typedef enum {
    LED_OOM          = 0,   /* red solid → blink-out before restart      */
    LED_ERROR        = 1,   /* red rapid flash                            */
    LED_THINKING     = 2,   /* blue fast pulse (LLM call in flight)       */
    LED_TOOL         = 3,   /* cyan steady (tool executing)               */
    LED_TELEGRAM_RX  = 4,   /* blue 3× flash then return to prev state    */
    LED_EMAIL        = 5,   /* magenta 2× flash then return               */
    LED_WIFI_LOST    = 6,   /* orange slow pulse                          */
    LED_WIFI_CONNECTING = 7,/* yellow medium pulse                        */
    LED_IDLE         = 8,   /* green gentle breathe                       */
    LED_BOOT         = 9,   /* orange dim breathe during startup          */
} led_state_t;

/* Initialise the WS2812 on GPIO8 and start the animation timer.
 * Call once from app_main before any other led_* calls. */
esp_err_t led_status_init(void);

/* Set the current display state.
 * Higher-priority states override lower ones; lower-priority calls
 * are silently ignored while a high-priority state is active.      */
void led_set_state(led_state_t state);

/* Trigger a brief flash overlay (TELEGRAM_RX or EMAIL) without
 * permanently changing the background state.                       */
void led_flash_overlay(led_state_t flash_state);
