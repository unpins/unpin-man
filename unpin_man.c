/*
 * unpin_man.c — front-end that turns mandoc into the `unpin man` renderer.
 *
 * unpin knows nothing about man: it only exposes a binary's embedded metadata
 * through `unpin bundle list|dump`. This front-end is the other half — a
 * patched mandoc (the `man` package) that, given `man [pkg] [page]` (a missing
 * pkg defaults to `unpin`, so a bare `unpin man` shows unpin's own manual):
 *
 *   1. runs `$UNPIN_SELF bundle list <pkg>` to enumerate the embedded
 *      embedded `unpin/man/` pages, picking the right (name, section, lang) and
 *      following whole-page `.so` redirects (shown by `bundle list` as
 *      symlink targets) — the port of unpin's old src/man.rs pick/roff_for;
 *   2. runs `$UNPIN_SELF bundle dump <pkg> <entry>` and feeds that roff
 *      straight into mandoc's renderer over a pipe (no temp files).
 *
 * The hard container/ZIP/EOCD logic stays in unpin's tested Rust; here we only
 * shell back to it and reuse all of mandoc's parsing/terminal output unchanged
 * by handing the dumped roff to mandoc on stdin and calling the original
 * mandoc entry point (renamed `mandoc_main` by unpin-front-end.patch) with a
 * synthetic `mandoc` argv, which makes it read stdin in file mode.
 *
 * $UNPIN_SELF is the absolute path of the unpin binary that launched us (set by
 * unpin's `run`); we fall back to `unpin` on $PATH when it is unset, so the
 * package is still usable when invoked by hand.
 */
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern int mandoc_main(int, char **);

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
 * NUL-terminated buffer. A non-zero exit is a real failure (PKG not installed,
 * unreadable binary, corrupt bundle) and aborts — matching the bundle family's
 * "absence is not an error, but a broken read is" contract.
 */
static char *
capture(const char *const argv[])
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

int
main(int argc, char *argv[])
{
	const char	*pkg, *page, *listv[5];
	char		*listing, *entry_path, *line, *saveptr;
	struct entry	*ents;
	size_t		 nents, cap;
	int		 fds[2];
	pid_t		 pid;
	int		 st;
	char		*margv[2];

	/* No package given (`unpin man`) defaults to unpin's own manual. */
	pkg = argc > 1 ? argv[1] : "unpin";
	page = argc > 2 ? argv[2] : pkg;	/* default page = pkg name */

	if ((self = getenv("UNPIN_SELF")) == NULL || self[0] == '\0')
		self = "unpin";

	/* 1. enumerate the package's embedded pages. */
	listv[0] = self;
	listv[1] = "bundle";
	listv[2] = "list";
	listv[3] = pkg;
	listv[4] = NULL;
	listing = capture(listv);

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

	/* 3. stream that entry's roff into mandoc on stdin. */
	if (pipe(fds) == -1)
		die("pipe: %s", strerror(errno));
	if ((pid = fork()) == -1)
		die("fork: %s", strerror(errno));
	if (pid == 0) {
		const char *dumpv[6];

		if (dup2(fds[1], STDOUT_FILENO) == -1)
			_exit(126);
		close(fds[0]);
		close(fds[1]);
		dumpv[0] = self;
		dumpv[1] = "bundle";
		dumpv[2] = "dump";
		dumpv[3] = pkg;
		dumpv[4] = entry_path;
		dumpv[5] = NULL;
		execvp(self, (char *const *)dumpv);
		_exit(127);
	}
	close(fds[1]);
	if (dup2(fds[0], STDIN_FILENO) == -1)
		die("dup2: %s", strerror(errno));
	close(fds[0]);

	/*
	 * Render with the original mandoc, forced into file mode (read stdin)
	 * by a `mandoc` argv[0]; setprogname covers HAVE_PROGNAME builds where
	 * the arg mode is taken from getprogname() rather than argv[0].
	 */
	setprogname("mandoc");
	margv[0] = (char *)"mandoc";
	margv[1] = NULL;
	st = mandoc_main(1, margv);

	while (waitpid(pid, &(int){0}, 0) == -1 && errno == EINTR)
		;
	return st;
}
