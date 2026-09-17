#include "protocol.h"
#include "additions.h"
#include "buffer.h"
#include "propfind.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct target { char path[MAX_TARGET_BYTES + 1]; char query[MAX_ADDITIONS_BYTES + 1]; bool has_query; };

static int parse_target(const struct request *r, struct target *target);
static int request_pot(const struct inventory *inventory, const struct request *request);
static void alternates(struct buffer *headers, struct buffer *body,
                       const struct inventory *inventory, int only);

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 207: return "Multi-Status";
    case 300: return "Multiple Options";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}

static const char *error_detail(int status)
{
    switch (status) {
    case 400: return "Invalid request syntax or command.";
    case 403: return "Milk additions are not permitted with peppermint tea.";
    case 404: return "No such pot or tea variety.";
    case 405: return "Use a method listed in Allow.";
    case 406: return "The requested additions or preparation parameters are unavailable.";
    case 408: return "The absolute request deadline expired.";
    case 409: return "The command conflicts with the current pot state or recipe.";
    case 411: return "Brewing commands require Content-Length.";
    case 413: return "The request body exceeds the limit.";
    case 414: return "The request target exceeds the limit.";
    case 415: return "Use a supported media type and encoding for this method and pot.";
    case 417: return "Send the command without Expect.";
    case 418: return "This pot brews tea, not coffee.";
    case 431: return "Request headers exceed the configured limits.";
    case 501: return "This method or transfer coding is not implemented.";
    case 503: return "The service is temporarily unavailable.";
    case 505: return "Use HTTP/1.0, HTTP/1.1, or HTCPCP/1.0.";
    default: return "The server could not construct a response.";
    }
}

static bool read_only(const struct request *r)
{
    return !strcmp(r->method, "GET") || !strcmp(r->method, "HEAD") ||
           !strcmp(r->method, "OPTIONS") || !strcmp(r->method, "PROPFIND");
}

static void serialize(const struct request *r, int status, struct buffer *body,
                      struct buffer *headers, struct response *response)
{
    char date[64];
    struct tm utc;
    time_t now = time(NULL);
    struct buffer out = {response->wire, sizeof(response->wire), 0, false};
    response->status = status;
    buffer_append(&out, "%s %d %s\r\n", wire_name(r->version), status, reason(status));
    if (gmtime_r(&now, &utc) && strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc))
        buffer_append(&out, "Date: %s\r\n", date);
    buffer_append(&out, "Server: potd\r\nContent-Type: %s\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n"
                 "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                 "Safe: %s\r\n", status == 207 ? "application/xml; charset=utf-8" : "application/json",
                 body->length, read_only(r) ? "yes" : "no");
    if (headers->length) buffer_append(&out, "%s", headers->data);
    buffer_append(&out, "\r\n");
    if (strcmp(r->method, "HEAD")) buffer_append(&out, "%s", body->data);
    if (out.failed || body->failed || headers->failed) {
        /* Fixed fallback preserves HEAD semantics and always closes the connection. */
        int n = snprintf(response->wire, sizeof(response->wire),
            "%s 500 Internal Server Error\r\nContent-Length: 0\r\n"
            "Connection: close\r\nCache-Control: no-store\r\nSafe: no\r\n\r\n",
            wire_name(r->version));
        response->length = n > 0 ? (size_t)n : 0;
        response->status = 500;
    } else response->length = out.length;
}

static void addition_json(struct buffer *b, uint32_t mask)
{
    bool first = true;
    buffer_append(b, "[");
    for (unsigned i = 0; i < ADDITION_COUNT; ++i) {
        if (!(mask & (UINT32_C(1) << i))) continue;
        buffer_append(b, "%s\"%s\"", first ? "" : ",", addition_names[i]);
        first = false;
    }
    buffer_append(b, "]");
}

void protocol_error(const struct inventory *inventory, const struct request *r,
                    int status, struct response *response)
{
    char body_data[2048] = "", header_data[4096] = "";
    struct buffer body = {body_data, sizeof(body_data), 0, false};
    struct buffer headers = {header_data, sizeof(header_data), 0, false};
    response->pot_id = request_pot(inventory, r);
    buffer_append(&body, "{\"error\":\"%s\",\"detail\":\"%s\"", reason(status), error_detail(status));
    if (status == 406) {
        buffer_append(&body, ",\"available_additions\":");
        addition_json(&body, (UINT32_C(1) << ADDITION_COUNT) - 1);
    }
    buffer_append(&body, "}\n");
    if (status == 405)
        buffer_append(&headers, "Allow: BREW, POST, GET, HEAD, WHEN, PROPFIND, OPTIONS\r\n");
    if (inventory && (!strcmp(r->method, "BREW") || !strcmp(r->method, "POST")) &&
        (ascii_equal(r->content_type, "message/coffeepot") ||
         ascii_equal(r->content_type, "application/coffee-pot-command"))) {
        struct target target = {{0}, {0}, false};
        if (!parse_target(r, &target) && !strcmp(target.path, "/"))
            alternates(&headers, NULL, inventory, -1);
    }
    serialize(r, status, &body, &headers, response);
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool coffee_scheme(const char *scheme)
{
    static const char *const names[] = {
        "coffee", "koffie", "q%C3%A6hv%C3%A6", "%D9%82%D9%87%D9%88%D8%A9",
        "akeita", "koffee", "kahva", "kafe", "caf%C3%E8", "caf%C3%A9",
        "%E5%92%96%E5%95%A1", "kava", "k%C3%A1va", "kaffe", "kafo", "kohv",
        "kahvi", "%4Baffee", "%CE%BA%CE%B1%CF%86%CE%AD",
        "%E0%A4%95%E0%A5%8C%E0%A4%AB%E0%A5%80", "%E3%82%B3%E3%83%BC%E3%83%92%E3%83%BC",
        "%EC%BB%A4%ED%94%BC", "%D0%BA%D0%BE%D1%84%D0%B5", "%E0%B8%81%E0%B8%B2%E0%B9%81%E0%B8%9F"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (ascii_equal(scheme, names[i])) return true;
    return false;
}

static bool decode(const char *begin, size_t count, char *out, bool path)
{
    size_t j = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned char c = (unsigned char)begin[i];
        bool encoded = c == '%';
        if (encoded) {
            if (i + 2 >= count) return false;
            int high = hex_value((unsigned char)begin[i + 1]);
            int low = hex_value((unsigned char)begin[i + 2]);
            if (high < 0 || low < 0) return false;
            c = (unsigned char)(high * 16 + low);
            i += 2;
        }
        if (path) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || (c == '/' && !encoded)))
                return false;
        } else if (c < 32 || c >= 127 || c == '#' || c == '\\') return false;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return true;
}

static int parse_target(const struct request *r, struct target *target)
{
    const char *s = r->target;
    bool absolute = false;
    if (*s != '/') {
        const char *colon = strchr(s, ':');
        char scheme[160];
        bool http;
        if (!colon || (size_t)(colon - s) >= sizeof(scheme)) return 400;
        memcpy(scheme, s, (size_t)(colon - s));
        scheme[colon - s] = '\0';
        http = ascii_equal(scheme, "http") || ascii_equal(scheme, "https");
        if (!http && !coffee_scheme(scheme)) return 400;
        s = colon + 1;
        absolute = true;
        if (s[0] == '/' && s[1] == '/') {
            char authority[256];
            const char *end = s + 2;
            s += 2;
            while (*end && *end != '/' && *end != '?') ++end;
            if ((size_t)(end - s) >= sizeof(authority)) return 400;
            memcpy(authority, s, (size_t)(end - s));
            authority[end - s] = '\0';
            if (!valid_authority(authority) ||
                (*r->host && !ascii_equal(authority, r->host))) return 400;
            s = end;
        } else if (http) return 400;
    }
    const char *query = strchr(s, '?');
    size_t count = query ? (size_t)(query - s) : strlen(s);
    if (!count && absolute) strcpy(target->path, "/");
    else if (*s != '/' || !decode(s, count, target->path, true)) return 400;
    if (query) {
        target->has_query = true;
        if (!decode(query + 1, strlen(query + 1), target->query, false)) return 400;
    }
    return 0;
}

static int find_variety(const char *name)
{
    for (unsigned i = 0; i < VARIETY_COUNT; ++i)
        if (!strcmp(name, varieties[i].name)) return (int)i;
    return -1;
}

static int first_pot(const struct inventory *inventory, enum pot_kind kind)
{
    for (size_t i = 0; i < inventory->count; ++i)
        if (inventory->pots[i].kind == kind) return (int)i;
    return -1;
}

static int route(const struct inventory *inventory, const char *path, int *id, int *variety)
{
    *id = -1;
    *variety = -1;
    if (!strcmp(path, "/")) return 0;
    if (!strncmp(path, "/pot-", 5)) {
        const char *s = path + 5;
        unsigned number = 0, digits = 0;
        if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') return 404;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 2) return 404;
            number = number * 10 + (unsigned)(*s++ - '0');
        }
        if (!digits || number >= inventory->count) return 404;
        *id = (int)number;
        if (!*s) return 0;
        if (*s++ != '/' || (*variety = find_variety(s)) < 0 ||
            inventory->pots[number].kind != POT_TEA) return 404;
        return 0;
    }
    *variety = find_variety(path + 1);
    if (*variety < 0 || (*id = first_pot(inventory, POT_TEA)) < 0) return 404;
    return 0;
}

/* Resolve log context for reads and failures without changing appliance state. */
static int request_pot(const struct inventory *inventory, const struct request *r)
{
    struct target target = {{0}, {0}, false};
    int id, variety;
    if (!inventory) return RESPONSE_POT_NONE;
    if (!strcmp(r->method, "OPTIONS") && !strcmp(r->target, "*")) return RESPONSE_POT_ALL;
    if (parse_target(r, &target) || route(inventory, target.path, &id, &variety))
        return RESPONSE_POT_NONE;
    if (id >= 0) return id;
    if (!strcmp(r->method, "WHEN")) return first_pot(inventory, POT_COFFEE);
    if ((!strcmp(r->method, "BREW") || !strcmp(r->method, "POST")) &&
        (ascii_equal(r->content_type, "message/coffeepot") ||
         ascii_equal(r->content_type, "application/coffee-pot-command"))) {
        id = first_pot(inventory, POT_COFFEE);
        if (id >= 0) return id;
    }
    return RESPONSE_POT_ALL;
}

static void alternates(struct buffer *headers, struct buffer *body,
                       const struct inventory *inventory, int only)
{
    bool first = true;
    buffer_append(headers, "Alternates: ");
    if (body) buffer_append(body, "{\"alternates\":[");
    for (size_t i = 0; i < inventory->count; ++i) {
        const struct pot *pot = &inventory->pots[i];
        if (only >= 0 && i != (size_t)only) continue;
        if (pot->kind == POT_COFFEE) {
            buffer_append(headers, "%s{\"/pot-%zu\" 1 {type message/coffeepot}}", first ? "" : ", ", i);
            if (body) buffer_append(body, "%s\"/pot-%zu\"", first ? "" : ",", i);
            first = false;
        } else {
            for (unsigned v = 0; v < VARIETY_COUNT; ++v) {
                buffer_append(headers, "%s{\"/pot-%zu/%s\" 1 {type message/teapot}}",
                       first ? "" : ", ", i, varieties[v].name);
                if (body) buffer_append(body, "%s\"/pot-%zu/%s\"", first ? "" : ",", i, varieties[v].name);
                first = false;
            }
        }
    }
    buffer_append(headers, "\r\nVary: Content-Type, Accept-Additions\r\n");
    if (body) buffer_append(body, "]}\n");
}

static void pot_json(struct buffer *body, const struct pot *pot, int id, uint64_t now)
{
    uint64_t elapsed = pot_elapsed(pot, now);
    buffer_append(body, "{\"pot\":\"pot-%d\",\"kind\":\"%s\",\"state\":\"%s\",\"variety\":",
           id, pot->kind == POT_TEA ? "tea" : "coffee", pot_phase_name(pot->phase));
    if (pot->variety < 0) buffer_append(body, "null");
    else buffer_append(body, "\"%s\"", varieties[pot->variety].name);
    buffer_append(body, ",\"brew_elapsed_ms\":%" PRIu64 ",\"recommended_ms\":%" PRIu64
                 ",\"strength_percent\":%" PRIu64 ",\"batches\":%" PRIu64 ",\"additions\":",
           elapsed, pot_duration(pot), elapsed * 100 / pot_duration(pot), pot->batches);
    addition_json(body, pot->additions);
    buffer_append(body, "}");
}

void protocol_handle(struct inventory *inventory, const struct request *r,
                     uint64_t now, struct response *response)
{
    struct target target = {{0}, {0}, false};
    char body_data[MAX_RESPONSE_BYTES - 2048] = "", header_data[4096] = "";
    struct buffer body = {body_data, sizeof(body_data), 0, false};
    struct buffer headers = {header_data, sizeof(header_data), 0, false};
    bool command = !strcmp(r->method, "BREW") || !strcmp(r->method, "POST");
    bool when = !strcmp(r->method, "WHEN");
    bool options = !strcmp(r->method, "OPTIONS");
    bool propfind = !strcmp(r->method, "PROPFIND");
    int status = 200, id = -1, variety = -1;
    uint32_t additions = 0;
    struct addition_preferences preferences;
    response->pot_id = request_pot(inventory, r);
    if (!command && !when && !read_only(r)) {
        status = (!strcmp(r->method, "PUT") || !strcmp(r->method, "DELETE") ||
                  !strcmp(r->method, "PATCH") || !strcmp(r->method, "TRACE") ||
                  !strcmp(r->method, "CONNECT")) ? 405 : 501;
        goto error;
    }
    if (!command && !propfind && r->content_length) { status = 400; goto error; }
    if (options && !strcmp(r->target, "*")) goto option_response;
    status = parse_target(r, &target);
    if (status) goto error;
    status = route(inventory, target.path, &id, &variety);
    if (status) goto error;
    if (target.has_query && r->has_additions) { status = 400; goto error; }
    status = additions_parse(target.has_query ? target.query : r->additions, &preferences);
    if (status) goto error;
    if (options) goto option_response;
    if (propfind) {
        status = propfind_response(r, inventory, target.path, id, now, &body);
        if (status != 207) goto error;
        goto done;
    }
    if (!command && !when) {
        status = additions_select(&preferences, &additions);
        if (status) goto error;
        status = 200;
        if (id < 0) {
            buffer_append(&body, !strcmp(r->method, "GET") ?
                          "{\"server\":\"potd\",\"service\":\"HTCPCP\",\"protocol\":\"HTCPCP/1.0\","
                          "\"extensions\":[\"HTCPCP-TEA\"],\"rfcs\":[\"RFC 2324\",\"RFC 7168\"],\"pots\":[" :
                          "{\"pots\":[");
            for (size_t i = 0; i < inventory->count; ++i) {
                if (i) buffer_append(&body, ",");
                pot_json(&body, &inventory->pots[i], (int)i, now);
            }
            buffer_append(&body, "]}\n");
        } else { pot_json(&body, &inventory->pots[id], id, now); buffer_append(&body, "\n"); }
        goto done;
    }
    if (when) {
        if (id < 0) id = first_pot(inventory, POT_COFFEE);
        if (id < 0) { status = 404; goto error; }
        if (variety >= 0 && inventory->pots[id].variety != variety) {
            status = 409; goto error;
        }
        status = pot_when(&inventory->pots[id]);
        if (status != 200) goto error;
    } else {
        bool tea = ascii_equal(r->content_type, "message/teapot");
        bool coffee = ascii_equal(r->content_type, "message/coffeepot") ||
                      ascii_equal(r->content_type, "application/coffee-pot-command");
        bool start = r->content_length == 5 && ascii_equal((const char *)r->body, "start");
        bool stop = r->content_length == 4 && ascii_equal((const char *)r->body, "stop");
        if (tea && (id < 0 || (inventory->pots[id].kind == POT_TEA && variety < 0))) {
            alternates(&headers, &body, inventory, id);
            status = 300;
            goto done;
        }
        if (!r->has_length) { status = 411; goto error; }
        if (!tea && !coffee) { status = 415; goto error; }
        if (!start && !stop) { status = 400; goto error; }
        if (id < 0) id = first_pot(inventory, POT_COFFEE);
        if (id < 0 || (coffee && inventory->pots[id].kind == POT_TEA)) {
            status = 418; goto error;
        }
        if (tea && inventory->pots[id].kind != POT_TEA) { status = 415; goto error; }
        if (start) {
            status = additions_select(&preferences, &additions);
            if (status) goto error;
        }
        if (start && variety == 2 && (additions & MILK_MASK)) { status = 403; goto error; }
        if (!strcmp(target.path, "/")) {
            /* Coffee still brews at the root; tea clients can discover alternatives. */
            alternates(&headers, NULL, inventory, -1);
        }
        if (headers.failed || body.failed) { status = 500; goto error; }
        status = start ? pot_start(&inventory->pots[id], variety, additions, now) :
                         pot_stop(&inventory->pots[id], variety, now);
        if (status != 200) goto error;
    }
    response->pot_id = id;
    pot_json(&body, &inventory->pots[id], id, now);
    buffer_append(&body, "\n");
    goto done;

option_response:
    buffer_append(&headers, "Allow: BREW, POST, GET, HEAD, WHEN, PROPFIND, OPTIONS\r\n");
    buffer_append(&body, "{\"protocols\":[\"HTCPCP/1.0\",\"HTCPCP-TEA\"]}\n");
    status = 200;
done:
    serialize(r, status, &body, &headers, response);
    return;
error:
    protocol_error(inventory, r, status, response);
}
