#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define program_name "lsc"

#include "config.h"
#include "filevercmp.h"
#include "grid.h"
#include "id.h"
#include "ls_colors.h"
#include "util.h"
#include "vec.h"

enum sort_type { SORT_FVER, SORT_SIZE, SORT_TIME };
enum uinfo_type { UINFO_NEVER, UINFO_AUTO, UINFO_ALWAYS };
enum date_type { DATE_NONE, DATE_REL, DATE_ABS };
enum layout_type { LAYOUT_GRID_COLUMNS, LAYOUT_GRID_LINES, LAYOUT_1LINE };

static struct {
	bool all;
	bool dir;
	bool c_time;
	// sorting
	bool group_dir;
	bool reverse;
	enum sort_type sort;
	// data/formatting
	enum layout_type layout;
	bool follow_links;
	bool strmode;
	enum uinfo_type userinfo;
	enum date_type date;
	bool size;
	bool classify;
} options;

static id_t myuid, mygid;

static struct ls_colors ls_colors;

typedef struct ctx {
	int nwidth, uwidth, gwidth;
	bool userinfo;
} ctx;

// big :(
typedef struct file_info {
	const char *name, *linkname;
	mode_t mode, linkmode;
	id_t uid, gid;
	time_t time;
	off_t size;
	uint16_t name_len, linkname_len;
	uint16_t uwidth, gwidth, nwidth;
	uint16_t name_suf;
	bool linkok;
} file_info;

static void fi_free(file_info *fi) {
	if (fi->name) free((void *)fi->name);
	if (fi->linkname) free((void *)fi->linkname);
}

#define fi_isdir(fi) (S_ISDIR((fi)->mode) || S_ISDIR((fi)->linkmode))

static inline int fi_cmp(const void *va, const void *vb) {
	file_info *a = (file_info *const)va;
	file_info *b = (file_info *const)vb;
	int rev = options.reverse ? -1 : 1;
	if (options.group_dir)
		if (fi_isdir(a) != fi_isdir(b))
			return fi_isdir(a) ? -1 : 1;
	if (options.sort == SORT_SIZE) {
		off_t s = a->size - b->size;
		if (s) return rev * ((s > 0) - (s < 0));
	}
	if (options.sort == SORT_TIME) {
		time_t t = a->time - b->time;
		if (t) return rev * ((t > 0) - (t < 0));
	}
	return rev * filevercmp(a->name, b->name,
		a->name_len, b->name_len, a->name_suf, b->name_suf);
}

// file info vector
VEC(fv, file_vec, file_info)

// read symlink target
static const char *ls_readlink(int dirfd, const char *name, size_t size) {
	char *buf = xmallocr(size + 1, 1); // allocate length + \0
	ssize_t n = readlinkat(dirfd, name, buf, size);
	if (n == -1) {
		free(buf);
		return 0;
	}
	assertx((size_t)n == size); // possible truncation
	buf[n] = '\0';
	return buf;
}

// populates file_info with file information
static int ls_stat(ctx *c, int dirfd, char *name, file_info *fi) {
	fi->name = name;
	fi->name_len = strlen(name);
	fi->name_suf = suf_index(name, fi->name_len);
	fi->linkname = 0;
	fi->linkname_len = 0;
	fi->linkmode = 0;
	fi->linkok = true;
	fi->mode = 0;
	struct stat st;
	if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == -1)
		return -1;
	fi->mode = st.st_mode;
	fi->time = options.c_time ? st.st_ctime : st.st_mtime;
	fi->size = st.st_size;
	fi->uid = st.st_uid;
	fi->gid = st.st_gid;
	if (options.userinfo == UINFO_AUTO)
		c->userinfo |= st.st_uid != myuid || st.st_gid != mygid;
	if (S_ISLNK(fi->mode)) {
		const char *ln = ls_readlink(dirfd, name, st.st_size);
		if (!ln) { fi->linkok = false; return 0; }
		fi->linkname = ln;
		fi->linkname_len = (size_t)st.st_size;
		if (fstatat(dirfd, name, &st, 0) == -1) {
			fi->linkok = false;
			return 0;
		}
		fi->linkmode = st.st_mode;
	}
	return 0;
}

// list directory
static int ls_readdir(ctx *c, file_vec *v, const char *name) {
	DIR *dir = opendir(name);
	if (!dir) {
		warn_errno("cannot open directory '%s'", name);
		return -1;
	}
	int fd = dirfd(dir);
	if (fd == -1) {
		warn_errno("%s", name);
		return -1;
	}
	struct dirent *dent;
	int err = 0;
	while ((dent = readdir(dir))) {
		const char *p = dent->d_name;
		if (p[0] == '.' && !options.all) continue;
		if (p[0] == '.' && p[1] == '\0') continue;
		if (p[0] == '.' && p[1] == '.' && p[2] == '\0') continue;
		struct file_info *out = fv_stage(v);
		char *dup = strdup(p);
		if (ls_stat(c, fd, dup, out) == -1) {
			free(dup);
			err = -1;
			warn_errno("cannot access '%s/%s'", name, p);
			continue;
		}
		fv_commit(v);
	}
	if (closedir(dir) == -1)
		return -1;
	return err;
}

// list file/directory
static int ls(ctx *c, file_vec *v, const char *name) {
	file_info *fi = fv_stage(v); // new uninitialized file_info
	char *dup = strdup(name);
	if (ls_stat(c, AT_FDCWD, dup, fi) == -1) {
		free(dup);
		warn_errno("cannot access '%s'", name);
		return -1;
	}
	if (!options.dir && fi_isdir(fi)) {
		free(dup);
		return ls_readdir(c, v, name);
	}
	fv_commit(v);
	return 0;
}

//
// Formatting
//

static void fmt_strmode(FILE *out, const mode_t mode) {
	switch (mode&S_IFMT) {
	case S_IFREG:  fputs(C_FILE,    out); break;
	case S_IFDIR:  fputs(C_DIR,     out); break;
	case S_IFCHR:  fputs(C_CHAR,    out); break;
	case S_IFBLK:  fputs(C_BLOCK,   out); break;
	case S_IFIFO:  fputs(C_FIFO,    out); break;
	case S_IFLNK:  fputs(C_LINK,    out); break;
	case S_IFSOCK: fputs(C_SOCK,    out); break;
	default:       fputs(C_UNKNOWN, out); break;
	}
	fputs(mode&S_IRUSR ? C_READ : C_NONE, out);
	fputs(mode&S_IWUSR ? C_WRITE : C_NONE, out);
	fputs(mode&S_ISUID ? mode&S_IXUSR ? C_UID_EXEC : C_UID
	                   : mode&S_IXUSR ? C_EXEC : C_NONE, out);
	fputs(mode&S_IRGRP ? C_READ : C_NONE, out);
	fputs(mode&S_IWGRP ? C_WRITE : C_NONE, out);
	fputs(mode&S_ISGID ? mode&S_IXGRP ? C_UID_EXEC : C_UID
	                   : mode&S_IXGRP ? C_EXEC : C_NONE, out);
	fputs(mode&S_IROTH ? C_READ : C_NONE, out);
	fputs(mode&S_IWOTH ? C_WRITE : C_NONE, out);
	fputs(mode&S_ISVTX ? mode&S_IXOTH ? C_STICKY : C_STICKY_O
	                   : mode&S_IXOTH ? C_EXEC : C_NONE, out);
	putc(' ', out);
}

#define SECOND 1
#define MINUTE (60 * SECOND)
#define HOUR   (60 * MINUTE)
#define DAY    (24 * HOUR)
#define WEEK   (7  * DAY)
#define MONTH  (30 * DAY)
#define YEAR   (12 * MONTH)

static time_t current_time(void) {
	struct timespec t;
	if (clock_gettime(CLOCK_REALTIME, &t) == -1)
		die_errno("%s", "current_time");
	return t.tv_sec;
}

static void fmt_abstime(FILE *out, const time_t now, const time_t then) {
	time_t diff = now - then;
	char buf[20];
	struct tm tm;
	localtime_r(&then, &tm);
	char *fmt = diff < MONTH * 6 ? "%e %b %H:%M" : "%e %b  %Y";
	strftime(buf, sizeof(buf), fmt, &tm);
	fputs(C_DAY, out);
	fputs(buf, out);
	putc(' ', out);
}

static void fmt3(char b[static 3], uint16_t x) {
	if (x/100) b[0] = '0' + x/100;
	if (x/100||x/10%10) b[1] = '0' + x/10%10;
	b[2] = '0' + x%10;
}

static void fmt_reltime(FILE *out, const time_t now, const time_t then) {
	time_t diff = now - then;
	if (diff < 0) {
		fputs(C_SECOND " 0s " C_END, out);
		return;
	}
	if (diff <= SECOND) {
		fputs(C_SECOND "<1s " C_END, out);
		return;
	}
	char b[4] = "  0s";
	if (diff < MINUTE) {
		fputs(C_SECOND, out);
	} else if (diff < HOUR) {
		fputs(C_MINUTE, out);
		diff /= MINUTE;
		b[3] = 'm';
	} else if (diff < HOUR*36) {
		fputs(C_HOUR, out);
		diff /= HOUR;
		b[3] = 'h';
	} else if (diff < MONTH) {
		fputs(C_DAY, out);
		diff /= DAY;
		b[3] = 'd';
	} else if (diff < YEAR) {
		fputs(C_WEEK, out);
		diff /= WEEK;
		b[3] = 'w';
	} else {
		fputs(C_YEAR, out);
		diff /= YEAR;
		b[3] = 'y';
	}
	fmt3(b, diff);
	fwrite(b+1, 1, 3, out);
	putc(' ', out);
}

static off_t divide(off_t x, off_t d) { return (x+((d-1)/2))/d; }

static void fmt_size(FILE *out, off_t sz) {
	fputs(C_SIZE, out);
	unsigned m = 0;
	off_t div = 1;
	off_t u = sz;
	while (u > 999) {
		div *= 1024;
		u = divide(u, 1024);
		m++;
	}
	off_t v = divide(sz*10, div);
	char b[3] = "  0";
	if (v/10 >= 10 || m == 0) {
		fmt3(b, u);
	} else if (sz != 0) {
		b[0] = '0' + v/10;
		b[1] = '.';
		b[2] = '0' + v%10;
	}
	fwrite(b, 1, 3, out);
	fputs(C_SIZES[m], out);
	putc(' ', out);
}

static int color_type(mode_t mode) {
	#define S_IXUGO (S_IXUSR|S_IXGRP|S_IXOTH)
	switch (mode&S_IFMT) {
	case S_IFREG:
		if (mode&S_ISUID) return L_SETUID;
		if (mode&S_ISGID) return L_SETGID;
		if (mode&S_IXUGO) return L_EXEC;
		return L_FILE;
	case S_IFDIR:
		if (mode&S_ISVTX && mode&S_IWOTH) return L_STICKYOW;
		if (mode&S_IWOTH) return L_OW;
		if (mode&S_ISVTX) return L_STICKY;
		return L_DIR;
	case S_IFLNK: return L_LINK;
	case S_IFIFO: return L_FIFO;
	case S_IFSOCK: return L_SOCK;
	case S_IFCHR: return L_CHR;
	case S_IFBLK: return L_BLK;
	default: return L_ORPHAN;
	}
}

static const char *suf_color(const char *name, size_t len) {
	while (len--) if (name[len] == '.')
		return ls_colors_lookup(&ls_colors, name + len);
	return 0;
}

static const char *file_color(const char *name, size_t len, int t) {
	if (t == L_FILE || t == L_LINK) {
		const char *c = suf_color(name, len);
		if (c) return c;
	}
	return ls_colors.labels[t];
}

static int strwidth(const char *s) {
	mbstate_t st = {0};
	wchar_t wc;
	int len = strlen(s), w = 0, i = 0;
	while (i < len) {
		int c = (unsigned char)s[i];
		if (c <= 0x7f) { // ascii fast path
			if (0x7f > c && c > 0x1f) { w++, i++; }
			continue;
		}
		int r = mbrtowc(&wc, s+i, len-i, &st);
		if (r < 0) break;
		w += wcwidth(wc);
		i += r;
	}
	return w;
}

static int fmt_name_width(const struct file_info *fi) {
	int w = strwidth(fi->name);
	mode_t m = fi->mode;
	if (options.follow_links && fi->linkname) {
		w += 1 + strlen(C_SYM_DELIM) + strwidth(fi->linkname);
		m = fi->linkmode;
	}
	if (options.classify)
		switch (color_type(m)) {
		case L_EXEC: case L_DIR: case L_OW: case L_STICKY: case L_STICKYOW:
		case L_LINK: case L_FIFO: case L_SOCK:
			w++;
		}
	return w;
}

static void fmt_name(FILE *out, const struct file_info *fi) {
	int t;
	const char *c;
	if (fi->linkname && options.follow_links) {
		t = fi->linkok ? color_type(fi->linkmode) : L_ORPHAN;
		c = file_color(fi->linkname, fi->linkname_len, t);
	} else {
		t = color_type(fi->mode);
		c = file_color(fi->name, fi->name_len, t);
	}
	fputs(C_ESC, out);
	fputs(c ? c : "0", out);
	fputs("m", out);
	fwrite(fi->name, 1, fi->name_len, out);
	if (c) fputs(C_END, out);
	if (options.follow_links && fi->linkname) {
		fputs(" " C_SYM_DELIM_COLOR C_SYM_DELIM C_ESC, out);
		fputs(c ? c : "0", out);
		fputs("m", out);
		fwrite(fi->linkname, 1, fi->linkname_len, out);
		if (c) fputs(C_END, out);
	}
	if (options.classify)
		switch (t) {
		case L_EXEC: fputs(CL_EXEC, out); break;
		case L_DIR: case L_OW: case L_STICKY: case L_STICKYOW:
		             fputs(CL_DIR, out); break;
		case L_LINK: fputs(CL_LINK, out); break;
		case L_FIFO: fputs(CL_FIFO, out); break;
		case L_SOCK: fputs(CL_SOCK, out); break;
		}
}

static void fmt_usergroup(FILE *out, id_t id, const char *n, int w, int mw) {
	if (n) fputs(n, out);
	else fprintf(out, "%d", id);
	for (int n = mw - w + 1; n--;)
		putc(' ', out);
}

static void fmt_userinfo(ctx *c, FILE *out, struct file_info *fi) {
	fputs(C_USERINFO, out);
	fmt_usergroup(out, fi->uid, getuser(fi->uid), fi->uwidth, c->uwidth);
	fmt_usergroup(out, fi->gid, getgroup(fi->gid), fi->gwidth, c->gwidth);
}

static int fmt_file_width(ctx *c, struct file_info *fi) {
	return
		(options.strmode          ? 10 + 1                          : 0) +
		(c->userinfo              ? fi->uwidth + 1 + fi->gwidth + 1 : 0) +
		(options.date == DATE_ABS ? 12 + 1                          : 0) +
		(options.date == DATE_REL ? 3 + 1                           : 0) +
		(options.size             ? 4 + 1                           : 0) +
		fi->nwidth;
}

static void fmt_file(ctx *c, FILE *out, time_t now, struct file_info *fi) {
	if (options.strmode)
		fmt_strmode(out, fi->mode);
	if (c->userinfo || options.userinfo == UINFO_ALWAYS)
		fmt_userinfo(c, out, fi);
	if (options.date == DATE_ABS)
		fmt_abstime(out, now, fi->time);
	if (options.date == DATE_REL)
		fmt_reltime(out, now, fi->time);
	if (options.size)
		fmt_size(out, fi->size);
	fmt_name(out, fi);
}

static void fmt_file_list(ctx c, FILE *out, time_t now, file_vec v) {
	if (c.userinfo)
		for (size_t i = 0; i < v.len; i++) {
			struct file_info *fi = fv_index(&v, i);
			const char *u = getuser(fi->uid);
			const char *g = getgroup(fi->gid);
			fi->uwidth = u ? strwidth(u) : snprintf(0, 0, "%d", fi->uid);
			fi->gwidth = g ? strwidth(g) : snprintf(0, 0, "%d", fi->gid);
			c.uwidth = MAX(fi->uwidth, c.uwidth);
			c.gwidth = MAX(fi->gwidth, c.gwidth);
		}
	if (options.layout == LAYOUT_1LINE)
		goto oneline;
	int *widths = xmallocr(v.len, sizeof(int)), max_width = 0;
	for (size_t i = 0; i < v.len; i++) {
		struct file_info *fi = fv_index(&v, i);
		fi->nwidth = fmt_name_width(fi);
		widths[i] = fmt_file_width(&c, fi);
		max_width = MAX(max_width, widths[i]);
	}
	struct winsize w;
	int term_width = ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1 ? 80 : w.ws_col;
	if (term_width < max_width) {
		free(widths);
		goto oneline;
	}
	int direction = options.layout == LAYOUT_GRID_LINES, padding = 2;
	struct grid g;
	bool grid = grid_layout(&g, direction, padding, term_width,
		max_width, widths, v.len);
	if (!grid) goto oneline;
	for (int y = 0; y < g.y; y++) {
		for (int x = 0; x < g.x; x++) {
			int i, p;
			if (!grid_index(&g, x, y, &i, &p)) continue;
			struct file_info *fi = fv_index(&v, i);
			fmt_file(&c, out, now, fi);
			while (p--) putc(' ', out);
			fi_free(fi);
		}
		putc('\n', out);
	}
	free(g.columns);
	free(widths);
	return;
oneline:
	for (size_t i = 0; i < v.len; i++) {
		struct file_info *fi = fv_index(&v, i);
		fmt_file(&c, out, now, fi);
		fi_free(fi);
		putc('\n', out);
	}
}

void usage(void) {
	log("Usage: %s [option ...] [file ...]"
		"\n  -a  show all files"
		"\n  -c  use ctime instead of mtime"
		"\n  -G  group directories first"
		"\n  -r  reverse sort"
		"\n  -S  sort by file size"
		"\n  -t  sort by mtime/ctime"
		"\n  -1  list one file per line"
		"\n  -g  show output in grid, by columns (default)"
		"\n  -x  show output in grid, by lines"
		"\n  -m  print file modes"
		"\n  -u  print user and group info (automatic)"
		"\n  -U  print user and group info (always)"
		"\n  -d  print relative modification time"
		"\n  -D  print absolute modification time"
		"\n  -z  print file size"
		"\n  -y  print symlink target"
		"\n  -F  print type indicator"
		"\n  -?  show this help"
		, program_name);
}

int main(int argc, char **argv) {
	setlocale(LC_ALL, "");
	int c;
	while ((c = getopt(argc, argv, "aicGrSt1gxmdDuUzFy?")) != -1)
		switch (c) {
		case 'a': options.all = true; break;
		case 'i': options.dir = true; break;
		case 'c': options.c_time = true; break;
		case 'G': options.group_dir = true; break;
		case 'S': options.sort = SORT_SIZE; break;
		case 't': options.sort = SORT_TIME; break;
		case 'r': options.reverse = true; break;
		case '1': options.layout = LAYOUT_1LINE; break;
		case 'g': options.layout = LAYOUT_GRID_COLUMNS; break;
		case 'x': options.layout = LAYOUT_GRID_LINES; break;
		case 'm': options.strmode = true; break;
		case 'd': options.date = DATE_REL; break;
		case 'D': options.date = DATE_ABS; break;
		case 'u': options.userinfo = UINFO_AUTO; break;
		case 'U': options.userinfo = UINFO_ALWAYS; break;
		case 'z': options.size = true; break;
		case 'F': options.classify = true; break;
		case 'y': options.follow_links = true; break;
		case '?': usage(); exit(EXIT_SUCCESS); break;
		default: exit(EXIT_FAILURE); break;
		}
	myuid = getuid(), mygid = getgid();
	ls_colors_parse(&ls_colors, getenv("LS_COLORS"));
	file_vec v;
	fv_init(&v, 64);
	time_t now = current_time();
	if (optind >= argc) argv[--optind] = ".";
	int err = 0, arg_num = argc - optind;
	for (int i = 0; i < arg_num; i++) {
		ctx c = {0};
		c.userinfo = options.userinfo == UINFO_ALWAYS;
		char *path = argv[optind + i];
		err |= ls(&c, &v, path) == -1;
		fv_sort(&v, fi_cmp);
		if (arg_num > 1) {
			if (i) putchar('\n');
			puts(path);
			puts(":\n");
		}
		fmt_file_list(c, stdout, now, v);
		fv_clear(&v);
	};
	return err;
}
