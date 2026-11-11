/*
   rmenc.c - encode files as a r@dio.mp3 / MEGARADIO.mp3 teletext stream

   This file is part of raspi-teletext.

   raspi-teletext is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   raspi-teletext is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with raspi-teletext. If not, see <http://www.gnu.org/licenses/>.
*/

// r@dio.mp3 (2000-2001) and MEGARADIO.mp3 (2002-2003) sent MP3 music,
// covers and HTML pages in the VBI of NBC Europe, as teletext packets in
// magazine 8 ("TV Radio Cast" by RapidSolution). rmenc writes such a
// stream as 42 byte packets to stdout, the format "teletext -" and
// "ttvd -t -" read. The format, reconstructed from the Megaradio.mp3
// 1.2.0.6 player, is described in
// https://github.com/itewreed/radiomp3-decoder (docs/FORMAT.md), and
// radiomp3-decoder turns a capture of it back into files. Without -l and
// stdin, the output is byte for byte what "radiomp3 testsignal" writes.
//
// Layers, from the top:
//
// Data row, 896 bytes:
//     0..9    header, every byte XORed with 0xA5: id (u16), offset (u32),
//             total length (u32), all little endian
//     10..13  Adler-32 of the row with these 4 bytes zero (little endian)
//     stream rows (id a multiple of 10, not 0 or 100): payload at 16, up
//             to 880 bytes
//     file rows (any other id): 24 bytes of file info (the name) at 14,
//             payload at 38, up to 858 bytes
//     id 100 is a filler row.
//
// Superblock: 13 data rows and 2 parity rows. Column c (0..63) is bytes
// c*14 .. c*14+13 of each data row, 182 bytes, protected by 28 Reed-Solomon
// parity bytes (GF(256), polynomial 0x11D, roots alpha^1..alpha^28) at
// c*28 in the 1792 bytes of the two parity rows.
//
// Page: each of the 15 rows goes out as one page in magazine 8, packets 0
// to 23. Packet 0 is a page header, packet 1 carries H(row 1..15), H(aux)
// and 38 bytes, packets 2..23 H(row) and 39 bytes each (H = Hamming 8/4).
// aux counts the superblocks, 1..15.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hamming.h"

#define ROW_SIZE (896)
#define DATA_ROWS (13)
#define ROWS (15)
#define COLUMNS (64)
#define COLUMN_WIDTH (14)
#define NPAR (28)
#define STREAM_PAYLOAD (880)
#define FILE_PAYLOAD (858)
#define INFO_SIZE (24)
#define FILLER_ID (100)
#define MAGAZINE (0)        // magazine 8, as coded in the MRAG
#define LAST_PACKET (23)
#define P1_PAYLOAD (38)
#define PN_PAYLOAD (39)

#define MAX_SOURCES (64)


// ---- Reed-Solomon, as Henry Minsky's rscode in the original player ----

static uint8_t gf_exp[512], gf_log[256];
static uint8_t genpoly[NPAR + 1];    // genpoly[0] is the highest power


static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return (a && b) ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}


static void rs_init(void)
{
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = x;
        gf_log[x] = i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++)
        gf_exp[i] = gf_exp[i - 255];

    // product of (x + alpha^i) for i = 1..28
    int len = 1;
    genpoly[0] = 1;
    for (int i = 1; i <= NPAR; i++) {
        uint8_t root = gf_exp[i];
        genpoly[len] = 0;
        for (int j = len; j > 0; j--)
            genpoly[j] ^= gf_mul(genpoly[j - 1], root);
        len++;
    }
}


// parity = msg * x^28 mod genpoly
static void rs_encode(const uint8_t *msg, int n, uint8_t *parity)
{
    uint8_t buf[256 + NPAR];
    memcpy(buf, msg, n);
    memset(buf + n, 0, NPAR);
    for (int i = 0; i < n; i++) {
        uint8_t coef = buf[i];
        if (coef)
            for (int j = 1; j <= NPAR; j++)
                buf[i + j] ^= gf_mul(coef, genpoly[j]);
    }
    memcpy(parity, buf + n, NPAR);
}


// ---- data rows ----

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n--) {
        a = (a + *p++) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}


static void put_le(uint8_t *p, uint32_t v, int n)
{
    while (n--) {
        *p++ = v & 0xFF;
        v >>= 8;
    }
}


static void build_row(uint8_t *row, int stream, uint16_t id, uint32_t offset,
                      uint32_t total, const uint8_t *data, size_t len,
                      const uint8_t *info)
{
    memset(row, 0, ROW_SIZE);
    put_le(row, id, 2);
    put_le(row + 2, offset, 4);
    put_le(row + 6, total, 4);
    if (stream) {
        memcpy(row + 16, data, len);
    } else {
        memcpy(row + 14, info, INFO_SIZE);
        memcpy(row + 38, data, len);
    }
    for (int i = 0; i < 10; i++)
        row[i] ^= 0xA5;
    // the checksum covers the obfuscated header, as on air
    put_le(row + 10, adler32(row, ROW_SIZE), 4);
}


// ---- sources: streams and files, sent round robin ----

typedef struct source {
    const char *path;
    int stream;             // 1 = live stream, 0 = file
    int live;               // read from stdin as it comes
    uint16_t id;
    uint8_t info[INFO_SIZE];
    uint8_t *data;          // whole file, unless live
    uint32_t size;
    uint32_t pos;           // read position in data
    uint32_t offset;        // offset sent in the row header
    int done;
} source;

static source sources[MAX_SOURCES];
static int nsources = 0;
static int loop = 0;


static void load(source *s)
{
    FILE *fp = fopen(s->path, "rb");
    if (!fp) {
        fprintf(stderr, "rmenc: %s: %s\n", s->path, strerror(errno));
        exit(1);
    }
    size_t cap = 65536, n = 0, r;
    s->data = malloc(cap);
    while (s->data && (r = fread(s->data + n, 1, cap - n, fp)) > 0) {
        n += r;
        if (n == cap)
            s->data = realloc(s->data, cap *= 2);
    }
    if (!s->data || ferror(fp)) {
        fprintf(stderr, "rmenc: %s: read error\n", s->path);
        exit(1);
    }
    fclose(fp);
    if (n > 0xFFFFFFFFu - STREAM_PAYLOAD) {
        fprintf(stderr, "rmenc: %s: too big\n", s->path);
        exit(1);
    }
    s->size = n;
}


// Makes the next row of a source. Returns 0 when the source has ended.
static int next_row(source *s, uint8_t *row)
{
    if (s->done)
        return 0;

    if (s->live) {
        // The total of a stream whose length isn't known yet is a
        // running mark: the end of this row's data.
        uint8_t buf[STREAM_PAYLOAD];
        size_t n = fread(buf, 1, sizeof buf, stdin);
        if (n == 0) {
            s->done = 1;
            return 0;
        }
        build_row(row, 1, s->id, s->offset, s->offset + n, buf, n, NULL);
        s->offset += n;
        return 1;
    }

    if (s->pos >= s->size) {
        if (!loop || s->size == 0) {
            s->done = 1;
            return 0;
        }
        s->pos = 0;
        // Files start again at offset 0, the decoder drops the copies it
        // already has. Streams run on, so that the decoder writes one
        // continuous MP3 file.
        if (!s->stream)
            s->offset = 0;
    }

    size_t max = s->stream ? STREAM_PAYLOAD : FILE_PAYLOAD;
    size_t n = s->size - s->pos;
    if (n > max)
        n = max;
    // A looped stream has no end either, so it also uses a running mark.
    uint32_t total = (s->stream && loop) ? s->offset + n : s->size;
    build_row(row, s->stream, s->id, s->offset, total, s->data + s->pos, n, s->info);
    s->pos += n;
    s->offset += n;
    return 1;
}


// ---- pages ----

static int page_number = 0x00;
static uint64_t packets_out = 0;


static void emit(const uint8_t *packet)
{
    if (fwrite(packet, 42, 1, stdout) != 1) {
        perror("rmenc: write");
        exit(1);
    }
    packets_out++;
}


static void mrag(uint8_t *p, int packet)
{
    int v = (MAGAZINE & 7) | (packet << 3);
    p[0] = hamming84(v & 0xF);
    p[1] = hamming84(v >> 4);
}


static void send_page(int row_index, const uint8_t *data, int aux)
{
    uint8_t pk[42];

    // header: page number, subcode and control bits all 0, no text
    mrag(pk, 0);
    pk[2] = hamming84(page_number & 0xF);
    pk[3] = hamming84(page_number >> 4);
    for (int i = 4; i < 10; i++)
        pk[i] = hamming84(0);
    memset(pk + 10, ' ', 32);
    emit(pk);

    mrag(pk, 1);
    pk[2] = hamming84(row_index);
    pk[3] = hamming84(aux);
    memcpy(pk + 4, data, P1_PAYLOAD);
    emit(pk);

    const uint8_t *p = data + P1_PAYLOAD;
    for (int n = 2; n <= LAST_PACKET; n++, p += PN_PAYLOAD) {
        mrag(pk, n);
        pk[2] = hamming84(row_index);
        memcpy(pk + 3, p, PN_PAYLOAD);
        emit(pk);
    }
}


static void send_superblock(uint8_t sb[ROWS][ROW_SIZE], int count)
{
    // parity rows 14 and 15, one RS codeword per column
    uint8_t *parity = sb[DATA_ROWS];
    for (int c = 0; c < COLUMNS; c++) {
        uint8_t msg[DATA_ROWS * COLUMN_WIDTH];
        for (int r = 0; r < DATA_ROWS; r++)
            memcpy(msg + r * COLUMN_WIDTH, sb[r] + c * COLUMN_WIDTH, COLUMN_WIDTH);
        rs_encode(msg, sizeof msg, parity + c * NPAR);
    }

    int aux = count % 15 + 1;
    for (int r = 0; r < ROWS; r++)
        send_page(r + 1, sb[r], aux);
    fflush(stdout);
}


// ---- command line ----

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] -s [ID:]file -f [ID:]file ...\n"
        "  -s [ID:]file  send as live stream (MP3); ID a multiple of 10, not\n"
        "                100 (default 10, 20, ...); \"-\" reads stdin as it comes\n"
        "  -f [ID:]file  send as file (cover, HTML, album track, stream.ini); ID\n"
        "                not a multiple of 10 (default 1, 2, 3, ...)\n"
        "  -l            loop: send the files over and over, and streams too\n"
        "                (they run on as one stream); ends when stdin ends\n"
        "  -p page       page number in the page headers, hex (default 00)\n"
        "Streams and files are sent in turn, one row of 880/858 bytes each.\n"
        "The output is 42 byte teletext packets, for \"teletext -\".\n", prog);
    exit(1);
}


static void add_source(const char *arg, int stream)
{
    static int next_stream_id = 10, next_file_id = 1;
    source *s;
    char *end;
    long id = -1;

    if (nsources == MAX_SOURCES) {
        fprintf(stderr, "rmenc: at most %d streams and files\n", MAX_SOURCES);
        exit(1);
    }
    s = &sources[nsources++];
    memset(s, 0, sizeof *s);

    // an optional "ID:" in front of the name
    if (arg[0] >= '0' && arg[0] <= '9') {
        long v = strtol(arg, &end, 10);
        if (*end == ':') {
            id = v;
            arg = end + 1;
        }
    }
    s->path = arg;
    s->stream = stream;

    if (id < 0) {
        if (stream) {
            id = next_stream_id;
            next_stream_id += (next_stream_id + 10 == FILLER_ID) ? 20 : 10;
        } else {
            id = next_file_id;
            next_file_id += (next_file_id % 10 == 9) ? 2 : 1;
        }
    }
    if (id > 0xFFFF || id == FILLER_ID
        || (stream && (id == 0 || id % 10 != 0))
        || (!stream && id != 0 && id % 10 == 0)) {
        fprintf(stderr, "rmenc: %s: ID %ld can't carry a %s\n", arg, id,
                stream ? "stream (needs a multiple of 10, not 0 or 100)"
                       : "file (needs a number that isn't a multiple of 10)");
        exit(1);
    }
    s->id = id;

    if (strcmp(arg, "-") == 0) {
        if (!stream) {
            fprintf(stderr, "rmenc: files can't come from stdin, only streams\n");
            exit(1);
        }
        for (int i = 0; i < nsources - 1; i++)
            if (sources[i].live) {
                fprintf(stderr, "rmenc: only one stream can come from stdin\n");
                exit(1);
            }
        s->live = 1;
    } else {
        load(s);
    }

    if (!stream) {
        // file info: the name, as far as it fits
        const char *base = strrchr(arg, '/');
        base = base ? base + 1 : arg;
        size_t n = strlen(base);
        memcpy(s->info, base, n < INFO_SIZE ? n : INFO_SIZE);
    }
}


int main(int argc, char *argv[])
{
    int c;
    while ((c = getopt(argc, argv, "s:f:lp:h")) != -1) {
        switch (c) {
            case 's': add_source(optarg, 1); break;
            case 'f': add_source(optarg, 0); break;
            case 'l': loop = 1; break;
            case 'p': page_number = strtol(optarg, NULL, 16) & 0xFF; break;
            default: usage(argv[0]);
        }
    }
    if (optind != argc || nsources == 0)
        usage(argv[0]);
    if (isatty(fileno(stdout))) {
        fprintf(stderr, "rmenc: stdout is a terminal, pipe it into \"teletext -\" or a file\n");
        return 1;
    }

    rs_init();

    int live = -1;
    for (int i = 0; i < nsources; i++)
        if (sources[i].live)
            live = i;

    static uint8_t sb[ROWS][ROW_SIZE];
    uint8_t filler[ROW_SIZE];
    build_row(filler, 1, FILLER_ID, 0, 0, NULL, 0, NULL);
    int rows = 0, count = 0, active = nsources;

    // round robin over the sources, as radiomp3 testsignal does
    while (active) {
        for (int i = 0; i < nsources; i++) {
            source *s = &sources[i];
            if (s->done)
                continue;
            if (!next_row(s, sb[rows])) {
                active--;
                continue;
            }
            if (++rows == DATA_ROWS) {
                send_superblock(sb, count++);
                rows = 0;
            }
        }
        // a live broadcast ends with its stream
        if (live >= 0 && sources[live].done)
            break;
    }
    if (rows) {
        while (rows < DATA_ROWS)
            memcpy(sb[rows++], filler, ROW_SIZE);
        send_superblock(sb, count++);
    }

    fprintf(stderr, "rmenc: %d superblocks, %llu packets\n", count,
            (unsigned long long)packets_out);
    return 0;
}
