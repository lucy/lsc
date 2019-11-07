#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "util.h"
#include "id.h"

struct id { struct id *next; id_t id; char name[]; };

static const char *put(struct id **cache, id_t id, const char *name) {
	struct id *p = xmallocr(sizeof(*p) + strlen(name) + 1, 1);
	strcpy(p->name, name);
	p->id = id;
	p->next = *cache, *cache = p;
	return p->name[0] ? p->name : 0;
}

static struct id *ucache;

const char *getuser(uid_t id) {
	for (struct id *p = ucache; p; p = p->next)
		if (p->id == id) return p->name[0] ? p->name : 0;
	struct passwd *e = getpwuid(id);
	return put(&ucache, id, e ? e->pw_name : "");
}

static struct id *gcache;

const char *getgroup(gid_t id) {
	for (struct id *p = gcache; p; p = p->next)
		if (p->id == id) return p->name[0] ? p->name : 0;
	struct group *e = getgrgid(id);
	return put(&ucache, id, e ? e->gr_name : "");
}
