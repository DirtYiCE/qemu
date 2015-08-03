/*
 * QEMU OS X CoreAudio audio driver
 *
 * Copyright (c) 2005 Mike Kronenberg
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

#include <CoreAudio/CoreAudio.h>
#include <string.h>             /* strerror */
#include <pthread.h>            /* pthread_X */

#include "qemu-common.h"
#include "audio.h"

#define AUDIO_CAP "coreaudio"
#include "audio_int.h"

static int isAtexit;

typedef struct coreaudioVoiceOut {
    HWVoiceOut hw;
    pthread_mutex_t mutex;
    AudioDeviceID outputDeviceID;
    UInt32 audioDevicePropertyBufferFrameSize;
    AudioStreamBasicDescription outputStreamBasicDescription;
    size_t samples;
} coreaudioVoiceOut;

static void coreaudio_logstatus (OSStatus status)
{
    const char *str = "BUG";

    switch(status) {
    case kAudioHardwareNoError:
        str = "kAudioHardwareNoError";
        break;

    case kAudioHardwareNotRunningError:
        str = "kAudioHardwareNotRunningError";
        break;

    case kAudioHardwareUnspecifiedError:
        str = "kAudioHardwareUnspecifiedError";
        break;

    case kAudioHardwareUnknownPropertyError:
        str = "kAudioHardwareUnknownPropertyError";
        break;

    case kAudioHardwareBadPropertySizeError:
        str = "kAudioHardwareBadPropertySizeError";
        break;

    case kAudioHardwareIllegalOperationError:
        str = "kAudioHardwareIllegalOperationError";
        break;

    case kAudioHardwareBadDeviceError:
        str = "kAudioHardwareBadDeviceError";
        break;

    case kAudioHardwareBadStreamError:
        str = "kAudioHardwareBadStreamError";
        break;

    case kAudioHardwareUnsupportedOperationError:
        str = "kAudioHardwareUnsupportedOperationError";
        break;

    case kAudioDeviceUnsupportedFormatError:
        str = "kAudioDeviceUnsupportedFormatError";
        break;

    case kAudioDevicePermissionsError:
        str = "kAudioDevicePermissionsError";
        break;

    default:
        AUD_log (AUDIO_CAP, "Reason: status code %" PRId32 "\n", (int32_t)status);
        return;
    }

    AUD_log (AUDIO_CAP, "Reason: %s\n", str);
}

static void GCC_FMT_ATTR (2, 3) coreaudio_logerr (
    OSStatus status,
    const char *fmt,
    ...
    )
{
    va_list ap;

    va_start (ap, fmt);
    AUD_log (AUDIO_CAP, fmt, ap);
    va_end (ap);

    coreaudio_logstatus (status);
}

static void GCC_FMT_ATTR (3, 4) coreaudio_logerr2 (
    OSStatus status,
    const char *typ,
    const char *fmt,
    ...
    )
{
    va_list ap;

    AUD_log (AUDIO_CAP, "Could not initialize %s\n", typ);

    va_start (ap, fmt);
    AUD_vlog (AUDIO_CAP, fmt, ap);
    va_end (ap);

    coreaudio_logstatus (status);
}

static inline UInt32 isPlaying (AudioDeviceID outputDeviceID)
{
    OSStatus status;
    UInt32 result = 0;
    UInt32 propertySize = sizeof(outputDeviceID);
    status = AudioDeviceGetProperty(
        outputDeviceID, 0, 0,
        kAudioDevicePropertyDeviceIsRunning, &propertySize, &result);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr(status,
                         "Could not determine whether Device is playing\n");
    }
    return result;
}

static void coreaudio_atexit (void)
{
    isAtexit = 1;
}

static int coreaudio_lock (coreaudioVoiceOut *core, const char *fn_name)
{
    int err;

    err = pthread_mutex_lock (&core->mutex);
    if (err) {
        dolog ("Could not lock voice for %s\nReason: %s\n",
               fn_name, strerror (err));
        return -1;
    }
    return 0;
}

static int coreaudio_unlock (coreaudioVoiceOut *core, const char *fn_name)
{
    int err;

    err = pthread_mutex_unlock (&core->mutex);
    if (err) {
        dolog ("Could not unlock voice for %s\nReason: %s\n",
               fn_name, strerror (err));
        return -1;
    }
    return 0;
}

#define COREAUDIO_WRAPPER_FUNC(name, ret_type, args_decl, args) \
    static ret_type glue(coreaudio_, name)args_decl             \
    {                                                           \
        coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;     \
        ret_type ret;                                           \
                                                                \
        if (coreaudio_lock(core, "coreaudio_" #name)) {         \
            return 0;                                           \
        }                                                       \
                                                                \
        ret = glue(audio_generic_, name)args;                   \
                                                                \
        coreaudio_unlock(core, "coreaudio_" #name);             \
        return ret;                                             \
    }
COREAUDIO_WRAPPER_FUNC(get_buffer_out, void *, (HWVocieOut *hw, size_t *size),
                       (hw, size))
COREAUDIO_WRAPPER_FUNC(put_buffer_out_nowrite, size_t,
                       (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
COREAUDIO_WRAPPER_FUNC(write, size_t, (HWVoiceOut *hw, void *buf, size_t size),
                       (hw, buf, size))
#undef COREAUDIO_WRAPPER_FUNC

/* callback to feed audiooutput buffer */
static OSStatus audioDeviceIOProc(
    AudioDeviceID inDevice,
    const AudioTimeStamp* inNow,
    const AudioBufferList* inInputData,
    const AudioTimeStamp* inInputTime,
    AudioBufferList* outOutputData,
    const AudioTimeStamp* inOutputTime,
    void* hwptr)
{
    UInt32 frameCount, pending_frames;
    void *out = outOutputData->mBuffers[0].mData;
    HWVoiceOut *hw = hwptr;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hwptr;
    size_t len;

    if (coreaudio_lock (core, "audioDeviceIOProc")) {
        inInputTime = 0;
        return 0;
    }

    frameCount = core->audioDevicePropertyBufferFrameSize;
    pending_frames = hw->pending_emul >> hw->info.shift;

    /* if there are not enough samples, set signal and return */
    if (pending_frames < frameCount) {
        inInputTime = 0;
        coreaudio_unlock (core, "audioDeviceIOProc(empty)");
        return 0;
    }

    len = frameCount << hw->info.shift;
    while (len) {
        size_t write_len;
        ssize_t start = ((ssize_t) hw->pos_emul) - hw->pending_emul;
        if (start < 0) {
            start += hw->size_emul;
        }
        assert(start >= 0 && start < hw->size_emul);

        write_len = audio_MIN(audio_MIN(hw->pending_emul, len),
                              hw->size_emul - start);

        memcpy(out, hw->buf_emul + start, write_len);
        hw->pending_emul -= write_len;
        len -= write_len;
        out += write_len;
    }

    coreaudio_unlock (core, "audioDeviceIOProc");
    return 0;
}

static UInt32 coreaudio_get_flags(struct audio_pcm_info *info,
                                  struct audsettings *as)
{
    UInt32 flags = info->sign ? kAudioFormatFlagIsSignedInteger : 0;
    if (as->endianness) { /* 0 = little, 1 = big */
        flags |= kAudioFormatFlagIsBigEndian;
    }

    if (flags == 0) { /* must not be 0 */
        flags = kAudioFormatFlagsAreAllClear;
    }
    return flags;
}

static int coreaudio_init_out(HWVoiceOut *hw, struct audsettings *as,
                              void *drv_opaque)
{
    OSStatus status;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;
    UInt32 propertySize;
    int err;
    const char *typ = "playback";
    AudioValueRange frameRange;
    Audiodev *dev = drv_opaque;
    AudiodevPerDirectionOptions *pdo = dev->out;
    int frames;

    /* create mutex */
    err = pthread_mutex_init(&core->mutex, NULL);
    if (err) {
        dolog("Could not create mutex\nReason: %s\n", strerror (err));
        return -1;
    }

    audio_pcm_init_info (&hw->info, as);

    /* open default output device */
    propertySize = sizeof(core->outputDeviceID);
    status = AudioHardwareGetProperty(
        kAudioHardwarePropertyDefaultOutputDevice,
        &propertySize,
        &core->outputDeviceID);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ,
                           "Could not get default output Device\n");
        return -1;
    }
    if (core->outputDeviceID == kAudioDeviceUnknown) {
        dolog ("Could not initialize %s - Unknown Audiodevice\n", typ);
        return -1;
    }

    /* get minimum and maximum buffer frame sizes */
    propertySize = sizeof(frameRange);
    status = AudioDeviceGetProperty(
        core->outputDeviceID,
        0,
        0,
        kAudioDevicePropertyBufferFrameSizeRange,
        &propertySize,
        &frameRange);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ,
                           "Could not get device buffer frame range\n");
        return -1;
    }

    frames = audio_buffer_frames(pdo, as, 11610);
    if (frameRange.mMinimum > frames) {
        core->audioDevicePropertyBufferFrameSize = (UInt32) frameRange.mMinimum;
        dolog ("warning: Upsizing Buffer Frames to %f\n", frameRange.mMinimum);
    }
    else if (frameRange.mMaximum < frames) {
        core->audioDevicePropertyBufferFrameSize = (UInt32) frameRange.mMaximum;
        dolog ("warning: Downsizing Buffer Frames to %f\n", frameRange.mMaximum);
    }
    else {
        core->audioDevicePropertyBufferFrameSize = frames;
    }

    /* set Buffer Frame Size */
    propertySize = sizeof(core->audioDevicePropertyBufferFrameSize);
    status = AudioDeviceSetProperty(
        core->outputDeviceID,
        NULL,
        0,
        false,
        kAudioDevicePropertyBufferFrameSize,
        propertySize,
        &core->audioDevicePropertyBufferFrameSize);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ,
                           "Could not set device buffer frame size %" PRIu32 "\n",
                           (uint32_t)core->audioDevicePropertyBufferFrameSize);
        return -1;
    }

    /* get Buffer Frame Size */
    propertySize = sizeof(core->audioDevicePropertyBufferFrameSize);
    status = AudioDeviceGetProperty(
        core->outputDeviceID,
        0,
        false,
        kAudioDevicePropertyBufferFrameSize,
        &propertySize,
        &core->audioDevicePropertyBufferFrameSize);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ,
                           "Could not get device buffer frame size\n");
        return -1;
    }
    core->samples = (pdo->has_buffer_count ? pdo->buffer_count : 4) *
        core->audioDevicePropertyBufferFrameSize;

    /* get StreamFormat */
    propertySize = sizeof(core->outputStreamBasicDescription);
    status = AudioDeviceGetProperty(
        core->outputDeviceID,
        0,
        false,
        kAudioDevicePropertyStreamFormat,
        &propertySize,
        &core->outputStreamBasicDescription);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ,
                           "Could not get Device Stream properties\n");
        core->outputDeviceID = kAudioDeviceUnknown;
        return -1;
    }

    /* set Samplerate */
    core->outputStreamBasicDescription.mSampleRate = (Float64) as->freq;
    core->outputStreamBasicDescription.mFormatID = kAudioFormatLinearPCM;
    core->outputStreamBasicDescription.mFormatFlags =
        coreaudio_get_flags(hw->info, as);
    core->outputStreamBasicDescription.mBytesPerPacket =
        core->outputStreamBasicDescription.mBytesPerFrame =
        hw->info.nchannels * hw->info.bits / 8;
    core->outputStreamBasicDescription.mFramesPerPacket = 1;
    core->outputStreamBasicDescription.mChannelsPerFrame = hw->info.nchannels;
    core->outputStreamBasicDescription.mBitsPerChannel = hw->info.bits;

    propertySize = sizeof(core->outputStreamBasicDescription);
    status = AudioDeviceSetProperty(
        core->outputDeviceID,
        0,
        0,
        0,
        kAudioDevicePropertyStreamFormat,
        propertySize,
        &core->outputStreamBasicDescription);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ, "Could not set samplerate %d\n",
                           as->freq);
        core->outputDeviceID = kAudioDeviceUnknown;
        return -1;
    }

    /* set Callback */
    status = AudioDeviceAddIOProc(core->outputDeviceID, audioDeviceIOProc, hw);
    if (status != kAudioHardwareNoError) {
        coreaudio_logerr2 (status, typ, "Could not set IOProc\n");
        core->outputDeviceID = kAudioDeviceUnknown;
        return -1;
    }

    /* start Playback */
    if (!isPlaying(core->outputDeviceID)) {
        status = AudioDeviceStart(core->outputDeviceID, audioDeviceIOProc);
        if (status != kAudioHardwareNoError) {
            coreaudio_logerr2 (status, typ, "Could not start playback\n");
            AudioDeviceRemoveIOProc(core->outputDeviceID, audioDeviceIOProc);
            core->outputDeviceID = kAudioDeviceUnknown;
            return -1;
        }
    }

    return 0;
}

static size_t coreaudio_buffer_size_out(HWVoiceOut *hw)
{
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;
    return core->samples;
}

static void coreaudio_fini_out (HWVoiceOut *hw)
{
    OSStatus status;
    int err;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    if (!isAtexit) {
        /* stop playback */
        if (isPlaying(core->outputDeviceID)) {
            status = AudioDeviceStop(core->outputDeviceID, audioDeviceIOProc);
            if (status != kAudioHardwareNoError) {
                coreaudio_logerr (status, "Could not stop playback\n");
            }
        }

        /* remove callback */
        status = AudioDeviceRemoveIOProc(core->outputDeviceID,
                                         audioDeviceIOProc);
        if (status != kAudioHardwareNoError) {
            coreaudio_logerr (status, "Could not remove IOProc\n");
        }
    }
    core->outputDeviceID = kAudioDeviceUnknown;

    /* destroy mutex */
    err = pthread_mutex_destroy(&core->mutex);
    if (err) {
        dolog("Could not destroy mutex\nReason: %s\n", strerror (err));
    }
}

static void coreaudio_enable_out (HWVoiceOut *hw, bool enable)
{
    OSStatus status;
    coreaudioVoiceOut *core = (coreaudioVoiceOut *) hw;

    if (enable) {
        /* start playback */
        if (!isPlaying(core->outputDeviceID)) {
            status = AudioDeviceStart(core->outputDeviceID, audioDeviceIOProc);
            if (status != kAudioHardwareNoError) {
                coreaudio_logerr (status, "Could not resume playback\n");
            }
        }
    } else {
        /* stop playback */
        if (!isAtexit) {
            if (isPlaying(core->outputDeviceID)) {
                status = AudioDeviceStop(core->outputDeviceID, audioDeviceIOProc);
                if (status != kAudioHardwareNoError) {
                    coreaudio_logerr (status, "Could not pause playback\n");
                }
            }
        }
    }
}

static void *coreaudio_audio_init(Audiodev *dev)
{
    atexit(coreaudio_atexit);
    return dev;
}

static void coreaudio_audio_fini (void *opaque)
{
}

static struct audio_pcm_ops coreaudio_pcm_ops = {
    .init_out = coreaudio_init_out,
    .fini_out = coreaudio_fini_out,
    .write    = coreaudio_write,
    .buffer_size_out = coreaudio_buffer_size_out,
    .get_buffer_out = coreaudio_get_buffer_out,
    .put_buffer_out = coreaudio_put_buffer_out_nowrite,
    .enable_out = coreaudio_enable_out
};

struct audio_driver coreaudio_audio_driver = {
    .name           = "coreaudio",
    .descr          = "CoreAudio http://developer.apple.com/audio/coreaudio.html",
    .init           = coreaudio_audio_init,
    .fini           = coreaudio_audio_fini,
    .pcm_ops        = &coreaudio_pcm_ops,
    .can_be_default = 1,
    .max_voices_out = 1,
    .max_voices_in  = 0,
    .voice_size_out = sizeof (coreaudioVoiceOut),
    .voice_size_in  = 0
};
