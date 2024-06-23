#ifndef _CHAT_H

#include <ctype.h>
#include <getopt.h>
#include <search.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/queue.h>

/***********************************
 * Room mates, Rooms & Connections
 ***********************************/
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

/***********************
 * Message Broker
 ***********************/

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
msg_set_active_conn(msg_broker_t *broker, conn_t *conn);
static int
msg_add(msg_broker_t *broker, int options, const char * format, ... );
static void
msg_flush_local(msg_broker_t *broker);

/***************************
 * State of the process
 ***************************/
typedef enum workmode_s {
    WORKMODE_SRV,
    WORKMODE_CLIADM,
    WORKMODE_CLIMATE
} workmode_t;
#define WORKMODE_CLI(MODE) ((MODE == WORKMODE_CLIADM) || (MODE == WORKMODE_CLIMATE))

typedef struct state_s {
    workmode_t      workmode;
    struct in_addr  net_addr;
    int             net_port;
    char           *adm_name;
    char           *adm_pass;
    msg_broker_t    msg_broker;

    roommates_t    *mates;
    rooms_t        *rooms;
    conns_t        *conns;
} state_t;

/**************************************
 * configure with admin line parser
 * configure with command line options
 **************************************/
typedef enum cfg_objtype_e {
    CFG_OBJ_NM,
    CFG_OBJ_NME
} cfg_objtype_t;

typedef struct cfg_obj_s {
    char   *name;
    size_t  name_sz;
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
cfg_admline_parse(char *cmdline, state_t *state, bool *quit);
static int
cfg_cmdline_parse(int argc, char **argv, state_t *state,  bool *helpshow);

#endif
