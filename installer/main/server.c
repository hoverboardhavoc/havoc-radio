#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dns_reply.h"
#include "update_session.h"

#define TICK_MS 20
#define STREAM_CHUNK 4096

typedef struct {
    int fd;
    bool want_write;
    uint8_t ip[4];
    update_session us;
} session;

static uint8_t s_chunk[STREAM_CHUNK];

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int bound_socket(int type, uint32_t addr, uint16_t port)
{
    int fd = socket(AF_INET, type, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = addr;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || set_nonblock(fd) < 0 ||
        (type == SOCK_STREAM && listen(fd, 2) < 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_dns(int fd, const server_config *cfg)
{
    uint8_t query[512];
    uint8_t reply[600];
    struct sockaddr_in from;
    socklen_t from_len = sizeof from;
    ssize_t n = recvfrom(fd, query, sizeof query, 0, (struct sockaddr *)&from, &from_len);
    if (n <= 0)
        return;
    char name[DNS_NAME_MAX];
    uint16_t qtype;
    size_t len = dns_reply(query, (size_t)n, cfg->answer_ip, reply, sizeof reply, name, &qtype);
    if (len == 0) {
        platform_log("dns: dropped a %d-byte packet from %s", (int)n, inet_ntoa(from.sin_addr));
        return;
    }
    platform_log("dns: %s (type %u) from %s", name, qtype, inet_ntoa(from.sin_addr));
    sendto(fd, reply, len, 0, (struct sockaddr *)&from, from_len);
    platform_dns_query((const uint8_t *)&from.sin_addr.s_addr, name);
}

const char *server_image_problem(uint32_t len)
{
    uint8_t first = 0;
    uint8_t head[US_HEAD_LEN] = {0};
    if (!platform_image_read(0, &first, 1) ||
        (len >= US_HEAD_AT + US_HEAD_LEN && !platform_image_read(US_HEAD_AT, head, US_HEAD_LEN)))
        return "image read failed";
    return update_image_problem(len, first, head);
}

static void accept_connection(int lfd, session *s, uint32_t now)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof peer;
    int fd = accept(lfd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0)
        return;
    if (s->fd >= 0) {
        platform_log("update: %s connected during a session, closed", inet_ntoa(peer.sin_addr));
        close(fd);
        return;
    }
    if (set_nonblock(fd) < 0) {
        close(fd);
        return;
    }
    s->fd = fd;
    s->want_write = false;
    memcpy(s->ip, &peer.sin_addr.s_addr, 4);
    uint32_t len = 0;
    const char *problem = platform_image_hold(&len);
    if (problem == NULL)
        problem = server_image_problem(len);
    update_start(&s->us, len, problem == NULL, now);
    platform_log("update: session from %s, image %lu bytes, %s", inet_ntoa(peer.sin_addr), (unsigned long)len,
                 problem ? problem : "ready");
    platform_session(s->ip, &s->us, false);
}

static void session_read(session *s, uint32_t now)
{
    uint8_t buf[256];
    for (;;) {
        ssize_t n = recv(s->fd, buf, sizeof buf, 0);
        if (n > 0) {
            update_received(&s->us, buf, (size_t)n, now);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            update_peer_closed(&s->us);
        return;
    }
}

static void session_close(session *s)
{
    close(s->fd);
    s->fd = -1;
    platform_image_release();
    platform_log("update: session end, MAC %s, %lu of %lu bytes sent, %s", s->us.mac_len ? s->us.mac : "none",
                 (unsigned long)s->us.sent, (unsigned long)s->us.image_len, update_outcome_name(s->us.outcome));
    platform_session(s->ip, &s->us, true);
}

// Runs the session's actions until it has to wait for the peer or the clock.
// Returns false when it closed the session.
static bool session_run(session *s, uint32_t now)
{
    s->want_write = false;
    for (;;) {
        update_action a = update_next(&s->us, now);
        if (a == US_NONE)
            return true;
        if (a == US_CLOSE) {
            session_close(s);
            return false;
        }
        const uint8_t *data;
        size_t len;
        if (a == US_SEND_HEADER) {
            data = (const uint8_t *)s->us.header;
            len = s->us.header_len;
        } else {
            data = s_chunk;
            len = s->us.image_len - s->us.sent;
            if (len > STREAM_CHUNK)
                len = STREAM_CHUNK;
            if (!platform_image_read(s->us.sent, s_chunk, len)) {
                platform_log("update: image read failed at %lu", (unsigned long)s->us.sent);
                update_peer_closed(&s->us);
                continue;
            }
        }
        ssize_t n = send(s->fd, data, len, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            s->want_write = true;
            return true;
        }
        if (n <= 0) {
            update_peer_closed(&s->us);
        } else if (a == US_SEND_HEADER) {
            // The header is the first write on the connection and fits any
            // send buffer, so a short write means the connection failed.
            if ((size_t)n == len)
                update_header_sent(&s->us, now);
            else
                update_peer_closed(&s->us);
        } else {
            update_sent(&s->us, (size_t)n);
        }
    }
}

static void session_step(session *s, uint32_t now)
{
    uint32_t sent = s->us.sent;
    if (session_run(s, now) && s->us.sent != sent)
        platform_session(s->ip, &s->us, false);
}

int server_run(const server_config *cfg)
{
    int dns = bound_socket(SOCK_DGRAM, cfg->bind_addr, cfg->dns_port);
    int lfd = bound_socket(SOCK_STREAM, cfg->bind_addr, cfg->update_port);
    if (dns < 0 || lfd < 0) {
        platform_log("server: cannot bind (errno %d)", errno);
        if (dns >= 0)
            close(dns);
        if (lfd >= 0)
            close(lfd);
        return -1;
    }
    session s = {.fd = -1};
    platform_log("ready: dns port %u, update port %u", cfg->dns_port, cfg->update_port);

    for (;;) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(dns, &rd);
        FD_SET(lfd, &rd);
        int maxfd = dns > lfd ? dns : lfd;
        int sfd = s.fd;
        if (sfd >= 0) {
            FD_SET(sfd, &rd);
            if (s.want_write)
                FD_SET(sfd, &wr);
            if (sfd > maxfd)
                maxfd = sfd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = TICK_MS * 1000};
        int r = select(maxfd + 1, &rd, &wr, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            platform_log("server: select failed (errno %d)", errno);
            return -1;
        }
        uint32_t now = platform_now_ms();
        if (r > 0 && FD_ISSET(dns, &rd))
            handle_dns(dns, cfg);
        if (r > 0 && FD_ISSET(lfd, &rd))
            accept_connection(lfd, &s, now);
        if (sfd >= 0 && FD_ISSET(sfd, &rd))
            session_read(&s, now);
        if (s.fd >= 0)
            session_step(&s, now);
    }
}
