#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include <rte_ethdev.h>

int rte_eth_dev_info_get(
    uint16_t port_id,
    struct rte_eth_dev_info *dev_info)
{
    int (*next)(uint16_t, struct rte_eth_dev_info *) = dlsym(RTLD_NEXT, "rte_eth_dev_info_get");
    int ret = next(port_id, dev_info);

    if (!ret && !strcmp(dev_info->driver_name, "net_ixgbe_vf")) {
        dev_info->flow_type_rss_offloads = 0;
    }

    return ret;
}
