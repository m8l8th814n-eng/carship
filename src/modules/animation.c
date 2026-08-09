#include "module.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *const default_frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                             "⠴", "⠦", "⠧", "⠇", "⠏"};

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Picks entry (now / interval) % count, so every process that renders the
 * prompt lands on the same frame without any shared state. */
static const char *pick(const toml_value *list, long long tick, const char *const *fallback,
                        size_t fallback_n)
{
	if (list && list->type == TOML_ARRAY && list->arr.n > 0) {
		size_t i = (size_t)(tick % (long long)list->arr.n);
		const char *s = toml_str(list->arr.items[i], NULL);
		if (s)
			return s;
	}
	if (!fallback || !fallback_n)
		return NULL;
	return fallback[(size_t)(tick % (long long)fallback_n)];
}

static bool probe_animation(module_state *st)
{
	long long interval = toml_int_at(st->cfg, "interval", 1000);
	if (interval <= 0)
		interval = 1000;

	long long tick = now_ms() / interval;

	const char *frame = pick(toml_get(st->cfg, "frames"), tick, default_frames,
	                         sizeof default_frames / sizeof *default_frames);
	if (!frame)
		return false;

	var_addz(&st->vars, "frame", frame);
	var_addf(&st->vars, "index", "%lld", tick);

	/* An optional parallel list lets the colour cycle independently of the
	 * glyph, which is what makes a gradient possible. */
	const char *frame_style = pick(toml_get(st->cfg, "styles"), tick, NULL, 0);
	if (frame_style)
		var_add_raw(&st->vars, "frame_style", xstrdup(frame_style));
	else
		var_add_raw(&st->vars, "frame_style",
		            xstrdup(toml_str_at(st->cfg, "style", "bold cyan")));

	return true;
}

const module_def animation_modules[] = {
	{.name = "animation", .format = "[$frame]($frame_style) ", .style = "bold cyan",
	 .disabled = true, .probe = probe_animation},
};

const size_t animation_modules_count = sizeof animation_modules / sizeof *animation_modules;
