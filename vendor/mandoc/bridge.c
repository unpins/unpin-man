/*
 * bridge.c — the in-process entry point the Rust crate calls (replaces
 * mandoc's `main`). Given roff bytes and a width, it parses with mandoc's own
 * parser and runs the terminal formatter, returning the rendered ANSI/UTF-8.
 *
 * Everything happens in memory — no temp files, no file descriptors. Input goes
 * through `mparse_readmem` (added to read.c) instead of an fd; output is
 * captured because term_ascii.c was patched to append each byte to a growable
 * buffer on the `termp` (`obuf`/`olen`) instead of writing to a FILE. This
 * sidesteps the whole class of platform I/O quirks (e.g. msvcrt's `tmpfile()`).
 *
 * It parses fresh on every call: manual pages are small, and the pager only
 * re-renders on a resize, so a re-parse per render is cheap and keeps the FFI
 * surface to a single function.
 */
#include "config.h"

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "mandoc.h"
#include "roff.h"		/* enum mandoc_os, struct roff_meta */
#include "mandoc_parse.h"
#include "mandoc_aux.h"
#include "out.h"		/* struct rofftbl, completes struct termp's `tbl` */
#include "manconf.h"
#include "term.h"
#include "main.h"

/*
 * term_tag.c is dropped from the build (it needs POSIX signals / mkstemps that
 * a render-only, cross-platform build neither has nor wants). The terminal
 * formatters call only term_tag_write — stub it.
 */
void
term_tag_write(struct roff_node *n, size_t line)
{
	(void)n;
	(void)line;
}

/*
 * Render `roff`/`rofflen` to a malloc'd UTF-8 buffer at `width` columns
 * (<= 0 ⇒ mandoc's default). On success returns the buffer and sets *outlen to
 * its byte length (sans the trailing NUL); returns NULL on a parse failure. The
 * caller frees the result with unpin_man_free_str.
 */
char *
unpin_man_render(const char *roff, size_t rofflen, int width, size_t *outlen)
{
	struct mparse	 *mp;
	struct roff_meta *meta;
	struct manoutput  out;
	struct termp	 *p;
	char		 *result;

	if (outlen != NULL)
		*outlen = 0;

	mchars_alloc();
	mp = mparse_alloc(MPARSE_SO | MPARSE_UTF8 | MPARSE_VALIDATE,
	    MANDOC_OS_OTHER, NULL);
	if (mp == NULL) {
		mchars_free();
		return NULL;
	}

	mparse_readmem(mp, roff, rofflen, "man");
	meta = mparse_result(mp);
	if (meta == NULL || meta->macroset == MACROSET_NONE) {
		mparse_free(mp);
		mchars_free();
		return NULL;
	}

	memset(&out, 0, sizeof(out));
	out.width = width > 0 ? (size_t)width : 0;
	p = utf8_alloc(&out);		/* p->obuf starts empty (calloc'd) */

	if (meta->macroset == MACROSET_MDOC)
		terminal_mdoc(p, meta);
	else
		terminal_man(p, meta);

	/* Hand the in-memory render buffer straight to the caller. term_membuf
	 * always keeps room for a trailing NUL, so obuf[olen] is in bounds. */
	if (p->obuf != NULL) {
		result = p->obuf;
		result[p->olen] = '\0';
		if (outlen != NULL)
			*outlen = p->olen;
		p->obuf = NULL;		/* transfer ownership before term_free */
	} else {
		result = mandoc_malloc(1);	/* empty render → "" */
		result[0] = '\0';
	}

	ascii_free(p);
	mparse_free(mp);
	mchars_free();
	return result;
}

/* Free a buffer returned by unpin_man_render. */
void
unpin_man_free_str(char *s)
{
	free(s);
}
