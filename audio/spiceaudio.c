/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * maintained by Gerd Hoffmann <kraxel@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/hw.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/qemu-spice.h"

#define AUDIO_CAP "spice"
#include "audio.h"
#include "audio_int.h"

#if SPICE_INTERFACE_PLAYBACK_MAJOR > 1 || SPICE_INTERFACE_PLAYBACK_MINOR >= 3
#define LINE_OUT_SAMPLES (480 * 4)
#else
#define LINE_OUT_SAMPLES (256 * 4)
#endif

#if SPICE_INTERFACE_RECORD_MAJOR > 2 || SPICE_INTERFACE_RECORD_MINOR >= 3
#define LINE_IN_SAMPLES (480 * 4)
#else
#define LINE_IN_SAMPLES (256 * 4)
#endif

typedef struct SpiceRateCtl {
    int64_t               start_ticks;
    int64_t               bytes_sent;
} SpiceRateCtl;

typedef struct SpiceVoiceOut {
    HWVoiceOut            hw;
    SpicePlaybackInstance sin;
    SpiceRateCtl          rate;
    int                   active;
    uint32_t              *frame;
    uint32_t              fpos;
    uint32_t              fsize;
} SpiceVoiceOut;

typedef struct SpiceVoiceIn {
    HWVoiceIn             hw;
    SpiceRecordInstance   sin;
    SpiceRateCtl          rate;
    int                   active;
} SpiceVoiceIn;

static const SpicePlaybackInterface playback_sif = {
    .base.type          = SPICE_INTERFACE_PLAYBACK,
    .base.description   = "playback",
    .base.major_version = SPICE_INTERFACE_PLAYBACK_MAJOR,
    .base.minor_version = SPICE_INTERFACE_PLAYBACK_MINOR,
};

static const SpiceRecordInterface record_sif = {
    .base.type          = SPICE_INTERFACE_RECORD,
    .base.description   = "record",
    .base.major_version = SPICE_INTERFACE_RECORD_MAJOR,
    .base.minor_version = SPICE_INTERFACE_RECORD_MINOR,
};

static void *spice_audio_init(Audiodev *dev)
{
    if (!using_spice) {
        return NULL;
    }
    return &spice_audio_init;
}

static void spice_audio_fini (void *opaque)
{
    /* nothing */
}

static void rate_start (SpiceRateCtl *rate)
{
    memset (rate, 0, sizeof (*rate));
    rate->start_ticks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int rate_get_samples (struct audio_pcm_info *info, SpiceRateCtl *rate)
{
    int64_t now;
    int64_t ticks;
    int64_t bytes;
    int64_t samples;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ticks = now - rate->start_ticks;
    bytes = muldiv64 (ticks, info->bytes_per_second, get_ticks_per_sec ());
    samples = (bytes - rate->bytes_sent) >> info->shift;
    if (samples < 0 || samples > 65536) {
        error_report("Resetting rate control (%" PRId64 " samples)", samples);
        rate_start (rate);
        samples = 0;
    }
    rate->bytes_sent += samples << info->shift;
    return samples;
}

/* playback */

static int line_out_init(HWVoiceOut *hw, struct audsettings *as,
                         void *drv_opaque)
{
    SpiceVoiceOut *out = container_of (hw, SpiceVoiceOut, hw);
    struct audsettings settings;

#if SPICE_INTERFACE_PLAYBACK_MAJOR > 1 || SPICE_INTERFACE_PLAYBACK_MINOR >= 3
    settings.freq       = spice_server_get_best_playback_rate(NULL);
#else
    settings.freq       = SPICE_INTERFACE_PLAYBACK_FREQ;
#endif
    settings.nchannels  = SPICE_INTERFACE_PLAYBACK_CHAN;
    settings.fmt        = AUDIO_FORMAT_S16;
    settings.endianness = AUDIO_HOST_ENDIANNESS;

    audio_pcm_init_info (&hw->info, &settings);
    hw->samples = LINE_OUT_SAMPLES;
    out->active = 0;

    out->sin.base.sif = &playback_sif.base;
    qemu_spice_add_interface (&out->sin.base);
#if SPICE_INTERFACE_PLAYBACK_MAJOR > 1 || SPICE_INTERFACE_PLAYBACK_MINOR >= 3
    spice_server_set_playback_rate(&out->sin, settings.freq);
#endif
    return 0;
}

static void line_out_fini (HWVoiceOut *hw)
{
    SpiceVoiceOut *out = container_of (hw, SpiceVoiceOut, hw);

    spice_server_remove_interface (&out->sin.base);
}

static void *line_out_get_buffer(HWVoiceOut *hw, size_t *size)
{
    SpiceVoiceOut *out = container_of(hw, SpiceVoiceOut, hw);
    size_t decr;

    if (!out->frame) {
        spice_server_playback_get_buffer(&out->sin, &out->frame, &out->fsize);
        out->fpos = 0;
    }

    decr = rate_get_samples(&hw->info, &out->rate);
    decr = MIN(out->fsize - out->fpos, decr);

    *size = decr << hw->info.shift;
    return out->frame + out->fpos;
}

static size_t line_out_put_buffer(HWVoiceOut *hw, void *buf, size_t size)
{
    SpiceVoiceOut *out = container_of(hw, SpiceVoiceOut, hw);

    out->fpos += size >> 2;
    assert(buf == out->frame + out->fpos && out->fpos <= out->fsize);

    if (out->fpos == out->fsize) { /* buffer full */
        spice_server_playback_put_samples(&out->sin, out->frame);
        out->frame = NULL;
    }

    return size;
}

static int line_out_ctl (HWVoiceOut *hw, int cmd, ...)
{
    SpiceVoiceOut *out = container_of (hw, SpiceVoiceOut, hw);

    switch (cmd) {
    case VOICE_ENABLE:
        if (out->active) {
            break;
        }
        out->active = 1;
        rate_start (&out->rate);
        spice_server_playback_start (&out->sin);
        break;
    case VOICE_DISABLE:
        if (!out->active) {
            break;
        }
        out->active = 0;
        if (out->frame) {
            memset(out->frame + out->fpos, 0, (out->fsize - out->fpos) << 2);
            spice_server_playback_put_samples (&out->sin, out->frame);
            out->frame = NULL;
        }
        spice_server_playback_stop (&out->sin);
        break;
    case VOICE_VOLUME:
        {
#if ((SPICE_INTERFACE_PLAYBACK_MAJOR >= 1) && (SPICE_INTERFACE_PLAYBACK_MINOR >= 2))
            SWVoiceOut *sw;
            va_list ap;
            uint16_t vol[2];

            va_start (ap, cmd);
            sw = va_arg (ap, SWVoiceOut *);
            va_end (ap);

            vol[0] = sw->vol.l / ((1ULL << 16) + 1);
            vol[1] = sw->vol.r / ((1ULL << 16) + 1);
            spice_server_playback_set_volume (&out->sin, 2, vol);
            spice_server_playback_set_mute (&out->sin, sw->vol.mute);
#endif
            break;
        }
    }

    return 0;
}

/* record */

static int line_in_init(HWVoiceIn *hw, struct audsettings *as, void *drv_opaque)
{
    SpiceVoiceIn *in = container_of (hw, SpiceVoiceIn, hw);
    struct audsettings settings;

#if SPICE_INTERFACE_RECORD_MAJOR > 2 || SPICE_INTERFACE_RECORD_MINOR >= 3
    settings.freq       = spice_server_get_best_record_rate(NULL);
#else
    settings.freq       = SPICE_INTERFACE_RECORD_FREQ;
#endif
    settings.nchannels  = SPICE_INTERFACE_RECORD_CHAN;
    settings.fmt        = AUDIO_FORMAT_S16;
    settings.endianness = AUDIO_HOST_ENDIANNESS;

    audio_pcm_init_info (&hw->info, &settings);
    hw->samples = LINE_IN_SAMPLES;
    in->active = 0;

    in->sin.base.sif = &record_sif.base;
    qemu_spice_add_interface (&in->sin.base);
#if SPICE_INTERFACE_RECORD_MAJOR > 2 || SPICE_INTERFACE_RECORD_MINOR >= 3
    spice_server_set_record_rate(&in->sin, settings.freq);
#endif
    return 0;
}

static void line_in_fini (HWVoiceIn *hw)
{
    SpiceVoiceIn *in = container_of (hw, SpiceVoiceIn, hw);

    spice_server_remove_interface (&in->sin.base);
}

static size_t line_in_read(HWVoiceIn *hw, void *buf, size_t len)
{
    SpiceVoiceIn *in = container_of (hw, SpiceVoiceIn, hw);
    uint64_t delta_samp = rate_get_samples(&hw->info, &in->rate);
    uint64_t to_read = MIN(len >> 2, delta_samp);
    size_t ready = spice_server_record_get_samples(&in->sin, buf, to_read);

    /* XXX: do we need this? */
    if (ready == 0) {
        memset(buf, 0, to_read << 2);
        ready = to_read;
    }

    return ready << 2;
}

static int line_in_ctl (HWVoiceIn *hw, int cmd, ...)
{
    SpiceVoiceIn *in = container_of (hw, SpiceVoiceIn, hw);

    switch (cmd) {
    case VOICE_ENABLE:
        if (in->active) {
            break;
        }
        in->active = 1;
        rate_start (&in->rate);
        spice_server_record_start (&in->sin);
        break;
    case VOICE_DISABLE:
        if (!in->active) {
            break;
        }
        in->active = 0;
        spice_server_record_stop (&in->sin);
        break;
    case VOICE_VOLUME:
        {
#if ((SPICE_INTERFACE_RECORD_MAJOR >= 2) && (SPICE_INTERFACE_RECORD_MINOR >= 2))
            SWVoiceIn *sw;
            va_list ap;
            uint16_t vol[2];

            va_start (ap, cmd);
            sw = va_arg (ap, SWVoiceIn *);
            va_end (ap);

            vol[0] = sw->vol.l / ((1ULL << 16) + 1);
            vol[1] = sw->vol.r / ((1ULL << 16) + 1);
            spice_server_record_set_volume (&in->sin, 2, vol);
            spice_server_record_set_mute (&in->sin, sw->vol.mute);
#endif
            break;
        }
    }

    return 0;
}

static struct audio_pcm_ops audio_callbacks = {
    .init_out = line_out_init,
    .fini_out = line_out_fini,
    .write    = audio_generic_write,
    .get_buffer_out = line_out_get_buffer,
    .put_buffer_out = line_out_put_buffer,
    .ctl_out  = line_out_ctl,

    .init_in  = line_in_init,
    .fini_in  = line_in_fini,
    .read     = line_in_read,
    .ctl_in   = line_in_ctl,
};

struct audio_driver spice_audio_driver = {
    .name           = "spice",
    .descr          = "spice audio driver",
    .init           = spice_audio_init,
    .fini           = spice_audio_fini,
    .pcm_ops        = &audio_callbacks,
    .max_voices_out = 1,
    .max_voices_in  = 1,
    .voice_size_out = sizeof (SpiceVoiceOut),
    .voice_size_in  = sizeof (SpiceVoiceIn),
#if ((SPICE_INTERFACE_PLAYBACK_MAJOR >= 1) && (SPICE_INTERFACE_PLAYBACK_MINOR >= 2))
    .ctl_caps       = VOICE_VOLUME_CAP
#endif
};

void qemu_spice_audio_init (void)
{
    spice_audio_driver.can_be_default = 1;
}
