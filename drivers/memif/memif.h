#ifndef USCHED_MEMIF_H
#define USCHED_MEMIF_H

#include <usched.h>

int memif_export_rxring(void *rx_queue, struct rxring *rxring);
int memif_rx_interrupt_fd(void *rx_queue);
int memif_enable_rx_interrupt(void *rx_queue);
int memif_disable_rx_interrupt(void *rx_queue);

#endif
