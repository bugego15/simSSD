#include "media/geometry.h"
#include "core/assert.h"

void nand_geo_init(nand_geometry_t *g, const ssd_config_t *cfg)
{
    SSD_ASSERT(g != NULL);
    SSD_ASSERT(cfg != NULL);

    g->channels         = cfg->channels;
    g->ces_per_ch       = cfg->ces_per_ch;
    g->dies_per_ce      = cfg->dies_per_ce;
    g->planes_per_die   = cfg->planes_per_die;
    g->blocks_per_plane = cfg->blocks_per_plane;
    g->pages_per_block  = cfg->pages_per_block;

    g->blocks_per_die = g->planes_per_die * g->blocks_per_plane;
    g->blocks_per_ce  = g->dies_per_ce * g->blocks_per_die;
    g->blocks_per_ch  = g->ces_per_ch * g->blocks_per_ce;

    g->total_blocks = (uint64_t)g->channels * g->blocks_per_ch;
    g->total_pages  = g->total_blocks * g->pages_per_block;
}

ppn_t nand_geo_ppn(const nand_geometry_t *g, const nand_addr_t *a)
{
    uint64_t idx;

    SSD_ASSERT(g != NULL);
    SSD_ASSERT(a != NULL);
    SSD_ASSERT(a->channel < g->channels);
    SSD_ASSERT(a->ce < g->ces_per_ch);
    SSD_ASSERT(a->die < g->dies_per_ce);
    SSD_ASSERT(a->plane < g->planes_per_die);
    SSD_ASSERT(a->block < g->blocks_per_plane);
    SSD_ASSERT(a->page < g->pages_per_block);

    idx = (uint64_t)a->channel * g->blocks_per_ch +
          (uint64_t)a->ce * g->blocks_per_ce +
          (uint64_t)a->die * g->blocks_per_die +
          (uint64_t)a->plane * g->blocks_per_plane +
          (uint64_t)a->block;

    return idx * g->pages_per_block + a->page;
}

void nand_geo_decode(const nand_geometry_t *g, ppn_t ppn, nand_addr_t *out)
{
    pbn_t pbn;

    SSD_ASSERT(g != NULL);
    SSD_ASSERT(out != NULL);
    SSD_ASSERT(ppn < g->total_pages);

    pbn = ppn / g->pages_per_block;
    nand_geo_decode_block(g, pbn, out);
    out->page = (uint32_t)(ppn % g->pages_per_block);
}

pbn_t nand_geo_pbn(const nand_geometry_t *g, const nand_addr_t *a)
{
    SSD_ASSERT(g != NULL);
    SSD_ASSERT(a != NULL);
    SSD_ASSERT(a->channel < g->channels);
    SSD_ASSERT(a->ce < g->ces_per_ch);
    SSD_ASSERT(a->die < g->dies_per_ce);
    SSD_ASSERT(a->plane < g->planes_per_die);
    SSD_ASSERT(a->block < g->blocks_per_plane);

    return (uint64_t)a->channel * g->blocks_per_ch +
           (uint64_t)a->ce * g->blocks_per_ce +
           (uint64_t)a->die * g->blocks_per_die +
           (uint64_t)a->plane * g->blocks_per_plane +
           (uint64_t)a->block;
}

void nand_geo_decode_block(const nand_geometry_t *g, pbn_t pbn, nand_addr_t *out)
{
    uint32_t rem;

    SSD_ASSERT(g != NULL);
    SSD_ASSERT(out != NULL);
    SSD_ASSERT(pbn < g->total_blocks);

    rem = (uint32_t)pbn;

    out->channel = rem / g->blocks_per_ch;
    rem %= g->blocks_per_ch;

    out->ce = rem / g->blocks_per_ce;
    rem %= g->blocks_per_ce;

    out->die = rem / g->blocks_per_die;
    rem %= g->blocks_per_die;

    out->plane = rem / g->blocks_per_plane;
    rem %= g->blocks_per_plane;

    out->block = rem;
    out->page  = 0;
}

ppn_t nand_geo_block_base(const nand_geometry_t *g, pbn_t pbn)
{
    SSD_ASSERT(g != NULL);
    SSD_ASSERT(pbn < g->total_blocks);
    return pbn * g->pages_per_block;
}

pbn_t nand_geo_pbn_of_ppn(const nand_geometry_t *g, ppn_t ppn)
{
    SSD_ASSERT(g != NULL);
    SSD_ASSERT(ppn < g->total_pages);
    return ppn / g->pages_per_block;
}

uint32_t nand_geo_channel_of_pbn(const nand_geometry_t *g, pbn_t pbn)
{
    SSD_ASSERT(g != NULL);
    SSD_ASSERT(pbn < g->total_blocks);
    return (uint32_t)(pbn / g->blocks_per_ch);
}

uint32_t nand_geo_channel_of_ppn(const nand_geometry_t *g, ppn_t ppn)
{
    return nand_geo_channel_of_pbn(g, nand_geo_pbn_of_ppn(g, ppn));
}
