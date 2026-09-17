#ifndef HTCPCP_PROTOCOL_H
#define HTCPCP_PROTOCOL_H

#include "parser.h"
#include "pot.h"

#define MAX_RESPONSE_BYTES 16384U

enum response_pot { RESPONSE_POT_ALL = -2, RESPONSE_POT_NONE = -1 };

struct response {
    char wire[MAX_RESPONSE_BYTES];
    size_t length;
    int status;
    int pot_id;
};

void protocol_error(const struct inventory *inventory, const struct request *request, int status,
                    struct response *response);
void protocol_handle(struct inventory *inventory, const struct request *request,
                     uint64_t now, struct response *response);

#endif
