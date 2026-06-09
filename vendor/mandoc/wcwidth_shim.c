/*
 * TEST shim: mingw-w64 has no wcwidth(). This is Markus Kuhn's public-domain
 * mk_wcwidth (https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c), trimmed.
 *
 * Signature is int (codepoint), NOT wchar_t: on Windows wchar_t is 16-bit
 * (UTF-16), so a wchar_t-based wcwidth couldn't represent astral codepoints
 * mandoc passes. Taking an int sidesteps that entirely.
 */
#include "config.h"

struct interval { int first, last; };

static int
bisearch(int ucs, const struct interval *t, int max)
{
	int min = 0, mid;

	if (ucs < t[0].first || ucs > t[max].last)
		return 0;
	while (max >= min) {
		mid = (min + max) / 2;
		if (ucs > t[mid].last)
			min = mid + 1;
		else if (ucs < t[mid].first)
			max = mid - 1;
		else
			return 1;
	}
	return 0;
}

int
wcwidth(int ucs)
{
	/* Sorted list of non-overlapping zero-width (combining) intervals. */
	static const struct interval combining[] = {
		{ 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
		{ 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 },
		{ 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 }, { 0x06EA, 0x06ED },
		{ 0x0711, 0x0711 }, { 0x0730, 0x074A }, { 0x07A6, 0x07B0 },
		{ 0x0901, 0x0903 }, { 0x093C, 0x093C }, { 0x0941, 0x0948 },
		{ 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0EB1, 0x0EB1 },
		{ 0x1DC0, 0x1DFF }, { 0x20D0, 0x20F0 }, { 0xFE20, 0xFE26 },
	};

	if (ucs == 0)
		return 0;
	if (ucs < 32 || (ucs >= 0x7f && ucs < 0xa0))
		return -1;

	if (bisearch(ucs, combining,
	    (int)(sizeof(combining) / sizeof(struct interval) - 1)))
		return 0;

	/* Wide (double-width) ranges: CJK, Hangul, fullwidth forms, emoji. */
	return 1 +
	    ((ucs >= 0x1100 && ucs <= 0x115F) ||	/* Hangul Jamo */
	     ucs == 0x2329 || ucs == 0x232A ||
	     (ucs >= 0x2E80 && ucs <= 0xA4CF && ucs != 0x303F) ||  /* CJK..Yi */
	     (ucs >= 0xAC00 && ucs <= 0xD7A3) ||	/* Hangul Syllables */
	     (ucs >= 0xF900 && ucs <= 0xFAFF) ||	/* CJK Compat */
	     (ucs >= 0xFE30 && ucs <= 0xFE4F) ||	/* CJK Compat Forms */
	     (ucs >= 0xFF00 && ucs <= 0xFF60) ||	/* Fullwidth Forms */
	     (ucs >= 0xFFE0 && ucs <= 0xFFE6) ||
	     (ucs >= 0x1F300 && ucs <= 0x1FAFF) ||	/* emoji/symbols */
	     (ucs >= 0x20000 && ucs <= 0x3FFFD));	/* CJK Ext B+ */
}
