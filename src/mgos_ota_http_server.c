/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#include "mgos_ota_http_server.h"
#include "mgos_ota_http_client.h"

#include "mgos_http_server.h"

#include "common/cs_dbg.h"
#include "mgos_hal.h"
#include "mgos_mongoose.h"
#include "mgos_sys_config.h"
#include "mgos_timers.h"
#include "mgos_utils.h"

static void handle_update_post(struct mg_connection *c, int ev, void *p) {
  struct mg_http_multipart_part *mp = (struct mg_http_multipart_part *) p;
  struct update_context *ctx = (struct update_context *) c->user_data;
  if (ctx == NULL && ev != MG_EV_HTTP_MULTIPART_REQUEST) return;
  switch (ev) {
    case MG_EV_HTTP_MULTIPART_REQUEST: {
      ctx = updater_context_create();
      if (ctx != NULL) {
        ctx->nc = c;
        c->user_data = ctx;
      } else {
        c->flags |= MG_F_CLOSE_IMMEDIATELY;
      }
      break;
    }
    case MG_EV_HTTP_PART_BEGIN: {
      LOG(LL_DEBUG, ("MG_EV_HTTP_PART_BEGIN: %p %s %s", ctx, mp->var_name,
                     mp->file_name));
      /* We use ctx->file_name as a temp buffer for non-file variable values. */
      if (mp->file_name[0] == '\0') {
        ctx->file_name[0] = '\0';
      }
      break;
    }
    case MG_EV_HTTP_PART_DATA: {
      LOG(LL_DEBUG, ("MG_EV_HTTP_PART_DATA: %p %s %s %d", ctx, mp->var_name,
                     mp->file_name, (int) mp->data.len));

      if (mp->file_name[0] == '\0') {
        /* It's a non-file form variable. */
        size_t l = strlen(ctx->file_name);
        size_t avail = sizeof(ctx->file_name) - l - 1;
        strncat(ctx->file_name, mp->data.p, MIN(mp->data.len, avail));
        break;
      } else if (!is_update_finished(ctx)) {
        updater_process(ctx, mp->data.p, mp->data.len);
        LOG(LL_DEBUG, ("updater_process res: %d", ctx->result));
      } else {
        /* Don't close connection just yet, not all browsers like that. */
      }
      break;
    }
    case MG_EV_HTTP_PART_END: {
      LOG(LL_DEBUG, ("MG_EV_HTTP_PART_END: %p %s %s %d", ctx, mp->var_name,
                     mp->file_name, mp->status));
      /* Part finished with an error. REQUEST_END will follow. */
      if (mp->status < 0) break;
      if (mp->file_name[0] == '\0') {
        /* It's a non-file form variable. Value is in ctx->file_name. */
        LOG(LL_DEBUG, ("Got var: %s=%s", mp->var_name, ctx->file_name));
        /* Commit timeout can be set after flashing. */
        if (strcmp(mp->var_name, "commit_timeout") == 0) {
          ctx->fctx.commit_timeout = atoi(ctx->file_name);
        }
      } else {
        /* End of the fw part, but there may still be parts with vars to follow,
         * which can modify settings (that can be applied post-flashing). */
      }
      break;
    }
    case MG_EV_HTTP_MULTIPART_REQUEST_END: {
      LOG(LL_DEBUG,
          ("MG_EV_HTTP_MULTIPART_REQUEST_END: %p %d", ctx, mp->status));
      /* Whatever happens, this is the last thing we do. */
      c->flags |= MG_F_SEND_AND_CLOSE;

      if (ctx == NULL) break;
      if (is_write_finished(ctx)) updater_finalize(ctx);
      if (!is_update_finished(ctx)) {
        ctx->result = -1;
        ctx->status_msg = "Update aborted";
        updater_finish(ctx);
      }
      if (mp->status < 0) {
        /* mp->status < 0 means connection is dead, do not send reply */
      } else {
        int code = (ctx->result > 0 ? 200 : 400);
        mg_send_response_line(c, code,
                              "Content-Type: text/plain\r\n"
                              "Connection: close\r\n");
        mg_printf(c, "%s\r\n",
                  ctx->status_msg ? ctx->status_msg : "Unknown error");
        if (is_reboot_required(ctx)) {
          LOG(LL_INFO, ("Rebooting device"));
          mgos_system_restart_after(101);
        }
        c->flags |= MG_F_SEND_AND_CLOSE;
      }
      updater_context_free(ctx);
      c->user_data = NULL;
      break;
    }
  }
}

struct mg_connection *s_update_request_conn;

static void mgos_ota_result_cb(struct update_context *ctx) {
  if (ctx != updater_context_get_current()) return;
  if (s_update_request_conn != NULL) {
    int code = (ctx->result > 0 ? 200 : 500);
    mg_send_response_line(s_update_request_conn, code,
                          "Content-Type: text/plain\r\n"
                          "Connection: close\r\n");
    mg_printf(s_update_request_conn, "(%d) %s\r\n", ctx->result,
              ctx->status_msg);
    s_update_request_conn->flags |= MG_F_SEND_AND_CLOSE;
    s_update_request_conn = NULL;
  }
}

static void update_handler(struct mg_connection *c, int ev, void *ev_data,
                           void *user_data) {
  switch (ev) {
    case MG_EV_HTTP_MULTIPART_REQUEST:
    case MG_EV_HTTP_PART_BEGIN:
    case MG_EV_HTTP_PART_DATA:
    case MG_EV_HTTP_PART_END:
    case MG_EV_HTTP_MULTIPART_REQUEST_END: {
      if (mgos_sys_config_get_update_enable_post()) {
        handle_update_post(c, ev, ev_data);
      } else {
        mg_send_response_line(c, 400,
                              "Content-Type: text/plain\r\n"
                              "Connection: close\r\n");
        mg_printf(c, "POST updates are disabled.");
        c->flags |= MG_F_SEND_AND_CLOSE;
      }
      return;
    }
    case MG_EV_HTTP_REQUEST: {
      struct http_message *hm = (struct http_message *) ev_data;
      if (updater_context_get_current() != NULL) {
        mg_send_response_line(c, 409,
                              "Content-Type: text/plain\r\n"
                              "Connection: close\r\n");
        mg_printf(c, "Another update is in progress.\r\n");
        c->flags |= MG_F_SEND_AND_CLOSE;
        return;
      }
      const char *url = mgos_sys_config_get_update_url();
      int commit_timeout = mgos_sys_config_get_update_commit_timeout();
      bool ignore_same_version = true;
      struct mg_str params =
          (mg_vcmp(&hm->method, "POST") == 0 ? hm->body : hm->query_string);
      size_t buf_len = params.len;
      char *buf = calloc(params.len, 1), *p = buf;
      int len = mg_get_http_var(&params, "url", p, buf_len);
      if (len > 0) {
        url = p;
        p += len + 1;
        buf_len -= len + 1;
      }
      len = mg_get_http_var(&params, "commit_timeout", p, buf_len);
      if (len > 0) {
        commit_timeout = atoi(p);
      }
      len = mg_get_http_var(&params, "ignore_same_version", p, buf_len);
      if (len > 0) {
        ignore_same_version = (atoi(p) > 0);
      }
      if (url != NULL) {
        s_update_request_conn = c;
        struct update_context *ctx = updater_context_create();
        if (ctx == NULL) {
          mg_send_response_line(c, 409,
                                "Content-Type: text/plain\r\n"
                                "Connection: close\r\n");
          mg_printf(c, "Failed to create updater context.\r\n");
          c->flags |= MG_F_SEND_AND_CLOSE;
          return;
        }
        ctx->ignore_same_version = ignore_same_version;
        ctx->fctx.commit_timeout = commit_timeout;
        ctx->result_cb = mgos_ota_result_cb;
        mgos_ota_http_start(ctx, url);

      } else {
        mg_send_response_line(c, 400,
                              "Content-Type: text/plain\r\n"
                              "Connection: close\r\n");
        mg_printf(c, "Update URL not specified and none is configured.\r\n");
        c->flags |= MG_F_SEND_AND_CLOSE;
      }
      free(buf);
      break;
    }
    case MG_EV_CLOSE: {
      if (s_update_request_conn == c) {
        /* Client went away while waiting for response. */
        s_update_request_conn = NULL;
      }
      break;
    }
  }
  (void) user_data;
}

static void update_action_handler(struct mg_connection *c, int ev, void *p,
                                  void *user_data) {
  if (ev != MG_EV_HTTP_REQUEST) return;
  struct http_message *hm = (struct http_message *) p;
  bool is_commit = (mg_vcmp(&hm->uri, "/update/commit") == 0);
  bool ok =
      (is_commit ? mgos_upd_commit() : mgos_upd_revert(false /* reboot */));
  mg_send_response_line(c, (ok ? 200 : 400),
                        "Content-Type: text/html\r\n"
                        "Connection: close");
  mg_printf(c, "\r\n%s\r\n", (ok ? "Ok" : "Error"));
  c->flags |= MG_F_SEND_AND_CLOSE;
  if (ok && !is_commit) mgos_system_restart_after(100);
  (void) user_data;
}

bool mgos_ota_http_server_init(void) {
  mgos_register_http_endpoint("/update/commit", update_action_handler, NULL);
  mgos_register_http_endpoint("/update/revert", update_action_handler, NULL);
  mgos_register_http_endpoint("/update", update_handler, NULL);
  return true;
}
