#define VEC(VEC_NAME, VEC_TYPE, ELEM_TYPE) \
typedef struct VEC_TYPE { \
	ELEM_TYPE *data; \
	size_t cap; \
	size_t len; \
} VEC_TYPE; \
\
static void VEC_NAME ## _init(VEC_TYPE *v, size_t init) { \
	v->data = xmallocr(init, sizeof(ELEM_TYPE)); \
	v->cap = init; \
	v->len = 0; \
} \
\
static void VEC_NAME ## _clear(VEC_TYPE *v) { v->len = 0; } \
\
static ELEM_TYPE *VEC_NAME ## _stage(VEC_TYPE *v) { \
	if (v->len >= v->cap) { \
		v->cap = size_mul(v->cap, 2); \
		v->data = xreallocr(v->data, v->cap, \
			sizeof(ELEM_TYPE)); \
	} \
	return &v->data[v->len]; \
} \
\
static void VEC_NAME ## _commit(VEC_TYPE *v) { v->len++; } \
\
static ELEM_TYPE *VEC_NAME ## _index(VEC_TYPE *v, size_t i) { \
	return &v->data[i]; \
} \
\
static void VEC_NAME ## _sort(VEC_TYPE *v, \
	int (*compar)(const void *, const void *)) { \
       qsort(v->data, v->len, sizeof(ELEM_TYPE), compar); \
}
