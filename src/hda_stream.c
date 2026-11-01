#include "hda_priv.h"
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <mm/dma.h>
#include <mm/paging.h>
#include <user/errno.h>
#include <ansii.h>

typedef struct {
    uint32_t rate;
    uint16_t bits;
    uint16_t cap;   // bit in the pcm parameter
} hda_rate_t;

static const hda_rate_t hda_rates[] = {
    {  8000, HDA_FMT_DIV(6),                       1u << 0 },
    { 11025, HDA_FMT_BASE_44K | HDA_FMT_DIV(4),    1u << 1 },
    { 16000, HDA_FMT_DIV(3),                       1u << 2 },
    { 22050, HDA_FMT_BASE_44K | HDA_FMT_DIV(2),    1u << 3 },
    { 32000, HDA_FMT_MULT(2) | HDA_FMT_DIV(3),     1u << 4 },
    { 44100, HDA_FMT_BASE_44K,                     1u << 5 },
    { 48000, 0,                                    1u << 6 },
};

int hda_rate_format(hda_t *c, uint32_t rate, uint16_t *format) {
    int best = -1;
    for (size_t i = 0; i < sizeof(hda_rates) / sizeof(hda_rates[0]); i++) {
        if (!(c->afg_pcm & hda_rates[i].cap))
            continue;
        if (best < 0 || hda_rates[i].rate <= rate)
            best = (int)i;
    }

    // every codec has to do 48k, a bad pcm parameter shouldnt stop us
    if (best < 0) {
        *format = HDA_FMT_16BIT | HDA_FMT_STEREO;
        return HDA_RATE_DEFAULT;
    }
    *format = hda_rates[best].bits | HDA_FMT_16BIT | HDA_FMT_STEREO;
    return (int)hda_rates[best].rate;
}

bool hda_stream_alloc(hda_t *c, paddr_t *ring_phys) {
    size_t ring_pages = HDA_RING_BYTES / PAGE_SIZE;
    c->bdl = dma_alloc(1, &c->bdl_phys, !c->ok64);
    c->pos = dma_alloc(1, &c->pos_phys, !c->ok64);
    void *ring = dma_alloc(ring_pages, ring_phys, !c->ok64);
    if (!c->bdl || !c->pos || !ring) {
        serial_printf(LOG_ERROR "hda: OOM for the dma ring\n");
        c->ring_phys = ring ? *ring_phys : 0;
        hda_stream_free(c);
        return false;
    }
    c->ring_phys = *ring_phys;

    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        c->bdl[i].addr   = (uint64_t)*ring_phys + i * HDA_PERIOD_BYTES;
        c->bdl[i].length = HDA_PERIOD_BYTES;
        c->bdl[i].flags  = 0;
    }
    return true;
}

void hda_stream_free(hda_t *c) {
    if (c->bdl)
        dma_free(c->bdl_phys, 1);
    if (c->pos)
        dma_free(c->pos_phys, 1);
    if (c->ring_phys)
        dma_free(c->ring_phys, HDA_RING_BYTES / PAGE_SIZE);
    c->bdl       = NULL;
    c->pos       = NULL;
    c->bdl_phys  = 0;
    c->pos_phys  = 0;
    c->ring_phys = 0;
}

static bool hda_stream_reset(hda_t *c) {
    uint32_t reg = hda_sd(c, HDA_SD_CTL);

    hda_write32(c, reg, (hda_read32(c, reg) & ~(HDA_SDCTL_RUN | HDA_SDSTS_MASK)));
    if (!mmio_wait32(c->mmio, reg, HDA_SDCTL_RUN, false, HDA_STREAM_WAIT_MS))
        serial_printf(LOG_WARN "hda: stream wont stop, ctl 0x%x\n", hda_read32(c, reg));

    hda_write32(c, reg, (hda_read32(c, reg) & ~HDA_SDSTS_MASK) | HDA_SDCTL_SRST);
    if (!mmio_wait32(c->mmio, reg, HDA_SDCTL_SRST, true, HDA_STREAM_WAIT_MS)) {
        serial_printf(LOG_ERROR "hda: stream wont enter reset, ctl 0x%x\n", hda_read32(c, reg));
        return false;
    }

    hda_write32(c, reg, hda_read32(c, reg) & ~(HDA_SDCTL_SRST | HDA_SDSTS_MASK));
    if (!mmio_wait32(c->mmio, reg, HDA_SDCTL_SRST, false, HDA_STREAM_WAIT_MS)) {
        serial_printf(LOG_ERROR "hda: stream wont leave reset, ctl 0x%x\n", hda_read32(c, reg));
        return false;
    }
    return true;
}

int hda_stream_start(hda_t *c) {
    if (!hda_stream_reset(c))
        return -EIO;

    hda_write32(c, hda_sd(c, HDA_SD_BDPL), (uint32_t)c->bdl_phys);
    hda_write32(c, hda_sd(c, HDA_SD_BDPU), (uint32_t)((uint64_t)c->bdl_phys >> 32));
    hda_write32(c, hda_sd(c, HDA_SD_CBL), HDA_RING_BYTES);
    hda_write16(c, hda_sd(c, HDA_SD_LVI), HDA_BDL_ENTRIES - 1);
    hda_write16(c, hda_sd(c, HDA_SD_FMT), c->format);

    // a stale position from the last run would look like the hardware jumped
    c->pos[(c->sd - HDA_SD_BASE) / HDA_SD_SIZE * 2] = 0;
    hda_write32(c, HDA_DPUBASE, (uint32_t)((uint64_t)c->pos_phys >> 32));
    hda_write32(c, HDA_DPLBASE, (uint32_t)c->pos_phys | HDA_DPLBASE_ENABLE);

    uint32_t reg = hda_sd(c, HDA_SD_CTL);
    uint32_t ctl = hda_read32(c, reg) & ~(HDA_SDSTS_MASK | HDA_SDCTL_TAG_MASK);
    ctl |= (uint32_t)HDA_STREAM_TAG << HDA_SDCTL_TAG_SHIFT;
    hda_write32(c, reg, ctl | HDA_SDSTS_CLEAR);
    hda_write32(c, reg, ctl | HDA_SDCTL_RUN);

    serial_printf(LOG_INFO "hda: stream on, %u Hz, fmt 0x%x, ctl 0x%x\n", c->rate, c->format, hda_read32(c, reg));
    return 0;
}

void hda_stream_stop(hda_t *c) {
    hda_stream_reset(c);
    hda_write32(c, HDA_DPLBASE, 0);
    hda_write32(c, HDA_DPUBASE, 0);
}

// the position buffer costs nothing to read, lpib is the fallback for chips that dont write it
uint32_t hda_stream_pos(hda_t *c) {
    uint32_t pos = c->use_lpib ? hda_read32(c, hda_sd(c, HDA_SD_LPIB))
                               : c->pos[(c->sd - HDA_SD_BASE) / HDA_SD_SIZE * 2];
    return pos < HDA_RING_BYTES ? pos : 0;
}
