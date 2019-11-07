struct grid {
	int direction, padding;
	int *widths, widths_len;
	int *columns, x, y;
};

bool grid_layout(struct grid *g, int direction, int padding, int term_width, int max_width, int *widths, int widths_len);
bool grid_index(struct grid *g, int x, int y, int *i, int *p);
