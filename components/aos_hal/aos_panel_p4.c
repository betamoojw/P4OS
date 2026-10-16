/*
 * P4OS - the panel's refresh against the rest of PSRAM's traffic.
 *
 * The DPI refresh reads the framebuffer from PSRAM through the DW-GDMA and
 * nothing waits for it: when a burst of other traffic (a game reading its
 * textures through the cache, a long memcpy) holds the bus, the DSI bridge's
 * FIFO runs dry and the panel flashes blue. IDF notices it in its bridge ISR
 * and only says so on the UART ("can't fetch data from external memory fast
 * enough, underrun happens"), which the portal cannot see, and its own ISR
 * clears the event: the bridge keeps no count.
 *
 * So the count comes from the bridge itself: its underrun interrupt is turned
 * off (IDF's handler only printed it, and cleared the event), and the raw
 * bit, which latches regardless, is read and cleared at every end of frame
 * from the refresh's own interrupt: a count of frames that ran dry.
 *
 * And the arbiter: the AXI interconnect gives each master a QoS priority
 * (0-15, all 0 out of reset and IDF never sets them on the P4). The panel's
 * DMA reads PSRAM through DW-GDMA master port 1; raising its read priority
 * over the cache's lets the refresh go first when both want the bus.
 */
#include "aos_hal.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/axi_icm_ll.h"
#include "hal/mipi_dsi_brg_ll.h"

static const char *TAG = "panel";

/* ---- underruns ---- */

static DRAM_ATTR volatile uint32_t s_underruns;

uint32_t aos_hal_panel_underruns(void) { return s_underruns; }

/* ---- the refresh's own rhythm ----
 * The DPI refresh is restarted by an interrupt at the end of every frame (the
 * DW-GDMA's, esp_lcd_panel_dpi.c): if that interrupt is late, the panel gets
 * no pixels meanwhile and shows blue, and the bridge reports no underrun. So
 * each restart is timed; a gap of more than one and a half frames is a late
 * one. */
static DRAM_ATTR int64_t s_last_us;
static DRAM_ATTR volatile uint32_t s_late, s_frames;
static DRAM_ATTR volatile uint32_t s_max_gap_us;
static DRAM_ATTR uint32_t s_frame_us = 33333;      /* set from the measured rate */

/* When the late ones came, and when the task watchdog went off (its ISR
 * calls esp_task_wdt_isr_user_handler), to see whether they go together */
#define STAMPS 8
static DRAM_ATTR uint32_t s_late_at[STAMPS], s_wdt_at[STAMPS];
static DRAM_ATTR volatile uint32_t s_late_n, s_wdt_n;

void IRAM_ATTR esp_task_wdt_isr_user_handler(void)
{
    s_wdt_at[s_wdt_n++ % STAMPS] = (uint32_t)(esp_timer_get_time() / 1000);
}

void aos_hal_panel_stamps(uint32_t *late, int *nlate, uint32_t *wdt, int *nwdt, int max)
{
    uint32_t a = s_late_n, b = s_wdt_n;
    int cap = max < STAMPS ? max : STAMPS;
    int na = a < (uint32_t)cap ? (int)a : cap, nb = b < (uint32_t)cap ? (int)b : cap;
    for (int i = 0; i < na; i++) late[i] = s_late_at[(a - na + i) % STAMPS];
    for (int i = 0; i < nb; i++) wdt[i] = s_wdt_at[(b - nb + i) % STAMPS];
    *nlate = na;
    *nwdt = nb;
}

void IRAM_ATTR aos_p4_panel_frame(void)
{
    if (MIPI_DSI_BRIDGE.int_raw.underrun_int_raw) {
        s_underruns++;
        MIPI_DSI_BRIDGE.int_clr.val = MIPI_DSI_BRG_LL_EVENT_UNDERRUN;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_us) {
        uint32_t gap = (uint32_t)(now - s_last_us);
        if (gap > s_max_gap_us) s_max_gap_us = gap;
        if (gap > s_frame_us + s_frame_us / 2) {
            s_late++;
            s_late_at[s_late_n++ % STAMPS] = (uint32_t)(now / 1000);
        }
    }
    s_last_us = now;
    s_frames++;
}

void aos_hal_panel_frames(uint32_t *frames, uint32_t *late, uint32_t *max_gap_us)
{
    if (frames) *frames = s_frames;
    if (late) *late = s_late;
    if (max_gap_us) *max_gap_us = s_max_gap_us;
}

/* what came since the last line, at most one line every 5 s */
static void report_cb(void *arg)
{
    static uint32_t told, told_late, frames0;
    static bool rate_set;
    uint32_t n = s_underruns, late = s_late, fr = s_frames;
    /* the first 5 s give the panel's real frame time */
    if (!rate_set && fr > frames0 + 10) {
        s_frame_us = 5000000u / (fr - frames0);
        rate_set = true;
        aos_hal_log("panel", "refresh at %u.%u Hz", (unsigned)(10000000u / s_frame_us / 10),
                    (unsigned)(10000000u / s_frame_us % 10));
        s_late = late = 0;
        s_max_gap_us = 0;
    }
    frames0 = fr;
    if (n != told) {
        aos_hal_log("panel", "%u frame(s) ran dry (underrun) in 5 s (%u since boot): the screen flashed",
                    (unsigned)(n - told), (unsigned)n);
        told = n;
    }
    if (late != told_late) {
        aos_hal_log("panel", "%u late restart(s) of the refresh in 5 s (%u since boot, the longest gap %u ms): the screen flashed",
                    (unsigned)(late - told_late), (unsigned)late, (unsigned)(s_max_gap_us / 1000));
        told_late = late;
    }
}

/* ---- the interconnect's priorities ---- */

static int s_dma, s_cache;

static void apply(int dma, int cache)
{
    s_dma = dma;
    s_cache = cache;
    /* read and write alike: the refresh only reads, the cache does both */
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, dma, dma);
    axi_icm_ll_set_cache_qos_arbiter_prio(cache, cache);
}

void aos_hal_panel_qos_get(int *dma, int *cache)
{
    if (dma) *dma = s_dma;
    if (cache) *cache = s_cache;
}

bool aos_hal_panel_qos_set(int dma, int cache, bool save)
{
    if (dma < 0 || dma > 15 || cache < 0 || cache > 15) return false;
    apply(dma, cache);
    ESP_LOGI(TAG, "QoS: panel DMA %d, cache %d%s", dma, cache, save ? " (kept)" : "");
    if (save) {
        aos_hal_pref_set_i32("pq_dma", dma);
        aos_hal_pref_set_i32("pq_cache", cache);
    }
    return true;
}

/* aos_hal_p4.c, once the panel is up */
void aos_p4_panel_watch_init(void)
{
    /* IDF masks underruns in the first h_size lines of a frame
     * (fifo_underrun_discard_vcnt = 720 of 1280): every line counts now. Its
     * interrupt goes off; aos_p4_panel_frame() reads the raw bit instead. */
    mipi_dsi_brg_ll_set_underrun_discard_count(&MIPI_DSI_BRIDGE, 0);
    mipi_dsi_brg_ll_update_dpi_config(&MIPI_DSI_BRIDGE);
    mipi_dsi_brg_ll_enable_interrupt(&MIPI_DSI_BRIDGE, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, false);
    MIPI_DSI_BRIDGE.int_clr.val = MIPI_DSI_BRG_LL_EVENT_UNDERRUN;

    int32_t dma = 0, cache = 0;
    aos_hal_pref_get_i32("pq_dma", &dma);
    aos_hal_pref_get_i32("pq_cache", &cache);
    if (dma < 0 || dma > 15) dma = 0;
    if (cache < 0 || cache > 15) cache = 0;
    apply(dma, cache);
    ESP_LOGI(TAG, "up on core %d (its interrupts too); QoS: panel DMA %d, cache %d",
             xPortGetCoreID(), (int)dma, (int)cache);

    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = report_cb, .name = "panel" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_periodic(t, 5 * 1000 * 1000);
}
