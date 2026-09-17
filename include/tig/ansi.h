/* Copyright (c) 2006-2026 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef TIG_ANSI_H
#define TIG_ANSI_H

#include "tig/tig.h"

/*
 * ANSI escape sequence handling.
 *
 * Only Select Graphic Rendition (SGR) sequences affect the state;
 * other sequences are recognized so they can be skipped.
 */

struct ansi_state {
	int fg;		/* COLOR_DEFAULT or an index in the 256-color palette. */
	int bg;
	int attr;	/* Curses attributes. */
};

#define ANSI_STATE_INIT { COLOR_DEFAULT, COLOR_DEFAULT, A_NORMAL }

static inline bool
ansi_state_is_default(const struct ansi_state *state)
{
	return state->fg == COLOR_DEFAULT &&
	       state->bg == COLOR_DEFAULT &&
	       state->attr == A_NORMAL;
}

/* Parse the escape sequence starting at @esc, which must point at an
 * escape character, and update @state accordingly. Returns a pointer
 * to the first byte after the sequence. */
const char *ansi_parse(const char *esc, struct ansi_state *state);

/* Copy @src to @dst without escape sequences. @dst must have room for
 * strlen(src) + 1 bytes. Returns the length of the result. */
size_t ansi_strip(char *dst, const char *src);

/* Map an index in the 256-color palette to the closest of the 16
 * basic colors. */
int ansi_color_to_16(int color);

#endif

/* vim: set ts=8 sw=8 noexpandtab: */
