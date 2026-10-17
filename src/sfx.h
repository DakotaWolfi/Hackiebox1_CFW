/*
 * Sound-prompt archive.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SFX_H
#define SFX_H

#include <stdint.h>

/* Prompt set 0: writes the prompt files of language 0 from
 * /fw/sfx.bin, at most once per run; block is 4096 bytes of work space. */
void sfx_unpack(uint8_t *block);

#endif
