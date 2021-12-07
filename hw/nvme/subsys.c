/*
 * QEMU NVM Express Subsystem: nvme-subsys
 *
 * Copyright (c) 2021 Minwoo Im <minwoo.im.dev@gmail.com>
 *
 * This code is licensed under the GNU GPL v2.  Refer COPYING.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"

#include "nvme.h"

void nvme_subsys_identify_set_common(NvmeIdNs *id_ns)
{
    static const NvmeLBAF default_lba_formats[] = {
        { .ds =  9           },
        { .ds =  9, .ms =  8 },
        { .ds =  9, .ms = 16 },
        { .ds =  9, .ms = 64 },
        { .ds = 12           },
        { .ds = 12, .ms =  8 },
        { .ds = 12, .ms = 16 },
        { .ds = 12, .ms = 64 },
    };

    id_ns->nlbaf = ARRAY_SIZE(default_lba_formats) - 1;

    id_ns->dlfeat = NVME_ID_NS_DLFEAT_ZEROES;
    id_ns->mc = NVME_ID_NS_MC_EXTENDED | NVME_ID_NS_MC_SEPARATE;
    id_ns->dpc = NVME_ID_NS_DPC_MASK;

    memcpy(&id_ns->lbaf, &default_lba_formats, sizeof(default_lba_formats));
}

int nvme_subsys_register_ctrl(NvmeCtrl *n, Error **errp)
{
    NvmeSubsystem *subsys = n->subsys;
    int cntlid, nsid;

    for (cntlid = 0; cntlid < ARRAY_SIZE(subsys->ctrls); cntlid++) {
        if (!subsys->ctrls[cntlid]) {
            break;
        }
    }

    if (cntlid == ARRAY_SIZE(subsys->ctrls)) {
        error_setg(errp, "no more free controller id");
        return -1;
    }

    subsys->ctrls[cntlid] = n;

    for (nsid = 1; nsid < ARRAY_SIZE(subsys->namespaces); nsid++) {
        NvmeNamespace *ns = subsys->namespaces[nsid];
        if (ns && (ns->flags & NVME_NS_SHARED) &&
            !(ns->flags & NVME_NS_DETACHED)) {
            nvme_attach_ns(n, ns);
        }
    }

    return cntlid;
}

void nvme_subsys_unregister_ctrl(NvmeSubsystem *subsys, NvmeCtrl *n)
{
    subsys->ctrls[n->cntlid] = NULL;
    n->cntlid = -1;
}

static void nvme_subsys_setup(NvmeSubsystem *subsys)
{
    const char *nqn = subsys->params.nqn ?
        subsys->params.nqn : subsys->parent_obj.id;

    snprintf((char *)subsys->subnqn, sizeof(subsys->subnqn),
             "nqn.2019-08.org.qemu:%s", nqn);
}

static void nvme_subsys_realize(DeviceState *dev, Error **errp)
{
    NvmeSubsystem *subsys = NVME_SUBSYS(dev);

    qbus_init(&subsys->bus, sizeof(NvmeBus), TYPE_NVME_BUS, dev, dev->id);

    nvme_subsys_setup(subsys);
}

static Property nvme_subsystem_props[] = {
    DEFINE_PROP_STRING("nqn", NvmeSubsystem, params.nqn),
    DEFINE_PROP_END_OF_LIST(),
};

static void nvme_subsys_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    dc->realize = nvme_subsys_realize;
    dc->desc = "Virtual NVMe subsystem";
    dc->hotpluggable = false;

    device_class_set_props(dc, nvme_subsystem_props);
}

static const TypeInfo nvme_subsys_info = {
    .name = TYPE_NVME_SUBSYS,
    .parent = TYPE_DEVICE,
    .class_init = nvme_subsys_class_init,
    .instance_size = sizeof(NvmeSubsystem),
};

static void nvme_subsys_register_types(void)
{
    type_register_static(&nvme_subsys_info);
}

type_init(nvme_subsys_register_types)
