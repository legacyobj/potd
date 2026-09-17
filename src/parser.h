#ifndef HTCPCP_PARSER_H
#define HTCPCP_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_HEADER_BYTES 8192U
#define MAX_LINE_BYTES 2048U
#define MAX_HEADERS 32U
#define MAX_COMMAND_BYTES 64U
#define MAX_BODY_BYTES 4096U
#define MAX_TARGET_BYTES 1024U
#define MAX_ADDITIONS_BYTES 1024U
#define MAX_REQUEST_BYTES (MAX_HEADER_BYTES + MAX_BODY_BYTES)

enum wire_version { WIRE_HTTP11, WIRE_HTTP10, WIRE_HTCPCP };

struct request {
    enum wire_version version;
    char method[32];
    char target[MAX_TARGET_BYTES + 1];
    char host[256];
    char content_type[128];
    char additions[MAX_ADDITIONS_BYTES + 1];
    char depth[16];
    unsigned char body[MAX_BODY_BYTES + 1];
    size_t content_length;
    bool has_length;
    bool has_additions;
};

struct parser {
    size_t scan;
    size_t line_start;
    size_t body_start;
    unsigned header_count;
    unsigned seen;
    bool first_line;
    bool headers_done;
};

/* Return 0 for more input, 1 for a request, or an HTTP error status. */
int request_parse(struct parser *parser, struct request *request,
                  const unsigned char *input, size_t length);
bool ascii_equal(const char *a, const char *b);
bool token_char(unsigned char c);
bool valid_authority(const char *authority);
const char *wire_name(enum wire_version version);

#endif
