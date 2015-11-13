#define _GNU_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xxhash/xxhash.h"

#define BUFLEN 65536

#define program_name "lsc"

#include "filevercmp.h"
#include "ht.h"
#include "util.h"
#include "fbuf.h"
#include "config.h"

enum type_str_index {
	i_none,     // Nothing else applies

	i_read,     // Readable
	i_write,    // Writeable
	i_exec,     // Executable

	i_dir,      // Directory
	i_char,     // Character device
	i_block,    // Block device
	i_fifo,     // FIFO
	i_link,     // Symlink

	i_sock,     // Socket
	i_uid,      // SUID
	i_uid_exec, // SUID and executable
	i_sticky,   // Sticky
	i_sticky_o, // Sticky, writeable by others

	i_unknown,  // Anything else
};

enum file_kind {
	T_FILE,
	T_DIR,
	T_LINK,
	T_FIFO,
	T_SOCK,
	T_BLK,
	T_CHR,
	T_ORPHAN,
	T_EXEC,
	T_SETUID,
	T_SETGID,
	T_STICKY,
	T_OW,
	T_STICKYOW,
};

void usage(void) {
	logf("Usage: %s [option ...] [file ...]\n"
		"  -C when  use colours (never, always or auto)\n"
		"  -F       append file type indicator\n"
		"  -a       show all files\n"
		"  -c       use ctime\n"
		"  -r       reverse sort\n"
		"  -g       group directories first\n"
		"  -S       sort by size\n"
		"  -t       sort by time\n"
		"  -h       show this help",
		program_name);
}

struct file_info {
	struct suf_indexed name;
	struct suf_indexed linkname;
	mode_t mode;
	mode_t linkmode;
	time_t time;
	off_t size;
	bool linkok;
};

struct file_list {
	struct file_info *data;
	size_t len;
	size_t cap;
};

enum use_color {
	COLOR_NEVER,
	COLOR_ALWAYS,
	COLOR_AUTO,
};

enum sort_type {
	SORT_SIZE,
	SORT_FVER,
	SORT_TIME,
};

struct opts {
	char **rest;
	size_t restc;
	enum sort_type sort;
	enum use_color color;
	int reverse;
	bool all;
	bool classify;
	bool ctime;
	bool group_dir;
};

static struct opts opts;

//
// Sorter
//

static int sorter(const struct file_info *a, const struct file_info *b)
{
	if (opts.group_dir) {
		if (S_ISDIR(a->linkmode) != S_ISDIR(b->linkmode)) {
			return S_ISDIR(a->linkmode) ? -1 : 1;
		}
		if (S_ISDIR(a->mode) != S_ISDIR(b->mode)) {
			return S_ISDIR(a->mode) ? -1 : 1;
		}
	}
	if (opts.sort == SORT_SIZE) {
		register ssize_t s = a->size-b->size;
		if (s) { return ((s>0)-(s<0))*opts.reverse; }
	} else if (opts.sort == SORT_TIME) {
		register time_t t = a->time - b->time;
		if (t) { return ((t>0)-(t<0))*opts.reverse; }
	}
	return filevercmp(a->name, b->name)*opts.reverse;
}

#define SORT_NAME fi
#define SORT_TYPE struct file_info *
#define SORT_CMP(x, y) (sorter(x, y))
#include "sort/sort.h"

//
// Argument parsing
//

static int parse_arg_colorize(int argc, char **argv, char **s, int i) {
	char *use;
	if ((*s)[1] == '\0') {
		if (i+1 < argc) {
			// No argument
			warn("option '-C' needs an argument\n");
			usage();
			die();
		}
		// Argument is next one
		use = argv[i+1];
		i++;
	} else {
		// Argument is part of this one
		use = *s+1;
		*s += strlen(*s)-1;
	}

	if (strcmp("never", use) == 0) {
		opts.color = COLOR_NEVER;
	} else if (strcmp("always", use) == 0) {
		opts.color = COLOR_ALWAYS;
	} else if (strcmp("auto", use) == 0) {
		opts.color = COLOR_AUTO;
	} else {
		warnf("invalid argument to option '-C': \"%s\"", use);
		usage();
		die();
	}
	return i;
}

static void parse_args(int argc, char **argv)
{
	opts.color = COLOR_AUTO;
	opts.sort = SORT_FVER;
	opts.rest = xmallocr((size_t)argc, sizeof(char *));
	opts.reverse = 1;
	size_t restc = 0;

	for (int i = 1; i < argc; i++) {
		char* s = argv[i];
		if (s[0] == '\0' || s[0] != '-' || s[1] == '\0') {
			// not an option
			opts.rest[restc] = s;
			restc++;
			continue;
		}
		if (s[1] == '-' && s[2] == '\0') {
			// "--" ends opts
			for (i = i+1; i < argc; i++) {
				opts.rest[restc] = argv[i];
				restc++;
			}
			break;
		}
		// loop through opts
		s++;
		for (char f = *s; f != '\0'; f = *(++s)) {
			switch (f) {
			case 'a':
				opts.all = true;
				break;
			case 'F':
				opts.classify = true;
				break;
			case 'c':
				opts.ctime = true;
				break;
			case 'r':
				opts.reverse = -1;
				break;
			case 'g':
				opts.group_dir = true;
				break;
			case 't':
				opts.sort = SORT_TIME;
				break;
			case 'S':
				opts.sort = SORT_SIZE;
				break;
			case 'C':
				i = parse_arg_colorize(argc, argv, &s, i);
				break;
			case 'h':
				usage();
				exit(EXIT_SUCCESS);
			default:
				warnf("unsupported option '%c'", f);
				usage();
				die();
			}
		}
	}
	if (restc == 0) {
		opts.rest[0] = ".";
		restc = 1;
	}
	opts.restc = restc;
}

//
// File listing
//

static void file_list_init(struct file_list *fl)
{
	fl->data = xmallocr(128, sizeof(struct file_info));
	fl->cap = 128;
	fl->len = 0;
}

static void file_list_clear(struct file_list *fl)
{
	for (size_t i = 0; i < fl->len; i++) {
		free((char*)fl->data[i].name.str);
		free((char*)fl->data[i].linkname.str);
	}
	fl->len = 0;
}

static void file_list_free(struct file_list *fl)
{
	free(fl->data);
	fl->cap = 0;
	fl->len = 0;
}

/*
char *clean_right(char *path) []byte {
	for i := len(path); i > 0; i-- {
		if path[i-1] != '/' {
			return path[:i]
		}
	}
	return path
}
*/

// guarantees that returned string is size long
static int ls_readlink(int dirfd, char *name, size_t size,
	struct suf_indexed *out)
{
	char *buf = xmalloc(size+1); // allocate length + \0
	ssize_t n = readlinkat(dirfd, name, buf, size);
	if (n == -1) {
		free(buf);
		return -1;
	}
	assertx((size_t)n <= size); // XXX: symlink grew, handle this better
	buf[n] = '\0';
	*out = new_suf_indexed_len(buf, n);
	return 0;
}

// stat writes a file_info describing the named file
static int ls_stat(int dirfd, char *name, struct file_info *out)
{
	struct stat st;
	if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		return -1;
	}

	*out = (struct file_info) {
		.name = new_suf_indexed(name),
		.linkname = (struct suf_indexed) {0},
		.mode = st.st_mode,
		.linkmode = 0,
		.time = opts.ctime ? st.st_ctim.tv_sec : st.st_mtim.tv_sec,
		.size = st.st_size,
		.linkok = true,
	};

	if (S_ISLNK(out->mode)) {
		int ln = ls_readlink(dirfd, name,
			st.st_size,
			&(out->linkname));
		if (ln == -1) {
			out->linkok = false;
			return 0;
		}
		if (fstatat(dirfd, name, &st, 0) == -1) {
			out->linkok = false;
			return 0;
		}
		out->linkmode = st.st_mode;
	}

	return 0;
}

static int ls_readdir(struct file_list *l, char *name)
{
	for (size_t l = strlen(name)-1; name[l] == '/' && l>1; l--) {
		name[l] = '\0';
	}
	int err = 0;
	DIR *dir = opendir(name);
	if (dir == NULL) {
		warnf("cannot open directory %s: %s", name, strerror(errno));
		return -1;
	}
	int fd = dirfd(dir);
	if (fd == -1) {
		warnf("%s: %s", name, strerror(errno));
		return -1;
	}
	struct dirent *dent;
	while ((dent = readdir(dir)) != NULL) {
		char *dn = dent->d_name;
		// skip ".\0", "..\0" and
		// names starting with '.' when opts.all is true
		if (dn[0] == '.' &&
			(!opts.all || dn[1] == '\0' ||
			(dn[1] == '.' && dn[2] == '\0'))) {
			continue;
		}
		if (l->len >= l->cap) {
			assertx(!size_mul_overflow(l->cap, 2, &l->cap));
			l->data = xreallocr(l->data, l->cap,
				sizeof(struct file_info));
		}
		dn = strdup(dn);
		if (ls_stat(fd, dn, l->data+l->len) == -1) {
			warnf("cannot access %s/%s: %s", name, dn, strerror(errno));
			err = -1; // Return -1 on errors
			free(dn);
			continue;
		}
		l->len++;
	}
	if (closedir(dir) == -1) {
		return -1;
	}
	return err;
}

// get info about file/directory name
// bufsize must be at least 1
int ls(struct file_list *l, char *name)
{
	char *s = strdup(name); // make freeing simpler
	if (ls_stat(AT_FDCWD, s, l->data) == -1) {
		warnf("cannot access %s: %s", s, strerror(errno));
		free(s);
		_exit(EXIT_FAILURE);
	}
	if (S_ISDIR(l->data->mode)) {
		free(s);
		if (ls_readdir(l, name) == -1) {
			return -1;
		}
		return 0;
	}
	l->len = 1;
	return 0;
}

//
// LS_COLORS parser
//

static int keyeq(const ssht_key_t a, const ssht_key_t b)
{
	return a.len == b.len && memcmp(a.key, b.key, a.len) == 0;
}

static unsigned keyhash(const ssht_key_t a)
{
	return XXH32(a.key, a.len, 0);
}

// XXX: global state
static char *lsc_env;
static ssht_t *ht;

static void parse_ls_color(void)
{
	lsc_env = getenv("LS_COLORS");
	ht = ssht_alloc(keyhash, keyeq);
	bool eq = false;
	size_t kb = 0, ke = 0;
	for (size_t i = 0; lsc_env[i] != '\0'; i++) {
		const char b = lsc_env[i];
		if (b == '=') {
			ke = i;
			eq = true;
			continue;
		}
		if (!eq || b != ':') {
			continue;
		}

		if (lsc_env[kb] == '*') {
			ssht_key_t k;
			ssht_value_t v;

			lsc_env[ke] = '\0';
			k.len = ke-kb-1;
			k.key = lsc_env+kb+1;

			lsc_env[i] = '\0';
			v = lsc_env+ke+1;

			ssht_set(ht, k, v);
		} else {
			// type colors are defined at compile time
		}
		kb = i + 1;
		i += 2;
		eq = false;
	}
}

static enum file_kind color_type(mode_t mode)
{
#define S_IXUGO (S_IXUSR|S_IXGRP|S_IXOTH)
	switch (mode&S_IFMT) {
	case S_IFREG:
		if ((mode&S_ISUID) != 0) {
			return T_SETUID;
		} else if ((mode&S_ISGID) != 0) {
			return T_SETGID;
		} else if ((mode&S_IXUGO) != 0) {
			return T_EXEC;
		}
		return T_FILE;
	case S_IFDIR:
		if ((mode&S_ISVTX) != 0 && (mode&S_IWOTH) != 0) {
			return T_STICKYOW;
		} else if ((mode&S_IWOTH) != 0) {
			return T_OW;
		} else if ((mode&S_ISVTX) != 0) {
			return T_STICKY;
		}
		return T_DIR;
	case S_IFLNK:
		return T_LINK;
	case S_IFIFO:
		return T_FIFO;
	case S_IFSOCK:
		return T_SOCK;
	case S_IFCHR:
		return T_CHR;
	case S_IFBLK:
		return T_BLK;
	default:
		// anything else is classified as orphan
		return T_ORPHAN;
	}
}

//
// Time printer
//

#define SECOND 1
#define MINUTE (60 * SECOND)
#define HOUR   (60 * MINUTE)
#define DAY    (24 * HOUR)
#define WEEK   (7  * DAY)
#define MONTH  (30 * DAY)
#define YEAR   (12 * MONTH)

static time_t current_time(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_REALTIME, &t) == -1) {
		die_errno();
	}
	return t.tv_sec;
}

void fmt3(char b[3], uint16_t x) {
	if (x/100) b[0] = '0' + x/100;
	if (x/100||x/10%10) b[1] = '0' + x/10%10;
	b[2] = '0' + x%10;
}

static void reltime_color(fb *out, const time_t now, const time_t then)
{
	time_t diff = now - then;
	char b[4] = "  0s";

	if (diff < 0) {
		fb_ws(out, C_SECOND "  0s" C_END);
		return;
	}

	if (diff <= SECOND) {
		fb_ws(out, C_SECOND " <1s" C_END);
		return;
	}

	if (diff < MINUTE) {
		fb_ws(out, C_SECOND);
		fmt3(b, diff);
		b[3] = 's';
	} else if (diff < HOUR) {
		fb_ws(out, C_MINUTE);
		diff /= MINUTE;
		b[3] = 'm';
	} else if (diff < HOUR*36) {
		fb_ws(out, C_HOUR);
		diff /= HOUR;
		b[3] = 'h';
	} else if (diff < MONTH) {
		fb_ws(out, C_DAY);
		diff /= DAY;
		b[3] = 'd';
	} else if (diff < YEAR) {
		fb_ws(out, C_WEEK);
		diff /= WEEK;
		b[3] = 'w';
	} else {
		fb_ws(out, C_YEAR);
		diff /= YEAR;
		b[3] = 'y';
	}
	fmt3(b, diff);
	fb_write(out, b, 4);
	fb_ws(out, C_END);
}

static void reltime_no_color(fb *out, const time_t now, const time_t then)
{
	time_t diff = now - then;
	char b[4] = "  0s";

	if (diff < 0) {
		fb_ws(out, "  0s");
		return;
	}

	if (diff <= SECOND) {
		fb_ws(out, " <1s");
		return;
	}

	if (diff < MINUTE) {
		b[4] = 's';
	} else if (diff < HOUR) {
		diff /= MINUTE;
		b[4] = 'm';
	} else if (diff < HOUR*36) {
		diff /= HOUR;
		b[4] = 'h';
	} else if (diff < MONTH) {
		diff /= DAY;
		b[4] = 'd';
	} else if (diff < YEAR) {
		diff /= WEEK;
		b[4] = 'w';
	} else {
		diff /= YEAR;
		b[4] = 'y';
	}
	fmt3(b, diff);
	fb_write(out, b, 4);
}

//
// Mode printer
//

#define tc fb_sstr

// create mode strings
static void strmode(fb *out, const mode_t mode, const struct sstr *ts)
{
	switch (mode&S_IFMT) {
	case S_IFREG: tc(out, ts[i_none]); break;
	case S_IFDIR: tc(out, ts[i_dir]); break;
	case S_IFCHR: tc(out, ts[i_char]); break;
	case S_IFBLK: tc(out, ts[i_block]); break;
	case S_IFIFO: tc(out, ts[i_fifo]); break;
	case S_IFLNK: tc(out, ts[i_link]); break;
	case S_IFSOCK: tc(out, ts[i_sock]); break;
	default: tc(out, ts[i_unknown]); break;
	}
	tc(out, ts[mode&S_IRUSR ? i_read : i_none]);
	tc(out, ts[mode&S_IWUSR ? i_write : i_none]);
	tc(out, ts[mode&S_ISUID
		? mode&S_IXUSR ? i_uid_exec : i_uid
		: mode&S_IXUSR ? i_exec : i_none]);
	tc(out, ts[mode&S_IRGRP ? i_read : i_none]);
	tc(out, ts[mode&S_IWGRP ? i_write : i_none]);
	tc(out, ts[mode&S_ISGID
		? mode&S_IXGRP ? i_uid_exec : i_uid
		: mode&S_IXGRP ? i_exec : i_none]);
	tc(out, ts[mode&S_IROTH ? i_read : i_none]);
	tc(out, ts[mode&S_IWOTH ? i_write : i_none]);
	tc(out, ts[mode&S_ISVTX
		? mode&S_IXOTH ? i_sticky : i_sticky_o
		: mode&S_IXOTH ? i_exec : i_none]);
}

#undef tc

//
// Size printer
//

static off_t divide(off_t x, off_t d) {
	return (x+((d-1)/2))/d;
}

static void write_size(fb *out, off_t sz, const struct sstr sufs[7])
{
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
		b[2] ='0' + v%10;
	}
	fb_write(out, b, 3);
	fb_sstr(out, sufs[m]);
}

static void size_color(fb *out, off_t size)
{
	fb_ws(out, C_SIZE);
	write_size(out, size, c_sizes);
}

static void size_no_color(fb *out, off_t size)
{
	write_size(out, size, n_sizes);
}

//
// Name printer
//

const char *type_color(enum file_kind t)
{
	switch (t) {
	case T_FILE:     return C_FILE;
	case T_DIR:      return C_DIR;
	case T_LINK:     return C_LINK;
	case T_FIFO:     return C_FIFO;
	case T_SOCK:     return C_SOCK;
	case T_BLK:      return C_BLK;
	case T_CHR:      return C_CHR;
	case T_ORPHAN:   return C_ORPHAN;
	case T_EXEC:     return C_EXEC;
	case T_SETUID:   return C_SETUID;
	case T_SETGID:   return C_SETGID;
	case T_STICKY:   return C_STICKY;
	case T_OW:       return C_OW;
	case T_STICKYOW: return C_STICKYOW;
	}
	abort();
}

static const char *suf_color(const char *name, size_t len)
{
	char *n = memrchr(name, '.', len);
	if (n != NULL) {
		ssht_key_t k;
		k.key = n;
		k.len = name+len-n;
		return ssht_get(ht, k);
	}
	return NULL;
}

static const char *file_color(const char *name, size_t len, enum file_kind t) {
	const char *c;
	if (t == T_FILE || t == T_LINK) {
		c = suf_color(name, len);
		if (c != NULL) {
			return c;
		}
	}
	return type_color(t);
}

void classify(fb *out, enum file_kind t) {
	switch (t) {
	case T_DIR:  fb_putc(out, '/'); break;
	case T_EXEC: fb_putc(out, '*'); break;
	case T_FIFO: fb_putc(out, '|'); break;
	case T_SOCK: fb_putc(out, '='); break;
	}
}

static void name_color(fb *out, const struct file_info *f)
{
	enum file_kind t;
	const char *c;
	if (f->linkname.str) {
		if (f->linkok) {
			t = color_type(f->linkmode);
		} else {
			t = T_ORPHAN;
		}
		c = file_color(f->linkname.str, f->linkname.len, t);
	} else {
		t = color_type(f->mode);
		c = file_color(f->name.str, f->name.len, t);
	}
	fb_ws(out, C_ESC);
	fb_puts(out, c);
	fb_ws(out, "m");
	fb_write(out, f->name.str, f->name.len);
	fb_ws(out, C_END);
	if (f->linkname.str) {
		if (opts.classify)
			fb_putc(out, '@');
		fb_ws(out, C_SYM_DELIM);
		fb_ws(out, C_ESC);
		fb_puts(out, c);
		fb_ws(out, "m");
		fb_write(out, f->linkname.str, f->linkname.len);
		fb_ws(out, C_END);
	}
	if (opts.classify)
		classify(out, t);
}

static void name_no_color(fb *out, const struct file_info *f)
{
	enum file_kind t;
	if (f->linkname.str) {
		if (f->linkok) {
			t = color_type(f->linkmode);
		} else {
			t = T_ORPHAN;
		}
	} else {
		t = color_type(f->mode);
	}
	fb_write(out, f->name.str, f->name.len);
	if (f->linkname.str) {
		if (opts.classify)
			fb_putc(out, '@');
		fb_ws(out, N_SYM_DELIM);
		fb_write(out, f->linkname.str, f->linkname.len);
	}
	if (opts.classify)
		classify(out, t);
}

int main(int argc, char **argv)
{
	parse_args(argc, argv);
	parse_ls_color();
	bool colorize =
		(opts.color == COLOR_AUTO && isatty(STDOUT_FILENO)) ||
		opts.color == COLOR_ALWAYS;
	struct file_list l;
	file_list_init(&l);
	fb out;
	fb_init(&out, STDOUT_FILENO, BUFLEN);
	time_t now = current_time();
	int err = EXIT_SUCCESS;

	const struct sstr *modes;
	void (*reltime)(fb *, const time_t, const time_t);
	void (*size)(fb *, off_t);
	void (*name)(fb *, const struct file_info *);

	if (colorize) {
		modes = c_modes;
		reltime = reltime_color;
		size = size_color;
		name = name_color;
	} else {
		modes = n_modes;
		reltime = reltime_no_color;
		size = size_no_color;
		name = name_no_color;
	}

	struct file_info **ll = NULL;
	size_t lllen = 0;
	struct file_info *fi;

	for (size_t i = 0; i < opts.restc; i++) {
		if (ls(&l, opts.rest[i]) == -1)
			err = EXIT_FAILURE;
		if (l.len>lllen) {
			ll = xreallocr(ll, l.len, sizeof(*ll));
			lllen=l.len;
		}
		for (size_t j = 0; j < l.len; j++) {
			ll[j] = &l.data[j];
		}
		fi_tim_sort(ll, l.len);
		for (size_t j = 0; j < l.len; j++) {
			fi = ll[j];
			strmode(&out, fi->mode, modes);
			reltime(&out, now, fi->time);
			fb_putc(&out, ' ');
			size(&out, fi->size);
			fb_putc(&out, ' ');
			name(&out, fi);
			fb_putc(&out, '\n');
		}
		file_list_clear(&l);
	};
	fb_flush(&out);
	file_list_free(&l);
	//fb_free(&out);
	return err;
}
