#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filevercmp.h"
#include "util.h"

static int order(char c) {
	if (ls_isalpha(c)) return c;
	if (ls_isdigit(c)) return 0;
	if (c == '~') return -1;
	return (int)c + 256;
}

static int verrevcmp(const char *a, const char *b, size_t al, size_t bl) {
	size_t ai = 0, bi = 0;
	while (ai < al || bi < bl) {
		int first_diff = 0;
		// XXX: heap overflow stuff with afl/asan
		while ((ai < al && !ls_isdigit(a[ai])) || (bi < bl && !ls_isdigit(b[bi]))) {
			int ac = (ai == al) ? 0 : order(a[ai]);
			int bc = (bi == bl) ? 0 : order(b[bi]);
			if (ac != bc) return ac - bc;
			ai++; bi++;
		}
		while (a[ai] == '0') ai++;
		while (b[bi] == '0') bi++;
		while (ls_isdigit(a[ai]) && ls_isdigit(b[bi])) {
			if (!first_diff) first_diff = a[ai] - b[bi];
			ai++; bi++;
		}
		if (ls_isdigit(a[ai])) return 1;
		if (ls_isdigit(b[bi])) return -1;
		if (first_diff) return first_diff;
	}
	return 0;
}

// read file extension
// ^\.?.*?(\.[A-Za-z~][A-Za-z0-9~])*$
size_t suf_index(const char *s, size_t len) {
	if (len != 0 && s[0] == '.') { s++; len--; }
	bool alpha = false;
	size_t match = 0;
	for (size_t j = 0; j < len; j++) {
		char c = s[len - j - 1];
		if (ls_isalpha(c) || c == '~')
			alpha = true;
		else if (alpha && c == '.')
			match = j + 1;
		else if (ls_isdigit(c))
			alpha = false;
		else
			break;
	}
	return len - match;
}


int filevercmp(const char *a, const char *b, size_t al, size_t bl, size_t ai, size_t bi) {
	if (!al || !bl) return !al - !bl;
	int s = strcmp(a, b);
	if (!s) return 0;
	if (a[0] == '.' && b[0] != '.') return -1;
	if (a[0] != '.' && b[0] == '.') return 1;
	if (a[0] == '.' && b[0] == '.') a++, al--, b++, bl--;
	if (ai == bi && !strncmp(a, b, ai)) {
		a += ai; ai = al - ai;
		b += bi; bi = bl - bi;
	}
	int r = verrevcmp(a, b, ai, bi);
	return r ? r : s;
}
