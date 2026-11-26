#include "gb_audio.h"
#include "of_audio.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 24000
#endif

#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif

#include "minigb_apu.h"

static struct minigb_apu_ctx s_apu;
static int16_t s_audio_24k[AUDIO_SAMPLES_TOTAL];
static int16_t s_audio_48k[AUDIO_SAMPLES_TOTAL * 2];
static bool s_audio_ready = false;

void gb_audio_init(void)
{
    of_audio_init();
    minigb_apu_audio_init(&s_apu);
    s_audio_ready = true;
}

void gb_audio_push_frame(void)
{
    if (!s_audio_ready) {
        return;
    }

    const int out_pairs = (int)(AUDIO_SAMPLES * 2u);
    if (of_audio_free() < out_pairs) {
        return;
    }

    minigb_apu_audio_callback(&s_apu, s_audio_24k);

    /* 2x nearest-neighbor upsample 24 kHz -> 48 kHz stereo (halves APU CPU cost). */
    for (unsigned i = 0; i < AUDIO_SAMPLES; i++) {
        int16_t l = s_audio_24k[i * 2u];
        int16_t r = s_audio_24k[i * 2u + 1u];
        s_audio_48k[i * 4u]      = l;
        s_audio_48k[i * 4u + 1u] = r;
        s_audio_48k[i * 4u + 2u] = l;
        s_audio_48k[i * 4u + 3u] = r;
    }

    (void)of_audio_write(s_audio_48k, out_pairs);
}

uint8_t gb_audio_read(uint16_t addr)
{
    if (!s_audio_ready) {
        return 0xFFU;
    }
    return minigb_apu_audio_read(&s_apu, addr);
}

void gb_audio_write(uint16_t addr, uint8_t val)
{
    if (!s_audio_ready) {
        return;
    }
    minigb_apu_audio_write(&s_apu, addr, val);
}
