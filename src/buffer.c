#include "buffer.h"

#include <stdarg.h>
#include <stdio.h>

void buffer_append(struct buffer *b, const char *format, ...)
{
    va_list args;
    int count;
    if (b->failed) return;
    va_start(args, format);
    count = vsnprintf(b->data + b->length, b->capacity - b->length, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= b->capacity - b->length) b->failed = true;
    else b->length += (size_t)count;
}

void buffer_xml(struct buffer *b, const char *text)
{
    for (const unsigned char *s = (const unsigned char *)text; *s; ++s) {
        switch (*s) {
        case '&': buffer_append(b, "&amp;"); break;
        case '<': buffer_append(b, "&lt;"); break;
        case '>': buffer_append(b, "&gt;"); break;
        case '"': buffer_append(b, "&quot;"); break;
        case '\'': buffer_append(b, "&apos;"); break;
        case '\t': buffer_append(b, "&#9;"); break;
        case '\n': buffer_append(b, "&#10;"); break;
        case '\r': buffer_append(b, "&#13;"); break;
        default: buffer_append(b, "%c", *s); break;
        }
    }
}
