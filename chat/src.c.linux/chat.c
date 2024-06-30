#include "chat.h"

/***********************************************
 * Message Broker
 ***********************************************/

static int
msg_add_bin(msg_t *msg, char *data, size_t size)
{
    if (!msg) {
        return -1;
    }
    uint16_t cursor = msg->hdr.len;
    msg->hdr.len += (size + msg->hdr.len > UINT16_MAX) ? UINT16_MAX : ((uint16_t)size + msg->hdr.len);
    if (msg->data_sz < msg->hdr.len) {
        msg->data_sz = msg->hdr.len;
        msg->data = realloc(msg->data, msg->data_sz);
    }
    if (msg->data_sz && !msg->data) {
        goto error;
    }
    memcpy(&msg->data[cursor], data, msg->hdr.len - cursor);

    return 0;
error:
    free(msg->data);
    memset(msg, 0, sizeof(*msg));
    return -1;
}

static int
msg_add_fmt(msg_t *msg, const char * format, ...)
{
    va_list args;
    va_start(args, format);
    int rc = msg_add_va(msg, format, args);
    va_end(args);

    return rc;
}

static int
msg_add_va(msg_t *msg, const char * format, va_list args)
{
    if (!msg) {
        return -1;
    }
    int size = vsnprintf (NULL, 0, format, args);
    if (size < 0) {
        goto error;
    }

    uint16_t cursor = msg->hdr.len;
    msg->hdr.len += (size + msg->hdr.len > UINT16_MAX) ? UINT16_MAX : ((uint16_t)size + msg->hdr.len);
    if (msg->data_sz < msg->hdr.len) {
        msg->data_sz = msg->hdr.len;
        msg->data = realloc(msg->data, msg->data_sz);
    }
    if (msg->data_sz && !msg->data) {
        goto error;
    }

    vsnprintf (&msg->data[cursor], msg->hdr.len - cursor, format, args);
    return 0;

error:
    free(msg->data);
    memset(msg, 0, sizeof(*msg));
    return -1;
}

static int
msg_io_read(msg_t *msg, int fd, size_t *cursor)
{
    char  *buffer;
    size_t expected;

    if (*cursor < sizeof(msg->hdr)) {
        buffer = (char *)&msg->hdr + *cursor;
        expected = sizeof(msg->hdr) - *cursor;
    } else {
        if (msg->data_sz < msg->hdr.len) {
            msg->data_sz = msg->hdr.len;
            msg->data = realloc(msg->data, msg->data_sz);
            if (!msg->data) {
                errno = ENOMEM;
                return MSG_IO_ERR;
            }
        }
        buffer = msg->data + *cursor - sizeof(msg->hdr);
        expected = msg->hdr.len - (*cursor - sizeof(msg->hdr));
    }

    int rc = recv(fd, buffer, expected, 0);
    *cursor += (rc > 0 ? rc : 0);

    if (rc < expected) {
        return (rc == -1) ? MSG_IO_ERR : ( (rc == 0) ? MSG_IO_DOWN : MSG_IO_AGAIN);
    }
    if (*cursor == sizeof(msg->hdr)) {
        rc = msg_io_read(msg, fd, cursor);
        if (rc < expected) {
            return (rc == -1) ? MSG_IO_ERR : ( (rc == 0) ? MSG_IO_DOWN : MSG_IO_AGAIN);
        }
    }
    return MSG_IO_OK;
}

static int
msg_io_write(msg_t *msg, int fd, size_t *cursor)
{
    char  *buffer;
    size_t expected;

    if (*cursor < sizeof(msg->hdr)) {
        buffer = (char *)&msg->hdr + *cursor;
        expected = sizeof(msg->hdr) - *cursor;
    } else  {
        buffer = msg->data + *cursor - sizeof(msg->hdr);
        expected = msg->hdr.len - (*cursor - sizeof(msg->hdr));
    }

    int rc = send(fd, buffer, expected, 0);
    *cursor += (rc > 0 ? rc : 0);

    if (rc < expected) {
        return (rc == -1) ? MSG_IO_ERR : ( (rc == 0) ? MSG_IO_DOWN : MSG_IO_AGAIN);
    }
    if (*cursor == sizeof(msg->hdr)) {
        rc = msg_io_write(msg, fd, cursor);
        if (rc < expected) {
            return (rc == -1) ? MSG_IO_ERR : ( (rc == 0) ? MSG_IO_DOWN : MSG_IO_AGAIN);
        }
    }
    return MSG_IO_OK;
}

static int
mbr_add_logi(msg_broker_t *broker, const char * format, ...)
{
    msg_t  *msg  = mbr_grow(broker, MSG_TYP_LI | MSG_COMMIT, NULL);
    if (!msg) {
        return -1;
    }

    va_list args;
    va_start(args, format);
    int rc = msg_add_va(msg, format, args);
    va_end(args);

    return 0;
}

static int
mbr_add_loge(msg_broker_t *broker, const char * format, ...)
{
    msg_t  *msg  = mbr_grow(broker, MSG_TYP_LE | MSG_COMMIT, NULL);
    if (!msg) {
        return -1;
    }

    va_list args;
    va_start(args, format);
    int rc = msg_add_va(msg, format, args);
    va_end(args);

    if (errno != 0) {
        msg_add_fmt(msg, " (%d: %s)", errno, strerror(errno));
    }
    return 0;
}

static msg_t *
mbr_grow(msg_broker_t *broker, uint16_t options, conn_t *conn)
{
    msg_t  *msg  = NULL;

    if (CIRCLEQ_EMPTY(&broker->ml_pool) || CIRCLEQ_LAST(&broker->ml_pool)->commit) {
        if (!(msg = calloc(1, sizeof(msg_t)))) {
            goto error;
        }
        CIRCLEQ_INSERT_TAIL(&broker->ml_pool, msg, cq_entry);
    } else {
        msg = CIRCLEQ_LAST(&broker->ml_pool);
    }
    msg->hdr.ops = options;
    msg->commit = options & MSG_COMMIT;

    if (msg->commit && (options & (MSG_TYP_LI | MSG_TYP_LE) )) {
        msgp_t *msgp = calloc(1, sizeof(msgp_t));
        if (msgp) {
            msgp->msg = msg;
            CIRCLEQ_INSERT_TAIL(&broker->mpl_local, msgp, cq_entry);
        }
    }

    return msg;

error:
    if (msg) {
        CIRCLEQ_REMOVE(&broker->ml_pool, msg, cq_entry);
        free(msg->data);
        free(msg);
    }
    return NULL;
}

static void
mbr_flush_locals(msg_broker_t *broker)
{
    while (!CIRCLEQ_EMPTY(&broker->mpl_local)) {
        msgp_t *msgp = CIRCLEQ_FIRST(&broker->mpl_local);
        char *sf_long = msgp->msg->hdr.ops & MSG_TYP_LE ? "EEE\n" : "iii\n";
        char *sf_shrt = msgp->msg->hdr.ops & MSG_TYP_LE ? "E  "   : "i  ";

        fprintf(stderr, "%s%s\n", strchr(msgp->msg->data, '\n') ? sf_long : sf_shrt, msgp->msg->data);
        CIRCLEQ_REMOVE(&broker->ml_pool, msgp->msg, cq_entry);
        CIRCLEQ_REMOVE(&broker->mpl_local, msgp, cq_entry);
        free(msgp->msg->data);
        free(msgp->msg);
        free(msgp);
    }
}

static void
mbr_clean(msg_broker_t *broker)
{
    msg_t *msg;
    CIRCLEQ_FOREACH(msg, &broker->ml_pool, cq_entry) {
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
rooms_compar(const void *room_l, const void *room_r)
{
    return strcmp(((room_t *)room_l)->name, ((room_t *)room_r)->name);
}

static int
conns_compar(const void *conn_l, const void *conn_r)
{
    if (((conn_t *)conn_l)->fd > ((conn_t *)conn_r)->fd) {
        return 1;
    } else if (((conn_t *)conn_l)->fd < ((conn_t *)conn_r)->fd) {
        return -1;
    } else {
        return 0;
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
//            msg_add(
//                    NULL, MSG_TYP_LE,
//                    "Room Mate name and password must be set. Ignore object '%.*s:%.*s'",
//                    (int)(cmate->val_sz), cmate->val,
//                    (int)(cmate->ext_sz), cmate->ext);
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
//                msg_add(NULL, MSG_TYP_LE,  "Room Mate %s exists. Can't create new one with same name");
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
//        msg_add(NULL, MSG_TYP_LE, "can't create new room '%.*s'", (int)room_name_sz, room_name);
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

/**************************
 * State of the process
 **************************/
static int
state_init(state_t *state)
{
    *state = (state_t){0};
    CIRCLEQ_INIT(&state->mbroker.ml_pool);
    CIRCLEQ_INIT(&state->mbroker.mpl_local);
    return 0;
}

static void
state_free(state_t *state)
{
    return;
}

static void
state_status_mates_wlk_short(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
//        roommate_t *mate = *(roommate_t **) ptr;
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "%s  ", mate->name);
    }
}
static void
state_status_mates_wlk_long(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
//        roommate_t *mate = *(roommate_t **) ptr;
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "  * name: %s, passwd: %s\n", mate->name, mate->passwd);
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "    rooms: ");
//        twalk_r(mate->rooms, state_status_rooms_wlk_short, ctx);
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "\n");
    }
}
static void
state_status_rooms_wlk_short(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
//        room_t *room = *(room_t **) ptr;
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "%s  ", room->name);
    }
}
static void
state_status_rooms_wlk_long(const void *ptr, VISIT order, void *ctx)
{
    if (order == postorder || order == leaf) {
//        room_t *room = *(room_t **) ptr;
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "  * name: %s, open: %s\n", room->name, room->is_open ? "yes" : "no");
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "    roommates: ");
//        twalk_r(room->mates, state_status_mates_wlk_short, ctx);
//        msg_add(NULL, *(int *)ctx | MSG_NOFIN, "\n");
    }
}

static void
state_status_take(state_t *state, int msg_opts)
{
//    msg_add(NULL, msg_opts | MSG_NOFIN, "preset roommates: \n");
    twalk_r(state->mates, state_status_mates_wlk_long, &msg_opts);
//    msg_add(NULL, msg_opts | MSG_NOFIN, "\n");

//    msg_add(NULL, msg_opts | MSG_NOFIN, "preset rooms: \n");
    twalk_r(state->rooms, state_status_rooms_wlk_long, &msg_opts);
//    msg_add(NULL, msg_opts, "");
}

/**************************
 * Network communication
 **************************/

int  signal_quit_flag;

void signal_quit_handler(int signum)
{
    signal_quit_flag = signum;
}

static int
cli_loop(state_t *state)
{
    int c;
    // read(fileno(stdin), &c, 1);
    // printf("characted is: %c\n", c);
    // STDIN_FILENO
    while((c=getchar())!= 'e') {
        putchar(c);
    }
    return 0;
}

static int
srv_loop(state_t *state)
{
    struct sigaction sigact;
    sigact.sa_handler = signal_quit_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;

    sigaction(SIGHUP,  &sigact, NULL);
    sigaction(SIGINT,  &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    /*
     * configure listening socket
     */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        mbr_add_loge(&state->mbroker, "can't create listen socket");
        return -1;
    }
    int sockopt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = state->net_port;
    listen_addr.sin_addr = state->net_addr;

    if (bind(listen_fd, (struct sockaddr *)(&listen_addr), sizeof(listen_addr)) < 0) {
        mbr_add_loge(
                &state->mbroker, "can't bind listen socket to %s:%d",
                inet_ntoa(listen_addr.sin_addr),
                ntohs(listen_addr.sin_port));
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, INT32_MAX) < 0) {
        mbr_add_loge(&state->mbroker, "listen socket error");
        close(listen_fd);
        return -1;
    }

    /*
     * configure event poll
     */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        mbr_add_loge(&state->mbroker, "epoll instance creation error");
        close(listen_fd);
        return -1;
    }
    const  int         EPEV_WPOOL = 16;
    struct epoll_event epev_wpool[EPEV_WPOOL];
    struct epoll_event epev_ctl;

    epev_ctl.data.ptr = NULL;
    epev_ctl.events   = EPOLLIN;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &epev_ctl) < 0) {
        mbr_add_loge(&state->mbroker, "can't add listen socket to epoll");
        close(epoll_fd);
        close(listen_fd);
        return -1;
    }

    for (;;) {
        int epev_cnt = epoll_wait(epoll_fd, epev_wpool, EPEV_WPOOL, -1);
        if (epev_cnt < 0) {
            mbr_add_loge(&state->mbroker, "epoll_wait error");
            break;
        }
        if (signal_quit_flag) {
            mbr_add_loge(&state->mbroker, "interrupted by %d signal", signal_quit_flag);
            break;
        }

        for (int iev = 0; iev < epev_cnt; iev++) {
            if (epev_wpool[iev].data.ptr == NULL) {
                /* got input event from the listen_fd. Establish new connection */
                conn_t *conn = calloc(1, sizeof(conn_t));
                if (!conn) {
                    mbr_add_loge(&state->mbroker, "can't create new client connection");
                    continue;
                }
                conn->fd = accept(listen_fd, (struct sockaddr *)&conn->addr, &conn->addr_len);
                if (conn->fd < 0) {
                    mbr_add_loge(&state->mbroker, "can't accept new client connection");
                    free(conn);
                    continue;
                }
                fcntl(conn->fd, F_SETFL, O_NONBLOCK);
                epev_ctl.data.ptr = conn;
                epev_ctl.events   = EPOLLIN;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->fd, &epev_ctl) < 0) {
                    fprintf(stderr, "can't add client socket to epoll");
                    close(conn->fd);
                    free(conn);
                    continue;
                }
                conn_t *pconn = tsearch(conn, &state->conns, conns_compar);
                if (!pconn || (*(conn_t **)pconn != conn)) {
                    fprintf(stderr, "can't add new connection to the state tree");
                    close(conn->fd);
                    free(conn);
                    continue;
                }

            } else if (epev_wpool[iev].events & EPOLLIN) {
                /* input event from the client connection */
                conn_t *conn = epev_wpool[iev].data.ptr;
                msg_io_read(&conn->msg_in, conn->fd, &conn->cursor_in);
                msg_io_write(&conn->msg_in, conn->fd, &conn->cursor_out);
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);

    return 0;
}

/**************************************
 * configure with admin line parser
 * configure with command line options
 **************************************/
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

    if (state->workmode == WORKMODE_ADM) {
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
                // TODO: show status here
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
            // TODO: show status here
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
            mbr_add_loge(&state->mbroker, "unexpected option was found");
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
        mbr_add_loge(&state->mbroker, "either --server OR --connect option must be set");
        retcode = -1;
    } else if (valopts.server && valopts.connect) {
        mbr_add_loge(&state->mbroker, "can't set --server AND --connect options simultaneously");
        retcode = -1;
    }

    if (!retcode && valopts.server && ( valopts.logadm || valopts.logmate || valopts.room)) {
        mbr_add_loge(&state->mbroker, "incorrect command line options combination");
        retcode = -1;
    }
    if (!retcode && valopts.connect && (valopts.admin || valopts.roommates || valopts.rooms)) {
        mbr_add_loge(&state->mbroker, "incorrect command line options combination");
        retcode = -1;
    }

    if (!retcode && valopts.server && !valopts.admin) {
        mbr_add_loge(&state->mbroker, "--admin option must be set for server mode");
        retcode = -1;
    }
    if (!retcode && valopts.connect && (!valopts.logadm && !valopts.logmate)) {
        mbr_add_loge(&state->mbroker, "either --logadm OR --logmate option must be set for client mode");
        retcode = -1;
    }
    if (!retcode && valopts.connect && valopts.logadm && valopts.logmate) {
        mbr_add_loge(&state->mbroker, "can't set --logadm AND --logmate options simultaneously");
        retcode = -1;
    }

    /* determine WORKMODE */
    if (!retcode) {
        if (valopts.server) {
            state->workmode = WORKMODE_SRV;
        } else if (valopts.logadm) {
            state->workmode = WORKMODE_ADM;
        } else if (valopts.logmate) {
            state->workmode = WORKMODE_MATE;
        } else {
            mbr_add_loge(&state->mbroker, "unexpected command line parsing state");
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
            mbr_add_loge(&state->mbroker, "unexpected value of --server/--connect option");
            retcode = -1;
            goto finalize;
        }
        char *host = ((cfg_obj_t *)LIST_FIRST(&netpair_ol))->val;
        char *port = ((cfg_obj_t *)LIST_FIRST(&netpair_ol))->ext;
        host[((cfg_obj_t *)LIST_FIRST(&netpair_ol))->val_sz] = '\0';

        if (!inet_aton(host, &state->net_addr) || (atoi(port) <= 0) || (atoi(port) > UINT16_MAX)) {
            mbr_add_loge(&state->mbroker, "unexpected value of --server/--connect option");
            retcode = -1;
            goto finalize;
        }
        state->net_port = htons((uint16_t)atoi(port));
    }

    /* validate admin/logadm/logmate name & password */
    if (!retcode) {
        char *credpair_s = valopts.admin ? valopts.admin : (valopts.logadm ? valopts.logadm : valopts.logmate);

        if ((cfg_objstring_parse(credpair_s, strlen(credpair_s), &credpair_ol, valopts.logmate ? CFG_OBJ_VE : CFG_OBJ_V) < 0)
            || LIST_EMPTY(&credpair_ol)
            || (!((cfg_obj_t *)LIST_FIRST(&credpair_ol))->val_sz)
            || (!((cfg_obj_t *)LIST_FIRST(&credpair_ol))->ext_sz && valopts.logmate)
           ) {
            mbr_add_loge(&state->mbroker, "unexpected value of --admin/--logadm/--logmate option");
            retcode = -1;
            goto finalize;
        }
        cfg_obj_t *cobj = (cfg_obj_t *)LIST_FIRST(&credpair_ol);
        char      *name, *pass;
        if (valopts.logmate) {
            name = cobj->val;
            pass = cobj->ext;
            name[cobj->val_sz] = '\0';
        } else {
            name = NULL;
            pass = cobj->val;
        }

        if (state->workmode == WORKMODE_SRV) {
            state->admin.passwd = strdup(pass);
        } else if (state->workmode == WORKMODE_ADM) {
//            retcode = msg_add(&state->msg_broker, MSG_TYP_CC, ":logadm %s", pass);
        } else if (state->workmode == WORKMODE_MATE) {
//            retcode = msg_add(&state->msg_broker, MSG_TYP_CC, ":logmate %s %s", name, pass);
        }
    }

    /* predefined room */
    if (!retcode && valopts.room) {
//        retcode = msg_add(&state->msg_broker, MSG_TYP_CC, ":enter %s", valopts.room);
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
    state_t state = {0};
    bool    helpshow = 0;
    int    parsecode = 0;
    state_init(&state);
    msg_set_global_broker(&state.mbroker);

    parsecode = cfg_cmdline_parse(argc, argv, &state, &helpshow);
    printf("helpshow: %i\n", helpshow);
    if (parsecode == 0) {
        char saddr[INET_ADDRSTRLEN] = {0};
        printf("workmode: %s\n",
                (state.workmode == WORKMODE_SRV ? "WORKMODE_SRV" : (state.workmode == WORKMODE_CLIADM ? "WORKMODE_CLIADM" : "WORKMODE_CLIMATE")));
        printf("netpoint: %s:%d\n", inet_ntop(AF_INET, &state.net_addr, saddr, sizeof(saddr)), state.net_port);
        if (state.workmode == WORKMODE_SRV) {
            printf("admin.passwd: %s\n", state.admin.passwd);
            state_status_take(&state, MSG_TYP_LI);
            msg_flush_local(&state.mbroker);
        } else {
            printf("message broker:\n");
            msg_dump(&state.mbroker);
        }
    } else {
        msg_flush_local(&state.mbroker);
        printf("Do the next command to show available commands:\n\t %s --help\n", argv[0]);
    }
    state_free(&state);

    return parsecode < 0 ? 1 : 0;
#endif

    state_t state;
    state_init(&state);

    bool helpshow = false;
    if (cfg_cmdline_parse(argc, argv, &state, &helpshow) < 0) {
        mbr_flush_locals(&state.mbroker);
    }

    srv_loop(&state);

    state_free(&state);
    return 0;
}
