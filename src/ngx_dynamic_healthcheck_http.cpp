#include "ngx_dynamic_healthcheck_http.h"

extern "C" {

#include <ngx_core.h>
#include <ngx_http.h>
#include <assert.h>

}


static in_port_t
get_in_port(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return ntohs(((struct sockaddr_in*)sa)->sin_port);
    }

    return ntohs(((struct sockaddr_in6*)sa)->sin6_port);
}


ngx_int_t
healthcheck_http_helper::make_request(ngx_dynamic_healthcheck_opts_t *shared,
    ngx_dynamic_hc_local_node_t *state)
{
    ngx_buf_t                       *buf = state->buf;
    ngx_connection_t                *c = state->pc.connection;
    ngx_uint_t                       i;
    ngx_str_t                        host;

    buf->last = ngx_snprintf(buf->last, buf->end - buf->last,
        "%V %V HTTP/1.%s\r\n",
        &shared->request_method,
        &shared->request_uri,
        shared->keepalive > c->requests + 1 ? "1" : "0");

    if (state->server.len >= 5
        && ngx_strncmp(state->server.data, "unix:", 5) != 0) {
        host = state->name;
        for (; host.len > 0 && host.data[host.len - 1] != ':';
               host.len--);
        host.len--;
        buf->last = ngx_snprintf(buf->last, buf->end - buf->last,
            "Host: %V:%d\r\n", &host, get_in_port(state->sockaddr));
    }

    buf->last = ngx_snprintf(buf->last, buf->end - buf->last,
        "User-Agent: nginx/"NGINX_VERSION"\r\n"
        "Connection: %s\r\n",
        shared->keepalive > c->requests + 1 ? "keep-alive" : "close");

    for (i = 0; i < shared->request_headers.len; i++)
        buf->last = ngx_snprintf(buf->last, buf->end - buf->last,
            "%V: %V\r\n",
            &shared->request_headers.data[i].key,
            &shared->request_headers.data[i].value);

    if (shared->request_body.len)
        buf->last = ngx_snprintf(buf->last, buf->end - buf->last,
            "Content-Length: %d\r\n\r\n%V",
            shared->request_body.len, &shared->request_body);
    else
        buf->last = ngx_snprintf(buf->last, buf->end - buf->last, "\r\n");

    if (buf->last == buf->end) {
        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "[%V] %V: %V addr=%V, fd=%d http "
                      "healthcheck_buffer_size too small for the request",
                      &module, &upstream, &server, &name, c->fd);
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
healthcheck_http_helper::parse_status_line(ngx_dynamic_hc_local_node_t *state)
{
    if (status.code != 0)
        return NGX_OK;

    switch (ngx_http_parse_status_line(&r, state->buf, &status)) {

        case NGX_OK:
            ngx_log_error(NGX_LOG_DEBUG,
                          state->pc.connection->log, 0,
                          "[%V] %V: %V addr=%V, "
                          "fd=%d http on_recv() status: %d",
                          &module, &upstream, &server, &name,
                          state->pc.connection->fd,
                          status.code);
            break;

        case NGX_AGAIN:
            return NGX_AGAIN;

        case NGX_ERROR:
        default:
            return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
parse_header(ngx_http_request_t *r, ngx_buf_t *buf, ngx_keyval_t *h)
{
    switch (ngx_http_parse_header_line(r, buf, 1)) {

        case NGX_OK:

            break;

        case NGX_AGAIN:

            return NGX_AGAIN;

        case NGX_HTTP_PARSE_HEADER_DONE:

            return NGX_HTTP_PARSE_HEADER_DONE;

        case NGX_HTTP_PARSE_INVALID_HEADER:
        case NGX_ERROR:
        default:
            return NGX_ERROR;
    }

    h->key.len = r->header_name_end - r->header_name_start;
    h->key.data = r->header_name_start;
    h->key.data[h->key.len] = 0;

    h->value.len = r->header_end - r->header_start;
    h->value.data = r->header_start;
    h->value.data[h->value.len] = 0;

    ngx_strlow(h->key.data, h->key.data, h->key.len);

    return NGX_OK;
}


ngx_int_t
healthcheck_http_helper::receive_data(ngx_dynamic_hc_local_node_t *state)
{
    ngx_connection_t  *c = state->pc.connection;
    ngx_buf_t         *buf = state->buf;
    ssize_t            size;

    if (remains > buf->end - buf->last) {

        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "[%V] %V: %V addr=%V, fd=%d healthcheck_buffer_size "
                      "too small for read body",
                      &module, &upstream, &server, &name, c->fd);
        return NGX_ERROR;
    }

    if (remains == 0)
        size = c->recv(c, buf->last, buf->end - buf->last);
    else
        size = c->recv(c, buf->last, remains);

    eof = c->read->pending_eof;

    ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                  "[%V] %V: %V addr=%V, "
                  "fd=%d http on_recv() recv: %d, eof=%d",
                  &module, &upstream, &server, &name, c->fd, size, eof);

    if (size == NGX_ERROR)
        return eof ? NGX_OK : NGX_ERROR;

    if (size == NGX_AGAIN)
        return NGX_AGAIN;

    if (size == 0 && eof)
        return NGX_DECLINED;

    buf->last += size;

    return eof ? NGX_OK : NGX_DONE;
}


ngx_int_t
healthcheck_http_helper::parse_headers(ngx_dynamic_hc_local_node_t *state)
{
    ngx_keyval_t  h;

    for (;;) {

        switch (parse_header(&r, state->buf, &h)) {

            case NGX_AGAIN:

                return NGX_AGAIN;

            case NGX_OK:

                ngx_log_error(NGX_LOG_DEBUG,
                              state->pc.connection->log, 0,
                              "[%V] %V: %V addr=%V, "
                              "fd=%d http on_recv() header: %V=%V",
                              &module, &upstream, &server, &name,
                              state->pc.connection->fd,
                              &h.key, &h.value);

                if (ngx_strcmp(h.key.data, "content-length") == 0)
                    content_length = ngx_atoi(h.value.data, h.value.len);

                if (ngx_strcmp(h.key.data, "transfer-encoding") == 0)
                    chunked = ngx_strcmp(h.value.data, "chunked") == 0;

                break;

            case NGX_HTTP_PARSE_HEADER_DONE:

                return NGX_HTTP_PARSE_HEADER_DONE;

            case NGX_ERROR:
            default:
                return NGX_ERROR;
        }

    }

    assert(0);

    return NGX_ABORT;
}


ngx_int_t
healthcheck_http_helper::parse_body_chunked(ngx_dynamic_hc_local_node_t *state)
{
    ngx_connection_t  *c = state->pc.connection;
    ssize_t            size;
    u_char            *sep;
    ngx_buf_t         *buf = state->buf;

    for (;;) {

        if (remains != 0) {

            size = ngx_min(buf->last - buf->pos, remains);

            ngx_memcpy(body.last, buf->pos, size);
            body.last += size;
            buf->pos += size;

            remains -= size;
            if (remains > 0)
                return NGX_AGAIN;

            buf->pos += 2;  // CRLF
        }

        if (buf->pos == buf->last) {

            buf->pos = buf->last = buf->start;
            return NGX_AGAIN;
        }

        sep = (u_char *) ngx_strstr(buf->pos, CRLF);
        if (sep == NULL) {

            if (eof) {

                ngx_log_error(NGX_LOG_WARN, c->log, 0,
                              "[%V] %V: %V addr=%V, fd=%d http "
                              "invalid chunked response",
                              &module, &upstream, &server, &name, c->fd);
                return NGX_ERROR;
            }

            return NGX_AGAIN;
        }

        remains = ngx_hextoi(buf->pos, sep - buf->pos);
        if (remains < 0) {

            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "[%V] %V: %V addr=%V, fd=%d http "
                          "invalid chunk size",
                          &module, &upstream, &server, &name, c->fd);
            return NGX_ERROR;
        }

        if (remains == 0) {

            *body.last = 0;
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                      "[%V] %V: %V addr=%V, fd=%d http"
                      " on_recv() body chunk, size=%d",
                      &module, &upstream, &server, &name, c->fd,
                      remains);

        if (remains > body.end - body.last - 1) {

            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "[%V] %V: %V addr=%V, fd=%d "
                          "healthcheck_buffer_size too small for read body",
                          &module, &upstream, &server, &name, c->fd);
            return NGX_ERROR;
        }

        buf->pos = sep + 2;  //CRLF after chunk size
    }
}


ngx_int_t
healthcheck_http_helper::parse_body(ngx_dynamic_hc_local_node_t *state)
{
    ngx_connection_t  *c = state->pc.connection;
    ngx_buf_t         *buf = state->buf;

    if (chunked)
        return parse_body_chunked(state);

    ngx_memcpy(body.last, buf->pos, buf->last - buf->pos);
    body.last += buf->last - buf->pos;
    buf->pos = buf->last = buf->start;

    if (content_length > 0) {

        remains = content_length - (body.last - body.start);

        if (remains == 0) {

            *body.last = 0;
            return NGX_OK;
        }
    }

    if (!eof)
        return NGX_AGAIN;

    if (remains == 0) {

        *body.last = 0;
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_WARN, c->log, 0,
                  "[%V] %V: %V addr=%V, fd=%d http"
                  " connection closed on recv body",
                  &module, &upstream, &server, &name, c->fd);

    return NGX_ERROR;
}


ngx_int_t
healthcheck_http_helper::receive_body(ngx_dynamic_healthcheck_opts_t *shared,
    ngx_dynamic_hc_local_node_t *state)
{
    ngx_connection_t  *c = state->pc.connection;

    if (content_length != -1 && chunked) {

        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "[%V] %V: %V addr=%V, fd=%d http "
                      "content-length present for chunked response",
                      &module, &upstream, &server, &name, c->fd);
        return NGX_ERROR;
    }

    if (body.start != NULL)
        goto receive;

    if (!chunked) {

        if (content_length == 0)
            return NGX_OK;

        if (content_length != -1)
            remains = content_length;

        if ((size_t) remains > shared->buffer_size - 1) {

            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "[%V] %V: %V addr=%V, fd=%d http "
                          "healthcheck_buffer_size too small"
                          " for read body",
                          &module, &upstream, &server, &name, c->fd);
            return NGX_ERROR;
        }
    }

    pool = ngx_create_pool(1024, c->log);
    if (pool == NULL) {

        ngx_log_error(NGX_LOG_WARN, c->log, c->fd, 0,
                      "[%V] %V: %V addr=%V, fd=%d http receiving body: "
                      "no memory for read body",
                      &module, &upstream, &server, &name, c->fd);
        return NGX_ERROR;
    }

    body.start = (u_char *) ngx_palloc(pool, shared->buffer_size);
    if (body.start == NULL) {

        ngx_log_error(NGX_LOG_WARN, c->log, 0,
                      "[%V] %V: %V addr=%V, fd=%d http receiving body: "
                      "no memory for read body",
                      &module, &upstream, &server, &name, c->fd);
        return NGX_ERROR;
    }

    body.pos = body.last = body.start;
    body.end = body.start + shared->buffer_size;

receive:

    for (;;) {

        switch (parse_body(state)) {

            case NGX_OK:
                return NGX_OK;

            case NGX_AGAIN:
                break;

            case NGX_ERROR:
            default:
                return NGX_ERROR;
        }

        switch (receive_data(state)) {

            case NGX_OK:        // all data received
            case NGX_DONE:      // continue
            case NGX_DECLINED:  // eof
                break;

            case NGX_AGAIN:
                return NGX_AGAIN;

            case NGX_ERROR:
            default:
                return NGX_ERROR;
        }
    }
}


ngx_int_t
healthcheck_http_helper::receive_headers(ngx_dynamic_healthcheck_opts_t *shared,
    ngx_dynamic_hc_local_node_t *state)
{
    ngx_connection_t  *c = state->pc.connection;

    for (;;) {

        // parse status line

        switch (parse_status_line(state)) {

            case NGX_OK:
                break;

            case NGX_AGAIN:
                goto receive;

            case NGX_ERROR:
            default:
                return NGX_ERROR;
        }

        // parse headers

        switch (parse_headers(state)) {

            case NGX_HTTP_PARSE_HEADER_DONE:
                return receive_body(shared, state);

            case NGX_AGAIN:
                goto receive;

            case NGX_ERROR:
            default:
                return NGX_ERROR;
        }

receive:

        if (eof) {

            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "[%V] %V: %V addr=%V, fd=%d http connection"
                          " closed on read status line and headers",
                          &module, &upstream, &server, &name, c->fd);
            return NGX_ERROR;
        }

        // receive available data

        switch (receive_data(state)) {

            case NGX_OK:        // all data received
            case NGX_DONE:      // continue
            case NGX_DECLINED:  // eof
                break;

            case NGX_AGAIN:
                return NGX_AGAIN;

            case NGX_ERROR:
            default:
                return NGX_ERROR;
        }
    }
}


ngx_int_t
healthcheck_http_helper::receive(ngx_dynamic_healthcheck_opts_t *shared,
    ngx_dynamic_hc_local_node_t *state)
{
    ngx_connection_t  *c = state->pc.connection;
    ngx_int_t          rc;
    ngx_str_t          s;
    ngx_uint_t         j;

    ngx_log_error(NGX_LOG_DEBUG, state->pc.connection->log, 0,
                  "[%V] %V: %V addr=%V, fd=%d http on_recv() %s",
                  &module, &upstream, &server, &name, c->fd,
                  body.start == NULL ? "start" : "continue");

    if (body.start == NULL)
        rc = receive_headers(shared, state);
    else
        rc = receive_body(shared, state);

    if (rc != NGX_OK)
        return rc;

    // response received
    
    s.data = body.start;
    s.len = body.last - body.start;

    if (s.len) {

        ngx_log_error(NGX_LOG_DEBUG,
                      state->pc.connection->log, 0,
                      "[%V] %V: %V addr=%V, fd=%d "
                      "http on_recv() body:\n%V",
                      &module, &upstream, &server, &name, c->fd, &s);
    }

    if (shared->response_codes.len) {

        for (j = 0; j < shared->response_codes.len; j++)
            if (shared->response_codes.data[j] == (ngx_int_t) status.code)
                break;

        if (j == shared->response_codes.len) {

            ngx_log_error(NGX_LOG_WARN, c->log, 0,
                          "[%V] %V: %V addr=%V, fd=%d http status "
                          "is not in 'check_response_codes'",
                          &module, &upstream, &server, &name, c->fd);
            return NGX_ERROR;
        }
    }

    if (shared->response_body.len) {

        switch(ngx_dynamic_healthcheck_match_buffer(&shared->response_body,
                                                    &s)) {

            case NGX_OK:

                ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                              "[%V] %V: %V addr=%V, fd=%d http pattern"
                              " '%V' found",
                              &module, &upstream, &server, &name, c->fd,
                              &shared->response_body);
                return NGX_OK;

            case NGX_ERROR:

                ngx_log_error(NGX_LOG_DEBUG, c->log, 0,
                              "[%V] %V: %V addr=%V, fd=%d http pattern"
                              "'%V' error",
                              &module, &upstream, &server, &name, c->fd,
                              &shared->response_body);
                return NGX_ERROR;

            case NGX_DECLINED:
            default:

                ngx_log_error(NGX_LOG_WARN, c->log, 0,
                              "[%V] %V: %V addr=%V, fd=%d http pattern"
                              " '%V' is not found",
                              &module, &upstream, &server, &name, c->fd,
                              &shared->response_body);
                return NGX_ERROR;
        }
    }

    return NGX_OK;
}


healthcheck_http_helper::~healthcheck_http_helper()
{
    if (pool != NULL)
        ngx_destroy_pool(pool);
}