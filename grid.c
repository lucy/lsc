#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grid.h"
#include "util.h"

bool grid_index(struct grid *g, int x, int y, int *i, int *p) {
	*i = g->direction ? y * g->x + x : g->y * x + y;
	if (*i >= g->widths_len) return false;
	*p = g->columns[x] - g->widths[*i] + g->padding;
	return true;
}

bool grid_layout(struct grid *g, int direction, int padding, int term_width,
	int max_width, int *widths, int widths_len)
{
	*g = (struct grid) { direction, padding, widths, widths_len, 0, 0, 0 };
	int *cols = 0;
	// iterate through numbers of rows, starting at upper bound
	int n = (term_width - max_width) / (padding + max_width) + 1;
	int upper_bound = (widths_len + n - 1) / n + 1;
	for (int r = upper_bound; r >= 1; r--) {
		// calculate number of columns for rows
		int c = (widths_len + r - 1) / r;
		// skip uninteresting rows
		r = ((widths_len + c - 1) / c);
		// total padding between columns
		int total_separator_width = (c - 1) * padding;
		// find maximum width in each column
		cols = xreallocr(cols, c, sizeof(*cols));
		memset(cols, 0, c * sizeof(*cols));
		for (int i = 0; i < widths_len; i++) {
			int ci = direction ? i % c : i / r;
			cols[ci] = MAX(cols[ci], widths[i]);
		}
		// calculate total width of columns
		int total = 0;
		for (int i = 0; i < c; i++) total += cols[i];
		// check if columns fit
		if (total > term_width - total_separator_width)
			break;
		// store last layout that fits
		int *tmp = g->columns;
		g->columns = cols, cols = tmp;
		g->x = c;
		g->y = r;
	}
	if (cols) free(cols);
	return !!g->columns;
}
