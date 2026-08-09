#include "format.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
	FN_TEXT,
	FN_VAR,
	FN_GROUP,    /* [children](style) */
	FN_OPTIONAL, /* (children) */
} fnode_type;

struct fnode {
	fnode_type type;
	char *text; /* literal, variable name, or style spec */
	fnode *kids;
	fnode *next;
};

static fnode *node_new(fnode_type type)
{
	fnode *n = xmalloc(sizeof *n);
	n->type = type;
	n->text = NULL;
	n->kids = NULL;
	n->next = NULL;
	return n;
}

void fmt_free(fnode *n)
{
	while (n) {
		fnode *next = n->next;
		fmt_free(n->kids);
		free(n->text);
		free(n);
		n = next;
	}
}

static bool is_var_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_';
}

static fnode *parse_seq(const char **pp, char terminator, bool *ok);

static void append(fnode **head, fnode **tail, fnode *n)
{
	if (*tail)
		(*tail)->next = n;
	else
		*head = n;
	*tail = n;
}

static void flush_text(str *buf, fnode **head, fnode **tail)
{
	if (!buf->len)
		return;
	fnode *n = node_new(FN_TEXT);
	n->text = str_take(buf);
	append(head, tail, n);
}

static fnode *parse_seq(const char **pp, char terminator, bool *ok)
{
	const char *p = *pp;
	fnode *head = NULL, *tail = NULL;
	str buf = {0};

	while (*p && *p != terminator) {
		if (*p == '\\') {
			p++;
			if (*p)
				str_addc(&buf, *p++);
			continue;
		}

		if (*p == '$') {
			p++;
			if (*p == '$') { /* $$ is a literal dollar */
				str_addc(&buf, '$');
				p++;
				continue;
			}

			str name = {0};
			if (*p == '{') {
				p++;
				while (*p && *p != '}')
					str_addc(&name, *p++);
				if (*p != '}') {
					str_free(&name);
					str_free(&buf);
					fmt_free(head);
					*ok = false;
					return NULL;
				}
				p++;
			} else {
				while (is_var_char(*p))
					str_addc(&name, *p++);
			}

			if (!name.len) { /* a bare '$' is literal text */
				str_addc(&buf, '$');
				str_free(&name);
				continue;
			}

			flush_text(&buf, &head, &tail);
			fnode *n = node_new(FN_VAR);
			n->text = str_take(&name);
			append(&head, &tail, n);
			continue;
		}

		if (*p == '[') {
			p++;
			flush_text(&buf, &head, &tail);

			fnode *n = node_new(FN_GROUP);
			n->kids = parse_seq(&p, ']', ok);
			if (!*ok || *p != ']') {
				fmt_free(n);
				str_free(&buf);
				fmt_free(head);
				*ok = false;
				return NULL;
			}
			p++;

			if (*p != '(') {
				fmt_free(n);
				str_free(&buf);
				fmt_free(head);
				*ok = false;
				return NULL;
			}
			p++;

			str spec = {0};
			while (*p && *p != ')') {
				if (*p == '\\' && p[1])
					p++;
				str_addc(&spec, *p++);
			}
			if (*p != ')') {
				str_free(&spec);
				fmt_free(n);
				str_free(&buf);
				fmt_free(head);
				*ok = false;
				return NULL;
			}
			p++;

			n->text = str_take(&spec);
			append(&head, &tail, n);
			continue;
		}

		if (*p == '(') {
			p++;
			flush_text(&buf, &head, &tail);

			fnode *n = node_new(FN_OPTIONAL);
			n->kids = parse_seq(&p, ')', ok);
			if (!*ok || *p != ')') {
				fmt_free(n);
				str_free(&buf);
				fmt_free(head);
				*ok = false;
				return NULL;
			}
			p++;
			append(&head, &tail, n);
			continue;
		}

		str_addc(&buf, *p++);
	}

	flush_text(&buf, &head, &tail);
	str_free(&buf);
	*pp = p;
	return head;
}

fnode *fmt_parse(const char *format)
{
	if (!format)
		return NULL;

	const char *p = format;
	bool ok = true;
	fnode *n = parse_seq(&p, '\0', &ok);

	if (!ok || *p != '\0') {
		fmt_free(n);
		return NULL;
	}

	/* An empty format parses fine but yields no nodes, which would otherwise
	 * be indistinguishable from a syntax error. Hand back an empty text node
	 * so that NULL always means "broken". */
	if (!n) {
		n = node_new(FN_TEXT);
		n->text = xstrdup("");
	}
	return n;
}

static bool expand_vars(const char *spec, const render_ctx *ctx, str *out)
{
	bool any = false;

	for (const char *p = spec; *p;) {
		if (*p != '$') {
			str_addc(out, *p++);
			continue;
		}

		p++;
		str name = {0};
		if (*p == '{') {
			p++;
			while (*p && *p != '}')
				str_addc(&name, *p++);
			if (*p == '}')
				p++;
		} else {
			while (is_var_char(*p))
				str_addc(&name, *p++);
		}

		if (name.len && ctx->resolve && ctx->resolve(ctx->user, name.data, out))
			any = true;
		str_free(&name);
	}
	return any;
}

void fmt_apply_style_spec(style *s, const char *spec, const render_ctx *ctx)
{
	if (!spec || !*spec)
		return;

	if (!strchr(spec, '$')) {
		style_apply(s, spec, ctx->palette);
		return;
	}

	str expanded = {0};
	expand_vars(spec, ctx, &expanded);
	style_apply(s, expanded.data ? expanded.data : "", ctx->palette);
	str_free(&expanded);
}

typedef struct {
	bool has_var;
	bool any_nonempty;
} render_stat;

static void render_seq(const fnode *n, const render_ctx *ctx, const style *parent, str *out,
                       render_stat *st);

/* Emits `body` wrapped in the SGR codes for `s`, re-asserting the style after
 * any embedded escape sequence so nested module output cannot leak through. */
static void emit_styled(str *out, const style *s, const esc_wrap *w, const char *body, size_t len)
{
	if (!len)
		return;

	if (style_is_plain(s)) {
		str_addn(out, body, len);
		return;
	}

	static const char reset[] = "\x1b[0m";
	const size_t reset_len = sizeof reset - 1;
	const size_t close_len = strlen(w->close);

	style_open(out, s, w);

	size_t i = 0;
	bool ends_reset = false;

	while (i < len) {
		if (len - i >= reset_len && memcmp(body + i, reset, reset_len) == 0) {
			str_addn(out, body + i, reset_len);
			i += reset_len;
			if (close_len && len - i >= close_len &&
			    memcmp(body + i, w->close, close_len) == 0) {
				str_addn(out, body + i, close_len);
				i += close_len;
			}
			/* Nothing left to style means no need to re-assert it. */
			ends_reset = (i == len);
			if (!ends_reset)
				style_open(out, s, w);
			continue;
		}
		str_addc(out, body[i++]);
		ends_reset = false;
	}

	if (!ends_reset)
		style_close(out, w);
}

static void render_node(const fnode *n, const render_ctx *ctx, const style *parent, str *out,
                        render_stat *st)
{
	switch (n->type) {
	case FN_TEXT:
		if (ctx->escape_percent) {
			for (const char *p = n->text; *p; p++) {
				str_addc(out, *p);
				if (*p == '%')
					str_addc(out, '%');
			}
		} else {
			str_add(out, n->text);
		}
		break;

	case FN_VAR: {
		st->has_var = true;
		str val = {0};
		if (ctx->resolve && ctx->resolve(ctx->user, n->text, &val) && val.len) {
			st->any_nonempty = true;
			str_addn(out, val.data, val.len);
		}
		str_free(&val);
		break;
	}

	case FN_GROUP: {
		style s = *parent;
		fmt_apply_style_spec(&s, n->text, ctx);

		str body = {0};
		render_seq(n->kids, ctx, &s, &body, st);
		emit_styled(out, &s, ctx->wrap, body.data ? body.data : "", body.len);
		str_free(&body);
		break;
	}

	case FN_OPTIONAL: {
		render_stat inner = {0};
		str body = {0};
		render_seq(n->kids, ctx, parent, &body, &inner);

		/* A group with variables that all came back empty disappears. */
		if (!inner.has_var || inner.any_nonempty)
			str_addn(out, body.data, body.len);

		st->has_var |= inner.has_var;
		st->any_nonempty |= inner.any_nonempty;
		str_free(&body);
		break;
	}
	}
}

static void render_seq(const fnode *n, const render_ctx *ctx, const style *parent, str *out,
                       render_stat *st)
{
	for (; n; n = n->next)
		render_node(n, ctx, parent, out, st);
}

void fmt_render(const fnode *n, const render_ctx *ctx, str *out)
{
	style base = {0};
	render_stat st = {0};
	render_seq(n, ctx, &base, out, &st);
}
