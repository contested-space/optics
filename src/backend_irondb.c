/* backend_irondb.c
   FreeBSD-style copyright and disclaimer apply
*/

#include "optics.h"
#include "utils/errors.h"
#include "utils/socket.h"
#include "utils/buffer.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/socket.h>


// -----------------------------------------------------------------------------
// irondb
// -----------------------------------------------------------------------------

struct irondb
{
    const char *host;
    const char *port;

    int fd;
    uint64_t last_attempt;
};

static bool irondb_connect(struct irondb *irondb)
{
    optics_assert(irondb->fd <= 0,
            "attempting to connect to irondb while already connected");

    irondb->fd = socket_stream_connect(irondb->host, irondb->port);
    if (irondb->fd > 0) return true;

    optics_perror(&optics_errno);
    return false;
}

static void irondb_send(
        struct irondb *irondb, const char *data, size_t len, optics_ts_t ts)
{
    if (irondb->fd <= 0) {
        if (irondb->last_attempt == ts) return;
        irondb->last_attempt = ts;
        if (!irondb_connect(irondb)) return;
    }

    ssize_t ret = send(irondb->fd, data, len, MSG_NOSIGNAL);
    if (ret > 0 && (size_t) ret == len) return;
    if (ret >= 0) {
        optics_warn("unexpected partial message send: %lu '%s'", len, data);
        return;
    }

    optics_warn_errno("unable to send to irondb host '%s:%s'",
            irondb->host, irondb->port);
    close(irondb->fd);
    irondb->fd = -1;
}


// -----------------------------------------------------------------------------
// callbacks
// -----------------------------------------------------------------------------

struct dump_ctx
{
    struct irondb *irondb;
    const struct optics_poll *poll;
};

static bool irondb_dump_normalized(
        void *ctx_, optics_ts_t ts, const char *key, double value)
{
    struct dump_ctx *ctx = ctx_;
    struct buffer buffer = {0};

    //buffer_printf(&buffer, "%s.%s", ctx->poll->prefix, ctx->poll->host);
    buffer_printf(&buffer, "%s %g %lu|ST[prefix:%s,host:%s]\n",
                  key, value, ts, ctx->poll->prefix, ctx->poll->host);

    irondb_send(ctx->irondb, buffer.data, buffer.len, ts);
    buffer_reset(&buffer);

    return true;
}

static void irondb_dump(
        void *ctx_, enum optics_poll_type type, const struct optics_poll *poll)
{
    if (type != optics_poll_metric) return;

    struct dump_ctx ctx = { .irondb = ctx_, .poll = poll };
    (void) optics_poll_normalize(poll, irondb_dump_normalized, &ctx);
}

static void irondb_free(void *ctx)
{
    struct irondb *irondb = ctx;
    close(irondb->fd);
    free((void *) irondb->host);
    free((void *) irondb->port);
    free(irondb);
}


// -----------------------------------------------------------------------------
// register
// -----------------------------------------------------------------------------


void optics_dump_irondb(struct optics_poller *poller, const char *host, const char *port)
{
    struct irondb *irondb = calloc(1, sizeof(*irondb));
    optics_assert_alloc(irondb);
    irondb->host = strdup(host);
    irondb->port = strdup(port);

    optics_poller_backend(poller, irondb, &irondb_dump, &irondb_free);
}
