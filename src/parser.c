#include "parser.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

static unsigned char lower_ascii(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

bool ascii_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower_ascii((unsigned char)*a++) != lower_ascii((unsigned char)*b++))
            return false;
    }
    return *a == *b;
}

bool token_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

const char *wire_name(enum wire_version version)
{
    switch (version) {
    case WIRE_HTCPCP: return "HTCPCP/1.0";
    case WIRE_HTTP10: return "HTTP/1.0";
    default: return "HTTP/1.1";
    }
}

bool valid_authority(const char *authority)
{
    const char *port = NULL;
    size_t n = strlen(authority);
    if (n == 0 || n >= 256) return false;
    if (*authority == '[') {
        const char *end = strchr(authority, ']');
        unsigned char address[16];
        char literal[INET6_ADDRSTRLEN];
        if (!end || end == authority + 1 ||
            (size_t)(end - authority - 1) >= sizeof(literal)) return false;
        memcpy(literal, authority + 1, (size_t)(end - authority - 1));
        literal[end - authority - 1] = '\0';
        if (inet_pton(AF_INET6, literal, address) != 1) return false;
        if (end[1] == ':') port = end + 2;
        else if (end[1]) return false;
    } else {
        const char *colon = strchr(authority, ':');
        size_t host_length = colon ? (size_t)(colon - authority) : n;
        size_t label = 0;
        if (!host_length || host_length > 253) return false;
        for (size_t i = 0; i < host_length; ++i) {
            unsigned char c = (unsigned char)authority[i];
            if (c == '.') {
                if (!label) return false;
                label = 0;
            } else {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-')) return false;
                if (++label > 63) return false;
            }
        }
        if (colon) port = colon + 1;
    }
    if (port) {
        unsigned value = 0;
        if (!*port) return false;
        for (size_t i = 0; port[i]; ++i) {
            if (i >= 5 || port[i] < '0' || port[i] > '9') return false;
            value = value * 10 + (unsigned)(port[i] - '0');
        }
        if (!value || value > 65535) return false;
    }
    return true;
}

static int request_line(struct request *r, char *line)
{
    char *target = strchr(line, ' ');
    char *version;
    if (!target || target == line) return 400;
    *target++ = '\0';
    if (strlen(line) >= sizeof(r->method)) return 400;
    for (const unsigned char *s = (unsigned char *)line; *s; ++s)
        if (!token_char(*s)) return 400;
    strcpy(r->method, line);
    version = strchr(target, ' ');
    if (!version || version == target) return 400;
    *version++ = '\0';
    if (strchr(version, ' ') || strchr(version, '\t')) return 400;
    if (!strcmp(version, "HTCPCP/1.0")) r->version = WIRE_HTCPCP;
    else if (!strcmp(version, "HTTP/1.0")) r->version = WIRE_HTTP10;
    else if (!strcmp(version, "HTTP/1.1")) r->version = WIRE_HTTP11;
    else return 505;
    if (strlen(target) > MAX_TARGET_BYTES) return 414;
    for (const unsigned char *s = (unsigned char *)target; *s; ++s)
        if (*s <= 32 || *s >= 127) return 400;
    strcpy(r->target, target);
    return 0;
}

enum header_bits {
    H_HOST = 1U, H_LENGTH = 2U, H_TYPE = 4U, H_TRANSFER = 8U,
    H_ENCODING = 16U, H_EXPECT = 32U, H_DEPTH = 64U
};

static int header_line(struct parser *p, struct request *r, char *line)
{
    char *colon = strchr(line, ':');
    char *value, *end;
    unsigned bit = 0;
    if (!colon || colon == line) return 400;
    if ((size_t)(colon - line) > 63) return 431;
    for (char *s = line; s < colon; ++s)
        if (!token_char((unsigned char)*s)) return 400;
    *colon = '\0';
    value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = '\0';
    if (ascii_equal(line, "Host")) bit = H_HOST;
    else if (ascii_equal(line, "Content-Length")) bit = H_LENGTH;
    else if (ascii_equal(line, "Content-Type")) bit = H_TYPE;
    else if (ascii_equal(line, "Transfer-Encoding")) bit = H_TRANSFER;
    else if (ascii_equal(line, "Content-Encoding")) bit = H_ENCODING;
    else if (ascii_equal(line, "Expect")) bit = H_EXPECT;
    else if (ascii_equal(line, "Depth")) bit = H_DEPTH;
    if (bit && (p->seen & bit)) return 400;
    p->seen |= bit;
    if (bit && !*value) return 400;
    switch (bit) {
    case H_HOST:
        if (!valid_authority(value)) return 400;
        strcpy(r->host, value);
        break;
    case H_LENGTH: {
        size_t length = 0;
        bool oversized = false;
        for (const char *s = value; *s; ++s) {
            if (*s < '0' || *s > '9') return 400;
            if (length > (MAX_BODY_BYTES - (size_t)(*s - '0')) / 10)
                oversized = true;
            if (!oversized) length = length * 10 + (size_t)(*s - '0');
        }
        /* Defer size rejection so TE plus CL is always rejected as ambiguous. */
        r->content_length = oversized ? MAX_BODY_BYTES + 1 : length;
        r->has_length = true;
        break;
    }
    case H_TYPE:
        if (strlen(value) >= sizeof(r->content_type)) return 431;
        strcpy(r->content_type, value);
        break;
    case H_DEPTH:
        if (strlen(value) >= sizeof(r->depth)) return 400;
        strcpy(r->depth, value);
        break;
    default: break;
    }
    if (ascii_equal(line, "Accept-Additions")) {
        size_t existing = strlen(r->additions), extra = strlen(value);
        if (existing + extra + (r->has_additions ? 1U : 0U) > MAX_ADDITIONS_BYTES)
            return 431;
        if (r->has_additions) r->additions[existing++] = ',';
        memcpy(r->additions + existing, value, extra + 1);
        r->has_additions = true;
    }
    return 0;
}

static int finish_headers(const struct parser *p, const struct request *r)
{
    if (r->version == WIRE_HTTP11 && !(p->seen & H_HOST)) return 400;
    if ((p->seen & H_TRANSFER) && r->has_length) return 400;
    if (p->seen & H_TRANSFER) return 501;
    if (p->seen & H_EXPECT) return 417;
    if (p->seen & H_ENCODING) return 415;
    size_t limit = !strcmp(r->method, "PROPFIND") ? MAX_BODY_BYTES : MAX_COMMAND_BYTES;
    if (r->content_length > limit) return 413;
    return 0;
}

int request_parse(struct parser *p, struct request *r,
                  const unsigned char *input, size_t length)
{
    if (length > MAX_REQUEST_BYTES) return 413;
    while (!p->headers_done && p->scan < length) {
        unsigned char c = input[p->scan];
        if (p->scan >= MAX_HEADER_BYTES) return 431;
        if (p->scan - p->line_start > MAX_LINE_BYTES)
            return p->first_line ? 431 : 414;
        if (c == '\r') {
            if (p->scan + 1 == length) return 0;
            if (input[p->scan + 1] != '\n') return 400;
        } else {
            if (c == '\n' || (c < 32 && c != '\t') || c >= 127) return 400;
            ++p->scan;
            continue;
        }
        size_t count = p->scan - p->line_start;
        char line[MAX_LINE_BYTES + 1];
        int status;
        if (p->scan + 2 > MAX_HEADER_BYTES) return 431;
        memcpy(line, input + p->line_start, count);
        line[count] = '\0';
        p->scan += 2;
        p->line_start = p->scan;
        if (!p->first_line) {
            status = request_line(r, line);
            p->first_line = true;
        } else if (!count) {
            status = finish_headers(p, r);
            if (!status) {
                p->headers_done = true;
                p->body_start = p->scan;
            }
        } else {
            if (++p->header_count > MAX_HEADERS) return 431;
            status = header_line(p, r, line);
        }
        if (status) return status;
    }
    if (!p->headers_done) return length >= MAX_HEADER_BYTES ? 431 : 0;
    if (length - p->body_start < r->content_length) return 0;
    memcpy(r->body, input + p->body_start, r->content_length);
    r->body[r->content_length] = '\0';
    return 1;
}
