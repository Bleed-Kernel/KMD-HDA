#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pci/pci.h>
#include <cpu/mmio.h>
#include <mm/pmm.h>
#include <mm/spinlock.h>
#include <ACPI/acpi_hpet.h>
#include <devices/type/audio_device.h>
#include "hda_regs.h"

#define HDA_BDL_ENTRIES         32
#define HDA_PERIOD_FRAMES       256
#define HDA_FRAME_BYTES         4
#define HDA_PERIOD_BYTES        (HDA_PERIOD_FRAMES * HDA_FRAME_BYTES)
#define HDA_RING_BYTES          (HDA_BDL_ENTRIES * HDA_PERIOD_BYTES)
#define HDA_RATE_DEFAULT        48000
#define HDA_STREAM_TAG          1

#define HDA_MAX_CODECS          15
#define HDA_MAX_ROUTES          8
#define HDA_MAX_PATH            8
#define HDA_MAX_CONNS           32

#define HDA_RESET_WAIT_MS       100
#define HDA_CODEC_WAIT_MS       200
#define HDA_RESET_TRIES         2
#define HDA_CMD_WAIT_MS         50
#define HDA_POWER_WAIT_MS       20
#define HDA_STREAM_WAIT_MS      10
#define HDA_CMD_MAX_FAILS       3           // these spin with interrupts off, give up on a dead codec fast
#define HDA_MAX_RESTARTS        3           // dma dying on us, per stream
#define HDA_BACK_PERIODS        2           // a position this far behind is jitter, not a whole lap
#define HDA_BAR_MIN             0x2000
#define HDA_STUCK_TICKS         250
#define HDA_POSBUF_TICKS        20          // the ring laps in 170ms, cant wait long on a dead position buffer
#define HDA_JACK_TICKS          250

#define HDA_VOL_CENTI_DB        48          // a percent of the slider, same 48dB range as ac97
#define HDA_VOL_STEP_Q16        62013
#define HDA_QEMU_VENDOR         0x1AF4      // qemus codec, its amp steps are linear amplitude
#define HDA_REALTEK_VENDOR      0x10EC
#define HDA_REALTEK_COEF_NID    0x20

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed)) hda_bdl_entry_t;

// a pin and how to get from a dac to it. path[0] is the pin, path[len-1] the dac
typedef struct {
    uint8_t  pin;
    uint8_t  device;
    uint32_t config;
    uint8_t  len;
    uint8_t  path[HDA_MAX_PATH];
    uint8_t  index[HDA_MAX_PATH];   // which connection of path[i] leads to path[i+1]
    uint8_t  vol_nid;               // 0 when nothing on the way has a gain
    bool     vol_in;
    uint8_t  vol_index;
    uint32_t vol_caps;
    bool     detect;
    bool     plugged;
    bool     trigger;
    bool     pin_mute;              // no amp here can mute, the pin has to just be turned off
    uint8_t  pin_ctl;
} hda_route_t;

typedef struct {
    pci_device_t    pci;
    uint64_t        mmio;
    uint16_t        gcap;
    bool            ok64;
    uint32_t        sd;             // our output stream descriptor

    spinlock_t      cmd_lock;
    paddr_t         cmd_phys;
    uint32_t       *corb;
    uint64_t       *rirb;
    uint32_t        corb_entries;
    uint32_t        rirb_entries;
    uint16_t        corb_wp;
    uint16_t        rirb_rp;
    uint32_t        cmd_fails;
    bool            immediate;      // corb never answered, using the immediate registers

    paddr_t         bdl_phys;
    hda_bdl_entry_t *bdl;
    paddr_t         pos_phys;
    volatile uint32_t *pos;
    paddr_t         ring_phys;
    bool            use_lpib;

    uint8_t         cad;
    uint8_t         afg;
    uint16_t        vendor;
    uint16_t        device;
    uint32_t        afg_in_amp;
    uint32_t        afg_out_amp;
    uint32_t        afg_pcm;
    hda_route_t     routes[HDA_MAX_ROUTES];
    uint32_t        route_count;
    spinlock_t      out_lock;       // the tick reads jacks while someone picks an output
    int32_t         output;         // a route, or AUDIO_OUTPUT_AUTO
    bool            hp_in;
    bool            line_in;
    bool            muted;

    uint32_t        rate;
    uint16_t        format;

    uint32_t        last_period;
    uint32_t        stuck_ticks;
    uint32_t        restarts;
    uint32_t        jack_ticks;
    bool            told_moving;
    audio_device_t *audio;
} hda_t;

// taken as a difference so the femtosecond counter wrapping doesnt end every wait early
static inline bool hda_waited(uint64_t start, uint32_t ms) {
    return hpet_get_femtoseconds() - start >= (uint64_t)ms * 1000000000000ULL;
}

// mmio helpers
static inline uint8_t  hda_read8(hda_t *c, uint32_t reg)   { return mmio_read8(c->mmio, reg); }
static inline uint16_t hda_read16(hda_t *c, uint32_t reg)  { return mmio_read16(c->mmio, reg); }
static inline uint32_t hda_read32(hda_t *c, uint32_t reg)  { return mmio_read32(c->mmio, reg); }
static inline void hda_write8(hda_t *c, uint32_t reg, uint8_t v)   { mmio_write8(c->mmio, reg, v); }
static inline void hda_write16(hda_t *c, uint32_t reg, uint16_t v) { mmio_write16(c->mmio, reg, v); }
static inline void hda_write32(hda_t *c, uint32_t reg, uint32_t v) { mmio_write32(c->mmio, reg, v); }

static inline uint32_t hda_sd(hda_t *c, uint32_t reg) { return c->sd + reg; }

void hda_pci_quirks(hda_t *c);
bool hda_ctrl_reset(hda_t *c);
bool hda_cmd_alloc(hda_t *c);
void hda_cmd_start(hda_t *c);
void hda_cmd_stop(hda_t *c);
void hda_cmd_free(hda_t *c);
bool hda_verb(hda_t *c, uint8_t nid, uint32_t verb, uint8_t payload, uint32_t *out);
bool hda_verb4(hda_t *c, uint8_t nid, uint32_t verb, uint16_t payload);

bool hda_codec_scan(hda_t *c, uint16_t present);
void hda_codec_stream(hda_t *c, uint8_t tag);
void hda_codec_format(hda_t *c);
void hda_codec_volume(hda_t *c, const struct audio_volume *vol);
void hda_codec_jack(hda_t *c, bool force);
int  hda_codec_outputs(hda_t *c, struct audio_outputs *outputs);
int  hda_codec_set_output(hda_t *c, int32_t index);

void hda_codec_quirks(hda_t *c);

bool hda_stream_alloc(hda_t *c, paddr_t *ring_phys);
void hda_stream_free(hda_t *c);
int  hda_stream_start(hda_t *c);
void hda_stream_stop(hda_t *c);
uint32_t hda_stream_pos(hda_t *c);
int  hda_rate_format(hda_t *c, uint32_t rate, uint16_t *format);
