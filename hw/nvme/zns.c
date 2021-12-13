#include "qemu/osdep.h"

#include "zns.h"
#include "trace.h"

void nvme_zns_assign_state(NvmeNamespace *ns, NvmeZone *zone,
                           NvmeZoneState state)
{
    if (QTAILQ_IN_USE(zone, entry)) {
        switch (nvme_zns_state(zone)) {
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            QTAILQ_REMOVE(&ns->exp_open_zones, zone, entry);
            break;
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
            QTAILQ_REMOVE(&ns->imp_open_zones, zone, entry);
            break;
        case NVME_ZONE_STATE_CLOSED:
            QTAILQ_REMOVE(&ns->closed_zones, zone, entry);
            break;
        case NVME_ZONE_STATE_FULL:
            QTAILQ_REMOVE(&ns->full_zones, zone, entry);
        default:
            ;
        }
    }

    nvme_zns_set_state(zone, state);

    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        QTAILQ_INSERT_TAIL(&ns->exp_open_zones, zone, entry);
        break;
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        QTAILQ_INSERT_TAIL(&ns->imp_open_zones, zone, entry);
        break;
    case NVME_ZONE_STATE_CLOSED:
        QTAILQ_INSERT_TAIL(&ns->closed_zones, zone, entry);
        break;
    case NVME_ZONE_STATE_FULL:
        QTAILQ_INSERT_TAIL(&ns->full_zones, zone, entry);
    case NVME_ZONE_STATE_READ_ONLY:
        break;
    default:
        zone->d.za = 0;
    }
}

/*
 * Check if we can open a zone without exceeding open/active limits.
 * AOR stands for "Active and Open Resources" (see TP 4053 section 2.5).
 */
int nvme_zns_aor_check(NvmeNamespace *ns, uint32_t act, uint32_t opn)
{
    if (ns->params.max_active_zones != 0 &&
        ns->nr_active_zones + act > ns->params.max_active_zones) {
        trace_pci_nvme_err_insuff_active_res(ns->params.max_active_zones);
        return NVME_ZONE_TOO_MANY_ACTIVE | NVME_DNR;
    }
    if (ns->params.max_open_zones != 0 &&
        ns->nr_open_zones + opn > ns->params.max_open_zones) {
        trace_pci_nvme_err_insuff_open_res(ns->params.max_open_zones);
        return NVME_ZONE_TOO_MANY_OPEN | NVME_DNR;
    }

    return NVME_SUCCESS;
}

void nvme_zns_init_state(NvmeNamespace *ns)
{
    uint64_t start = 0, zone_size = ns->zone_size;
    uint64_t capacity = ns->num_zones * zone_size;
    NvmeZone *zone;
    int i;

    ns->zone_array = g_new0(NvmeZone, ns->num_zones);
    if (ns->params.zd_extension_size) {
        ns->zd_extensions = g_malloc0(ns->params.zd_extension_size *
                                      ns->num_zones);
    }

    QTAILQ_INIT(&ns->exp_open_zones);
    QTAILQ_INIT(&ns->imp_open_zones);
    QTAILQ_INIT(&ns->closed_zones);
    QTAILQ_INIT(&ns->full_zones);

    zone = ns->zone_array;
    for (i = 0; i < ns->num_zones; i++, zone++) {
        if (start + zone_size > capacity) {
            zone_size = capacity - start;
        }
        zone->d.zt = NVME_ZONE_TYPE_SEQ_WRITE;
        nvme_zns_set_state(zone, NVME_ZONE_STATE_EMPTY);
        zone->d.za = 0;
        zone->d.zcap = ns->zone_capacity;
        zone->d.zslba = start;
        zone->d.wp = start;
        zone->w_ptr = start;
        start += zone_size;
    }

    ns->zone_size_log2 = 0;
    if (is_power_of_2(ns->zone_size)) {
        ns->zone_size_log2 = 63 - clz64(ns->zone_size);
    }
}

void nvme_zns_init(NvmeNamespace *ns)
{
    NvmeIdNsZoned *id_ns_z;
    int i;

    nvme_zns_init_state(ns);

    id_ns_z = g_malloc0(sizeof(NvmeIdNsZoned));

    /* MAR/MOR are zeroes-based, FFFFFFFFFh means no limit */
    id_ns_z->mar = cpu_to_le32(ns->params.max_active_zones - 1);
    id_ns_z->mor = cpu_to_le32(ns->params.max_open_zones - 1);
    id_ns_z->zoc = 0;
    id_ns_z->ozcs = ns->params.cross_zone_read ? 0x01 : 0x00;

    for (i = 0; i <= ns->id_ns.nlbaf; i++) {
        id_ns_z->lbafe[i].zsze = cpu_to_le64(ns->zone_size);
        id_ns_z->lbafe[i].zdes =
            ns->params.zd_extension_size >> 6; /* Units of 64B */
    }

    ns->csi = NVME_CSI_ZONED;
    ns->id_ns.nsze = cpu_to_le64(ns->num_zones * ns->zone_size);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.ncap;

    /*
     * The device uses the BDRV_BLOCK_ZERO flag to determine the "deallocated"
     * status of logical blocks. Since the spec defines that logical blocks
     * SHALL be deallocated when then zone is in the Empty or Offline states,
     * we can only support DULBE if the zone size is a multiple of the
     * calculated NPDG.
     */
    if (ns->zone_size % (ns->id_ns.npdg + 1)) {
        warn_report("the zone size (%"PRIu64" blocks) is not a multiple of "
                    "the calculated deallocation granularity (%d blocks); "
                    "DULBE support disabled",
                    ns->zone_size, ns->id_ns.npdg + 1);

        ns->id_ns.nsfeat &= ~0x4;
    }

    ns->id_ns_zoned = id_ns_z;
}

void nvme_zns_clear_zone(NvmeNamespace *ns, NvmeZone *zone)
{
    uint8_t state;

    zone->w_ptr = zone->d.wp;
    state = nvme_zns_state(zone);
    if (zone->d.wp != zone->d.zslba ||
        (zone->d.za & NVME_ZA_ZD_EXT_VALID)) {
        if (state != NVME_ZONE_STATE_CLOSED) {
            trace_pci_nvme_clear_ns_close(state, zone->d.zslba);
            nvme_zns_set_state(zone, NVME_ZONE_STATE_CLOSED);
        }
        nvme_zns_aor_inc_active(ns);
        QTAILQ_INSERT_HEAD(&ns->closed_zones, zone, entry);
    } else {
        trace_pci_nvme_clear_ns_reset(state, zone->d.zslba);
        nvme_zns_set_state(zone, NVME_ZONE_STATE_EMPTY);
    }
}

/*
 * Close all the zones that are currently open.
 */
void nvme_zns_shutdown(NvmeNamespace *ns)
{
    NvmeZone *zone, *next;

    QTAILQ_FOREACH_SAFE(zone, &ns->closed_zones, entry, next) {
        QTAILQ_REMOVE(&ns->closed_zones, zone, entry);
        nvme_zns_aor_dec_active(ns);
        nvme_zns_clear_zone(ns, zone);
    }
    QTAILQ_FOREACH_SAFE(zone, &ns->imp_open_zones, entry, next) {
        QTAILQ_REMOVE(&ns->imp_open_zones, zone, entry);
        nvme_zns_aor_dec_open(ns);
        nvme_zns_aor_dec_active(ns);
        nvme_zns_clear_zone(ns, zone);
    }
    QTAILQ_FOREACH_SAFE(zone, &ns->exp_open_zones, entry, next) {
        QTAILQ_REMOVE(&ns->exp_open_zones, zone, entry);
        nvme_zns_aor_dec_open(ns);
        nvme_zns_aor_dec_active(ns);
        nvme_zns_clear_zone(ns, zone);
    }

    assert(ns->nr_open_zones == 0);
}

void nvme_zns_cleanup(NvmeNamespace *ns)
{
        g_free(ns->id_ns_zoned);
        g_free(ns->zone_array);
        g_free(ns->zd_extensions);
}
