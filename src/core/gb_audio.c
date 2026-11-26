#include "gb_audio.h"
#include "of_audio.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef OF_PC

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 16000
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

    /* Nearest-neighbor upsample AUDIO_SAMPLE_RATE -> 48 kHz stereo (e.g. 3x from 16 kHz). */
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

#else /* Pocket FPGA hardware mixer DMG synthesizer */

#include "of_cache.h"
#include "of_mixer.h"

#include <string.h>

typedef struct {
    bool enabled;
    bool dac_on;
    bool len_enable;
    bool env_dir;
    bool playing;
    uint8_t duty;
    uint8_t volume;
    uint8_t env_period;
    uint8_t env_timer;
    uint16_t freq;
    uint16_t len_counter;
    int voice;
    const int16_t *cur_pcm;
    uint32_t cur_len;
} dmg_chan_t;

typedef struct {
    bool enabled;
    bool negate;
    uint8_t period;
    uint8_t shift;
    uint8_t timer;
    uint16_t shadow_freq;
} dmg_sweep_t;

static uint8_t s_reg[0x30];
static dmg_chan_t s_ch[4];
static dmg_sweep_t s_sweep;
static bool s_power = false;
static bool s_audio_ready = false;

/* 64-byte aligned waveforms in SDRAM for FPGA mixer DMA. */
static int16_t s_pulse_pcm[4][32] __attribute__((aligned(64)));
static int16_t s_wave_pcm[32] __attribute__((aligned(64)));
static int16_t s_noise15_pcm[1024] __attribute__((aligned(64)));
static int16_t s_noise7_pcm[1024] __attribute__((aligned(64)));

static const uint8_t s_reg_read_mask[0x20] = {
    /* 0xFF10..0xFF14 (CH1) */ 0x80U, 0x3FU, 0x00U, 0xFFU, 0xBFU,
    /* 0xFF15..0xFF19 (CH2) */ 0xFFU, 0x3FU, 0x00U, 0xFFU, 0xBFU,
    /* 0xFF1A..0xFF1E (CH3) */ 0x7FU, 0xFFU, 0x9FU, 0xFFU, 0xBFU,
    /* 0xFF1F..0xFF23 (CH4) */ 0xFFU, 0xFFU, 0x00U, 0x00U, 0xBFU,
    /* 0xFF24..0xFF26 (CTL) */ 0x00U, 0x00U, 0x70U,
    /* 0xFF27..0xFF2F       */ 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
};

static void stop_channel_voice(int idx)
{
    if (s_ch[idx].voice >= 0 && s_ch[idx].playing) {
        of_mixer_stop(s_ch[idx].voice);
    }
    s_ch[idx].voice = -1;
    s_ch[idx].playing = false;
    s_ch[idx].cur_pcm = NULL;
    s_ch[idx].cur_len = 0U;
}

static void reset_channels(void)
{
    for (int i = 0; i < 4; i++) {
        stop_channel_voice(i);
        memset(&s_ch[i], 0, sizeof(s_ch[i]));
        s_ch[i].voice = -1;
    }
    memset(&s_sweep, 0, sizeof(s_sweep));
}

static void init_waveforms(void)
{
    static const unsigned duty_high[4] = { 4U, 8U, 16U, 24U };

    for (unsigned d = 0U; d < 4U; d++) {
        for (unsigned i = 0U; i < 32U; i++) {
            s_pulse_pcm[d][i] = (i < duty_high[d]) ? 12000 : -12000;
        }
    }

    for (unsigned i = 0U; i < 32U; i++) {
        s_wave_pcm[i] = 0;
    }

    uint16_t lfsr15 = 0x7FFFU;
    for (unsigned i = 0U; i < 1024U; i++) {
        s_noise15_pcm[i] = (lfsr15 & 1U) ? -10000 : 10000;
        uint16_t x = (uint16_t)((lfsr15 ^ (lfsr15 >> 1)) & 1U);
        lfsr15 = (uint16_t)((lfsr15 >> 1) | (x << 14));
    }

    uint16_t lfsr7 = 0x7FFFU;
    for (unsigned i = 0U; i < 1024U; i++) {
        s_noise7_pcm[i] = (lfsr7 & 1U) ? -10000 : 10000;
        uint16_t x = (uint16_t)((lfsr7 ^ (lfsr7 >> 1)) & 1U);
        lfsr7 = (uint16_t)((lfsr7 >> 1) | (x << 14));
        lfsr7 = (uint16_t)((lfsr7 & (uint16_t)~(1U << 6)) | (x << 6));
    }

    of_cache_flush_range(s_pulse_pcm, (uint32_t)sizeof(s_pulse_pcm));
    of_cache_flush_range(s_wave_pcm, (uint32_t)sizeof(s_wave_pcm));
    of_cache_flush_range(s_noise15_pcm, (uint32_t)sizeof(s_noise15_pcm));
    of_cache_flush_range(s_noise7_pcm, (uint32_t)sizeof(s_noise7_pcm));
}

static uint32_t sweep_calc_freq(void)
{
    uint32_t delta = (uint32_t)(s_sweep.shadow_freq >> s_sweep.shift);
    if (s_sweep.negate) {
        return (s_sweep.shadow_freq >= delta) ? (uint32_t)(s_sweep.shadow_freq - delta) : 0U;
    }
    return (uint32_t)s_sweep.shadow_freq + delta;
}

static void tick_sweep(void)
{
    if (!s_ch[0].enabled || !s_sweep.enabled) {
        return;
    }

    if (s_sweep.timer > 0U) {
        s_sweep.timer--;
    }
    if (s_sweep.timer != 0U) {
        return;
    }

    s_sweep.timer = (s_sweep.period != 0U) ? s_sweep.period : 8U;
    if (s_sweep.period == 0U) {
        return;
    }

    uint32_t next_freq = sweep_calc_freq();
    if (next_freq > 2047U) {
        s_ch[0].enabled = false;
        return;
    }

    if (s_sweep.shift != 0U) {
        s_sweep.shadow_freq = (uint16_t)next_freq;
        s_ch[0].freq = (uint16_t)next_freq;
        if (sweep_calc_freq() > 2047U) {
            s_ch[0].enabled = false;
        }
    }
}

void gb_audio_init(void)
{
    of_audio_init();
    of_mixer_init(8, OF_MIXER_OUTPUT_RATE);

    memset(s_reg, 0, sizeof(s_reg));
    for (int i = 0; i < 4; i++) {
        memset(&s_ch[i], 0, sizeof(s_ch[i]));
        s_ch[i].voice = -1;
    }
    memset(&s_sweep, 0, sizeof(s_sweep));
    s_power = false;

    init_waveforms();
    s_audio_ready = true;
}

void gb_audio_push_frame(void)
{
    if (!s_audio_ready) {
        return;
    }

    /* Tick length counters (256 Hz ~= 4 ticks/frame), envelopes (64 Hz ~= 1 tick/frame),
     * and CH1 frequency sweep (128 Hz ~= 2 ticks/frame). */
    if (s_power) {
        for (int i = 0; i < 4; i++) {
            dmg_chan_t *c = &s_ch[i];
            if (!c->enabled) {
                continue;
            }
            if (c->len_enable && c->len_counter > 0U) {
                if (c->len_counter > 4U) {
                    c->len_counter = (uint16_t)(c->len_counter - 4U);
                } else {
                    c->len_counter = 0U;
                    c->enabled = false;
                }
            }
            if (i != 2 && c->enabled && c->env_period > 0U) {
                if (c->env_timer > 0U) {
                    c->env_timer--;
                }
                if (c->env_timer == 0U) {
                    c->env_timer = c->env_period;
                    if (c->env_dir) {
                        if (c->volume < 15U) {
                            c->volume++;
                        }
                    } else {
                        if (c->volume > 0U) {
                            c->volume--;
                        }
                    }
                }
            }
        }

        tick_sweep();
        tick_sweep();
    }

    const uint32_t master_l = (uint32_t)(((s_reg[0x14] >> 4) & 7U) + 1U);
    const uint32_t master_r = (uint32_t)((s_reg[0x14] & 7U) + 1U);
    const uint8_t nr51 = s_reg[0x15];

    static const uint8_t wave_vol_lut[4] = { 0U, 15U, 7U, 4U };

    for (int i = 0; i < 4; i++) {
        dmg_chan_t *c = &s_ch[i];
        uint32_t vol = (i == 2) ? (uint32_t)wave_vol_lut[(s_reg[0x0C] >> 5) & 3U] : (uint32_t)c->volume;

        if (!s_power || !c->enabled || vol == 0U) {
            if (c->playing || c->voice >= 0) {
                stop_channel_voice(i);
            }
            continue;
        }

        const bool route_l = (nr51 & (uint8_t)(1U << (i + 4))) != 0U;
        const bool route_r = (nr51 & (uint8_t)(1U << i)) != 0U;
        const int vol_l = route_l ? (int)((vol * master_l) / 2U) : 0;
        const int vol_r = route_r ? (int)((vol * master_r) / 2U) : 0;

        if (vol_l == 0 && vol_r == 0) {
            if (c->playing || c->voice >= 0) {
                stop_channel_voice(i);
            }
            continue;
        }

        uint32_t hz = 44100U;
        const int16_t *pcm = NULL;
        uint32_t sample_count = 32U;

        if (i == 0 || i == 1) {
            uint32_t denom = 2048U - (uint32_t)(c->freq & 0x7FFU);
            hz = (131072U * 32U) / (denom ? denom : 1U);
            if (hz < 2000U) {
                hz = 2000U;
            } else if (hz > 480000U) {
                hz = 480000U;
            }
            pcm = s_pulse_pcm[c->duty & 3U];
            sample_count = 32U;
        } else if (i == 2) {
            uint32_t denom = 2048U - (uint32_t)(c->freq & 0x7FFU);
            hz = (65536U * 32U) / (denom ? denom : 1U);
            if (hz < 2000U) {
                hz = 2000U;
            } else if (hz > 480000U) {
                hz = 480000U;
            }
            pcm = s_wave_pcm;
            sample_count = 32U;
        } else {
            uint8_t nr43 = s_reg[0x12];
            uint32_t div = (nr43 & 7U) ? ((uint32_t)(nr43 & 7U) * 16U) : 8U;
            uint32_t shift = (uint32_t)((nr43 >> 4) & 0x0FU);
            hz = (shift < 14U) ? (4194304U / (div << shift)) : 1000U;
            if (hz < 1000U) {
                hz = 1000U;
            } else if (hz > 96000U) {
                hz = 96000U;
            }
            pcm = (nr43 & 0x08U) ? s_noise7_pcm : s_noise15_pcm;
            sample_count = 1024U;
        }

        const int peak_vol = (vol_l > vol_r) ? vol_l : vol_r;
        if (!c->playing || c->voice < 0 || !of_mixer_voice_active(c->voice)) {
            int v = of_mixer_play((const uint8_t *)pcm, sample_count, hz, 0, peak_vol);
            if (v >= 0) {
                c->voice = v;
                c->playing = true;
                c->cur_pcm = pcm;
                c->cur_len = sample_count;
                of_mixer_set_loop(v, 0, (int)sample_count);
                of_mixer_set_voice(v, (int)hz, vol_l, vol_r);
            }
        } else if (c->cur_pcm != pcm || c->cur_len != sample_count) {
            of_mixer_retrigger(c->voice, (const uint8_t *)pcm, sample_count, hz, peak_vol);
            c->cur_pcm = pcm;
            c->cur_len = sample_count;
            of_mixer_set_loop(c->voice, 0, (int)sample_count);
            of_mixer_set_voice(c->voice, (int)hz, vol_l, vol_r);
        } else {
            of_mixer_set_voice(c->voice, (int)hz, vol_l, vol_r);
        }
    }

    of_mixer_pump();
}

uint8_t gb_audio_read(uint16_t addr)
{
    if (!s_audio_ready || addr < 0xFF10U || addr > 0xFF3FU) {
        return 0xFFU;
    }

    uint32_t off = (uint32_t)(addr - 0xFF10U);
    if (off >= 0x20U) {
        return s_reg[off];
    }

    if (addr == 0xFF26U) {
        uint8_t status = 0x70U;
        if (s_power) {
            status |= 0x80U;
            for (int i = 0; i < 4; i++) {
                if (s_ch[i].enabled) {
                    status |= (uint8_t)(1U << i);
                }
            }
        }
        return status;
    }

    return (uint8_t)(s_reg[off] | s_reg_read_mask[off]);
}

void gb_audio_write(uint16_t addr, uint8_t val)
{
    if (!s_audio_ready || addr < 0xFF10U || addr > 0xFF3FU) {
        return;
    }

    uint32_t off = (uint32_t)(addr - 0xFF10U);

    /* Wave RAM (0xFF30..0xFF3F) is always writable regardless of NR52 power. */
    if (off >= 0x20U) {
        s_reg[off] = val;
        uint32_t w = (off - 0x20U) * 2U;
        int hi = (int)((val >> 4) & 0x0FU);
        int lo = (int)(val & 0x0FU);
        s_wave_pcm[w] = (int16_t)((hi - 8) * 1500);
        s_wave_pcm[w + 1U] = (int16_t)((lo - 8) * 1500);
        of_cache_flush_range(s_wave_pcm, (uint32_t)sizeof(s_wave_pcm));
        return;
    }

    /* NR52 master power control. */
    if (addr == 0xFF26U) {
        bool power_on = (val & 0x80U) != 0U;
        if (!power_on) {
            memset(s_reg, 0, 0x16U);
            s_reg[0x16] = 0x00U;
            s_power = false;
            reset_channels();
        } else if (!s_power) {
            s_power = true;
            s_reg[0x16] = 0x80U;
        }
        return;
    }

    if (!s_power) {
        return;
    }

    s_reg[off] = val;

    switch (addr) {
    /* CH1: Pulse + Sweep */
    case 0xFF11U:
        s_ch[0].duty = (uint8_t)((val >> 6) & 3U);
        s_ch[0].len_counter = (uint16_t)(64U - (val & 0x3FU));
        break;
    case 0xFF12U:
        s_ch[0].dac_on = (val & 0xF8U) != 0U;
        if (!s_ch[0].dac_on) {
            s_ch[0].enabled = false;
        }
        break;
    case 0xFF13U:
        s_ch[0].freq = (uint16_t)((s_ch[0].freq & 0x700U) | val);
        break;
    case 0xFF14U:
        s_ch[0].freq = (uint16_t)((s_ch[0].freq & 0x0FFU) | (((uint16_t)(val & 7U)) << 8));
        s_ch[0].len_enable = (val & 0x40U) != 0U;
        if (val & 0x80U) {
            if (s_ch[0].len_counter == 0U) {
                s_ch[0].len_counter = 64U;
            }
            s_ch[0].volume = (uint8_t)((s_reg[0x02] >> 4) & 0x0FU);
            s_ch[0].env_dir = (s_reg[0x02] & 0x08U) != 0U;
            s_ch[0].env_period = (uint8_t)(s_reg[0x02] & 0x07U);
            s_ch[0].env_timer = (s_ch[0].env_period != 0U) ? s_ch[0].env_period : 8U;
            s_ch[0].dac_on = (s_reg[0x02] & 0xF8U) != 0U;
            s_ch[0].enabled = s_ch[0].dac_on;

            s_sweep.shadow_freq = s_ch[0].freq;
            s_sweep.period = (uint8_t)((s_reg[0x00] >> 4) & 7U);
            s_sweep.negate = (s_reg[0x00] & 0x08U) != 0U;
            s_sweep.shift = (uint8_t)(s_reg[0x00] & 7U);
            s_sweep.timer = (s_sweep.period != 0U) ? s_sweep.period : 8U;
            s_sweep.enabled = (s_sweep.period != 0U) || (s_sweep.shift != 0U);
            if (s_sweep.shift != 0U && sweep_calc_freq() > 2047U) {
                s_ch[0].enabled = false;
            }
        }
        break;

    /* CH2: Pulse */
    case 0xFF16U:
        s_ch[1].duty = (uint8_t)((val >> 6) & 3U);
        s_ch[1].len_counter = (uint16_t)(64U - (val & 0x3FU));
        break;
    case 0xFF17U:
        s_ch[1].dac_on = (val & 0xF8U) != 0U;
        if (!s_ch[1].dac_on) {
            s_ch[1].enabled = false;
        }
        break;
    case 0xFF18U:
        s_ch[1].freq = (uint16_t)((s_ch[1].freq & 0x700U) | val);
        break;
    case 0xFF19U:
        s_ch[1].freq = (uint16_t)((s_ch[1].freq & 0x0FFU) | (((uint16_t)(val & 7U)) << 8));
        s_ch[1].len_enable = (val & 0x40U) != 0U;
        if (val & 0x80U) {
            if (s_ch[1].len_counter == 0U) {
                s_ch[1].len_counter = 64U;
            }
            s_ch[1].volume = (uint8_t)((s_reg[0x07] >> 4) & 0x0FU);
            s_ch[1].env_dir = (s_reg[0x07] & 0x08U) != 0U;
            s_ch[1].env_period = (uint8_t)(s_reg[0x07] & 0x07U);
            s_ch[1].env_timer = (s_ch[1].env_period != 0U) ? s_ch[1].env_period : 8U;
            s_ch[1].dac_on = (s_reg[0x07] & 0xF8U) != 0U;
            s_ch[1].enabled = s_ch[1].dac_on;
        }
        break;

    /* CH3: Wave */
    case 0xFF1AU:
        s_ch[2].dac_on = (val & 0x80U) != 0U;
        if (!s_ch[2].dac_on) {
            s_ch[2].enabled = false;
        }
        break;
    case 0xFF1BU:
        s_ch[2].len_counter = (uint16_t)(256U - val);
        break;
    case 0xFF1DU:
        s_ch[2].freq = (uint16_t)((s_ch[2].freq & 0x700U) | val);
        break;
    case 0xFF1EU:
        s_ch[2].freq = (uint16_t)((s_ch[2].freq & 0x0FFU) | (((uint16_t)(val & 7U)) << 8));
        s_ch[2].len_enable = (val & 0x40U) != 0U;
        if (val & 0x80U) {
            if (s_ch[2].len_counter == 0U) {
                s_ch[2].len_counter = 256U;
            }
            s_ch[2].dac_on = (s_reg[0x0A] & 0x80U) != 0U;
            s_ch[2].enabled = s_ch[2].dac_on;
        }
        break;

    /* CH4: Noise */
    case 0xFF20U:
        s_ch[3].len_counter = (uint16_t)(64U - (val & 0x3FU));
        break;
    case 0xFF21U:
        s_ch[3].dac_on = (val & 0xF8U) != 0U;
        if (!s_ch[3].dac_on) {
            s_ch[3].enabled = false;
        }
        break;
    case 0xFF23U:
        s_ch[3].len_enable = (val & 0x40U) != 0U;
        if (val & 0x80U) {
            if (s_ch[3].len_counter == 0U) {
                s_ch[3].len_counter = 64U;
            }
            s_ch[3].volume = (uint8_t)((s_reg[0x11] >> 4) & 0x0FU);
            s_ch[3].env_dir = (s_reg[0x11] & 0x08U) != 0U;
            s_ch[3].env_period = (uint8_t)(s_reg[0x11] & 0x07U);
            s_ch[3].env_timer = (s_ch[3].env_period != 0U) ? s_ch[3].env_period : 8U;
            s_ch[3].dac_on = (s_reg[0x11] & 0xF8U) != 0U;
            s_ch[3].enabled = s_ch[3].dac_on;
        }
        break;

    default:
        break;
    }
}

#endif /* OF_PC */
