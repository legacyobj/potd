#include "parser.h"
#include "pot.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HARD_CONNECTION_LIMIT 1024U
#define SOURCE_SLOTS 512U
#define SOURCE_RETENTION_MS UINT64_C(60000)
#define ACCEPT_BATCH 32U
#define SHUTDOWN_MS UINT64_C(3000)

struct config {
    const char *bind_address;
    unsigned port, coffee, tea, connections, per_ip, global_rate, per_ip_rate;
    unsigned header_ms, body_ms, write_ms;
};

struct bucket { double tokens; uint64_t updated_ms; };
struct source {
    unsigned char key[16];
    int family;
    unsigned connections;
    uint64_t seen_ms;
    struct bucket bucket;
};

struct connection {
    int fd;
    size_t source_id;
    uint64_t deadline_ms;
    uint64_t started_ms;
    bool writing;
    size_t received;
    size_t sent;
    struct parser parser;
    struct request request;
    struct response response;
    unsigned char input[MAX_REQUEST_BYTES];
};

struct server {
    struct config config;
    struct inventory inventory;
    struct source sources[SOURCE_SLOTS];
    struct bucket admission;
    struct connection *connections;
    struct pollfd *pollfds;
    unsigned active;
    uint64_t accepted, rejected, completed, timed_out;
};

static volatile sig_atomic_t stopping;
static struct bucket log_bucket;
static uint64_t suppressed_logs;
static size_t log_pot_count;

static uint64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) _exit(1);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void increment(uint64_t *value)
{
    if (*value < UINT64_MAX) ++*value;
}

static void refill(struct bucket *bucket, uint64_t now, unsigned rate, unsigned burst)
{
    if (now > bucket->updated_ms) {
        bucket->tokens += (double)(now - bucket->updated_ms) * rate / 1000.0;
        if (bucket->tokens > burst) bucket->tokens = burst;
        bucket->updated_ms = now;
    }
}

/* At most one nonblocking write per record. Never log client-supplied text. */
static void log_record(const char *event, const char *peer, int status, int pot,
                       uint64_t detail, bool essential)
{
    uint64_t now = now_ms();
    char line[384], pot_name[16];
    refill(&log_bucket, now, 5, 20);
    if (!essential && log_bucket.tokens < 1) { increment(&suppressed_logs); return; }
    if (!essential) log_bucket.tokens -= 1;
    if (pot >= 0) (void)snprintf(pot_name, sizeof(pot_name), "%d", pot);
    else (void)snprintf(pot_name, sizeof(pot_name), "%s",
                       pot == RESPONSE_POT_ALL ? "all" : "none");
    int n = snprintf(line, sizeof(line),
        "time=%lld event=%s peer=%s status=%d pot=%s pots=%zu detail=%" PRIu64
        " suppressed=%" PRIu64 "\n", (long long)time(NULL), event, peer,
        status, pot_name, log_pot_count, detail, suppressed_logs);
    if (n > 0 && (size_t)n < sizeof(line) && write(STDERR_FILENO, line, (size_t)n) == n)
        suppressed_logs = 0;
    else increment(&suppressed_logs);
}

static void signal_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int descriptor_flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && descriptor_flags >= 0 &&
           fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

static void source_key(const struct sockaddr_storage *address, unsigned char key[16], int *family)
{
    memset(key, 0, 16);
    *family = address->ss_family;
    if (*family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        memcpy(key, &v4->sin_addr, 4);
    } else {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        /* Aggregate IPv6 by /64 to bound address-rotation within a subnet. */
        memcpy(key, &v6->sin6_addr, 8);
    }
}

static void peer_name(const struct source *source, char *out, size_t capacity)
{
    if (!inet_ntop(source->family, source->key, out, (socklen_t)capacity))
        (void)snprintf(out, capacity, "unknown");
}

static int source_slot(struct server *server, const struct sockaddr_storage *address, uint64_t now)
{
    unsigned char key[16];
    int family, available = -1;
    source_key(address, key, &family);
    for (unsigned i = 0; i < SOURCE_SLOTS; ++i) {
        struct source *source = &server->sources[i];
        if (source->family == family && !memcmp(source->key, key, sizeof(key))) {
            source->seen_ms = now;
            return (int)i;
        }
        if (!source->family || (!source->connections && now - source->seen_ms >= SOURCE_RETENTION_MS))
            available = (int)i;
    }
    if (available >= 0) {
        struct source *source = &server->sources[available];
        memset(source, 0, sizeof(*source));
        memcpy(source->key, key, sizeof(key));
        source->family = family;
        source->seen_ms = now;
        source->bucket.tokens = server->config.per_ip_rate * 2;
        source->bucket.updated_ms = now;
    }
    return available;
}

static void close_connection(struct server *server, struct connection *connection)
{
    close(connection->fd);
    --server->sources[connection->source_id].connections;
    --server->active;
    connection->fd = -1;
}

static void respond(struct server *server, struct connection *connection, int parse_status, uint64_t now)
{
    if (parse_status == 1)
        protocol_handle(&server->inventory, &connection->request, now, &connection->response);
    else protocol_error(&server->inventory, &connection->request, parse_status, &connection->response);
    connection->writing = true;
    connection->deadline_ms = now + server->config.write_ms;
    char peer[INET6_ADDRSTRLEN];
    peer_name(&server->sources[connection->source_id], peer, sizeof(peer));
    const char *event = !strcmp(connection->request.method, "BREW") ? "brew" :
                        !strcmp(connection->request.method, "POST") ? "post" :
                        !strcmp(connection->request.method, "WHEN") ? "when" : "request";
    log_record(event, peer, connection->response.status, connection->response.pot_id,
               now - connection->started_ms, false);
}

static void read_connection(struct server *server, struct connection *connection, uint64_t now)
{
    if (connection->received == sizeof(connection->input)) {
        respond(server, connection, 413, now);
        return;
    }
    ssize_t count = recv(connection->fd, connection->input + connection->received,
                         sizeof(connection->input) - connection->received, 0);
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            close_connection(server, connection);
        return;
    }
    if (!count) {
        if (connection->received) respond(server, connection, 400, now);
        else close_connection(server, connection);
        return;
    }
    connection->received += (size_t)count;
    bool had_headers = connection->parser.headers_done;
    int status = request_parse(&connection->parser, &connection->request,
                               connection->input, connection->received);
    if (status) respond(server, connection, status, now);
    else if (!had_headers && connection->parser.headers_done)
        connection->deadline_ms = now + server->config.body_ms;
}

static void write_connection(struct server *server, struct connection *connection)
{
    ssize_t count = send(connection->fd, connection->response.wire + connection->sent,
                         connection->response.length - connection->sent, 0);
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            close_connection(server, connection);
        return;
    }
    if (!count) { close_connection(server, connection); return; }
    connection->sent += (size_t)count;
    if (connection->sent == connection->response.length) {
        increment(&server->completed);
        close_connection(server, connection);
    }
}

static void accept_connections(struct server *server, int listener, uint64_t now, uint64_t *backoff)
{
    for (unsigned attempt = 0; attempt < ACCEPT_BATCH && !stopping; ++attempt) {
        if (server->admission.tokens < 1) return;
        struct sockaddr_storage address;
        socklen_t length = sizeof(address);
        int fd = accept(listener, (struct sockaddr *)&address, &length);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            /* Descriptor exhaustion and other persistent failures must not spin. */
            *backoff = now + 1000;
            log_record("accept_error", "-", 0, -1, (uint64_t)errno, false);
            return;
        }
        server->admission.tokens -= 1;
        int source_id = source_slot(server, &address, now);
        struct source *source = source_id >= 0 ? &server->sources[source_id] : NULL;
        if (source) refill(&source->bucket, now, server->config.per_ip_rate, server->config.per_ip_rate * 2);
        if (!source || server->active >= server->config.connections ||
            source->connections >= server->config.per_ip || source->bucket.tokens < 1 || !nonblocking(fd)) {
            close(fd);
            increment(&server->rejected);
            continue;
        }
        source->bucket.tokens -= 1;
        for (unsigned i = 0; i < server->config.connections; ++i) {
            struct connection *connection = &server->connections[i];
            if (connection->fd >= 0) continue;
            memset(connection, 0, sizeof(*connection));
            connection->fd = fd;
            connection->source_id = (size_t)source_id;
            connection->started_ms = now;
            connection->deadline_ms = now + server->config.header_ms;
            ++source->connections;
            ++server->active;
            increment(&server->accepted);
            break;
        }
    }
}

static int run_server(struct server *server, int listener)
{
    uint64_t backoff = 0, shutdown_deadline = 0, next_summary = now_ms() + 60000;
    int result = 0;
    for (;;) {
        uint64_t now = now_ms();
        if (stopping && !shutdown_deadline) {
            shutdown_deadline = now + SHUTDOWN_MS;
            close(listener);
            listener = -1;
            log_record("draining", "-", 0, -1, server->active, true);
        }
        if (shutdown_deadline && (!server->active || now >= shutdown_deadline)) break;
        refill(&server->admission, now, server->config.global_rate, server->config.global_rate * 2);
        for (size_t i = 0; i < server->inventory.count; ++i)
            if (pot_tick(&server->inventory.pots[i], now))
                log_record("brew_complete", "-", 200, (int)i, 0, false);
        for (unsigned i = 0; i < server->config.connections; ++i) {
            struct connection *connection = &server->connections[i];
            if (connection->fd < 0 || now < connection->deadline_ms) continue;
            increment(&server->timed_out);
            if (connection->writing) close_connection(server, connection);
            else respond(server, connection, 408, now);
        }
        if (now >= next_summary) {
            log_record("accepted_total", "-", 0, -1, server->accepted, true);
            log_record("rejected_total", "-", 0, -1, server->rejected, true);
            log_record("completed_total", "-", 0, -1, server->completed, true);
            log_record("timeouts_total", "-", 0, -1, server->timed_out, true);
            next_summary = now + 60000;
        }
        server->pollfds[0].fd = now >= backoff && server->admission.tokens >= 1 ? listener : -1;
        server->pollfds[0].events = POLLIN;
        int timeout = 100;
        for (unsigned i = 0; i < server->config.connections; ++i) {
            struct connection *connection = &server->connections[i];
            struct pollfd *pollfd = &server->pollfds[i + 1];
            pollfd->fd = connection->fd;
            pollfd->events = connection->writing ? POLLOUT : POLLIN;
            if (connection->fd >= 0 && connection->deadline_ms > now &&
                connection->deadline_ms - now < (uint64_t)timeout)
                timeout = (int)(connection->deadline_ms - now);
        }
        int ready = poll(server->pollfds, server->config.connections + 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            log_record("poll_error", "-", 0, -1, (uint64_t)errno, true);
            result = 1;
            break;
        }
        now = now_ms();
        for (unsigned i = 0; i < server->config.connections; ++i) {
            struct connection *connection = &server->connections[i];
            short events = server->pollfds[i + 1].revents;
            if (connection->fd < 0 || !events) continue;
            /* Recheck after poll: a byte arriving at the deadline cannot extend it. */
            if (now >= connection->deadline_ms) {
                increment(&server->timed_out);
                if (connection->writing) close_connection(server, connection);
                else respond(server, connection, 408, now);
            } else if (events & (POLLERR | POLLNVAL)) close_connection(server, connection);
            else if (connection->writing) {
                if (events & POLLOUT) write_connection(server, connection);
                else if (events & POLLHUP) close_connection(server, connection);
            } else if (events & (POLLIN | POLLHUP)) read_connection(server, connection, now);
        }
        if (!stopping && (server->pollfds[0].revents & POLLIN))
            accept_connections(server, listener, now, &backoff);
        if (server->pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_record("listener_error", "-", 0, -1, 0, true);
            result = 1;
            break;
        }
    }
    if (listener >= 0) close(listener);
    for (unsigned i = 0; i < server->config.connections; ++i)
        if (server->connections[i].fd >= 0) close_connection(server, &server->connections[i]);
    log_record("stopped", "-", result, -1, server->completed, true);
    return result;
}

static bool number(const char *text, unsigned minimum, unsigned maximum, unsigned *out)
{
    unsigned value = 0;
    if (!*text) return false;
    for (const unsigned char *s = (const unsigned char *)text; *s; ++s) {
        if (*s < '0' || *s > '9' || value > maximum / 10) return false;
        value = value * 10 + (unsigned)(*s - '0');
        if (value > maximum) return false;
    }
    if (value < minimum) return false;
    *out = value;
    return true;
}

static void usage(FILE *out)
{
    fputs("Usage: potd [options]\n"
          "  --bind ADDRESS             Numeric IPv4 or IPv6 address (127.0.0.1)\n"
          "  --port NUMBER              TCP port, 0 selects a free port (8080)\n"
          "  --coffee-pots NUMBER       Coffee pots, 0..16 (1)\n"
          "  --tea-pots NUMBER          Tea pots, 0..16 (2); total 1..16\n"
          "  --max-connections NUMBER   Concurrent clients, 1..1024 (128)\n"
          "  --per-ip-connections N     Concurrent clients per source (16)\n"
          "  --global-rate NUMBER       Accepted sockets/second, 1..10000 (200)\n"
          "  --per-ip-rate NUMBER       Requests/second per source, 1..10000 (16)\n"
          "  --header-timeout-ms N      Absolute header deadline, 100..60000 (5000)\n"
          "  --body-timeout-ms N        Absolute body deadline, 100..60000 (5000)\n"
          "  --write-timeout-ms N       Absolute write deadline, 100..60000 (3000)\n"
          "  --help                     Show this help\n", out);
}

static bool parse_config(int argc, char **argv, struct config *config)
{
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        unsigned *destination = NULL, minimum = 1, maximum = HARD_CONNECTION_LIMIT;
        if (!strcmp(option, "--help")) { usage(stdout); exit(0); }
        if (i + 1 >= argc) return false;
        const char *value = argv[++i];
        if (!strcmp(option, "--bind")) { config->bind_address = value; continue; }
        if (!strcmp(option, "--port")) { destination = &config->port; minimum = 0; maximum = 65535; }
        else if (!strcmp(option, "--coffee-pots")) { destination = &config->coffee; minimum = 0; maximum = MAX_POTS; }
        else if (!strcmp(option, "--tea-pots")) { destination = &config->tea; minimum = 0; maximum = MAX_POTS; }
        else if (!strcmp(option, "--max-connections")) destination = &config->connections;
        else if (!strcmp(option, "--per-ip-connections")) destination = &config->per_ip;
        else if (!strcmp(option, "--global-rate")) { destination = &config->global_rate; maximum = 10000; }
        else if (!strcmp(option, "--per-ip-rate")) { destination = &config->per_ip_rate; maximum = 10000; }
        else if (!strcmp(option, "--header-timeout-ms")) { destination = &config->header_ms; minimum = 100; maximum = 60000; }
        else if (!strcmp(option, "--body-timeout-ms")) { destination = &config->body_ms; minimum = 100; maximum = 60000; }
        else if (!strcmp(option, "--write-timeout-ms")) { destination = &config->write_ms; minimum = 100; maximum = 60000; }
        else return false;
        if (!number(value, minimum, maximum, destination)) return false;
    }
    if (config->per_ip > config->connections) config->per_ip = config->connections;
    return config->coffee + config->tea > 0 && config->coffee + config->tea <= MAX_POTS;
}

static int listen_socket(const struct config *config, unsigned *bound_port)
{
    struct sockaddr_storage address;
    socklen_t length;
    int family, on = 1;
    memset(&address, 0, sizeof(address));
    struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
    if (inet_pton(AF_INET, config->bind_address, &v4->sin_addr) == 1) {
        family = AF_INET;
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)config->port);
        length = sizeof(*v4);
    } else if (inet_pton(AF_INET6, config->bind_address, &v6->sin6_addr) == 1) {
        family = AF_INET6;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)config->port);
        length = sizeof(*v6);
    } else { errno = EINVAL; return -1; }
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (!nonblocking(fd) || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) != 0) ||
        bind(fd, (struct sockaddr *)&address, length) != 0 ||
        listen(fd, (int)config->connections) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    *bound_port = ntohs(family == AF_INET ? v4->sin_port : v6->sin6_port);
    return fd;
}

int main(int argc, char **argv)
{
    struct server server;
    memset(&server, 0, sizeof(server));
    server.config = (struct config){"127.0.0.1", 8080, 1, 2, 128, 16, 200, 16, 5000, 5000, 3000};
    if (!parse_config(argc, argv, &server.config)) { usage(stderr); return 2; }
    inventory_init(&server.inventory, server.config.coffee, server.config.tea);
    log_pot_count = server.inventory.count;
    if (!nonblocking(STDERR_FILENO)) { fputs("Cannot configure nonblocking logging.\n", stderr); return 1; }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = signal_stop;
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) return 1;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) != 0) return 1;
    struct rlimit descriptors;
    if (getrlimit(RLIMIT_NOFILE, &descriptors) != 0 ||
        descriptors.rlim_cur < (rlim_t)server.config.connections + 8) {
        log_record("descriptor_limit_too_low", "-", 0, -1, server.config.connections + 8, true);
        return 1;
    }
    server.connections = calloc(server.config.connections, sizeof(*server.connections));
    server.pollfds = calloc(server.config.connections + 1, sizeof(*server.pollfds));
    if (!server.connections || !server.pollfds) {
        free(server.connections);
        free(server.pollfds);
        log_record("allocation_failed", "-", 0, -1, 0, true);
        return 1;
    }
    for (unsigned i = 0; i < server.config.connections; ++i) server.connections[i].fd = -1;
    server.admission.tokens = server.config.global_rate * 2;
    server.admission.updated_ms = now_ms();
    unsigned port;
    int listener = listen_socket(&server.config, &port);
    int result = 1;
    if (listener < 0) log_record("listen_failed", "-", 0, -1, (uint64_t)errno, true);
    else {
        log_record("listening", server.config.bind_address, 0, -1, port, true);
        result = run_server(&server, listener);
    }
    free(server.connections);
    free(server.pollfds);
    return result;
}
