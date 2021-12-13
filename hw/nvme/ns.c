/*
 * QEMU NVM Express Virtual Namespace
 *
 * Copyright (c) 2019 CNEX Labs
 * Copyright (c) 2020 Samsung Electronics
 *
 * Authors:
 *  Klaus Jensen      <k.jensen@samsung.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See the
 * COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"

#include "nvme.h"
#include "zns.h"
#include "trace.h"

#define MIN_DISCARD_GRANULARITY (4 * KiB)

void nvme_ns_init_format(NvmeNamespace *ns)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    BlockDriverInfo bdi;
    int npdg, nlbas, ret;

    ns->lbaf = id_ns->lbaf[NVME_ID_NS_FLBAS_INDEX(id_ns->flbas)];
    ns->lbasz = 1 << ns->lbaf.ds;

    nlbas = ns->size / (ns->lbasz + ns->lbaf.ms);

    id_ns->nsze = cpu_to_le64(nlbas);

    /* no thin provisioning */
    id_ns->ncap = id_ns->nsze;
    id_ns->nuse = id_ns->ncap;

    ns->moff = (int64_t)nlbas << ns->lbaf.ds;

    npdg = ns->discard_granularity / ns->lbasz;

    ret = bdrv_get_info(blk_bs(ns->blk), &bdi);
    if (ret >= 0 && bdi.cluster_size > ns->discard_granularity) {
        npdg = bdi.cluster_size / ns->lbasz;
    }

    id_ns->npda = id_ns->npdg = npdg - 1;
}

static int nvme_ns_init(NvmeNamespace *ns, Error **errp)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    uint8_t ds;
    uint16_t ms;
    int i;

    ns->csi = NVME_CSI_NVM;
    ns->status = 0x0;

    ns->id_ns.dlfeat = 0x1;

    /* support DULBE and I/O optimization fields */
    id_ns->nsfeat |= (0x4 | 0x10);

    if (ns->flags & NVME_NS_SHARED) {
        id_ns->nmic |= NVME_NMIC_NS_SHARED;
    }

    id_ns->eui64 = cpu_to_be64(ns->eui64.v);

    /* simple copy */
    id_ns->mssrl = cpu_to_le16(ns->scc.mssrl);
    id_ns->mcl = cpu_to_le32(ns->scc.mcl);
    id_ns->msrc = ns->scc.msrc;

    ds = 31 - clz32(ns->blkconf.logical_block_size);
    ms = ns->params.ms;

    id_ns->mc = NVME_ID_NS_MC_EXTENDED | NVME_ID_NS_MC_SEPARATE;

    if (ns->flags & NVME_NS_NVM_EXTENDED_LBA) {
        id_ns->flbas |= NVME_ID_NS_FLBAS_EXTENDED;
    }

    id_ns->dpc = 0x1f;
    id_ns->dps = ns->pi_type;
    if (ns->pi_type && (ns->flags & NVME_NS_NVM_PROT_FIRST)) {
        id_ns->dps |= NVME_ID_NS_DPS_FIRST_EIGHT;
    }

    static const NvmeLBAF lbaf[16] = {
        [0] = { .ds =  9           },
        [1] = { .ds =  9, .ms =  8 },
        [2] = { .ds =  9, .ms = 16 },
        [3] = { .ds =  9, .ms = 64 },
        [4] = { .ds = 12           },
        [5] = { .ds = 12, .ms =  8 },
        [6] = { .ds = 12, .ms = 16 },
        [7] = { .ds = 12, .ms = 64 },
    };

    memcpy(&id_ns->lbaf, &lbaf, sizeof(lbaf));
    id_ns->nlbaf = 7;

    for (i = 0; i <= id_ns->nlbaf; i++) {
        NvmeLBAF *lbaf = &id_ns->lbaf[i];
        if (lbaf->ds == ds) {
            if (lbaf->ms == ms) {
                id_ns->flbas |= i;
                goto lbaf_found;
            }
        }
    }

    /* add non-standard lba format */
    id_ns->nlbaf++;
    id_ns->lbaf[id_ns->nlbaf].ds = ds;
    id_ns->lbaf[id_ns->nlbaf].ms = ms;
    id_ns->flbas |= id_ns->nlbaf;

lbaf_found:
    nvme_ns_init_format(ns);

    return 0;
}

static int nvme_ns_init_blkconf(NvmeNamespace *ns, BlockConf *blkconf,
                                Error **errp)
{
    bool read_only;

    if (!blkconf_blocksizes(blkconf, errp)) {
        return -1;
    }

    read_only = !blk_supports_write_perm(ns->blk);
    if (!blkconf_apply_backend_options(blkconf, read_only, false, errp)) {
        return -1;
    }

    if (blkconf->discard_granularity == -1) {
        blkconf->discard_granularity =
            MAX(blkconf->logical_block_size, MIN_DISCARD_GRANULARITY);
    }

    ns->discard_granularity = blkconf->discard_granularity;

    ns->size = blk_getlength(ns->blk);
    if (ns->size < 0) {
        error_setg_errno(errp, -ns->size, "could not get blockdev size");
        return -1;
    }

    return 0;
}

static int nvme_zns_check_calc_geometry(NvmeNamespace *ns, Error **errp)
{
    uint64_t zone_size, zone_cap;

    /* Make sure that the values of ZNS properties are sane */
    if (ns->params.zone_size_bs) {
        zone_size = ns->params.zone_size_bs;
    } else {
        zone_size = NVME_DEFAULT_ZONE_SIZE;
    }
    if (ns->params.zone_cap_bs) {
        zone_cap = ns->params.zone_cap_bs;
    } else {
        zone_cap = zone_size;
    }
    if (zone_cap > zone_size) {
        error_setg(errp, "zone capacity %"PRIu64"B exceeds "
                   "zone size %"PRIu64"B", zone_cap, zone_size);
        return -1;
    }
    if (zone_size < ns->lbasz) {
        error_setg(errp, "zone size %"PRIu64"B too small, "
                   "must be at least %zuB", zone_size, ns->lbasz);
        return -1;
    }
    if (zone_cap < ns->lbasz) {
        error_setg(errp, "zone capacity %"PRIu64"B too small, "
                   "must be at least %zuB", zone_cap, ns->lbasz);
        return -1;
    }

    /*
     * Save the main zone geometry values to avoid
     * calculating them later again.
     */
    ns->zone_size = zone_size / ns->lbasz;
    ns->zone_capacity = zone_cap / ns->lbasz;
    ns->num_zones = le64_to_cpu(ns->id_ns.nsze) / ns->zone_size;

    /* Do a few more sanity checks of ZNS properties */
    if (!ns->num_zones) {
        error_setg(errp,
                   "insufficient drive capacity, must be at least the size "
                   "of one zone (%"PRIu64"B)", zone_size);
        return -1;
    }

    return 0;
}

static int nvme_ns_check_constraints(NvmeNamespace *ns, Error **errp)
{
    if (!ns->blkconf.blk) {
        error_setg(errp, "block backend not configured");
        return -1;
    }

    if (ns->params.pi && ns->params.ms < 8) {
        error_setg(errp, "at least 8 bytes of metadata required to enable "
                   "protection information");
        return -1;
    }

    if (ns->params.nsid > NVME_MAX_NAMESPACES) {
        error_setg(errp, "invalid namespace id (must be between 0 and %d)",
                   NVME_MAX_NAMESPACES);
        return -1;
    }

    if (ns->params.zoned) {
        if (ns->params.max_active_zones) {
            if (ns->params.max_open_zones > ns->params.max_active_zones) {
                error_setg(errp, "max_open_zones (%u) exceeds "
                           "max_active_zones (%u)", ns->params.max_open_zones,
                           ns->params.max_active_zones);
                return -1;
            }

            if (!ns->params.max_open_zones) {
                ns->params.max_open_zones = ns->params.max_active_zones;
            }
        }

        if (ns->params.zd_extension_size) {
            if (ns->params.zd_extension_size & 0x3f) {
                error_setg(errp, "zone descriptor extension size must be a "
                           "multiple of 64B");
                return -1;
            }
            if ((ns->params.zd_extension_size >> 6) > 0xff) {
                error_setg(errp,
                           "zone descriptor extension size is too large");
                return -1;
            }
        }
    }

    return 0;
}

static void nvme_ns_set_params(NvmeNamespace *ns, NvmeNamespaceParams *params)
{
    ns->nsid = params->nsid;
    ns->pi_type = params->pi;

    ns->scc.mssrl = params->mssrl;
    ns->scc.mcl = params->mcl;
    ns->scc.msrc = params->msrc;

    memcpy(&ns->uuid, &params->uuid, sizeof(ns->uuid));

    if (params->eui64) {
        stq_be_p(&ns->eui64.v, params->eui64);
    }

    if (params->eui64_default) {
        ns->flags |= NVME_NS_EUI64_SET_DEFAULT;
    }

    if (params->shared) {
        ns->flags |= NVME_NS_SHARED;
    }

    if (params->mset) {
        ns->flags |= NVME_NS_NVM_EXTENDED_LBA;
    }

    if (params->pil) {
        ns->flags |= NVME_NS_NVM_PROT_FIRST;
    }

    if (params->zoned) {
        ns->flags |= NVME_NS_ZONED;

        ns->zd_extension_size = params->zd_extension_size;
        ns->max_open_zones = params->max_open_zones;
        ns->max_active_zones = params->max_active_zones;

        if (params->cross_zone_read) {
            ns->flags |= NVME_NS_ZONED_CROSS_READ;
        }
    }
}

int nvme_ns_setup(NvmeNamespace *ns, Error **errp)
{
    static uint64_t ns_count;

    ns->blk = ns->blkconf.blk;

    if (nvme_ns_check_constraints(ns, errp)) {
        return -1;
    }

    nvme_ns_set_params(ns, &ns->params);

    /* substitute a missing EUI-64 by an autogenerated one */
    ++ns_count;
    if (!ns->eui64.v && (ns->flags & NVME_NS_EUI64_SET_DEFAULT)) {
        ns->eui64.v = ns_count + NVME_EUI64_DEFAULT;
    }

    if (nvme_ns_init_blkconf(ns, &ns->blkconf, errp)) {
        return -1;
    }

    if (nvme_ns_init(ns, errp)) {
        return -1;
    }
    if (ns->flags & NVME_NS_ZONED) {
        if (nvme_zns_check_calc_geometry(ns, errp) != 0) {
            return -1;
        }
        nvme_zns_init(ns);
    }

    return 0;
}

void nvme_ns_drain(NvmeNamespace *ns)
{
    blk_drain(ns->blk);
}

void nvme_ns_shutdown(NvmeNamespace *ns)
{
    blk_flush(ns->blk);
    if (ns->flags & NVME_NS_ZONED) {
        nvme_zns_shutdown(ns);
    }
}

void nvme_ns_cleanup(NvmeNamespace *ns)
{
    if (ns->flags & NVME_NS_ZONED) {
        nvme_zns_cleanup(ns);
    }
}

static void nvme_ns_unrealize(DeviceState *dev)
{
    NvmeNamespace *ns = NVME_NS(dev);

    nvme_ns_drain(ns);
    nvme_ns_shutdown(ns);
    nvme_ns_cleanup(ns);
}

static void nvme_ns_realize(DeviceState *dev, Error **errp)
{
    NvmeNamespace *ns = NVME_NS(dev);
    BusState *s = qdev_get_parent_bus(dev);
    NvmeCtrl *n = NVME(s->parent);
    NvmeSubsystem *subsys = n->subsys;
    uint32_t nsid = ns->params.nsid;
    int i;

    if (!n->subsys) {
        if (ns->params.detached) {
            error_setg(errp, "detached requires that the nvme device is "
                       "linked to an nvme-subsys device");
            return;
        }
    } else {
        /*
         * If this namespace belongs to a subsystem (through a link on the
         * controller device), reparent the device.
         */
        if (!qdev_set_parent_bus(dev, &subsys->bus.parent_bus, errp)) {
            return;
        }
    }

    if (nvme_ns_setup(ns, errp)) {
        return;
    }

    if (!nsid) {
        for (i = 1; i <= NVME_MAX_NAMESPACES; i++) {
            if (nvme_ns(n, i) || nvme_subsys_ns(subsys, i)) {
                continue;
            }

            nsid = ns->nsid = i;
            break;
        }

        if (!nsid) {
            error_setg(errp, "no free namespace id");
            return;
        }
    } else {
        if (nvme_ns(n, nsid) || nvme_subsys_ns(subsys, nsid)) {
            error_setg(errp, "namespace id '%d' already allocated", nsid);
            return;
        }
    }

    if (subsys) {
        subsys->namespaces[nsid] = ns;

        if (ns->params.detached) {
            return;
        }

        if (ns->params.shared) {
            for (i = 0; i < ARRAY_SIZE(subsys->ctrls); i++) {
                NvmeCtrl *ctrl = subsys->ctrls[i];

                if (ctrl) {
                    nvme_attach_ns(ctrl, ns);
                }
            }

            return;
        }
    }

    nvme_attach_ns(n, ns);
}

static Property nvme_ns_props[] = {
    DEFINE_BLOCK_PROPERTIES(NvmeNamespace, blkconf),
    DEFINE_PROP_BOOL("detached", NvmeNamespace, params.detached, false),
    DEFINE_PROP_BOOL("shared", NvmeNamespace, params.shared, true),
    DEFINE_PROP_UINT32("nsid", NvmeNamespace, params.nsid, 0),
    DEFINE_PROP_UUID("uuid", NvmeNamespace, params.uuid),
    DEFINE_PROP_UINT64("eui64", NvmeNamespace, params.eui64, 0),
    DEFINE_PROP_UINT16("ms", NvmeNamespace, params.ms, 0),
    DEFINE_PROP_UINT8("mset", NvmeNamespace, params.mset, 0),
    DEFINE_PROP_UINT8("pi", NvmeNamespace, params.pi, 0),
    DEFINE_PROP_UINT8("pil", NvmeNamespace, params.pil, 0),
    DEFINE_PROP_UINT16("mssrl", NvmeNamespace, params.mssrl, 128),
    DEFINE_PROP_UINT32("mcl", NvmeNamespace, params.mcl, 128),
    DEFINE_PROP_UINT8("msrc", NvmeNamespace, params.msrc, 127),
    DEFINE_PROP_BOOL("zoned", NvmeNamespace, params.zoned, false),
    DEFINE_PROP_SIZE("zoned.zone_size", NvmeNamespace, params.zone_size_bs,
                     NVME_DEFAULT_ZONE_SIZE),
    DEFINE_PROP_SIZE("zoned.zone_capacity", NvmeNamespace, params.zone_cap_bs,
                     0),
    DEFINE_PROP_BOOL("zoned.cross_read", NvmeNamespace,
                     params.cross_zone_read, false),
    DEFINE_PROP_UINT32("zoned.max_active", NvmeNamespace,
                       params.max_active_zones, 0),
    DEFINE_PROP_UINT32("zoned.max_open", NvmeNamespace,
                       params.max_open_zones, 0),
    DEFINE_PROP_UINT32("zoned.descr_ext_size", NvmeNamespace,
                       params.zd_extension_size, 0),
    DEFINE_PROP_BOOL("eui64-default", NvmeNamespace, params.eui64_default,
                     true),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvme_ns_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->bus_type = TYPE_NVME_BUS;
    dc->realize = nvme_ns_realize;
    dc->unrealize = nvme_ns_unrealize;
    device_class_set_props(dc, nvme_ns_props);
    dc->desc = "Virtual NVMe namespace";
}

static void nvme_ns_instance_init(Object *obj)
{
    NvmeNamespace *ns = NVME_NS(obj);
    char *bootindex = g_strdup_printf("/namespace@%d,0", ns->params.nsid);

    device_add_bootindex_property(obj, &ns->bootindex, "bootindex",
                                  bootindex, DEVICE(obj));

    g_free(bootindex);
}

static const TypeInfo nvme_ns_info = {
    .name = TYPE_NVME_NS,
    .parent = TYPE_DEVICE,
    .class_init = nvme_ns_class_init,
    .instance_size = sizeof(NvmeNamespace),
    .instance_init = nvme_ns_instance_init,
};

static void nvme_ns_register_types(void)
{
    type_register_static(&nvme_ns_info);
}

type_init(nvme_ns_register_types)
