#include <ctype.h>
#include <stdarg.h>
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

typedef struct state_s {
    bool        is_admin;
    roommates_t *mates;
    rooms_t     *rooms;
    conns_t     *conns;
} state_t;

/***********************
 * message broker
 ***********************/

/* Message Types */
#define MSG_TYP_CM      (0x1)   /* Client Chat Message */
#define MSG_TYP_CC      (0x2)   /* Client Command */
#define MSG_TYP_SC      (0x3)   /* Server Command */
#define MSG_TYP_SI      (0x4)   /* Server Info Message */
#define MSG_TYP_SE      (0x5)   /* Server Error Message */
#define MSG_TYP_LI      (0x6)   /* Server Info Message, Local */
#define MSG_TYP_LE      (0x7)   /* Server Error Message, Local */
#define MSG_TYP_MASK(X) (X & 0x0F)

/* Message broadcast width */
#define MSG_WID_AC      (0x1 << 4)  /* to Active Connection only */
#define MSG_WID_MT      (0x2 << 4)  /* to all Room Mate connections, except Active Connection */
#define MSG_WID_MTA     (0x3 << 4)  /* to all Room Mate connections, including Active Connection */
#define MSG_WID_RM      (0x4 << 4)  /* to all Room connections, except Active Connection */
#define MSG_WID_RMA     (0x5 << 4)  /* to all Room connections, including Active Connection */
#define MSG_WID_MASK(X) (X & 0xF0)

#define MSG_NOFIN       (0x1 << 8)  /* wait next part of the message */

typedef struct msg_s {
    char   *data;
    size_t  data_sz;
    int     options;
    bool    ready;
    int     refs;
    CIRCLEQ_ENTRY(msg_s) cq_entry;
} msg_t;
typedef CIRCLEQ_HEAD(msg_list_s, msg_s) msg_list_t;

typedef struct msg_broker_s {
    conn_t *active_conn;
    msg_list_t msg_pool;
    msg_list_t msgs_local;
} msg_broker_t;

static void
msg_set_active_conn(msg_broker_t *broker, conn_t *conn)
{
    broker->active_conn = conn;
}

static int
msg_add(msg_broker_t *broker, int options, const char * format, ... )
{
    /* add new message to the list, or update data on last message */
    msg_t *msg = NULL;
    if (CIRCLEQ_EMPTY(&broker->msg_pool) || CIRCLEQ_LAST(&broker->msg_pool)->ready) {
        msg = calloc(1, sizeof(msg_t));
        if (!(msg = calloc(1, sizeof(msg_t)))) {
            goto error;
        }
        CIRCLEQ_INSERT_TAIL(&broker->msg_pool, msg, cq_entry);
    } else {
        msg = CIRCLEQ_LAST(&broker->msg_pool);
    }
    va_list args;
    va_start(args, format);
    char *chunk = NULL;
    int   chunk_sz = vsnprintf (NULL, 0, format, args);
    va_end(args);
    if (chunk_sz < 0) {
        goto error;
    }
    if (!(chunk = realloc(msg->data, msg->data_sz + chunk_sz + 1))) {
        goto error;
    }
    msg->data = chunk;
    va_start(args, format);
    vsnprintf (&msg->data[msg->data_sz], chunk_sz + 1, format, args);
    va_end(args);
    msg->data_sz += chunk_sz;

    msg->options = options;
    if (!(msg->options & MSG_NOFIN)) {
        msg->ready = 1;

        /* create appropriate references */
        if (!broker->active_conn ||
            (MSG_TYP_MASK(msg->options) == MSG_TYP_LE) || (MSG_TYP_MASK(msg->options) == MSG_TYP_LI)
            ) {
            CIRCLEQ_INSERT_TAIL(&broker->msgs_local, msg, cq_entry);
        }
    }
    return 0;

error:
    if (msg) {
        CIRCLEQ_REMOVE(&broker->msg_pool, msg, cq_entry);
        free(msg->data);
        free(msg);
    }
    return -1;
}

static void
msg_flush_local(msg_broker_t *broker)
{
    msg_t *mfirst = CIRCLEQ_FIRST(&broker->msgs_local);
    for (; mfirst != (void *)&broker->msgs_local; ) {
        fprintf(stdout, "message: '%.*s'\n", (int)mfirst->data_sz, mfirst->data);
        msg_t *mnext = CIRCLEQ_NEXT(mfirst, cq_entry);
        free(mfirst);
        mfirst = mnext;
    }
}

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
 * status routines
 ***********************/
typedef enum status_verbosity_e {
    STATUS_VERBOSITY_SHORT,
    STATUS_VERBOSITY_LONG
} status_verbosity_t;

static void
roommates_status(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
        roommate_t *mate = *(roommate_t **) ptr;
        if ((uint64_t)ctx == STATUS_VERBOSITY_SHORT) {
            printf("%s  ", mate->name);
        } else {
            printf("  * %s (%s)\n", mate->name, mate->passwd);
        }
    }
}

static void
rooms_status(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
        room_t *room = *(room_t **) ptr;
        if ((uint64_t)ctx == STATUS_VERBOSITY_SHORT) {
            printf("%s  ", room->name);
        } else {
            printf("  * name: %s, open: %s, mates:  ", room->name, room->is_open ? "yes" : "no");
            twalk_r(room->mates, roommates_status, (void *)STATUS_VERBOSITY_SHORT);
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
        tdelete(mate, &troom->mates, roommates_compar);
        tdelete(troom, &mate->rooms, rooms_compar);
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
        tdelete(room, &tmate->rooms, rooms_compar);
        tdelete(tmate, &room->mates, roommates_compar);
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
        room_del(troom);
    }
    return 0;
}

/**************************
 * configuration handling
 **************************/

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

static int
cfg_admin(char *cmdline, state_t *state, bool *quit)
{
    int   retcode = 0;
    char *cmdline_sptr = NULL;
    char *cmdline_sdup = strdup(cmdline);

    char *command = !cmdline_sdup ? NULL : strtok_r(cmdline_sdup, " ", &cmdline_sptr);
    if (!command) {
        free(cmdline_sdup);
        return -1;
    }
    if (strcmp(command, ":quit") == 0) {
        *quit = true;
        return 0;
    } else {
        *quit = false;
    }

    if (state->is_admin) {
        if (strcmp(command, ":roommates") == 0) {
            char *subcmd = strtok_r(NULL, " ", &cmdline_sptr);

            if (subcmd && (strcmp(subcmd, "add") == 0) && cmdline_sptr) {
                cfg_objlist_t col_mates;
                LIST_INIT(&col_mates);
                cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_ROOMMATES);

                if (!LIST_EMPTY(&col_mates)) {
                    roommates_add(&state->mates, &col_mates);
                    cfg_objlist_clear(&col_mates);
                }
            } else if (subcmd && (strcmp(subcmd, "del") == 0) && cmdline_sptr) {
                cfg_objlist_t col_mates;
                LIST_INIT(&col_mates);
                cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_ROOMMATES);

                if (!LIST_EMPTY(&col_mates)) {
                    roommates_del(&state->mates, &col_mates);
                    cfg_objlist_clear(&col_mates);
                }
            } else if (subcmd && strcmp(subcmd, "clear") == 0) {
                roommates_clear(&state->mates);

            } else if (subcmd && strcmp(subcmd, "show") == 0) {
                twalk_r(state->mates, roommates_status, (void *)STATUS_VERBOSITY_LONG);
            }
        }

        if (strcmp(command, ":rooms") == 0) {
            char *subcmd = strtok_r(NULL, " ", &cmdline_sptr);

            if (subcmd && strcmp(subcmd, "addmates") == 0) {
                char *rname = strtok_r(NULL, " ", &cmdline_sptr);
                if (rname && cmdline_sptr) {
                    cfg_objlist_t col_mates;
                    LIST_INIT(&col_mates);
                    cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_ROOMMATES);

                    if (!LIST_EMPTY(&col_mates)) {
                        room_add_mates(&state->rooms, &state->mates, rname, strlen(rname), &col_mates);
                        cfg_objlist_clear(&col_mates);
                    }
                }
            }
            if (subcmd && strcmp(subcmd, "delmates") == 0) {
                char *rname = strtok_r(NULL, " ", &cmdline_sptr);
                if (rname && cmdline_sptr) {
                    cfg_objlist_t col_mates;
                    LIST_INIT(&col_mates);
                    cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_ROOMMATES);

                    if (!LIST_EMPTY(&col_mates)) {
                        room_del_mates(&state->rooms, rname, strlen(rname), &col_mates);
                        cfg_objlist_clear(&col_mates);
                    }
                }
            }
        }

        if (strcmp(command, ":status") == 0) {
            printf("Room Mates status:\n");
            twalk_r(state->mates, roommates_status, (void *)STATUS_VERBOSITY_LONG);
            printf("Rooms status:\n");
            twalk_r(state->rooms, rooms_status, (void *)STATUS_VERBOSITY_LONG);
        }
    }
    free(cmdline_sdup);
    return retcode;
}

int main(int argc, char **argv)
{
#if 0
    state_t state = {.is_admin = true};
    bool fquit = false;

    while(!fquit) {
        char  *line = NULL;
        size_t line_sz = 0;
        getline(&line, &line_sz, stdin);
        while (line && strlen(line) && line[strlen(line)-1] == '\n') {
            line[strlen(line)-1] = '\0';
        }
        if (line) {
            cfg_admin(line, &state, &fquit);
        }
        free(line);
    }
    roommates_clear(&state.mates);
    rooms_clear(&state.rooms);
#endif

    msg_broker_t broker = {0};
    CIRCLEQ_INIT(&broker.msg_pool);
    CIRCLEQ_INIT(&broker.msgs_local);
    msg_add(&broker, MSG_TYP_CC | MSG_NOFIN, "Hello");
    msg_add(&broker, MSG_TYP_CC | MSG_NOFIN, ", World");
    msg_add(&broker, MSG_TYP_CC, "!");
    msg_add(&broker, MSG_TYP_CC, "Hello, World!");
    msg_flush_local(&broker);

    return 0;
}
