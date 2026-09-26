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

#define LAIKADFU_DIAG_CONFIG_VERSION 1u

#define LAIKADFU_DIAG_OP_CHECKPOINT 0x43485054u /* CHPT */
#define LAIKADFU_DIAG_OP_READ32     0x52333220u /* R32  */
#define LAIKADFU_DIAG_OP_WRITE32    0x57333220u /* W32  */
#define LAIKADFU_DIAG_OP_CLOCK      0x434c4b20u /* CLK  */
#define LAIKADFU_DIAG_OP_TIMEOUT    0x54494d45u /* TIME */
#define LAIKADFU_DIAG_OP_FAIL       0x4641494cu /* FAIL */

struct laikadfu_diag_config {
	uint64_t magic;
	uint32_t mode;
	uint32_t checkpoint;
	uint32_t version;
	uint32_t failed;
	uint32_t last_op;
	uint32_t last_error;
	uint64_t counter;
	uint64_t arg0;
	uint64_t arg1;
	uint64_t arg2;
	uint64_t arg3;
};

extern volatile struct laikadfu_diag_config gLaikaDFUDiag;
void laikadfu_diag_checkpoint(uint32_t checkpoint);
void laikadfu_diag_record(uint32_t op, uint32_t error, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
void laikadfu_diag_fail_record(uint32_t code, uint64_t arg0, uint64_t arg1, uint64_t arg2);

#endif
