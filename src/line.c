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
#include "tig/types.h"
#include "tig/refdb.h"
#include "tig/line.h"
#include "tig/util.h"
#include "tig/ansi.h"

static struct line_rule *line_rule;
static size_t line_rules;

struct line_color_pair {
	int fg;
	int bg;
};

static struct line_color_pair *color_pair;
static size_t color_pairs;

/* Line types created on demand for ANSI colors. */
static enum line_type *color_type;
static size_t color_types;

static bool colors_enabled;
static bool colors_direct;
static int default_fg = COLOR_WHITE;
static int default_bg = COLOR_BLACK;

/* Extended color pairs take colors beyond the range of a short, as
 * needed for the RGB values of direct color terminals. */
#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS >= 20170401
#define HAVE_EXTENDED_PAIRS 1
#endif

DEFINE_ALLOCATOR(realloc_line_rule, struct line_rule, 8)
DEFINE_ALLOCATOR(realloc_color_pair, struct line_color_pair, 8)
DEFINE_ALLOCATOR(realloc_color_type, enum line_type, 32)

enum line_type
get_line_type(const char *line)
{
	int linelen = strlen(line);
	enum line_type type;

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];

		if (rule->regex && !regexec(rule->regex, line, 0, NULL, 0))
			return type;

		/* Case insensitive search matches Signed-off-by lines better. */
		if (rule->linelen && linelen >= rule->linelen &&
		    !strncasecmp(rule->line, line, rule->linelen))
			return type;
	}

	return LINE_DEFAULT;
}

enum line_type
get_line_type_from_ref(const struct ref *ref)
{
	if (ref->type == REFERENCE_HEAD)
		return LINE_MAIN_HEAD;
	else if (ref->type == REFERENCE_LOCAL_TAG)
		return LINE_MAIN_LOCAL_TAG;
	else if (ref->type == REFERENCE_TAG)
		return LINE_MAIN_TAG;
	else if (ref->type == REFERENCE_TRACKED_REMOTE)
		return LINE_MAIN_TRACKED;
	else if (ref->type == REFERENCE_REMOTE)
		return LINE_MAIN_REMOTE;
	else if (ref->type == REFERENCE_STASH)
		return LINE_MAIN_STASH;
	else if (ref->type == REFERENCE_NOTE)
		return LINE_MAIN_NOTE;
	else if (ref->type == REFERENCE_PREFETCH)
		return LINE_MAIN_PREFETCH;
	else if (ref->type == REFERENCE_OTHER)
		return LINE_MAIN_OTHER;
	else if (ref->type == REFERENCE_REPLACE)
		return LINE_MAIN_REPLACE;

	return LINE_MAIN_REF;
}

const char *
get_line_type_name(enum line_type type)
{
	assert(0 <= type && type < line_rules);
	return line_rule[type].name;
}

struct line_info *
get_line_info(const char *prefix, enum line_type type)
{
	struct line_info *info;
	struct line_rule *rule;

	assert(0 <= type && type < line_rules);
	rule = &line_rule[type];
	for (info = &rule->info; info; info = info->next) {
		if (prefix && info->prefix == prefix)
			return info;
		if (!prefix && !info->prefix)
			return info;
	}

	return &rule->info;
}

static struct line_info *
init_line_info(const char *prefix, const char *name, size_t namelen, const char *line, size_t linelen, regex_t *regex)
{
	struct line_rule *rule;

	if (!realloc_line_rule(&line_rule, line_rules, 1))
		die("Failed to allocate line info");

	rule = &line_rule[line_rules++];
	rule->name = name;
	rule->namelen = namelen;
	rule->line = line;
	rule->linelen = linelen;
	rule->regex = regex;

	rule->info.prefix = prefix;
	rule->info.fg = COLOR_DEFAULT;
	rule->info.bg = COLOR_DEFAULT;

	return &rule->info;
}

#define INIT_BUILTIN_LINE_INFO(type, line) \
	init_line_info(NULL, #type, STRING_SIZE(#type), (line), STRING_SIZE(line), NULL)

static struct line_rule *
find_line_rule(struct line_rule *query)
{
	enum line_type type;

	if (!line_rules) {
		LINE_INFO(INIT_BUILTIN_LINE_INFO);
	}

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];

		if (query->namelen && enum_equals(*rule, query->name, query->namelen))
			return rule;

		if (query->linelen && query->linelen == rule->linelen &&
		    !strncasecmp(rule->line, query->line, rule->linelen))
			return rule;
	}

	return NULL;
}

struct line_info *
add_line_rule(const char *prefix, struct line_rule *query)
{
	struct line_rule *rule = find_line_rule(query);
	struct line_info *info, *last;

	if (!rule) {
		if (query->name)
			return NULL;

		return init_line_info(prefix, "", 0, query->line, query->linelen, query->regex);
	}

	/* When a rule already exists and we are just adding view-specific
	 * colors, query->line and query->regex can be freed. */
	free((void *) query->line);
	if (query->regex) {
		regfree(query->regex);
		free(query->regex);
	}

	for (info = &rule->info; info; last = info, info = info->next)
		if (info->prefix == prefix)
			return info;

	info = calloc(1, sizeof(*info));
	if (info)
		info->prefix = prefix;
	last->next = info;
	return info;
}

bool
foreach_line_rule(line_rule_visitor_fn visitor, void *data)
{
	enum line_type type;

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];

		if (!visitor(data, rule))
			return false;
	}

	return true;
}

/* Color pair IDs are limited to 8 bits by COLOR_PAIR(). */
#define MAX_COLOR_PAIRS	255

/* Direct color terminals only know the eight basic colors by number and
 * take RGB values for anything else. */
static int
terminal_color(int color)
{
	int rgb;

	if (!colors_direct || color == COLOR_DEFAULT || color < 8)
		return color;

	rgb = ansi_color_is_rgb(color) ? ANSI_COLOR_RGB_VALUE(color)
				       : ansi_color_to_rgb(color);

	return rgb < 8 ? COLOR_BLACK : rgb;
}

static bool
init_line_info_color_pair(struct line_info *info)
{
	int bg = terminal_color(info->bg == COLOR_DEFAULT ? default_bg : info->bg);
	int fg = terminal_color(info->fg == COLOR_DEFAULT ? default_fg : info->fg);
	int i;

	for (i = 0; i < color_pairs; i++) {
		if (color_pair[i].fg == info->fg && color_pair[i].bg == info->bg) {
			info->color_pair = i;
			return true;
		}
	}

	if (COLOR_ID(color_pairs) > MAX_COLOR_PAIRS || COLOR_ID(color_pairs) >= COLOR_PAIRS)
		return false;

	if (!realloc_color_pair(&color_pair, color_pairs, 1))
		die("Failed to allocate color pair");

	color_pair[color_pairs].fg = info->fg;
	color_pair[color_pairs].bg = info->bg;
	info->color_pair = color_pairs++;
#ifdef HAVE_EXTENDED_PAIRS
	init_extended_pair(COLOR_ID(info->color_pair), fg, bg);
#else
	init_pair(COLOR_ID(info->color_pair), fg, bg);
#endif
	return true;
}

void
init_colors(void)
{
	char *no_color = getenv("NO_COLOR");
	struct line_rule query = { "default", STRING_SIZE("default") };
	struct line_rule *rule = find_line_rule(&query);
	enum line_type type;

	default_bg = rule ? rule->info.bg : COLOR_BLACK;
	default_fg = rule ? rule->info.fg : COLOR_WHITE;

	/* XXX: Even if the terminal does not support colors (e.g.
	 * TERM=dumb) init_colors() must ensure that the built-in rules
	 * have been initialized. This is done by the above call to
	 * find_line_rule(). */
	if (!has_colors() || (no_color != NULL && no_color[0] != '\0'))
		return;

	start_color();
	colors_enabled = true;
#ifdef HAVE_EXTENDED_PAIRS
	colors_direct = COLORS >= 1 << 24;
#endif

	if (assume_default_colors(terminal_color(default_fg), terminal_color(default_bg)) == ERR) {
		default_bg = COLOR_BLACK;
		default_fg = COLOR_WHITE;
	}

	for (type = 0; type < line_rules; type++) {
		struct line_rule *rule = &line_rule[type];
		struct line_info *info;

		for (info = &rule->info; info; info = info->next) {
			init_line_info_color_pair(info);
		}
	}
}

bool
has_direct_colors(void)
{
	return colors_direct;
}

/* Reduce a color to what the terminal supports. */
static int
reduce_color(int color, int *attr)
{
	if (color == COLOR_DEFAULT || colors_direct)
		return color;

	if (ansi_color_is_rgb(color))
		color = ansi_color_to_256(color);

	if (color < COLORS)
		return color;

	if (color >= 16 && COLORS < 256)
		color = ansi_color_to_16(color);

	if (color >= 8 && COLORS < 16) {
		color -= 8;
		if (attr)
			*attr |= A_BOLD;
	}

	return color < COLORS ? color : COLOR_DEFAULT;
}

enum line_type
get_line_type_from_color(int fg, int bg, int attr, enum line_type base)
{
	struct line_info *info = get_line_info(NULL, base);
	int base_attr = info->attr;
	int base_fg = colors_enabled ? reduce_color(info->fg, &base_attr) : COLOR_DEFAULT;
	int base_bg = colors_enabled ? reduce_color(info->bg, NULL) : COLOR_DEFAULT;
	int base_pair = info->color_pair;
	size_t i;

	if (!colors_enabled) {
		fg = COLOR_DEFAULT;
		bg = COLOR_DEFAULT;
	} else {
		fg = reduce_color(fg, &attr);
		bg = reduce_color(bg, NULL);
	}

	if (fg == base_fg && bg == base_bg && attr == base_attr)
		return base;

	for (i = 0; i < color_types; i++) {
		info = &line_rule[color_type[i]].info;
		if (info->fg == fg && info->bg == bg && info->attr == attr)
			return color_type[i];
	}

	if (!realloc_color_type(&color_type, color_types, 1))
		return base;

	info = init_line_info(NULL, "", 0, "", 0, NULL);
	info->fg = fg;
	info->bg = bg;
	info->attr = attr;

	/* Out of color pairs; keep the attributes with the colors of @base. */
	if (colors_enabled && !init_line_info_color_pair(info))
		info->color_pair = base_pair;

	color_type[color_types++] = (enum line_type) (line_rules - 1);
	return (enum line_type) (line_rules - 1);
}

/* vim: set ts=8 sw=8 noexpandtab: */
