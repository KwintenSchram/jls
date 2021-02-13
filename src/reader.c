/*
 * Copyright 2021 Jetperch LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jls/reader.h"
#include "jls/raw.h"
#include "jls/format.h"
#include "jls/ec.h"
#include "jls/log.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#define PAYLOAD_BUFFER_SIZE_DEFAULT (1 << 25)   // 32 MB
#define STRING_BUFFER_SIZE_DEFAULT (1 << 23)    // 8 MB
#define ANNOTATIONS_SIZE_DEFAULT (1 << 20)      // 1 MB
#define SIGNAL_MASK  (0x0fff)


#define ROE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        return rc__;                        \
    }                                       \
} while (0)

#define RLE(x)  do {                        \
    int32_t rc__ = (x);                     \
    if (rc__) {                             \
        JLS_LOGE("error %d: " #x, rc__);    \
        return rc__;                        \
    }                                       \
} while (0)

#if 0
struct source_s {
    struct jls_source_def_s def;
};

struct signal_s {
    struct jls_signal_def_s def;
};

struct jls_rd_s {
    struct source_s * sources[JLS_SOURCE_COUNT];
    struct signal_s * signals[JLS_SIGNAL_COUNT];
    struct jls_chunk_header_s hdr;  // the header for the current chunk
    uint8_t * chunk_buffer;         // storage for the
    size_t chunk_buffer_sz;
    FILE * f;
};
#endif


struct chunk_s {
    struct jls_chunk_header_s hdr;
    int64_t offset;
};

struct payload_s {
    uint8_t * start;
    uint8_t * cur;
    uint8_t * end;  // current end
    size_t length;  // current length
    size_t alloc_size;
};

struct strings_s {
    struct strings_s * next;
    char * start;
    char * cur;
    char * end;
};

struct signal_s {
    int64_t timestamp_start;
    int64_t timestamp_end;
    int64_t track_defs[4];
    int64_t track_head_offsets[4];
    int64_t track_head_data[4][JLS_SUMMARY_LEVEL_COUNT];
};

struct jls_rd_s {
    struct jls_raw_s * raw;
    struct jls_source_def_s source_def[JLS_SOURCE_COUNT];
    struct jls_source_def_s source_def_api[JLS_SOURCE_COUNT];
    struct jls_signal_def_s signal_def[JLS_SIGNAL_COUNT];
    struct jls_signal_def_s signal_def_api[JLS_SOURCE_COUNT];

    struct signal_s signals[JLS_SIGNAL_COUNT];

    struct chunk_s chunk_cur;
    struct payload_s payload;
    struct strings_s * strings_tail;
    struct strings_s * strings_head;

    struct chunk_s source_head;  // source_def
    struct chunk_s signal_head;  // signal_det, track_*_def, track_*_head

    struct chunk_s user_data_head;  // user_data, ignore first
    struct chunk_s user_data_cur;
};

static int32_t strings_alloc(struct jls_rd_s * self) {
    struct strings_s * s = malloc(STRING_BUFFER_SIZE_DEFAULT);
    if (!s) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    char * c8 = (char *) s;
    s->next = NULL;
    s->start = c8 + sizeof(struct strings_s);
    s->cur = s->start;
    s->end = c8 + STRING_BUFFER_SIZE_DEFAULT;
    if (!self->strings_head) {
        self->strings_head = s;
        self->strings_tail = s;
    } else {
        self->strings_tail->next = s;
    }
    return 0;
}

static void strings_free(struct jls_rd_s * self) {
    struct strings_s * s = self->strings_head;
    while (s) {
        struct strings_s * n = s->next;
        free(s);
        s = n;
    }
}

static int32_t payload_skip(struct jls_rd_s * self, size_t count) {
    if ((self->payload.cur + count) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    self->payload.cur += count;
    return 0;
}

static int32_t payload_parse_u8(struct jls_rd_s * self, uint8_t * value) {
    if ((self->payload.cur + 1) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = self->payload.cur[0];
    self->payload.cur += 1;
    return 0;
}

static int32_t payload_parse_u16(struct jls_rd_s * self, uint16_t * value) {
    if ((self->payload.cur + 2) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint16_t) self->payload.cur[0])
            | (((uint16_t) self->payload.cur[1]) << 8);
    self->payload.cur += 2;
    return 0;
}

static int32_t payload_parse_u32(struct jls_rd_s * self, uint32_t * value) {
    if ((self->payload.cur + 4) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint32_t) self->payload.cur[0])
             | (((uint32_t) self->payload.cur[1]) << 8)
             | (((uint32_t) self->payload.cur[2]) << 16)
             | (((uint32_t) self->payload.cur[3]) << 24);
    self->payload.cur += 4;
    return 0;
}

#if 0

static int32_t payload_parse_u64(struct jls_rd_s * self, uint64_t * value) {
    if ((self->payload.cur + 8) > self->payload.end) {
        return JLS_ERROR_EMPTY;
    }
    *value = ((uint64_t) self->payload.cur[0])
             | (((uint64_t) self->payload.cur[1]) << 8)
             | (((uint64_t) self->payload.cur[2]) << 16)
             | (((uint64_t) self->payload.cur[3]) << 24)
             | (((uint64_t) self->payload.cur[4]) << 32)
             | (((uint64_t) self->payload.cur[5]) << 40)
             | (((uint64_t) self->payload.cur[6]) << 48)
             | (((uint64_t) self->payload.cur[7]) << 56);
    self->payload.cur += 8;
    return 0;
}

#endif

static int32_t payload_parse_str(struct jls_rd_s * self, char ** value) {
    struct strings_s * s = self->strings_tail;
    char * str = s->cur;
    char ch;
    while (self->payload.cur != self->payload.end) {
        if (s->cur >= s->end) {
            ROE(strings_alloc(self));
            // copy over partial.
            while (str <= s->end) {
                *self->strings_tail->cur++ = *str++;
            }
            s = self->strings_tail;
            str = s->start;
        }

        ch = (char) *self->payload.cur++;
        *s->cur++ = ch;
        if (ch == 0) {
            if (*self->payload.cur == 0x1f) {
                self->payload.cur++;
            }
            *value = str;
            return 0;
        }
    }

    *value = NULL;
    return JLS_ERROR_EMPTY;
}

static int32_t rd(struct jls_rd_s * self) {
    while (1) {
        self->chunk_cur.offset = jls_raw_chunk_tell(self->raw);
        int32_t rc = jls_raw_rd(self->raw, &self->chunk_cur.hdr, self->payload.alloc_size, self->payload.start);
        if (rc == JLS_ERROR_TOO_BIG) {
            size_t sz_new = self->payload.alloc_size;
            while (sz_new < self->chunk_cur.hdr.payload_length) {
                sz_new *= 2;
            }
            uint8_t *ptr = realloc(self->payload.start, sz_new);
            if (!ptr) {
                return JLS_ERROR_NOT_ENOUGH_MEMORY;
            }
            self->payload.start = ptr;
            self->payload.cur = ptr;
            self->payload.end = ptr;
            self->payload.length = 0;
            self->payload.alloc_size = sz_new;
        } else if (rc == 0) {
            self->payload.cur = self->payload.start;
            self->payload.length = self->chunk_cur.hdr.payload_length;
            self->payload.end = self->payload.start + self->payload.length;
            return 0;
        } else {
            return rc;
        }
    }
}

static int32_t scan_sources(struct jls_rd_s * self) {
    JLS_LOGI("scan_sources");
    ROE(jls_raw_chunk_seek(self->raw, self->source_head.offset));
    while (1) {
        ROE(rd(self));
        uint16_t source_id = self->chunk_cur.hdr.chunk_meta;
        if (source_id >= JLS_SOURCE_COUNT) {
            JLS_LOGW("source_id %d too big - skip", (int) source_id);
        } else {
            struct jls_source_def_s *src = &self->source_def[source_id];
            ROE(payload_skip(self, 64));
            ROE(payload_parse_str(self, (char **) &src->name));
            ROE(payload_parse_str(self, (char **) &src->vendor));
            ROE(payload_parse_str(self, (char **) &src->model));
            ROE(payload_parse_str(self, (char **) &src->version));
            ROE(payload_parse_str(self, (char **) &src->serial_number));
            src->source_id = source_id;  // indicate that this source is valid!
            JLS_LOGI("Found source %d : %s", (int) source_id, src->name);
        }
        if (!self->chunk_cur.hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->chunk_cur.hdr.item_next));
    }
    return 0;
}

static int32_t signal_validate(struct jls_rd_s * self, uint16_t signal_id, struct jls_signal_def_s * s) {
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    if (self->source_def[s->source_id].source_id != s->source_id) {
        JLS_LOGW("signal %d: source_id %d not found", (int) signal_id, (int) s->source_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    switch (s->signal_type) {
        case JLS_SIGNAL_TYPE_FSR: break;
        case JLS_SIGNAL_TYPE_VSR: break;
        default:
            JLS_LOGW("signal %d: invalid signal_type: %d", (int) signal_id, (int) s->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }

    // todo check data_type
    return 0;
}

static int32_t handle_signal_def(struct jls_rd_s * self) {
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta;
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big - skip", (int) signal_id);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_signal_def_s *s = &self->signal_def[signal_id];
    s->signal_id = signal_id;
    ROE(payload_parse_u16(self, &s->source_id));
    ROE(payload_parse_u8(self, &s->signal_type));
    ROE(payload_skip(self, 1));
    ROE(payload_parse_u32(self, &s->data_type));
    ROE(payload_parse_u32(self, &s->sample_rate));
    ROE(payload_parse_u32(self, &s->samples_per_data));
    ROE(payload_parse_u32(self, &s->sample_decimate_factor));
    ROE(payload_parse_u32(self, &s->entries_per_summary));
    ROE(payload_parse_u32(self, &s->summary_decimate_factor));
    ROE(payload_parse_u32(self, &s->utc_rate_auto));
    ROE(payload_skip(self, 64));
    ROE(payload_parse_str(self, (char **) &s->name));
    ROE(payload_parse_str(self, (char **) &s->si_units));
    if (0 == signal_validate(self, signal_id, s)) {  // validate passed
        s->signal_id = signal_id;  // indicate that this signal is valid
        JLS_LOGI("Found signal %d : %s", (int) signal_id, s->name);
    }  // else skip
    return 0;
}

static bool is_signal_defined(struct jls_rd_s * self, uint16_t signal_id) {
    signal_id &= SIGNAL_MASK;  // mask off chunk_meta depth
    if (signal_id >= JLS_SIGNAL_COUNT) {
        JLS_LOGW("signal_id %d too big", (int) signal_id);
        return false;
    }
    if (self->signal_def[signal_id].signal_id != signal_id) {
        JLS_LOGW("signal_id %d not defined", (int) signal_id);
        return false;
    }
    return true;
}

static inline uint8_t tag_parse_track_type(uint8_t tag) {
    return (tag >> 3) & 3;
}

static int32_t validate_track_tag(struct jls_rd_s * self, uint16_t signal_id, uint8_t tag) {
    if (!is_signal_defined(self, signal_id)) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    struct jls_signal_def_s * signal_def = &self->signal_def[signal_id];
    uint8_t track_type = tag_parse_track_type(tag);
    switch (signal_def->signal_type) {
        case JLS_SIGNAL_TYPE_FSR:
            if ((track_type == JLS_TRACK_TYPE_FSR)
                || (track_type == JLS_TRACK_TYPE_ANNOTATION)
                || (track_type == JLS_TRACK_TYPE_UTC)) {
                // good
            } else {
                JLS_LOGW("unsupported track %d for FSR signal", (int) track_type);
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        case JLS_SIGNAL_TYPE_VSR:
            if ((track_type == JLS_TRACK_TYPE_VSR)
                || (track_type == JLS_TRACK_TYPE_ANNOTATION)) {
                // good
            } else {
                JLS_LOGW("unsupported track %d for VSR signal", (int) track_type);
                return JLS_ERROR_PARAMETER_INVALID;
            }
            break;
        default:
            // should have already been checked.
            JLS_LOGW("unsupported signal type: %d", (int) signal_def->signal_type);
            return JLS_ERROR_PARAMETER_INVALID;
    }
    return 0;
}

static int32_t handle_track_def(struct jls_rd_s * self, int64_t pos) {
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta & SIGNAL_MASK;
    ROE(validate_track_tag(self, signal_id, self->chunk_cur.hdr.tag));
    self->signals[signal_id].track_defs[tag_parse_track_type(self->chunk_cur.hdr.tag)] = pos;
    return 0;
}

static int32_t handle_track_head(struct jls_rd_s * self, int64_t pos) {
    uint16_t signal_id = self->chunk_cur.hdr.chunk_meta & SIGNAL_MASK;
    ROE(validate_track_tag(self, signal_id, self->chunk_cur.hdr.tag));
    uint8_t track_type = tag_parse_track_type(self->chunk_cur.hdr.tag);

    self->signals[signal_id].track_head_offsets[track_type] = pos;
    size_t expect_sz = JLS_SUMMARY_LEVEL_COUNT * sizeof(int64_t);

    if (self->payload.length != expect_sz) {
        JLS_LOGW("cannot parse signal %d head, sz=%zu, expect=%zu",
                 (int) signal_id, self->payload.length, expect_sz);
        return JLS_ERROR_PARAMETER_INVALID;
    }
    memcpy(self->signals[signal_id].track_head_data[track_type], self->payload.start, expect_sz);
    return 0;
}

static int32_t scan_signals(struct jls_rd_s * self) {
    JLS_LOGI("scan_signals");
    ROE(jls_raw_chunk_seek(self->raw, self->signal_head.offset));
    while (1) {
        ROE(rd(self));
        if (self->chunk_cur.hdr.tag == JLS_TAG_SIGNAL_DEF) {
            handle_signal_def(self);
        } else if ((self->chunk_cur.hdr.tag & 7) == JLS_TRACK_CHUNK_DEF) {
            handle_track_def(self, self->chunk_cur.offset);
        } else if ((self->chunk_cur.hdr.tag & 7) == JLS_TRACK_CHUNK_HEAD) {
            handle_track_head(self, self->chunk_cur.offset);
        } else {
            JLS_LOGW("unknown tag %d in signal list", (int) self->chunk_cur.hdr.tag);
        }
        if (!self->chunk_cur.hdr.item_next) {
            break;
        }
        ROE(jls_raw_chunk_seek(self->raw, self->chunk_cur.hdr.item_next));
    }
    return 0;
}

static int32_t scan(struct jls_rd_s * self) {
    int32_t rc = 0;
    uint8_t found = 0;

    for (int i = 0; found != 7; ++i) {
        if (i == 3) {
            JLS_LOGW("malformed JLS, continue searching");
        }
        int64_t pos = jls_raw_chunk_tell(self->raw);
        rc = rd(self);
        if (rc == JLS_ERROR_EMPTY) {
            return 0;
        } else if (rc) {
            return rc;
        }

        JLS_LOGI("tag %d : %s", self->chunk_cur.hdr.tag, jls_tag_to_name(self->chunk_cur.hdr.tag));

        switch (self->chunk_cur.hdr.tag) {
            case JLS_TAG_USER_DATA:
                found |= 1;
                if (!self->user_data_head.offset) {
                    self->user_data_head.offset = pos;
                    self->user_data_head.hdr = self->chunk_cur.hdr;
                    self->user_data_cur = self->user_data_head;
                }
                break;
            case JLS_TAG_SOURCE_DEF:
                found |= 2;
                if (!self->source_head.offset) {
                    self->source_head.offset = pos;
                    self->source_head.hdr = self->chunk_cur.hdr;
                }
                break;
            case JLS_TAG_SIGNAL_DEF:
                found |= 4;
                if (!self->signal_head.offset) {
                    self->signal_head.offset = pos;
                    self->signal_head.hdr = self->chunk_cur.hdr;
                }
                break;
            default:
                break;  // skip
        }
    }
    JLS_LOGI("found initial tags");
    ROE(scan_sources(self));
    ROE(scan_signals(self));
    return 0;
}

int32_t jls_rd_open(struct jls_rd_s ** instance, const char * path) {
    if (!instance) {
        return JLS_ERROR_PARAMETER_INVALID;
    }

    struct jls_rd_s *self = calloc(1, sizeof(struct jls_rd_s));
    if (!self) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    self->payload.start = malloc(PAYLOAD_BUFFER_SIZE_DEFAULT);
    strings_alloc(self);

    if (!self->payload.start || !self->strings_head) {
        jls_rd_close(self);
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }
    self->payload.alloc_size = PAYLOAD_BUFFER_SIZE_DEFAULT;
    self->payload.cur = self->payload.start;
    self->payload.end = self->payload.start;

    int32_t rc = jls_raw_open(&self->raw, path, "r");
    if (rc) {
        jls_rd_close(self);
        return rc;
    }

    ROE(scan(self));
    *instance = self;
    return rc;
}

void jls_rd_close(struct jls_rd_s * self) {
    if (self) {
        jls_raw_close(self->raw);
        strings_free(self);
        if (self->payload.start) {
            free(self->payload.start);
            self->payload.start = NULL;
        }
        self->raw = NULL;
        free(self);
    }
}

int32_t jls_rd_sources(struct jls_rd_s * self, struct jls_source_def_s ** sources, uint16_t * count) {
    if (!self || !sources || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t c = 0;
    for (uint16_t i = 0; i < JLS_SOURCE_COUNT; ++i) {
        if (self->source_def[i].source_id == i) {
            // Note: source 0 is always defined, so calloc is ok
            self->source_def_api[c++] = self->source_def[i];
        }
    }
    *sources = self->source_def_api;
    *count = c;
    return 0;
}

int32_t jls_rd_signals(struct jls_rd_s * self, struct jls_signal_def_s ** signals, uint16_t * count) {
    if (!self || !signals || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint16_t c = 0;
    for (uint16_t i = 0; i < JLS_SIGNAL_COUNT; ++i) {
        if (self->signal_def[i].signal_id == i) {
            // Note: signal 0 is always defined, so calloc is ok
            self->signal_def_api[c++] = self->signal_def[i];
        }
    }
    *signals = self->signal_def_api;
    *count = c;
    return 0;
}

int32_t seek(struct jls_rd_s * self, uint16_t signal_id, uint8_t level, int64_t sample_id) {
    if (!is_signal_defined(self, signal_id)) {
        return JLS_ERROR_NOT_FOUND;
    }
    struct jls_signal_def_s * signal_def = &self->signal_def[signal_id];
    if (signal_def->signal_type != JLS_SIGNAL_TYPE_FSR) {
                JLS_LOGW("fsr_length not support for signal type %d", (int) signal_def->signal_type);
        return JLS_ERROR_NOT_SUPPORTED;
    }
    struct signal_s * s = &self->signals[signal_id];
    int64_t offset = 0;
    int64_t * offsets = s->track_head_data[JLS_TRACK_TYPE_FSR];
    int initial_level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; initial_level >= 0; --initial_level) {
        if (offsets[initial_level]) {
            offset = offsets[initial_level];
            break;
        }
    }
    if (!offset) {
        return JLS_ERROR_NOT_FOUND;
    }

    for (int lvl = initial_level; lvl > level; --lvl) {
        JLS_LOGI("signal %d, level %d, index=%" PRIi64, (int) signal_id, (int) lvl, offset);
        int64_t step_size = signal_def->samples_per_data;
        for (int k = 1; k < lvl; ++k) {
            step_size *= signal_def->entries_per_summary;
        }
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(rd(self));

        int64_t * payload = (int64_t *) self->payload.start;
        int64_t chunk_timestamp = payload[0];
        int64_t chunk_entries = payload[1];
        if (((2 + chunk_entries) * sizeof(int64_t)) > self->payload.length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_PARAMETER_INVALID;
        }

        int64_t idx = (sample_id - chunk_timestamp) / step_size;

        JLS_LOGI("index level=%d, entries=%d, 0:%" PRIi64 ", 1:%" PRIi64 ", end:%" PRIi64,
                 (int) lvl, (int) chunk_entries, payload[2], payload[3], payload[2 + payload[1] - 1]);
        offset = payload[2 + idx];
    }

    ROE(jls_raw_chunk_seek(self->raw, offset));
    return 0;
}


int32_t jls_rd_fsr_length(struct jls_rd_s * self, uint16_t signal_id, int64_t * samples) {
    if (!is_signal_defined(self, signal_id)) {
        return JLS_ERROR_NOT_FOUND;
    }
    struct jls_signal_def_s * signal_def = &self->signal_def[signal_id];
    if (signal_def->signal_type != JLS_SIGNAL_TYPE_FSR) {
        JLS_LOGW("fsr_length not support for signal type %d", (int) signal_def->signal_type);
        return JLS_ERROR_NOT_SUPPORTED;
    }
    struct signal_s * s = &self->signals[signal_id];
    int64_t offset = 0;
    int64_t * offsets = s->track_head_data[JLS_TRACK_TYPE_FSR];
    int level = JLS_SUMMARY_LEVEL_COUNT - 1;
    for (; level >= 0; --level) {
        if (offsets[level]) {
            offset = offsets[level];
            break;
        }
    }
    if (!offset) {
        *samples = 0;
        return 0;
    }

    for (int lvl = level; lvl > 0; --lvl) {
        JLS_LOGI("signal %d, level %d, index=%" PRIi64, (int) signal_id, (int) lvl, offset);
        ROE(jls_raw_chunk_seek(self->raw, offset));
        ROE(rd(self));

        int64_t * payload = (int64_t *) self->payload.start;
        if (((2 + payload[1]) * sizeof(int64_t)) > self->payload.length) {
            JLS_LOGE("invalid payload length");
            return JLS_ERROR_PARAMETER_INVALID;
        }
        JLS_LOGI("index level=%d, entries=%d, 0:%" PRIi64 ", 1:%" PRIi64 ", end:%" PRIi64,
                 (int) lvl, (int) payload[1], payload[2], payload[3], payload[2 + payload[1] - 1]);
        offset = payload[2 + payload[1] - 1];
    }

    ROE(jls_raw_chunk_seek(self->raw, offset));
    ROE(rd(self));
    int64_t * payload = (int64_t *) self->payload.start;
    JLS_LOGI("samples: %" PRIi64 ", %" PRIi64, payload[0], payload[1]);
    *samples = payload[0] + payload[1];
    return 0;
}

int32_t jls_rd_fsr_f32(struct jls_rd_s * self, uint16_t signal_id, int64_t start_sample_id,
                       float * data, int64_t data_length) {
    ROE(seek(self, signal_id, 0, start_sample_id));
    while (data_length > 0) {
        ROE(rd(self));
        int64_t * i64 = ((int64_t*) self->payload.start);
        int64_t chunk_sample_id = i64[0];
        int64_t chunk_sample_count = i64[1];
        int64_t idx_start = 0;
        if (start_sample_id > chunk_sample_id) {
            idx_start = start_sample_id - chunk_sample_id;
        }
        chunk_sample_count -= idx_start;
        if (data_length < chunk_sample_count) {
            chunk_sample_count = data_length;
        }
        float * f32 = (float *) &i64[2];
        memcpy(data, f32 + idx_start, (size_t) chunk_sample_count * sizeof(float));
        data += chunk_sample_count;
        data_length -= chunk_sample_count;
    }
    return 0;
}

int32_t rd_user_data(struct jls_rd_s * self, struct jls_rd_user_data_s * user_data) {
    ROE(rd(self));
    if (self->chunk_cur.hdr.tag != JLS_TAG_USER_DATA) {
        return JLS_ERROR_NOT_FOUND;
    }
    uint8_t storage_type = (uint8_t) ((self->chunk_cur.hdr.chunk_meta >> 12) & 0x0f);
    switch (storage_type) {
        case JLS_STORAGE_TYPE_BINARY:  // intentional fall-through
        case JLS_STORAGE_TYPE_STRING:  // intentional fall-through
        case JLS_STORAGE_TYPE_JSON:
            break;
        default:
            return JLS_ERROR_PARAMETER_INVALID;
    }
    user_data->chunk_meta = self->chunk_cur.hdr.chunk_meta & 0x0fff;
    user_data->storage_type = storage_type;
    user_data->data = self->payload.start;
    user_data->data_size = self->chunk_cur.hdr.payload_length;
    self->user_data_cur = self->chunk_cur;
    return 0;
}

int32_t jls_rd_annotations(struct jls_rd_s * self, uint16_t signal_id,
                           struct jls_rd_annotation_s ** annotations, uint32_t * count) {
    if (!is_signal_defined(self, signal_id)) {
        return JLS_ERROR_NOT_FOUND;
    }
    if (!annotations || !count) {
        return JLS_ERROR_PARAMETER_INVALID;
    }
    uint8_t * p = malloc(ANNOTATIONS_SIZE_DEFAULT);
    if (!p) {
        return JLS_ERROR_NOT_ENOUGH_MEMORY;
    }

    // todo
    *annotations = 0;
    *count = 0;

    return 0;
}

void jls_rd_annotations_free(struct jls_rd_s * self, struct jls_rd_annotation_s * annotations);

int32_t jls_rd_user_data_reset(struct jls_rd_s * self) {
    self->user_data_cur = self->user_data_head;
    return 0;
}

int32_t jls_rd_user_data_next(struct jls_rd_s * self, struct jls_rd_user_data_s * user_data) {
    if (!self->user_data_cur.offset || !self->user_data_cur.hdr.item_next) {
        return JLS_ERROR_EMPTY;
    }
    ROE(jls_raw_chunk_seek(self->raw, self->user_data_cur.hdr.item_next));
    return rd_user_data(self, user_data);
}

int32_t jls_rd_user_data_prev(struct jls_rd_s * self, struct jls_rd_user_data_s * user_data) {
    if ((self->user_data_cur.offset <= 0) || !self->user_data_cur.hdr.item_prev) {
        return JLS_ERROR_EMPTY;
    }
    if (self->user_data_cur.hdr.item_prev == (uint64_t) self->user_data_head.offset) {
        jls_rd_user_data_reset(self);
        return JLS_ERROR_EMPTY;
    }
    ROE(jls_raw_chunk_seek(self->raw, self->user_data_cur.hdr.item_prev));
    return rd_user_data(self, user_data);
}
