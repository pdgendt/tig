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

#include "tig/tig.h"
#include "tig/ansi.h"

#define ANSI_ESC		'\033'
#define ANSI_MAX_PARAMS		32
#define ANSI_MAX_PARAM_VALUE	9999

/* Byte classes of ECMA-48 control sequences. */
#define ansi_is_param(c)	((c) >= 0x30 && (c) <= 0x3f)
#define ansi_is_intermediate(c)	((c) >= 0x20 && (c) <= 0x2f)
#define ansi_is_final(c)	((c) >= 0x40 && (c) <= 0x7e)

/* The 16 basic colors as rendered by xterm. */
static const unsigned char ansi_basic_colors[16][3] = {
	{ 0, 0, 0 },       { 205, 0, 0 },     { 0, 205, 0 },     { 205, 205, 0 },
	{ 0, 0, 238 },     { 205, 0, 205 },   { 0, 205, 205 },   { 229, 229, 229 },
	{ 127, 127, 127 }, { 255, 0, 0 },     { 0, 255, 0 },     { 255, 255, 0 },
	{ 92, 92, 255 },   { 255, 0, 255 },   { 0, 255, 255 },   { 255, 255, 255 },
};

/* The intensity levels of the 6x6x6 color cube. */
static const unsigned char ansi_cube_levels[6] = { 0, 95, 135, 175, 215, 255 };

static int
ansi_cube_index(int level)
{
	if (level < 48)
		return 0;
	if (level < 115)
		return 1;
	return (level - 35) / 40;
}

/* Map an RGB triple to the closest entry in the 256-color palette. */
static int
ansi_color_from_rgb(int r, int g, int b)
{
	if (r == g && g == b) {
		if (r < 8)
			return 16;
		if (r > 246)
			return 231;
		return 232 + MIN((r - 8 + 5) / 10, 23);
	}

	return 16 + 36 * ansi_cube_index(r) + 6 * ansi_cube_index(g) + ansi_cube_index(b);
}

int
ansi_color_to_rgb(int color)
{
	int r, g, b;

	assert(color >= 0 && color < 256);

	if (color < 16) {
		r = ansi_basic_colors[color][0];
		g = ansi_basic_colors[color][1];
		b = ansi_basic_colors[color][2];
	} else if (color >= 232) {
		r = g = b = 8 + (color - 232) * 10;
	} else {
		color -= 16;
		r = ansi_cube_levels[color / 36];
		g = ansi_cube_levels[(color / 6) % 6];
		b = ansi_cube_levels[color % 6];
	}

	return (r << 16) | (g << 8) | b;
}

int
ansi_color_to_256(int color)
{
	int rgb = ANSI_COLOR_RGB_VALUE(color);

	return ansi_color_from_rgb(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

int
ansi_color_to_16(int color)
{
	int rgb, r, g, b;
	int best = 0;
	int best_distance = -1;
	int i;

	if (color < 16)
		return color;

	rgb = ansi_color_to_rgb(color);
	r = rgb >> 16;
	g = (rgb >> 8) & 255;
	b = rgb & 255;

	for (i = 0; i < 16; i++) {
		int dr = r - ansi_basic_colors[i][0];
		int dg = g - ansi_basic_colors[i][1];
		int db = b - ansi_basic_colors[i][2];
		int distance = dr * dr + dg * dg + db * db;

		if (best_distance < 0 || distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}

	return best;
}

/* Parse the @count arguments of an extended color (38 or 48) parameter.
 * Returns the number of arguments consumed. */
static int
ansi_parse_extended_color(const int *args, int count, bool colons, int *color)
{
	if (count < 1)
		return 0;

	if (args[0] == 5) {
		if (count < 2)
			return count;
		if (args[1] >= 0 && args[1] < 256)
			*color = args[1];
		return 2;
	}

	if (args[0] == 2) {
		int offset = 1;

		/* The colon form may carry a color space identifier. */
		if (colons && count >= 5)
			offset = 2;
		if (count < offset + 3)
			return count;
		*color = ANSI_COLOR_RGB
		       | (MIN(args[offset], 255) << 16)
		       | (MIN(args[offset + 1], 255) << 8)
		       | MIN(args[offset + 2], 255);
		return offset + 3;
	}

	return 1;
}

static void
ansi_parse_sgr(const char *params, const char *end, struct ansi_state *state)
{
	int args[ANSI_MAX_PARAMS];
	char seps[ANSI_MAX_PARAMS];
	int count = 0;
	int i;

	/* No parameters is the same as a reset. */
	if (params == end) {
		state->fg = COLOR_DEFAULT;
		state->bg = COLOR_DEFAULT;
		state->attr = A_NORMAL;
		return;
	}

	while (params < end && count < ANSI_MAX_PARAMS) {
		int value = 0;

		while (params < end && isdigit((unsigned char) *params)) {
			value = MIN(value * 10 + (*params - '0'), ANSI_MAX_PARAM_VALUE);
			params++;
		}

		args[count] = value;
		seps[count] = params < end ? *params : 0;
		count++;

		if (params < end && (*params == ';' || *params == ':'))
			params++;
		else
			break;
	}

	for (i = 0; i < count; i++) {
		int arg = args[i];

		switch (arg) {
		case 0:
			state->fg = COLOR_DEFAULT;
			state->bg = COLOR_DEFAULT;
			state->attr = A_NORMAL;
			break;
		case 1:
			state->attr |= A_BOLD;
			break;
		case 2:
			state->attr |= A_DIM;
			break;
#ifdef A_ITALIC
		case 3:
			state->attr |= A_ITALIC;
			break;
#endif
		case 4:
			state->attr |= A_UNDERLINE;
			break;
		case 5:
		case 6:
			state->attr |= A_BLINK;
			break;
		case 7:
			state->attr |= A_REVERSE;
			break;
		case 8:
			state->attr |= A_INVIS;
			break;
		case 22:
			state->attr &= ~(A_BOLD | A_DIM);
			break;
#ifdef A_ITALIC
		case 23:
			state->attr &= ~A_ITALIC;
			break;
#endif
		case 24:
			state->attr &= ~A_UNDERLINE;
			break;
		case 25:
			state->attr &= ~A_BLINK;
			break;
		case 27:
			state->attr &= ~A_REVERSE;
			break;
		case 28:
			state->attr &= ~A_INVIS;
			break;
		case 38:
		case 48: {
			int *color = arg == 38 ? &state->fg : &state->bg;
			int subs = 0;

			/* Sub-parameters joined by colons all belong to this
			 * parameter, otherwise the following ones may not. */
			while (i + 1 + subs < count && seps[i + subs] == ':')
				subs++;
			if (subs)
				i += ansi_parse_extended_color(args + i + 1, subs, true, color);
			else
				i += ansi_parse_extended_color(args + i + 1, count - i - 1, false, color);
			break;
		}
		case 39:
			state->fg = COLOR_DEFAULT;
			break;
		case 49:
			state->bg = COLOR_DEFAULT;
			break;
		default:
			if (arg >= 30 && arg <= 37)
				state->fg = arg - 30;
			else if (arg >= 40 && arg <= 47)
				state->bg = arg - 40;
			else if (arg >= 90 && arg <= 97)
				state->fg = arg - 90 + 8;
			else if (arg >= 100 && arg <= 107)
				state->bg = arg - 100 + 8;
			break;
		}
	}
}

static const char *
ansi_parse_csi(const char *p, struct ansi_state *state)
{
	const char *params = p;

	while (ansi_is_param((unsigned char) *p))
		p++;
	while (ansi_is_intermediate((unsigned char) *p))
		p++;

	if (!ansi_is_final((unsigned char) *p))
		return p;

	/* Private parameters (<=>?) belong to other sequences. */
	if (*p == 'm' && (params == p || (unsigned char) *params < 0x3c))
		ansi_parse_sgr(params, p, state);

	return p + 1;
}

/* Skip a control string, which is terminated by a string terminator
 * or, for compatibility, a bell character. */
static const char *
ansi_skip_string(const char *p)
{
	while (*p) {
		if (*p == '\a')
			return p + 1;
		if (*p == ANSI_ESC && p[1] == '\\')
			return p + 2;
		p++;
	}

	return p;
}

const char *
ansi_parse(const char *esc, struct ansi_state *state)
{
	const char *p = esc + 1;

	assert(*esc == ANSI_ESC);

	switch (*p) {
	case 0:
		return p;
	case '[':
		return ansi_parse_csi(p + 1, state);
	case ']':
	case 'P':
	case 'X':
	case '^':
	case '_':
		return ansi_skip_string(p + 1);
	default:
		while (ansi_is_intermediate((unsigned char) *p))
			p++;
		/* Only consume a valid final byte, keeping other text intact. */
		return (unsigned char) *p >= 0x30 && (unsigned char) *p <= 0x7e ? p + 1 : p;
	}
}

size_t
ansi_strip(char *dst, const char *src)
{
	struct ansi_state state = ANSI_STATE_INIT;
	size_t length = 0;

	while (*src) {
		if (*src == ANSI_ESC) {
			src = ansi_parse(src, &state);
		} else {
			dst[length++] = *src++;
		}
	}

	dst[length] = 0;
	return length;
}

/* vim: set ts=8 sw=8 noexpandtab: */
