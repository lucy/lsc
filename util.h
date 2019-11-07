#define log(fmt, ...) (assertx(fprintf(stderr, fmt "\n", __VA_ARGS__) >= 0))

#ifdef program_name
#define warn(fmt, ...) (log("%s: " fmt, program_name, __VA_ARGS__))
#define die(fmt, ...) do { warn(fmt, __VA_ARGS__); exit(1); } while (0)
#define warn_errno(fmt, ...) warn(fmt ": %s", __VA_ARGS__, strerror(errno))
#define die_errno(fmt, ...) die(fmt ": %s", __VA_ARGS__, strerror(errno))
#endif // program_name

#define assertx(expr) (expr?(void)0:abort())

#define MAX(x, y) ((x)>(y)?(x):(y))
#define MIN(x, y) ((x)<(y)?(x):(y))

#define ls_isalpha(c) (((unsigned)(c)|32)-'a' < 26)
#define ls_isdigit(c) ((unsigned)(c)-'0' < 10)

static inline size_t size_mul(size_t a, size_t b) {
    if (b > 1 && SIZE_MAX / b < a) abort();
	return a * b;
}

static inline void *xmallocr(size_t nmemb, size_t size) {
	void *p = malloc(size_mul(nmemb, size));
	assertx(p);
	return p;
}

static inline void *xreallocr(void *p, size_t nmemb, size_t size) {
	p = realloc(p, size_mul(nmemb, size));
	assertx(p);
	return p;
}
