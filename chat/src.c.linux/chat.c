#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
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

/***********************
 * comparison
 ***********************/
static int
roommates_compar(const void *mate_l, const void *mate_r)
{
    return strcmp(((roommate_t *)mate_l)->name, ((roommate_t *)mate_r)->name);
}

static int
rooms_compar(const void *mate_l, const void *mate_r)
{
    return strcmp(((room_t *)mate_l)->name, ((room_t *)mate_r)->name);
}

/***********************
 * walk
 ***********************/
typedef enum status_verbosity_e {
    STATUS_VERBOSITY_SHORT,
    STATUS_VERBOSITY_LONG
} status_verbosity_t;

static void
roommates_walk(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
        roommate_t *mate = *(roommate_t **) ptr;
        if ((uint64_t)ctx == STATUS_VERBOSITY_SHORT) {
            printf("%s  ", mate->name);
        } else {
            printf("%s:%s  ", mate->name, mate->passwd);
        }
    }
}

static void
rooms_walk(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
        room_t *room = *(room_t **) ptr;
        if ((uint64_t)ctx == STATUS_VERBOSITY_SHORT) {
            printf("%s  ", room->name);
        } else {
            printf("name: %s, open: %s, mates:  ", room->name, room->is_open ? "yes" : "no");
            twalk_r(room->mates, roommates_walk, (void *)STATUS_VERBOSITY_SHORT);
            printf("\n");
        }
    }
}

/***********************
 * room mates handling
 ***********************/
static int
roommate_create(roommate_t **mate, cfg_obj_t *cfgmate)
{
    if (!cfgmate->name_sz || !cfgmate->ext_sz) {
        return -1;
    }
    if ((*mate = calloc(1, sizeof(roommate_t))) == NULL) {
        goto error;
    }
    if (((*mate)->name = strndup(cfgmate->name, cfgmate->name_sz)) == NULL) {
        goto error;
    }
    if (((*mate)->passwd = strndup(cfgmate->ext, cfgmate->ext_sz)) == NULL) {
        goto error;
    }
    return 0;

error:
    if (*mate) {
        free((*mate)->name);
        free((*mate)->passwd);
        free((*mate));
        *mate = NULL;
    }
    return -1;
}

static void
roommate_del(roommate_t *mate)
{
    while (mate->rooms) {
        room_t *troom = *(room_t **)(mate->rooms);
        tdelete(mate, troom->mates, roommates_compar);
        tdelete(troom, mate->rooms, rooms_compar);
   }
   free(mate->name);
   free(mate->passwd);
   free(mate);
}

static int
roommates_add(roommates_t **mates, cfg_objlist_t *cfgmates)
{
    cfg_obj_t *cmate;
    LIST_FOREACH(cmate, cfgmates, lentry) {
        if (!cmate->name_sz || !cmate->ext_sz) {
            fprintf(stderr, "Room Mate name and password must be set. Ignore object '%.*s:%.*s'\n",
                    (int)(cmate->name_sz), cmate->name,
                    (int)(cmate->ext_sz), cmate->ext);
            continue;
        }
        roommate_t *roommate;
        if (roommate_create(&roommate, cmate) >= 0) {
            void *pmate = tsearch(roommate, mates, roommates_compar);
            if (!pmate) {
                /* allocation error */
                roommate_del(roommate);
                return -1;
            } else if (*(roommate_t **)pmate != roommate) {
                /* already exists */
                //fprintf(stderr, "Room Mate %s exists. Can't create new one with same name\n");
                roommate_del(roommate);
            }
        }
    }
    return 0;
}

static int
roommates_del(roommates_t **mates, cfg_objlist_t *cfgmates)
{
    cfg_obj_t *cmate;
    LIST_FOREACH(cmate, cfgmates, lentry) {
        roommate_t kmate = {
            .name = strndup(cmate->name, cmate->name_sz)
        };
        if (kmate.name) {
            void *pmate = tfind(&kmate, mates, roommates_compar);
            if (pmate) {
                roommate_t *tmate = *(roommate_t **)pmate;
                tdelete(tmate, mates, roommates_compar);
                roommate_del(tmate);
            }
        }
        free(kmate.name);
    }
    return 0;
}

static int
roommates_clear(roommates_t **mates)
{
    while (*mates) {
        roommate_t *tmate = *(roommate_t **)(*mates);
        tdelete(tmate, mates, roommates_compar);
        roommate_del(tmate);
    }
    return 0;
}

/******************
 * rooms handling
 ******************/
static int
room_create(room_t **room, char *name, size_t name_sz)
{
    if ((*room = calloc(1, sizeof(room_t))) == NULL) {
        goto error;
    }
    if (((*room)->name = strndup(name, name_sz)) == NULL) {
        goto error;
    }
    return 0;

error:
    if (*room) {
        free((*room)->name);
        *room = NULL;
    }
    return -1;
}

static void
room_del(room_t *room)
{
    while (room->mates) {
        roommate_t *tmate = *(roommate_t **)(room->mates);
        tdelete(room, tmate->rooms, rooms_compar);
        tdelete(tmate, room->mates, roommates_compar);
    }
    free(room->name);
    free(room);
}

static int
room_add_mates(rooms_t **rooms, roommates_t **mates, char *room_name, size_t room_name_sz, cfg_objlist_t *cfgmates)
{
    room_t *room;
    if (room_create(&room, room_name, room_name_sz) < 0) {
        fprintf(stderr, "Can't create new room '%.*s'\n", (int)room_name_sz, room_name);
        return -1;
    }
    void *proom = tsearch(room, rooms, rooms_compar);
    if (!proom) {
        /* allocation error */
        room_del(room);
        return -1;
    } else if (*(room_t **)proom != room) {
        /* already exists */
        room_del(room);
        room = *(room_t **)proom;
    }

    /* add mates to the room */
    cfg_obj_t *cmate;
    LIST_FOREACH(cmate, cfgmates, lentry) {
        if (cmate->name_sz == 1 && cmate->name[0] == '*') {
            room->is_open = true;
            continue;
        }
        roommate_t kmate = {
            .name = strndup(cmate->name, cmate->name_sz)
        };
        if (!kmate.name) {
            /* allocation error */
            return -1;
        }
        void *pmate = tfind(&kmate, mates, roommates_compar);
        if (pmate) {
            roommate_t *tmate = *(roommate_t **)pmate;
            tsearch(tmate, &room->mates, roommates_compar);
            tsearch(room, &tmate->rooms, rooms_compar);
        }
        free(kmate.name);
    }
    return 0;
}

static int
room_del_mates(rooms_t *rooms, char *name, size_t name_sz, cfg_objlist_t *cfgmates)
{
    /* find the room by name */
    roommate_t kroom = {
        .name = strndup(name, name_sz)
    };
    if (!kroom.name) {
        /* allocation error */
        return -1;
    }
    void *proom = tfind(&kroom, rooms, rooms_compar);
    free(kroom.name);
    if (!proom) {
        /* the room does not exists */
        return -1;
    }

    /* delete mates from the room */
    room_t *troom = *(room_t **)proom;
    cfg_obj_t *cmate;
    LIST_FOREACH(cmate, cfgmates, lentry) {
        if (cmate->name_sz == 1 && cmate->name[0] == '*') {
            troom->is_open = false;
            continue;
        }
        roommate_t kmate = {
            .name = strndup(cmate->name, cmate->name_sz)
        };
        if (!kmate.name) {
            /* allocation error */
            continue;
        }
        void *pmate = tfind(&kmate, &troom->mates, roommates_compar);
        if (!pmate) {
            continue;
        }
        roommate_t *tmate = *(roommate_t **)pmate;

        tdelete(troom, &tmate->rooms, rooms_compar);
        tdelete(tmate, &troom->mates, roommates_compar);
    }
    return 0;
}

static int
room_clear_mates(room_t *room)
{
    while (room->mates) {
        roommate_t *tmate = *(roommate_t **)(room->mates);
        tdelete(room, &tmate->rooms, rooms_compar);
        tdelete(tmate, &room->mates, roommates_compar);
    }
    return 0;
}

static int
rooms_clear(rooms_t **rooms)
{
    while (*rooms) {
        room_t *troom = *(room_t **)(*rooms);
        tdelete(troom, rooms, rooms_compar);
        //room_del(troom);
    }
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
    char *rmates_cstr = "alpha:111,bravo:222,charlie:333,delta:444";
    cfg_objlist_t rmates_clist;

    LIST_INIT(&rmates_clist);
    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);

    roommates_t *roommates = NULL;
    roommates_add(&roommates, &rmates_clist);
    cfg_objlist_clear(&rmates_clist);

    rooms_t *rooms = NULL;

    rmates_cstr = "alpha,bravo";
    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);
    room_add_mates(&rooms, &roommates, "bedroom", strlen("bedroom"), &rmates_clist);
    cfg_objlist_clear(&rmates_clist);

    rmates_cstr = "charlie,delta,unknown,*";
    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);
    room_add_mates(&rooms, &roommates, "guestroom", strlen("guestroom"), &rmates_clist);
    cfg_objlist_clear(&rmates_clist);
    twalk_r(rooms, rooms_walk, (void *)STATUS_VERBOSITY_LONG);

    rmates_cstr = "delta,unknown,*";
    cfg_objstring_parse((char*)rmates_cstr, strlen(rmates_cstr), &rmates_clist, CFG_OBJ_ROOMMATES);
    room_del_mates(&rooms, "guestroom", strlen("guestroom"), &rmates_clist);
    twalk_r(rooms, rooms_walk, (void *)STATUS_VERBOSITY_LONG);

    rooms_clear(&rooms);
    twalk_r(rooms, rooms_walk, (void *)STATUS_VERBOSITY_LONG);
    return 0;
}
