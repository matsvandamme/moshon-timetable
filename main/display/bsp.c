#include "bsp.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

#include <string.h>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"

#include "lvgl.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static esp_lcd_panel_io_handle_t s_io_handle    = NULL;
static esp_lcd_panel_handle_t    s_panel_handle = NULL;
static esp_lcd_touch_handle_t    s_touch_handle = NULL;
static i2c_master_bus_handle_t   s_i2c_bus      = NULL;

static lv_display_t  *s_display = NULL;
static lv_indev_t    *s_touch_indev = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;
static TaskHandle_t      s_lvgl_task  = NULL;

#define LVGL_TASK_STACK    (8 * 1024)
#define LVGL_TASK_PRIO     2
#define LVGL_TICK_PERIOD_MS  2

// PARTIAL render mode with 30-line draw buffers, now placed in INTERNAL
// RAM (MALLOC_CAP_DMA) instead of PSRAM. PSRAM-backed DMA forced us to do
// manual esp_cache_msync() in the flush callback, and a small portion of
// flushes still occasionally landed with stale cache bytes mixed in — the
// sporadic red/green colour-bleed the user kept reporting (clock area
// flashing green, via lines / row backgrounds flashing red). Internal RAM
// with the DMA cap eliminates the coherency surface entirely.
//
// 30 lines * 480 px * 2 B = 28,800 bytes per buffer; 57 KB double-buffered.
// The companion `max_transfer_bytes` in the i80 bus config tracks this.
#define LVGL_DRAW_BUF_LINES  30
#define LVGL_DRAW_BUF_BYTES  (LCD_H_RES * LVGL_DRAW_BUF_LINES * 2)

// ---------------------------------------------------------------------------
// LVGL <-> esp_lcd glue
// ---------------------------------------------------------------------------

static bool IRAM_ATTR on_panel_io_color_trans_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    // ST7796 over esp_lcd i80 reads each 16-bit pixel as MSB-first, but LVGL
    // emits little-endian uint16_t in memory. Without this swap, NMBS navy
    // shows up as olive-green. Doing it in software here is more reliable
    // than lv_display_set_color_format(LV_COLOR_FORMAT_RGB565_SWAPPED),
    // which doesn't appear to take effect with this LVGL 9.2 + ESP-IDF combo.
    size_t pixels = (size_t)(area->x2 - area->x1 + 1) * (size_t)(area->y2 - area->y1 + 1);
    uint16_t *p = (uint16_t *)px_map;
    for (size_t i = 0; i < pixels; i++) {
        p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
    }
    // Internal-RAM DMA buffer — no explicit cache writeback needed; GDMA
    // and CPU share the same memory view here.
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool s_was_pressed = false;
    uint16_t x = 0, y = 0;
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_touch_handle);
    bool pressed = esp_lcd_touch_get_coordinates(s_touch_handle, &x, &y, NULL, &cnt, 1);
    if (pressed && cnt > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        // Edge-triggered log so we see touch events without spamming the serial.
        if (!s_was_pressed) {
            ESP_LOGI(TAG_BSP, "touch press at (%u, %u)", x, y);
            s_was_pressed = true;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_was_pressed = false;
    }
}

static void lvgl_tick_inc_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_task(void *arg)
{
    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_inc_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    while (1) {
        uint32_t delay_ms = 5;
        if (bsp_lvgl_lock(portMAX_DELAY)) {
            delay_ms = lv_timer_handler();
            bsp_lvgl_unlock();
        }
        if (delay_ms < 5)   delay_ms = 5;
        if (delay_ms > 500) delay_ms = 500;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool bsp_lvgl_lock(uint32_t timeout_ms)
{
    const TickType_t t = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, t) == pdTRUE;
}

void bsp_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

// ---------------------------------------------------------------------------
// Backlight (PWM via LEDC)
// ---------------------------------------------------------------------------

#define BSP_BL_LEDC_TIMER     LEDC_TIMER_0
#define BSP_BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BSP_BL_LEDC_DUTY_RES  LEDC_TIMER_10_BIT
#define BSP_BL_LEDC_FREQ_HZ   5000

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BSP_BL_LEDC_MODE,
        .duty_resolution = BSP_BL_LEDC_DUTY_RES,
        .timer_num       = BSP_BL_LEDC_TIMER,
        .freq_hz         = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = BSP_LCD_PIN_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel    = BSP_BL_LEDC_CHANNEL,
        .timer_sel  = BSP_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    return ESP_OK;
}

void bsp_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t max_duty = (1U << BSP_BL_LEDC_DUTY_RES) - 1;
    uint32_t duty = (max_duty * pct) / 100;
    ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}

// ---------------------------------------------------------------------------
// Display init (i80 8-bit -> ST7796)
// ---------------------------------------------------------------------------

static esp_err_t display_init(void)
{
    ESP_LOGI(TAG_BSP, "Init i80 bus");
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num     = BSP_LCD_PIN_DC,
        .wr_gpio_num     = BSP_LCD_PIN_PCLK,
        .clk_src         = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums  = {
            BSP_LCD_PIN_DATA0, BSP_LCD_PIN_DATA1, BSP_LCD_PIN_DATA2, BSP_LCD_PIN_DATA3,
            BSP_LCD_PIN_DATA4, BSP_LCD_PIN_DATA5, BSP_LCD_PIN_DATA6, BSP_LCD_PIN_DATA7,
        },
        .bus_width       = 8,
        // Match the LVGL draw-buffer size exactly. The i80 bus pre-allocates
        // DMA descriptors for transfers up to this size; if LVGL hands it a
        // larger buffer the bus driver corrupts memory (the partial-render
        // red band + scratch-marks at 100 lines and the LoadProhibited crash
        // at full-screen both came from this mismatch).
        .max_transfer_bytes = LVGL_DRAW_BUF_BYTES + 16,
        .psram_trans_align  = 64,
        .sram_trans_align   = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    ESP_LOGI(TAG_BSP, "Init panel IO");
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num         = BSP_LCD_PIN_CS,
        .pclk_hz             = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth   = 10,
        .dc_levels = {
            .dc_idle_level   = 0,
            .dc_cmd_level    = 0,
            .dc_dummy_level  = 0,
            .dc_data_level   = 1,
        },
        // We register the done-callback later via
        // esp_lcd_panel_io_register_event_callbacks() once the LVGL display
        // exists. Leave inline fields NULL here so v5.x doesn't fire twice.
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &s_io_handle));

    ESP_LOGI(TAG_BSP, "Init ST7796 panel");
    // On the WT32-SC01 Plus the esp_lcd_st7796 driver's `rgb_ele_order`
    // appears inverted vs the API name: passing RGB here makes the panel
    // render with R and B channels swapped (navy -> brown, yellow -> cyan).
    // Use BGR to get correct RGB ordering on the panel.
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(s_io_handle, &panel_cfg, &s_panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    // Landscape orientation. With swap_xy=true the panel's native X/Y are
    // swapped, so `mirror_y` here corresponds to the user's left/right flip
    // and `mirror_x` to top/bottom. Setting (mirror_x=true, mirror_y=true)
    // puts (0,0) at the physical top-left for the WT32-SC01 Plus board
    // (text reads left-to-right). If the board's ribbon ends up on the
    // other side, drop one of the trues.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, true));
    // Empirically: this WT32-SC01 Plus panel is "normally inverted" — leaving
    // invert_color OFF turns white into black on the LCD. Setting it ON sends
    // the DINVON command which puts the panel into "normal" display mode for
    // this hardware. Yes, the API name reads backwards on this board.
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Touch init (I2C + FT6336)
// ---------------------------------------------------------------------------

static esp_err_t touch_init(void)
{
    ESP_LOGI(TAG_BSP, "Init I2C for touch");
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port          = BSP_TOUCH_I2C_NUM,
        .sda_io_num        = BSP_TOUCH_PIN_SDA,
        .scl_io_num        = BSP_TOUCH_PIN_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io_handle));

    // Touch transform aligned to the display's swap_xy=true + mirror(1,1).
    // Empirically: with both mirrors=1 on the touch panel, the FT6336
    // returned Y values up to ~462 (i.e. unclamped raw Y instead of mapped
    // landscape Y). Dropping both mirrors AND keeping swap_xy=1 lets the
    // driver's clamping work and gives Y in the expected 0..319 range.
    esp_lcd_touch_config_t tp_cfg = {
        .x_max  = LCD_H_RES,
        .y_max  = LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_PIN_RST,
        .int_gpio_num = BSP_TOUCH_PIN_INT,
        .flags = {
            .swap_xy  = 1,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch_handle));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// LVGL init
// ---------------------------------------------------------------------------

static esp_err_t lvgl_init(void)
{
    ESP_LOGI(TAG_BSP, "Init LVGL");
    lv_init();

    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_mutex) return ESP_ERR_NO_MEM;

    // Internal-RAM DMA-capable draw buffers. PSRAM-backed DMA needed manual
    // cache writebacks in the flush callback and still left intermittent
    // colour bleed; internal RAM is cache-coherent with GDMA out of the
    // box. 57 KB total — comfortable on the S3's ~300 KB available SRAM.
    void *buf1 = heap_caps_malloc(LVGL_DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(LVGL_DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG_BSP, "Failed to allocate LVGL draw buffers in internal RAM");
        return ESP_ERR_NO_MEM;
    }
    memset(buf1, 0, LVGL_DRAW_BUF_BYTES);
    memset(buf2, 0, LVGL_DRAW_BUF_BYTES);

    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
    // PARTIAL mode with 160-line double buffer (~307 KB total in PSRAM).
    // Larger chunks than the original 60-line setup means fewer flush calls
    // and far less chance of partial-render artifacts (red band, scratches).
    lv_display_set_buffers(s_display, buf1, buf2, LVGL_DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_display, s_panel_handle);
    // Byte swap to the panel is handled manually in lvgl_flush_cb above
    // (LVGL 9.2's set_color_format(SWAPPED) was not effective here).

    // Wire the panel IO done-callback to flush_ready.
    esp_lcd_panel_io_callbacks_t cb = {
        .on_color_trans_done = on_panel_io_color_trans_done,
    };
    esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cb, s_display);

    // Touch input device.
    s_touch_indev = lv_indev_create();
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch_indev, lvgl_touch_read_cb);
    lv_indev_set_display(s_touch_indev, s_display);
    // 3 seconds for the wipe-Wi-Fi long-press gesture (handled in ui.c).
    lv_indev_set_long_press_time(s_touch_indev, 3000);

    BaseType_t ok = xTaskCreatePinnedToCore(
        lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, &s_lvgl_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG_BSP, "Failed to create LVGL task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG_BSP, "BSP init starting");
    ESP_ERROR_CHECK(backlight_init());
    bsp_set_backlight(0);                  // dark while we set up to hide flash garbage
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(lvgl_init());
    bsp_set_backlight(80);                 // fade-in handled by UI if desired
    ESP_LOGI(TAG_BSP, "BSP init OK");
    return ESP_OK;
}
