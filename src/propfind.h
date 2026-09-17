#ifndef HTCPCP_PROPFIND_H
#define HTCPCP_PROPFIND_H

#include "buffer.h"
#include "parser.h"
#include "pot.h"

#define MAX_XML_PROPERTIES 16U
#define XML_ARENA_BYTES 131072U

int propfind_response(const struct request *request, const struct inventory *inventory,
                      const char *path, int pot_id, uint64_t now, struct buffer *body);

#endif
