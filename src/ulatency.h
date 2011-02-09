/*
    Copyright 2010,2011 ulatencyd developers

    This file is part of ulatencyd.

    ulatencyd is free software: you can redistribute it and/or modify it under 
    the terms of the GNU General Public License as published by the 
    Free Software Foundation, either version 3 of the License, 
    or (at your option) any later version.

    ulatencyd is distributed in the hope that it will be useful, 
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License 
    along with ulatencyd. If not, see http://www.gnu.org/licenses/.
*/

#ifndef __ulatency_h__
#define __ulatency_h__
#include <glib.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <time.h>
#include <stdint.h>
#include "proc/procps.h"
#include "proc/readproc.h"

#ifdef ENABLE_DBUS
#include <dbus/dbus-glib.h>
#endif
//#include <libcgroup.h>


#define G_LOG_LEVEL_TRACE   1 << 8
#define g_trace(...)    g_log (G_LOG_DOMAIN,         \
                               G_LOG_LEVEL_TRACE,    \
                               __VA_ARGS__)

#define VERSION 0.4.1

#define OPENPROC_FLAGS (PROC_FILLMEM | \
  PROC_FILLUSR | PROC_FILLGRP | PROC_FILLSTATUS | PROC_FILLSTAT | \
  PROC_FILLWCHAN | PROC_FILLCGROUP | PROC_FILLSUPGRP | PROC_FILLCGROUP)

#define OPENPROC_FLAGS_MINIMAL (PROC_FILLSTATUS | PROC_FILLCGROUP)


#define CONFIG_CORE "core"

#define U_HEAD \
  guint ref; \
  void (*free_fnk)(void *data);

struct _U_HEAD {
  U_HEAD;
};

enum U_PROC_STATE {
  UPROC_NEW          = (1<<0),
  UPROC_INVALID      = (1<<1),
  UPROC_BASIC        = (1<<2),
  UPROC_ALIVE        = (1<<3),
  UPROC_HAS_PARENT   = (1<<4),
};

#define U_PROC_OK_MASK ~UPROC_INVALID

#define U_PROC_IS_INVALID(P) ( P ->ustate & UPROC_INVALID )
#define U_PROC_IS_VALID(P) ((( P ->ustate & U_PROC_OK_MASK ) & UPROC_INVALID ) == 0)

#define U_PROC_SET_STATE(P,STATE) ( P ->ustate = ( P ->ustate | STATE ))
#define U_PROC_UNSET_STATE(P,STATE) ( P ->ustate = ( P ->ustate & ~STATE ))
#define U_PROC_HAS_STATE(P,STATE) ( ( P ->ustate & STATE ) == STATE )


enum FILTER_TYPES {
  FILTER_LUA,
  FILTER_C
};

enum FILTER_FLAGS {
  FILTER_STOP          = (1<<0),
  FILTER_SKIP_CHILD   = (1<<1),
};

#define FILTER_TIMEOUT(v) ( v & 0xFFFF)
#define FILTER_FLAGS(v) ( v >> 16)
#define FILTER_MIX(flages,timeout) (( flags << 16 ) | timeout )


enum FILTER_PRIORITIES {
  PRIO_IDLE=-1,
};

// default categories for convinience

enum IO_PRIO_CLASS {
  IOPRIO_CLASS_NONE,
  IOPRIO_CLASS_RT,
  IOPRIO_CLASS_BE,
  IOPRIO_CLASS_IDLE,
};

struct lua_callback {
  lua_State *lua_state;
  int lua_state_id;
  int lua_func;
  int lua_data;
};

struct lua_filter {
  lua_State *lua_state;
  int lua_state_id;
  int lua_func;
  int lua_data;
  int filter;
  GRegex *regexp_cmdline;
  GRegex *regexp_basename;
  double min_percent;
};

struct filter_block {
  unsigned int pid;
  GTime timeout;
  gboolean skip;
};


typedef struct _u_proc {
  U_HEAD;
  int           pid; // duplicate of proc.tgid
  int           ustate; // status bits for process
  struct proc_t proc;
  char        **cgroup_origin; // the original cgroups this process was created in
  GArray        proc_history;
  int           history_len;
  guint         last_update; // for detecting dead processes
  GNode         *node; // for parent/child lookups
  GHashTable    *skip_filter;
  GList         *flags;
  int           changed; // flags or main parameters of process like uid, gid, sid
  void          *filter_owner;
  int           block_scheduler; // this should be respected by the scheduler
  int           lua_data;
  // we don't use the libproc parsers here as we do not update these values
  // that often
  char          *cmdfile;
  char          *cmdline_match;
  GHashTable    *environ; // str:str hash table
  GPtrArray     *cmdline;
  char          *exe;

  // fake pgid because it can't be changed.
  pid_t         fake_pgrp;
  pid_t         fake_pgrp_old;
  pid_t         fake_session;
  pid_t         fake_session_old;
} u_proc;

typedef struct _filter {
  U_HEAD;
  enum FILTER_TYPES type;
  char *name;
  int (*precheck)(struct _filter *filter);
  int (*check)(u_proc *pr, struct _filter *filter);
  int (*postcheck)(struct _filter *filter);
  int (*callback)(u_proc *pr, struct _filter *filter);
  int (*exit)(u_proc *pr, struct _filter *filter);
  void *data;
} u_filter;

#define INC_REF(P) P ->ref++;
#define DEC_REF(P) \
 do { struct _U_HEAD *uh = (struct _U_HEAD *) P ; uh->ref--; g_assert(uh->ref >= 0); \
  if( uh->ref == 0 && uh->free_fnk) { uh->free_fnk( P ); P = NULL; }} while(0);

#define FREE_IF_UNREF(P,FNK) if( P ->ref == 0 ) { FNK ( P ); }


#define U_MALLOC(SIZE) g_malloc0(gsize n_bytes);
#define U_FREE(PTR) g_free( PTR );

/*typedef enum {
  NONE = 0,
  REPLACE_SOURCE,
  ADD,
} FLAG_BEHAVIOUR;
*/
typedef struct _FLAG {
  U_HEAD;
  void          *source;       // pointer to a data structure that is the "owner"
//  FLAG_BEHAVIOUR age;
  char          *name;         // label name
  char          *reason;       // why the flag was set. This makes most sense with emergency flags
  time_t         timeout;       // timeout when the flag will disapear
  int32_t        priority;      // custom data: priority
  int64_t        value;         // custom data: value
  int64_t        threshold;     // custom data: threshold
  uint32_t       inherit : 1;      // will apply to all children
} u_flag;


u_flag *u_flag_new(u_filter *source, const char *name);
void u_flag_free(void *data);

int u_flag_add(u_proc *proc, u_flag *flag);
int u_flag_del(u_proc *proc, u_flag *flag);
int u_flag_clear_source(u_proc *proc, void *source);
int u_flag_clear_name(u_proc *proc, const char *name);
int u_flag_clear_all(u_proc *proc);
int u_flag_clear_timeout(u_proc *proc, time_t timeout);

struct u_cgroup {
  struct cgroup *group;
  char *name;
  int ref;
};

struct u_cgroup_controller {
  struct cgroup_controller *controller;
  char *name;
  int ref; // struct 
};


struct user_active_process {
  guint pid;
  time_t last_change;
};

enum USER_ACTIVE_AGENT {
  USER_ACTIVE_AGENT_NONE = 0,
  USER_ACTIVE_AGENT_DISABLED,
  USER_ACTIVE_AGENT_DBUS,
  USER_ACTIVE_AGENT_MODULE=1000,
};

// tracking for user sessions
typedef struct {
  gchar     *name;
  gchar     *X11Display;
  gchar     *X11Device;
  // most likely dbus session
  gchar     *dbus_session;
  uid_t     uid;
  uint32_t  idle;
  uint32_t  active;
#ifdef ENABLE_DBUS
  DBusGProxy *proxy;
#endif
} u_session;

// list of active sessions
extern GList *U_session_list;

struct user_active {
  uid_t uid;
  guint max_processes;
  guint active_agent;    // tracker of the active list
  // FIXME: last change time
  time_t last_change;   // time when the last change happend
  GList *actives;       // list of user_active_process
};


typedef struct {
  int (*all)(void);    // make scheduler run over all processes
  int (*one)(u_proc *);  // schedule for one (new) process
  int (*set_config)(char *name);  // configure the scheduler for using a different configuration
  char *(*get_config)(void);  // returns the name of current config
  GPtrArray *(*list_configs)(void);  // returns a list of valid configs
  char *(*get_config_description)(char *name);
} u_scheduler;


// module prototype
int (*MODULE_INIT)(void);


// global variables
extern GMainLoop *main_loop;
extern GList *filter_list;
extern GKeyFile *config_data;
extern GList* active_users;
extern GHashTable* processes;
extern GNode* processes_tree;
extern lua_State *lua_main_state;
extern GList* system_flags;
extern int    system_flags_changed;
#ifdef ENABLE_DBUS
extern DBusGConnection *U_dbus_connection; // usully the system bus, but may differ on develop mode
extern DBusGConnection *U_dbus_connection_system; // always the system bus
#endif


//extern gchar *load_pattern;

// core.c
int load_modules(char *path);
int load_rule_directory(char *path, char *load_pattern, int fatal);
int load_rule_file(char *name);
int load_lua_rule_file(lua_State *L, const char *name);

/* u_proc* u_proc_new(proc_t proc)
 *
 * Allocates a new u_proc structure.
 *
 * @param proc: optional proc_t to copy data from. Will cause state U_PROC_ALIVE.
 * Returns: new allocated u_proc with refcount 1
 */
u_proc* u_proc_new(proc_t *proc);
void cp_proc_t(const struct proc_t *src,struct proc_t *dst);

static inline u_proc *proc_by_pid(pid_t pid) {
  return g_hash_table_lookup(processes, GUINT_TO_POINTER(pid));
}

enum ENSURE_WHAT {
  BASIC,
  ENVIRONMENT,
  CMDLINE,
  EXE,
};

int u_proc_ensure(u_proc *proc, enum ENSURE_WHAT what, int update);


u_filter *filter_new();
void filter_register(u_filter *filter);
void filter_free(u_filter *filter);
void filter_unregister(u_filter *filter);
void filter_run();
void filter_for_proc(u_proc *proc);

int filter_run_for_proc(gpointer data, gpointer user_data);
void cp_proc_t(const struct proc_t *src, struct proc_t *dst);

// notify system of a new pids/changed/dead pids
int process_new(pid_t pid, int noupdate);
int process_new_lazy(pid_t pid, pid_t parent);
int process_new_list(GArray *list, int noupdate);
int process_remove(u_proc *proc);
int process_remove_by_pid(pid_t pid);
// low level update api
int process_update_pids(pid_t pids[]);
int process_update_pid(pid_t pid);
int process_run_one(u_proc *proc, int update);

int process_update_all();


int scheduler_run_one(u_proc *proc);
int scheduler_run();
u_scheduler *scheduler_get();
int scheduler_set(u_scheduler *scheduler);

int iterate(void *);

int core_init();
void core_unload();

// caches
double get_last_load();
double get_last_percent();

// misc stuff
guint get_plugin_id();


// lua_binding
int l_filter_run_for_proc(u_proc *pr, u_filter *flt);

extern u_scheduler LUA_SCHEDULER;


// sysctrl.c
int ioprio_getpid(pid_t pid, int *ioprio, int *ioclass);
int ioprio_setpid(pid_t pid, int ioprio, int ioclass);
int adj_oom_killer(pid_t pid, int adj);
int get_oom_killer(pid_t pid);

// group.c
void set_active_pid(unsigned int uid, unsigned int pid);
struct user_active* get_userlist(guint uid, gboolean create);
int is_active_pid(u_proc *proc);
int get_active_pos(u_proc *proc);

// sysinfo.c
GHashTable * u_read_env_hash (pid_t pid);
char *       u_pid_get_env (pid_t pid, const char *var);
GPtrArray *  search_user_env(uid_t uid, const char *name, int update);
GPtrArray *  u_read_0file (pid_t pid, const char *what);


// dbus consts
#define U_DBUS_SERVICE_NAME     "org.quamquam.ulatencyd"
#define U_DBUS_USER_PATH        "/org/quamquam/ulatencyd/User"
#define U_DBUS_USER_INTERFACE   "org.quamquam.ulatencyd.User"
#define U_DBUS_SYSTEM_PATH      "/org/quamquam/ulatencyd/System"
#define U_DBUS_SYSTEM_INTERFACE "org.quamquam.ulatencyd.System"

#endif