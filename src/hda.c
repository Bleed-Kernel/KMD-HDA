#include "hda_priv.h"
#include <kmd/kmd.h>
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

static hda_t hda;

static int hda_start(void *priv) {
    hda_t *c = priv;
    c->last_period = 0;
    c->stuck_ticks = 0;
    c->jack_ticks  = 0;
    c->restarts    = 0;
    hda_codec_jack(c, true);
    return hda_stream_start(c);
}

static void hda_stop(void *priv) {
    hda_stream_stop(priv);
}

static uint32_t hda_hw_offset(void *priv) {
    return hda_stream_pos(priv);
}

static int hda_set_rate(void *priv, uint32_t rate) {
    hda_t *c = priv;
    if (rate < 8000 || rate > 48000)
        return -EINVAL;

    uint16_t format;
    int      got = hda_rate_format(c, rate, &format);
    c->rate   = (uint32_t)got;
    c->format = format;
    hda_codec_format(c);
    return got;
}

static int hda_set_volume(void *priv, const struct audio_volume *vol) {
    hda_codec_volume(priv, vol);
    return 0;
}

static int hda_get_outputs(void *priv, struct audio_outputs *outputs) {
    return hda_codec_outputs(priv, outputs);
}

static int hda_set_output(void *priv, int32_t index) {
    return hda_codec_set_output(priv, index);
}

static const audio_driver_t hda_driver = {
    .name        = "hda",
    .start       = hda_start,
    .stop        = hda_stop,
    .hw_offset   = hda_hw_offset,
    .set_rate    = hda_set_rate,
    .set_volume  = hda_set_volume,
    .get_outputs = hda_get_outputs,
    .set_output  = hda_set_output,
};

static void hda_stuck(hda_t *c) {
    c->stuck_ticks++;
    if (!c->use_lpib && c->stuck_ticks == HDA_POSBUF_TICKS) {
        uint32_t lpib = hda_read32(c, hda_sd(c, HDA_SD_LPIB));
        if (lpib) {
            serial_printf(LOG_WARN "hda: position buffer never moved but lpib is 0x%x, using lpib\n", lpib);
            c->use_lpib    = true;
            c->stuck_ticks = 0;
        }
        return;
    }

    if (c->stuck_ticks != HDA_STUCK_TICKS)
        return;

    serial_printf(LOG_WARN "hda: dma hasnt moved, ctl 0x%x lpib 0x%x, restart %u\n", hda_read32(c, hda_sd(c, HDA_SD_CTL)),
                  hda_read32(c, hda_sd(c, HDA_SD_LPIB)), c->restarts);

    // anyone writing is asleep waiting on periods that wont come
    if (c->restarts >= HDA_MAX_RESTARTS)
        return;

    c->restarts++;
    c->stuck_ticks = 0;
    c->last_period = 0;
    hda_stream_start(c);
    audio_device_advance(c->audio, 0);
}

static void hda_tick(void *ctx) {
    hda_t *c = ctx;
    if (!c->audio || !c->audio->running)
        return;

    if (++c->jack_ticks >= HDA_JACK_TICKS) {
        c->jack_ticks = 0;
        hda_codec_jack(c, false);
    }

    uint32_t period = hda_stream_pos(c) / HDA_PERIOD_BYTES;
    if (period == c->last_period) {
        hda_stuck(c);
        return;
    }

    // advance would take this as a nearly full lap and wipe the ring
    if ((period + HDA_BDL_ENTRIES - c->last_period) % HDA_BDL_ENTRIES >= HDA_BDL_ENTRIES - HDA_BACK_PERIODS)
        return;

    if (!c->told_moving) {
        c->told_moving = true;
        serial_printf(LOG_OK "hda: dma is moving (%s)\n", c->use_lpib ? "lpib" : "position buffer");
    }
    c->stuck_ticks = 0;
    c->last_period = period;
    audio_device_advance(c->audio, period);
}

static bool hda_find_codecs(hda_t *c, uint16_t *present) {
    uint64_t start = hpet_get_femtoseconds();
    while (!hda_waited(start, HDA_CODEC_WAIT_MS)) {
        *present = hda_read16(c, HDA_STATESTS) & 0x7FFF;
        if (*present)
            return true;
        asm volatile("pause");
    }
    return false;
}

// the speakers on these are usually i2s behind the dsp, not on this link at all
static void hda_tell_dsp(hda_t *c) {
    if (c->pci.subclass == PCI_SUBCLASS_AUDIO || c->pci.prog_if == PCI_PROGIF_HDA_DSP)
        serial_printf(LOG_WARN "hda: %04x:%04x is a dsp audio controller, analog is likely i2s and needs dsp firmware\n",
                      c->pci.vendor_id, c->pci.device_id);
}

static int hda_probe(kmd_device_t *dev) {
    hda_t *c = &hda;
    if (c->audio) {
        serial_printf(LOG_INFO "hda: already driving one card, leaving " PCI_SPEC " alone\n", PCI_SPEC_ARGS(&dev->pci));
        return -EBUSY;
    }

    memset(c, 0, sizeof(*c));
    c->pci    = dev->pci;
    c->output = AUDIO_OUTPUT_AUTO;
    spinlock_init(&c->out_lock);

    if (c->pci.bar[0] & 1) {
        serial_printf(LOG_WARN "hda: %04x:%04x has no memory bar, skipping it\n", c->pci.vendor_id, c->pci.device_id);
        return -ENODEV;
    }

    uint64_t size = pci_bar_size(&c->pci, 0);
    if (size < HDA_BAR_MIN) {
        serial_printf(LOG_WARN "hda: bar0 is only %llu bytes, skipping it\n", size);
        return -ENODEV;
    }

    pci_enable(&c->pci);
    c->mmio = pci_map_bar(&c->pci, 0, size);
    if (!c->mmio) {
        serial_printf(LOG_ERROR "hda: couldnt map bar0 (0x%x, %llu bytes)\n", c->pci.bar[0], size);
        return -EIO;
    }

    // intel audio class devices arent all hda, the audio dsps on their own have other registers here
    if (hda_read8(c, HDA_VMAJ) != 1) {
        serial_printf(LOG_INFO "hda: %04x:%04x class %02x.%02x.%02x isnt hda, vmaj 0x%x\n", c->pci.vendor_id,
                      c->pci.device_id, c->pci.class_code, c->pci.subclass, c->pci.prog_if, hda_read8(c, HDA_VMAJ));
        return -ENODEV;
    }

    c->gcap = hda_read16(c, HDA_GCAP);
    c->ok64 = c->gcap & HDA_GCAP_64OK;
    serial_printf(LOG_INFO "hda: %04x:%04x at " PCI_SPEC ", v%u.%u, gcap 0x%x, %u in %u out %u bidi\n", c->pci.vendor_id,
                  c->pci.device_id, PCI_SPEC_ARGS(&c->pci), hda_read8(c, HDA_VMAJ), hda_read8(c, HDA_VMIN), c->gcap,
                  HDA_GCAP_ISS(c->gcap), HDA_GCAP_OSS(c->gcap), HDA_GCAP_BSS(c->gcap));

    // all ones is a device thats asleep or not decoding
    if (c->gcap == 0xFFFF || !HDA_GCAP_OSS(c->gcap)) {
        serial_printf(LOG_WARN "hda: no output streams, gcap 0x%x\n", c->gcap);
        return -ENODEV;
    }
    c->sd = HDA_SD_BASE + HDA_GCAP_ISS(c->gcap) * HDA_SD_SIZE;

    hda_pci_quirks(c);
    if (!hda_cmd_alloc(c))
        return -ENOMEM;

    // some chips miss the codecs on the first reset out of firmware
    uint16_t present = 0;
    for (int i = 0; i < HDA_RESET_TRIES && !present; i++) {
        if (!hda_ctrl_reset(c)) {
            hda_cmd_free(c);
            return -EIO;
        }
        hda_find_codecs(c, &present);
    }
    if (!present) {
        serial_printf(LOG_INFO "hda: no codecs on " PCI_SPEC "\n", PCI_SPEC_ARGS(&c->pci));
        hda_tell_dsp(c);
        hda_cmd_free(c);
        return -ENODEV;
    }
    serial_printf(LOG_INFO "hda: codec mask 0x%x\n", present);

    hda_cmd_start(c);
    if (!hda_codec_scan(c, present)) {
        serial_printf(LOG_INFO "hda: nothing on " PCI_SPEC " can make sound come out of a speaker\n",
                      PCI_SPEC_ARGS(&c->pci));
        hda_tell_dsp(c);
        hda_cmd_stop(c);
        hda_cmd_free(c);
        return -ENODEV;
    }

    paddr_t ring_phys = 0;
    if (!hda_stream_alloc(c, &ring_phys)) {
        hda_cmd_stop(c);
        hda_cmd_free(c);
        return -ENOMEM;
    }

    c->rate = (uint32_t)hda_rate_format(c, HDA_RATE_DEFAULT, &c->format);
    hda_codec_format(c);
    hda_codec_stream(c, HDA_STREAM_TAG);

    struct audio_volume full = { .left = 100, .right = 100 };
    hda_codec_volume(c, &full);

    c->audio = audio_device_register("audio0", &hda_driver, c, ring_phys, HDA_PERIOD_BYTES, HDA_BDL_ENTRIES, c->rate, 2);
    if (!c->audio) {
        serial_printf(LOG_ERROR "hda: couldnt register /dev/audio0\n");
        hda_stream_free(c);
        hda_cmd_stop(c);
        hda_cmd_free(c);
        return -EIO;
    }

    if (tick_register(hda_tick, c) != 0) {
        serial_printf(LOG_ERROR "hda: no room for the tick, nothing would ever drive the card\n");
        audio_device_unregister(c->audio);
        c->audio = NULL;
        hda_stream_free(c);
        hda_cmd_stop(c);
        hda_cmd_free(c);
        return -EIO;
    }

    dev->priv = c;
    return 0;
}

static int hda_remove(kmd_device_t *dev) {
    hda_t *c = dev->priv;

    tick_unregister(hda_tick, c);
    int r = audio_device_unregister(c->audio);
    if (r < 0) {
        serial_printf(LOG_WARN "hda: /dev/audio0 is in use, staying (%d)\n", r);
        tick_register(hda_tick, c);
        return r;
    }

    c->audio = NULL;
    hda_codec_stream(c, 0);
    hda_cmd_stop(c);
    hda_ctrl_reset(c);
    hda_stream_free(c);
    hda_cmd_free(c);
    serial_printf(LOG_OK "hda: let go of the card\n");
    return 0;
}

static int hda_unload(void) {
    return 0;
}

static const kmd_pci_match_t hda_ids[] = {
    KMD_PCI_CLASS(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA, KMD_ANY),
    { PCI_VENDOR_INTEL, KMD_ANY, PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO, KMD_ANY },
    KMD_PCI_END,
};

KMD_DRIVER(hda_kmd) = {
    .description = "Intel High Definition Audio",
    .pci         = hda_ids,
    .probe       = hda_probe,
    .remove      = hda_remove,
    .unload      = hda_unload,
};
