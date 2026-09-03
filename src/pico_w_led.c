#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico_w_led.h"

#include "pico/flash.h"
#include "hardware/flash.h"


static uint16_t repeating_time = 500;
static uint16_t repeating_time_short = 200;
static bool is_repeating = false;
static bool led_static_state = false;
static bool led_on = true;
static bool led_short = true;

static void led_do_work(async_context_t *context, async_at_time_worker_t *worker);
async_at_time_worker_t led_timeout = { .do_work = led_do_work };

static void led_do_work(async_context_t *context, async_at_time_worker_t *worker) {
    // Invert the led

    led_on = !led_on;
    led_short = !led_short;

    if (!is_repeating){
        led_on = led_static_state;
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
    if (is_repeating){
        async_context_add_at_time_worker_in_ms(context, worker, (led_short ? repeating_time : repeating_time_short) );
    }
}


void set_led_mode_pairing(){
    // In main start the process off - AFTER having called cyw43_arch_init...
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time = 100;
    repeating_time_short = 100;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}

void set_led_mode_playing_sbc(){
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time = 1000;
    repeating_time_short = 1000;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}

void set_led_mode_playing_ldac(){
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time_short = 1000;
    repeating_time = 3000;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}

void set_led_mode_playing_aac(){
    led_on = true;
    led_short = true;
    is_repeating = true;
    repeating_time_short = 1000;
    repeating_time = 200;
    async_context_add_at_time_worker_in_ms(cyw43_arch_async_context(), &led_timeout, repeating_time);
}




void set_led_mode_off(){
    stop_double_blink();
    stop_triple_blink();
    led_on = true;
    led_short = true;
    is_repeating = false;
    led_static_state = false;
}


void set_led_mode_on(){
    led_on = true;
    led_short = true;
    is_repeating = false;
    led_static_state = true;
}


// ——————————————————————————————————————————
// Double-blink repeat:
// on–off–on–off–pause → repeat
// ——————————————————————————————————————————
static const uint16_t dbl_durations[] = {
    200,   // step 0: LED on  (first blink on-time)
    200,   // step 1: LED off (first blink off-time)
    200,   // step 2: LED on  (second blink on-time)
    200,   // step 3: LED off (second blink off-time)
   2000    // step 4: pause (LED off) before repeating
};
static const bool dbl_states[] = {
    true, false, true, false, false
};
#define DBL_STEPS (sizeof(dbl_states)/sizeof(dbl_states[0]))

static size_t dbl_index = 0;
static async_at_time_worker_t dbl_worker = { .do_work = NULL };

static void dbl_do_work(async_context_t *ctx, async_at_time_worker_t *w){
    // apply LED state for this step
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, dbl_states[dbl_index]);
    // schedule next
    uint16_t delay = dbl_durations[dbl_index];
    dbl_index = (dbl_index + 1) % DBL_STEPS;
    async_context_add_at_time_worker_in_ms(ctx, w, delay);
}

void set_led_mode_double_blink(void){
    // wire up on first use
    if (!dbl_worker.do_work) dbl_worker.do_work = dbl_do_work;
    // start at step 0
    dbl_index = 0;
    async_context_add_at_time_worker_in_ms(
      cyw43_arch_async_context(),
      &dbl_worker,
      dbl_durations[0]
    );
}


// ——————————————————————————————————————————
// Triple-blink repeat:
// on–off–on–off–on–off–pause → repeat
// ——————————————————————————————————————————
static const uint16_t tri_durations[] = {
    200,  // step 0: on
    200,  // step 1: off
    200,  // step 2: on
    200,  // step 3: off
    200,  // step 4: on
    200,  // step 5: off
   2000   // step 6: pause
};
static const bool tri_states[] = {
    true, false, true, false, true, false, false
};
#define TRI_STEPS (sizeof(tri_states)/sizeof(tri_states[0]))

static size_t tri_index = 0;
static async_at_time_worker_t tri_worker = { .do_work = NULL };

static void tri_do_work(async_context_t *ctx, async_at_time_worker_t *w){
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, tri_states[tri_index]);
    uint16_t delay = tri_durations[tri_index];
    tri_index = (tri_index + 1) % TRI_STEPS;
    async_context_add_at_time_worker_in_ms(ctx, w, delay);
}

void set_led_mode_triple_blink(void){
    if (!tri_worker.do_work) tri_worker.do_work = tri_do_work;
    tri_index = 0;
    async_context_add_at_time_worker_in_ms(
      cyw43_arch_async_context(),
      &tri_worker,
      tri_durations[0]
    );
}

// stop double-blink
void stop_double_blink(void){
    async_context_remove_at_time_worker(
        cyw43_arch_async_context(),
        &dbl_worker   // the worker you started in set_led_mode_double_blink_repeat()
    );
    // restore LED to your “static” on/off state
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_static_state);
}

// stop triple-blink
void stop_triple_blink(void){
    async_context_remove_at_time_worker(
        cyw43_arch_async_context(),
        &tri_worker   // the worker you started in set_led_mode_triple_blink_repeat()
    );
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_static_state);
}




// ─── FLASH LAYOUT ─────────────────────────────────────────────────────────────
// These come from the board definitions in the Pico SDK:
// #define FLASH_PAGE_SIZE       256
// #define FLASH_SECTOR_SIZE     4096
#define FLASH_SIZE_BYTES      PICO_FLASH_SIZE_BYTES

// NOTE: the BTstack link-key TLV (pico_btstack) uses the LAST TWO flash
// sectors (PICO_FLASH_BANK_STORAGE_OFFSET = flash end - 2*4KB). The original
// code stored the slot byte + MACs in the last sector, so every link-key
// write could corrupt them (observed as EF-garbage slot MACs). Keep our
// storage one sector below the TLV region.
#define SLOT_STORAGE_SECTOR_BASE  (FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)

// We write the slot byte to the very last byte of our own sector:
#define TARGET_OFFSET         (SLOT_STORAGE_SECTOR_BASE + FLASH_SECTOR_SIZE - 1)

// ─── RAM BUFFERS & STATE FOR CALLBACK ─────────────────────────────────────────
static uint8_t  page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(FLASH_PAGE_SIZE)));
static uint32_t write_offset;
static uint8_t  write_value;

// ─── FLASH WRITE CALLBACK ────────────────────────────────────────────────────
// Runs *from* SRAM (no XIP), so it can erase & program safely.
void __time_critical_func(flash_write_cb)(void *unused) {
    (void) unused;

    // 1) Update our one byte in the RAM page copy
    uint32_t page_index = write_offset & (FLASH_PAGE_SIZE - 1);
    page_buf[page_index] = write_value;

    // 2) Erase the 4 KiB sector containing that page
    uint32_t sector_base = write_offset & ~(FLASH_SECTOR_SIZE - 1);
    flash_range_erase(sector_base, FLASH_SECTOR_SIZE);

    // 3) Re-program the full 256-byte page
    uint32_t page_base = write_offset & ~(FLASH_PAGE_SIZE - 1);
    flash_range_program(page_base, page_buf, FLASH_PAGE_SIZE);
}

// ─── HIGH-LEVEL HELPER ─────────────────────────────────────────────────────────
// Copies the target page into RAM, stashes parameters, then invokes
// flash_safe_execute() to do the erase+program from SRAM.
//
// Returns true on success.
bool write_uint8_last_flash(uint8_t value) {
    // Compute base of the page holding our target byte
    uint32_t page_base = TARGET_OFFSET & ~(FLASH_PAGE_SIZE - 1);

    // 1) Copy existing data from flash into RAM buffer
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + page_base);
    memcpy(page_buf, flash_ptr, FLASH_PAGE_SIZE);

    // 2) Stash what we need for the callback
    write_offset = TARGET_OFFSET;
    write_value  = value;

    // 3) Erase + program via safe, SRAM-resident callback (100 ms timeout)
    return flash_safe_execute(flash_write_cb, NULL, 100) == PICO_OK;
}


/**
 * Read a single byte from flash at the given offset.
 * 
 * @param offset    Byte‐address within the flash chip (0 .. PICO_FLASH_SIZE_BYTES-1)
 * @return          The uint8_t value stored at that flash address
 */
uint8_t read_uint8_from_flash(uint32_t offset) {
    // XIP_BASE is the base address where flash is mapped into the RP2040 address space
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + offset);
    // Volatile read to ensure the compiler doesn’t optimize this away
    return *(volatile const uint8_t *)flash_ptr;
}

/**
 * Convenience: read the byte you stored at the very last flash address.
 */
uint8_t read_uint8_last_flash(void) {
    return read_uint8_from_flash(TARGET_OFFSET);
}



// Base of the final 256-byte page of OUR slot sector (below the BTstack TLV):
#define LAST_PAGE_BASE        (SLOT_STORAGE_SECTOR_BASE + FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE)
// Base of its containing 4 KiB sector:
#define LAST_SECTOR_BASE      SLOT_STORAGE_SECTOR_BASE

// We carve out two 7-byte slots at the start of that page:

#define SLOT1_OFFSET_IN_PAGE  0           // bytes [0..6]
#define SLOT2_OFFSET_IN_PAGE  MAC_LEN     // bytes [7..13]

// ─── RAM BUFFER & STATE ───────────────────────────────────────────────────────
static uint8_t mac_page_buf[FLASH_PAGE_SIZE]
    __attribute__((aligned(FLASH_PAGE_SIZE)));
static const uint32_t mac_page_base   = LAST_PAGE_BASE;
static const uint32_t mac_sector_base = LAST_SECTOR_BASE;

// ─── FLASH‐SAFE CALLBACK ──────────────────────────────────────────────────────
// Runs from SRAM (no XIP), erases the sector & re-programs the full page.
void __time_critical_func(mac_flash_cb)(void *unused) {
    (void)unused;
    flash_range_erase(mac_sector_base, FLASH_SECTOR_SIZE);
    flash_range_program(mac_page_base, mac_page_buf, FLASH_PAGE_SIZE);
}

// ─── GENERIC WRITE/READ HELPERS ───────────────────────────────────────────────
static bool _write_mac_slot(uint32_t slot_offset, const uint8_t mac[MAC_LEN]) {
    // 1) Snapshot existing page into RAM
    memcpy(mac_page_buf, (const void *)(XIP_BASE + mac_page_base), FLASH_PAGE_SIZE);
    // 2) Overwrite just the 7 bytes for this slot
    memcpy(&mac_page_buf[slot_offset], mac, MAC_LEN);
    // 3) Erase + program via flash_safe_execute
    return flash_safe_execute(mac_flash_cb, NULL, 100) == PICO_OK;
}

static void _read_mac_slot(uint32_t slot_offset, uint8_t mac[MAC_LEN]) {
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + mac_page_base + slot_offset);
    for (int i = 0; i < MAC_LEN; i++) {
        mac[i] = flash_ptr[i];
    }
}

// ─── SLOT‐SPECIFIC APIs ────────────────────────────────────────────────────────

/**
 * Read 7-byte MAC from slot #1 into `mac[]`.
 */
void read_slot1_mac(uint8_t mac[MAC_LEN]) {
    _read_mac_slot(SLOT1_OFFSET_IN_PAGE, mac);
}

/**
 * Write 7-byte `mac[]` into slot #1.
 * Returns true on success.
 */
bool write_slot1_mac(const uint8_t mac[MAC_LEN]) {
    return _write_mac_slot(SLOT1_OFFSET_IN_PAGE, mac);
}

/**
 * Read 7-byte MAC from slot #2 into `mac[]`.
 */
void read_slot2_mac(uint8_t mac[MAC_LEN]) {
    _read_mac_slot(SLOT2_OFFSET_IN_PAGE, mac);
}

/**
 * Write 7-byte `mac[]` into slot #2.
 * Returns true on success.
 */
bool write_slot2_mac(const uint8_t mac[MAC_LEN]) {
    return _write_mac_slot(SLOT2_OFFSET_IN_PAGE, mac);
}

// Gain lives on the same last page as the MACs / slot byte, past the 12-byte
// MAC pair. All writers copy-then-program that page, so they preserve each other.
#define SETTINGS_MAGIC            0x5A
#define SETTINGS_EXTRA_MAGIC      0xA5
#define SETTINGS_MAGIC_OFF        16
#define SETTINGS_GAIN_OFF         17
#define SETTINGS_EXTRA_OFF        18
#define SETTINGS_AUTOANS_OFF      19
#define SETTINGS_CHIME_OFF        20
#define SETTINGS_ADAPT_OFF        21
#define SETTINGS_SLEEP_OFF        22
#define SETTINGS_CROWN_OFF        23
#define SETTINGS_LISTEN_OFF       24

uint8_t read_mic_gain_flash(void) {
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + mac_page_base);
    if (flash_ptr[SETTINGS_MAGIC_OFF] != SETTINGS_MAGIC) return 0;
    uint8_t db = flash_ptr[SETTINGS_GAIN_OFF];
    if (db > 24) return 0;
    return db;
}

bool write_mic_gain_flash(uint8_t gain_db) {
    host_prefs_t prefs;
    read_host_prefs_flash(&prefs);
    return write_host_settings_flash(gain_db, &prefs);
}

void read_host_prefs_flash(host_prefs_t *out) {
    // Defaults match handshake replay (LibrePods + user spec).
    out->auto_ans    = 2;
    out->chime       = 50;
    out->adapt_vol   = 2;
    out->sleep_det   = 1;
    out->crown_dir   = 2;
    out->listen_mask = 0x0F;
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + mac_page_base);
    if (flash_ptr[SETTINGS_MAGIC_OFF] != SETTINGS_MAGIC) return;
    if (flash_ptr[SETTINGS_EXTRA_OFF] != SETTINGS_EXTRA_MAGIC) return;
    uint8_t a = flash_ptr[SETTINGS_AUTOANS_OFF];
    uint8_t c = flash_ptr[SETTINGS_CHIME_OFF];
    uint8_t v = flash_ptr[SETTINGS_ADAPT_OFF];
    uint8_t s = flash_ptr[SETTINGS_SLEEP_OFF];
    uint8_t d = flash_ptr[SETTINGS_CROWN_OFF];
    uint8_t m = flash_ptr[SETTINGS_LISTEN_OFF];
    if (a == 1 || a == 2) out->auto_ans = a;
    if (c <= 100) out->chime = c;
    if (v == 1 || v == 2) out->adapt_vol = v;
    if (s == 1 || s == 2) out->sleep_det = s;
    if (d == 1 || d == 2) out->crown_dir = d;
    if (m != 0) out->listen_mask = m;
}

bool write_host_settings_flash(uint8_t gain_db, const host_prefs_t *prefs) {
    if (gain_db > 24) gain_db = 24;
    memcpy(mac_page_buf, (const void *)(XIP_BASE + mac_page_base), FLASH_PAGE_SIZE);
    mac_page_buf[SETTINGS_MAGIC_OFF]   = SETTINGS_MAGIC;
    mac_page_buf[SETTINGS_GAIN_OFF]    = gain_db;
    mac_page_buf[SETTINGS_EXTRA_OFF]   = SETTINGS_EXTRA_MAGIC;
    mac_page_buf[SETTINGS_AUTOANS_OFF] = prefs->auto_ans;
    mac_page_buf[SETTINGS_CHIME_OFF]   = prefs->chime;
    mac_page_buf[SETTINGS_ADAPT_OFF]   = prefs->adapt_vol;
    mac_page_buf[SETTINGS_SLEEP_OFF]   = prefs->sleep_det;
    mac_page_buf[SETTINGS_CROWN_OFF]   = prefs->crown_dir;
    mac_page_buf[SETTINGS_LISTEN_OFF]  = prefs->listen_mask;
    return flash_safe_execute(mac_flash_cb, NULL, 100) == PICO_OK;
}