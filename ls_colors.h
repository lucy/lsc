enum labels {
	L_LEFT, L_RIGHT, L_END, L_RESET, L_NORM, L_FILE, L_DIR, L_LINK, L_FIFO,
	L_SOCK, L_BLK, L_CHR, L_MISSING, L_ORPHAN, L_EXEC, L_DOOR, L_SETUID,
	L_SETGID, L_STICKY, L_OW, L_STICKYOW, L_CAP, L_MULTIHARDLINK, L_CLR_TO_EOL,
	L_LENGTH,
};

struct ext_pair {
	const char *ext;
	const char *color;
};

struct ls_colors {
	char *labels[L_LENGTH];
	struct ext_pair *ext_map;
	size_t exts;
};

const char *ls_colors_lookup(struct ls_colors *lsc, const char *ext);
void ls_colors_parse(struct ls_colors *lsc, char *lsc_env);
