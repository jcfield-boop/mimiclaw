#include "led_status.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led";

/* ── Hardware ────────────────────────────────────────────── */
#define LED_GPIO        8
#define LED_COUNT       1
#define TICK_MS         20      /* animation timer period       */

/* Maximum brightness 0-255 (20% to avoid eye-strain at close range) */
#define MAX_BRIGHT      51

/* ── State ───────────────────────────────────────────────── */
static led_strip_handle_t s_strip = NULL;
static TimerHandle_t      s_timer  = NULL;

static led_state_t  s_state     = LED_BOOT;   /* current state         */
static led_state_t  s_bg_state  = LED_BOOT;   /* state under flash     */
static bool         s_in_flash  = false;
static int          s_flash_rem = 0;          /* half-periods remaining */
static bool         s_flash_on  = false;
static uint16_t     s_phase     = 0;          /* 0-255 animation phase  */

/* ── Colour table ────────────────────────────────────────── */
typedef struct { uint8_t r, g, b; } rgb_t;

/* Colours at full brightness — scaled by animation */
static const rgb_t STATE_COLOUR[] = {
    [LED_OOM]            = {255,   0,   0},  /* red           */
    [LED_ERROR]          = {255,   0,   0},  /* red           */
    [LED_THINKING]       = {  0,  80, 255},  /* blue          */
    [LED_TOOL]           = {  0, 200, 200},  /* cyan          */
    [LED_TELEGRAM_RX]    = {  0,  80, 255},  /* blue          */
    [LED_EMAIL]          = {180,   0, 180},  /* magenta       */
    [LED_WIFI_LOST]      = {255,  80,   0},  /* orange        */
    [LED_WIFI_CONNECTING]= {220, 180,   0},  /* yellow        */
    [LED_IDLE]           = {  0, 255,  40},  /* green         */
    [LED_BOOT]           = {255,  80,   0},  /* orange dim    */
};

/* ── Helpers ─────────────────────────────────────────────── */

/* Map phase 0-255 to a brightness 0-MAX_BRIGHT using a smooth
 * triangle wave with a raised floor so it never goes fully dark. */
static uint8_t breathe(uint16_t phase)
{
    /* Triangle: 0→255→0 */
    uint8_t p = (phase <= 127) ? (phase * 2) : (255 - (phase - 128) * 2);
    /* Gamma-ish: square then scale */
    uint32_t v = ((uint32_t)p * p * MAX_BRIGHT) / (255 * 255);
    /* Raised floor: at least 5% of max so it never fully turns off */
    uint32_t floor_val = MAX_BRIGHT / 20;
    return (uint8_t)(v < floor_val ? floor_val : v);
}

static void set_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void set_scaled(const rgb_t *c, uint8_t scale)
{
    set_pixel(
        (uint8_t)((uint32_t)c->r * scale / 255),
        (uint8_t)((uint32_t)c->g * scale / 255),
        (uint8_t)((uint32_t)c->b * scale / 255)
    );
}

/* ── Animation timer callback (fires every TICK_MS ms) ───── */
static void anim_tick(TimerHandle_t xTimer)
{
    (void)xTimer;

    /* ── Flash overlay handling ───────────────────── */
    if (s_in_flash) {
        if (s_flash_rem > 0) {
            s_flash_on = !s_flash_on;
            s_flash_rem--;
            if (s_flash_on) {
                set_scaled(&STATE_COLOUR[s_state], MAX_BRIGHT);
            } else {
                set_pixel(0, 0, 0);
            }
        } else {
            /* Flash complete — restore background state */
            s_in_flash = false;
            s_state    = s_bg_state;
            s_phase    = 0;
        }
        return;
    }

    /* ── Per-state animation ──────────────────────── */
    const rgb_t *c = &STATE_COLOUR[s_state];

    switch (s_state) {

    case LED_BOOT:
        /* Dim orange breathe, slow (5s) */
        s_phase = (s_phase + 1) & 0xFF;
        set_scaled(c, breathe(s_phase) / 3);  /* extra dim for boot */
        break;

    case LED_IDLE:
        /* Green gentle breathe, 5 s period (256 ticks × 20ms ≈ 5.1s) */
        s_phase = (s_phase + 1) & 0xFF;
        set_scaled(c, breathe(s_phase));
        break;

    case LED_WIFI_CONNECTING:
        /* Yellow medium pulse, 2.5 s period */
        s_phase = (s_phase + 2) & 0xFF;
        set_scaled(c, breathe(s_phase));
        break;

    case LED_WIFI_LOST:
        /* Orange slow pulse, 3 s period */
        s_phase = (s_phase + 1) & 0xFF;
        set_scaled(c, breathe(s_phase));
        break;

    case LED_THINKING:
        /* Blue fast pulse, ~1 s period */
        s_phase = (s_phase + 5) & 0xFF;
        set_scaled(c, breathe(s_phase));
        break;

    case LED_TOOL:
        /* Cyan steady at half brightness */
        set_scaled(c, MAX_BRIGHT / 2);
        break;

    case LED_ERROR:
        /* Red rapid flash, ~4 Hz */
        s_phase = (s_phase + 13) & 0xFF;
        set_scaled(c, (s_phase < 128) ? MAX_BRIGHT : 0);
        break;

    case LED_OOM:
        /* Red solid for 2s then fade off (device restarts in 3s) */
        s_phase++;
        if (s_phase < 100) {
            set_scaled(c, MAX_BRIGHT);
        } else {
            /* Fade out */
            uint8_t fade = (s_phase < 200) ? (uint8_t)(MAX_BRIGHT - (s_phase - 100) * MAX_BRIGHT / 100) : 0;
            set_scaled(c, fade);
        }
        break;

    default:
        set_pixel(0, 0, 0);
        break;
    }
}

/* ── Public API ──────────────────────────────────────────── */

esp_err_t led_status_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num          = LED_GPIO,
        .max_leds                = LED_COUNT,
        .led_model               = LED_MODEL_WS2812,
        .color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out        = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return err;
    }

    led_strip_clear(s_strip);

    s_timer = xTimerCreate("led_anim", pdMS_TO_TICKS(TICK_MS),
                           pdTRUE, NULL, anim_tick);
    if (!s_timer) {
        ESP_LOGE(TAG, "Failed to create animation timer");
        return ESP_FAIL;
    }

    xTimerStart(s_timer, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "LED status ready (GPIO%d, tick=%dms)", LED_GPIO, TICK_MS);
    return ESP_OK;
}

void led_set_state(led_state_t state)
{
    if (!s_strip) return;

    /* Flash overlays are handled separately; ignore led_set_state during flash */
    if (s_in_flash && state > LED_ERROR) return;

    /* Only update if the new state has equal or higher priority */
    if (state <= s_state || state == LED_IDLE || state == LED_BOOT ||
        state == LED_WIFI_CONNECTING || state == LED_WIFI_LOST) {
        s_state  = state;
        s_bg_state = state;
        s_phase  = 0;
        s_in_flash = false;
    }
}

void led_flash_overlay(led_state_t flash_state)
{
    if (!s_strip) return;
    /* Don't interrupt critical states */
    if (s_state <= LED_ERROR) return;

    s_bg_state  = s_state;
    s_state     = flash_state;
    s_in_flash  = true;
    s_flash_on  = false;
    s_phase     = 0;

    /* Number of half-periods:
     *   TELEGRAM_RX: 3 flashes × 2 half-periods = 6, each ~150ms
     *   EMAIL:       2 flashes × 2 half-periods = 4                */
    s_flash_rem = (flash_state == LED_EMAIL) ? 8 : 12;
}
