// Copyright (c) 0cyn All Rights Reserved
#ifndef L41KA_LAIKADFU_DIAG_H
#define L41KA_LAIKADFU_DIAG_H

#include <stdint.h>

#define LAIKADFU_DIAG_MAGIC 0x47414944414b4c34ull /* 4LkADIAG */

#define LAIKADFU_DIAG_NONE         0u
#define LAIKADFU_DIAG_ENTRY        1u
#define LAIKADFU_DIAG_PRE_COMPLEX  2u
#define LAIKADFU_DIAG_POST_COMPLEX 3u
#define LAIKADFU_DIAG_POST_DART    4u
#define LAIKADFU_DIAG_POST_RESET   5u
#define LAIKADFU_DIAG_POST_CONNECT 6u
#define LAIKADFU_DIAG_PRE_EP0      7u
#define LAIKADFU_DIAG_SETUP_SEEN   8u
#define LAIKADFU_DIAG_SET_ADDRESS  9u
#define LAIKADFU_DIAG_DESCRIPTOR   10u
#define LAIKADFU_DIAG_REBOOT_BASE 100u

struct laikadfu_diag_config {
	uint64_t magic;
	uint32_t mode;
	uint32_t checkpoint;
};

extern volatile struct laikadfu_diag_config gLaikaDFUDiag;
void laikadfu_diag_checkpoint(uint32_t checkpoint);

#endif
