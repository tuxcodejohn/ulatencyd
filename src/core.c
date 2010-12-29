#include "ulatency.h"

#include "proc/procps.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <glib.h>
#include <stdio.h>
#include <sys/types.h>
#include <dlfcn.h>


lua_State *lua_main_state;
GList *filter_list;
GNode *processes_tree;
GHashTable *processes;
static int iteration;

/*************************************************************
 * u_proc code
 ************************************************************/

void filter_block_free(gpointer fb) {
  free(fb);
}

void u_head_free(gpointer fb) {
  DEC_REF(fb);
}


void u_proc_free(void *ptr) {
  u_proc *proc = ptr;
  DEC_REF(proc);
  if(proc->ref)
    return;
  
  g_hash_table_remove_all (proc->skip_filter);
  g_hash_table_unref(proc->skip_filter);
  g_node_destroy(proc->node);
  free(proc);
}


u_proc* u_proc_new(proc_t *proc) {
  u_proc *rv;
  
  rv = g_new0(u_proc, 1);
  
  rv->free_fnk = u_proc_free;
  rv->ref = 1;
  rv->skip_filter = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, filter_block_free);

  rv->flags = NULL;

  if(proc) {
    rv->pid = proc->tgid;
    U_PROC_SET_STATE(rv,UPROC_ALIVE);
    memcpy(&(rv->proc), proc, sizeof(proc_t));
  } else {
    U_PROC_SET_STATE(rv,UPROC_NEW);
  }

  return rv;
}

void processes_free_value(gpointer data) {
  u_proc *proc = data;
  U_PROC_SET_STATE(proc, UPROC_INVALID);
  g_node_unlink(proc->node);
  DEC_REF(proc);
}

// rebuild the node tree from content of the hash table
void rebuild_tree() {
//  processes_tree
  GHashTableIter iter;
  gpointer key, value;
  u_proc *proc, *parent;

  // clear root node
  g_node_destroy(processes_tree);
  processes_tree = g_node_new(NULL);

  // create nodes first
  g_hash_table_iter_init (&iter, processes);
  while (g_hash_table_iter_next (&iter, &key, &value)) 
  {
    proc = (u_proc *)value;
    proc->node = g_node_new(proc);
    g_node_append(processes_tree, proc->node);
  }

  // now we can lookup the parents and attach the node to the parent
  g_hash_table_iter_init (&iter, processes);
  while (g_hash_table_iter_next (&iter, &key, &value)) 
  {
    proc = (u_proc *)value;
    if(proc->proc.ppid) {
      parent = proc_by_pid(proc->proc.ppid);
      g_assert(parent && parent->node);
      g_node_unlink(proc->node);
      g_node_append(parent->node, proc->node);
    }
  }


}


int update_processes() {
  PROCTAB *proctab;
  proc_t buf;
  u_proc *proc;
  u_proc *parent;
  gboolean full_update = FALSE;

  proctab = openproc(OPENPROC_FLAGS);
  if(!proctab)
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't open /proc");

  while(readproc(proctab, &buf)){
    proc = proc_by_pid(buf.tgid);
    if(proc) {
      // free all changable allocated buffers
      freesupgrp(&buf);
      freeproc_light(&buf);
    } else {
      proc = u_proc_new(&buf);
      g_hash_table_insert(processes, GUINT_TO_POINTER(proc->pid), proc);
    }
    // we can simply steal the pointer of the current allocated buffer
    memcpy(&(proc->proc), &buf, sizeof(proc_t));
    U_PROC_UNSET_STATE(proc, UPROC_NEW);


    if(!proc->node) {
      proc->node = g_node_new(proc);
      if(proc->proc.ppid) {
        parent = g_hash_table_lookup(processes, GUINT_TO_POINTER(proc->proc.ppid));
        // the parent should exist. in case it is missing we have to run a full
        // tree rebuild then
        if(parent) {
          g_node_append(parent->node, proc->node);
        } else {
          full_update = TRUE;
        }
      }
    }
    //g_list_foreach(filter_list, filter_run_for_proc, &buf);
    //freesupgrp(&buf);
  }
  closeproc(proctab);
  if(full_update) {
    rebuild_tree();
  }

}




void u_flag_free(void *ptr) {
  u_flag *flag = ptr;

  g_assert(flag->ref == 0);

  if(flag->name)
    free(flag->name);
  free(flag);
}

u_flag *u_flag_new(u_filter *source, const char *name) {
  u_flag *rv;
  
  rv = malloc(sizeof(u_flag));
  memset(rv, 0, sizeof(u_flag));
  
  rv->free_fnk = u_flag_free;
  rv->ref = 1;
  rv->source = source;

  if(name) {
    rv->name = g_strdup(name);
  }

  return rv;
}

int u_flag_add(u_proc *proc, u_flag *flag) {
  if(!g_list_find(proc->flags, flag)) {
    proc->flags = g_list_insert(proc->flags, flag, 0);
    INC_REF(flag);
    }
  proc->flags_changed = iteration;
}

int u_flag_del(u_proc *proc, u_flag *flag) {
  if(g_list_index(proc->flags, flag) != -1) {
    DEC_REF(flag);
  }
  proc->flags = g_list_remove(proc->flags, flag);
  proc->flags_changed = iteration;
}

static gint u_flag_match_source(gconstpointer a, gconstpointer match) {
  u_flag *flg = (u_flag *)a;

  if(flg->source == match)
    return 0;

  return -1;
}

static int u_flag_match_name(gconstpointer a, gconstpointer name) {
  u_flag *flg = (u_flag *)a;

  return strcmp(flg->name, (char *)name);
}


int u_flag_clear_source(u_proc *proc, void *source) {
  GList *item;
  u_flag *flg;
  while((item = g_list_find_custom(proc->flags, source, u_flag_match_source)) != NULL) {
    flg = (u_flag *)item->data;
    proc->flags = g_list_remove_link (proc->flags, item);
    DEC_REF(item->data);
    item->data = NULL;
    g_list_free(item);
  }
  proc->flags_changed = iteration;
}


int u_flag_clear_name(u_proc *proc, const char *name) {
  GList *item;
  u_flag *flg;
  while((item = g_list_find_custom(proc->flags, name, u_flag_match_name)) != NULL) {
    flg = (u_flag *)item->data;
    proc->flags = g_list_remove_link (proc->flags, item);
    DEC_REF(item->data);
    item->data = NULL;
    g_list_free(item);
  }
  proc->flags_changed = iteration;
}



int u_flag_clear_all(u_proc *proc) {
  GList *item;
  u_flag *flg;
  while((item = g_list_first(proc->flags)) != NULL) {
    flg = (u_flag *)item->data;
    proc->flags = g_list_remove_link (proc->flags, item);
    DEC_REF(item->data);
    item->data = NULL;
    g_list_free(item);
  }
  g_list_free(proc->flags);
  proc->flags_changed = iteration;
}



/*************************************************************
 * filter code
 ************************************************************/

void u_filter_free(void *ptr) {
  // FIXME
}

u_filter* filter_new() {
  u_filter *rv = malloc(sizeof(u_filter));
  memset(rv, 0, sizeof(u_filter));
  rv->free_fnk = u_filter_free;
  return rv;
}

void filter_register(u_filter *filter) {
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "register new filter: %s", filter->name ? filter->name : "unknown");
  filter_list = g_list_append(filter_list, filter);
}


int filter_run_for_proc(gpointer data, gpointer user_data) {
  u_proc *proc = data;
  u_filter *flt = user_data;
  struct filter_block *flt_block =NULL;
  int rv = 0;
  time_t ttime = 0;
  int timeout, flags;

  printf("filter for proc %p\n", flt);

  g_assert(data);

  flt_block = (struct filter_block *)g_hash_table_lookup(proc->skip_filter, GUINT_TO_POINTER(flt));

  //g_hash_table_lookup
  if(flt_block) {
    time (&ttime);
    if(flt_block->skip)
      return 0;
    if(flt_block->timeout > ttime)
      return 0;
  }

  if(flt->check) {
    // if return 0 the real callback will be skipped
    if(!flt->check(proc, flt))
      return;
  }

  rv = flt->callback(proc, flt);

  if(rv == 0)
    return rv;

  if(!flt_block)
    flt_block = malloc(sizeof(struct filter_block));

  flt_block->pid = proc->proc.tgid;

  timeout = FILTER_TIMEOUT(rv);
  flags = FILTER_FLAGS(rv);

  if(timeout) {
    if(!ttime)
      time (&ttime);
    flt_block->timeout = ttime + abs(timeout);
  } else if(rv == FILTER_STOP) {
    flt_block->skip = TRUE;
  }

  g_hash_table_insert(proc->skip_filter, GUINT_TO_POINTER(flt), flt_block);
  return rv;
}

static GNode *blocked_parent;

gboolean filter_run_for_node(GNode *node, gpointer data) {
  GNode *tmp;
  int rv;
  printf("run for node\n");
  if(node == processes_tree)
    return FALSE;
  if(blocked_parent) {
    do {
      tmp = node->parent;
      if(!tmp)
        break;

      if(tmp == blocked_parent) {
        // we don't run filters on nodes those parent set the skip child flag
        return FALSE;
      } else if (tmp == processes_tree) {
        // we can unset the block, as we must have left all childs
        blocked_parent = NULL;
        break;
      }
    } while(TRUE);
  }
  rv = filter_run_for_proc(node->data, data);

  if(FILTER_FLAGS(rv) & FILTER_SKIP_CHILD) {
    blocked_parent = node;
  }
  return FALSE;
}

void scheduler_run() {
  // FIXME make scheduler more flexible
  l_scheduler_run(lua_main_state);
}

void filter_run() {
  u_filter *flt;
  GList *cur = g_list_first(filter_list);
  while(cur) {
    flt = cur->data;
    blocked_parent = NULL;
    g_node_traverse(processes_tree, G_PRE_ORDER,G_TRAVERSE_ALL, -1, 
                    filter_run_for_node, flt);
    cur = g_list_next(cur);
  }
  blocked_parent = NULL;
}

int iterate(gpointer ignored) {
  iteration += 1;
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "update processes: %d", iteration);
  update_processes();
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "run filter: %d", iteration);
  filter_run();
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "schedule: %d", iteration);
  scheduler_run();
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "done: %d", iteration);
  return TRUE;
}


/***************************************************************************
 * rules and modules handling
 **************************************************************************/

int load_rule_directory(char *path, char *load_pattern) {
  DIR             *dip;
  struct dirent   *dit;
  char rpath[PATH_MAX+1];

  if ((dip = opendir(path)) == NULL)
  {
    perror("opendir");
    return 0;
  }

  if(load_pattern)
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "load pattern: %s", load_pattern);

  while ((dit = readdir(dip)) != NULL)
  {
    if(fnmatch("*.lua", dit->d_name, 0))
      continue;
    if(load_pattern && (fnmatch(load_pattern, dit->d_name, 0) != 0)) {
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "skip rule: %s", dit->d_name);
      continue;
    }

    snprintf(rpath, PATH_MAX, "%s/%s", path, dit->d_name);
    load_lua_rule_file(lua_main_state, rpath);
  }
  free(dip);
}


int load_modules(char *modules_directory) {
  DIR             *dip;
  struct dirent   *dit;
  char rpath[PATH_MAX+1];
  char *minit_name, *module_name, *error;
  char **disabled;
  gsize  disabled_len, i;
  gboolean skip;
  void *handle;
  int (*minit)(void);

  if ((dip = opendir(modules_directory)) == NULL)
  {
    perror("opendir");
    return 0;
  }

  disabled = g_key_file_get_string_list(config_data, CONFIG_CORE,
                                        "disabled_modules", &disabled_len, NULL);

  while ((dit = readdir(dip)) != NULL)
  {
    skip = FALSE;
    if(fnmatch("*.so", dit->d_name, 0))
      continue;

    module_name = g_strndup(dit->d_name,strlen(dit->d_name)-3);

    for(i = 0; i < disabled_len; i++) {
      if(!g_strcasecmp(disabled[i], module_name)) {
        skip = TRUE;
        break;
      }
    }
    if(!skip) {
      snprintf(rpath, PATH_MAX, "%s/%s", modules_directory, dit->d_name);
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "load module %s", dit->d_name);

      handle = dlopen(rpath, RTLD_LAZY);
      if (!handle) {
        //fprintf(stderr, "%s\n", dlerror());
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load module %s", rpath);
      }
      dlerror();

      minit_name = g_strconcat(module_name, "_init", NULL);
      *(void **) (&minit) = dlsym(handle, minit_name);

      if ((error = dlerror()) != NULL)  {
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load module %s: %s", module_name, error);
      }

      if(minit())
        g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "module %s returned error", module_name);

      free(minit_name);
    } else
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "skip module %s", module_name);

    free(module_name);
  }
  g_free(disabled);
  free(dip);
}

int core_init() {
  // load config
  iteration = 1;
  filter_list = g_list_alloc();
  processes_tree = g_node_new(NULL);
  processes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, 
                                    processes_free_value);

  // configure lua
  lua_main_state = luaL_newstate();
  luaL_openlibs(lua_main_state);
  luaopen_bc(lua_main_state);
  luaopen_ulatency(lua_main_state);
#ifdef LIBCGROUP
  luaopen_cgroup(lua_main_state);
#endif
  // FIXME
  if(load_lua_rule_file(lua_main_state, "src/core.lua"))
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "can't load core library");
}

void core_unload() {
  lua_gc (lua_main_state, LUA_GCCOLLECT, 0);
}
