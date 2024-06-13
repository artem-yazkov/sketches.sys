#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

typedef struct roommate_s {
    char   *name;
    char   *passwd;
    void   *conns;
} roommate_t;

typedef struct room_s {
    char   *name;
    void   *mates;
    bool    is_open;
} room_t;

typedef enum cfg_objtype_e {
    CFG_OBJ_ROOMS,
    CFG_OBJ_ROOMMATES
} cfg_objtype_t;

typedef struct cfg_obj_s {
    char   *name;
    size_t  name_sz;
    char   *ext;
    size_t  ext_sz;
    LIST_ENTRY(cfg_obj_s) lentry;
} cfg_obj_t;
typedef LIST_HEAD(cfg_objlist_s, cfg_obj_s) cfg_objlist_t;

static int
cfg_objlist_clear(cfg_objlist_t *objlist)
{
    while (!LIST_EMPTY(objlist)) {
        cfg_obj_t *cfg_obj = LIST_FIRST(objlist);
        LIST_REMOVE(cfg_obj, lentry);
        free(cfg_obj);
    }
    return 0;
}

#define CFG_OBJ_OUTER_DELIMS(c) (isspace(c) || c == ',' || c == ';')
#define CFG_OBJ_INNER_DELIMS(c) (c == ':')
static int
cfg_objstring_parse(char *objstring, size_t objstring_sz, cfg_objlist_t *objlist, cfg_objtype_t objtype)
{
    size_t obj_b, name_b, ext_b;
    size_t obj_e, name_e, ext_e;

    for (obj_b = 0, obj_e = 0 ; obj_b < objstring_sz;) {
        /* skip outer delimiters */
        for (; CFG_OBJ_OUTER_DELIMS(objstring[obj_b]) && (obj_b < objstring_sz); obj_b++);
        /* take object */
        for (obj_e = obj_b; !CFG_OBJ_OUTER_DELIMS(objstring[obj_e]) && (obj_e < objstring_sz); obj_e++);

        if (objtype == CFG_OBJ_ROOMMATES) {
            /* take object name */
            for (name_b = name_e = obj_b; !CFG_OBJ_INNER_DELIMS(objstring[name_e]) && (name_e < obj_e); name_e++);
            /* take object extension */
            ext_b = (name_e < obj_e) ? name_e + 1 : name_e;
            ext_e = obj_e;
        } else {
            name_b = obj_b;
            name_e = obj_e;
            ext_b = ext_e = name_e;
        }

        if (name_b < name_e) {
            cfg_obj_t *obj = calloc(1, sizeof(cfg_obj_t));
            obj->name = &objstring[name_b];
            obj->name_sz = name_e - name_b;
            if (ext_b < ext_e) {
                obj->ext = &objstring[ext_b];
                obj->ext_sz = ext_e - ext_b;
            }
            LIST_INSERT_HEAD(objlist, obj, lentry);
        }
        obj_b = obj_e + 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    cfg_objlist_t roommates = {0};
    char *objstring = ":one,,two:;; three:e";

    cfg_objstring_parse(objstring, strlen(objstring), &roommates, CFG_OBJ_ROOMMATES);
    cfg_obj_t *cfg_obj;
    LIST_FOREACH(cfg_obj, &roommates, lentry) {
        printf("name: '%.*s', ext: '%.*s'\n",
                (int)(cfg_obj->name_sz), cfg_obj->name,
                (int)(cfg_obj->ext_sz), cfg_obj->ext);
    }
    cfg_objlist_clear(&roommates);
    return 0;
}
