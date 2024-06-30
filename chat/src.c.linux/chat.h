#ifndef _CHAT_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <search.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/queue.h>

/***********************
 * Message Broker
 ***********************/
typedef struct conn_s conn_t;

/* Message Types */
#define MSG_TYP_CM      (0x1)   /* Chat Message, from Client to Client      */
#define MSG_TYP_CC      (0x2)   /* Client Command, from Client to Server    */
#define MSG_TYP_SC      (0x3)   /* Server Command, from Server to Client    */
#define MSG_TYP_SI      (0x4)   /* Server Info Message, Server -> Client    */
#define MSG_TYP_SE      (0x5)   /* Server Error Message, Server -> Client   */
#define MSG_TYP_LI      (0x6)   /* Server Info Message, Server -> Local     */
#define MSG_TYP_LE      (0x7)   /* Server Error Message, Server -> Local    */
#define MSG_TYP_MASK(X) (X & 0x0F)

/* Message broadcast width */
#define MSG_WID_AC      (0x1 << 4)  /* to Active Connection only */
#define MSG_WID_MT      (0x2 << 4)  /* to all Room Mate connections, except Active Connection */
#define MSG_WID_MTA     (0x3 << 4)  /* to all Room Mate connections, including Active Connection */
#define MSG_WID_RM      (0x4 << 4)  /* to all Room connections, except Active Connection */
#define MSG_WID_RMA     (0x5 << 4)  /* to all Room connections, including Active Connection */
#define MSG_WID_MASK(X) (X & 0xF0)

#define MSG_COMMIT      (0x1 << 8)  /*    */
#define MSG_NET_FIN     (0x2 << 8)  /* disconnect client when sent the message */

typedef struct msg_s {
    struct  msg_hdr_s {
        uint16_t ops;
        uint16_t len;
    } hdr;
    char   *data;
    size_t  data_sz;
    bool    commit;
    int     refs;
    CIRCLEQ_ENTRY(msg_s) cq_entry;
} msg_t;
typedef CIRCLEQ_HEAD(msg_list_s, msg_s) msg_list_t;

typedef struct msgp_s {
    msg_t *msg;
    CIRCLEQ_ENTRY(msgp_s) cq_entry;
} msgp_t;
typedef CIRCLEQ_HEAD(msgp_list_s, msgp_s) msgp_list_t;

typedef struct msg_broker_s {
    msg_list_t  ml_pool;
    msgp_list_t mpl_local;
} msg_broker_t;

#define MSG_IO_AGAIN  ( 1)
#define MSG_IO_DOWN   (-2)
#define MSG_IO_ERR    (-1)
#define MSG_IO_OK     ( 0)

static int
msg_add_bin(msg_t *msg, char *data, size_t size);
static int
msg_add_fmt(msg_t *msg, const char * format, ...);
static int
msg_add_va(msg_t *msg, const char * format, va_list args);

static int
msg_io_read(msg_t *msg, int fd, size_t *cursor);
static int
msg_io_write(msg_t *msg, int fd, size_t *cursor);

static int
mbr_add_logi(msg_broker_t *broker, const char * format, ...);
static int
mbr_add_loge(msg_broker_t *broker, const char * format, ...);

static msg_t *
mbr_grow(msg_broker_t *broker, uint16_t options, conn_t *conn);
static void
mbr_flush_locals(msg_broker_t *broker);
static void
mbr_clean(msg_broker_t *broker);

/***********************************
 * Room mates, Rooms & Connections
 ***********************************/
typedef void roommates_t;
typedef void rooms_t;
typedef void conns_t;

typedef struct admin_s {
    char        *passwd;
    conns_t     *conns;
} admin_t;

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
    int                 fd;
    struct sockaddr_in  addr;
    socklen_t           addr_len;

    bool                is_adm;
    roommate_t         *roommate;
    room_t             *room;

    msg_t               msg_in;
    size_t              cursor_in;
    msgp_list_t         mpl_out;
    size_t              cursor_out;
} conn_t;

/***************************
 * State of the process
 ***************************/
typedef enum workmode_s {
    WORKMODE_SRV,
    WORKMODE_ADM,
    WORKMODE_MATE
} workmode_t;
#define WORKMODE_CLI(MODE) ((MODE == WORKMODE_CLIADM) || (MODE == WORKMODE_CLIMATE))

typedef struct state_s {
    workmode_t      workmode;
    struct in_addr  net_addr;
    int             net_port;
    admin_t         admin;
    msg_broker_t    mbroker;

    roommates_t    *mates;
    rooms_t        *rooms;
    conns_t        *conns;
} state_t;

static int
state_init(state_t *state);
static void
state_free(state_t *state);

static void
state_status_mates_wlk_short(const void *ptr, VISIT order, void *ctx);
static void
state_status_mates_wlk_long(const void *ptr, VISIT order, void *ctx);
static void
state_status_rooms_wlk_short(const void *ptr, VISIT order, void *ctx);
static void
state_status_rooms_wlk_long(const void *ptr, VISIT order, void *ctx);
static void
state_status_take(state_t *state, int msg_opts);

/**************************
 * Network communication
 **************************/
static int
cli_loop(state_t *state);
static int
srv_loop(state_t *state);

/**************************************
 * configure with admin line parser
 * configure with command line options
 **************************************/
typedef enum cfg_objtype_e {
    CFG_OBJ_V,
    CFG_OBJ_VE
} cfg_objtype_t;

typedef struct cfg_obj_s {
    char   *val;
    size_t  val_sz;
    char   *ext;
    size_t  ext_sz;
    LIST_ENTRY(cfg_obj_s) lentry;
} cfg_obj_t;
typedef LIST_HEAD(cfg_objlist_s, cfg_obj_s) cfg_objlist_t;

#define CFG_OBJ_OUTER_DELIMS(c) (isspace(c) || c == ',' || c == ';')
#define CFG_OBJ_INNER_DELIMS(c) (c == ':')
static int
cfg_objstring_parse(char *objstring, size_t objstring_sz, cfg_objlist_t *objlist, cfg_objtype_t objtype);
static int
cfg_objlist_clear(cfg_objlist_t *objlist);
static int
cfg_admline_parse(char *cmdline, state_t *state, bool *quit);
static int
cfg_cmdline_parse(int argc, char **argv, state_t *state,  bool *helpshow);

#endif
