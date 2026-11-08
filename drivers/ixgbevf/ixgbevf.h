#ifndef __IXGBEVF_H__
#define __IXGBEVF_H__

#include <stdint.h>
#include <usched.h>
#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_power.h>
#include <ixgbe_type.h>
#include <ixgbe_rxtx.h>

int ixgbevf_export_rxring(void *rx_queue, struct rxring *rxring);

#endif
