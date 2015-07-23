/*
 * QEMU Audio subsystem
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hw/hw.h"
#include "audio.h"
#include "monitor/monitor.h"
#include "qapi-visit.h"
#include "qapi/opts-visitor.h"
#include "qemu/timer.h"
#include "qemu/config-file.h"
#include "sysemu/sysemu.h"

#define AUDIO_CAP "audio"
#include "audio_int.h"

/* #define DEBUG_LIVE */
/* #define DEBUG_OUT */
/* #define DEBUG_CAPTURE */
/* #define DEBUG_POLL */

#define SW_NAME(sw) (sw)->name ? (sw)->name : "unknown"


/* Order of CONFIG_AUDIO_DRIVERS is import.
   The 1st one is the one used by default, that is the reason
    that we generate the list.
*/
struct audio_driver *drvtab[] = {
#ifdef CONFIG_SPICE
    &spice_audio_driver,
#endif
    CONFIG_AUDIO_DRIVERS
    &no_audio_driver,
    &wav_audio_driver,
    NULL
};

static QTAILQ_HEAD(AudioStateHead, AudioState) audio_states =
    QTAILQ_HEAD_INITIALIZER(audio_states);

const struct mixeng_volume nominal_volume = {
    .mute = 0,
#ifdef FLOAT_MIXENG
    .r = 1.0,
    .l = 1.0,
#else
    .r = 1ULL << 32,
    .l = 1ULL << 32,
#endif
};

static bool legacy_config;

#ifdef AUDIO_IS_FLAWLESS_AND_NO_CHECKS_ARE_REQURIED
#error No its not
#else
int audio_bug (const char *funcname, int cond)
{
    if (cond) {
        static int shown;

        AUD_log (NULL, "A bug was just triggered in %s\n", funcname);
        if (!shown) {
            shown = 1;
            AUD_log (NULL, "Save all your work and restart without audio\n");
            AUD_log (NULL, "I am sorry\n");
        }
        AUD_log (NULL, "Context:\n");

#if defined AUDIO_BREAKPOINT_ON_BUG
#  if defined HOST_I386
#    if defined __GNUC__
        __asm__ ("int3");
#    elif defined _MSC_VER
        _asm _emit 0xcc;
#    else
        abort ();
#    endif
#  else
        abort ();
#  endif
#endif
    }

    return cond;
}
#endif

static inline int audio_bits_to_index (int bits)
{
    switch (bits) {
    case 8:
        return 0;

    case 16:
        return 1;

    case 32:
        return 2;

    default:
        audio_bug ("bits_to_index", 1);
        AUD_log (NULL, "invalid bits %d\n", bits);
        return 0;
    }
}

void *audio_calloc (const char *funcname, int nmemb, size_t size)
{
    int cond;
    size_t len;

    len = nmemb * size;
    cond = !nmemb || !size;
    cond |= nmemb < 0;
    cond |= len < size;

    if (audio_bug ("audio_calloc", cond)) {
        AUD_log (NULL, "%s passed invalid arguments to audio_calloc\n",
                 funcname);
        AUD_log (NULL, "nmemb=%d size=%zu (len=%zu)\n", nmemb, size, len);
        return NULL;
    }

    return g_malloc0 (len);
}

void AUD_vlog (const char *cap, const char *fmt, va_list ap)
{
    if (cap) {
        fprintf(stderr, "%s: ", cap);
    }

    vfprintf(stderr, fmt, ap);
}

void AUD_log (const char *cap, const char *fmt, ...)
{
    va_list ap;

    va_start (ap, fmt);
    AUD_vlog (cap, fmt, ap);
    va_end (ap);
}

static void audio_print_settings (struct audsettings *as)
{
    dolog ("frequency=%d nchannels=%d fmt=", as->freq, as->nchannels);

    switch (as->fmt) {
    case AUDIO_FORMAT_S8:
        AUD_log (NULL, "S8");
        break;
    case AUDIO_FORMAT_U8:
        AUD_log (NULL, "U8");
        break;
    case AUDIO_FORMAT_S16:
        AUD_log (NULL, "S16");
        break;
    case AUDIO_FORMAT_U16:
        AUD_log (NULL, "U16");
        break;
    case AUDIO_FORMAT_S32:
        AUD_log (NULL, "S32");
        break;
    case AUDIO_FORMAT_U32:
        AUD_log (NULL, "U32");
        break;
    default:
        AUD_log (NULL, "invalid(%d)", as->fmt);
        break;
    }

    AUD_log (NULL, " endianness=");
    switch (as->endianness) {
    case 0:
        AUD_log (NULL, "little");
        break;
    case 1:
        AUD_log (NULL, "big");
        break;
    default:
        AUD_log (NULL, "invalid");
        break;
    }
    AUD_log (NULL, "\n");
}

static int audio_validate_settings (struct audsettings *as)
{
    int invalid;

    invalid = as->nchannels != 1 && as->nchannels != 2;
    invalid |= as->endianness != 0 && as->endianness != 1;

    switch (as->fmt) {
    case AUDIO_FORMAT_S8:
    case AUDIO_FORMAT_U8:
    case AUDIO_FORMAT_S16:
    case AUDIO_FORMAT_U16:
    case AUDIO_FORMAT_S32:
    case AUDIO_FORMAT_U32:
        break;
    default:
        invalid = 1;
        break;
    }

    invalid |= as->freq <= 0;
    return invalid ? -1 : 0;
}

static int audio_pcm_info_eq (struct audio_pcm_info *info, struct audsettings *as)
{
    int bits = 8, sign = 0;

    switch (as->fmt) {
    case AUDIO_FORMAT_S8:
        sign = 1;
        /* fall through */
    case AUDIO_FORMAT_U8:
        break;

    case AUDIO_FORMAT_S16:
        sign = 1;
        /* fall through */
    case AUDIO_FORMAT_U16:
        bits = 16;
        break;

    case AUDIO_FORMAT_S32:
        sign = 1;
        /* fall through */
    case AUDIO_FORMAT_U32:
        bits = 32;
        break;

    default:
        abort();
    }
    return info->freq == as->freq
        && info->nchannels == as->nchannels
        && info->sign == sign
        && info->bits == bits
        && info->swap_endianness == (as->endianness != AUDIO_HOST_ENDIANNESS);
}

void audio_pcm_init_info (struct audio_pcm_info *info, struct audsettings *as)
{
    int bits = 8, sign = 0, shift = 0;

    switch (as->fmt) {
    case AUDIO_FORMAT_S8:
        sign = 1;
    case AUDIO_FORMAT_U8:
        break;

    case AUDIO_FORMAT_S16:
        sign = 1;
    case AUDIO_FORMAT_U16:
        bits = 16;
        shift = 1;
        break;

    case AUDIO_FORMAT_S32:
        sign = 1;
    case AUDIO_FORMAT_U32:
        bits = 32;
        shift = 2;
        break;

    default:
        abort();
    }

    info->freq = as->freq;
    info->bits = bits;
    info->sign = sign;
    info->nchannels = as->nchannels;
    info->shift = (as->nchannels == 2) + shift;
    info->align = (1 << info->shift) - 1;
    info->bytes_per_second = info->freq << info->shift;
    info->swap_endianness = (as->endianness != AUDIO_HOST_ENDIANNESS);
}

void audio_pcm_info_clear_buf (struct audio_pcm_info *info, void *buf, int len)
{
    if (!len) {
        return;
    }

    if (info->sign) {
        memset (buf, 0x00, len << info->shift);
    }
    else {
        switch (info->bits) {
        case 8:
            memset (buf, 0x80, len << info->shift);
            break;

        case 16:
            {
                int i;
                uint16_t *p = buf;
                int shift = info->nchannels - 1;
                short s = INT16_MAX;

                if (info->swap_endianness) {
                    s = bswap16 (s);
                }

                for (i = 0; i < len << shift; i++) {
                    p[i] = s;
                }
            }
            break;

        case 32:
            {
                int i;
                uint32_t *p = buf;
                int shift = info->nchannels - 1;
                int32_t s = INT32_MAX;

                if (info->swap_endianness) {
                    s = bswap32 (s);
                }

                for (i = 0; i < len << shift; i++) {
                    p[i] = s;
                }
            }
            break;

        default:
            AUD_log (NULL, "audio_pcm_info_clear_buf: invalid bits %d\n",
                     info->bits);
            break;
        }
    }
}

/*
 * Capture
 */
static void noop_conv (struct st_sample *dst, const void *src, int samples)
{
    (void) src;
    (void) dst;
    (void) samples;
}

static CaptureVoiceOut *audio_pcm_capture_find_specific(AudioState *s,
                                                        struct audsettings *as)
{
    CaptureVoiceOut *cap;

    for (cap = s->cap_head.lh_first; cap; cap = cap->entries.le_next) {
        if (audio_pcm_info_eq (&cap->hw.info, as)) {
            return cap;
        }
    }
    return NULL;
}

static void audio_notify_capture (CaptureVoiceOut *cap, audcnotification_e cmd)
{
    struct capture_callback *cb;

#ifdef DEBUG_CAPTURE
    dolog ("notification %d sent\n", cmd);
#endif
    for (cb = cap->cb_head.lh_first; cb; cb = cb->entries.le_next) {
        cb->ops.notify (cb->opaque, cmd);
    }
}

static void audio_capture_maybe_changed (CaptureVoiceOut *cap, int enabled)
{
    if (cap->hw.enabled != enabled) {
        audcnotification_e cmd;
        cap->hw.enabled = enabled;
        cmd = enabled ? AUD_CNOTIFY_ENABLE : AUD_CNOTIFY_DISABLE;
        audio_notify_capture (cap, cmd);
    }
}

static void audio_recalc_and_notify_capture (CaptureVoiceOut *cap)
{
    HWVoiceOut *hw = &cap->hw;
    SWVoiceOut *sw;
    int enabled = 0;

    for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
        if (sw->active) {
            enabled = 1;
            break;
        }
    }
    audio_capture_maybe_changed (cap, enabled);
}

static void audio_detach_capture (HWVoiceOut *hw)
{
    SWVoiceCap *sc = hw->cap_head.lh_first;

    while (sc) {
        SWVoiceCap *sc1 = sc->entries.le_next;
        SWVoiceOut *sw = &sc->sw;
        CaptureVoiceOut *cap = sc->cap;
        int was_active = sw->active;

        if (sw->rate) {
            st_rate_stop (sw->rate);
            sw->rate = NULL;
        }

        QLIST_REMOVE (sw, entries);
        QLIST_REMOVE (sc, entries);
        g_free (sc);
        if (was_active) {
            /* We have removed soft voice from the capture:
               this might have changed the overall status of the capture
               since this might have been the only active voice */
            audio_recalc_and_notify_capture (cap);
        }
        sc = sc1;
    }
}

static int audio_attach_capture (HWVoiceOut *hw)
{
    AudioState *s = hw->s;
    CaptureVoiceOut *cap;

    audio_detach_capture (hw);
    for (cap = s->cap_head.lh_first; cap; cap = cap->entries.le_next) {
        SWVoiceCap *sc;
        SWVoiceOut *sw;
        HWVoiceOut *hw_cap = &cap->hw;

        sc = audio_calloc (AUDIO_FUNC, 1, sizeof (*sc));
        if (!sc) {
            dolog ("Could not allocate soft capture voice (%zu bytes)\n",
                   sizeof (*sc));
            return -1;
        }

        sc->cap = cap;
        sw = &sc->sw;
        sw->hw = hw_cap;
        sw->info = hw->info;
        sw->empty = 1;
        sw->active = hw->enabled;
        sw->conv = noop_conv;
        sw->ratio = ((int64_t) hw_cap->info.freq << 32) / sw->info.freq;
        sw->vol = nominal_volume;
        sw->rate = st_rate_start (sw->info.freq, hw_cap->info.freq);
        if (!sw->rate) {
            dolog ("Could not start rate conversion for `%s'\n", SW_NAME (sw));
            g_free (sw);
            return -1;
        }
        QLIST_INSERT_HEAD (&hw_cap->sw_head, sw, entries);
        QLIST_INSERT_HEAD (&hw->cap_head, sc, entries);
#ifdef DEBUG_CAPTURE
        sw->name = g_strdup_printf ("for %p %d,%d,%d",
                                    hw, sw->info.freq, sw->info.bits,
                                    sw->info.nchannels);
        dolog ("Added %s active = %d\n", sw->name, sw->active);
#endif
        if (sw->active) {
            audio_capture_maybe_changed (cap, 1);
        }
    }
    return 0;
}

/*
 * Hard voice (capture)
 */
static size_t audio_pcm_hw_find_min_in (HWVoiceIn *hw)
{
    SWVoiceIn *sw;
    size_t m = hw->total_samples_captured;

    for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
        if (sw->active) {
            m = audio_MIN (m, sw->total_hw_samples_acquired);
        }
    }
    return m;
}

static size_t audio_pcm_hw_get_live_in(HWVoiceIn *hw)
{
    size_t live = hw->total_samples_captured - audio_pcm_hw_find_min_in (hw);
    if (audio_bug (AUDIO_FUNC, live > hw->conv_buf->size)) {
        dolog("live=%zu samples=%zu\n", live, hw->conv_buf->size);
        return 0;
    }
    return live;
}

static void audio_pcm_hw_clip_out(HWVoiceOut *hw, void *pcm_buf,
                                  size_t len)
{
    size_t clipped = 0;
    size_t pos = hw->mix_buf->pos;

    while (len) {
        st_sample *src = hw->mix_buf->samples + pos;
        uint8_t *dst = advance (pcm_buf, clipped << hw->info.shift);
        size_t samples_till_end_of_buf = hw->mix_buf->size - pos;
        size_t samples_to_clip = audio_MIN(len, samples_till_end_of_buf);

        hw->clip (dst, src, samples_to_clip);

        pos = (pos + samples_to_clip) % hw->mix_buf->size;
        len -= samples_to_clip;
        clipped += samples_to_clip;
    }
}

/*
 * Soft voice (capture)
 */
static size_t audio_pcm_sw_get_rpos_in(SWVoiceIn *sw)
{
    HWVoiceIn *hw = sw->hw;
    ssize_t live = hw->total_samples_captured - sw->total_hw_samples_acquired;
    ssize_t rpos;

    if (audio_bug(AUDIO_FUNC, live < 0 || live > hw->conv_buf->size)) {
        dolog("live=%zd samples=%zu\n", live, hw->conv_buf->size);
        return 0;
    }

    rpos = hw->conv_buf->pos - live;
    if (rpos >= 0) {
        return rpos;
    }
    else {
        return hw->conv_buf->size + rpos;
    }
}

static size_t audio_pcm_sw_read(SWVoiceIn *sw, void *buf, size_t size)
{
    HWVoiceIn *hw = sw->hw;
    size_t samples, live, ret = 0, swlim, isamp, osamp, rpos, total = 0;
    struct st_sample *src, *dst = sw->buf;

    rpos = audio_pcm_sw_get_rpos_in (sw) % hw->conv_buf->size;

    live = hw->total_samples_captured - sw->total_hw_samples_acquired;
    if (audio_bug (AUDIO_FUNC, live > hw->conv_buf->size)) {
        dolog("live_in=%zu samples=%zu\n", live, hw->conv_buf->size);
        return 0;
    }

    samples = size >> sw->info.shift;
    if (!live) {
        return 0;
    }

    swlim = (live * sw->ratio) >> 32;
    swlim = audio_MIN (swlim, samples);

    while (swlim) {
        src = hw->conv_buf->samples + rpos;
        if (hw->conv_buf->pos > rpos) {
            isamp = hw->conv_buf->pos - rpos;
        } else {
            isamp = hw->conv_buf->size - rpos;
        }

        if (!isamp) {
            break;
        }
        osamp = swlim;

        st_rate_flow (sw->rate, src, dst, &isamp, &osamp);
        swlim -= osamp;
        rpos = (rpos + isamp) % hw->conv_buf->size;
        dst += osamp;
        ret += osamp;
        total += isamp;
    }

    if (!(hw->ctl_caps & VOICE_VOLUME_CAP)) {
        mixeng_volume (sw->buf, ret, &sw->vol);
    }

    sw->clip (buf, sw->buf, ret);
    sw->total_hw_samples_acquired += total;
    return ret << sw->info.shift;
}

/*
 * Hard voice (playback)
 */
static size_t audio_pcm_hw_find_min_out (HWVoiceOut *hw, int *nb_livep)
{
    SWVoiceOut *sw;
    size_t m = SIZE_MAX;
    int nb_live = 0;

    for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
        if (sw->active || !sw->empty) {
            m = audio_MIN (m, sw->total_hw_samples_mixed);
            nb_live += 1;
        }
    }

    *nb_livep = nb_live;
    return m;
}

static size_t audio_pcm_hw_get_live_out (HWVoiceOut *hw, int *nb_live)
{
    size_t smin;
    int nb_live1;

    smin = audio_pcm_hw_find_min_out (hw, &nb_live1);
    if (nb_live) {
        *nb_live = nb_live1;
    }

    if (nb_live1) {
        size_t live = smin;

        if (audio_bug(AUDIO_FUNC, live > hw->mix_buf->size)) {
            dolog("live=%zu hw->samples=%zu\n", live, hw->mix_buf->size);
            return 0;
        }
        return live;
    }
    return 0;
}

/*
 * Soft voice (playback)
 */
static size_t audio_pcm_sw_write(SWVoiceOut *sw, void *buf, size_t size)
{
    size_t hwsamples, samples, isamp, osamp, wpos, live, dead, left, swlim, blck;
    int ret = 0;
    size_t pos = 0, total = 0;

    if (!sw) {
        return size;
    }

    hwsamples = sw->hw->mix_buf->size;

    live = sw->total_hw_samples_mixed;
    if (audio_bug (AUDIO_FUNC, live > hwsamples)){
        dolog("live=%zu samples=%zu\n", live, hwsamples);
        return 0;
    }

    if (live == hwsamples) {
#ifdef DEBUG_OUT
        dolog ("%s is full %d\n", sw->name, live);
#endif
        return 0;
    }

    wpos = (sw->hw->mix_buf->pos + live) % hwsamples;
    samples = size >> sw->info.shift;

    dead = hwsamples - live;
    swlim = ((int64_t) dead << 32) / sw->ratio;
    swlim = audio_MIN (swlim, samples);
    if (swlim) {
        sw->conv (sw->buf, buf, swlim);

        if (!(sw->hw->ctl_caps & VOICE_VOLUME_CAP)) {
            mixeng_volume (sw->buf, swlim, &sw->vol);
        }
    }

    while (swlim) {
        dead = hwsamples - live;
        left = hwsamples - wpos;
        blck = audio_MIN (dead, left);
        if (!blck) {
            break;
        }
        isamp = swlim;
        osamp = blck;
        st_rate_flow_mix (
            sw->rate,
            sw->buf + pos,
            sw->hw->mix_buf->samples + wpos,
            &isamp,
            &osamp
            );
        ret += isamp;
        swlim -= isamp;
        pos += isamp;
        live += osamp;
        wpos = (wpos + osamp) % hwsamples;
        total += osamp;
    }

    sw->total_hw_samples_mixed += total;
    sw->empty = sw->total_hw_samples_mixed == 0;

#ifdef DEBUG_OUT
    dolog (
        "%s: write size %d ret %d total sw %d\n",
        SW_NAME (sw),
        size >> sw->info.shift,
        ret,
        sw->total_hw_samples_mixed
        );
#endif

    return ret << sw->info.shift;
}

#ifdef DEBUG_AUDIO
static void audio_pcm_print_info (const char *cap, struct audio_pcm_info *info)
{
    dolog ("%s: bits %d, sign %d, freq %d, nchan %d\n",
           cap, info->bits, info->sign, info->freq, info->nchannels);
}
#endif

#define DAC
#include "audio_template.h"
#undef DAC
#include "audio_template.h"

/*
 * Timer
 */
static int audio_is_timer_needed(AudioState *s)
{
    HWVoiceIn *hwi = NULL;
    HWVoiceOut *hwo = NULL;

    while ((hwo = audio_pcm_hw_find_any_enabled_out(s, hwo))) {
        if (!hwo->poll_mode) return 1;
    }
    while ((hwi = audio_pcm_hw_find_any_enabled_in(s, hwi))) {
        if (!hwi->poll_mode) return 1;
    }
    return 0;
}

static void audio_reset_timer (AudioState *s)
{
    if (audio_is_timer_needed(s)) {
        timer_mod (s->ts,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->period_ticks);
    }
    else {
        timer_del (s->ts);
    }
}

static void audio_timer (void *opaque)
{
    AudioState *s = opaque;
    audio_run(s, "timer");
    audio_reset_timer(s);
}

/*
 * Public API
 */
size_t AUD_write(SWVoiceOut *sw, void *buf, size_t size)
{
    HWVoiceOut *hw;

    if (!sw) {
        /* XXX: Consider options */
        return size;
    }
    hw = sw->hw;

    if (!hw->enabled) {
        dolog ("Writing to disabled voice %s\n", SW_NAME (sw));
        return 0;
    }

    if (hw->s->dev->out->mixeng) {
        return audio_pcm_sw_write(sw, buf, size);
    } else {
        return hw->pcm_ops->write(hw, buf, size);
    }
}

size_t AUD_read(SWVoiceIn *sw, void *buf, size_t size)
{
    HWVoiceIn *hw;

    if (!sw) {
        /* XXX: Consider options */
        return size;
    }
    hw = sw->hw;

    if (!hw->enabled) {
        dolog ("Reading from disabled voice %s\n", SW_NAME (sw));
        return 0;
    }

    if (hw->s->dev->in->mixeng) {
        return audio_pcm_sw_read(sw, buf, size);
    } else {
        return hw->pcm_ops->read(hw, buf, size);
    }
}

int AUD_get_buffer_size_out (SWVoiceOut *sw)
{
    return sw->hw->mix_buf->size << sw->hw->info.shift;
}

void AUD_set_active_out (SWVoiceOut *sw, int on)
{
    HWVoiceOut *hw;

    if (!sw) {
        return;
    }

    hw = sw->hw;
    if (sw->active != on) {
        AudioState *s = sw->s;
        SWVoiceOut *temp_sw;
        SWVoiceCap *sc;

        if (on) {
            hw->pending_disable = 0;
            if (!hw->enabled) {
                hw->enabled = 1;
                if (s->vm_running) {
                    hw->pcm_ops->ctl_out (hw, VOICE_ENABLE);
                    audio_reset_timer (s);
                }
            }
        }
        else {
            if (hw->enabled) {
                int nb_active = 0;

                for (temp_sw = hw->sw_head.lh_first; temp_sw;
                     temp_sw = temp_sw->entries.le_next) {
                    nb_active += temp_sw->active != 0;
                }

                hw->pending_disable = nb_active == 1;
            }
        }

        for (sc = hw->cap_head.lh_first; sc; sc = sc->entries.le_next) {
            sc->sw.active = hw->enabled;
            if (hw->enabled) {
                audio_capture_maybe_changed (sc->cap, 1);
            }
        }
        sw->active = on;
    }
}

void AUD_set_active_in (SWVoiceIn *sw, int on)
{
    HWVoiceIn *hw;

    if (!sw) {
        return;
    }

    hw = sw->hw;
    if (sw->active != on) {
        AudioState *s = sw->s;
        SWVoiceIn *temp_sw;

        if (on) {
            if (!hw->enabled) {
                hw->enabled = 1;
                if (s->vm_running) {
                    hw->pcm_ops->ctl_in (hw, VOICE_ENABLE);
                    audio_reset_timer (s);
                }
            }
            sw->total_hw_samples_acquired = hw->total_samples_captured;
        }
        else {
            if (hw->enabled) {
                int nb_active = 0;

                for (temp_sw = hw->sw_head.lh_first; temp_sw;
                     temp_sw = temp_sw->entries.le_next) {
                    nb_active += temp_sw->active != 0;
                }

                if (nb_active == 1) {
                    hw->enabled = 0;
                    hw->pcm_ops->ctl_in (hw, VOICE_DISABLE);
                }
            }
        }
        sw->active = on;
    }
}

static size_t audio_get_avail (SWVoiceIn *sw)
{
    size_t live;

    if (!sw) {
        return 0;
    }

    live = sw->hw->total_samples_captured - sw->total_hw_samples_acquired;
    if (audio_bug (AUDIO_FUNC, live > sw->hw->conv_buf->size)) {
        dolog("live=%zu samples=%zu\n", live, sw->hw->conv_buf->size);
        return 0;
    }

    ldebug (
        "%s: get_avail live %d ret %" PRId64 "\n",
        SW_NAME (sw),
        live, (((int64_t) live << 32) / sw->ratio) << sw->info.shift
        );

    return (((int64_t) live << 32) / sw->ratio) << sw->info.shift;
}

static size_t audio_get_free(SWVoiceOut *sw)
{
    size_t live, dead;

    if (!sw) {
        return 0;
    }

    live = sw->total_hw_samples_mixed;

    if (audio_bug (AUDIO_FUNC, live > sw->hw->mix_buf->size)) {
        dolog("live=%zu samples=%zu\n", live, sw->hw->mix_buf->size);
        return 0;
    }

    dead = sw->hw->mix_buf->size - live;

#ifdef DEBUG_OUT
    dolog ("%s: get_free live %d dead %d ret %" PRId64 "\n",
           SW_NAME (sw),
           live, dead, (((int64_t) dead << 32) / sw->ratio) << sw->info.shift);
#endif

    return (((int64_t) dead << 32) / sw->ratio) << sw->info.shift;
}

static void audio_capture_mix_and_clear(HWVoiceOut *hw, size_t rpos,
                                        size_t samples)
{
    size_t n;

    if (hw->enabled) {
        SWVoiceCap *sc;

        for (sc = hw->cap_head.lh_first; sc; sc = sc->entries.le_next) {
            SWVoiceOut *sw = &sc->sw;
            int rpos2 = rpos;

            n = samples;
            while (n) {
                size_t till_end_of_hw = hw->mix_buf->size - rpos2;
                size_t to_write = audio_MIN (till_end_of_hw, n);
                size_t bytes = to_write << hw->info.shift;
                size_t written;

                sw->buf = hw->mix_buf->samples + rpos2;
                written = audio_pcm_sw_write (sw, NULL, bytes);
                if (written - bytes) {
                    dolog("Could not mix %zu bytes into a capture "
                          "buffer, mixed %zu\n",
                          bytes, written);
                    break;
                }
                n -= to_write;
                rpos2 = (rpos2 + to_write) % hw->mix_buf->size;
            }
        }
    }

    n = audio_MIN(samples, hw->mix_buf->size - rpos);
    mixeng_clear(hw->mix_buf->samples + rpos, n);
    mixeng_clear(hw->mix_buf->samples, samples - n);
}

static size_t audio_pcm_hw_run_out(HWVoiceOut *hw, size_t live)
{
    size_t clipped = 0;

    while (live) {
        size_t size, decr, proc;
        void *buf = hw->pcm_ops->get_buffer_out(hw, &size);

        decr = audio_MIN(size >> hw->info.shift, live);
        audio_pcm_hw_clip_out(hw, buf, decr);
        proc = hw->pcm_ops->put_buffer_out(hw, buf, decr << hw->info.shift) >>
            hw->info.shift;

        live -= proc;
        clipped += proc;
        hw->mix_buf->pos = (hw->mix_buf->pos + proc) % hw->mix_buf->size;

        if (proc == 0 || proc < decr) {
            break;
        }
    }

    return clipped;
}

static void audio_run_out (AudioState *s)
{
    HWVoiceOut *hw = NULL;
    SWVoiceOut *sw;

    if (!s->dev->out->mixeng) {
        while ((hw = audio_pcm_hw_find_any_enabled_out(s, hw))) {
            /* there is exactly 1 sw for each hw with no mixeng */
            sw = hw->sw_head.lh_first;

            if (hw->pending_disable) {
                hw->enabled = 0;
                hw->pending_disable = 0;
                if (hw->pcm_ops->enable_out) {
                    hw->pcm_ops->enable_out(hw, false);
                }
            }

            if (sw->active) {
                sw->callback.fn(sw->callback.opaque, INT_MAX);
            }
        }
        return;
    }

    while ((hw = audio_pcm_hw_find_any_enabled_out(s, hw))) {
        size_t played, live, prev_rpos, free;
        int nb_live, cleanup_required;

        live = audio_pcm_hw_get_live_out (hw, &nb_live);
        if (!nb_live) {
            live = 0;
        }

        if (audio_bug(AUDIO_FUNC, live > hw->mix_buf->size)) {
            dolog("live=%zu samples=%zu\n", live, hw->mix_buf->size);
            continue;
        }

        if (hw->pending_disable && !nb_live) {
            SWVoiceCap *sc;
#ifdef DEBUG_OUT
            dolog ("Disabling voice\n");
#endif
            hw->enabled = 0;
            hw->pending_disable = 0;
            hw->pcm_ops->ctl_out (hw, VOICE_DISABLE);
            for (sc = hw->cap_head.lh_first; sc; sc = sc->entries.le_next) {
                sc->sw.active = 0;
                audio_recalc_and_notify_capture (sc->cap);
            }
            continue;
        }

        if (!live) {
            for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
                if (sw->active) {
                    free = audio_get_free (sw);
                    if (free > 0) {
                        sw->callback.fn (sw->callback.opaque, free);
                    }
                }
            }
            continue;
        }

        prev_rpos = hw->mix_buf->pos;
        played = audio_pcm_hw_run_out(hw, live);
        if (audio_bug(AUDIO_FUNC, hw->mix_buf->pos >= hw->mix_buf->size)) {
            dolog("rpos=%zu samples=%zu played=%zu\n",
                  hw->mix_buf->pos, hw->mix_buf->size, played);
            hw->mix_buf->pos = 0;
        }

#ifdef DEBUG_OUT
        dolog ("played=%zu\n", played);
#endif

        if (played) {
            hw->ts_helper += played;
            audio_capture_mix_and_clear (hw, prev_rpos, played);
        }

        cleanup_required = 0;
        for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
            if (!sw->active && sw->empty) {
                continue;
            }

            if (audio_bug (AUDIO_FUNC, played > sw->total_hw_samples_mixed)) {
                dolog ("played=%zu sw->total_hw_samples_mixed=%zu\n",
                       played, sw->total_hw_samples_mixed);
                played = sw->total_hw_samples_mixed;
            }

            sw->total_hw_samples_mixed -= played;

            if (!sw->total_hw_samples_mixed) {
                sw->empty = 1;
                cleanup_required |= !sw->active && !sw->callback.fn;
            }

            if (sw->active) {
                free = audio_get_free (sw);
                if (free > 0) {
                    sw->callback.fn (sw->callback.opaque, free);
                }
            }
        }

        if (cleanup_required) {
            SWVoiceOut *sw1;

            sw = hw->sw_head.lh_first;
            while (sw) {
                sw1 = sw->entries.le_next;
                if (!sw->active && !sw->callback.fn) {
                    audio_close_out (sw);
                }
                sw = sw1;
            }
        }
    }
}

static size_t audio_pcm_hw_run_in(HWVoiceIn *hw, size_t samples)
{
    size_t conv = 0;
    STSampleBuffer *conv_buf = hw->conv_buf;

    while (samples) {
        size_t proc;
        size_t size = samples * hw->info.bytes_per_frame;
        void *buf = hw->pcm_ops->get_buffer_in(hw, &size);

        assert(size % hw->info.bytes_per_frame == 0);
        if (size == 0) {
            hw->pcm_ops->put_buffer_in(hw, buf, size);
            break;
        }

        proc = audio_MIN(size / hw->info.bytes_per_frame,
                         conv_buf->size - conv_buf->pos);

        hw->conv(conv_buf->samples + conv_buf->pos, buf, proc);
        conv_buf->pos = (conv_buf->pos + proc) % conv_buf->size;

        samples -= proc;
        conv += proc;
        hw->pcm_ops->put_buffer_in(hw, buf, proc * hw->info.bytes_per_frame);
    }

    return conv;
}

static void audio_run_in (AudioState *s)
{
    HWVoiceIn *hw = NULL;

    if (!s->dev->in->mixeng) {
        while ((hw = audio_pcm_hw_find_any_enabled_in(s, hw))) {
            /* there is exactly 1 sw for each hw with no mixeng */
            SWVoiceIn *sw = hw->sw_head.lh_first;
            if (sw->active) {
                sw->callback.fn(sw->callback.opaque, INT_MAX);
            }
        }
        return;
    }

    while ((hw = audio_pcm_hw_find_any_enabled_in(s, hw))) {
        SWVoiceIn *sw;
        size_t captured, min;

        captured = audio_pcm_hw_run_in(
            hw, hw->conv_buf->size -  audio_pcm_hw_get_live_in(hw));

        min = audio_pcm_hw_find_min_in (hw);
        hw->total_samples_captured += captured - min;
        hw->ts_helper += captured;

        for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
            sw->total_hw_samples_acquired -= min;

            if (sw->active) {
                size_t avail;

                avail = audio_get_avail (sw);
                if (avail > 0) {
                    sw->callback.fn (sw->callback.opaque, avail);
                }
            }
        }
    }
}

static void audio_run_capture (AudioState *s)
{
    CaptureVoiceOut *cap;

    for (cap = s->cap_head.lh_first; cap; cap = cap->entries.le_next) {
        size_t live, rpos, captured;
        HWVoiceOut *hw = &cap->hw;
        SWVoiceOut *sw;

        captured = live = audio_pcm_hw_get_live_out (hw, NULL);
        rpos = hw->mix_buf->pos;
        while (live) {
            size_t left = hw->mix_buf->size - rpos;
            size_t to_capture = audio_MIN (live, left);
            struct st_sample *src;
            struct capture_callback *cb;

            src = hw->mix_buf->samples + rpos;
            hw->clip (cap->buf, src, to_capture);
            mixeng_clear (src, to_capture);

            for (cb = cap->cb_head.lh_first; cb; cb = cb->entries.le_next) {
                cb->ops.capture (cb->opaque, cap->buf,
                                 to_capture << hw->info.shift);
            }
            rpos = (rpos + to_capture) % hw->mix_buf->size;
            live -= to_capture;
        }
        hw->mix_buf->pos = rpos;

        for (sw = hw->sw_head.lh_first; sw; sw = sw->entries.le_next) {
            if (!sw->active && sw->empty) {
                continue;
            }

            if (audio_bug (AUDIO_FUNC, captured > sw->total_hw_samples_mixed)) {
                dolog("captured=%zu sw->total_hw_samples_mixed=%zu\n",
                      captured, sw->total_hw_samples_mixed);
                captured = sw->total_hw_samples_mixed;
            }

            sw->total_hw_samples_mixed -= captured;
            sw->empty = sw->total_hw_samples_mixed == 0;
        }
    }
}

void audio_run(AudioState *s, const char *msg)
{
    audio_run_out(s);
    audio_run_in(s);
    audio_run_capture(s);

#ifdef DEBUG_POLL
    {
        static double prevtime;
        double currtime;
        struct timeval tv;

        if (gettimeofday (&tv, NULL)) {
            perror ("audio_run: gettimeofday");
            return;
        }

        currtime = tv.tv_sec + tv.tv_usec * 1e-6;
        dolog ("Elapsed since last %s: %f\n", msg, currtime - prevtime);
        prevtime = currtime;
    }
#endif
}

void *audio_generic_get_buffer_in(HWVoiceIn *hw, size_t *size)
{
    ssize_t start;

    if (unlikely(!hw->buf_emul)) {
        size_t calc_size = hw->conv_buf->size << hw->info.shift;
        hw->buf_emul = g_malloc(calc_size);
        hw->size_emul = calc_size;
        hw->pos_emul = hw->pending_emul = 0;
    }

    while (hw->pending_emul < hw->size_emul) {
        size_t read_len = audio_MIN(hw->size_emul - hw->pos_emul,
                                    hw->size_emul - hw->pending_emul);
        size_t read = hw->pcm_ops->read(hw, hw->buf_emul + hw->pos_emul,
                                        read_len);
        hw->pending_emul += read;
        if (read < read_len) {
            break;
        }
    }

    start = ((ssize_t) hw->pos_emul) - hw->pending_emul;
    if (start < 0) {
        start += hw->size_emul;
    }
    assert(start >= 0 && start < hw->size_emul);

    *size = audio_MIN(hw->pending_emul, hw->size_emul - start);
    return hw->buf_emul + start;
}

void audio_generic_put_buffer_in(HWVoiceIn *hw, void *buf, size_t size)
{
    assert(size <= hw->pending_emul);
    hw->pending_emul -= size;
}

void *audio_generic_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    if (unlikely(!hw->buf_emul)) {
        size_t calc_size = hw->mix_buf->size << hw->info.shift;

        hw->buf_emul = g_malloc(calc_size);
        hw->size_emul = calc_size;
        hw->pos_emul = hw->pending_emul = 0;
    }

    *size = audio_MIN(hw->size_emul - hw->pending_emul,
                      hw->size_emul - hw->pos_emul);
    return hw->buf_emul + hw->pos_emul;
}

size_t audio_generic_put_buffer_out_nowrite(HWVoiceOut *hw, void *buf,
                                            size_t size)
{
    assert(buf == hw->buf_emul + hw->pos_emul &&
           size + hw->pending_emul <= hw->size_emul);

    hw->pending_emul += size;
    hw->pos_emul = (hw->pos_emul + size) % hw->size_emul;

    return size;
}

size_t audio_generic_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
    audio_generic_put_buffer_out_nowrite(hw, buf, size);

    while (hw->pending_emul) {
        size_t write_len, written;
        ssize_t start = ((ssize_t) hw->pos_emul) - hw->pending_emul;
        if (start < 0) {
            start += hw->size_emul;
        }
        assert(start >= 0 && start < hw->size_emul);

        write_len = audio_MIN(hw->pending_emul, hw->size_emul - start);

        written = hw->pcm_ops->write(hw, hw->buf_emul + start, write_len);
        hw->pending_emul -= written;

        if (written < write_len) {
            break;
        }
    }

    /* fake we have written everything. non-written data remain in pending_emul,
     * so we do not have to clip them multiple times */
    return size;
}

size_t audio_generic_write(HWVoiceOut *hw, void *buf, size_t size)
{
    size_t dst_size, copy_size;
    void *dst = hw->pcm_ops->get_buffer_out(hw, &dst_size);
    copy_size = audio_MIN(size, dst_size);

    memcpy(dst, buf, copy_size);
    return hw->pcm_ops->put_buffer_out(hw, buf, copy_size);
}

size_t audio_generic_read(HWVoiceIn *hw, void *buf, size_t size)
{
    size_t dst_size, copy_size;
    void *dst = hw->pcm_ops->get_buffer_in(hw, &dst_size);
    copy_size = audio_MIN(size, dst_size);

    memcpy(dst, buf, copy_size);
    hw->pcm_ops->put_buffer_in(hw, buf, copy_size);
    return copy_size;
}


static int audio_driver_init(AudioState *s, struct audio_driver *drv,
                             Audiodev *dev)
{
    s->drv_opaque = drv->init(dev);

    if (s->drv_opaque) {
        if (!drv->pcm_ops->get_buffer_in) {
            drv->pcm_ops->get_buffer_in = audio_generic_get_buffer_in;
            drv->pcm_ops->put_buffer_in = audio_generic_put_buffer_in;
        }
        if (!drv->pcm_ops->get_buffer_out) {
            drv->pcm_ops->get_buffer_out = audio_generic_get_buffer_out;
            drv->pcm_ops->put_buffer_out = audio_generic_put_buffer_out;
        }

        audio_init_nb_voices_out(s, drv);
        audio_init_nb_voices_in(s, drv);
        s->drv = drv;
        return 0;
    }
    else {
        dolog ("Could not init `%s' audio driver\n", drv->name);
        return -1;
    }
}

static void audio_vm_change_state_handler (void *opaque, int running,
                                           RunState state)
{
    AudioState *s = opaque;
    HWVoiceOut *hwo = NULL;
    HWVoiceIn *hwi = NULL;
    int op = running ? VOICE_ENABLE : VOICE_DISABLE;

    s->vm_running = running;
    while ((hwo = audio_pcm_hw_find_any_enabled_out(s, hwo))) {
        hwo->pcm_ops->ctl_out (hwo, op);
    }

    while ((hwi = audio_pcm_hw_find_any_enabled_in(s, hwi))) {
        hwi->pcm_ops->ctl_in (hwi, op);
    }
    audio_reset_timer (s);
}

static void free_audio_state(AudioState *s)
{
    HWVoiceOut *hwo = NULL;
    HWVoiceIn *hwi = NULL;

    while ((hwo = audio_pcm_hw_find_any_out(s, hwo))) {
        SWVoiceCap *sc;

        if (hwo->enabled) {
            hwo->pcm_ops->ctl_out (hwo, VOICE_DISABLE);
        }
        hwo->pcm_ops->fini_out (hwo);

        for (sc = hwo->cap_head.lh_first; sc; sc = sc->entries.le_next) {
            CaptureVoiceOut *cap = sc->cap;
            struct capture_callback *cb;

            for (cb = cap->cb_head.lh_first; cb; cb = cb->entries.le_next) {
                cb->ops.destroy (cb->opaque);
            }
        }
    }

    while ((hwi = audio_pcm_hw_find_any_in(s, hwi))) {
        if (hwi->enabled) {
            hwi->pcm_ops->ctl_in (hwi, VOICE_DISABLE);
        }
        hwi->pcm_ops->fini_in (hwi);
    }

    if (s->drv) {
        s->drv->fini (s->drv_opaque);
    }

    qapi_free_Audiodev(s->dev);
    g_free(s);
}

static void audio_atexit(void)
{
    while (!QTAILQ_EMPTY(&audio_states)) {
        AudioState *s = QTAILQ_FIRST(&audio_states);
        QTAILQ_REMOVE(&audio_states, s, list);
        free_audio_state(s);
    }
}

static const VMStateDescription vmstate_audio = {
    .name = "audio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

static Audiodev *parse_option(QemuOpts *opts, Error **errp);
static AudioState *audio_init(Audiodev *dev)
{
    static bool atexit_registered;
    size_t i;
    int done = 0;
    const char *drvname = NULL;
    VMChangeStateEntry *e;
    AudioState *s;
    QemuOptsList *list = NULL; /* silence gcc warning about uninitialized
                                * variable */

    /* if we have dev, this function was called because of an -audiodev
     * argument => initialize a new state with it
     * if dev == NULL => legacy implicit initialization, return the already
     * created state or create a new one */
    if (dev) {
        drvname = AudiodevDriver_lookup[dev->kind];
    } else if (!QTAILQ_EMPTY(&audio_states)) {
        if (!legacy_config) {
            dolog("Must specify audiodev when using -audiodev\n");
            exit(1);
        }
        return QTAILQ_FIRST(&audio_states);
    } else {
        legacy_config = true;
        audio_handle_legacy_opts();
        list = qemu_find_opts("audiodev");
        dev = parse_option(QTAILQ_FIRST(&list->head), &error_abort);
        if (!dev) {
            exit(1);
        }
    }

    s = g_malloc0(sizeof(AudioState));
    s->dev = dev;

    QLIST_INIT (&s->hw_head_out);
    QLIST_INIT (&s->hw_head_in);
    QLIST_INIT (&s->cap_head);
    if (!atexit_registered) {
        atexit(audio_atexit);
        atexit_registered = true;
    }
    QTAILQ_INSERT_TAIL(&audio_states, s, list);

    s->ts = timer_new_ns(QEMU_CLOCK_VIRTUAL, audio_timer, s);
    if (!s->ts) {
        hw_error("Could not create audio timer\n");
    }

    s->nb_hw_voices_out = dev->out->voices;
    s->nb_hw_voices_in = dev->in->voices;

    if (s->nb_hw_voices_out <= 0) {
        dolog ("Bogus number of playback voices %d, setting to 1\n",
               s->nb_hw_voices_out);
        s->nb_hw_voices_out = 1;
    }

    if (s->nb_hw_voices_in <= 0) {
        dolog ("Bogus number of capture voices %d, setting to 0\n",
               s->nb_hw_voices_in);
        s->nb_hw_voices_in = 0;
    }

    if (drvname) {
        int found = 0;

        for (i = 0; drvtab[i]; i++) {
            if (!strcmp (drvname, drvtab[i]->name)) {
                done = !audio_driver_init (s, drvtab[i], dev);
                found = 1;
                break;
            }
        }

        if (!found) {
            dolog ("Unknown audio driver `%s'\n", drvname);
        }
    } else {
        for (i = 0; !done && drvtab[i]; i++) {
            QemuOpts *opts = qemu_opts_find(list, drvtab[i]->name);
            if (opts) {
                qapi_free_Audiodev(dev);
                dev = parse_option(opts, &error_abort);
                if (!dev) {
                    exit(1);
                }
                s->dev = dev;
                done = !audio_driver_init(s, drvtab[i], dev);
            }
        }
    }

    if (!done) {
        done = !audio_driver_init (s, &no_audio_driver, dev);
        if (!done) {
            hw_error("Could not initialize audio subsystem\n");
        }
        else {
            dolog ("warning: Using timer based audio emulation\n");
        }
    }

    if (dev->timer_period <= 0) {
        if (dev->timer_period < 0) {
            dolog ("warning: Timer period is negative - %" PRId64
                   " treating as zero\n",
                   dev->timer_period);
        }
        s->period_ticks = 1;
    } else {
        s->period_ticks =
            muldiv64(dev->timer_period, get_ticks_per_sec(), 1000000);
    }

    e = qemu_add_vm_change_state_handler (audio_vm_change_state_handler, s);
    if (!e) {
        dolog ("warning: Could not register change state handler\n"
               "(Audio can continue looping even after stopping the VM)\n");
    }

    QLIST_INIT (&s->card_head);
    vmstate_register (NULL, 0, &vmstate_audio, s);
    return s;
}

void AUD_register_card (const char *name, QEMUSoundCard *card)
{
    if (!card->state) {
        card->state = audio_init(NULL);
    }

    card->name = g_strdup (name);
    memset (&card->entries, 0, sizeof (card->entries));
    QLIST_INSERT_HEAD (&card->state->card_head, card, entries);
}

void AUD_remove_card (QEMUSoundCard *card)
{
    QLIST_REMOVE (card, entries);
    g_free (card->name);
}


CaptureVoiceOut *AUD_add_capture(
    AudioState *s,
    struct audsettings *as,
    struct audio_capture_ops *ops,
    void *cb_opaque
    )
{
    CaptureVoiceOut *cap;
    struct capture_callback *cb;

    if (!s) {
        if (!legacy_config) {
            dolog("Must specify audiodev when using -audiodev\n");
            goto err0;
        }
        s = QTAILQ_FIRST(&audio_states);
    }

    if (!s->dev->out->mixeng) {
        dolog("Can't capture with mixeng disabled\n");
        goto err0;
    }

    if (audio_validate_settings (as)) {
        dolog ("Invalid settings were passed when trying to add capture\n");
        audio_print_settings (as);
        goto err0;
    }

    cb = audio_calloc (AUDIO_FUNC, 1, sizeof (*cb));
    if (!cb) {
        dolog ("Could not allocate capture callback information, size %zu\n",
               sizeof (*cb));
        goto err0;
    }
    cb->ops = *ops;
    cb->opaque = cb_opaque;

    cap = audio_pcm_capture_find_specific(s, as);
    if (cap) {
        QLIST_INSERT_HEAD (&cap->cb_head, cb, entries);
        return cap;
    }
    else {
        HWVoiceOut *hw;
        CaptureVoiceOut *cap;

        cap = audio_calloc (AUDIO_FUNC, 1, sizeof (*cap));
        if (!cap) {
            dolog ("Could not allocate capture voice, size %zu\n",
                   sizeof (*cap));
            goto err1;
        }

        hw = &cap->hw;
        hw->s = s;
        QLIST_INIT (&hw->sw_head);
        QLIST_INIT (&cap->cb_head);

        audio_pcm_hw_alloc_resources_out(hw);

        audio_pcm_init_info (&hw->info, as);

        cap->buf = audio_calloc(AUDIO_FUNC, hw->mix_buf->size,
                                1 << hw->info.shift);
        if (!cap->buf) {
            dolog ("Could not allocate capture buffer "
                   "(%zu samples, each %d bytes)\n",
                   hw->mix_buf->size, 1 << hw->info.shift);
            goto err3;
        }

        hw->clip = mixeng_clip
            [hw->info.nchannels == 2]
            [hw->info.sign]
            [hw->info.swap_endianness]
            [audio_bits_to_index (hw->info.bits)];

        QLIST_INSERT_HEAD (&s->cap_head, cap, entries);
        QLIST_INSERT_HEAD (&cap->cb_head, cb, entries);

        hw = NULL;
        while ((hw = audio_pcm_hw_find_any_out(s, hw))) {
            audio_attach_capture (hw);
        }
        return cap;

    err3:
        g_free (cap->hw.mix_buf);
        g_free (cap);
    err1:
        g_free (cb);
    err0:
        return NULL;
    }
}

void AUD_del_capture (CaptureVoiceOut *cap, void *cb_opaque)
{
    struct capture_callback *cb;

    for (cb = cap->cb_head.lh_first; cb; cb = cb->entries.le_next) {
        if (cb->opaque == cb_opaque) {
            cb->ops.destroy (cb_opaque);
            QLIST_REMOVE (cb, entries);
            g_free (cb);

            if (!cap->cb_head.lh_first) {
                SWVoiceOut *sw = cap->hw.sw_head.lh_first, *sw1;

                while (sw) {
                    SWVoiceCap *sc = (SWVoiceCap *) sw;
#ifdef DEBUG_CAPTURE
                    dolog ("freeing %s\n", sw->name);
#endif

                    sw1 = sw->entries.le_next;
                    if (sw->rate) {
                        st_rate_stop (sw->rate);
                        sw->rate = NULL;
                    }
                    QLIST_REMOVE (sw, entries);
                    QLIST_REMOVE (sc, entries);
                    g_free (sc);
                    sw = sw1;
                }
                QLIST_REMOVE (cap, entries);
                g_free (cap);
            }
            return;
        }
    }
}

void AUD_set_volume_out (SWVoiceOut *sw, int mute, uint8_t lvol, uint8_t rvol)
{
    if (sw) {
        HWVoiceOut *hw = sw->hw;

        sw->vol.mute = mute;
        sw->vol.l = nominal_volume.l * lvol / 255;
        sw->vol.r = nominal_volume.r * rvol / 255;

        if (hw->pcm_ops->ctl_out) {
            hw->pcm_ops->ctl_out (hw, VOICE_VOLUME, sw);
        }
    }
}

void AUD_set_volume_in (SWVoiceIn *sw, int mute, uint8_t lvol, uint8_t rvol)
{
    if (sw) {
        HWVoiceIn *hw = sw->hw;

        sw->vol.mute = mute;
        sw->vol.l = nominal_volume.l * lvol / 255;
        sw->vol.r = nominal_volume.r * rvol / 255;

        if (hw->pcm_ops->ctl_in) {
            hw->pcm_ops->ctl_in (hw, VOICE_VOLUME, sw);
        }
    }
}

QemuOptsList qemu_audiodev_opts = {
    .name = "audiodev",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_audiodev_opts.head),
    .implied_opt_name = "driver",
    .desc = {
        /*
         * no elements => accept any params
         * sanity checking will happen later
         */
        { /* end of list */ }
    },
};

static void validate_per_direction_opts(AudiodevPerDirectionOptions *pdo,
                                        Error **errp)
{
    if (!pdo->has_mixeng) {
        pdo->has_mixeng = true;
        pdo->mixeng = true;
    }
    if (!pdo->has_fixed_settings) {
        pdo->has_fixed_settings = true;
        pdo->fixed_settings = pdo->mixeng;
    }
    if (!pdo->fixed_settings &&
        (pdo->has_frequency || pdo->has_channels || pdo->has_format)) {
        error_setg(errp,
                   "You can't use frequency, channels or format with fixed-settings=off");
        return;
    }
    if (!pdo->mixeng && pdo->fixed_settings) {
        error_setg(errp, "You can't use fixed-settings without mixeng");
        return;
    }

    if (!pdo->has_frequency) {
        pdo->has_frequency = true;
        pdo->frequency = 44100;
    }
    if (!pdo->has_channels) {
        pdo->has_channels = true;
        pdo->channels = 2;
    }
    if (!pdo->has_voices) {
        pdo->has_voices = true;
        pdo->voices = pdo->mixeng ? 1 : INT_MAX;
    }
    if (!pdo->has_format) {
        pdo->has_format = true;
        pdo->format = AUDIO_FORMAT_S16;
    }
}

static Audiodev *parse_option(QemuOpts *opts, Error **errp)
{
    Error *local_err = NULL;
    OptsVisitor *ov = opts_visitor_new(opts);
    Audiodev *dev = NULL;
    visit_type_Audiodev(opts_get_visitor(ov), &dev, NULL, &local_err);
    opts_visitor_cleanup(ov);

    if (local_err) {
        goto err2;
    }

    validate_per_direction_opts(dev->in, &local_err);
    if (local_err) {
        goto err;
    }
    validate_per_direction_opts(dev->out, &local_err);
    if (local_err) {
        goto err;
    }

    if (!dev->has_timer_period) {
        dev->has_timer_period = true;
        dev->timer_period = 10000; /* 100Hz -> 10ms */
    }

    return dev;

err:
    qapi_free_Audiodev(dev);
err2:
    error_propagate(errp, local_err);
    return NULL;
}

static int each_option(void *opaque, QemuOpts *opts, Error **errp)
{
    Audiodev *dev = parse_option(opts, errp);
    if (!dev) {
        return -1;
    }
    return audio_init(dev) ? 0 : -1;
}

void audio_set_options(void)
{
    if (qemu_opts_foreach(qemu_find_opts("audiodev"), each_option, NULL,
                          &error_abort)) {
        exit(1);
    }
}

audsettings audiodev_to_audsettings(AudiodevPerDirectionOptions *pdo)
{
    return (audsettings) {
        .freq = pdo->frequency,
        .nchannels = pdo->channels,
        .fmt = pdo->format,
        .endianness = AUDIO_HOST_ENDIANNESS,
    };
}

int audioformat_bytes_per_sample(AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_U8:
    case AUDIO_FORMAT_S8:
        return 1;

    case AUDIO_FORMAT_U16:
    case AUDIO_FORMAT_S16:
        return 2;

    case AUDIO_FORMAT_U32:
    case AUDIO_FORMAT_S32:
        return 4;

    case AUDIO_FORMAT_MAX:
        ;
    }
    abort();
}


/* frames = freq * usec / 1e6 */
int audio_buffer_frames(AudiodevPerDirectionOptions *pdo,
                        audsettings *as, int def_usecs)
{
    uint64_t usecs = pdo->has_buffer_len ? pdo->buffer_len : def_usecs;
    return (as->freq * usecs + 500000) / 1000000;
}

/* samples = channels * frames = channels * freq * usec / 1e6 */
int audio_buffer_samples(AudiodevPerDirectionOptions *pdo,
                         audsettings *as, int def_usecs)
{
    return as->nchannels * audio_buffer_frames(pdo, as, def_usecs);
}

/* bytes = bytes_per_sample * samples =
 *   bytes_per_sample * channels * freq * usec / 1e6 */
int audio_buffer_bytes(AudiodevPerDirectionOptions *pdo,
                       audsettings *as, int def_usecs)
{
    return audio_buffer_samples(pdo, as, def_usecs) *
        audioformat_bytes_per_sample(as->fmt);
}

AudioState *audio_state_by_name(const char *name)
{
    AudioState *s;
    QTAILQ_FOREACH(s, &audio_states, list) {
        assert(s->dev);
        if (strcmp(name, s->dev->id) == 0) {
            return s;
        }
    }
    return NULL;
}

const char *audio_get_id(QEMUSoundCard *card)
{
    if (card->state) {
        assert(card->state->dev);
        return card->state->dev->id;
    } else {
        return "";
    }
}

void audio_rate_start(RateCtl *rate)
{
    memset(rate, 0, sizeof(RateCtl));
    rate->start_ticks = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

size_t audio_rate_get_bytes(struct audio_pcm_info *info, RateCtl *rate,
                            size_t bytes_avail)
{
    int64_t now;
    int64_t ticks;
    int64_t bytes;
    int64_t samples;
    size_t ret;

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ticks = now - rate->start_ticks;
    bytes = muldiv64(ticks, info->bytes_per_second, get_ticks_per_sec());
    samples = (bytes - rate->bytes_sent) >> info->shift;
    if (samples < 0 || samples > 65536) {
        AUD_log(NULL, "Resetting rate control (%" PRId64 " samples)", samples);
        audio_rate_start(rate);
        samples = 0;
    }

    ret = audio_MIN(samples << info->shift, bytes_avail);
    rate->bytes_sent += ret;
    return ret;
}
