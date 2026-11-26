#ifndef GB_AUDIO_H
#define GB_AUDIO_H

#include <stdint.h>

/* Initialize 48 kHz stereo DMG APU + openfpgaOS audio output. */
void gb_audio_init(void);

/* Synthesize one frame (~803 stereo pairs @ 48 kHz) and push to of_audio. */
void gb_audio_push_frame(void);

/* Peanut-GB APU register hooks (0xFF10..0xFF3F). */
uint8_t gb_audio_read(uint16_t addr);
void    gb_audio_write(uint16_t addr, uint8_t val);

#endif /* GB_AUDIO_H */
