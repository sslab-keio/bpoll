#ifndef _SHMEM_H_
#define _SHMEM_H_

#include <stddef.h>

void *attach_or_create_shmem(const char *name, size_t size, bool *created);

#endif
