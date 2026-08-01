#ifndef COMBINED_STAGE2_PATCHFINDER_PF_AARCH64_H
#define COMBINED_STAGE2_PATCHFINDER_PF_AARCH64_H

#include <stdint.h>

#ifdef __cplusplus
namespace aarch64 {
#endif

#ifdef __cplusplus
enum
{
	ARM64_PACIBSP = 0xd503237f,
	ARM64_PACIASP = 0xd503233f,
	ARM64_RET = 0xd65f03c0,
	ARM64_NOP = 0xd503201f,
	ARM64_SVC_7 = 0xd40000e1,
	ARM64_DSB_SY = 0xd5033f9f,
	ARM64_DMB_SY = 0xd5033fbf,
	ARM64_ISB = 0xd5033fdf,
	ARM64_MRS_X8_SCTLR_EL1 = 0xd5381008,
	ARM64_BIC_X8_X8_1 = 0x927ff908,
	ARM64_MSR_SCTLR_EL1_X8 = 0xd5181008,
	ARM64_BR_X15 = 0xd61f01e0,
};
#else
#define ARM64_PACIBSP UINT32_C(0xd503237f)
#define ARM64_PACIASP UINT32_C(0xd503233f)
#define ARM64_RET UINT32_C(0xd65f03c0)
#define ARM64_NOP UINT32_C(0xd503201f)
#define ARM64_SVC_7 UINT32_C(0xd40000e1)
#define ARM64_DSB_SY UINT32_C(0xd5033f9f)
#define ARM64_DMB_SY UINT32_C(0xd5033fbf)
#define ARM64_ISB UINT32_C(0xd5033fdf)
#define ARM64_MRS_X8_SCTLR_EL1 UINT32_C(0xd5381008)
#define ARM64_BIC_X8_X8_1 UINT32_C(0x927ff908)
#define ARM64_MSR_SCTLR_EL1_X8 UINT32_C(0xd5181008)
#define ARM64_BR_X15 UINT32_C(0xd61f01e0)
#endif

static inline int is_b(uint32_t instruction)
{
	return (instruction & 0xfc000000u) == 0x14000000u;
}

static inline int is_bl(uint32_t instruction)
{
	return (instruction & 0xfc000000u) == 0x94000000u;
}

static inline uintptr_t branch_target(
	uintptr_t addr, uint32_t instruction)
{
	int64_t immediate = instruction & 0x03ffffffu;
	if (immediate & 0x02000000ll)
		immediate -= 0x04000000ll;
	return (uintptr_t)((int64_t)addr + immediate * 4);
}

static inline uintptr_t cbz_target(
	uintptr_t addr, uint32_t instruction)
{
	int64_t immediate = (instruction >> 5) & 0x7ffffu;
	if (immediate & 0x40000ll)
		immediate -= 0x80000ll;
	return (uintptr_t)((int64_t)addr + immediate * 4);
}

static inline uintptr_t tbz_target(
	uintptr_t addr, uint32_t instruction)
{
	int64_t immediate = (instruction >> 5) & 0x3fffu;
	if (immediate & 0x2000ll)
		immediate -= 0x4000ll;
	return (uintptr_t)((int64_t)addr + immediate * 4);
}

static inline int decode_adrp_add(
	uintptr_t image_addr,
	uintptr_t instruction_addr,
	uint32_t adrp,
	uint32_t add,
	uintptr_t *target,
	uint32_t *reg)
{
	uint32_t rd = adrp & 0x1fu;
	if (rd == 31)
		return 0;

	int64_t immediate = ((adrp >> 5) & 0x7ffffu) << 2;
	immediate |= (adrp >> 29) & 3u;
	if (immediate & 0x100000ll)
		immediate -= 0x200000ll;

	if ((adrp & 0x9f000000u) == 0x10000000u && add == ARM64_NOP)
	{
		*target = (uintptr_t)((int64_t)instruction_addr + immediate);
		*reg = rd;
		return 1;
	}
	if ((adrp & 0x9f000000u) != 0x90000000u
		|| (add & 0xffc00000u) != 0x91000000u
		|| (add & 0x1fu) != rd
		|| ((add >> 5) & 0x1fu) != rd)
		return 0;

	uintptr_t page_offset = (instruction_addr - image_addr) & ~(uintptr_t)0xfff;
	*target = (uintptr_t)((int64_t)image_addr + (int64_t)page_offset
		+ immediate * 0x1000) + ((add >> 10) & 0xfffu);
	*reg = rd;
	return 1;
}

static inline int is_mov_x(
	uint32_t instruction, uint32_t dst, uint32_t src)
{
	return instruction == (0xaa0003e0u | (src << 16) | dst);
}

static inline int is_mov_w_immediate(
	uint32_t instruction, uint32_t dst, uint32_t value)
{
	return (instruction & 0xffe0001fu) == (0x52800000u | dst)
		&& ((instruction >> 5) & 0xffffu) == value;
}

static inline uint32_t assemble_b(uintptr_t address, uintptr_t target)
{
	int64_t delta = (int64_t)target - (int64_t)address;
	return 0x14000000u | ((uint32_t)(delta / 4) & 0x03ffffffu);
}

static inline uint32_t assemble_mov_w(uint32_t reg, uint16_t value)
{
	return 0x52800000u | (uint32_t)value << 5 | reg;
}

static inline uint32_t assemble_orr_x(
	uint32_t destination, uint32_t left, uint32_t right)
{
	return 0xaa000000u | right << 16 | left << 5 | destination;
}

static inline uint32_t assemble_movz_x(
	uint32_t reg, uint16_t value, uint32_t shift)
{
	return 0xd2800000u | (shift / 16) << 21 | (uint32_t)value << 5 | reg;
}

static inline uint32_t assemble_movk_x(
	uint32_t reg, uint16_t value, uint32_t shift)
{
	return 0xf2800000u | (shift / 16) << 21 | (uint32_t)value << 5 | reg;
}

#ifdef __cplusplus
#define PF_AARCH64_CONSTEXPR constexpr
#else
#define PF_AARCH64_CONSTEXPR static inline
#endif

PF_AARCH64_CONSTEXPR int cmp(uint32_t actual, uint32_t expected, uint32_t mask)
{
	return (actual & mask) == (expected & mask);
}

#ifdef __cplusplus
enum
{
	kRdMask = 0x0000001fu,
	kRtMask = kRdMask,
	kRnMask = 0x000003e0u,
	kRt2Mask = 0x00007c00u,
	kRmMask = 0x001f0000u,
	kImm12Mask = 0x003ffc00u,
	kLoadStoreImm12Mask = kImm12Mask,
	kPairImm7Mask = 0x003f8000u,
	kBranchImm19Mask = 0x00ffffe0u,
	kTestBranchImm14Mask = 0x0007ffe0u,
	kTestBitMask = 0x8007c000u,
	kTestBranchOpMask = 0x01000000u,
	kConditionMask = 0x0000000fu,
	kSfMask = 0x80000000u,
};
#else
#define kRdMask UINT32_C(0x0000001f)
#define kRtMask kRdMask
#define kRnMask UINT32_C(0x000003e0)
#define kRt2Mask UINT32_C(0x00007c00)
#define kRmMask UINT32_C(0x001f0000)
#define kImm12Mask UINT32_C(0x003ffc00)
#define kLoadStoreImm12Mask kImm12Mask
#define kPairImm7Mask UINT32_C(0x003f8000)
#define kBranchImm19Mask UINT32_C(0x00ffffe0)
#define kTestBranchImm14Mask UINT32_C(0x0007ffe0)
#define kTestBitMask UINT32_C(0x8007c000)
#define kTestBranchOpMask UINT32_C(0x01000000)
#define kConditionMask UINT32_C(0x0000000f)
#define kSfMask UINT32_C(0x80000000)
#endif

typedef enum condition
{
	condition_Equal = 0,
	condition_NotEqual = 1,
	condition_Lower = 3,
	condition_LowerOrSame = 9,
} condition;

typedef enum shift
{
	shift_Lsl = 0,
	shift_Lsr = 1,
	shift_Asr = 2,
} shift;

PF_AARCH64_CONSTEXPR uint32_t branch_immediate(
	uintptr_t address, uintptr_t target, uint32_t mask)
{
	int64_t delta = (int64_t)target - (int64_t)address;
	return (uint32_t)(delta / 4) & mask;
}

PF_AARCH64_CONSTEXPR uint32_t b_cond(
	condition condition, uintptr_t address, uintptr_t target)
{
	return 0x54000000u | (branch_immediate(address, target, 0x7ffffu) << 5)
		| (uint32_t)condition;
}

PF_AARCH64_CONSTEXPR uint32_t cbz_w(
	uint32_t reg, uintptr_t address, uintptr_t target)
{
	return 0x34000000u | (branch_immediate(address, target, 0x7ffffu) << 5) | reg;
}

PF_AARCH64_CONSTEXPR uint32_t tbz(
	uint32_t reg, uint32_t bit, uintptr_t address, uintptr_t target)
{
	return 0x36000000u | ((bit & 0x20u) << 26) | ((bit & 0x1fu) << 19)
		| (branch_immediate(address, target, 0x3fffu) << 5) | reg;
}

PF_AARCH64_CONSTEXPR uint32_t movz_w(
	uint32_t reg, uint16_t value, uint32_t shift)
{
	return 0x52800000u | ((shift / 16u) << 21) | ((uint32_t)value << 5) | reg;
}

PF_AARCH64_CONSTEXPR uint32_t movk_w(
	uint32_t reg, uint16_t value, uint32_t shift)
{
	return 0x72800000u | ((shift / 16u) << 21) | ((uint32_t)value << 5) | reg;
}

PF_AARCH64_CONSTEXPR uint32_t movz_x(
	uint32_t reg, uint16_t value, uint32_t shift)
{
	return 0xd2800000u | ((shift / 16u) << 21) | ((uint32_t)value << 5) | reg;
}

PF_AARCH64_CONSTEXPR uint32_t movk_x(
	uint32_t reg, uint16_t value, uint32_t shift)
{
	return 0xf2800000u | ((shift / 16u) << 21) | ((uint32_t)value << 5) | reg;
}

PF_AARCH64_CONSTEXPR uint32_t orr_x(
	uint32_t destination, uint32_t left, uint32_t right)
{
	return 0xaa000000u | (right << 16) | (left << 5) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t mov_register_w(
	uint32_t destination, uint32_t source)
{
	return 0x2a0003e0u | (source << 16) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t mov_register_x(
	uint32_t destination, uint32_t source)
{
	return 0xaa0003e0u | (source << 16) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t br(uint32_t reg)
{
	return 0xd61f0000u | (reg << 5);
}

PF_AARCH64_CONSTEXPR uint32_t add_x(
	uint32_t destination, uint32_t source, uint32_t immediate)
{
	return 0x91000000u | (immediate << 10) | (source << 5) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t sub_x(
	uint32_t destination, uint32_t source, uint32_t immediate)
{
	return 0xd1000000u | (immediate << 10) | (source << 5) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t cmp_w_immediate(
	uint32_t reg, uint32_t immediate)
{
	return 0x7100001fu | (immediate << 10) | (reg << 5);
}

PF_AARCH64_CONSTEXPR uint32_t cmp_w_shifted(
	uint32_t left, uint32_t right, shift shift, uint32_t amount)
{
	return 0x6b00001fu | ((uint32_t)shift << 22) | (right << 16)
		| (amount << 10) | (left << 5);
}

PF_AARCH64_CONSTEXPR uint32_t and_immediate_w(uint32_t destination,
	uint32_t source, uint32_t rotate, uint32_t ones)
{
	return 0x12000000u | (rotate << 16) | (ones << 10)
		| (source << 5) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t orr_immediate_w(uint32_t destination,
	uint32_t source, uint32_t rotate, uint32_t ones)
{
	return 0x32000000u | (rotate << 16) | (ones << 10)
		| (source << 5) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t and_immediate_x(uint32_t destination,
	uint32_t source, uint32_t n, uint32_t rotate, uint32_t ones)
{
	return 0x92000000u | (n << 22) | (rotate << 16) | (ones << 10)
		| (source << 5) | destination;
}

PF_AARCH64_CONSTEXPR uint32_t ldr_b(
	uint32_t target, uint32_t base, uint32_t byteOffset)
{
	return 0x39400000u | (byteOffset << 10) | (base << 5) | target;
}

PF_AARCH64_CONSTEXPR uint32_t ldr_w(
	uint32_t target, uint32_t base, uint32_t byteOffset)
{
	return 0xb9400000u | ((byteOffset / 4u) << 10) | (base << 5) | target;
}

PF_AARCH64_CONSTEXPR uint32_t ldr_x(
	uint32_t target, uint32_t base, uint32_t byteOffset)
{
	return 0xf9400000u | ((byteOffset / 8u) << 10) | (base << 5) | target;
}

PF_AARCH64_CONSTEXPR uint32_t ldr_literal_x(
	uint32_t target, uintptr_t address, uintptr_t literal)
{
	return 0x58000000u | (branch_immediate(address, literal, 0x7ffffu) << 5) | target;
}

PF_AARCH64_CONSTEXPR uint32_t str_w(
	uint32_t target, uint32_t base, uint32_t byteOffset)
{
	return 0xb9000000u | ((byteOffset / 4u) << 10) | (base << 5) | target;
}

PF_AARCH64_CONSTEXPR uint32_t stp_x(
	uint32_t first, uint32_t second, uint32_t base, int32_t byteOffset)
{
	uint32_t immediate = (uint32_t)(byteOffset / 8) & 0x7fu;
	return 0xa9000000u | (immediate << 15) | (second << 10) | (base << 5) | first;
}

PF_AARCH64_CONSTEXPR uint32_t stp_w(
	uint32_t first, uint32_t second, uint32_t base, int32_t byteOffset)
{
	uint32_t immediate = (uint32_t)(byteOffset / 4) & 0x7fu;
	return 0x29000000u | (immediate << 15) | (second << 10) | (base << 5) | first;
}

#ifdef __cplusplus
static_assert(movz_x(8, 0x10, 0) == 0xd2800208u);
static_assert(movk_x(8, 0x3d2d, 16) == 0xf2a7a5a8u);
static_assert(ldr_w(8, 8, 0) == 0xb9400108u);
static_assert(ldr_x(2, 31, 0x28) == 0xf94017e2u);
static_assert(ldr_literal_x(16, 0x30, 0x38) == 0x58000050u);
static_assert(br(18) == 0xd61f0240u);
static_assert(stp_x(29, 30, 31, -0xa0) == 0xa9367bfdu);
static_assert(stp_w(23, 25, 31, 0x50) == 0x290a67f7u);
static_assert(tbz(8, 0, 0x1000, 0x1010) == 0x36000088u);
static_assert(cmp(0xd2800393u, movz_x(0, 0x1c, 0), ~kRdMask));
static_assert(!cmp(0xd28003b3u, movz_x(0, 0x1c, 0), ~kRdMask));
#endif

#undef PF_AARCH64_CONSTEXPR

#ifdef __cplusplus
} // namespace aarch64
#endif // __cplusplus
#endif
