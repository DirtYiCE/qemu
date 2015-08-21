#include "audio.h"
#include "qemu-common.h"
#include "qemu/config-file.h"

#define AUDIO_CAP "audio-legacy"
#include "audio_int.h"

typedef enum EnvTransform {
    ENV_TRANSFORM_NONE,
    ENV_TRANSFORM_BOOL,
    ENV_TRANSFORM_FMT,
    ENV_TRANSFORM_FRAMES_TO_USECS_IN,
    ENV_TRANSFORM_FRAMES_TO_USECS_OUT,
    ENV_TRANSFORM_SAMPLES_TO_USECS_IN,
    ENV_TRANSFORM_SAMPLES_TO_USECS_OUT,
    ENV_TRANSFORM_BYTES_TO_USECS_IN,
    ENV_TRANSFORM_BYTES_TO_USECS_OUT,
    ENV_TRANSFORM_MILLIS_TO_USECS,
    ENV_TRANSFORM_HZ_TO_USECS,
} EnvTransform;

typedef struct SimpleEnvMap {
    const char *name;
    const char *option;
    EnvTransform transform;
} SimpleEnvMap;

SimpleEnvMap global_map[] = {
    /* DAC/out settings */
    { "QEMU_AUDIO_DAC_FIXED_SETTINGS", "out.fixed-settings",
      ENV_TRANSFORM_BOOL },
    { "QEMU_AUDIO_DAC_FIXED_FREQ", "out.frequency" },
    { "QEMU_AUDIO_DAC_FIXED_FMT", "out.format", ENV_TRANSFORM_FMT },
    { "QEMU_AUDIO_DAC_FIXED_CHANNELS", "out.channels" },
    { "QEMU_AUDIO_DAC_VOICES", "out.voices" },

    /* ADC/in settings */
    { "QEMU_AUDIO_ADC_FIXED_SETTINGS", "in.fixed-settings",
      ENV_TRANSFORM_BOOL },
    { "QEMU_AUDIO_ADC_FIXED_FREQ", "in.frequency" },
    { "QEMU_AUDIO_ADC_FIXED_FMT", "in.format", ENV_TRANSFORM_FMT },
    { "QEMU_AUDIO_ADC_FIXED_CHANNELS", "in.channels" },
    { "QEMU_AUDIO_ADC_VOICES", "in.voices" },

    /* general */
    { "QEMU_AUDIO_TIMER_PERIOD", "timer-period", ENV_TRANSFORM_HZ_TO_USECS },
    { /* End of list */ }
};

static unsigned long long toull(const char *str)
{
    unsigned long long ret;
    if (parse_uint_full(str, &ret, 10)) {
        dolog("Invalid integer value `%s'\n", str);
        exit(1);
    }
    return ret;
}

/* non reentrant typesafe or anything, but enough in this small c file */
static const char *tostr(unsigned long long val)
{
    /* max length in decimal possible for an unsigned long long number */
    #define LEN ((CHAR_BIT * sizeof(unsigned long long) - 1) / 3 + 2)
    static char ret[LEN];
    snprintf(ret, LEN, "%llu", val);
    return ret;
}

static uint64_t frames_to_usecs(QemuOpts *opts, uint64_t frames, bool in)
{
    const char *opt = in ? "in.frequency" : "out.frequency";
    uint64_t freq = qemu_opt_get_number(opts, opt, 44100);
    return (frames * 1000000 + freq/2) / freq;
}

static uint64_t samples_to_usecs(QemuOpts *opts, uint64_t samples, bool in)
{
    const char *opt = in ? "in.channels" : "out.channels";
    uint64_t channels = qemu_opt_get_number(opts, opt, 2);
    return frames_to_usecs(opts, samples/channels, in);
}

static uint64_t bytes_to_usecs(QemuOpts *opts, uint64_t bytes, bool in)
{
    const char *opt = in ? "in.format" : "out.format";
    const char *val = qemu_opt_get(opts, opt);
    uint64_t bytes_per_sample = (val ? toull(val) : 16) / 8;
    return samples_to_usecs(opts, bytes * bytes_per_sample, in);
}

static const char *transform_val(QemuOpts *opts, const char *val,
                                 EnvTransform transform)
{
    switch (transform) {
    case ENV_TRANSFORM_NONE:
        return val;

    case ENV_TRANSFORM_BOOL:
        return toull(val) ? "on" : "off";

    case ENV_TRANSFORM_FMT:
        if (strcasecmp(val, "u8") == 0) {
            return "u8";
        } else if (strcasecmp(val, "u16") == 0) {
            return "u16";
        } else if (strcasecmp(val, "u32") == 0) {
            return "u32";
        } else if (strcasecmp(val, "s8") == 0) {
            return "s8";
        } else if (strcasecmp(val, "s16") == 0) {
            return "s16";
        } else if (strcasecmp(val, "s32") == 0) {
            return "s32";
        } else {
            dolog("Invalid audio format `%s'\n", val);
            exit(1);
        }

    case ENV_TRANSFORM_FRAMES_TO_USECS_IN:
        return tostr(frames_to_usecs(opts, toull(val), true));
    case ENV_TRANSFORM_FRAMES_TO_USECS_OUT:
        return tostr(frames_to_usecs(opts, toull(val), false));

    case ENV_TRANSFORM_SAMPLES_TO_USECS_IN:
        return tostr(samples_to_usecs(opts, toull(val), true));
    case ENV_TRANSFORM_SAMPLES_TO_USECS_OUT:
        return tostr(samples_to_usecs(opts, toull(val), false));

    case ENV_TRANSFORM_BYTES_TO_USECS_IN:
        return tostr(bytes_to_usecs(opts, toull(val), true));
    case ENV_TRANSFORM_BYTES_TO_USECS_OUT:
        return tostr(bytes_to_usecs(opts, toull(val), false));

    case ENV_TRANSFORM_MILLIS_TO_USECS:
        return tostr(toull(val) * 1000);

    case ENV_TRANSFORM_HZ_TO_USECS:
        return tostr(1000000 / toull(val));
    }

    abort(); /* it's unreachable, gcc */
}

static void handle_env_opts(QemuOpts *opts, SimpleEnvMap *map)
{
    while (map->name) {
        const char *val = getenv(map->name);

        if (val) {
            qemu_opt_set(opts, map->option,
                         transform_val(opts, val, map->transform),
                         &error_abort);
        }

        ++map;
    }
}

static void legacy_opt(const char *drv)
{
    QemuOpts *opts;
    opts = qemu_opts_create(qemu_find_opts("audiodev"), drv, true,
                            &error_abort);
    qemu_opt_set(opts, "driver", drv, &error_abort);

    handle_env_opts(opts, global_map);
}

void audio_handle_legacy_opts(void)
{
    const char *drv = getenv("QEMU_AUDIO_DRV");

    if (drv) {
        legacy_opt(drv);
    } else {
        struct audio_driver **drv;
        for (drv = drvtab; *drv; ++drv) {
            if ((*drv)->can_be_default) {
                legacy_opt((*drv)->name);
            }
        }
    }
}

static int legacy_help_each(void *opaque, QemuOpts *opts, Error **errp)
{
    printf("-audiodev ");
    qemu_opts_print(opts, ",");
    printf("\n");
    return 0;
}

void audio_legacy_help(void)
{
    printf("Environment variable based configuration deprecated.\n");
    printf("Please use the new -audiodev option.\n");

    audio_handle_legacy_opts();
    printf("\nEquivalent -audiodev to your current environment variables:\n");
    qemu_opts_foreach(qemu_find_opts("audiodev"), legacy_help_each, NULL, NULL);
}
