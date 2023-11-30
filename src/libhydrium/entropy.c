#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bitwriter.h"
#include "entropy.h"
#include "internal.h"
#include "math-functions.h"
#include "memory.h"

// State Flush. Monsieur Bond Wins.
typedef struct StateFlush {
    size_t token_index;
    uint16_t value;
} StateFlush;

typedef struct StateFlushChain {
    StateFlush *state_flushes;
    size_t pos;
    size_t capacity;
    struct StateFlushChain *prev_chain;
} StateFlushChain;

typedef struct FrequencyEntry {
    int32_t token;
    uint32_t frequency;
    int32_t depth;
    int32_t max_depth;
    struct FrequencyEntry *left_child;
    struct FrequencyEntry *right_child;
} FrequencyEntry;

static const HYDVLCElement ans_dist_prefix_lengths[14] = {
    {17, 5}, {11, 4}, {15, 4}, {3, 4}, {9, 4},  {7, 4},  {4, 3},
    {2, 3},  {5, 3},  {6, 3},  {0, 3}, {33, 6}, {1, 7}, {65, 7},
};

static const HYDHybridUintConfig lz77_len_conf = {7, 0, 0};

static const uint32_t prefix_zig_zag[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static const HYDVLCElement prefix_level0_table[6] = {
    {0, 2}, {7, 4}, {3, 3}, {2, 2}, {1, 2}, {15, 4},
};

static HYDStatusCode write_ans_u8(HYDBitWriter *bw, uint8_t b) {
    hyd_write_bool(bw, b);
    if (!b)
        return bw->overflow_state;
    int l = hyd_fllog2(b);
    hyd_write(bw, l, 3);
    return hyd_write(bw, b, l);
}

void hyd_entropy_stream_destroy(HYDEntropyStream *stream) {
    hyd_free(stream->allocator, stream->frequencies);
    hyd_free(stream->allocator, stream->cluster_map);
    hyd_free(stream->allocator, stream->symbols);
    hyd_free(stream->allocator, stream->configs);
    hyd_free(stream->allocator, stream->alphabet_sizes);
    if (stream->alias_table) {
        for (size_t i = 0; i < stream->max_alphabet_size * stream->num_clusters; i++)
            hyd_free(stream->allocator, stream->alias_table[i].cutoffs);
    }
    hyd_free(stream->allocator, stream->alias_table);
    hyd_free(stream->allocator, stream->vlc_table);
    memset(stream, 0, sizeof(HYDEntropyStream));
}

HYDStatusCode hyd_entropy_set_hybrid_config(HYDEntropyStream *stream, uint8_t min_cluster, uint8_t to_cluster,
                                            int split_exponent, int msb_in_token, int lsb_in_token) {
    if (to_cluster && min_cluster >= to_cluster)
        return HYD_INTERNAL_ERROR;

    for (uint8_t j = min_cluster; (!to_cluster || j < to_cluster) && j < stream->num_clusters; j++) {
        stream->configs[j].split_exponent = split_exponent;
        stream->configs[j].msb_in_token = msb_in_token;
        stream->configs[j].lsb_in_token = lsb_in_token;
    }

    return HYD_OK;
}

static HYDStatusCode write_cluster_map(HYDEntropyStream *stream) {
    HYDBitWriter *bw = stream->bw;

    if (stream->num_dists == 1)
        return HYD_OK;

    int nbits = hyd_cllog2(stream->num_clusters);

    if (nbits <= 3 && stream->num_dists * nbits <= 32) {
        // simple clustering
        hyd_write_bool(bw, 1);
        hyd_write(bw, nbits, 2);
        for (size_t i = 0; i < stream->num_dists; i++)
            hyd_write(bw, stream->cluster_map[i], nbits);
        return bw->overflow_state;
    }

    HYDEntropyStream nested;
    HYDStatusCode ret;
    // non simple clustering
    hyd_write_bool(bw, 0);
    // use mtf = true
    hyd_write_bool(bw, 1);
    ret = hyd_entropy_init_stream(&nested, stream->allocator, bw, stream->num_dists, (const uint8_t[]){0},
        1, 1, 64);
    if (ret < HYD_ERROR_START)
        goto fail;
    if ((ret = hyd_entropy_set_hybrid_config(&nested, 0, 0, 4, 1, 0)) < HYD_ERROR_START)
        goto fail;
    uint8_t mtf[256];
    for (int i = 0; i < 256; i++)
        mtf[i] = i;
    for (uint32_t j = 0; j < stream->num_dists; j++) {
        uint8_t index = 0;
        for (int k = 0; k < 256; k++) {
            if (mtf[k] == stream->cluster_map[j]) {
                index = k;
                break;
            }
        }
        if ((ret = hyd_entropy_send_symbol(&nested, 0, index)) < HYD_ERROR_START)
            goto fail;
        if (index) {
            uint8_t value = mtf[index];
            memmove(mtf + 1, mtf, index);
            mtf[0] = value;
        }
    }

    if ((ret = hyd_prefix_finalize_stream(&nested)) < HYD_ERROR_START)
        goto fail;

    return bw->overflow_state;

fail:
    hyd_entropy_stream_destroy(&nested);
    return ret;
}

static HYDStatusCode write_hybrid_uint_config(HYDEntropyStream *stream, const HYDHybridUintConfig *config,
                                              int log_alphabet_size) {
    HYDBitWriter *bw = stream->bw;

    // split_exponent
    hyd_write(bw, config->split_exponent, hyd_cllog2(1 + log_alphabet_size));
    if (config->split_exponent == log_alphabet_size)
        return bw->overflow_state;
    // msb_in_token
    hyd_write(bw, config->msb_in_token,
        hyd_cllog2(1 + config->split_exponent));
    // lsb_in_token
    hyd_write(bw, config->lsb_in_token,
        hyd_cllog2(1 + config->split_exponent - config->msb_in_token));
    return bw->overflow_state;
}

static HYDStatusCode generate_alias_mapping(HYDEntropyStream *stream, size_t cluster, int log_alphabet_size, int32_t uniq_pos) {
    int log_bucket_size = 12 - log_alphabet_size;
    uint32_t bucket_size = 1 << log_bucket_size;
    uint32_t table_size = 1 << log_alphabet_size;
    uint32_t symbols[256] = { 0 };
    uint32_t cutoffs[256] = { 0 };
    uint32_t offsets[256] = { 0 };
    HYDAliasEntry *alias_table = stream->alias_table + cluster * stream->max_alphabet_size;

    if (uniq_pos >= 0) {
        for (uint32_t i = 0; i < table_size; i++) {
            symbols[i] = uniq_pos;
            offsets[i] = i * bucket_size;
        }
        alias_table[uniq_pos].count = table_size;
    } else {
        size_t underfull_pos = 0;
        size_t overfull_pos = 0;
        uint8_t underfull[256];
        uint8_t overfull[256];
        for (size_t pos = 0; pos < stream->max_alphabet_size; pos++) {
            cutoffs[pos] = stream->frequencies[stream->max_alphabet_size * cluster + pos];
            if (cutoffs[pos] < bucket_size)
                underfull[underfull_pos++] = pos;
            else if (cutoffs[pos] > bucket_size)
                overfull[overfull_pos++] = pos;
        }

        for (uint32_t i = stream->max_alphabet_size; i < table_size; i++)
            underfull[underfull_pos++] = i;

        while (overfull_pos) {
            if (!underfull_pos)
                return HYD_INTERNAL_ERROR;
            uint8_t u = underfull[--underfull_pos];
            uint8_t o = overfull[--overfull_pos];
            int32_t by = bucket_size - cutoffs[u];
            offsets[u] = (cutoffs[o] -= by);
            symbols[u] = o;
            if (cutoffs[o] < bucket_size)
                underfull[underfull_pos++] = o;
            else if (cutoffs[o] > bucket_size)
                overfull[overfull_pos++] = o;
        }

        for (uint32_t sym = 0; sym < table_size; sym++) {
            if (cutoffs[sym] == bucket_size) {
                symbols[sym] = sym;
                cutoffs[sym] = offsets[sym] = 0;
            } else {
                offsets[sym] -= cutoffs[sym];
            }
            alias_table[symbols[sym]].count++;
        }
    }

    for (uint32_t sym = 0; sym < stream->max_alphabet_size; sym++) {
        alias_table[sym].cutoffs = hyd_mallocarray(stream->allocator, 3 * (alias_table[sym].count + 1),
            sizeof(int32_t));
        if (!alias_table[sym].cutoffs)
            return HYD_NOMEM;
        memset(alias_table[sym].cutoffs, -1, 3 * (alias_table[sym].count + 1) * sizeof(int32_t));
        alias_table[sym].offsets = alias_table[sym].cutoffs + alias_table[sym].count + 1;
        alias_table[sym].original = alias_table[sym].offsets + alias_table[sym].count + 1;
        alias_table[sym].offsets[0] = 0;
        alias_table[sym].cutoffs[0] = cutoffs[sym];
        alias_table[sym].original[0] = sym;
    }

    for (uint32_t i = 0; i < table_size; i++) {
        size_t j = 1;
        while (alias_table[symbols[i]].cutoffs[j] >= 0)
            j++;
        alias_table[symbols[i]].cutoffs[j] = cutoffs[i];
        alias_table[symbols[i]].offsets[j] = offsets[i];
        alias_table[symbols[i]].original[j] = i;
    }

    return HYD_OK;
}

static int32_t write_ans_frequencies(HYDEntropyStream *stream, uint32_t *frequencies) {
    HYDBitWriter *bw = stream->bw;
    size_t total = 0;
    for (size_t k = 0; k < stream->max_alphabet_size; k++)
        total += frequencies[k];
    if (!total)
        total = 1;
    size_t new_total = 0;
    for (size_t k = 0; k < stream->max_alphabet_size; k++) {
        frequencies[k] = (((uint64_t)frequencies[k] << 12) / total) & 0xFFFF;
        if (!frequencies[k])
            frequencies[k] = 1;
        new_total += frequencies[k];
    }

    size_t j = stream->max_alphabet_size - 1;
    while (new_total > (1 << 12)) {
        size_t diff = new_total - (1 << 12);
        if (diff < frequencies[j]) {
            frequencies[j] -= diff;
            new_total -= diff;
            break;
        } else if (frequencies[j] > 1) {
            new_total -= frequencies[j] - 1;
            frequencies[j] = 1;
        }
        j--;
    }

    frequencies[0] += (1 << 12) - new_total;
    if (frequencies[0] == 1 << 12) {
        // simple dist
        hyd_write(bw, 0x1, 2);
        write_ans_u8(bw, 0);
        return 0;
    }

    if (frequencies[0] + frequencies[1] == 1 << 12) {
        // simple dual peak dist
        hyd_write(bw, 0x3, 2);
        write_ans_u8(bw, 0);
        write_ans_u8(bw, 1);
        hyd_write(bw, frequencies[0], 12);
        return HYD_DEFAULT;
    }

    // simple dist and flat dist = 0
    hyd_write(bw, 0, 2);
    // len = 3
    hyd_write(bw, 0x7, 3);
    // shift = 13
    hyd_write(bw, 0x6, 3);
    write_ans_u8(bw, stream->max_alphabet_size - 3);
    int log_counts[256];
    size_t omit_pos = 0;
    size_t omit_log = 0;
    for (size_t k = 0; k < stream->max_alphabet_size; k++) {
        log_counts[k] = frequencies[k] ? 1 + hyd_fllog2(frequencies[k]) : 0;
        hyd_write(bw, ans_dist_prefix_lengths[log_counts[k]].symbol, ans_dist_prefix_lengths[log_counts[k]].length);
        if (log_counts[k] > omit_log) {
            omit_log = log_counts[k];
            omit_pos = k;
        }
    }
    for (size_t k = 0; k < stream->max_alphabet_size; k++) {
        if (k == omit_pos || log_counts[k] <= 1)
            continue;
        hyd_write(bw, frequencies[k], log_counts[k] - 1);
    }

    return HYD_DEFAULT;
}

HYDStatusCode hyd_entropy_init_stream(HYDEntropyStream *stream, HYDAllocator *allocator, HYDBitWriter *bw,
                                  size_t symbol_count, const uint8_t *cluster_map, size_t num_dists,
                                  int custom_configs, uint32_t lz77_min_symbol) {
    HYDStatusCode ret;
    memset(stream, 0, sizeof(HYDEntropyStream));
    if (!num_dists || !symbol_count) {
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }
    if (lz77_min_symbol) {
        num_dists++;
        stream->lz77_min_length = 3;
        stream->lz77_min_symbol = lz77_min_symbol;
    }
    stream->num_dists = num_dists;
    stream->allocator = allocator;
    stream->bw = bw;
    stream->symbol_count = symbol_count;
    stream->cluster_map = hyd_malloc(allocator, num_dists);
    stream->symbols = hyd_mallocarray(allocator, stream->symbol_count, sizeof(HYDHybridSymbol));
    if (!stream->cluster_map || !stream->symbols) {
        ret = HYD_NOMEM;
        goto fail;
    }
    memcpy(stream->cluster_map, cluster_map, num_dists - !!lz77_min_symbol);
    for (size_t i = 0; i < num_dists - !!lz77_min_symbol; i++) {
        if (stream->cluster_map[i] >= stream->num_clusters)
            stream->num_clusters = stream->cluster_map[i] + 1;
    }
    if (stream->num_clusters > num_dists) {
        ret = HYD_INTERNAL_ERROR;
        goto fail;
    }

    if (lz77_min_symbol)
        stream->cluster_map[num_dists - 1] = stream->num_clusters++;

    stream->configs = hyd_mallocarray(allocator, stream->num_clusters, sizeof(HYDHybridUintConfig));
    stream->alphabet_sizes = hyd_calloc(allocator, stream->num_clusters, sizeof(uint32_t));
    if (!stream->configs || !stream->alphabet_sizes) {
        ret = HYD_NOMEM;
        goto fail;
    }

    if (!custom_configs) {
        hyd_entropy_set_hybrid_config(stream, 0, stream->num_clusters - !!stream->lz77_min_symbol, 4, 1, 1);
        if (stream->lz77_min_symbol)
            hyd_entropy_set_hybrid_config(stream, stream->num_clusters - 1, stream->num_clusters, 4, 0, 0);
    }

    return HYD_OK;

fail:
    hyd_entropy_stream_destroy(stream);
    return ret;
}

static void hybridize(uint32_t symbol, HYDHybridSymbol *hybrid_symbol, const HYDHybridUintConfig *config) {
    int split = 1 << config->split_exponent;
    if (symbol < split) {
        hybrid_symbol->token = symbol;
        hybrid_symbol->residue = hybrid_symbol->residue_bits = 0;
    } else {
        uint32_t n = hyd_fllog2(symbol) - config->lsb_in_token - config->msb_in_token;
        uint32_t low = symbol & ~(~UINT32_C(0) << config->lsb_in_token);
        symbol >>= config->lsb_in_token;
        hybrid_symbol->residue = symbol & ~(~UINT32_C(0) << n);
        symbol >>= n;
        uint32_t high = symbol & ~(~UINT32_C(0) << config->msb_in_token);
        hybrid_symbol->residue_bits = n;
        hybrid_symbol->token = split + (low | (high << config->lsb_in_token) |
                        ((n - config->split_exponent + config->lsb_in_token + config->msb_in_token) <<
                        (config->msb_in_token + config->lsb_in_token)));
    }
}

static HYDStatusCode send_hybridized_symbol(HYDEntropyStream *stream, const HYDHybridSymbol *symbol) {
    if (stream->symbol_pos >= stream->symbol_count) {
        HYDHybridSymbol *symbols = hyd_reallocarray(stream->allocator, stream->symbols, stream->symbol_count << 1,
            sizeof(HYDHybridSymbol));
        if (!symbols)
            return HYD_NOMEM;
        stream->symbols = symbols;
        stream->symbol_count <<= 1;
    }
    stream->symbols[stream->symbol_pos++] = *symbol;
    if (!stream->wrote_stream_header) {
        if (symbol->token >= stream->max_alphabet_size)
            stream->max_alphabet_size = 1 + symbol->token;
        if (symbol->token >= stream->alphabet_sizes[symbol->cluster])
            stream->alphabet_sizes[symbol->cluster] = 1 + symbol->token;
    }
    return HYD_OK;
}

static HYDStatusCode send_entropy_symbol0(HYDEntropyStream *stream, size_t dist, uint32_t symbol,
                                          const HYDHybridUintConfig *extra_config) {
    HYDHybridSymbol hybrid_symbol;
    hybrid_symbol.cluster = stream->cluster_map[dist];
    const HYDHybridUintConfig *config = extra_config ? extra_config : &stream->configs[hybrid_symbol.cluster];
    hybridize(symbol, &hybrid_symbol, config);
    return send_hybridized_symbol(stream, &hybrid_symbol);
}

static HYDStatusCode flush_lz77(HYDEntropyStream *stream, size_t dist) {
    HYDStatusCode ret;
    uint32_t last_symbol = stream->last_symbol - 1;

    if (stream->lz77_rle_count > stream->lz77_min_length) {
        uint32_t repeat_count = stream->lz77_rle_count - stream->lz77_min_length;
        HYDHybridSymbol hybrid_symbol;
        hybridize(repeat_count, &hybrid_symbol, &lz77_len_conf);
        hybrid_symbol.cluster = stream->cluster_map[dist];
        hybrid_symbol.token += stream->lz77_min_symbol;
        if ((ret = send_hybridized_symbol(stream, &hybrid_symbol)) < HYD_ERROR_START)
            return ret;
        if ((ret = send_entropy_symbol0(stream, stream->num_clusters - 1, 0, NULL)) < HYD_ERROR_START)
            return ret;
    } else if (stream->last_symbol) {
        for (uint32_t k = 0; k < stream->lz77_rle_count; k++) {
            if ((ret = send_entropy_symbol0(stream, dist, last_symbol, NULL)) < HYD_ERROR_START)
                return ret;
        }
    }

    stream->lz77_rle_count = 0;

    return HYD_OK;
}

HYDStatusCode hyd_entropy_send_symbol(HYDEntropyStream *stream, size_t dist, uint32_t symbol) {
    HYDStatusCode ret = HYD_OK;

    if (!stream->lz77_min_symbol)
        return send_entropy_symbol0(stream, dist, symbol, NULL);

    if (stream->last_symbol == symbol + 1) {
        if (++stream->lz77_rle_count < 128)
            return HYD_OK;
        stream->lz77_rle_count--;
    }

    if ((ret = flush_lz77(stream, dist)) < HYD_ERROR_START)
        return ret;

    stream->last_symbol = symbol + 1;

    return send_entropy_symbol0(stream, dist, symbol, NULL);
}

static HYDStatusCode stream_header_common(HYDEntropyStream *stream, int *las, int prefix_codes) {
    HYDStatusCode ret = HYD_OK;
    HYDBitWriter *bw = stream->bw;
    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    *las = log_alphabet_size;
    hyd_write_bool(bw, stream->lz77_min_symbol);
    if (stream->lz77_min_symbol) {
        flush_lz77(stream, 0);
        hyd_write_u32(bw, (const uint32_t[4]){224, 512, 4096, 8}, (const uint32_t[4]){0, 0, 0, 15},
            stream->lz77_min_symbol);
        hyd_write_u32(bw, (const uint32_t[4]){3, 4, 5, 9}, (const uint32_t[4]){0, 0, 2, 8},
            stream->lz77_min_length);
        ret = write_hybrid_uint_config(stream, &lz77_len_conf, 8);
    }
    if (ret < HYD_ERROR_START)
        return ret;
    if ((ret = write_cluster_map(stream)) < HYD_ERROR_START)
        return ret;

    hyd_write_bool(bw, prefix_codes);
    if (!prefix_codes)
        hyd_write(bw, log_alphabet_size - 5, 2);

    for (size_t i = 0; i < stream->num_clusters; i++) {
        ret = write_hybrid_uint_config(stream, &stream->configs[i], prefix_codes ? 15 : log_alphabet_size);
        if (ret < HYD_ERROR_START)
            return ret;
    }

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    /* populate frequencies */
    stream->frequencies = hyd_calloc(stream->allocator, table_size, sizeof(uint32_t));
    if (!stream->frequencies)
        return HYD_NOMEM;
    for (size_t pos = 0; pos < stream->symbol_pos; pos++)
        stream->frequencies[stream->symbols[pos].cluster * stream->max_alphabet_size + stream->symbols[pos].token]++;

    return bw->overflow_state;
}

static int symbol_compare(const void *a, const void *b) {
    const HYDVLCElement *vlc_a = a;
    const HYDVLCElement *vlc_b = b;
    if (vlc_a->length == vlc_b->length)
        return vlc_a->symbol - vlc_b->symbol;

    return !vlc_b->length ? -1 : !vlc_a->length ? 1 : vlc_a->length - vlc_b->length;
}

static int huffman_compare(const FrequencyEntry *fa, const FrequencyEntry *fb) {
    const int32_t pb = fb->frequency;
    const int32_t pa = fa->frequency;
    const int32_t ta = fa->token;
    const int32_t tb = fb->token;
    return pa != pb ? (!pb ? -1 : !pa ? 1 : pa - pb) : (!tb ? -1 : !ta ? 1 : ta - tb);
}

static int32_t collect(FrequencyEntry *entry) {
    if (!entry)
        return 0;
    int32_t self = ++entry->depth;
    int32_t left = collect(entry->left_child);
    int32_t right = collect(entry->right_child);
    return entry->max_depth = hyd_max3(self, left, right);
}

static HYDStatusCode build_huffman_tree(HYDAllocator *allocator, const uint32_t *frequencies,
                                        uint32_t *lengths, uint32_t alphabet_size, int32_t max_depth) {
    HYDStatusCode ret = HYD_OK;
    FrequencyEntry *tree = hyd_calloc(allocator, (2 * alphabet_size - 1), sizeof(FrequencyEntry));
    if (!tree) {
        ret = HYD_NOMEM;
        goto end;
    }

    for (uint32_t token = 0; token < alphabet_size; token++) {
        tree[token].frequency = frequencies[token];
        tree[token].token = 1 + token;
        tree[token].left_child = tree[token].right_child = NULL;
    }

    if (max_depth < 0)
        max_depth = hyd_cllog2(alphabet_size);

    memset(tree + alphabet_size, 0, (alphabet_size - 1) * sizeof(FrequencyEntry));
    for (uint32_t k = 0; k < alphabet_size - 1; k++) {
        FrequencyEntry *smallest = NULL;
        FrequencyEntry *second = NULL;
        int32_t nz = 0;
        for (uint32_t j = 2 * k; j < alphabet_size + k; j++)
            nz += !!tree[j].frequency;
        int32_t target = max_depth - (nz > 1 ? hyd_cllog2(nz - 1) : 0);
        for (uint32_t j = 2 * k; j < alphabet_size + k; j++) {
            if (tree[j].max_depth >= target)
                continue;
            if (!smallest || huffman_compare(&tree[j], smallest) < 0) {
                second = smallest;
                smallest = &tree[j];
            } else if (!second || huffman_compare(&tree[j], second) < 0) {
                second = &tree[j];
            }
        }
        if (!smallest || !second) {
            ret = HYD_INTERNAL_ERROR;
            goto end;
        }
        if (!second->frequency)
            break;
        hyd_swap(FrequencyEntry, *smallest, tree[2 * k]);
        smallest = &tree[2 * k];
        hyd_swap(FrequencyEntry, *second, tree[2 * k + 1]);
        second = &tree[2 * k + 1];
        FrequencyEntry *entry = &tree[alphabet_size + k];
        entry->frequency = smallest->frequency + second->frequency;
        entry->left_child = smallest;
        entry->right_child = second;
        collect(entry);
    }

    for (uint32_t j = 0; j < 2 * alphabet_size - 1; j++) {
        if (tree[j].token)
            lengths[tree[j].token - 1] = tree[j].depth;
    }

end:
    hyd_free(allocator, tree);
    return ret;
}

static HYDStatusCode build_prefix_table(HYDAllocator *allocator, HYDVLCElement *table,
                                        const uint32_t *lengths, uint32_t alphabet_size) {
    HYDStatusCode ret = HYD_OK;
    HYDVLCElement *pre_table = hyd_mallocarray(allocator, alphabet_size, sizeof(HYDVLCElement));
    if (!pre_table)
        return HYD_NOMEM;

    for (int32_t j = 0; j < alphabet_size; j++) {
        pre_table[j].length = lengths[j];
        pre_table[j].symbol = j;
    }

    qsort(pre_table, alphabet_size, sizeof(HYDVLCElement), &symbol_compare);

    uint64_t code = 0;
    for (int32_t j = 0; j < alphabet_size; j++) {
        if (!pre_table[j].length)
            continue;
        uint32_t s = pre_table[j].symbol;
        table[s].symbol = hyd_bitswap32(code);
        table[s].length = pre_table[j].length;
        code += UINT64_C(1) << (32 - pre_table[j].length);
    }

    if (code && code != (UINT64_C(1) << 32))
        ret = HYD_INTERNAL_ERROR;

    hyd_free(allocator, pre_table);
    return ret;
}

static void flush_zeroes(HYDBitWriter *bw, const HYDVLCElement *level1_table, uint32_t num_zeroes) {
    if (num_zeroes >= 3) {
        int32_t k = 0;
        uint32_t nz_residues[8];
        while (num_zeroes > 10) {
            uint32_t new_num_zeroes = (num_zeroes + 13) / 8;
            nz_residues[k++] = num_zeroes - 8 * new_num_zeroes + 16;
            num_zeroes = new_num_zeroes;
        }
        nz_residues[k++] = num_zeroes;
        for (int32_t l = k - 1; l >= 0; l--) {
            hyd_write(bw, level1_table[17].symbol, level1_table[17].length);
            uint32_t res = nz_residues[l];
            hyd_write(bw, res - 3, 3);
        }
    } else if (num_zeroes) {
        for (uint32_t k = 0; k < num_zeroes; k++)
            hyd_write(bw, level1_table[0].symbol, level1_table[0].length);
    }
}

static HYDStatusCode write_complex_prefix_lengths(HYDEntropyStream *stream, uint32_t alphabet_size,
                                                  const uint32_t *lengths) {
    HYDBitWriter *bw = stream->bw;
    HYDStatusCode ret = HYD_OK;
    HYDVLCElement *level1_table = NULL;

    // hskip = 0
    hyd_write(bw, 0, 2);

    uint32_t level1_freqs[18] = { 0 };

    uint32_t num_zeroes = 0;
    for (uint32_t j = 0; j < alphabet_size; j++) {
        uint32_t code = lengths[j];
        if (!code) {
            num_zeroes++;
            continue;
        }
        if (num_zeroes >= 3) {
            while (num_zeroes > 10) {
                level1_freqs[17]++;
                num_zeroes = (num_zeroes + 13) / 8;
            }
            level1_freqs[17]++;
        } else {
            level1_freqs[0] += num_zeroes;
        }
        num_zeroes = 0;
        level1_freqs[code]++;
    }

    uint32_t level1_lengths[18] = { 0 };
    ret = build_huffman_tree(stream->allocator, level1_freqs, level1_lengths, 18, 5);
    if (ret < HYD_ERROR_START)
        goto end;

    uint32_t total_code = 0;
    for (uint32_t j = 0; j < 18; j++) {
        uint32_t code = level1_lengths[prefix_zig_zag[j]];
        hyd_write(bw, prefix_level0_table[code].symbol,
                      prefix_level0_table[code].length);
        if (code)
            total_code += 32 >> code;
        if (total_code >= 32)
            break;
    }
    if (total_code && total_code != 32) {
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    level1_table = hyd_calloc(stream->allocator, 18, sizeof(HYDVLCElement));
    if (!level1_table) {
        ret = HYD_NOMEM;
        goto end;
    }

    ret = build_prefix_table(stream->allocator, level1_table, level1_lengths, 18);
    if (ret < HYD_ERROR_START)
        goto end;

    total_code = 0;
    num_zeroes = 0;
    for (uint32_t j = 0; j < alphabet_size; j++) {
        uint32_t code = lengths[j];
        if (!code) {
            num_zeroes++;
            continue;
        }
        flush_zeroes(bw, level1_table, num_zeroes);
        num_zeroes = 0;
        hyd_write(bw, level1_table[code].symbol, level1_table[code].length);
        total_code += 32768 >> code;
        if (total_code == 32768)
            break;
    }
    flush_zeroes(bw, level1_table, num_zeroes);

end:
    hyd_free(stream->allocator, level1_table);
    return ret;
}

HYDStatusCode hyd_prefix_write_stream_header(HYDEntropyStream *stream) {
    HYDStatusCode ret;
    int log_alphabet_size;
    HYDBitWriter *bw = stream->bw;
    uint32_t *global_lengths = NULL;

    if ((ret = stream_header_common(stream, &log_alphabet_size, 1)) < HYD_ERROR_START)
        goto fail;

    for (size_t i = 0; i < stream->num_clusters; i++) {
        if (stream->alphabet_sizes[i] <= 1) {
            hyd_write_bool(bw, 0);
            continue;
        }
        hyd_write_bool(bw, 1);
        int n = hyd_fllog2(stream->alphabet_sizes[i] - 1);
        hyd_write(bw, n, 4);
        hyd_write(bw, stream->alphabet_sizes[i] - 1, n);
    }

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    global_lengths = hyd_calloc(stream->allocator, table_size, sizeof(uint32_t));
    stream->vlc_table = hyd_calloc(stream->allocator, table_size, sizeof(HYDVLCElement));
    if (!global_lengths || !stream->vlc_table) {
        ret = HYD_NOMEM;
        goto fail;
    }

    for (size_t i = 0; i < stream->num_clusters; i++) {
        if (stream->alphabet_sizes[i] <= 1)
            continue;
        uint32_t *lengths = global_lengths + i * stream->max_alphabet_size;
        const uint32_t *freqs = stream->frequencies + i * stream->max_alphabet_size;

        ret = build_huffman_tree(stream->allocator, freqs, lengths, stream->alphabet_sizes[i], 15);
        if (ret < HYD_ERROR_START)
            goto fail;
        uint32_t nsym = 0;
        HYDVLCElement tokens[4] = { 0 };
        for (uint32_t j = 0; j < stream->alphabet_sizes[i]; j++) {
            if (lengths[j]) {
                if (nsym < 4) {
                    tokens[nsym].symbol = j;
                    tokens[nsym].length = lengths[j];
                }
                nsym++;
            }
        }

        if (nsym > 4) {
            if ((ret = write_complex_prefix_lengths(stream, stream->alphabet_sizes[i], lengths)) < HYD_ERROR_START)
                goto fail;
            continue;
        }

        if (nsym == 0) {
            nsym = 1;
            tokens[0].symbol = stream->alphabet_sizes[i] - 1;
        }

        // hskip = 1
        hyd_write(bw, 1, 2);
        hyd_write(bw, nsym - 1, 2);
        int log_alphabet_size = hyd_cllog2(stream->alphabet_sizes[i]);
        if (nsym > 1)
            qsort(tokens, 4, sizeof(HYDVLCElement), &symbol_compare);
        for (int n = 0; n < nsym; n++)
            hyd_write(bw, tokens[n].symbol, log_alphabet_size);
        if (nsym == 4)
            hyd_write_bool(bw, tokens[3].length == 3);
    }

    for (size_t i = 0; i < stream->num_clusters; i++) {
        HYDVLCElement *table = stream->vlc_table + i * stream->max_alphabet_size;
        const uint32_t *lengths = global_lengths + i * stream->max_alphabet_size;
        ret = build_prefix_table(stream->allocator, table, lengths, stream->alphabet_sizes[i]);
        if (ret < HYD_ERROR_START)
            goto fail;
    }

    hyd_free(stream->allocator, global_lengths);
    stream->wrote_stream_header = 1;
    return bw->overflow_state;

fail:
    hyd_free(stream->allocator, global_lengths);
    hyd_entropy_stream_destroy(stream);
    return ret;
}

HYDStatusCode hyd_ans_write_stream_header(HYDEntropyStream *stream) {

    HYDStatusCode ret;
    int log_alphabet_size;
    HYDBitWriter *bw = stream->bw;

    for (size_t i = 0; i < stream->num_clusters; i++) {
        const HYDHybridUintConfig *config = &stream->configs[i];
        size_t max_token = (((29 - config->split_exponent + config->msb_in_token + config->lsb_in_token) <<
            (config->msb_in_token + config->lsb_in_token)) | ~(~UINT32_C(0) << (config->msb_in_token +
            config->lsb_in_token))) + (1 << config->split_exponent);
        stream->max_alphabet_size = hyd_max(stream->max_alphabet_size, max_token);
    }

    if ((ret = stream_header_common(stream, &log_alphabet_size, 0)) < HYD_ERROR_START)
        goto fail;

    size_t table_size = stream->num_clusters * stream->max_alphabet_size;

    /* generate alias mappings */
    stream->alias_table = hyd_calloc(stream->allocator, table_size, sizeof(HYDAliasEntry));
    if (!stream->alias_table) {
        ret = HYD_NOMEM;
        goto fail;
    }
    for (size_t i = 0; i < stream->num_clusters; i++) {
        int32_t uniq_pos = write_ans_frequencies(stream, stream->frequencies + i * stream->max_alphabet_size);
        if (uniq_pos < HYD_ERROR_START) {
            ret = uniq_pos;
            goto fail;
        }
        ret = generate_alias_mapping(stream, i, log_alphabet_size, uniq_pos);
        if (ret < HYD_ERROR_START)
            goto fail;
    }

    stream->wrote_stream_header = 1;

    return bw->overflow_state;

fail:
    hyd_entropy_stream_destroy(stream);
    return ret;
}

HYDStatusCode hyd_prefix_write_stream_symbols(HYDEntropyStream *stream, size_t symbol_start, size_t symbol_count) {
    HYDBitWriter *bw = stream->bw;

    if (symbol_count + symbol_start > stream->symbol_pos)
        return HYD_INTERNAL_ERROR;

    const HYDHybridSymbol *symbols = stream->symbols + symbol_start;
    for (size_t p = 0; p < symbol_count; p++) {
        size_t cluster = symbols[p].cluster;
        uint32_t token = symbols[p].token;
        const HYDVLCElement *entry = &stream->vlc_table[cluster * stream->max_alphabet_size + token];
        hyd_write(bw, entry->symbol, entry->length);
        hyd_write(bw, symbols[p].residue, symbols[p].residue_bits);
    }

    return bw->overflow_state;
}

HYDStatusCode hyd_prefix_finalize_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = hyd_prefix_write_stream_header(stream);
    if (ret < HYD_ERROR_START)
        goto end;
    ret = hyd_prefix_write_stream_symbols(stream, 0, stream->symbol_pos);

end:
    hyd_entropy_stream_destroy(stream);
    return ret;
}

static HYDStatusCode append_state_flush(HYDAllocator *allocator, StateFlushChain **flushes,
                                        size_t token_index, uint16_t value) {
    if ((*flushes)->pos == (*flushes)->capacity) {
        StateFlushChain *chain = hyd_malloc(allocator, sizeof(StateFlushChain));
        if (!chain)
            return HYD_NOMEM;
        chain->state_flushes = hyd_mallocarray(allocator, 1 << 10, sizeof(StateFlush));
        if (!chain->state_flushes){
            hyd_free(allocator, chain);
            return HYD_NOMEM;
        }
        chain->capacity = 1 << 10;
        chain->pos = 0;
        chain->prev_chain = *flushes;
        *flushes = chain;
    }
    (*flushes)->state_flushes[(*flushes)->pos++] = (StateFlush){token_index, value};

    return HYD_OK;
}

static StateFlush *pop_state_flush(HYDAllocator *allocator, StateFlushChain **flushes) {
    if ((*flushes)->pos > 0)
        return &(*flushes)->state_flushes[--(*flushes)->pos];
    StateFlushChain *prev_chain = (*flushes)->prev_chain;
    if (!prev_chain)
        return NULL;
    hyd_free(allocator, (*flushes)->state_flushes);
    hyd_free(allocator, *flushes);
    *flushes = prev_chain;
    return pop_state_flush(allocator, flushes);
}

HYDStatusCode hyd_ans_write_stream_symbols(HYDEntropyStream *stream, size_t symbol_start, size_t symbol_count) {
    HYDStatusCode ret = HYD_OK;
    StateFlushChain flushes_base = { 0 }, *flushes = &flushes_base;
    HYDBitWriter *bw = stream->bw;
    int log_alphabet_size = hyd_cllog2(stream->max_alphabet_size);
    if (log_alphabet_size < 5)
        log_alphabet_size = 5;
    const uint32_t log_bucket_size = 12 - log_alphabet_size;
    const uint32_t pos_mask = ~(~UINT32_C(0) << log_bucket_size);
    if (!stream->alias_table) {
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    if (symbol_count + symbol_start > stream->symbol_pos) {
        ret = HYD_INTERNAL_ERROR;
        goto end;
    }

    flushes->state_flushes = hyd_mallocarray(stream->allocator, 1024, sizeof(StateFlush));
    if (!flushes->state_flushes) {
        ret = HYD_NOMEM;
        goto end;
    }
    flushes->capacity = 1 << 10;

    uint32_t state = 0x130000;
    const HYDHybridSymbol *symbols = stream->symbols + symbol_start;
    for (size_t p2 = 0; p2 < symbol_count; p2++) {
        const size_t p = symbol_count - p2 - 1;
        const uint8_t symbol = symbols[p].token;
        const size_t cluster = symbols[p].cluster;
        const size_t index = cluster * stream->max_alphabet_size + symbol;
        const uint32_t freq = stream->frequencies[index];
        if ((state >> 20) >= freq) {
            append_state_flush(stream->allocator, &flushes, p, state & 0xFFFF);
            state >>= 16;
        }
        const uint32_t offset = state % freq;
        uint32_t i, pos, j;
        for (j = 0; j <= stream->alias_table[index].count; j++) {
            pos = offset - stream->alias_table[index].offsets[j];
            int32_t k = pos - stream->alias_table[index].cutoffs[j];
            if (pos <= pos_mask && (j > 0 ? k >= 0 : k < 0)) {
                i = stream->alias_table[index].original[j];
                break;
            }
        }
        if (j > stream->alias_table[index].count) {
            ret = HYD_INTERNAL_ERROR;
            goto end;
        }
        state = ((state / freq) << 12) | (i << log_bucket_size) | pos;
    }
    append_state_flush(stream->allocator, &flushes, 0, (state >> 16) & 0xFFFF);
    append_state_flush(stream->allocator, &flushes, 0, state & 0xFFFF);
    for (size_t p = 0; p < symbol_count; p++) {
        StateFlush *flush;
        while ((flush = pop_state_flush(stream->allocator, &flushes))) {
            if (p >= flush->token_index) {
                hyd_write(bw, flush->value, 16);
            } else {
                flushes->pos++;
                break;
            }
        }
        hyd_write(bw, symbols[p].residue, symbols[p].residue_bits);
    }

    ret = bw->overflow_state;

end:
    while (flushes->prev_chain) {
        StateFlushChain *prev = flushes->prev_chain;
        hyd_free(stream->allocator, flushes->state_flushes);
        hyd_free(stream->allocator, flushes);
        flushes = prev;
    }
    hyd_free(stream->allocator, flushes->state_flushes);
    return ret;
}

HYDStatusCode hyd_ans_finalize_stream(HYDEntropyStream *stream) {
    HYDStatusCode ret = hyd_ans_write_stream_header(stream);
    if (ret < HYD_ERROR_START)
        goto end;
    ret = hyd_ans_write_stream_symbols(stream, 0, stream->symbol_pos);

end:
    hyd_entropy_stream_destroy(stream);
    return ret;
}
