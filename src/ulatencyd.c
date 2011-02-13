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

#include "ulatency.h"
#include "config.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#ifndef O_NOFOLLOW
// not important here to have NOFOLLOW. very unlikly attack
#define O_NOFOLLOW 0
#endif

#ifdef ENABLE_DBUS
#include <dbus/dbus-glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

DBusGConnection *U_dbus_connection;
DBusGConnection *U_dbus_connection_system;
#endif

#include <glib.h>
#include "proc/sysinfo.h"
#include "proc/readproc.h"
#ifdef LIBCGROUP
#include <libcgroup.h>
#endif
#include <sys/mman.h>
#include <error.h>

static gchar *config_file = QUOTEME(CONFIG_FILE);
static gchar *rules_directory = QUOTEME(RULES_DIRECTORY);
static gchar *modules_directory = QUOTEME(MODULES_DIRECTORY);
static gchar *load_pattern = NULL;
static gint verbose = 1<<5;
static char *mount_point;
static char *log_file = NULL;
int          log_fd = -1;

GKeyFile *config_data;

/*
static gint max_size = 8;

static gboolean beep = FALSE;
*/
//static gboolean rand = FALSE;

static gboolean opt_daemon = FALSE;

int init_netlink(GMainLoop *loop);

static gboolean opt_verbose(const gchar *option_name, const gchar *value, gpointer data, GError **error) {
  int i = 1;
  if(value) {
    i = atoi(value);
  }
  verbose = verbose << i;
  return TRUE;
}

static gboolean opt_quiet(const gchar *option_name, const gchar *value, gpointer data, GError **error) {
  int i = 1;
  if(value) {
    i = atoi(value);
  }
  verbose = verbose >> i;
  return TRUE;
}

static GOptionEntry entries[] =
{
  { "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_file, "Use config file", NULL},
  { "rules-directory", 'r', 0, G_OPTION_ARG_FILENAME, &rules_directory, "Path with ", NULL},
  { "rule-pattern", 0, 0, G_OPTION_ARG_STRING, &load_pattern, "Load only rules matching the pattern", NULL},
  { "verbose", 'v', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK, &opt_verbose, "More verbose. Can be passed multiple times", NULL },
  { "quiet", 'q', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK, &opt_quiet, "More quiet. Can be passed multiple times", NULL },
  { "log-file", 'f', 0, G_OPTION_ARG_FILENAME, &log_file, "Log to file", NULL},
  { "daemonize", 'd', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_NONE, &opt_daemon, "Run daemon in background", NULL },
  { NULL }
};


int filter_interval;

GMainContext *main_context;
GMainLoop *main_loop;


void cleanup() {
  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "cleanup daemon");
#ifdef LIBCGROUP
  cgroup_unload_cgroups();
#endif
  // for valgrind
  core_unload();
}

#define FORMAT_UNSIGNED_BUFSIZE ((GLIB_SIZEOF_LONG * 3) + 3)
#define	STRING_BUFFER_SIZE	(FORMAT_UNSIGNED_BUFSIZE + 32)

#define CHAR_IS_SAFE(wc) (!((wc < 0x20 && wc != '\t' && wc != '\n' && wc != '\r') || \
			    (wc == 0x7f) || \
			    (wc >= 0x80 && wc < 0xa0)))

static void
escape_string (GString *string)
{
  const char *p = string->str;
  gunichar wc;

  while (p < string->str + string->len) {
    gboolean safe;
    wc = g_utf8_get_char_validated (p, -1);
    if (wc == (gunichar)-1 || wc == (gunichar)-2) {
      gchar *tmp;
      guint pos;

      pos = p - string->str;

      /* Emit invalid UTF-8 as hex escapes 
             */
      tmp = g_strdup_printf ("\\x%02x", (guint)(guchar)*p);
      g_string_erase (string, pos, 1);
      g_string_insert (string, pos, tmp);

      p = string->str + (pos + 4); /* Skip over escape sequence */

      g_free (tmp);
      continue;
    }
    if (wc == '\r')
      safe = *(p + 1) == '\n';
    else
      safe = CHAR_IS_SAFE (wc);

    if (!safe) {
      gchar *tmp;
      guint pos;

      pos = p - string->str;

      /* Largest char we escape is 0x0a, so we don't have to worry
       * about 8-digit \Uxxxxyyyy
       */
      tmp = g_strdup_printf ("\\u%04x", wc); 
      g_string_erase (string, pos, g_utf8_next_char (p) - p);
      g_string_insert (string, pos, tmp);
      g_free (tmp);

      p = string->str + (pos + 6); /* Skip over escape sequence */

    } else
      p = g_utf8_next_char (p);
  }
}


static void log_file_handler (const gchar    *log_domain,
                              GLogLevelFlags  log_level,
                              const gchar    *message,
                              gpointer        unused_data)
{
  gboolean is_fatal = (log_level & G_LOG_FLAG_FATAL) != 0;
  gchar level_prefix[STRING_BUFFER_SIZE], *string;
  GString *gstring;

  if (log_level & G_LOG_FLAG_RECURSION) {
      return;
  }

  gstring = g_string_new (NULL);

  if (log_domain) {
    g_string_append (gstring, log_domain);
    g_string_append_c (gstring, '-');
  }

  switch (log_level & G_LOG_LEVEL_MASK) {
    case G_LOG_LEVEL_ERROR:
      strcpy (level_prefix, "ERROR");
      break;
    case G_LOG_LEVEL_CRITICAL:
      strcpy (level_prefix, "CRITICAL");
      break;
    case G_LOG_LEVEL_WARNING:
      strcpy (level_prefix, "WARNING");
      break;
    case G_LOG_LEVEL_MESSAGE:
      strcpy (level_prefix, "Message");
      break;
    case G_LOG_LEVEL_INFO:
      strcpy (level_prefix, "INFO");
      break;
    case G_LOG_LEVEL_DEBUG:
      strcpy (level_prefix, "DEBUG");
      break;
    case U_LOG_LEVEL_TRACE:
      strcpy (level_prefix, "TRACE");
      break;
    case U_LOG_LEVEL_SCHED:
      strcpy (level_prefix, "SCHED");
      break;
    default:
      strcpy (level_prefix, "LOG-");
  }

  g_string_append (gstring, level_prefix);

  g_string_append (gstring, ": ");

  if (!message) {

    g_string_append (gstring, "(NULL) message");

  } else {

    GString *msg;

    msg = g_string_new (message);
    escape_string (msg);

    g_string_append (gstring, msg->str);	/* charset is UTF-8 already */

    g_string_free (msg, TRUE);

  }

  if (is_fatal)
    g_string_append (gstring, "\naborting...\n");
  else
    g_string_append (gstring, "\n");

  string = g_string_free (gstring, FALSE);

  write (log_fd, string, strlen (string));
  g_free (string);
}

static void close_logfile() {
  if(log_fd >= 0)
    close(log_fd);
  log_fd = -1;
}

static int open_logfile(char *file) {
  if(file == NULL)
    file = log_file;
  if(file == NULL)
    return FALSE;
  if(log_fd)
    close_logfile();
  log_fd = open(file, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if(log_fd == -1)
    g_warning("can't write to log file %s", file);
  return TRUE;
}


static void filter_log_handler(const gchar *log_domain, GLogLevelFlags log_level,
                        const gchar *message, gpointer unused_data) {

  if(log_level <= verbose) {
    if(log_fd != -1)
      log_file_handler(log_domain, log_level, message, unused_data);
    else
      g_log_default_handler(log_domain, log_level, message, unused_data);
  }
}

void load_config() {
  GError *error = NULL;

  if(!g_key_file_load_from_file(config_data, config_file, 
                                G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS,
                                &error)) {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "could not load config file: %s: %s", config_file, error->message);
  }

  filter_interval = g_key_file_get_integer(config_data, CONFIG_CORE, "interval", NULL);
  if(!filter_interval)
    filter_interval = 60;

  mount_point = g_key_file_get_string(config_data, CONFIG_CORE, "mount_point", NULL);
  if(!mount_point)
    mount_point = "/dev/cgroups";
}


#define DEFAULT_CGROUPS "cpu,memory"

int mount_cgroups() {
  gchar   *argv[10];
  gint    i, rv;
  gint    result;
  GError  *error = NULL;
  char    *sub = g_key_file_get_string(config_data, CONFIG_CORE, "cgroup_subsys", NULL);

  if (!sub) {
    sub = DEFAULT_CGROUPS;
  }

  argv[i=0] = "/bin/mount";
  argv[++i] = "-t";
  argv[++i] = "cgroup";
  argv[++i] = "-o";
  argv[++i] = sub;
  argv[++i] = "none";
  argv[++i] = mount_point;
  argv[++i] = NULL;
  rv = g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, &result, &error);
  if(rv && !result) {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "mounted cgroups on %s", mount_point);
  } else {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "error mounting cgroups on %s: %s", mount_point, (error && error->message) ? error->message : "");
    return FALSE;
  }
  return TRUE;
}
/*
 
gboolean            g_spawn_sync                        ("/",
                                                         gchar **argv,
                                                         gchar **envp,
                                                         GSpawnFlags flags,
                                                         GSpawnChildSetupFunc child_setup,
                                                         gpointer user_data,
                                                         gchar **standard_output,
                                                         gchar **standard_error,
                                                         gint *exit_status,
                                                         GError **error);

}

*/

#ifdef ENABLE_DBUS
static int do_dbus_init() {
  GError *error = NULL;
  DBusConnection *con;
#ifdef DEVELOP_MODE
  char *env_uid;
  uid_t target = 0;
  if(getuid() == 0) {
    env_uid = getenv("SUDO_UID");
    if(!env_uid)
      g_error("please set SUDO_UID env");
    target = atoi(env_uid);
    seteuid(target);
    U_dbus_connection = dbus_g_bus_get (DBUS_BUS_SESSION,
                               &error);
    seteuid(0);
  
  } else {
    U_dbus_connection = dbus_g_bus_get (DBUS_BUS_SESSION,
                               &error);
  }
  U_dbus_connection_system = dbus_g_bus_get (DBUS_BUS_SYSTEM,
                               &error);
  g_warning("DEVELOP_MODE ON: using session dbus");
#else
  U_dbus_connection = dbus_g_bus_get (DBUS_BUS_SYSTEM,
                               &error);
  U_dbus_connection_system = U_dbus_connection;
#endif
  if (U_dbus_connection == NULL)
    {
      g_warning("Failed to open connection to bus: %s\n",
                  error->message);
      g_error_free (error);
      return FALSE;
    }
  con = dbus_g_connection_get_connection(U_dbus_connection);
#ifndef DEVELOP_MODE
  dbus_connection_set_exit_on_disconnect (con, FALSE);
#endif
  return TRUE;
}
#endif

static void
signal_reload (int signal)
{
  g_warning("FIXME: reload config");
}


static void
signal_cleanup (int signal)
{
  // we have to make sure cgroup_unload_cgroups is called
  printf("abort cleanup\n");
#ifdef LIBCGROUP
  cgroup_unload_cgroups();
#endif
  exit(1);
}

static void
signal_logrotate (int signal)
{
  close_logfile();
  open_logfile(log_file);
}

int timeout_long(gpointer data) {

  // check if dbus connection is still alive
#ifdef ENABLE_DBUS
  if(U_dbus_connection) {
    DBusConnection *con = dbus_g_connection_get_connection(U_dbus_connection);
    if(!dbus_connection_get_is_connected(con)) {
      g_warning("got disconnected from dbus system bus. reconnecting...");
      dbus_g_connection_unref(U_dbus_connection);
      do_dbus_init();
    }
  } else {
    do_dbus_init();
  }
#endif

  // try the make current memory non swapalbe
  if(mlockall(MCL_CURRENT) && getuid() == 0)
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_INFO, "can't mlock memory");

  return TRUE;
}



int main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context;

  // required for dbus
  g_type_init ();

  config_data = g_key_file_new();

#ifdef ENABLE_DBUS
  g_thread_init(NULL);
  dbus_g_thread_init();

  do_dbus_init();
#endif 

  context = g_option_context_new ("- latency optimizing daemon");
  g_option_context_add_main_entries (context, entries, /*GETTEXT_PACKAGE*/NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_print ("option parsing failed: %s\n", error->message);
      exit (1);
    }

  pid_t pid, sid;
  if (opt_daemon) {
    pid = fork();
    if (pid < 0) {
      exit (1);
    }
    if (pid > 0) {
      exit (0);
    }

    umask (0);

    sid = setsid();

    if (sid < 0) {
      exit (1);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
  }

  load_config();

  g_log_set_default_handler(filter_log_handler, NULL);
  if(log_file) {
    open_logfile(log_file);
  }

  main_context = g_main_context_default();
  main_loop = g_main_loop_new(main_context, FALSE);

#if LIBCGROUP
  if(cgroup_init()) {
    g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "could not init libcgroup. try mounting cgroups...");
    g_mkdir_with_parents(mount_point, 0755);
    if(!mount_cgroups() || cgroup_init()) {
#ifdef DEVELOP_MODE
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "give up init libcgroup");
      //g_log(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "give up init libcgroup");
#else
      g_log(G_LOG_DOMAIN, G_LOG_LEVEL_ERROR, "give up init libcgroup");
#endif
    }
  }
#else
  //mount_cgroups();
#endif

  g_atexit(cleanup);
  
  if (signal (SIGABRT, signal_cleanup) == SIG_IGN)
    signal (SIGABRT, SIG_IGN);
  if (signal (SIGINT, signal_cleanup) == SIG_IGN)
    signal (SIGINT, SIG_IGN);
  if (signal (SIGTERM, signal_cleanup) == SIG_IGN)
    signal (SIGTERM, SIG_IGN);
  if (signal (SIGUSR1, signal_reload) == SIG_IGN)
    signal (SIGUSR1, SIG_IGN);
  if (signal (SIGUSR2, signal_logrotate) == SIG_IGN)
    signal (SIGUSR2, SIG_IGN);



  
  //signal (SIGABRT, cleanup_on_abort);
  core_init();
  adj_oom_killer(getpid(), -1000);
  load_modules(modules_directory);
  load_rule_directory(rules_directory, load_pattern, TRUE);

  process_update_all();
  init_netlink(main_loop);

  // small hack
  timeout_long(NULL);
  iterate(GUINT_TO_POINTER(0));
  g_timeout_add_seconds(60, timeout_long, GUINT_TO_POINTER(1));

  g_timeout_add_seconds(filter_interval, iterate, GUINT_TO_POINTER(1));

  g_log(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, "ulatencyd started successfull");
  if(g_main_loop_is_running(main_loop));
    g_main_loop_run(main_loop);
  return 0;
}
