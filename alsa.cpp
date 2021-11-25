#ifndef _MSC_VER

#include <alsa/asoundlib.h>
#include <math.h>

const char *device = "default"; /* playback device */
const snd_pcm_format_t format = SND_PCM_FORMAT_S16; /* sample format */
const unsigned int rate = 16000; /* stream rate */
const unsigned int channels = 1; /* count of channels */

unsigned int buffer_time = 30000; /* ring buffer length in us: 2x or 3x period_time. */
unsigned int period_time = 15000; /* period time in us */
snd_pcm_sframes_t buffer_size;
snd_pcm_sframes_t period_size;

snd_output_t *output = NULL;

void emit_samples(const snd_pcm_channel_area_t *areas,
                          snd_pcm_uframes_t offset,
                          int count, const short* ps, ssize_t cb)
{
    // ;;;; cb and count are duplicates.
#define fast_and_dangerous
#ifdef fast_and_dangerous
    const int format_bits = 16;
    const int bps = 2;
    const int phys_bps = 2;
    const bool big_endian = false;
    const bool to_unsigned = false;
#else
    const int format_bits = snd_pcm_format_width(format);
    const int bps = format_bits / 8; /* bytes per sample */
    const int phys_bps = snd_pcm_format_physical_width(format) / 8;
    const int big_endian = snd_pcm_format_big_endian(format) == 1;
    const int to_unsigned = snd_pcm_format_unsigned(format) == 1;
#endif

    unsigned char *samples[channels];
    int steps[channels];

    /* verify and prepare the contents of areas */
    unsigned int chn;
    for (chn = 0; chn < channels; chn++) {
        if ((areas[chn].first % 8) != 0) {
            printf("areas[%i].first == %i, aborting...\n", chn, areas[chn].first);
            exit(EXIT_FAILURE);
        }
        samples[chn] = /*(signed short *)*/(((unsigned char *)areas[chn].addr) + (areas[chn].first / 8));
        if ((areas[chn].step % 16) != 0) {
            printf("areas[%i].step == %i, aborting...\n", chn, areas[chn].step);
            exit(EXIT_FAILURE);
        }
        steps[chn] = areas[chn].step / 8;
        samples[chn] += offset * steps[chn];
    }

    /* fill the channel areas */
    while (count-- > 0) {
	int res = int(*ps++);
        if (to_unsigned)
            res ^= 1U << (format_bits - 1);
        for (chn = 0; chn < channels; chn++) {
            /* Generate data in native endian format */
            if (big_endian) {
                for (int i = 0; i < bps; i++)
                    *(samples[chn] + phys_bps - 1 - i) = (res >> i * 8) & 0xff;
            } else {
                for (int i = 0; i < bps; i++)
                    *(samples[chn] + i) = (res >> i * 8) & 0xff;
            }
            samples[chn] += steps[chn];
        }
    }
}
static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *params, snd_pcm_access_t access)
{
    /* choose all parameters */
    int err = snd_pcm_hw_params_any(handle, params);
    if (err < 0) {
        printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
        return err;
    }
    /* set hardware resampling */
    // Report back from alsa to timeliner the *actual* sampling rate?
    err = snd_pcm_hw_params_set_rate_resample(handle, params, 1);
    if (err < 0) {
        printf("Resampling setup failed for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* set the interleaved read/write format */
    err = snd_pcm_hw_params_set_access(handle, params, access);
    if (err < 0) {
        printf("Access type not available for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* set the sample format */
    err = snd_pcm_hw_params_set_format(handle, params, format);
    if (err < 0) {
        printf("Sample format not available for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    if (err < 0) {
        printf("Channels count (%i) not available for playbacks: %s\n", channels, snd_strerror(err));
        return err;
    }
    /* set the stream rate */
    unsigned int rrate = rate;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
    if (err < 0) {
        printf("Rate %iHz not available for playback: %s\n", rate, snd_strerror(err));
        return err;
    }
    if (rrate != rate) {
        printf("Rate doesn't match (requested %iHz, get %iHz)\n", rate, err);
        return -EINVAL;
    }
    /* set the buffer time */
    int dir = 0;
    err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir);
    if (err < 0) {
        printf("Unable to set buffer time %i for playback: %s\n", buffer_time, snd_strerror(err));
        return err;
    }
    snd_pcm_uframes_t size = 0;
    err = snd_pcm_hw_params_get_buffer_size(params, &size);
    if (err < 0) {
        printf("Unable to get buffer size for playback: %s\n", snd_strerror(err));
        return err;
    }
    buffer_size = size;
    /* set the period time */
    err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, &dir);
    if (err < 0) {
        printf("Unable to set period time %i for playback: %s\n", period_time, snd_strerror(err));
        return err;
    }
    err = snd_pcm_hw_params_get_period_size(params, &size, &dir);
    if (err < 0) {
        printf("Unable to get period size for playback: %s\n", snd_strerror(err));
        return err;
    }
    period_size = size;
    /* write the parameters to device */
    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
        return err;
    }
    return 0;
}
static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
    /* get the current swparams */
    int err = snd_pcm_sw_params_current(handle, swparams);
    if (err < 0) {
        printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* start the transfer when the buffer is almost full: */
    /* (buffer_size / avail_min) * avail_min */
    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, (buffer_size / period_size) * period_size);
    if (err < 0) {
        printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
    if (err < 0) {
        printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(handle, swparams);
    if (err < 0) {
        printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
        return err;
    }
    return 0;
}
/*
* Underrun and suspend recovery
*/
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    if (err == -EPIPE) { /* under-run */
        err = snd_pcm_prepare(handle);
        if (err < 0)
            printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
        return 0;
    }
    if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN)
            sleep(1); /* wait until the suspend flag is released */
        if (err < 0) {
            err = snd_pcm_prepare(handle);
            if (err < 0)
                printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
        }
        return 0;
    }
    return err;
}

int alsaBuf() { return period_size; }

void write_loop(snd_pcm_t *handle, signed short *samples, snd_pcm_channel_area_t *areas,
	const short* ps, ssize_t cb)
{
    emit_samples(areas, 0, period_size, ps, cb);
	signed short *ptr = samples;
        int cptr = period_size;
        while (cptr > 0) {
            const int err = snd_pcm_writei(handle, ptr, cptr);
            if (err == -EAGAIN)
                continue;
            if (err < 0) {
                if (xrun_recovery(handle, err) < 0) {
                    printf("Write error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }
                break; /* skip one period */
            }
            ptr += err * channels;
            cptr -= err;
        }
}

static signed short *samples = NULL;
static snd_pcm_channel_area_t *areas = NULL;
static snd_pcm_t *handle = NULL;

void alsaInit(const unsigned SR)
{
    setenv("ALSA_CARD", "0", 1);
    snd_pcm_hw_params_t *hwparams; snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_t *swparams; snd_pcm_sw_params_alloca(&swparams);
    int err = snd_output_stdio_attach(&output, stdout, 0);
    if (err < 0) {
        printf("timeliner ALSA Output failed: %s\n", snd_strerror(err));
        return;
    }
    if (SR != rate)
      printf("timeliner ALSA Warning: ALSA playback overriding requested sample rate of %d Hz with hardcoded %d Hz.\n", SR, rate);
    printf("timeliner ALSA Playback device is %s\n", device);
    printf("Stream parameters are %iHz, %s, %i channels\n", rate, snd_pcm_format_name(format), channels);
    if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        printf("timeliner ALSA Playback open error: %s\n", snd_strerror(err));
        return;
    }
    if ((err = set_hwparams(handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        printf("timeliner ALSA Setting of hwparams failed: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    if ((err = set_swparams(handle, swparams)) < 0) {
        printf("timeliner ALSA Setting of swparams failed: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    samples = (short int*)malloc((period_size * channels * snd_pcm_format_physical_width(format)) / 8);
    if (samples == NULL) {
        printf("timeliner ALSA Out of memory\n");
        exit(EXIT_FAILURE);
    }
    areas = (snd_pcm_channel_area_t*)calloc(channels, sizeof(snd_pcm_channel_area_t));
    if (areas == NULL) {
        printf("timeliner ALSA Out of memory\n");
        exit(EXIT_FAILURE);
    }
    unsigned int chn;
    for (chn = 0; chn < channels; chn++) {
        areas[chn].addr = samples;
        areas[chn].first = chn * snd_pcm_format_physical_width(format);
        areas[chn].step = channels * snd_pcm_format_physical_width(format);
    }
}

void alsaTick(const short* ps, ssize_t cb)
{
  write_loop(handle, samples, areas, ps, cb);
}

void alsaTerm()
{
    free(areas);
    free(samples);
    snd_pcm_close(handle);
}

#endif
