// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cmath>
#include "math/math_util.h"

#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Common/CPUDetect.h"
#include "Core/Config.h"
#include "Core/Reporting.h"

#include "Core/MIPS/IR/IRFrontend.h"
#include "Core/MIPS/IR/IRRegCache.h"

// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.

// #define CONDITIONAL_DISABLE { fpr.ReleaseSpillLocksAndDiscardTemps(); Comp_Generic(op); return; }
#define CONDITIONAL_DISABLE ;
#define DISABLE { Comp_Generic(op); return; }

#define _RS MIPS_GET_RS(op)
#define _RT MIPS_GET_RT(op)
#define _RD MIPS_GET_RD(op)
#define _FS MIPS_GET_FS(op)
#define _FT MIPS_GET_FT(op)
#define _FD MIPS_GET_FD(op)
#define _SA MIPS_GET_SA(op)
#define _POS  ((op>> 6) & 0x1F)
#define _SIZE ((op>>11) & 0x1F)
#define _IMM16 (signed short)(op & 0xFFFF)
#define _IMM26 (op & 0x03FFFFFF)

namespace MIPSComp {
	static void ApplyVoffset(u8 regs[4], int count) {
		for (int i = 0; i < count; i++) {
			regs[i] = voffset[regs[i]];
		}
	}

	static bool IsConsecutive4(const u8 regs[4]) {
		return (regs[1] == regs[0] + 1 && regs[2] == regs[1] + 1 && regs[3] == regs[2] + 1);
	}

	void IRFrontend::Comp_VPFX(MIPSOpcode op)	{
		CONDITIONAL_DISABLE;
		int data = op & 0xFFFFF;
		int regnum = (op >> 24) & 3;
		switch (regnum) {
		case 0:  // S
			js.prefixS = data;
			js.prefixSFlag = JitState::PREFIX_KNOWN_DIRTY;
			break;
		case 1:  // T
			js.prefixT = data;
			js.prefixTFlag = JitState::PREFIX_KNOWN_DIRTY;
			break;
		case 2:  // D
			js.prefixD = data;
			js.prefixDFlag = JitState::PREFIX_KNOWN_DIRTY;
			break;
		default:
			ERROR_LOG(CPU, "VPFX - bad regnum %i : data=%08x", regnum, data);
			break;
		}
	}

	void IRFrontend::ApplyPrefixST(u8 *vregs, u32 prefix, VectorSize sz) {
		if (prefix == 0xE4)
			return;

		int n = GetNumVectorElements(sz);
		u8 origV[4];
		static const float constantArray[8] = { 0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f };

		for (int i = 0; i < n; i++)
			origV[i] = vregs[i];

		for (int i = 0; i < n; i++) {
			int regnum = (prefix >> (i * 2)) & 3;
			int abs = (prefix >> (8 + i)) & 1;
			int negate = (prefix >> (16 + i)) & 1;
			int constants = (prefix >> (12 + i)) & 1;

			// Unchanged, hurray.
			if (!constants && regnum == i && !abs && !negate)
				continue;

			/*
			// This puts the value into a temp reg, so we won't write the modified value back.
			vregs[i] = fpr.GetTempV();
			if (!constants) {
				fpr.MapDirtyInV(vregs[i], origV[regnum]);
				fpr.SpillLockV(vregs[i]);

				// Prefix may say "z, z, z, z" but if this is a pair, we force to x.
				// TODO: But some ops seem to use const 0 instead?
				if (regnum >= n) {
					WARN_LOG(CPU, "JIT: Invalid VFPU swizzle: %08x : %d / %d at PC = %08x (%s)", prefix, regnum, n, GetCompilerPC(), MIPSDisasmAt(GetCompilerPC()));
					regnum = 0;
				}

				if (abs) {
					fp.FABS(fpr.V(vregs[i]), fpr.V(origV[regnum]));
					if (negate)
						fp.FNEG(fpr.V(vregs[i]), fpr.V(vregs[i]));
				} else {
					if (negate)
						fp.FNEG(fpr.V(vregs[i]), fpr.V(origV[regnum]));
					else
						fp.FMOV(fpr.V(vregs[i]), fpr.V(origV[regnum]));
				}
			} else {
				fpr.MapRegV(vregs[i], MAP_DIRTY | MAP_NOINIT);
				fpr.SpillLockV(vregs[i]);
				fp.MOVI2F(fpr.V(vregs[i]), constantArray[regnum + (abs << 2)], SCRATCH1, (bool)negate);
			}
			*/
		}
	}

	void IRFrontend::GetVectorRegsPrefixD(u8 *regs, VectorSize sz, int vectorReg) {
		_assert_(js.prefixDFlag & JitState::PREFIX_KNOWN);

		GetVectorRegs(regs, sz, vectorReg);
		if (js.prefixD == 0)
			return;

		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n; i++) {
			// Hopefully this is rare, we'll just write it into a reg we drop.
			//if (js.VfpuWriteMask(i))
			//	regs[i] = fpr.GetTempV();
		}
	}

	void IRFrontend::ApplyPrefixD(const u8 *vregs, VectorSize sz) {
		_assert_(js.prefixDFlag & JitState::PREFIX_KNOWN);
		if (!js.prefixD)
			return;

		/*
		int n = GetNumVectorElements(sz);
		for (int i = 0; i < n; i++) {
			if (js.VfpuWriteMask(i))
				continue;

			int sat = (js.prefixD >> (i * 2)) & 3;
			if (sat == 1) {
				// clamped = x < 0 ? (x > 1 ? 1 : x) : x [0, 1]
				fpr.MapRegV(vregs[i], MAP_DIRTY);

				fp.MOVI2F(S0, 0.0f, SCRATCH1);
				fp.MOVI2F(S1, 1.0f, SCRATCH1);
				fp.FMIN(fpr.V(vregs[i]), fpr.V(vregs[i]), S1);
				fp.FMAX(fpr.V(vregs[i]), fpr.V(vregs[i]), S0);
			} else if (sat == 3) {
				// clamped = x < -1 ? (x > 1 ? 1 : x) : x [-1, 1]
				fpr.MapRegV(vregs[i], MAP_DIRTY);

				fp.MOVI2F(S0, -1.0f, SCRATCH1);
				fp.MOVI2F(S1, 1.0f, SCRATCH1);
				fp.FMIN(fpr.V(vregs[i]), fpr.V(vregs[i]), S1);
				fp.FMAX(fpr.V(vregs[i]), fpr.V(vregs[i]), S0);
			}
		}
		*/
	}

	void IRFrontend::Comp_SV(MIPSOpcode op) {
		s32 offset = (signed short)(op & 0xFFFC);
		int vt = ((op >> 16) & 0x1f) | ((op & 3) << 5);
		MIPSGPReg rs = _RS;
		switch (op >> 26) {
		case 50: //lv.s
			ir.Write(IROp::LoadFloatV, voffset[vt], rs, ir.AddConstant(offset));
			break;

		case 58: //sv.s
			ir.Write(IROp::StoreFloatV, voffset[vt], rs, ir.AddConstant(offset));
			break;

		default:
			DISABLE;
		}
	}

	void IRFrontend::Comp_SVQ(MIPSOpcode op) {
		int imm = (signed short)(op & 0xFFFC);
		int vt = (((op >> 16) & 0x1f)) | ((op & 1) << 5);
		MIPSGPReg rs = _RS;

		u8 vregs[4];
		GetVectorRegs(vregs, V_Quad, vt);
		ApplyVoffset(vregs, 4);  // Translate to memory order

		switch (op >> 26) {
		case 54: //lv.q
			if (IsConsecutive4(vregs)) {
				ir.Write(IROp::LoadVec4, vregs[0], rs, ir.AddConstant(imm));
			} else {
				// Let's not even bother with "vertical" loads for now.
				ir.Write(IROp::LoadFloatV, vregs[0], rs, ir.AddConstant(imm));
				ir.Write(IROp::LoadFloatV, vregs[1], rs, ir.AddConstant(imm + 4));
				ir.Write(IROp::LoadFloatV, vregs[2], rs, ir.AddConstant(imm + 8));
				ir.Write(IROp::LoadFloatV, vregs[3], rs, ir.AddConstant(imm + 12));
			}
			break;

		case 62: //sv.q
			if (IsConsecutive4(vregs)) {
				ir.Write(IROp::StoreVec4, vregs[0], rs, ir.AddConstant(imm));
			} else {
				// Let's not even bother with "vertical" stores for now.
				ir.Write(IROp::StoreFloatV, vregs[0], rs, ir.AddConstant(imm));
				ir.Write(IROp::StoreFloatV, vregs[1], rs, ir.AddConstant(imm + 4));
				ir.Write(IROp::StoreFloatV, vregs[2], rs, ir.AddConstant(imm + 8));
				ir.Write(IROp::StoreFloatV, vregs[3], rs, ir.AddConstant(imm + 12));
			}
			break;

		default:
			DISABLE;
			break;
		}
	}

	void IRFrontend::Comp_VVectorInit(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VIdt(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VMatrixInit(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VHdp(MIPSOpcode op) {
		DISABLE;
	}

	static const float MEMORY_ALIGNED16(vavg_table[4]) = { 1.0f, 1.0f / 2.0f, 1.0f / 3.0f, 1.0f / 4.0f };

	void IRFrontend::Comp_Vhoriz(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VDot(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VecDo3(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VV2Op(MIPSOpcode op) {
		CONDITIONAL_DISABLE;
		// Pre-processing: Eliminate silly no-op VMOVs, common in Wipeout Pure
		if (((op >> 16) & 0x1f) == 0 && _VS == _VD && js.HasNoPrefix()) {
			return;
		}
		DISABLE;
	}

	void IRFrontend::Comp_Vi2f(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vh2f(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vf2i(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Mftv(MIPSOpcode op) {
		int imm = op & 0xFF;
		MIPSGPReg rt = _RT;
		switch ((op >> 21) & 0x1f) {
		case 3: //mfv / mfvc
						// rt = 0, imm = 255 appears to be used as a CPU interlock by some games.
			if (rt != 0) {
				if (imm < 128) {  //R(rt) = VI(imm);
					ir.Write(IROp::VMovToGPR, rt, voffset[imm]);
				} else {
					DISABLE;
				}
			}
			break;

		case 7: // mtv
			if (imm < 128) {
				ir.Write(IROp::VMovFromGPR, voffset[imm], rt);
			} else {
				DISABLE;
			}
			break;

		default:
			DISABLE;
		}
	}

	void IRFrontend::Comp_Vmfvc(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vmtvc(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vmmov(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VScl(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vmmul(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vmscl(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vtfm(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VCrs(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VDet(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vi2x(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vx2i(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_VCrossQuat(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vcmp(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vcmov(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Viim(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vfim(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vcst(MIPSOpcode op) {
		DISABLE;
	}

	// Very heavily used by FF:CC. Should be replaced by a fast approximation instead of
	// calling the math library.
	void IRFrontend::Comp_VRot(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vsgn(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vocp(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_ColorConv(MIPSOpcode op) {
		DISABLE;
	}

	void IRFrontend::Comp_Vbfy(MIPSOpcode op) {
		DISABLE;
	}
}
