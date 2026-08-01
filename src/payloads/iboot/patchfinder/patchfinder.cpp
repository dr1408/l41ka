// Copyright (c) 0cyn All Rights Reserved



#include <stdint.h>

#include <pf_aarch64.h>

namespace {

	using namespace aarch64;

	constexpr uintptr_t kCodeSize = 0x100000;
	constexpr uintptr_t kImageSize = 0x200000;
#ifndef COMBINED_STAGE2_HOST_TEST
	constexpr uintptr_t kPageSize = 0x4000;
#endif
	constexpr uint8_t kAutobootOnce[] = "auto-boot-once";
	constexpr uint8_t kAutoboot[] = "auto-boot";
	constexpr uint8_t kDisableBootWdt[] = "disable-boot-wdt";
	constexpr uint8_t kBootVersion[] = "Boot-";
	constexpr uint8_t kStage2Loader[] = " (l41ka stage2 loader)";
	constexpr uint8_t kRecfg[] = "reconfig-breakpoints";
	constexpr uint8_t kDebugSoc[] = "debug-soc";

#ifndef COMBINED_STAGE2_HOST_TEST
	extern "C" const uint8_t laikadfu[];
	extern "C" const uint8_t laikadfu_end[];
	extern "C" const uint8_t start[];
#else
	constexpr uint8_t laikadfu[] = {0x00, 0x00, 0x00, 0x14};
	extern "C" void combined_stage2_test_set_patcher(const char* name);
	extern "C" void combined_stage2_test_record_patch(uintptr_t addr, const uint8_t* bytes, uintptr_t count);
#endif

	bool contains(uintptr_t base, uintptr_t addr, uintptr_t size, uintptr_t limit = kImageSize)
	{
		return addr >= base && addr - base <= limit && size <= limit - (addr - base);
	}

	void patch32(uintptr_t address, uint32_t value)
	{
		*reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(address)) = value;
#ifdef COMBINED_STAGE2_HOST_TEST
		combined_stage2_test_record_patch(
			address, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address)), 4);
#endif
	}

	void patchBytes(uintptr_t address, const uint8_t* bytes, uintptr_t size)
	{
		auto* destination = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(address));
		for (uintptr_t i = 0; i < size; ++i)
			destination[i] = bytes[i];
#ifdef COMBINED_STAGE2_HOST_TEST
		combined_stage2_test_record_patch(
			address, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address)), size);
#endif
	}

	class Patcher
	{
	public:
		Patcher(uintptr_t base, const char* name) : base_(base), name_(name) {}
		virtual bool Match(const uint32_t* cursor, bool& found) = 0;
		virtual const char* name() const { return name_; }
		virtual bool required() const { return true; }
		virtual void Patch() = 0;

	protected:
		bool lastCursor(const uint32_t* cursor) const
		{
			return reinterpret_cast<uintptr_t>(cursor) == base_ + kCodeSize - 4;
		}
		const uintptr_t base_;

	private:
		const char* name_;
	};

	bool bytesMatch(uintptr_t base, uintptr_t address, const uint8_t* expected, uintptr_t count)
	{
		if (!contains(base, address, count))
			return false;
		const auto* cursor = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(address));
		for (uintptr_t i = 0; i < count; ++i)
			if (cursor[i] != expected[i])
				return false;
		return true;
	}

	uintptr_t functionStart(uintptr_t base, uintptr_t address)
	{
		uintptr_t limit = address > base + 0x80 ? address - 0x80 : base;
		const auto* cursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(address));
		for (uintptr_t current = address; current >= limit + 4; current -= 4)
		{
			uint32_t instruction = cursor[-1];
			if (instruction == ARM64_PACIBSP || instruction == ARM64_PACIASP)
				return current - 4;
			--cursor;
		}
		return 0;
	}

	bool branchChainEndsInDmb(uintptr_t base, uintptr_t address)
	{
		for (uint32_t depth = 0; depth < 32; ++depth)
		{
			if (!contains(base, address, 4, kCodeSize))
				return false;
			uint32_t instruction = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(address))[0];
			if (instruction == ARM64_DMB_SY)
				return true;
			if (!is_b(instruction) && !is_bl(instruction))
				return false;
			address = branch_target(address, instruction);
		}
		return false;
	}

	class NVRAMPatcher : public Patcher
	{
	public:
		NVRAMPatcher(uintptr_t base, const char* patcherName, const uint8_t* name, uint32_t nameSize,
			uint32_t defaultValue) :
			Patcher(base, patcherName), name_(name), nameSize_(nameSize), defaultValue_(defaultValue)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			uintptr_t target = 0;
			uint32_t reg = 0;
			if (contains(base_, addr, 8, kCodeSize)
				&& decode_adrp_add(base_, addr, cursor[0], cursor[1], &target, &reg)
				&& bytesMatch(base_, target, name_, nameSize_))
			{
				bool haveX0 = reg == 0;
				bool haveX1 = false;
				bool haveDefault = false;
				uintptr_t result = 0;
				uintptr_t last = addr + 64;
				if (last > base_ + kCodeSize - 8)
					last = base_ + kCodeSize - 8;
				auto* scanCursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(addr + 8));
				for (uintptr_t current = addr + 8; current <= last; current += 4, ++scanCursor)
				{
					uint32_t instruction = scanCursor[0];
					if (is_mov_x(instruction, 0, reg))
						haveX0 = true;
					else if (is_mov_x(instruction, 1, reg))
						haveX1 = true;
					else if (is_mov_w_immediate(instruction, 4, defaultValue_))
						haveDefault = true;
					else if (is_bl(instruction))
					{
						if (haveX0 && haveX1 && haveDefault)
						{
							uint32_t resultInstruction = scanCursor[1];
							uint32_t resultReg = resultInstruction & 0x1fu;
							if (is_mov_x(resultInstruction, resultReg, 0) && resultReg >= 19 && resultReg <= 28)
							{
								result = current + 4;
								break;
							}
						}
						haveX0 = haveX1 = haveDefault = false;
					}
				}
				if (result != 0)
				{
					if (resultCount_ == 0)
						result_ = result;
					++resultCount_;
				}
			}
			if (!lastCursor(cursor))
				return false;
			found = address() != 0;
			return true;
		}

		void Patch() override
		{
			uintptr_t addr = address();
			patch32(addr,
				assemble_mov_w(reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(addr))[0] & 0x1fu, 1));
		}

		uintptr_t address() const { return resultCount_ == 1 ? result_ : 0; }

	private:
		const uint8_t* name_;
		uint32_t nameSize_;
		uint32_t defaultValue_;
		uintptr_t result_ = 0;
		uint32_t resultCount_ = 0;
	};

	class AutobootPatcher final : public NVRAMPatcher
	{
	public:
		AutobootPatcher(uintptr_t base, const NVRAMPatcher& once) :
			NVRAMPatcher(base, "AutobootPatcher", kAutoboot, sizeof(kAutoboot), 0), once_(once)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			if (!NVRAMPatcher::Match(cursor, found))
				return false;
			if (once_.address() == 0 || address() == 0)
				return true;
			uintptr_t mode = once_.address() + 0x34;
			uintptr_t delay = address() + 0xb0;
			if (!contains(base_, mode, 4, kCodeSize) || !contains(base_, delay, 4, kCodeSize))
				return true;
			const auto* onceCursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(mode));
			const auto* autobootCursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(address()));
			uint32_t branch = onceCursor[0];
			constexpr uint32_t wildcardTestBranch = kTestBitMask | kTestBranchImm14Mask | kTestBranchOpMask;
			extended_ = cmp(branch, tbz(0, 0, 0, 0), ~wildcardTestBranch)
				&& is_bl(autobootCursor[1])
				&& autobootCursor[2] == mov_register_x(23, 0)
				&& autobootCursor[44] == mov_register_x(28, 0);
			return true;
		}
		bool extended() const { return extended_; }

	private:
		const NVRAMPatcher& once_;
		bool extended_ = false;
	};

	class ExtendedAutobootPatcher : public Patcher
	{
	public:
		ExtendedAutobootPatcher(uintptr_t base, const char* name, const NVRAMPatcher& once,
			const AutobootPatcher& autoboot) :
			Patcher(base, name), once_(once), autoboot_(autoboot)
		{}

		bool required() const override { return autoboot_.extended(); }

	protected:
		bool complete(const uint32_t* cursor, bool& found)
		{
			if (!lastCursor(cursor))
				return false;
			if (!autoboot_.extended())
			{
				found = false;
				return true;
			}
			if (!resolving_)
			{
				resolving_ = true;
				return false;
			}
			found = matched_;
			return true;
		}

		const NVRAMPatcher& once_;
		const AutobootPatcher& autoboot_;
		bool matched_ = false;
		bool resolving_ = false;
	};

	class ForceLocalAutobootPatcher final : public ExtendedAutobootPatcher
	{
	public:
		ForceLocalAutobootPatcher(uintptr_t base, const NVRAMPatcher& once, const AutobootPatcher& autoboot) :
			ExtendedAutobootPatcher(base, "ForceLocalAutobootPatcher", once, autoboot)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (resolving_ && autoboot_.extended() && addr == once_.address() + 0x34
				&& cmp(cursor[0], tbz(0, 0, 0, 0), ~(kTestBitMask | kTestBranchImm14Mask | kTestBranchOpMask)))
				matched_ = true;
			return complete(cursor, found);
		}

		void Patch() override
		{
			if (!autoboot_.extended())
				return;
			uintptr_t addr = once_.address() + 0x34;
			patch32(addr,
				assemble_b(addr,
					tbz_target(addr, reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(addr))[0])));
		}
	};

	class IgnoreBootCommandPatcher final : public ExtendedAutobootPatcher
	{
	public:
		IgnoreBootCommandPatcher(uintptr_t base, const NVRAMPatcher& once, const AutobootPatcher& autoboot) :
			ExtendedAutobootPatcher(base, "IgnoreBootCommandPatcher", once, autoboot)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (resolving_ && autoboot_.extended() && addr == autoboot_.address() + 8
				&& cursor[0] == mov_register_x(23, 0))
				matched_ = true;
			return complete(cursor, found);
		}

		void Patch() override
		{
			if (autoboot_.extended())
				patch32(autoboot_.address() + 8, assemble_orr_x(23, 31, 31));
		}
	};

	class ClearBootdelayPatcher final : public ExtendedAutobootPatcher
	{
	public:
		ClearBootdelayPatcher(uintptr_t base, const NVRAMPatcher& once, const AutobootPatcher& autoboot) :
			ExtendedAutobootPatcher(base, "ClearBootdelayPatcher", once, autoboot)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (resolving_ && autoboot_.extended() && addr == autoboot_.address() + 0xb0
				&& cursor[0] == mov_register_x(28, 0))
				matched_ = true;
			return complete(cursor, found);
		}

		void Patch() override
		{
			if (autoboot_.extended())
				patch32(autoboot_.address() + 0xb0, assemble_orr_x(28, 31, 31));
		}
	};

	class WatchdogCallsPatcher final : public Patcher
	{
	public:
		explicit WatchdogCallsPatcher(uintptr_t base) : Patcher(base, "WatchdogCallsPatcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (!resolving_)
				discover(cursor, addr);
			else if (is_bl(cursor[0]))
			{
				uintptr_t target = branch_target(addr, cursor[0]);
				if (target == wrapper_)
				{
					if (armCount_ == 0)
						arm_ = addr;
					++armCount_;
				}
				if (target == reloadTarget_ && !(addr >= wrapper_ && addr < xref_ + 0x80))
				{
					if (reloadCount_ == 0)
						reload_ = addr;
					++reloadCount_;
				}
			}
			if (!lastCursor(cursor))
				return false;
			if (!resolving_)
			{
				resolving_ = true;
				return false;
			}
			found = wrapper_ != 0 && reloadTarget_ != 0 && armCount_ == 1 && reloadCount_ == 1;
			return true;
		}

		void Patch() override
		{
			patch32(arm_, ARM64_NOP);
			patch32(reload_, ARM64_NOP);
		}

	private:
		bool discover(const uint32_t* cursor, uintptr_t addr)
		{
			uintptr_t target = 0;
			uint32_t reg = 0;
			if (xref_ != 0
				|| !contains(base_, addr, 8, kCodeSize)
				|| !decode_adrp_add(base_, addr, cursor[0], cursor[1], &target, &reg)
				|| !bytesMatch(base_, target, kDisableBootWdt, sizeof(kDisableBootWdt)))
				return false;
			xref_ = addr;
			wrapper_ = functionStart(base_, addr);
			uintptr_t limit = addr + 0x80;
			if (limit > base_ + kCodeSize)
				limit = base_ + kCodeSize;
			cursor += 2;
			for (uintptr_t current = addr + 8; current < limit; current += 4, ++cursor)
			{
				uint32_t branch = cursor[0];
				if ((!is_b(branch) && !is_bl(branch)))
					continue;
				uintptr_t candidate = branch_target(current, branch);
				if (looksLikeReload(candidate))
				{
					reloadTarget_ = candidate;
					break;
				}
			}
			return true;
		}

		bool looksLikeReload(uintptr_t addr) const
		{
			if (!contains(base_, addr, 48, kCodeSize))
				return false;
			const auto* cursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(addr));
			uint32_t baseReg = cursor[0] & 0x1fu;
			return cmp(cursor[0], movz_x(0, 0x1c, 0), ~kRdMask)
				&& cmp(cursor[1], movk_x(0, 0x3d2b, 16), ~kRdMask)
				&& cmp(cursor[2], movk_x(0, 2, 32), ~kRdMask)
				&& (cursor[1] & 0x1fu) == baseReg && (cursor[2] & 0x1fu) == baseReg && cursor[11] == ARM64_RET;
		}

		uintptr_t xref_ = 0;
		uintptr_t wrapper_ = 0;
		uintptr_t reloadTarget_ = 0;
		uintptr_t arm_ = 0;
		uintptr_t reload_ = 0;
		uint32_t armCount_ = 0;
		uint32_t reloadCount_ = 0;
		bool resolving_ = false;
	};

	class Stage2Patcher final : public Patcher
	{
	public:
		explicit Stage2Patcher(uintptr_t base) : Patcher(base, "Stage2Patcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			uintptr_t target = 0;
			uint32_t reg = 0;
			if (contains(base_, addr, 16, kCodeSize)
				&& decode_adrp_add(base_, addr, cursor[0], cursor[1], &target, &reg)
				&& contains(base_, target, 32))
			{
				const auto* name = reinterpret_cast<const uint8_t*>(target);
				bool valid = name[0] == static_cast<uint8_t>('i') || name[0] == static_cast<uint8_t>('m');
				uintptr_t i = 0;
				for (; valid && i < sizeof(kBootVersion) - 1 && name[i + 1] == kBootVersion[i]; ++i)
				{
				}
				valid = valid && i == sizeof(kBootVersion) - 1;
				++i;
				for (; valid && i < 32; ++i)
				{
					uint8_t value = name[i];
					if (value == 0)
						break;
					if ((value < static_cast<uint8_t>('0') || value > static_cast<uint8_t>('9'))
						&& value != static_cast<uint8_t>('.'))
						break;
				}
				valid = valid && i < 32 && name[i] == 0 && contains(base_, target + i, sizeof(kStage2Loader));
				for (uintptr_t available = 0; valid && available < sizeof(kStage2Loader); ++available)
					valid = name[i + available] == 0;

				uint32_t stp = cursor[2];
				uint32_t add = cursor[3];
				if (valid && cmp(stp, stp_x(reg, reg, 0, 0), ~(kPairImm7Mask | kRnMask))
					&& add == add_x(reg, reg, 0x100) && (address_ == 0 || target + i < address_))
					address_ = target + i;
			}
			if (!lastCursor(cursor))
				return false;
			found = address_ != 0;
			return true;
		}

		void Patch() override { patchBytes(address_, kStage2Loader, sizeof(kStage2Loader)); }

	private:
		uintptr_t address_ = 0;
	};

	class ReconfigPatcher final : public Patcher
	{
	public:
		explicit ReconfigPatcher(uintptr_t base) : Patcher(base, "ReconfigPatcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			if (foundCount_ == 0)
			{
				uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
				uintptr_t target = 0;
				uint32_t reg = 0;
				if (contains(base_, addr, 8, kCodeSize)
					&& decode_adrp_add(base_, addr, cursor[0], cursor[1], &target, &reg)
					&& bytesMatch(base_, target, kRecfg, sizeof(kRecfg)))
				{
					found_ = functionStart(base_, addr);
					foundCount_ = found_ != 0 ? 1u : 0u;
				}
			}
			if (!lastCursor(cursor))
				return false;
			found = foundCount_ == 1;
			return true;
		}

		void Patch() override { patch32(found_, ARM64_RET); }

	private:
		uintptr_t found_ = 0;
		uint32_t foundCount_ = 0;
	};

	class AESPatcher final : public Patcher
	{
	public:
		explicit AESPatcher(uintptr_t base) : Patcher(base, "AESPatcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (contains(base_, addr, 20, kCodeSize) && cursor[0] == movz_w(0, 4, 16) && is_bl(cursor[1])
				&& cursor[2] == mov_register_x(19, 0) && cursor[3] == movz_w(0, 8, 16) && is_bl(cursor[4]))
			{
				uintptr_t limit = addr + 0x80;
				if (limit > base_ + kCodeSize)
					limit = base_ + kCodeSize;
				uintptr_t target = 0;
				const uint32_t* scanCursor = cursor + 5;
				for (uintptr_t current = addr + 20; current < limit; current += 4, ++scanCursor)
				{
					uint32_t branch = scanCursor[0];
					if (is_b(branch) || is_bl(branch))
					{
						uintptr_t candidate = branch_target(current, branch);
						if (contains(base_, candidate, 1, kCodeSize))
							target = candidate;
						break;
					}
				}
				if (target != 0)
				{
					if (foundCount_ == 0)
						found_ = target;
					++foundCount_;
				}
			}
			if (!lastCursor(cursor))
				return false;
			found = foundCount_ == 1;
			return true;
		}

		void Patch() override { patch32(found_, ARM64_RET); }

	private:
		uintptr_t found_ = 0;
		uint32_t foundCount_ = 0;
	};

	class RVBARPatcher final : public Patcher
	{
	public:
		explicit RVBARPatcher(uintptr_t base) : Patcher(base, "RVBARPatcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (contains(base_, addr, 12, kCodeSize))
			{
				uint32_t instruction = cursor[0];
				uint32_t mov = cursor[1];
				uint32_t orr = cursor[2];
				if (cmp(instruction, and_immediate_x(0, 0, 1, 0x35, 0x34), ~kRdMask)
					&& cmp(mov, mov_register_w(0, 1), ~kRdMask)
					&& cmp(orr, orr_x(0, instruction & kRdMask, mov & kRdMask), ~kRdMask))
				{
					if (foundCount_ == 0)
						found_ = addr + 8;
					++foundCount_;
				}
			}
			if (!lastCursor(cursor))
				return false;
			found = foundCount_ == 1;
			return true;
		}

		void Patch() override
		{
			uintptr_t addr = found_;
			uint32_t instruction = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(addr))[0];
			patch32(addr, assemble_orr_x(instruction & 0x1fu, (instruction >> 5) & 0x1fu, 31));
		}
	private:
		uintptr_t found_ = 0;
		uint32_t foundCount_ = 0;
	};

	class FusePatternPatcher : public Patcher
	{
	public:
		FusePatternPatcher(uintptr_t base, const char* name) : Patcher(base, name) {}

	protected:
		bool matchIos18(
			const uint32_t* cursor, uintptr_t addr, uintptr_t& found, uint32_t& count, uintptr_t& destination)
		{
			if (!contains(base_, addr, 44, kCodeSize))
				return false;
			uint32_t cbz = cursor[0];
			uint32_t tbz = cursor[5];
			if (!cmp(cbz, cbz_w(0, 0, 0), ~kBranchImm19Mask)
				|| cursor[1] != movz_x(8, 0x10, 0)
				|| cursor[2] != movk_x(8, 0x3d2d, 16)
				|| cursor[3] != movk_x(8, 2, 32) || cursor[4] != ldr_w(8, 8, 0)
				|| !cmp(tbz, aarch64::tbz(8, 0, 0, 0), ~kTestBranchImm14Mask)
				|| cbz_target(addr, cbz) != tbz_target(addr + 20, tbz) || !is_bl(cursor[6])
				|| !cmp(cursor[7], aarch64::tbz(0, 8, 0, 0), ~kTestBranchImm14Mask)
				|| !is_bl(cursor[8])
				|| !cmp(cursor[9], aarch64::tbz(0, 8, 0, 0), ~kTestBranchImm14Mask)
				|| !is_bl(cursor[10]))
				return false;
			if (count == 0)
				found = addr;
			++count;
			destination = addr + 40;
			return true;
		}

		bool matchNewer(
			const uint32_t* cursor, uintptr_t addr, uintptr_t& found, uint32_t& count, uintptr_t& destination)
		{
			if (!contains(base_, addr, 20, kCodeSize)
				|| cursor[0] != movk_x(19, 2, 32) || cursor[1] != ldr_w(8, 19, 0)
				|| cursor[2] != orr_immediate_w(8, 8, 1, 0) || cursor[3] != str_w(8, 19, 0))
				return false;
			uint32_t call = cursor[4];
			if (!is_bl(call) || !branchChainEndsInDmb(base_, branch_target(addr + 16, call)))
				return false;
			for (uintptr_t current = addr; current >= base_ + 4; current -= 4)
			{
				uintptr_t candidate = current - 4;
				uint32_t branch = cursor[-1];
				if (cmp(branch, cbz_w(0, 0, 0), ~(kSfMask | kBranchImm19Mask | kRtMask)))
				{
					if (count == 0)
						found = candidate;
					++count;
					destination = cbz_target(candidate, branch);
					return true;
				}
				if (branch == ARM64_PACIBSP || branch == ARM64_PACIASP)
					return false;
				--cursor;
			}
			return false;
		}
	};

	class FuseStayPatcher final : public FusePatternPatcher
	{
	public:
		explicit FuseStayPatcher(uintptr_t base) : FusePatternPatcher(base, "FuseStayPatcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			matchIos18(cursor, addr, ios18_, ios18Count_, ios18Destination_);
			matchNewer(cursor, addr, newer_, newerCount_, newerDestination_);
			if (!lastCursor(cursor))
				return false;
			found = ios18Count_ == 1 || newerCount_ == 1;
			return true;
		}

		void Patch() override
		{
			uintptr_t addr = ios18Count_ == 1 ? ios18_ : 0;
			uintptr_t destination = ios18Destination_;
			if (addr == 0)
			{
				addr = newerCount_ == 1 ? newer_ : 0;
				destination = newerDestination_;
			}
			patch32(addr, assemble_b(addr, destination));
		}
		uintptr_t address() const { return ios18Count_ == 1 ? ios18_ : (newerCount_ == 1 ? newer_ : 0); }

	private:
		uintptr_t ios18_ = 0;
		uintptr_t newer_ = 0;
		uint32_t ios18Count_ = 0;
		uint32_t newerCount_ = 0;
		uintptr_t ios18Destination_ = 0;
		uintptr_t newerDestination_ = 0;
	};

	class FuseLockPatcher final : public FusePatternPatcher
	{
	public:
		FuseLockPatcher(uintptr_t base, const FuseStayPatcher& stay) :
			FusePatternPatcher(base, "FuseLockPatcher"), stay_(stay)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			matchNewer(cursor, reinterpret_cast<uintptr_t>(cursor), found_, foundCount_, destination_);
			if (!lastCursor(cursor))
				return false;
			found = foundCount_ == 1;
			return true;
		}

		void Patch() override
		{
			if (!needsPatch())
				return;
			patch32(found_, assemble_b(found_, destination_));
		}
		bool needsPatch() const
		{
			uintptr_t found = foundCount_ == 1 ? found_ : 0;
			return found != 0 && found != stay_.address() ? 1u : 0u;
		}
	private:
		const FuseStayPatcher& stay_;
		uintptr_t found_ = 0;
		uint32_t foundCount_ = 0;
		uintptr_t destination_ = 0;
	};

	class FuseDebugPatcher final : public Patcher
	{
	public:
		explicit FuseDebugPatcher(uintptr_t base) : Patcher(base, "FuseDebugPatcher") {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			matchStringReference(cursor, addr);
			if (contains(base_, addr, 12, kCodeSize) && is_bl(cursor[0])
				&& cursor[1] == cmp_w_immediate(0, 0x10)
				&& cmp(cursor[2], b_cond(condition_LowerOrSame, 0, 0), ~kBranchImm19Mask))
			{
				if (fallbackCount_ == 0)
					fallback_ = addr;
				++fallbackCount_;
			}
			if (!lastCursor(cursor))
				return false;
			uintptr_t result = xrefCount_ != 0 ? (xrefCount_ == 1 ? xref_ : 0) : (fallbackCount_ == 1 ? fallback_ : 0);
			found = result != 0;
			return true;
		}

		void Patch() override
		{
			uintptr_t addr = xrefCount_ != 0 ? (xrefCount_ == 1 ? xref_ : 0) : (fallbackCount_ == 1 ? fallback_ : 0);
			patch32(addr, assemble_mov_w(0, 0x101));
		}
	private:
		bool matchStringReference(const uint32_t* cursor, uintptr_t addr)
		{
			uintptr_t target = 0;
			uint32_t reg = 0;
			if (!contains(base_, addr, 8, kCodeSize)
				|| !decode_adrp_add(base_, addr, cursor[0], cursor[1], &target, &reg)
				|| !bytesMatch(base_, target, kDebugSoc, sizeof(kDebugSoc)))
				return false;
			for (uintptr_t current = addr; current >= base_ + 4; current -= 4)
			{
				uintptr_t candidate = current - 4;
				if (!contains(base_, candidate, 16, kCodeSize))
				{
					--cursor;
					continue;
				}
				uint32_t instruction = cursor[-1];
				if (is_bl(instruction)
					&& cursor[0] == and_immediate_w(8, 0, 0, 15)
					&& cursor[1] == cmp_w_immediate(8, 0x100)
					&& cmp(cursor[2], b_cond(condition_Lower, 0, 0), ~kBranchImm19Mask))
				{
					if (candidate != xref_)
					{
						if (xrefCount_ == 0)
							xref_ = candidate;
						++xrefCount_;
					}
					break;
				}
				--cursor;
			}
			return true;
		}

		uintptr_t xref_ = 0;
		uintptr_t fallback_ = 0;
		uint32_t xrefCount_ = 0;
		uint32_t fallbackCount_ = 0;
	};

	class ManifestHardwarePatcher final : public Patcher
	{
	public:
		ManifestHardwarePatcher(uintptr_t base, const AutobootPatcher& autoboot) :
			Patcher(base, "ManifestHardwarePatcher"), autoboot_(autoboot)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (addr >= base_ + 4 && contains(base_, addr, 36, kCodeSize))
			{
				uint32_t load = cursor[0];
				uint32_t compareInstruction = cursor[1];
				uint32_t skip = cursor[2];
				uint32_t fuseTbz = cursor[4];
				uint32_t revisionMask = cursor[5];
				uint32_t expectedLoad = cursor[6];
				uintptr_t target = cbz_target(addr + 8, skip);
				uint32_t shiftedCompare =
					cmp_w_shifted(expectedLoad & kRtMask, revisionMask & kRdMask, shift_Lsr, 8);
				if (cmp(cursor[-1], b_cond(condition_NotEqual, 0, 0), ~kBranchImm19Mask)
					&& cmp(load, ldr_b(0, 31, 0), ~(kRtMask | kLoadStoreImm12Mask))
					&& cmp(compareInstruction, cmp_w_immediate(0, 1), ~kRnMask)
					&& ((compareInstruction >> 5) & 0x1fu) == (load & 0x1fu)
					&& cmp(skip, b_cond(condition_NotEqual, 0, 0), ~kBranchImm19Mask) && is_bl(cursor[3])
					&& cmp(fuseTbz, tbz(0, 0, 0, 0), ~kTestBranchImm14Mask)
					&& cmp(revisionMask, and_immediate_w(0, 0, 24, 7), ~kRdMask)
					&& cmp(expectedLoad, ldr_b(0, 31, 0), ~(kRtMask | kLoadStoreImm12Mask))
					&& cursor[7] == shiftedCompare
					&& cmp(cursor[8], b_cond(condition_NotEqual, 0, 0), ~kBranchImm19Mask)
					&& target > addr + 32 && target <= addr + 0x40)
				{
					if (foundCount_ == 0)
						found_ = addr + 8;
					++foundCount_;
					target_ = target;
				}
			}
			if (!lastCursor(cursor))
				return false;
			found = foundCount_ == 1;
			return true;
		}

		void Patch() override
		{
			if (!autoboot_.extended())
				return;
			uintptr_t addr = found_;
			patch32(addr - 12, ARM64_NOP);
			patch32(addr, assemble_b(addr, target_));
		}
		bool required() const override { return autoboot_.extended(); }

	private:
		const AutobootPatcher& autoboot_;
		uintptr_t found_ = 0;
		uint32_t foundCount_ = 0;
		uintptr_t target_ = 0;
	};

	class ManifestDigestPatcher final : public Patcher
	{
	public:
		ManifestDigestPatcher(uintptr_t base, const AutobootPatcher& autoboot) :
			Patcher(base, "ManifestDigestPatcher"), autoboot_(autoboot)
		{}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			if (contains(base_, addr, 28, kCodeSize))
			{
				uint32_t branch = cursor[5];
				if (cursor[0] == movz_w(1, 0x5354, 0)
					&& cursor[1] == movk_w(1, 0x4447, 16)
					&& (cursor[2] == mov_register_x(2, 19) || cursor[2] == ldr_x(2, 31, 0x28))
					&& is_bl(cursor[3])
					&& cursor[4] == mov_register_x(20, 0)
					&& cmp(branch, cbz_w(0, 0, 0), ~kBranchImm19Mask) && is_bl(cursor[6]))
				{
					uintptr_t target = cbz_target(addr + 20, branch);
					if (target > addr + 24 && target < base_ + kCodeSize)
					{
						if (foundCount_ == 0)
							found_ = addr + 20;
						++foundCount_;
						target_ = target;
					}
				}
			}
			if (!lastCursor(cursor))
				return false;
			found = foundCount_ == 1;
			return true;
		}

		void Patch() override
		{
			if (!autoboot_.extended())
				return;
			uintptr_t addr = found_;
			patch32(addr - 4, assemble_mov_w(20, 0));
			patch32(addr, assemble_b(addr, target_));
		}
		bool required() const override { return autoboot_.extended(); }

	private:
		const AutobootPatcher& autoboot_;
		uintptr_t found_ = 0;
		uint32_t foundCount_ = 0;
		uintptr_t target_ = 0;
	};

	class SVCPatcher final : public Patcher
	{
	public:
		SVCPatcher(uintptr_t base, uintptr_t payload) : Patcher(base, "SVCPatcher"), payload_(payload) {}

		bool Match(const uint32_t* cursor, bool& found) override
		{
			uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
			uint32_t instruction = cursor[0];
			if (resolving_)
				resolveReference(addr, instruction);
			else
			{
				svcInstructionCount_ += instruction == ARM64_SVC_7 ? 1u : 0u;
				matchPrimary(cursor, addr);
				if (looksLikeHandler(cursor, addr))
				{
					if (fallbackCount_ < 8)
						fallback_[fallbackCount_++] = addr;
					else
						fallbackOverflow_ = true;
				}
			}
			if (!lastCursor(cursor))
				return false;
			if (!resolving_)
			{
				resolving_ = true;
				return false;
			}
			finalizeDiscovery();
			found = resolved_ != 0 && contains(base_, resolved_, 52, kCodeSize);
			return true;
		}

		void Patch() override
		{
			uint64_t payload = payload_;
			patch32(resolved_ + 16, assemble_movz_x(15, static_cast<uint16_t>(payload), 0));
			patch32(resolved_ + 20, assemble_movk_x(15, static_cast<uint16_t>(payload >> 16), 16));
			patch32(resolved_ + 24, assemble_movk_x(15, static_cast<uint16_t>(payload >> 32), 32));
			patch32(resolved_ + 28, ARM64_DSB_SY);
			patch32(resolved_ + 32, ARM64_MRS_X8_SCTLR_EL1);
			patch32(resolved_ + 36, ARM64_BIC_X8_X8_1);
			patch32(resolved_ + 40, ARM64_MSR_SCTLR_EL1_X8);
			patch32(resolved_ + 44, ARM64_ISB);
			patch32(resolved_ + 48, ARM64_BR_X15);
		}

	private:
		void finalizeDiscovery()
		{
			if (svcInstructionCount_ != 1)
				return;
			if (primaryCount_ != 0)
			{
				resolved_ = primaryCount_ == 1 ? primary_ : 0;
				return;
			}
			if (fallbackOverflow_)
				return;
			for (uint32_t i = 0; i < fallbackCount_; ++i)
			{
				if (fallbackReferences_[i] != 1)
					continue;
				if (resolved_ != 0)
				{
					resolved_ = 0;
					return;
				}
				resolved_ = fallback_[i];
			}
		}
		bool matchPrimary(const uint32_t* cursor, uintptr_t addr)
		{
			if (!contains(base_, addr, 36, kCodeSize) || cursor[0] != ARM64_PACIBSP
				|| cursor[1] != mov_register_x(4, 30)
				|| !is_bl(cursor[2])
				|| cursor[3] != mov_register_x(30, 4)
				|| !cmp(cursor[4], stp_x(29, 30, 31, 0), ~kPairImm7Mask)
				|| !cmp(cursor[5], add_x(29, 31, 0), ~kImm12Mask)
				|| ((cursor[4] >> 15) & 0x7fu) * 8 != ((cursor[5] >> 10) & 0xfffu)
				|| cursor[6] != mov_register_x(4, 30)
				|| !is_bl(cursor[7])
				|| cursor[8] != mov_register_x(30, 4))
				return false;
			uintptr_t getter = branch_target(addr + 28, cursor[7]);
			uintptr_t target = 0;
			uint32_t reg = 0;
			if (contains(base_, getter, 12, kCodeSize))
			{
				const auto* getterCursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(getter));
				if (decode_adrp_add(base_, getter, getterCursor[0], getterCursor[1], &target, &reg)
					&& target >= base_ + kCodeSize && reg == 8 && getterCursor[2] == ARM64_RET)
				{
					if (primaryCount_ == 0)
						primary_ = addr;
					++primaryCount_;
				}
			}
			return true;
		}

		bool looksLikeHandler(const uint32_t* cursor, uintptr_t addr) const
		{
			if (!contains(base_, addr, 0x90, kCodeSize) || cursor[0] != ARM64_PACIBSP)
				return false;
			bool outlined =
				cursor[1] == mov_register_x(4, 30)
				&& is_bl(cursor[2])
				&& cursor[3] == mov_register_x(30, 4);
			bool standard = cmp(cursor[1], sub_x(31, 31, 0), ~kImm12Mask);
			for (uint32_t i = 2; standard && i <= 3; ++i)
			{
				uint32_t pair = cursor[i];
				uint32_t first = pair & 0x1fu;
				uint32_t second = (pair >> 10) & 0x1fu;
				standard = cmp(pair, stp_x(0, 0, 31, 0), ~(kRtMask | kRt2Mask | kPairImm7Mask))
					&& first >= 19 && first <= 28 && second >= 19 && second <= 28;
			}
			if (!outlined && !standard)
				return false;
			uintptr_t body = 0;
			for (uint32_t i = 4; i <= 9; ++i)
			{
				uint32_t frame = cursor[i];
				uint32_t add = cursor[i + 1];
				uint32_t frameSize = ((frame >> 15) & 0x7fu) * 8;
				if (cmp(frame, stp_x(29, 30, 31, 0), ~kPairImm7Mask)
					&& cmp(add, add_x(29, 31, 0), ~kImm12Mask)
					&& frameSize == ((add >> 10) & 0xfffu) && frameSize >= 0xa0)
				{
					body = addr + static_cast<uintptr_t>(i + 2) * 4;
					break;
				}
			}
			if (body == 0)
				return false;
			uint32_t byteLoads = 0;
			bool cmpOne = false;
			bool conditionalBranch = false;
			uint32_t inputMask = 0;
			const auto* bodyCursor = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(body));
			for (uintptr_t current = body; current < addr + 0x90; current += 4, ++bodyCursor)
			{
				uint32_t instruction = bodyCursor[0];
				byteLoads += cmp(instruction, ldr_b(0, 0, 0), ~(kRtMask | kRnMask | kLoadStoreImm12Mask)) ?
					1u :
					0u;
				cmpOne |= cmp(instruction, cmp_w_immediate(0, 1), ~kRnMask);
				conditionalBranch |=
					cmp(instruction, b_cond(condition_Equal, 0, 0), ~(kConditionMask | kBranchImm19Mask))
					|| cmp(instruction, tbz(0, 0, 0, 0),
						~(kRtMask | kTestBitMask | kTestBranchImm14Mask | kTestBranchOpMask));
				if (cmp(instruction, mov_register_x(0, 0), ~(kRdMask | kRmMask)))
				{
					uint32_t source = (instruction >> 16) & 0x1fu;
					uint32_t destination = instruction & 0x1fu;
					if (source < 4 && destination >= 19 && destination <= 28)
						inputMask |= 1u << source;
				}
			}
			return byteLoads >= 2 && cmpOne && conditionalBranch && inputMask == 0xf;
		}

		bool resolveReference(uintptr_t addr, uint32_t instruction)
		{
			if (!is_b(instruction) && !is_bl(instruction))
				return false;
			uintptr_t target = branch_target(addr, instruction);
			for (uint32_t i = 0; i < fallbackCount_; ++i)
				if (target == fallback_[i])
					++fallbackReferences_[i];
			return true;
		}

		uintptr_t payload_;
		uintptr_t primary_ = 0;
		uint32_t primaryCount_ = 0;
		uint32_t svcInstructionCount_ = 0;
		uintptr_t fallback_[8] = {};
		uint32_t fallbackReferences_[8] = {};
		uint32_t fallbackCount_ = 0;
		bool fallbackOverflow_ = false;
		uintptr_t resolved_ = 0;
		bool resolving_ = false;
	};

}  // namespace

extern "C" int patchfinder(uintptr_t ibootAddr)
{
	uintptr_t payloadStart = reinterpret_cast<uintptr_t>(laikadfu);
#ifndef COMBINED_STAGE2_HOST_TEST
	uintptr_t payloadEnd = reinterpret_cast<uintptr_t>(laikadfu_end);
#else
	uintptr_t payloadEnd = payloadStart + sizeof(laikadfu);
#endif
	uintptr_t payloadSize = payloadEnd >= payloadStart ? payloadEnd - payloadStart : 0;
	int payloadError = payloadSize == 0 || payloadSize > 0x3000 ? -6 : 0;
#ifndef COMBINED_STAGE2_HOST_TEST
	uintptr_t pageStart = reinterpret_cast<uintptr_t>(start);
	if (payloadError == 0
		&& (payloadStart < pageStart || payloadStart - pageStart > kPageSize
			|| payloadSize > kPageSize - (payloadStart - pageStart)))
		payloadError = -7;
	uintptr_t payloadAddress = payloadStart;
#else
	uintptr_t payloadAddress = 0x19c384000ull;
#endif

	NVRAMPatcher autobootOnce(
		ibootAddr, "AutobootOncePatcher", kAutobootOnce, sizeof(kAutobootOnce), 1);
	AutobootPatcher autoboot(ibootAddr, autobootOnce);
	ForceLocalAutobootPatcher forceLocal(ibootAddr, autobootOnce, autoboot);
	IgnoreBootCommandPatcher ignoreBootCommand(ibootAddr, autobootOnce, autoboot);
	ClearBootdelayPatcher clearBootdelay(ibootAddr, autobootOnce, autoboot);
	WatchdogCallsPatcher watchdog(ibootAddr);
	Stage2Patcher stage2(ibootAddr);
	ReconfigPatcher reconfig(ibootAddr);
	AESPatcher aes(ibootAddr);
	RVBARPatcher rvbar(ibootAddr);
	FuseStayPatcher fuseStay(ibootAddr);
	FuseLockPatcher fuseLock(ibootAddr, fuseStay);
	FuseDebugPatcher fuseDebug(ibootAddr);
	ManifestHardwarePatcher manifestHardware(ibootAddr, autoboot);
	ManifestDigestPatcher manifestDigest(ibootAddr, autoboot);
	SVCPatcher svc(ibootAddr, payloadAddress);

	struct Entry
	{
		Patcher* patcher;
		bool found = false;
		bool done = false;
	};
	Entry patchfinders[] = {
		{&autobootOnce},
		{&autoboot},
		{&forceLocal},
		{&ignoreBootCommand},
		{&clearBootdelay},
		{&watchdog},
		{&stage2},
		{&reconfig},
		{&aes},
		{&rvbar},
		{&fuseStay},
		{&fuseLock},
		{&fuseDebug},
		{&manifestHardware},
		{&manifestDigest},
		{&svc},
	};
	constexpr uint32_t patchfinderCount = sizeof(patchfinders) / sizeof(patchfinders[0]);

	uint32_t doneCount = 0;
	while (doneCount != patchfinderCount)
		for (uintptr_t offset = 0; offset < kCodeSize; offset += 4)
			for (uint32_t i = 0; i < patchfinderCount; ++i)
			{
				Entry& entry = patchfinders[i];
				if (!entry.done
					&& entry.patcher->Match(reinterpret_cast<const uint32_t*>(ibootAddr + offset), entry.found))
				{
					entry.done = true;
					++doneCount;
				}
			}

	for (uint32_t i = 0; i < patchfinderCount; ++i)
		if (patchfinders[i].patcher->required() && !patchfinders[i].found)
			return -static_cast<int>(i + 1);
	if (payloadError != 0)
		return payloadError;

	// i had some issues getting iOS 18 to autoboot
	// more and better RE will clear this up but for now we are sort of shotgun patching this stuff
	for (uint32_t i = 0; i < patchfinderCount; ++i)
	{
#ifdef COMBINED_STAGE2_HOST_TEST
		combined_stage2_test_set_patcher(patchfinders[i].patcher->name());
#endif
		patchfinders[i].patcher->Patch();
	}
	return 0;
}
