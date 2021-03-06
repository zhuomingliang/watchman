/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#ifdef HAVE_LIBGIMLI_H
# include <libgimli.h>
#endif

/* This needs to be recursive safe because we may log to clients
 * while we are dispatching subscriptions to clients */
pthread_mutex_t w_client_lock;
w_ht_t *clients = NULL;
static int listener_fd;
static pthread_t reaper_thread;

static w_ht_t *command_funcs = NULL;

json_t *make_response(void)
{
  json_t *resp = json_object();

  set_prop(resp, "version", json_string_nocheck(PACKAGE_VERSION));

  return resp;
}

bool clock_id_string(uint32_t ticks, char *buf, size_t bufsize)
{
  int res = snprintf(buf, bufsize, "c:%d:%" PRIu32,
              (int)getpid(), ticks);

  if (res == -1) {
    return false;
  }
  return (size_t)res < bufsize;
}

// Renders the current clock id string to the supplied buffer.
// Must be called with the root locked.
static bool current_clock_id_string(w_root_t *root,
    char *buf, size_t bufsize)
{
  return clock_id_string(root->ticks, buf, bufsize);
}

/* Add the current clock value to the response.
 * must be called with the root locked */
void annotate_with_clock(w_root_t *root, json_t *resp)
{
  char buf[128];

  if (current_clock_id_string(root, buf, sizeof(buf))) {
    set_prop(resp, "clock", json_string_nocheck(buf));
  }
}

/* must be called with the w_client_lock held */
bool enqueue_response(struct watchman_client *client,
    json_t *json, bool ping)
{
  struct watchman_client_response *resp;

  resp = calloc(1, sizeof(*resp));
  if (!resp) {
    return false;
  }
  resp->json = json;

  if (client->tail) {
    client->tail->next = resp;
  } else {
    client->head = resp;
  }
  client->tail = resp;

  if (ping) {
    ignore_result(write(client->ping[1], "a", 1));
  }

  return true;
}

void send_and_dispose_response(struct watchman_client *client,
    json_t *response)
{
  pthread_mutex_lock(&w_client_lock);
  if (!enqueue_response(client, response, false)) {
    json_decref(response);
  }
  pthread_mutex_unlock(&w_client_lock);
}

void send_error_response(struct watchman_client *client,
    const char *fmt, ...)
{
  char buf[WATCHMAN_NAME_MAX];
  va_list ap;
  json_t *resp = make_response();

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  set_prop(resp, "error", json_string_nocheck(buf));

  send_and_dispose_response(client, resp);
}

static void client_delete(w_ht_val_t val)
{
  struct watchman_client *client = w_ht_val_ptr(val);
  struct watchman_client_response *resp;

  /* cancel subscriptions */
  w_ht_free(client->subscriptions);

  while (client->head) {
    resp = client->head;
    client->head = resp->next;
    json_decref(resp->json);
    free(resp);
  }

  w_json_buffer_free(&client->reader);
  w_json_buffer_free(&client->writer);
  close(client->ping[0]);
  close(client->ping[1]);
  close(client->fd);
  free(client);
}

static struct watchman_hash_funcs client_hash_funcs = {
  NULL, // copy_key
  NULL, // del_key
  NULL, // equal_key
  NULL, // hash_key
  NULL, // copy_val
  client_delete
};

static void delete_subscription(w_ht_val_t val)
{
  struct watchman_client_subscription *sub = w_ht_val_ptr(val);

  w_string_delref(sub->name);
  w_query_delref(sub->query);
  free(sub);
}

static const struct watchman_hash_funcs subscription_hash_funcs = {
  w_ht_string_copy,
  w_ht_string_del,
  w_ht_string_equal,
  w_ht_string_hash,
  NULL,
  delete_subscription
};

// may attempt to lock the root!
bool w_parse_clockspec(w_root_t *root,
    json_t *value,
    struct w_clockspec_query *since,
    bool allow_cursor)
{
  const char *str;
  int pid;

  if (json_is_integer(value)) {
    since->is_timestamp = true;
    since->tv.tv_usec = 0;
    since->tv.tv_sec = json_integer_value(value);
    return true;
  }

  str = json_string_value(value);
  if (!str) {
    return false;
  }

  if (allow_cursor && root && str[0] == 'n' && str[1] == ':') {
    w_string_t *name = w_string_new(str);
    w_ht_val_t ticks_val;

    since->is_timestamp = false;
    w_root_lock(root);

    since->is_fresh_instance = !w_ht_lookup(root->cursors, w_ht_ptr_val(name),
                                            &ticks_val, false);
    if (since->is_fresh_instance) {
      since->ticks = 0;
    } else {
      since->ticks = (uint32_t)ticks_val;
    }

    // Bump the tick value and record it against the cursor.
    // We need to bump the tick value so that repeated queries
    // when nothing has changed in the filesystem won't continue
    // to return the same set of files; we only want the first
    // of these to return the files and the rest to return nothing
    // until something subsequently changes
    w_ht_replace(root->cursors, w_ht_ptr_val(name), ++root->ticks);

    w_log(W_LOG_DBG, "resolved cursor %s -> %" PRIu32 "\n",
        str, since->ticks);

    w_string_delref(name);

    w_root_unlock(root);
    return true;
  }

  if (sscanf(str, "c:%d:%" PRIu32, &pid, &since->ticks) == 2) {
    since->is_timestamp = false;
    if (pid == getpid()) {
      if (root && since->ticks == root->ticks) {
        /* Force ticks to increment.  This avoids returning and querying the
         * same tick value over and over when no files have changed in the
         * meantime */
        w_root_lock(root);
        root->ticks++;
        w_root_unlock(root);
      }
      return true;
    }
    // If the pid doesn't match, they asked a different
    // incarnation of the server, so we treat them as having
    // never spoken to us before
    since->is_fresh_instance = true;
    since->ticks = 0;
    return true;
  }

  return false;
}

json_t *w_match_results_to_json(
    uint32_t num_matches,
    struct watchman_rule_match *matches)
{
  json_t *file_list = json_array_of_size(num_matches);
  uint32_t i;

  if (num_matches) {
    // Build a template for the serializer
    json_t *templ = json_array_of_size(15);
    static const char *field_list[] = {
      "name", "exists", "size", "mode", "uid", "gid",
      "mtime", "ctime", "ino", "dev", "nlink", "new",
      "oclock", "cclock",
    };

    for (i = 0; i < sizeof(field_list)/sizeof(field_list[0]); i++) {
      json_array_append_new(templ, json_string_nocheck(field_list[i]));
    }

    json_array_set_template_new(file_list, templ);
  }

  for (i = 0; i < num_matches; i++) {
    struct watchman_file *file = matches[i].file;
    w_string_t *relname = matches[i].relname;
    char buf[128];

    json_t *record = json_object_of_size(15);

    set_prop(record, "name", json_string_nocheck(relname->buf));
    set_prop(record, "exists", json_boolean(file->exists));
    // Only report stat data if we think this file exists.  If it doesn't,
    // we probably have stale data cached in file->st which is useless to
    // report on.
    if (file->exists) {
      // Note: our JSON library supports 64-bit integers, but this may
      // pose a compatibility issue for others.  We'll see if anyone
      // runs into an issue and deal with it then...
      set_prop(record, "size", json_integer(file->st.st_size));
      set_prop(record, "mode", json_integer(file->st.st_mode));
      set_prop(record, "uid", json_integer(file->st.st_uid));
      set_prop(record, "gid", json_integer(file->st.st_gid));
      set_prop(record, "mtime", json_integer(file->st.st_mtime));
      set_prop(record, "ctime", json_integer(file->st.st_ctime));
      set_prop(record, "ino", json_integer(file->st.st_ino));
      set_prop(record, "dev", json_integer(file->st.st_dev));
      set_prop(record, "nlink", json_integer(file->st.st_nlink));

      if (matches[i].is_new) {
        set_prop(record, "new", json_true());
      }

      if (clock_id_string(file->ctime.ticks, buf, sizeof(buf))) {
        set_prop(record, "cclock", json_string_nocheck(buf));
      }
    }
    if (clock_id_string(file->otime.ticks, buf, sizeof(buf))) {
      set_prop(record, "oclock", json_string_nocheck(buf));
    }

    json_array_append_new(file_list, record);
  }

  return file_list;
}

w_root_t *resolve_root_or_err(
    struct watchman_client *client,
    json_t *args,
    int root_index,
    bool create)
{
  w_root_t *root;
  const char *root_name;
  char *errmsg = NULL;
  json_t *ele;

  ele = json_array_get(args, root_index);
  if (!ele) {
    send_error_response(client, "wrong number of arguments");
    return NULL;
  }

  root_name = json_string_value(ele);
  if (!root_name) {
    send_error_response(client,
        "invalid value for argument %d, expected "
        "a string naming the root dir",
        root_index);
    return NULL;
  }

  if (client->client_mode) {
    root = w_root_resolve_for_client_mode(root_name, &errmsg);
  } else {
    root = w_root_resolve(root_name, create, &errmsg);
  }

  if (!root) {
    send_error_response(client,
        "unable to resolve root %s: %s",
        root_name, errmsg);
    free(errmsg);
  }
  return root;
}

static void cmd_shutdown(
    struct watchman_client *client,
    json_t *args)
{
  void *ignored;
  unused_parameter(client);
  unused_parameter(args);

  w_log(W_LOG_ERR, "shutdown-server was requested, exiting!\n");

  /* close out some resources to persuade valgrind to run clean */
  close(listener_fd);
  listener_fd = -1;
  w_root_free_watched_roots();

  pthread_mutex_lock(&w_client_lock);
  w_ht_del(clients, client->fd);
  pthread_mutex_unlock(&w_client_lock);
  pthread_join(reaper_thread, &ignored);
  cfg_shutdown();

  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  exit(0);
}

static struct watchman_command_handler_def commands[] = {
  { "find", cmd_find },
  { "since", cmd_since },
  { "query", cmd_query },
  { "watch", cmd_watch },
  { "watch-list", cmd_watch_list },
  { "watch-del", cmd_watch_delete },
  { "trigger", cmd_trigger },
  { "trigger-list", cmd_trigger_list },
  { "trigger-del", cmd_trigger_delete },
  { "subscribe", cmd_subscribe },
  { "unsubscribe", cmd_unsubscribe },
  { "shutdown-server", cmd_shutdown },
  { "log-level", cmd_loglevel },
  { "log", cmd_log },
  { "version", cmd_version },
  { "clock", cmd_clock },
  { "get-sockname", cmd_get_sockname },
  { "get-pid", cmd_get_pid },
  { NULL, NULL }
};

void register_commands(struct watchman_command_handler_def *defs)
{
  int i;

  command_funcs = w_ht_new(16, &w_ht_string_funcs);
  for (i = 0; defs[i].name; i++) {
    w_ht_set(command_funcs,
        w_ht_ptr_val(w_string_new(defs[i].name)),
        w_ht_ptr_val(defs[i].func));
  }

  w_query_init_all();
}

bool dispatch_command(struct watchman_client *client, json_t *args)
{
  watchman_command_func func;
  const char *cmd_name;
  w_string_t *cmd;

  if (!json_array_size(args)) {
    send_error_response(client,
        "invalid command (expected an array with some elements!)");
    return false;
  }

  cmd_name = json_string_value(json_array_get(args, 0));
  if (!cmd_name) {
    send_error_response(client,
        "invalid command: expected element 0 to be the command name");
    return false;
  }
  cmd = w_string_new(cmd_name);
  func = w_ht_val_ptr(w_ht_get(command_funcs, w_ht_ptr_val(cmd)));
  w_string_delref(cmd);

  if (func) {
    func(client, args);
    return true;
  }
  send_error_response(client, "unknown command %s", cmd_name);

  return false;
}

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
static void *client_thread(void *ptr)
{
  struct watchman_client *client = ptr;
  struct pollfd pfd[2];
  json_t *request;
  json_error_t jerr;
  char buf[16];

  w_set_nonblock(client->fd);

  while (true) {
    // Wait for input from either the client socket or
    // via the ping pipe, which signals that some other
    // thread wants to unilaterally send data to the client

    pfd[0].fd = client->fd;
    pfd[0].events = POLLIN|POLLHUP|POLLERR;
    pfd[0].revents = 0;

    pfd[1].fd = client->ping[0];
    pfd[1].events = POLLIN|POLLHUP|POLLERR;
    pfd[1].revents = 0;

    ignore_result(poll(pfd, 2, 200));

    if (pfd[0].revents & (POLLHUP|POLLERR)) {
disconected:
      pthread_mutex_lock(&w_client_lock);
      w_ht_del(clients, client->fd);
      pthread_mutex_unlock(&w_client_lock);
      break;
    }

    if (pfd[0].revents) {
      request = w_json_buffer_next(&client->reader, client->fd, &jerr);

      if (!request && errno == EAGAIN) {
        // That's fine
      } else if (!request) {
        // Not so cool
        send_error_response(client, "invalid json at position %d: %s",
            jerr.position, jerr.text);
        w_log(W_LOG_ERR, "invalid data from client: %s\n", jerr.text);

        goto disconected;
      } else if (request) {
        client->pdu_type = client->reader.pdu_type;
        dispatch_command(client, request);
        json_decref(request);
      }
    }

    if (pfd[1].revents) {
      ignore_result(read(client->ping[0], buf, sizeof(buf)));
    }

    /* now send our response(s) */
    while (client->head) {
      struct watchman_client_response *resp;

      /* de-queue the first response */
      pthread_mutex_lock(&w_client_lock);
      resp = client->head;
      if (resp) {
        client->head = resp->next;
        if (client->tail == resp) {
          client->tail = NULL;
        }
      }
      pthread_mutex_unlock(&w_client_lock);

      if (resp) {
        w_clear_nonblock(client->fd);

        /* Return the data in the same format that was used to ask for it */
        w_ser_write_pdu(client->pdu_type, &client->writer,
            client->fd, resp->json);

        json_decref(resp->json);
        free(resp);

        w_set_nonblock(client->fd);
      }
    }
  }

  return NULL;
}

void w_log_to_clients(int level, const char *buf)
{
  json_t *json = NULL;
  w_ht_iter_t iter;

  if (!clients) {
    return;
  }

  pthread_mutex_lock(&w_client_lock);
  if (w_ht_first(clients, &iter)) do {
    struct watchman_client *client = w_ht_val_ptr(iter.value);

    if (client->log_level != W_LOG_OFF && client->log_level >= level) {
      json = make_response();
      if (json) {
        set_prop(json, "log", json_string_nocheck(buf));
        if (!enqueue_response(client, json, true)) {
          json_decref(json);
        }
      }
    }

  } while (w_ht_next(clients, &iter));
  pthread_mutex_unlock(&w_client_lock);
}

static void *child_reaper(void *arg)
{
  sigset_t sigset;
  unused_parameter(arg);

  // Unblock SIGCHLD only in this thread
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGCHLD);
  pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);

  while (listener_fd != -1) {
    usleep(200000);
    w_reap_children(true);
  }

  return 0;
}

// This is just a placeholder.
// This catches SIGUSR1 so we don't terminate.
// We use this to interrupt blocking syscalls
// on the worker threads
static void wakeme(int signo)
{
  unused_parameter(signo);
}

#ifdef HAVE_KQUEUE
#ifdef __OpenBSD__
#include <sys/siginfo.h>
#endif
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

bool w_start_listener(const char *path)
{
  struct sockaddr_un un;
  pthread_t thr;
  pthread_attr_t attr;
  pthread_mutexattr_t mattr;
  struct sigaction sa;
  sigset_t sigset;
#ifdef HAVE_LIBGIMLI_H
  volatile struct gimli_heartbeat *hb = NULL;
#endif

  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&w_client_lock, &mattr);
  pthread_mutexattr_destroy(&mattr);

#ifdef HAVE_LIBGIMLI_H
  hb = gimli_heartbeat_attach();
#endif

#ifdef HAVE_KQUEUE
  {
    struct rlimit limit;
    int mib[2] = { CTL_KERN,
#ifdef KERN_MAXFILESPERPROC
      KERN_MAXFILESPERPROC
#else
      KERN_MAXFILES
#endif
    };
    int maxperproc;
    size_t len;

    len = sizeof(maxperproc);
    sysctl(mib, 2, &maxperproc, &len, NULL, 0);

    getrlimit(RLIMIT_NOFILE, &limit);
    w_log(W_LOG_ERR, "file limit is %" PRIu64
        " kern.maxfilesperproc=%i\n",
        limit.rlim_cur, maxperproc);

    if (limit.rlim_cur != RLIM_INFINITY &&
        maxperproc > 0 &&
        limit.rlim_cur < (rlim_t)maxperproc) {
      limit.rlim_cur = maxperproc;

      if (setrlimit(RLIMIT_NOFILE, &limit)) {
        w_log(W_LOG_ERR,
          "failed to raise limit to %" PRIu64 " (%s).\n",
          limit.rlim_cur,
          strerror(errno));
      } else {
        w_log(W_LOG_ERR,
            "raised file limit to %" PRIu64 "\n",
            limit.rlim_cur);
      }
    }

    getrlimit(RLIMIT_NOFILE, &limit);
    if (limit.rlim_cur < 10240) {
      w_log(W_LOG_ERR,
          "Your file descriptor limit is very low (%" PRIu64 "), "
          "please consult the watchman docs on raising the limits\n",
          limit.rlim_cur);
    }
  }
#endif

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    w_log(W_LOG_ERR, "%s: path is too long\n",
        path);
    return false;
  }

  signal(SIGPIPE, SIG_IGN);

  /* allow SIGUSR1 and SIGCHLD to wake up a blocked thread, without restarting
   * syscalls */
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = wakeme;
  sa.sa_flags = 0;
  sigaction(SIGUSR1, &sa, NULL);
  sigaction(SIGCHLD, &sa, NULL);

  // Block SIGCHLD everywhere
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGCHLD);
  sigprocmask(SIG_BLOCK, &sigset, NULL);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  listener_fd = socket(PF_LOCAL, SOCK_STREAM, 0);
  if (listener_fd == -1) {
    w_log(W_LOG_ERR, "socket: %s\n",
        strerror(errno));
    return false;
  }

  un.sun_family = PF_LOCAL;
  strcpy(un.sun_path, path);
  unlink(path);

  if (bind(listener_fd, (struct sockaddr*)&un, sizeof(un)) != 0) {
    w_log(W_LOG_ERR, "bind(%s): %s\n",
      path, strerror(errno));
    close(listener_fd);
    return false;
  }

  if (listen(listener_fd, 200) != 0) {
    w_log(W_LOG_ERR, "listen(%s): %s\n",
        path, strerror(errno));
    close(listener_fd);
    return false;
  }

  w_set_cloexec(listener_fd);

  if (pthread_create(&reaper_thread, NULL, child_reaper, NULL)) {
    w_log(W_LOG_FATAL, "pthread_create(reaper): %s\n",
        strerror(errno));
    return false;
  }

  if (!clients) {
    clients = w_ht_new(2, &client_hash_funcs);
  }

  // Wire up the command handlers
  register_commands(commands);

  w_state_load();

#ifdef HAVE_LIBGIMLI_H
  if (hb) {
    gimli_heartbeat_set(hb, GIMLI_HB_RUNNING);
  }
  w_set_nonblock(listener_fd);
#endif

  // Now run the dispatch
  while (true) {
    int client_fd;
    struct watchman_client *client;
    struct pollfd pfd;
    int bufsize;

#ifdef HAVE_LIBGIMLI_H
    if (hb) {
      gimli_heartbeat_set(hb, GIMLI_HB_RUNNING);
    }
#endif

    pfd.events = POLLIN;
    pfd.fd = listener_fd;
    poll(&pfd, 1, 10000);

    client_fd = accept(listener_fd, NULL, 0);
    if (client_fd == -1) {
      continue;
    }
    w_set_cloexec(client_fd);
    bufsize = WATCHMAN_IO_BUF_SIZE;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    client = calloc(1, sizeof(*client));
    client->fd = client_fd;
    if (!w_json_buffer_init(&client->reader)) {
      // FIXME: error handling
    }
    if (!w_json_buffer_init(&client->writer)) {
      // FIXME: error handling
    }
    if (pipe(client->ping)) {
      // FIXME: error handling
    }
    client->subscriptions = w_ht_new(2, &subscription_hash_funcs);
    w_set_cloexec(client->ping[0]);
    w_set_nonblock(client->ping[0]);
    w_set_cloexec(client->ping[1]);
    w_set_nonblock(client->ping[1]);

    pthread_mutex_lock(&w_client_lock);
    w_ht_set(clients, client->fd, w_ht_ptr_val(client));
    pthread_mutex_unlock(&w_client_lock);

    // Start a thread for the client.
    // We used to use libevent for this, but we have
    // a low volume of concurrent clients and the json
    // parse/encode APIs are not easily used in a non-blocking
    // server architecture.
    if (pthread_create(&thr, &attr, client_thread, client)) {
      // It didn't work out, sorry!
      pthread_mutex_lock(&w_client_lock);
      w_ht_del(clients, client->fd);
      pthread_mutex_unlock(&w_client_lock);
    }
  }

  pthread_attr_destroy(&attr);
  return true;
}

/* vim:ts=2:sw=2:et:
 */

