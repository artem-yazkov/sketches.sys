#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <search.h>
#include <sys/queue.h>

typedef void roommates_t;
typedef void rooms_t;
typedef void conns_t;

typedef struct roommate_s {
    char        *name;
    char        *passwd;
    rooms_t     *rooms;
    conns_t     *conns;
} roommate_t;

typedef struct room_s {
    char        *name;
    bool         is_open;
    roommates_t *mates;
    conns_t     *conns;
} room_t;

typedef struct conn_s {
    roommate_t *roommate;
    room_t     *room;
} conn_t;

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

/*
 * room mates handling
 */
static int
roommate_create(roommate_t **roommate, cfg_obj_t *obj)
{
    if (!obj->name_sz || !obj->ext_sz) {
        return -1;
    }
    if ((*roommate = calloc(1, sizeof(roommate_t))) == NULL) {
        goto error;
    }
    if (((*roommate)->name = strndup(obj->name, obj->name_sz)) == NULL) {
        goto error;
    }
    if (((*roommate)->passwd = strndup(obj->ext, obj->ext_sz)) == NULL) {
        goto error;
    }
    return 0;

error:
    if (*roommate) {
        free((*roommate)->name);
        free((*roommate)->passwd);
        free((*roommate));
        *roommate = NULL;
    }
    return -1;
}

static void
roommate_del(roommate_t *roommate)
{
    free(roommate->name);
    free(roommate->passwd);
    free(roommate);
}

static int
roommates_compar(const void *mate_l, const void *mate_r)
{
    return strcmp(((roommate_t *)mate_l)->name, ((roommate_t *)mate_r)->name);
}

static int
roommates_add(roommates_t **roommates, cfg_objlist_t *objlist)
{
    int newmates = 0;
    cfg_obj_t *cfg_obj;
    LIST_FOREACH(cfg_obj, objlist, lentry) {
        if (!cfg_obj->name_sz || !cfg_obj->ext_sz) {
            continue;
        }
        roommate_t *roommate;
        if (roommate_create(&roommate, cfg_obj) >= 0) {
            void *tmate = tsearch(roommate, roommates, roommates_compar);
            if (!tmate) {
                /* allocation error */
                roommate_del(roommate);
            } else if (*(roommate_t **)tmate != roommate) {
                /* already exists */
                roommate_del(roommate);
            } else {
                newmates++;
            }
        }
    }
    return newmates;
}

static int
roommates_del(roommates_t **roommates, cfg_objlist_t *objlist)
{
    cfg_obj_t *cfg_obj;
    LIST_FOREACH(cfg_obj, objlist, lentry) {
        roommate_t roommate = {
            .name = strndup(cfg_obj->name, cfg_obj->name_sz),
            .passwd = strndup(cfg_obj->ext, cfg_obj->ext_sz)
        };
        if (roommate.name && roommate.passwd) {
            tdelete(&roommate, roommates, roommates_compar);
        }
        free(roommate.name);
        free(roommate.passwd);
    }
    return 0;
}

static int
roommates_clear(roommates_t **roommates)
{
    while (*roommates) {
        roommate_t *roommate = *(roommate_t **)(*roommates);
        tdelete(roommate, roommates, roommates_compar);
        roommate_del(roommate);
    }
    return 0;
}

static void
roommates_walk(const void *ptr, VISIT order, int level)
{
    if (order == postorder || order == leaf) {
        roommate_t *roommate = *(roommate_t **) ptr;
        printf("roommate:: name:%s, passwd:%s\n", roommate->name, roommate->passwd);
    }
}

/*
 * rooms handling
 */
static int
rooms_compar(const void *mate_l, const void *mate_r)
{
    return strcmp(((room_t *)mate_l)->name, ((room_t *)mate_r)->name);
}

static int
room_add_mates(room_t *room, char *name, size_t name_sz, cfg_objlist_t *objlist)
{
    return 0;
}

static int
room_del_mates(room_t *room, char *name, size_t name_sz, cfg_objlist_t *objlist)
{
    return 0;
}

static int
room_clear_mates(room_t *room, char *name, size_t name_sz)
{
    return 0;
}

static int
room_del(room_t *room, char *name, size_t name_sz)
{
    return 0;
}

static int
room_clear(room_t *room)
{
    return 0;
}

/*
 * configuration handling
 */

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

int main(int argc, char **argv)
{
#if 0
    cfg_objlist_t roommates = {0};
    char *objstring = ":one,,two:;; three:e";

    LIST_INIT(&roommates);
    cfg_objstring_parse(objstring, strlen(objstring), &roommates, CFG_OBJ_ROOMMATES);

    cfg_obj_t *cfg_obj;
    LIST_FOREACH(cfg_obj, &roommates, lentry) {
        printf("name: '%.*s', ext: '%.*s'\n",
                (int)(cfg_obj->name_sz), cfg_obj->name,
                (int)(cfg_obj->ext_sz), cfg_obj->ext);
    }
    cfg_objlist_clear(&roommates);
#endif

    char *rmates_cstr = "alpha:alpha,bravo:bravo,charlie:charlie";
    cfg_objlist_t rmates_clist;

    LIST_INIT(&rmates_clist);
    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);

    roommates_t *roommates = NULL;
    roommates_add(&roommates, &rmates_clist);
    twalk(roommates, roommates_walk);
    printf("\n");

    rmates_cstr = "bravo:bravo,delta:delta";
    cfg_objlist_clear(&rmates_clist);
    LIST_INIT(&rmates_clist);

    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);
    roommates_add(&roommates, &rmates_clist);
    twalk(roommates, roommates_walk);
    printf("\n");


    rmates_cstr = "bravo:bravo,bravo:bravo,unknown:unknown";
    cfg_objlist_clear(&rmates_clist);
    LIST_INIT(&rmates_clist);

    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);
    roommates_del(&roommates, &rmates_clist);
    twalk(roommates, roommates_walk);
    printf("\n");

    roommates_clear(&roommates);
    twalk(roommates, roommates_walk);
    printf("\n");

    return 0;
}
