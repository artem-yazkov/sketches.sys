#include "chat.h"

/***********************************************
 * Message Broker
 ***********************************************/

msg_broker_t *global_broker;

static void
msg_set_active_conn(msg_broker_t *broker, conn_t *conn)
{
    broker = broker ? broker : global_broker;
    broker->active_conn = conn;
}

static void
msg_set_global_broker(msg_broker_t *broker)
{
    global_broker = broker;
}

static int
msg_add(msg_broker_t *broker, int options, const char * format, ... )
{
    broker = broker ? broker : global_broker;

    /* add new message to the list, or update data on last message */
    msg_t *msg = NULL;
    if (CIRCLEQ_EMPTY(&broker->ml_pool) || CIRCLEQ_LAST(&broker->ml_pool)->ready) {
        msg = calloc(1, sizeof(msg_t));
        if (!(msg = calloc(1, sizeof(msg_t)))) {
            goto error;
        }
        CIRCLEQ_INSERT_TAIL(&broker->ml_pool, msg, cq_entry);
    } else {
        msg = CIRCLEQ_LAST(&broker->ml_pool);
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
            CIRCLEQ_INSERT_TAIL(&broker->ml_local, msg, cq_entry);
        }
    }
    return 0;

error:
    if (msg) {
        CIRCLEQ_REMOVE(&broker->ml_pool, msg, cq_entry);
        free(msg->data);
        free(msg);
    }
    {
        fprintf(stderr, "can't handle the message: ");
        va_list args;
        va_start(args, format);
        char *chunk = NULL;
        int   chunk_sz = vfprintf (stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
    return -1;
}

static void
msg_flush_local(msg_broker_t *broker)
{
    broker = broker ? broker : global_broker;
    msg_t *mfirst = CIRCLEQ_FIRST(&broker->ml_local);
    for (; mfirst != (void *)&broker->ml_local; ) {
        fprintf(stderr, "  '%.*s'\n", (int)mfirst->data_sz, mfirst->data);
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
    if (!cfgmate->val_sz || !cfgmate->ext_sz) {
        return -1;
    }
    if ((*mate = calloc(1, sizeof(roommate_t))) == NULL) {
        goto error;
    }
    if (((*mate)->name = strndup(cfgmate->val, cfgmate->val_sz)) == NULL) {
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
        if (!cmate->val_sz || !cmate->ext_sz) {
            msg_add(NULL, MSG_TYP_LE,
                    "Room Mate name and password must be set. Ignore object '%.*s:%.*s'",
                    (int)(cmate->val_sz), cmate->val,
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
                msg_add(NULL, MSG_TYP_LE,  "Room Mate %s exists. Can't create new one with same name");
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
            .name = strndup(cmate->val, cmate->val_sz)
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
        msg_add(NULL, MSG_TYP_LE, "can't create new room '%.*s'", (int)room_name_sz, room_name);
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
        if (cmate->val_sz == 1 && cmate->val[0] == '*') {
            room->is_open = true;
            continue;
        }
        roommate_t kmate = {
            .name = strndup(cmate->val, cmate->val_sz)
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
        if (cmate->val_sz == 1 && cmate->val[0] == '*') {
            troom->is_open = false;
            continue;
        }
        roommate_t kmate = {
            .name = strndup(cmate->val, cmate->val_sz)
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

static int
state_init(state_t *state)
{
    CIRCLEQ_INIT(&state->msg_broker.ml_pool);
    CIRCLEQ_INIT(&state->msg_broker.ml_local);
    return 0;
}

static void
state_free(state_t *state)
{
    return;
}

/**************************
 * configuration handling
 **************************/
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

        if (objtype == CFG_OBJ_VE) {
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
            obj->val = &objstring[name_b];
            obj->val_sz = name_e - name_b;
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
cfg_admline_parse(char *cmdline, state_t *state, bool *quit)
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

    if (state->workmode == WORKMODE_CLIADM) {
        if (strcmp(command, ":roommates") == 0) {
            char *subcmd = strtok_r(NULL, " ", &cmdline_sptr);

            if (subcmd && (strcmp(subcmd, "add") == 0) && cmdline_sptr) {
                cfg_objlist_t col_mates;
                LIST_INIT(&col_mates);
                cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_VE);

                if (!LIST_EMPTY(&col_mates)) {
                    roommates_add(&state->mates, &col_mates);
                    cfg_objlist_clear(&col_mates);
                }
            } else if (subcmd && (strcmp(subcmd, "del") == 0) && cmdline_sptr) {
                cfg_objlist_t col_mates;
                LIST_INIT(&col_mates);
                cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_VE);

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
                    cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_VE);

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
                    cfg_objstring_parse(cmdline_sptr, strlen(cmdline_sptr), &col_mates, CFG_OBJ_VE);

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

static int
cfg_cmdline_parse(int argc, char **argv, state_t *state, bool *helpshow)
{
    int   retcode = 0;
    char *shortopts = "s:a:m:R:c:L:l:r:h";
    struct option longopts[] = {
            {"server",    required_argument, NULL, 's'},
            {"admin",     required_argument, NULL, 'a'},
            {"roommates", required_argument, NULL, 'm'},
            {"rooms",     required_argument, NULL, 'R'},

            {"connect",   required_argument, NULL, 'c'},
            {"logadm",    required_argument, NULL, 'L'},
            {"logmate",   required_argument, NULL, 'l'},
            {"room",      required_argument, NULL, 'r'},

            {"help",      no_argument,       NULL, 'h'},
            {NULL,        0,                 NULL,  0}
    };
    struct valopts_s {
        char    *server;
        char    *admin;
        char   **roommates;
        size_t   roommates_cn;
        size_t   roommates_sz;
        char   **rooms;
        size_t   rooms_cn;
        size_t   rooms_sz;

        char    *connect;
        char    *logadm;
        char    *logmate;
        char    *room;

        bool     help;
    } valopts = {0};

    int   opt;
    while ((opt = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
        switch(opt) {
        case 's':
            valopts.server = strdup(optarg);
            break;
        case 'a':
            valopts.admin = strdup(optarg);
            break;
        case 'm':
            valopts.roommates_sz = (valopts.roommates_sz == valopts.roommates_cn) ? valopts.roommates_sz * 2 + 1 : valopts.roommates_sz;
            valopts.roommates = realloc(valopts.roommates, valopts.roommates_sz * sizeof(valopts.roommates[0]));
            valopts.roommates[valopts.roommates_cn++] = optarg;
            break;
        case 'R':
            valopts.rooms_sz = (valopts.rooms_sz == valopts.rooms_cn) ? valopts.rooms_sz * 2 + 1 : valopts.rooms_sz;
            valopts.rooms = realloc(valopts.rooms, valopts.rooms_sz * sizeof(valopts.rooms[0]));
            valopts.rooms[valopts.rooms_cn++] = optarg;
            break;
        case 'c':
            valopts.connect = strdup(optarg);
            break;
        case 'L':
            valopts.logadm = strdup(optarg);
            break;
        case 'l':
            valopts.logmate = strdup(optarg);
            break;
        case 'r':
            valopts.room = strdup(optarg);
            break;
        case 'h':
            valopts.help = true;
            break;
        default:
            msg_add(NULL, MSG_TYP_LE, "unexpected option was found");
        }
    }
    if (argc == 2 && valopts.help) {
        *helpshow = 1;
        retcode = 1;
    } else {
        *helpshow = 0;
    }

    /* Validate Option Combinations */

    if (!valopts.server && !valopts.connect) {
        msg_add(NULL, MSG_TYP_LE, "either --server OR --connect option must be set");
        retcode = -1;
    } else if (valopts.server && valopts.connect) {
        msg_add(NULL, MSG_TYP_LE, "can't set --server AND --connect options simultaneously");
        retcode = -1;
    }

    if (!retcode && valopts.server && ( valopts.logadm || valopts.logmate || valopts.room)) {
        msg_add(NULL, MSG_TYP_LE, "incorrect command line options combination");
        retcode = -1;
    }
    if (!retcode && valopts.connect && (valopts.admin || valopts.roommates || valopts.rooms)) {
        msg_add(NULL, MSG_TYP_LE, "incorrect command line options combination");
        retcode = -1;
    }

    if (!retcode && valopts.server && !valopts.admin) {
        msg_add(NULL, MSG_TYP_LE, "--admin option must be set for server mode");
        retcode = -1;
    }
    if (!retcode && valopts.connect && (!valopts.logadm && !valopts.logmate)) {
        msg_add(NULL, MSG_TYP_LE, "either --logadm OR --logmate option must be set for client mode");
        retcode = -1;
    }
    if (!retcode && valopts.connect && valopts.logadm && valopts.logmate) {
        msg_add(NULL, MSG_TYP_LE, "can't set --logadm AND --logmate options simultaneously");
        retcode = -1;
    }

    /* determine WORKMODE */
    if (!retcode) {
        if (valopts.server) {
            state->workmode = WORKMODE_SRV;
        } else if (valopts.logadm) {
            state->workmode = WORKMODE_CLIADM;
        } else if (valopts.logmate) {
            state->workmode = WORKMODE_CLIMATE;
        } else {
            msg_add(NULL, MSG_TYP_LE, "unexpected command line parsing state");
            retcode = -1;
        }
    }

    cfg_objlist_t netpair_ol  = {0};
    cfg_objlist_t credpair_ol = {0};
    LIST_INIT(&netpair_ol);
    LIST_INIT(&credpair_ol);

    /* validate listen/connection address & port */
    if (!retcode) {
        char *netpair_s = valopts.server ? valopts.server : valopts.connect;
        if ((cfg_objstring_parse(netpair_s, strlen(netpair_s), &netpair_ol, CFG_OBJ_VE) < 0)
            || LIST_EMPTY(&netpair_ol)
            || !((cfg_obj_t *)LIST_FIRST(&netpair_ol))->val_sz
            || !((cfg_obj_t *)LIST_FIRST(&netpair_ol))->ext_sz
           ) {
            msg_add(NULL, MSG_TYP_LE, "unexpected value of --server/--connect option");
            retcode = -1;
            goto finalize;
        }
        char *host = ((cfg_obj_t *)LIST_FIRST(&netpair_ol))->val;
        char *port = ((cfg_obj_t *)LIST_FIRST(&netpair_ol))->ext;
        host[((cfg_obj_t *)LIST_FIRST(&netpair_ol))->val_sz] = '\0';

        if (!inet_aton(host, &state->net_addr) || !(state->net_port = atoi(port))) {
            msg_add(NULL, MSG_TYP_LE, "unexpected value of --server/--connect option");
            retcode = -1;
            goto finalize;
        }
    }

    /* validate admin/logadm/logmate name & password */
    if (!retcode) {
        char *credpair_s = valopts.admin ? valopts.admin : (valopts.logadm ? valopts.logadm : valopts.logmate);

        if ((cfg_objstring_parse(credpair_s, strlen(credpair_s), &credpair_ol, valopts.logmate ? CFG_OBJ_VE : CFG_OBJ_V) < 0)
            || LIST_EMPTY(&credpair_ol)
            || (!((cfg_obj_t *)LIST_FIRST(&credpair_ol))->val_sz)
            || (!((cfg_obj_t *)LIST_FIRST(&credpair_ol))->ext_sz && valopts.logmate)
           ) {
            msg_add(NULL, MSG_TYP_LE, "unexpected value of --admin/--logadm/--logmate option");
            retcode = -1;
            goto finalize;
        }
        cfg_obj_t *cobj = (cfg_obj_t *)LIST_FIRST(&netpair_ol);
        char      *name, *pass;
        if (valopts.logmate) {
            name = cobj->val;
            pass = cobj->ext;
            name[cobj->val_sz] = '\0';
        } else {
            name = NULL;
            pass = name = cobj->val;
        }

        if (state->workmode == WORKMODE_SRV) {
            state->admin.passwd = strdup(pass);
        } else if (state->workmode == WORKMODE_CLIADM) {
            retcode = msg_add(&state->msg_broker, MSG_TYP_CC, ":logadm %s", pass);
        } else if (state->workmode == WORKMODE_CLIMATE) {
            retcode = msg_add(&state->msg_broker, MSG_TYP_CC, ":logmate %s %s", name, pass);
        }
    }

    /* predefined room */
    if (!retcode && valopts.room) {
        retcode = msg_add(&state->msg_broker, MSG_TYP_CC, ":enter %s", valopts.room);
    }

    /* predefined mates */
    if (!retcode && valopts.roommates) {
        for (size_t i = 0; i < valopts.roommates_cn; i++) {
            cfg_objlist_t mates;
            LIST_INIT(&mates);
            cfg_objstring_parse(valopts.roommates[i], strlen(valopts.roommates[i]), &mates, CFG_OBJ_VE);
            if (!retcode) {
                retcode = roommates_add(&state->mates, &mates);
            }
            cfg_objlist_clear(&mates);
        }
    }

    /* predefined rooms */
    if (!retcode && valopts.rooms) {
        for (size_t i = 0; i < valopts.rooms_cn; i++) {
            char *delim = strchr(valopts.rooms[i], '@');
            if (delim) {
                size_t mates_b = 0;
                size_t mates_e = delim - valopts.rooms[i];
                size_t rooms_b = mates_e + 1;
                size_t rooms_e = strlen(valopts.rooms[i]);

                cfg_obj_t    *mate, *room;
                cfg_objlist_t mates, rooms;
                LIST_INIT(&mates);
                LIST_INIT(&rooms);

                cfg_objstring_parse(&valopts.rooms[i][mates_b], (mates_e - mates_b), &mates, CFG_OBJ_V);
                cfg_objstring_parse(&valopts.rooms[i][rooms_b], (rooms_e - rooms_b), &rooms, CFG_OBJ_V);
                LIST_FOREACH(room, &rooms, lentry) {
                    if (!retcode) {
                        retcode = room_add_mates(&state->rooms, &state->mates, room->val, room->val_sz, &mates);
                    }
                }
                cfg_objlist_clear(&mates);
                cfg_objlist_clear(&rooms);
            }
        }
    }

finalize:
    cfg_objlist_clear(&netpair_ol);
    cfg_objlist_clear(&credpair_ol);

    free(valopts.server);
    free(valopts.admin);
    free(valopts.roommates);
    free(valopts.rooms);

    free(valopts.connect);
    free(valopts.logadm);
    free(valopts.logmate);
    free(valopts.room);

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
            cfg_admline_parse(line, &state, &fquit);
        }
        free(line);
    }
    roommates_clear(&state.mates);
    rooms_clear(&state.rooms);
#endif
#if 0
    msg_broker_t broker = {0};
    CIRCLEQ_INIT(&broker.ml_pool);
    CIRCLEQ_INIT(&broker.ml_local);
    msg_add(&broker, MSG_TYP_CC | MSG_NOFIN, "Hello");
    msg_add(&broker, MSG_TYP_CC | MSG_NOFIN, ", World");
    msg_add(&broker, MSG_TYP_CC, "!");
    msg_add(&broker, MSG_TYP_CC, "Hello, World!");
    msg_flush_local(&broker);
    //getc
#endif

    state_t state = {0};
    bool    helpshow = 0;
    int    parsecode = 0;
    state_init(&state);
    msg_set_global_broker(&state.msg_broker);

    parsecode = cfg_cmdline_parse(argc, argv, &state, &helpshow);
    printf("helpshow: %i\n", helpshow);
    if (parsecode == 0) {
        printf("workmode: %s\n",
                (state.workmode == WORKMODE_SRV ? "WORKMODE_SRV" : (state.workmode == WORKMODE_CLIADM ? "WORKMODE_CLIADM" : "WORKMODE_CLIMATE")));
        //state.net_addr
        //state.net_port

    } else {
        msg_flush_local(NULL);
        printf("Do the next command to show available commands:\n\t %s --help\n", argv[0]);
    }
    state_free(&state);

    return parsecode < 0 ? 1 : 0;
}
