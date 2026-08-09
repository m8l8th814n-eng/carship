#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("carship: out of memory\n", stderr);
		exit(1);
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q) {
		fputs("carship: out of memory\n", stderr);
		exit(1);
	}
	return q;
}

char *xstrdup(const char *s)
{
	return s ? xstrndup(s, strlen(s)) : NULL;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void str_reserve(str *s, size_t extra)
{
	if (s->len + extra + 1 <= s->cap)
		return;
	size_t cap = s->cap ? s->cap : 64;
	while (cap < s->len + extra + 1)
		cap *= 2;
	s->data = xrealloc(s->data, cap);
	s->cap = cap;
}

void str_addc(str *s, char c)
{
	str_reserve(s, 1);
	s->data[s->len++] = c;
	s->data[s->len] = '\0';
}

void str_addn(str *s, const char *p, size_t n)
{
	if (!p || !n)
		return;
	str_reserve(s, n);
	memcpy(s->data + s->len, p, n);
	s->len += n;
	s->data[s->len] = '\0';
}

void str_add(str *s, const char *p)
{
	if (p)
		str_addn(s, p, strlen(p));
}

void str_addf(str *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	va_list copy;
	va_copy(copy, ap);
	int n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (n > 0) {
		str_reserve(s, (size_t)n);
		vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap);
		s->len += (size_t)n;
	}
	va_end(ap);
}

void str_clear(str *s)
{
	s->len = 0;
	if (s->data)
		s->data[0] = '\0';
}

void str_free(str *s)
{
	free(s->data);
	s->data = NULL;
	s->len = s->cap = 0;
}

char *str_take(str *s)
{
	char *p = s->data ? s->data : xstrdup("");
	s->data = NULL;
	s->len = s->cap = 0;
	return p;
}

bool str_eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

bool has_prefix(const char *s, const char *prefix)
{
	size_t n = strlen(prefix);
	return strncmp(s, prefix, n) == 0;
}

bool has_suffix(const char *s, const char *suffix)
{
	size_t ls = strlen(s), lx = strlen(suffix);
	return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

char *trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

char *read_file(const char *path, size_t *len_out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	str out = {0};
	char buf[8192];
	ssize_t n;
	while ((n = read(fd, buf, sizeof buf)) > 0)
		str_addn(&out, buf, (size_t)n);
	close(fd);

	if (n < 0) {
		str_free(&out);
		return NULL;
	}
	if (len_out)
		*len_out = out.len;
	return str_take(&out);
}

bool file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

bool dir_has_entry(const char *dir, const char *name)
{
	str p = {0};
	str_addf(&p, "%s/%s", dir, name);
	bool found = file_exists(p.data);
	str_free(&p);
	return found;
}

/* A PATH element that is not absolute resolves against the working directory,
 * so a hostile repository could ship its own "go" or "node" and have carship
 * run it the moment the prompt is drawn. Version lookups therefore execute
 * with those elements removed. Returns NULL when PATH is already safe. */
static const char *safe_path(void)
{
	static char *cached;
	static bool computed;

	if (computed)
		return cached;
	computed = true;

	const char *path = getenv("PATH");
	if (!path)
		return NULL;

	str clean = {0};
	bool dropped = false;

	for (const char *p = path; *p;) {
		const char *sep = strchr(p, ':');
		size_t len = sep ? (size_t)(sep - p) : strlen(p);

		/* An empty element means the working directory, same as ".". */
		if (len > 0 && p[0] == '/') {
			if (clean.len)
				str_addc(&clean, ':');
			str_addn(&clean, p, len);
		} else {
			dropped = true;
		}

		p = sep ? sep + 1 : p + len;
	}

	if (!dropped) {
		str_free(&clean);
		return NULL;
	}

	cached = str_take(&clean);
	return cached;
}

char *capture(const char *cwd, char *const argv[], int timeout_ms)
{
	const char *path = safe_path();

	int pipefd[2];
	if (pipe(pipefd) < 0)
		return NULL;

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	if (pid == 0) {
		close(pipefd[0]);
		if (path)
			setenv("PATH", path, 1);
		if (cwd && chdir(cwd) != 0)
			_exit(127);
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
		dup2(pipefd[1], STDOUT_FILENO);
		if (pipefd[1] > STDERR_FILENO)
			close(pipefd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(pipefd[1]);
	fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

	str out = {0};
	struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
	bool timed_out = false;

	for (;;) {
		int r = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			timed_out = true;
			kill(pid, SIGKILL);
			break;
		}
		char buf[4096];
		ssize_t n = read(pipefd[0], buf, sizeof buf);
		if (n > 0) {
			str_addn(&out, buf, (size_t)n);
		} else if (n == 0) {
			break;
		} else if (errno != EAGAIN && errno != EINTR) {
			break;
		}
	}

	close(pipefd[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;

	if (timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		str_free(&out);
		return NULL;
	}
	return str_take(&out);
}

static int rune_width(uint32_t cp)
{
	if (cp == 0)
		return 0;
	if (cp < 32 || (cp >= 0x7f && cp < 0xa0))
		return 0;
	/* Combining marks and zero-width joiners occupy no cells. */
	if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x200b && cp <= 0x200f) ||
	    (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0x20d0 && cp <= 0x20f0))
		return 0;
	if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) ||
	    (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
	    (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
	    (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
	    (cp >= 0x1f900 && cp <= 0x1f9ff) || (cp >= 0x20000 && cp <= 0x3fffd))
		return 2;
	return 1;
}

size_t display_width(const char *s)
{
	size_t w = 0;
	const unsigned char *p = (const unsigned char *)s;

	while (*p) {
		if (*p == 0x1b) {
			p++;
			if (*p == '[') {
				p++;
				while (*p && (*p < '@' || *p > '~'))
					p++;
				if (*p)
					p++;
			} else if (*p == ']') {
				p++;
				while (*p && *p != 0x07)
					p++;
				if (*p)
					p++;
			} else if (*p) {
				p++;
			}
			continue;
		}

		uint32_t cp;
		int len;
		if (*p < 0x80) {
			cp = *p;
			len = 1;
		} else if ((*p & 0xe0) == 0xc0) {
			cp = *p & 0x1fu;
			len = 2;
		} else if ((*p & 0xf0) == 0xe0) {
			cp = *p & 0x0fu;
			len = 3;
		} else if ((*p & 0xf8) == 0xf0) {
			cp = *p & 0x07u;
			len = 4;
		} else {
			p++;
			continue;
		}

		int i;
		for (i = 1; i < len; i++) {
			if ((p[i] & 0xc0) != 0x80)
				break;
			cp = (cp << 6) | (p[i] & 0x3fu);
		}
		if (i < len) {
			p++;
			continue;
		}

		w += (size_t)rune_width(cp);
		p += len;
	}
	return w;
}
