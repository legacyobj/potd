#ifndef HTCPCP_BUFFER_H
#define HTCPCP_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

struct buffer { char *data; size_t capacity; size_t length; bool failed; };
void buffer_append(struct buffer *buffer, const char *format, ...);
void buffer_xml(struct buffer *buffer, const char *text);

#endif
