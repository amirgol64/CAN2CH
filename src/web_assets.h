#pragma once

/* The table itself is generated from web/ by tools/embed_web.py. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *path; /* e.g. "/index.html" */
    const char *mime;
    const uint8_t *data; /* gzip-compressed */
    size_t len;
} web_asset_t;

extern const web_asset_t web_assets[];
extern const size_t web_assets_count;
