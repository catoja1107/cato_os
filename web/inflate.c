/*
 * Streaming DEFLATE decoder (RFC 1951), in the style of zlib's "puff"
 * reference decoder: canonical Huffman codes decoded one bit at a time.
 */
#include "image_internal.h"

#define MAX_BITS 15
#define MAX_LITERAL_CODES 286
#define MAX_DISTANCE_CODES 30
#define FIXED_LITERAL_CODES 288
#define WINDOW_SIZE 32768u

struct huffman {
    int16_t count[MAX_BITS + 1];
    int16_t symbol[FIXED_LITERAL_CODES];
};

struct inflater {
    struct inflate_source source;
    inflate_sink sink;
    void *context;
    uint32_t bits;
    int bit_count;
    int error;
    int stopped;
    uint32_t total; /* Bytes produced, used to reject distances before the start. */
    uint32_t position;
    uint32_t flushed;
    uint8_t window[WINDOW_SIZE];
    struct huffman literals;
    struct huffman distances;
};

static const int16_t LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const int16_t LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const int16_t DISTANCE_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static const int16_t DISTANCE_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static struct inflater g_inflater;

static int need_bits(struct inflater *s, int count)
{
    uint32_t value = s->bits;
    while (s->bit_count < count) {
        int byte = s->source.read(s->source.context);
        if (byte < 0) {
            s->error = 1;
            return 0;
        }
        value |= (uint32_t)byte << s->bit_count;
        s->bit_count += 8;
    }
    s->bits = value >> count;
    s->bit_count -= count;
    return (int)(value & ((1u << count) - 1));
}

static void flush(struct inflater *s)
{
    if (s->position > s->flushed && !s->stopped) {
        if (!s->sink(s->context, s->window + s->flushed, (int)(s->position - s->flushed)))
            s->stopped = 1;
    }
    s->flushed = s->position;
}

static void put(struct inflater *s, uint8_t value)
{
    s->window[s->position++] = value;
    s->total++;
    if (s->position == WINDOW_SIZE) {
        flush(s);
        s->position = 0;
        s->flushed = 0;
    }
}

/* Builds a decoding table. Returns 0 for a complete code, >0 if incomplete, <0 if invalid. */
static int construct(struct huffman *h, const int16_t *lengths, int count)
{
    int16_t offsets[MAX_BITS + 1];

    for (int length = 0; length <= MAX_BITS; length++)
        h->count[length] = 0;
    for (int symbol = 0; symbol < count; symbol++)
        h->count[lengths[symbol]]++;
    if (h->count[0] == count)
        return 0;

    int left = 1;
    for (int length = 1; length <= MAX_BITS; length++) {
        left <<= 1;
        left -= h->count[length];
        if (left < 0)
            return -1;
    }

    offsets[1] = 0;
    for (int length = 1; length < MAX_BITS; length++)
        offsets[length + 1] = (int16_t)(offsets[length] + h->count[length]);
    for (int symbol = 0; symbol < count; symbol++) {
        if (lengths[symbol] != 0)
            h->symbol[offsets[lengths[symbol]]++] = (int16_t)symbol;
    }
    return left;
}

static int decode(struct inflater *s, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int length = 1; length <= MAX_BITS; length++) {
        code |= need_bits(s, 1);
        if (s->error)
            return -1;
        int count = h->count[length];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->error = 1;
    return -1;
}

static void stored_block(struct inflater *s)
{
    s->bits = 0;
    s->bit_count = 0;
    int low = need_bits(s, 8), high = need_bits(s, 8);
    int nlow = need_bits(s, 8), nhigh = need_bits(s, 8);
    if (s->error)
        return;
    int length = low | (high << 8);
    if (length != (~(nlow | (nhigh << 8)) & 0xffff)) {
        s->error = 1;
        return;
    }
    while (length-- > 0 && !s->error && !s->stopped) {
        int byte = s->source.read(s->source.context);
        if (byte < 0)
            s->error = 1;
        else
            put(s, (uint8_t)byte);
    }
}

static void codes(struct inflater *s)
{
    for (;;) {
        int symbol = decode(s, &s->literals);
        if (s->error || s->stopped)
            return;
        if (symbol < 256) {
            put(s, (uint8_t)symbol);
            continue;
        }
        if (symbol == 256)
            return;

        symbol -= 257;
        if (symbol >= 29) {
            s->error = 1;
            return;
        }
        int length = LENGTH_BASE[symbol] + need_bits(s, LENGTH_EXTRA[symbol]);
        int distance_symbol = decode(s, &s->distances);
        if (s->error)
            return;
        if (distance_symbol < 0 || distance_symbol >= 30) {
            s->error = 1;
            return;
        }
        uint32_t distance = (uint32_t)(DISTANCE_BASE[distance_symbol] +
                                       need_bits(s, DISTANCE_EXTRA[distance_symbol]));
        if (s->error || distance > s->total) {
            s->error = 1;
            return;
        }
        while (length-- > 0) {
            put(s, s->window[(s->position - distance) & (WINDOW_SIZE - 1)]);
            if (s->stopped)
                return;
        }
    }
}

static void fixed_block(struct inflater *s)
{
    int16_t lengths[FIXED_LITERAL_CODES];
    int symbol = 0;
    for (; symbol < 144; symbol++)
        lengths[symbol] = 8;
    for (; symbol < 256; symbol++)
        lengths[symbol] = 9;
    for (; symbol < 280; symbol++)
        lengths[symbol] = 7;
    for (; symbol < FIXED_LITERAL_CODES; symbol++)
        lengths[symbol] = 8;
    construct(&s->literals, lengths, FIXED_LITERAL_CODES);
    for (symbol = 0; symbol < MAX_DISTANCE_CODES; symbol++)
        lengths[symbol] = 5;
    construct(&s->distances, lengths, MAX_DISTANCE_CODES);
    codes(s);
}

static void dynamic_block(struct inflater *s)
{
    static const uint8_t ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int16_t lengths[MAX_LITERAL_CODES + MAX_DISTANCE_CODES];

    int literal_count = need_bits(s, 5) + 257;
    int distance_count = need_bits(s, 5) + 1;
    int code_count = need_bits(s, 4) + 4;
    if (s->error || literal_count > MAX_LITERAL_CODES || distance_count > MAX_DISTANCE_CODES) {
        s->error = 1;
        return;
    }

    int index = 0;
    for (; index < code_count; index++)
        lengths[ORDER[index]] = (int16_t)need_bits(s, 3);
    for (; index < 19; index++)
        lengths[ORDER[index]] = 0;
    if (s->error || construct(&s->literals, lengths, 19) != 0) {
        s->error = 1;
        return;
    }

    index = 0;
    while (index < literal_count + distance_count) {
        int symbol = decode(s, &s->literals);
        if (s->error)
            return;
        if (symbol < 16) {
            lengths[index++] = (int16_t)symbol;
            continue;
        }
        int repeat_length = 0;
        int repeat;
        if (symbol == 16) {
            if (index == 0) {
                s->error = 1;
                return;
            }
            repeat_length = lengths[index - 1];
            repeat = 3 + need_bits(s, 2);
        } else if (symbol == 17) {
            repeat = 3 + need_bits(s, 3);
        } else {
            repeat = 11 + need_bits(s, 7);
        }
        if (s->error || index + repeat > literal_count + distance_count) {
            s->error = 1;
            return;
        }
        while (repeat-- > 0)
            lengths[index++] = (int16_t)repeat_length;
    }

    /* The end-of-block code must exist, and incomplete codes are only allowed for one symbol. */
    if (lengths[256] == 0) {
        s->error = 1;
        return;
    }
    int result = construct(&s->literals, lengths, literal_count);
    if (result < 0 || (result > 0 && literal_count - s->literals.count[0] != 1)) {
        s->error = 1;
        return;
    }
    result = construct(&s->distances, lengths + literal_count, distance_count);
    if (result < 0 || (result > 0 && distance_count - s->distances.count[0] != 1)) {
        s->error = 1;
        return;
    }
    codes(s);
}

int inflate_stream(struct inflate_source source, inflate_sink sink, void *context)
{
    struct inflater *s = &g_inflater;
    s->source = source;
    s->sink = sink;
    s->context = context;
    s->bits = 0;
    s->bit_count = 0;
    s->error = 0;
    s->stopped = 0;
    s->total = 0;
    s->position = 0;
    s->flushed = 0;

    int last;
    do {
        last = need_bits(s, 1);
        int type = need_bits(s, 2);
        if (s->error)
            break;
        if (type == 0)
            stored_block(s);
        else if (type == 1)
            fixed_block(s);
        else if (type == 2)
            dynamic_block(s);
        else
            s->error = 1;
    } while (!last && !s->error && !s->stopped);

    if (!s->error)
        flush(s);
    if (s->stopped)
        return 1;
    return s->error ? -1 : 0;
}
