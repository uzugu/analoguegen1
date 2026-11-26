#include "gb_audio.h"
#include "of_audio.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 48000
#endif

#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif

#include "minigb_apu.h"

static struct minigb_apu_ctx s_apu;
static int16_t s_audio_buf[AUDIO_SAMPLES_TOTAL];
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

    minigb_apu_audio_callback(&s_apu, s_audio_buf);
    if (of_audio_free() >= (int)AUDIO_SAMPLES) {
        (void)of_audio_write(s_audio_buf, (int)AUDIO_SAMPLES);
    }
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
