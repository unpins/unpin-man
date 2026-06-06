/*
 * unpin_man.c — front-end that turns mandoc into the `unpin man` renderer,
 * with a built-in reflowing pager.
 *
 * unpin knows nothing about man: it only exposes a binary's embedded metadata
 * through `unpin bundle list|dump`. This front-end is the other half — a
 * patched mandoc (the `man` package) that, given `man [pkg] [page]` (a missing
 * pkg defaults to `unpin`, so a bare `unpin man` shows unpin's own manual):
 *
 *   1. runs `$UNPIN_SELF bundle list <pkg>` to enumerate the embedded
 *      `unpin/man/` pages, picking the right (name, section, lang) and
 *      following whole-page `.so` redirects (shown by `bundle list` as
 *      symlink targets) — the port of unpin's old src/man.rs pick/roff_for;
 *   2. runs `$UNPIN_SELF bundle dump <pkg> <entry>`, slurps that roff into
 *      memory, and parses it once with mandoc's own parser.
 *
 * Rendering then diverges from stock mandoc. Stock `man` shells out to an
 * external pager (`less`), which cannot reflow — it pages a stream already
 * wrapped to a fixed width. Instead we keep the parsed tree and re-run
 * mandoc's terminal formatter at the live terminal width: on a tty we drive a
 * small built-in pager that, on SIGWINCH, re-renders at the new width and
 * re-anchors the view to the same content (mandoc's formatter is idempotent on
 * the tree, so re-render == re-parse). nroff backspace-overstrike is mapped to
 * ANSI SGR. When stdout is not a tty we emit one plain, styling-free render so
 * `unpin man pkg | grep` works. No external pager, no temp pager files.
 *
 * The term->fp hook the formatter writes through is added by
 * unpin-front-end.patch; everything below it is this front-end's own code.
 *
 * $UNPIN_SELF is the absolute path of the unpin binary that launched us (set by
 * unpin's `run`); we fall back to `unpin` on $PATH when it is unset, so the
 * package is still usable when invoked by hand.
 */
#include "config.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "mandoc.h"
#include "roff.h"
#include "mandoc_parse.h"
#include "manconf.h"
#include "out.h"
#include "term.h"
#include "main.h"

/* Provided by libc (BSD) or mandoc's compat_progname.c when HAVE_PROGNAME=0. */
extern void setprogname(const char *);

#define UNPIN_MAN_PREFIX "unpin/man/"
#define MAX_SO_DEPTH 4

struct entry {
	char	*name;		/* page name, e.g. "ls"                       */
	char	*lang;		/* language tag, e.g. "en"                    */
	int	 section;	/* leading digits of the section suffix       */
	char	*path;		/* full bundle path, e.g. "unpin/man/ls.1"    */
	int	 is_symlink;	/* a `.so` whole-page redirect                */
	char	*tgt_name;	/* redirect target name, when is_symlink      */
	int	 tgt_section;	/* redirect target section, when is_symlink   */
};

static const char *self;	/* the unpin binary to shell back to */

static void
die(const char *fmt, ...)
{
	va_list ap;

	fputs("man: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* Leading decimal digits of a section token: "1" -> 1, "3pm" -> 3, "x" -> 0. */
static int
parse_section(const char *s)
{
	int n = 0;

	while (isdigit((unsigned char)*s))
		n = n * 10 + (*s++ - '0');
	return n;
}

/*
 * Run `self bundle <op> ...` with stdout captured into a freshly malloc'd,
 * NUL-terminated buffer (its length, sans the NUL, is stored in *lenout when
 * non-NULL). A non-zero exit is a real failure (PKG not installed, unreadable
 * binary, corrupt bundle) and aborts — matching the bundle family's "absence
 * is not an error, but a broken read is" contract.
 */
static char *
capture(const char *const argv[], size_t *lenout)
{
	int	 fds[2];
	pid_t	 pid;
	char	*buf;
	size_t	 cap = 8192, len = 0;
	int	 st;

	if (pipe(fds) == -1)
		die("pipe: %s", strerror(errno));
	if ((pid = fork()) == -1)
		die("fork: %s", strerror(errno));
	if (pid == 0) {
		if (dup2(fds[1], STDOUT_FILENO) == -1)
			_exit(126);
		close(fds[0]);
		close(fds[1]);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(fds[1]);
	if ((buf = malloc(cap)) == NULL)
		die("out of memory");
	for (;;) {
		ssize_t n;

		if (len + 4096 >= cap) {
			cap *= 2;
			if ((buf = realloc(buf, cap)) == NULL)
				die("out of memory");
		}
		n = read(fds[0], buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("read from `%s bundle`: %s", self, strerror(errno));
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fds[0]);
	while (waitpid(pid, &st, 0) == -1 && errno == EINTR)
		;
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		die("`%s bundle %s ...` failed", self, argv[2]);
	buf[len] = '\0';
	if (lenout != NULL)
		*lenout = len;
	return buf;
}

/*
 * Parse one `bundle list` line into *e. Returns 1 if it is a man-page entry
 * (`unpin/man/...`), 0 otherwise (aliases, future kinds, blank lines). The
 * line is mutated in place: `e` borrows pointers into it.
 *
 * Line shapes (see unpin src/bundle.rs entry_line):
 *   unpin/man/ls.1\t909            regular roff page (size after the tab)
 *   unpin/man/dir.1\t-> ls.1       `.so` redirect (symlink target after `-> `)
 *   unpin/man/pt_BR/ls.1\t909      a non-default language page
 */
static int
parse_line(char *line, struct entry *e)
{
	static char	 en[] = "en";
	char		*tab, *rel, *file, *dot, *rhs;

	if ((tab = strchr(line, '\t')) == NULL)
		return 0;
	*tab = '\0';
	rhs = tab + 1;

	if (strncmp(line, UNPIN_MAN_PREFIX, sizeof(UNPIN_MAN_PREFIX) - 1) != 0)
		return 0;

	memset(e, 0, sizeof(*e));
	/* Keep an untruncated copy: name/lang below NUL-terminate within line. */
	if ((e->path = strdup(line)) == NULL)
		die("out of memory");
	rel = line + sizeof(UNPIN_MAN_PREFIX) - 1;

	/* `<lang>/<file>` or, with no slash, the default language `en`. */
	if ((file = strchr(rel, '/')) != NULL) {
		*file++ = '\0';
		e->lang = rel;
		if (strchr(file, '/') != NULL)
			return 0;	/* nested beyond one lang dir — skip */
	} else {
		e->lang = en;
		file = rel;
	}

	if ((dot = strrchr(file, '.')) == NULL)
		return 0;		/* no section suffix — not a page */
	*dot = '\0';
	e->name = file;
	e->section = parse_section(dot + 1);

	if (strncmp(rhs, "-> ", 3) == 0) {
		char *tgt = rhs + 3, *base, *tdot;

		while (*tgt == ' ')
			tgt++;
		base = strrchr(tgt, '/');
		base = base ? base + 1 : tgt;
		if ((tdot = strrchr(base, '.')) == NULL)
			die("malformed .so target %s", rhs + 3);
		*tdot = '\0';
		e->is_symlink = 1;
		e->tgt_name = base;
		e->tgt_section = parse_section(tdot + 1);
	}
	return 1;
}

/*
 * Pick the best page for (name, section, lang): prefer `lang`, fall back to
 * `en`; with section < 0 (unspecified) take the lowest-numbered one. Mirrors
 * unpin src/man.rs::pick.
 */
static const struct entry *
pick(const struct entry *ents, size_t n, const char *name, int section,
    const char *lang)
{
	const char *langs[2];
	size_t	    i, l;

	langs[0] = lang;
	langs[1] = "en";
	for (l = 0; l < 2; l++) {
		const struct entry *best = NULL;

		for (i = 0; i < n; i++) {
			const struct entry *e = &ents[i];

			if (strcmp(e->name, name) != 0 ||
			    strcmp(e->lang, langs[l]) != 0)
				continue;
			if (section >= 0 && e->section != section)
				continue;
			if (best == NULL || e->section < best->section)
				best = e;
		}
		if (best != NULL)
			return best;
		if (strcmp(langs[0], "en") == 0)
			break;	/* lang == "en": don't scan twice */
	}
	return NULL;
}

/*
 * Resolve (name, lang) to the bundle path of a roff page, following whole-page
 * `.so` redirects up to MAX_SO_DEPTH with cycle detection. Mirrors unpin
 * src/man.rs::roff_for. Returns a malloc'd copy of the entry path.
 */
static char *
resolve(const struct entry *ents, size_t n, const char *name, const char *lang)
{
	struct { const char *name; int section; } seen[MAX_SO_DEPTH];
	const char *cur_name = name;
	int	    cur_section = -1;	/* unspecified */
	size_t	    depth = 0;

	for (;;) {
		const struct entry *e;
		size_t		    i;

		for (i = 0; i < depth; i++)
			if (strcmp(seen[i].name, cur_name) == 0 &&
			    seen[i].section == cur_section)
				die("circular .so redirect at %s(%d)",
				    cur_name, cur_section);
		if (depth >= MAX_SO_DEPTH)
			die(".so redirect chain for %s exceeds %d hops",
			    name, MAX_SO_DEPTH);
		seen[depth].name = cur_name;
		seen[depth].section = cur_section;
		depth++;

		if ((e = pick(ents, n, cur_name, cur_section, lang)) == NULL) {
			if (depth > 1)
				die("broken .so redirect — %s not found",
				    cur_name);
			die("no embedded manual page for %s", cur_name);
		}
		if (!e->is_symlink) {
			char *p = strdup(e->path);

			if (p == NULL)
				die("out of memory");
			return p;
		}
		cur_name = e->tgt_name;
		cur_section = e->tgt_section;
	}
}

/* ================================================================== */
/* mandoc bridge: parse the roff once, re-format the tree at any width */
/* ================================================================== */

static struct mparse	 *g_mp;
static struct roff_meta	 *g_meta;

/* Parse the in-memory roff once into the node tree (kept for the run). The
 * parser only reads from an fd, so stage the bytes through a tmpfile; this
 * happens once per invocation, not per render. */
static void
parse_roff(const char *buf, size_t len)
{
	FILE	*tf;
	int	 rfd;

	mchars_alloc();
	g_mp = mparse_alloc(MPARSE_SO | MPARSE_UTF8 | MPARSE_LATIN1 |
	    MPARSE_VALIDATE, MANDOC_OS_OTHER, NULL);
	if ((tf = tmpfile()) == NULL)
		die("tmpfile: %s", strerror(errno));
	if (len && fwrite(buf, 1, len, tf) != len)
		die("staging roff: %s", strerror(errno));
	fflush(tf);
	/* mparse_readfd close(2)s the fd it is given, so hand it a dup and
	 * keep `tf` to unlink the tmpfile on fclose. */
	rfd = dup(fileno(tf));
	if (rfd == -1)
		die("dup: %s", strerror(errno));
	lseek(rfd, 0, SEEK_SET);
	mparse_readfd(g_mp, rfd, "man");
	fclose(tf);

	g_meta = mparse_result(g_mp);
	if (g_meta == NULL || g_meta->macroset == MACROSET_NONE)
		die("the manual page is not valid roff");
}

/* Render the cached tree at `width` columns (0 = mandoc default) into a
 * malloc'd byte buffer. The locale backend writes wide chars, so capture
 * through an fd-backed FILE and read the bytes back via the raw fd — mixing
 * byte and wide stdio on one FILE is undefined and corrupts at buffer
 * boundaries. */
static char *
doc_render(int width, size_t *outlen)
{
	struct manoutput out;
	struct termp	*p;
	FILE		*cap;
	char		*buf;
	off_t		 sz, off;
	ssize_t		 n;
	int		 fd;

	memset(&out, 0, sizeof(out));
	out.width = (size_t)width;
	p = locale_alloc(&out);
	if ((cap = tmpfile()) == NULL)
		die("tmpfile: %s", strerror(errno));
	p->fp = cap;
	if (g_meta->macroset == MACROSET_MDOC)
		terminal_mdoc(p, g_meta);
	else
		terminal_man(p, g_meta);
	fflush(cap);

	fd = fileno(cap);
	sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if ((buf = malloc((size_t)sz + 1)) == NULL)
		die("out of memory");
	for (off = 0; off < sz && (n = read(fd, buf + off, sz - off)) > 0; )
		off += n;
	buf[off] = '\0';
	fclose(cap);
	ascii_free(p);
	*outlen = (size_t)off;
	return buf;
}

/* ================================================================== */
/* overstrike -> ANSI, with a styling-stripped plain copy for anchoring */
/* ================================================================== */

struct line {
	char	*display;	/* bytes to print (with SGR escapes)   */
	char	*plain;		/* styling-stripped UTF-8 (for anchor) */
};

/* byte length of the UTF-8 glyph at s (1..4), clamped to max. */
static int
glyph_len(const char *s, int max)
{
	unsigned char c = (unsigned char)s[0];
	int n;

	if (c < 0x80)		n = 1;
	else if ((c >> 5) == 0x6) n = 2;
	else if ((c >> 4) == 0xe) n = 3;
	else if ((c >> 3) == 0x1e) n = 4;
	else			n = 1;
	return n > max ? max : n;
}

struct sb { char *b; size_t len, cap; };

static void
sb_add(struct sb *s, const char *p, size_t n)
{
	if (s->len + n + 1 > s->cap) {
		s->cap = (s->len + n + 1) * 2;
		if ((s->b = realloc(s->b, s->cap)) == NULL)
			die("out of memory");
	}
	memcpy(s->b + s->len, p, n);
	s->len += n;
	s->b[s->len] = '\0';
}

/*
 * Reduce one rendered line into display (ANSI SGR) + plain (no styling).
 * mandoc emits backspace-overstrike per output column: bold as `c\bc`,
 * underline as `_\bc`, bold+underline as `_\bc\bc`. Per column: underscore
 * with a real glyph => underline; the same glyph twice or more => bold; a
 * lone underscore stays a literal underscore.
 */
static void
process_line(const char *raw, size_t len, struct line *out)
{
	struct sb disp = {0}, plain = {0};
	size_t	  i = 0;
	int	  cb = 0, cu = 0;	/* current bold/under in display */

	while (i < len) {
		const char *disp_g = raw + i;
		int	    gn = glyph_len(raw + i, (int)(len - i));
		int	    disp_n = gn;
		int	    nUnder = (gn == 1 && raw[i] == '_');
		int	    nGlyph = !(gn == 1 && raw[i] == '_');
		int	    bold, under;

		i += gn;
		while (i < len && raw[i] == '\b') {
			int n2;

			i++;			/* skip '\b' */
			if (i >= len)
				break;
			n2 = glyph_len(raw + i, (int)(len - i));
			if (n2 == 1 && raw[i] == '_')
				nUnder++;
			else {
				nGlyph++;
				disp_g = raw + i;
				disp_n = n2;
			}
			i += n2;
		}

		if (nGlyph == 0) {		/* underscore(s) only */
			disp_g = "_";
			disp_n = 1;
			bold = nUnder >= 2;
			under = 0;
		} else {
			bold = nGlyph >= 2;
			under = nUnder >= 1;
		}

		if (bold != cb || under != cu) {
			if (cb || cu)
				sb_add(&disp, "\033[0m", 4);
			if (bold || under) {
				sb_add(&disp, "\033[", 2);
				if (bold)
					sb_add(&disp, "1", 1);
				if (bold && under)
					sb_add(&disp, ";", 1);
				if (under)
					sb_add(&disp, "4", 1);
				sb_add(&disp, "m", 1);
			}
			cb = bold;
			cu = under;
		}
		sb_add(&disp, disp_g, disp_n);
		sb_add(&plain, disp_g, disp_n);
	}
	if (cb || cu)
		sb_add(&disp, "\033[0m", 4);

	out->display = disp.b ? disp.b : strdup("");
	out->plain = plain.b ? plain.b : strdup("");
}

static struct line *
split_lines(const char *buf, size_t len, int *nout)
{
	struct line *lines = NULL;
	int	     n = 0, cap = 0;
	size_t	     i = 0;

	/* A trailing '\n' terminates the last line, not starts an empty one. */
	while (i < len) {
		size_t j = i;

		while (j < len && buf[j] != '\n')
			j++;
		if (n == cap) {
			cap = cap ? cap * 2 : 256;
			if ((lines = realloc(lines, cap * sizeof(*lines))) == NULL)
				die("out of memory");
		}
		process_line(buf + i, j - i, &lines[n++]);
		i = j + 1;
	}
	*nout = n;
	return lines;
}

static void
free_lines(struct line *l, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		free(l[i].display);
		free(l[i].plain);
	}
	free(l);
}

/* ================================================================== */
/* content anchor across a re-render at a new width                    */
/* ================================================================== */

static int
clampi(int v, int lo, int hi)
{
	if (hi < lo)
		hi = lo;
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void
anchor_key(const char *plain, char *out, int outsz)
{
	int i = 0;

	while (*plain == ' ' || *plain == '\t')
		plain++;
	while (plain[i] != '\0' && i < outsz - 1) {
		out[i] = plain[i];
		i++;
	}
	while (i > 0 && out[i - 1] == ' ')
		i--;
	out[i] = '\0';
}

/* New top index that keeps the same content in view after a re-render:
 * anchor on the first non-blank line at/below the old top, biased to the
 * proportional position to disambiguate repeats. */
static int
find_anchor(struct line *ol, int on, int otop, struct line *nl, int nn)
{
	char	key[80], k2[80];
	int	a, i, off, best = -1, bestd = 1 << 30, guess;
	double	frac;

	if (nn <= 0)
		return 0;
	for (a = otop; a < on; a++) {
		const char *q = ol[a].plain;
		while (*q == ' ')
			q++;
		if (*q != '\0')
			break;
	}
	if (a >= on)
		a = clampi(otop, 0, on - 1);
	anchor_key(ol[a].plain, key, sizeof(key));

	frac = on > 0 ? (double)a / on : 0.0;
	guess = (int)(frac * nn);
	if (key[0] == '\0')
		return clampi(guess, 0, nn - 1);

	for (i = 0; i < nn; i++) {
		size_t m;

		anchor_key(nl[i].plain, k2, sizeof(k2));
		if (k2[0] == '\0')
			continue;
		m = strlen(key) < strlen(k2) ? strlen(key) : strlen(k2);
		if (m >= 8 && strncmp(key, k2, m) == 0) {
			int d = i > guess ? i - guess : guess - i;
			if (d < bestd) {
				bestd = d;
				best = i;
			}
		}
	}
	if (best < 0)
		return clampi(guess, 0, nn - 1);
	off = a - otop;			/* keep the anchor at the same row */
	return clampi(best - off, 0, nn - 1);
}

/* ================================================================== */
/* interactive pager                                                  */
/* ================================================================== */

static volatile sig_atomic_t g_winch = 1;	/* 1 => size unknown */
static struct termios	     g_saved;
static int		     g_raw = 0, g_tty = -1;

static void
on_winch(int sig)
{
	(void)sig;
	g_winch = 1;
}

static void
raw_restore(void)
{
	if (g_raw) {
		tcsetattr(g_tty, TCSAFLUSH, &g_saved);
		g_raw = 0;
	}
	(void)write(STDOUT_FILENO, "\033[?1049l\033[?25h", 14);
}

static int
get_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col < 1)
		return -1;
	*rows = ws.ws_row > 0 ? ws.ws_row : 24;
	*cols = ws.ws_col;
	return 0;
}

static void
draw(struct line *lines, int nlines, int top, int rows, const char *name)
{
	struct sb out = {0};
	char	  status[256];
	int	  r, content = rows - 1, bot;

	sb_add(&out, "\033[H", 3);
	for (r = 0; r < content; r++) {
		int idx = top + r;

		if (idx < nlines)
			sb_add(&out, lines[idx].display,
			    strlen(lines[idx].display));
		sb_add(&out, "\033[K\r\n", 5);
	}
	bot = nlines > content ? top + content : nlines;
	snprintf(status, sizeof(status),
	    "\033[7m %s  %d-%d/%d (%d%%)  [space/b j/k g/G q] \033[0m\033[K",
	    name, nlines ? top + 1 : 0, bot, nlines,
	    nlines ? (int)((long)bot * 100 / nlines) : 100);
	sb_add(&out, status, strlen(status));
	(void)write(STDOUT_FILENO, out.b, out.len);
	free(out.b);
}

static void
run_pager(const char *name)
{
	struct line	*lines = NULL;
	int		 nlines = 0, top = 0, rows = 24, cols = 80;
	struct termios	 t;
	struct sigaction sa;

	/*
	 * Read keys from the terminal. In this design stdin is not the roff
	 * pipe (the dump is captured over its own pipe), so when stdin is a tty
	 * it is the controlling terminal — and on Windows/cosmo console input
	 * arrives on stdin, where /dev/tty does not. Prefer stdin; fall back to
	 * /dev/tty only when stdin was redirected (Unix).
	 */
	if (isatty(STDIN_FILENO))
		g_tty = STDIN_FILENO;
	else if ((g_tty = open("/dev/tty", O_RDWR)) == -1)
		g_tty = STDIN_FILENO;
	if (tcgetattr(g_tty, &g_saved) == 0) {
		t = g_saved;
		t.c_lflag &= ~(ICANON | ECHO);
		t.c_iflag &= ~(IXON | ICRNL);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(g_tty, TCSAFLUSH, &t);
		g_raw = 1;
	}
	atexit(raw_restore);

	/* No SA_RESTART: SIGWINCH must EINTR the blocked read so the loop
	 * re-renders; signal(3)'s glibc default would auto-restart it. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGWINCH, &sa, NULL);

	(void)write(STDOUT_FILENO, "\033[?1049h\033[?25l", 14);	/* alt screen */

	for (;;) {
		unsigned char c;
		int	      step;
		ssize_t	      rd;

		if (g_winch) {
			int	     ncols, nrows, ntop, nn = 0;
			struct line *nl;
			char	    *nb;
			size_t	     nl_len;

			g_winch = 0;
			if (get_size(&nrows, &ncols) == -1) {
				nrows = 24;
				ncols = 80;
			}
			/* Leave a one-column right gutter so filled lines do
			 * not sit flush against the edge (matches man(1)'s
			 * ws_col - 1) and avoid tripping auto-margin wrap. */
			nb = doc_render(ncols > 2 ? ncols - 1 : ncols, &nl_len);
			nl = split_lines(nb, nl_len, &nn);
			free(nb);
			ntop = lines ? find_anchor(lines, nlines, top, nl, nn)
				     : 0;
			if (lines)
				free_lines(lines, nlines);
			lines = nl;
			nlines = nn;
			rows = nrows;
			cols = ncols;
			top = ntop;
			draw(lines, nlines, top, rows, name);
			continue;
		}

		rd = read(g_tty, &c, 1);
		if (rd <= 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		step = 0;
		if (c == '\033') {		/* arrow / page keys */
			unsigned char seq[2];

			if (read(g_tty, &seq[0], 1) == 1 &&
			    read(g_tty, &seq[1], 1) == 1 && seq[0] == '[') {
				if (seq[1] == 'A')
					step = -1;
				else if (seq[1] == 'B')
					step = 1;
				else if (seq[1] == '6')
					step = rows - 2;
				else if (seq[1] == '5')
					step = -(rows - 2);
			}
		} else switch (c) {
		case 'q':
			goto done;
		case ' ':
		case 'f':
			step = rows - 2;
			break;
		case 'b':
			step = -(rows - 2);
			break;
		case 'j':
		case '\r':
		case '\n':
			step = 1;
			break;
		case 'k':
			step = -1;
			break;
		case 'd':
			step = (rows - 1) / 2;
			break;
		case 'u':
			step = -((rows - 1) / 2);
			break;
		case 'g':
			top = 0;
			draw(lines, nlines, top, rows, name);
			continue;
		case 'G':
			top = clampi(nlines - (rows - 1), 0, nlines);
			draw(lines, nlines, top, rows, name);
			continue;
		default:
			continue;
		}

		if (step != 0) {
			top = clampi(top + step, 0, nlines - (rows - 1));
			draw(lines, nlines, top, rows, name);
		}
	}
done:
	(void)cols;
	if (lines)
		free_lines(lines, nlines);
}

/* stdout is not a tty: emit one plain, styling-free render (greppable). */
static void
one_shot(void)
{
	char	    *buf;
	size_t	     len;
	struct line *lines;
	int	     n, i, rows, cols = 0;

	if (get_size(&rows, &cols) == -1)
		cols = 0;		/* 0 => mandoc default width */
	buf = doc_render(cols, &len);
	lines = split_lines(buf, len, &n);
	for (i = 0; i < n; i++)
		puts(lines[i].plain);
	free(buf);
	free_lines(lines, n);
}

int
main(int argc, char *argv[])
{
	const char	*pkg, *page, *listv[5], *dumpv[6];
	char		*listing, *roff, *entry_path, *line, *saveptr;
	struct entry	*ents;
	size_t		 nents, cap, rofflen;

	/* No package given (`unpin man`) defaults to unpin's own manual. */
	pkg = argc > 1 ? argv[1] : "unpin";
	page = argc > 2 ? argv[2] : pkg;	/* default page = pkg name */

	setprogname("man");
	if ((self = getenv("UNPIN_SELF")) == NULL || self[0] == '\0')
		self = "unpin";

	/* 1. enumerate the package's embedded pages. */
	listv[0] = self;
	listv[1] = "bundle";
	listv[2] = "list";
	listv[3] = pkg;
	listv[4] = NULL;
	listing = capture(listv, NULL);

	cap = 16;
	nents = 0;
	if ((ents = malloc(cap * sizeof(*ents))) == NULL)
		die("out of memory");
	for (line = strtok_r(listing, "\n", &saveptr); line != NULL;
	    line = strtok_r(NULL, "\n", &saveptr)) {
		struct entry e;

		if (!parse_line(line, &e))
			continue;
		if (nents == cap) {
			cap *= 2;
			if ((ents = realloc(ents, cap * sizeof(*ents))) == NULL)
				die("out of memory");
		}
		ents[nents++] = e;
	}
	if (nents == 0)
		die("`%s` has no embedded manual", pkg);

	/* 2. pick + follow `.so` -> the roff entry to render. */
	entry_path = resolve(ents, nents, page, "en");

	/* 3. slurp that entry's roff and parse it once. */
	dumpv[0] = self;
	dumpv[1] = "bundle";
	dumpv[2] = "dump";
	dumpv[3] = pkg;
	dumpv[4] = entry_path;
	dumpv[5] = NULL;
	roff = capture(dumpv, &rofflen);
	parse_roff(roff, rofflen);

	/* 4. page it on a tty, or emit one plain render when redirected. */
	if (isatty(STDOUT_FILENO))
		run_pager(page);
	else
		one_shot();
	return 0;
}
