#include "propfind.h"

#include <expat.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#define DAV "DAV:"
#define POT_NS "urn:potd"
#define SEPARATOR '\x1f'

union allocation_header { max_align_t alignment; size_t size; };
static union { max_align_t alignment; unsigned char bytes[XML_ARENA_BYTES]; } arena;
static size_t arena_used;

/* The single event-loop owner reuses this fixed arena between XML requests. */
static void *xml_allocate(size_t size)
{
    size_t alignment = _Alignof(max_align_t);
    if (size > XML_ARENA_BYTES - sizeof(union allocation_header) - alignment) return NULL;
    size_t total = sizeof(union allocation_header) + size;
    total = (total + alignment - 1) / alignment * alignment;
    if (total > XML_ARENA_BYTES - arena_used) return NULL;
    unsigned char *header = arena.bytes + arena_used;
    memcpy(header, &size, sizeof(size));
    arena_used += total;
    return header + sizeof(union allocation_header);
}

static void xml_release(void *pointer)
{
    (void)pointer;
}

static void *xml_resize(void *pointer, size_t size)
{
    if (!pointer) return xml_allocate(size);
    size_t old_size;
    memcpy(&old_size, (unsigned char *)pointer - sizeof(union allocation_header), sizeof(old_size));
    if (size <= old_size) return pointer;
    void *replacement = xml_allocate(size);
    if (replacement) memcpy(replacement, pointer, old_size);
    return replacement;
}

struct property_name { char namespace_uri[128]; char local[64]; };
enum query_mode { QUERY_NONE, QUERY_ALL, QUERY_NAMES, QUERY_SELECTED };
enum container_kind { CONTAINER_EMPTY, CONTAINER_PROPERTIES };

struct xml_query {
    XML_Parser parser;
    enum query_mode mode;
    enum container_kind container;
    struct property_name properties[MAX_XML_PROPERTIES];
    size_t count;
    unsigned depth, nodes, ignore_depth;
    bool included;
    int error;
};

static void xml_fail(struct xml_query *query, int status)
{
    query->error = status;
    (void)XML_StopParser(query->parser, XML_FALSE);
}

static bool dav_name(const char *name, const char *local)
{
    return !strncmp(name, DAV "\x1f", 5) && !strcmp(name + 5, local);
}

static void XMLCALL start_element(void *data, const XML_Char *name, const XML_Char **attributes)
{
    struct xml_query *query = data;
    (void)attributes;
    if (++query->nodes > 64 || ++query->depth > 8) { xml_fail(query, 413); return; }
    if (query->ignore_depth) return;
    if (query->depth == 1) {
        if (!dav_name(name, "propfind")) xml_fail(query, 400);
    } else if (query->depth == 2) {
        query->container = CONTAINER_EMPTY;
        if (dav_name(name, "include")) {
            if (query->mode != QUERY_ALL || query->included) { xml_fail(query, 400); return; }
            query->included = true;
            query->container = CONTAINER_PROPERTIES;
        } else if (dav_name(name, "allprop") || dav_name(name, "propname") || dav_name(name, "prop")) {
            if (query->mode != QUERY_NONE) { xml_fail(query, 400); return; }
            if (dav_name(name, "allprop")) query->mode = QUERY_ALL;
            else if (dav_name(name, "propname")) query->mode = QUERY_NAMES;
            else { query->mode = QUERY_SELECTED; query->container = CONTAINER_PROPERTIES; }
        } else {
            /* WebDAV ignores unrecognized additive XML extensions. */
            query->ignore_depth = query->depth;
        }
    } else if (query->depth == 3 && query->container == CONTAINER_PROPERTIES) {
        struct property_name property = {{0}, {0}};
        const char *separator = strchr(name, SEPARATOR);
        const char *local = separator ? separator + 1 : name;
        size_t ns_length = separator ? (size_t)(separator - name) : 0;
        if (ns_length >= sizeof(property.namespace_uri) || strlen(local) >= sizeof(property.local)) {
            xml_fail(query, 413); return;
        }
        memcpy(property.namespace_uri, name, ns_length);
        strcpy(property.local, local);
        for (size_t i = 0; i < query->count; ++i)
            if (!strcmp(property.namespace_uri, query->properties[i].namespace_uri) &&
                !strcmp(property.local, query->properties[i].local)) return;
        if (query->count == MAX_XML_PROPERTIES) { xml_fail(query, 413); return; }
        query->properties[query->count++] = property;
    } else xml_fail(query, 400);
}

static void XMLCALL end_element(void *data, const XML_Char *name)
{
    struct xml_query *query = data;
    (void)name;
    if (query->ignore_depth == query->depth) query->ignore_depth = 0;
    --query->depth;
}

static void XMLCALL text_content(void *data, const XML_Char *text, int length)
{
    struct xml_query *query = data;
    if (query->ignore_depth) return;
    for (int i = 0; i < length; ++i)
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') {
            xml_fail(query, 400); return;
        }
}

static void XMLCALL reject_doctype(void *data, const XML_Char *name,
                                  const XML_Char *system_id, const XML_Char *public_id, int internal)
{
    (void)name; (void)system_id; (void)public_id; (void)internal;
    xml_fail(data, 400);
}

static int XMLCALL reject_external(XML_Parser parser, const XML_Char *context,
                                   const XML_Char *base, const XML_Char *system_id,
                                   const XML_Char *public_id)
{
    (void)parser; (void)context; (void)base; (void)system_id; (void)public_id;
    return XML_STATUS_ERROR;
}

static bool xml_media(const char *value)
{
    char media[128];
    memcpy(media, value, strlen(value) + 1);
    char *parameter = strchr(media, ';');
    if (parameter) {
        *parameter++ = '\0';
        while (*parameter == ' ' || *parameter == '\t') ++parameter;
        if (!ascii_equal(parameter, "charset=utf-8") &&
            !ascii_equal(parameter, "charset=\"utf-8\"")) return false;
    }
    size_t length = strlen(media);
    while (length && (media[length - 1] == ' ' || media[length - 1] == '\t')) media[--length] = '\0';
    return ascii_equal(media, "application/xml") || ascii_equal(media, "text/xml");
}

static int parse_query(const struct request *request, struct xml_query *query)
{
    memset(query, 0, sizeof(*query));
    if (*request->depth && strcmp(request->depth, "0") && strcmp(request->depth, "1") &&
        strcmp(request->depth, "infinity")) return 400;
    if (!request->content_length) { query->mode = QUERY_ALL; return 0; }
    if (!xml_media(request->content_type)) return 415;
    XML_Memory_Handling_Suite memory = {xml_allocate, xml_resize, xml_release};
    arena_used = 0;
    query->parser = XML_ParserCreate_MM("UTF-8", &memory, "\x1f");
    if (!query->parser) return 500;
    XML_SetUserData(query->parser, query);
    XML_SetElementHandler(query->parser, start_element, end_element);
    XML_SetCharacterDataHandler(query->parser, text_content);
    XML_SetStartDoctypeDeclHandler(query->parser, reject_doctype);
    XML_SetExternalEntityRefHandler(query->parser, reject_external);
    (void)XML_SetParamEntityParsing(query->parser, XML_PARAM_ENTITY_PARSING_NEVER);
    enum XML_Status result = XML_Parse(query->parser, (const char *)request->body,
                                      (int)request->content_length, XML_TRUE);
    int error = query->error;
    if (!error && result != XML_STATUS_OK)
        error = XML_GetErrorCode(query->parser) == XML_ERROR_NO_MEMORY ? 413 : 400;
    XML_ParserFree(query->parser);
    query->parser = NULL;
    if (error) return error;
    return query->mode == QUERY_NONE ? 400 : 0;
}

enum property_id {
    DISPLAY_NAME, RESOURCE_TYPE, CONTENT_TYPE, KIND, STATE, VARIETY, ELAPSED,
    RECOMMENDED, STRENGTH, BATCHES, ADDITIONS, POT_COUNT, PROPERTY_COUNT
};

static const struct property_name properties[PROPERTY_COUNT] = {
    {DAV, "displayname"}, {DAV, "resourcetype"}, {DAV, "getcontenttype"},
    {POT_NS, "kind"}, {POT_NS, "state"}, {POT_NS, "variety"},
    {POT_NS, "brew-elapsed-ms"}, {POT_NS, "recommended-ms"},
    {POT_NS, "strength-percent"}, {POT_NS, "batches"}, {POT_NS, "additions"},
    {POT_NS, "pot-count"}
};

static int property_id(const struct property_name *name, int pot_id)
{
    for (unsigned i = 0; i < PROPERTY_COUNT; ++i) {
        bool available = i <= CONTENT_TYPE || (pot_id < 0 ? i == POT_COUNT : i != POT_COUNT);
        if (available && !strcmp(name->namespace_uri, properties[i].namespace_uri) &&
            !strcmp(name->local, properties[i].local)) return (int)i;
    }
    return -1;
}

static void property_value(struct buffer *body, unsigned property, const struct inventory *inventory,
                           const char *path, int id, uint64_t now)
{
    const struct pot *pot = id >= 0 ? &inventory->pots[id] : NULL;
    switch (property) {
    case DISPLAY_NAME: buffer_xml(body, path); break;
    case CONTENT_TYPE: buffer_append(body, "application/json"); break;
    case POT_COUNT: buffer_append(body, "%zu", inventory->count); break;
    case KIND: buffer_append(body, "%s", pot->kind == POT_TEA ? "tea" : "coffee"); break;
    case STATE: buffer_append(body, "%s", pot_phase_name(pot->phase)); break;
    case VARIETY:
        if (pot->variety >= 0) buffer_append(body, "%s", varieties[pot->variety].name);
        break;
    case ELAPSED: buffer_append(body, "%" PRIu64, pot_elapsed(pot, now)); break;
    case RECOMMENDED: buffer_append(body, "%" PRIu64, pot_duration(pot)); break;
    case STRENGTH: buffer_append(body, "%" PRIu64, pot_elapsed(pot, now) * 100 / pot_duration(pot)); break;
    case BATCHES: buffer_append(body, "%" PRIu64, pot->batches); break;
    case ADDITIONS:
        for (unsigned i = 0; i < ADDITION_COUNT; ++i)
            if (pot->additions & (UINT32_C(1) << i))
                buffer_append(body, "<P:addition>%s</P:addition>", addition_names[i]);
        break;
    default: break;
    }
}

int propfind_response(const struct request *request, const struct inventory *inventory,
                      const char *path, int pot_id, uint64_t now, struct buffer *body)
{
    struct xml_query query;
    int status = parse_query(request, &query);
    if (status) return status;
    uint32_t selected = 0;
    bool unknown[MAX_XML_PROPERTIES] = {false}, has_unknown = false;
    if (query.mode != QUERY_SELECTED)
        for (unsigned i = 0; i < PROPERTY_COUNT; ++i)
            if (property_id(&properties[i], pot_id) >= 0) selected |= UINT32_C(1) << i;
    for (size_t i = 0; i < query.count; ++i) {
        int id = property_id(&query.properties[i], pot_id);
        if (id >= 0) selected |= UINT32_C(1) << (unsigned)id;
        else { unknown[i] = true; has_unknown = true; }
    }
    buffer_append(body, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                       "<D:multistatus xmlns:D=\"DAV:\" xmlns:P=\"urn:potd\">"
                       "<D:response><D:href>");
    buffer_xml(body, path);
    buffer_append(body, "</D:href>");
    if (selected || !has_unknown) {
        buffer_append(body, "<D:propstat><D:prop>");
        for (unsigned i = 0; i < PROPERTY_COUNT; ++i) {
            if (!(selected & (UINT32_C(1) << i))) continue;
            const char *prefix = i <= CONTENT_TYPE ? "D" : "P";
            buffer_append(body, "<%s:%s>", prefix, properties[i].local);
            if (query.mode != QUERY_NAMES) property_value(body, i, inventory, path, pot_id, now);
            buffer_append(body, "</%s:%s>", prefix, properties[i].local);
        }
        buffer_append(body, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>");
    }
    if (has_unknown) {
        buffer_append(body, "<D:propstat><D:prop>");
        for (size_t i = 0; i < query.count; ++i) {
            if (!unknown[i]) continue;
            const struct property_name *property = &query.properties[i];
            if (*property->namespace_uri) {
                buffer_append(body, "<U:%s xmlns:U=\"", property->local);
                buffer_xml(body, property->namespace_uri);
                buffer_append(body, "\"/>");
            } else buffer_append(body, "<%s/>", property->local);
        }
        buffer_append(body, "</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>");
    }
    buffer_append(body, "</D:response></D:multistatus>\n");
    return body->failed ? 500 : 207;
}
