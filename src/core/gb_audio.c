#include "gb_audio.h"
#include "of_audio.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 12000
#endif

#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif

#include "minigb_apu.h"

static struct minigb_apu_ctx s_apu;
static int16_t s_audio_24k[2048];
static int16_t s_audio_48k[4096];
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

    const unsigned repeat = (48000u / (unsigned)AUDIO_SAMPLE_RATE);
    const unsigned mult = (repeat >= 1u && repeat <= 4u) ? repeat : 2u;
    const int out_pairs = (int)(AUDIO_SAMPLES * mult);
    if (of_audio_free() < out_pairs) {
        return;
    }

    minigb_apu_audio_callback(&s_apu, s_audio_24k);

    /* Nearest-neighbor upsample AUDIO_SAMPLE_RATE -> 48 kHz stereo (e.g. 4x from 12 kHz). */
    unsigned dst_idx = 0u;
    for (unsigned i = 0u; i < AUDIO_SAMPLES; i++) {
        int16_t l = s_audio_24k[i * 2u];
        int16_t r = s_audio_24k[i * 2u + 1u];
        for (unsigned k = 0u; k < mult; k++) {
            s_audio_48k[dst_idx++] = l;
            s_audio_48k[dst_idx++] = r;
        }
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
