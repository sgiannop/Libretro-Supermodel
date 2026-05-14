#ifdef __aarch64__

#include "JitArm64.h"
#include "Arm64Emitter.h"

#include <sys/mman.h>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Function pointer table indices (8 bytes each, packed at m_code_buf[0])
// ---------------------------------------------------------------------------
enum {
    FN_JIT_READ8       = 0,
    FN_JIT_READ16      = 1,
    FN_JIT_READ32      = 2,
    FN_JIT_READ64      = 3,
    FN_JIT_WRITE8      = 4,
    FN_JIT_WRITE16     = 5,
    FN_JIT_WRITE32     = 6,
    FN_JIT_WRITE64     = 7,
    FN_JIT_READ_TBL    = 8,
    FN_JIT_READ_TBU    = 9,
    FN_JIT_FRES        = 10,
    FN_JIT_FRSQRTE     = 11,
    FN_PPC_DISPATCH    = 12,
    // indices 13-15 reserved
};
static_assert(FN_PPC_DISPATCH < JitArm64::FN_TABLE_ENTRIES, "FN table overflow");

// Pointer to the function table base (set when the code buffer is allocated).
// Used by emit_call() to choose ADRP+LDR_X over MOV_X64.
static uint8_t *g_fn_tbl = nullptr;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
JitArm64 &JitArm64::get()
{
    static JitArm64 s_instance;
    return s_instance;
}

// ---------------------------------------------------------------------------
// Code buffer allocation
// ---------------------------------------------------------------------------
bool JitArm64::init()
{
    if (m_code_buf) return true;    // already initialised

    // Try RWX mapping first (works on RPi5/Linux without selinux restrictions)
    void *p = mmap(nullptr, CODE_BUF_SIZE,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p != MAP_FAILED) {
        m_code_buf  = (uint8_t *)p;
        m_write_buf = m_code_buf;
        m_dual_map  = false;
    } else {
        // Fallback: allocate RW, compile blocks there, mprotect to RX before execution.
        // On Android 10+ this is the only reliable approach.
        p = mmap(nullptr, CODE_BUF_SIZE,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return false;
        m_write_buf = (uint8_t *)p;
        m_code_buf  = m_write_buf;
        m_dual_map  = false;
    }

    // Fill function pointer table at the very start of the write buffer.
    // Blocks start at FN_TABLE_BYTES to keep the table intact across flushes.
    void **tbl = (void **)m_write_buf;
    tbl[FN_JIT_READ8]    = (void *)&jit_read8;
    tbl[FN_JIT_READ16]   = (void *)&jit_read16;
    tbl[FN_JIT_READ32]   = (void *)&jit_read32;
    tbl[FN_JIT_READ64]   = (void *)&jit_read64;
    tbl[FN_JIT_WRITE8]   = (void *)&jit_write8;
    tbl[FN_JIT_WRITE16]  = (void *)&jit_write16;
    tbl[FN_JIT_WRITE32]  = (void *)&jit_write32;
    tbl[FN_JIT_WRITE64]  = (void *)&jit_write64;
    tbl[FN_JIT_READ_TBL] = (void *)&jit_read_tbl;
    tbl[FN_JIT_READ_TBU] = (void *)&jit_read_tbu;
    tbl[FN_JIT_FRES]     = (void *)&jit_fres;
    tbl[FN_JIT_FRSQRTE]  = (void *)&jit_frsqrte;
    tbl[FN_PPC_DISPATCH] = (void *)&ppc_dispatch_opcode;
    g_fn_tbl   = m_write_buf;
    m_code_pos = FN_TABLE_BYTES;    // blocks start after the table
    return true;
}

void JitArm64::shutdown()
{
    if (m_code_buf) {
        munmap(m_code_buf, CODE_BUF_SIZE);
        m_code_buf  = nullptr;
        m_write_buf = nullptr;
    }
    m_cache.clear();
    m_code_pos = 0;
}

void JitArm64::flush()
{
    m_cache.clear();
    // Preserve the function pointer table at offset 0; blocks restart after it.
    m_code_pos = (g_fn_tbl != nullptr) ? FN_TABLE_BYTES : 0;
    memset(m_fast_cache, 0, sizeof(m_fast_cache));
}

// ---------------------------------------------------------------------------
// Block lookup
// ---------------------------------------------------------------------------
JitBlock *JitArm64::get_or_compile(uint32_t pc)
{
    // Fast path: direct-mapped cache (avoids hash map for hot blocks)
    uint32_t slot = (pc >> 2) & FAST_CACHE_MASK;
    JitBlock *blk = m_fast_cache[slot];
    if (blk && blk->start_pc == pc) return blk;

    // Slow path: hash map lookup or compilation
    auto it = m_cache.find(pc);
    if (it != m_cache.end()) {
        m_fast_cache[slot] = &it->second;
        return &it->second;
    }
    blk = compile(pc);
    if (blk) m_fast_cache[slot] = blk;
    return blk;
}

// ---------------------------------------------------------------------------
// Helpers used during compilation
// ---------------------------------------------------------------------------

// Limits
static constexpr int MAX_BLOCK_INSTS = 128;

// ARM64 register assignments:
//   X19  = pointer to PPC_REGS (callee-saved, set in prologue)
//   W0-W4 = scratch for computations
//   X16  = scratch for function addresses (caller-saved, IP0)
//
// Struct field offsets computed via offsetof()
static int OFF_FATAL;
static int OFF_R;
static int OFF_PC;
static int OFF_NPC;
static int OFF_LR;
static int OFF_CTR;
static int OFF_XER;
static int OFF_MSR;
static int OFF_CR;
static int OFF_ICOUNT;
static int OFF_FPR;
static int OFF_SPRG;
static int OFF_DEC;
static int OFF_FPSCR;
static int OFF_SRR0;
static int OFF_SRR1;
static int OFF_DEC_TRIGGER;
static int OFF_INT_PENDING;

static bool g_offsets_computed = false;

static void compute_offsets()
{
    if (g_offsets_computed) return;
    PPC_REGS *dummy = nullptr;
#define OFF(f) (int)((uint8_t*)&dummy->f - (uint8_t*)dummy)
    OFF_FATAL  = OFF(fatalError);
    OFF_R      = OFF(r[0]);
    OFF_PC     = OFF(pc);
    OFF_NPC    = OFF(npc);
    OFF_LR     = OFF(lr);
    OFF_CTR    = OFF(ctr);
    OFF_XER    = OFF(xer);
    OFF_MSR    = OFF(msr);
    OFF_CR     = OFF(cr[0]);
    OFF_ICOUNT = OFF(icount);
    OFF_FPR    = OFF(fpr[0]);
    OFF_SPRG   = OFF(sprg[0]);
    OFF_DEC    = OFF(dec);
    OFF_FPSCR  = OFF(fpscr);
    OFF_SRR0        = OFF(srr0);
    OFF_SRR1        = OFF(srr1);
    OFF_DEC_TRIGGER = OFF(dec_trigger_cycle);
    OFF_INT_PENDING = OFF(interrupt_pending);
#undef OFF
    g_offsets_computed = true;
}

// X19 register index (holds PPC_REGS*)
static constexpr int PPC_PTR = 19;

// Scratch registers (caller-saved, safe to use freely between C calls)
static constexpr int W0 = 0, W1 = 1, W2 = 2, W3 = 3, W4 = 4;
static constexpr int X16 = 16;   // IP0: scratch for call targets

// FP scratch registers (D0-D7 are caller-saved; we use D0-D2 for JIT ops)
static constexpr int D0 = 0, D1 = 1, D2 = 2;

// ---------------------------------------------------------------------------
// Emit helpers
// ---------------------------------------------------------------------------

// Load a PPC GPR into a W register
static void emit_load_gpr(Arm64Emitter &e, int dst_Wn, int ppc_reg)
{
    int off = OFF_R + ppc_reg * 4;
    if (off < 16384)
        e.LDR_W(dst_Wn, PPC_PTR, off);
    else {
        e.MOV_W32(dst_Wn, off);
        // Use extended register addressing: LDR Wt, [X19, X0] not easily encoded; just use LDUR with offset
        // For large offsets: add base to offset reg, LDR with register offset
        // Since OFF_R + 31*4 = at most ~128+124 = ~252 bytes, we're always under 16384. Safe.
    }
}

// Store a W register into a PPC GPR
static void emit_store_gpr(Arm64Emitter &e, int src_Wn, int ppc_reg)
{
    int off = OFF_R + ppc_reg * 4;
    e.STR_W(src_Wn, PPC_PTR, off);
}

// Load a PPC FPR (stored as 64-bit double) into an ARM FP register
static void emit_load_fpr(Arm64Emitter &e, int dst_Dd, int ppc_fpr)
{
    int off = OFF_FPR + ppc_fpr * 8;
    e.LDR_D(dst_Dd, PPC_PTR, (uint32_t)off);
}

// Store an ARM FP register into a PPC FPR slot
static void emit_store_fpr(Arm64Emitter &e, int src_Dd, int ppc_fpr)
{
    int off = OFF_FPR + ppc_fpr * 8;
    e.STR_D(src_Dd, PPC_PTR, (uint32_t)off);
}

// Emit: Wd = sign_extend_16(simm16)  [1-2 instructions]
static void emit_load_simm16(Arm64Emitter &e, int Wd, int16_t simm)
{
    uint32_t v = (uint32_t)(int32_t)simm;
    e.MOV_W32(Wd, v);
}

// Add simm16 to Wn and store in Wd, keeping Wn unchanged
static void emit_add_simm16(Arm64Emitter &e, int Wd, int Wn, int16_t simm)
{
    if (simm == 0) {
        if (Wd != Wn) e.MOV_W(Wd, Wn);
        return;
    }
    int v = simm;
    if (v > 0 && v <= 4095) {
        e.ADD_W_IMM(Wd, Wn, v);
    } else if (v < 0 && -v <= 4095) {
        e.SUB_W_IMM(Wd, Wn, (uint32_t)(-v));
    } else {
        e.MOV_W32(W1, (uint32_t)(int32_t)simm);
        e.ADD_W(Wd, Wn, W1);
    }
}

// Update CR field crfD after a signed comparison (flags already set by CMP/SUBS)
// Uses W1, W2, W3 as scratch. Clobbers W0 if reading XER.
static void emit_cr_from_flags_signed(Arm64Emitter &e, int crfD)
{
    // Build: cr[crfD] = (LT<<3) | (GT<<2) | (EQ<<1) | SO
    e.CSET_W(W1, A64_LT);           // W1 = 1 if LT (signed)
    e.CSET_W(W2, A64_GT);           // W2 = 1 if GT (signed)
    e.CSET_W(W3, A64_EQ);           // W3 = 1 if EQ
    e.LSL_W_IMM(W1, W1, 3);         // W1 = LT<<3
    e.ORR_W_LSL(W1, W1, W2, 2);     // W1 |= GT<<2
    e.ORR_W_LSL(W1, W1, W3, 1);     // W1 |= EQ<<1
    // Add SO bit from XER bit 31 (fold LSR+ORR into one shifted-register ORR)
    e.LDR_W(W2, PPC_PTR, OFF_XER);
    e.ORR_W_LSR(W1, W1, W2, 31);    // W1 |= (XER >> 31) = W1 | SO
    e.STRB(W1, PPC_PTR, OFF_CR + crfD);
}

// Update CR field crfD after unsigned comparison (flags set by CMP/SUBS for unsigned)
// Uses cmpl which is really unsigned: CMP as unsigned = use Unsigned Higher / Lower
static void emit_cr_from_flags_unsigned(Arm64Emitter &e, int crfD)
{
    // For unsigned CMP rA, rB: LT = CC (carry clear), GT = HI, EQ = EQ
    e.CSET_W(W1, A64_CC);           // W1 = 1 if unsigned LT
    e.CSET_W(W2, A64_HI);           // W2 = 1 if unsigned GT
    e.CSET_W(W3, A64_EQ);           // W3 = 1 if EQ
    e.LSL_W_IMM(W1, W1, 3);
    e.ORR_W_LSL(W1, W1, W2, 2);
    e.ORR_W_LSL(W1, W1, W3, 1);
    e.LDR_W(W2, PPC_PTR, OFF_XER);
    e.ORR_W_LSR(W1, W1, W2, 31);    // W1 |= (XER >> 31) = W1 | SO
    e.STRB(W1, PPC_PTR, OFF_CR + crfD);
}

// Update CR0 after an ALU instruction that updates flags (Rc=1 bit)
// Assumes the result is in W0 and flags are NOT set. Sets flags first.
static void emit_set_cr0_from_W0(Arm64Emitter &e)
{
    e.CMP_W_IMM(W0, 0);             // set flags based on W0
    emit_cr_from_flags_signed(e, 0);
}

// Extract CR bit crbit into bit 0 of Wdst (0 or 1). Clobbers only Wdst.
// crbit is 0-31 where bit 0 = CR0[LT], bit 31 = CR7[SO].
static void emit_load_cr_bit(Arm64Emitter &e, int Wdst, int crbit)
{
    int field  = crbit / 4;
    int bitpos = 3 - (crbit % 4);   // nibble bit: 3=MSB(LT), 0=LSB(SO)
    e.LDRB(Wdst, PPC_PTR, OFF_CR + field);
    e.UBFM_W(Wdst, Wdst, bitpos, bitpos);  // extract bit[bitpos] → bit[0], zero-extend
}

// Store bit 0 of Wbit into CR bit crbit. Clobbers Wbit (unused after), Wtmp.
static void emit_store_cr_bit(Arm64Emitter &e, int Wbit, int Wtmp, int crbit)
{
    int field  = crbit / 4;
    int bitpos = 3 - (crbit % 4);
    e.LDRB(Wtmp, PPC_PTR, OFF_CR + field);
    e.BFI_W(Wtmp, Wbit, bitpos, 1);    // insert Wbit[0] into Wtmp[bitpos]
    e.STRB(Wtmp, PPC_PTR, OFF_CR + field);
}

// Load XER.CA (bit 29) into Wdst as 0 or 1. Clobbers only Wdst.
static void emit_load_xer_ca(Arm64Emitter &e, int Wdst)
{
    e.LDR_W(Wdst, PPC_PTR, OFF_XER);
    e.UBFM_W(Wdst, Wdst, 29, 29);   // extract bit 29 → bit 0
}

// Set ARM carry flag from Wca (0 or 1). Clobbers Wtmp.
// After this call: ARM C = Wca.
static void emit_arm_carry_from_W(Arm64Emitter &e, int Wca, int Wtmp)
{
    e.MVN_W(Wtmp, A64_WZR);          // Wtmp = 0xFFFFFFFF
    e.ADDS_W(A64_WZR, Wca, Wtmp);    // 0+0xFFFFFFFF=no carry, 1+0xFFFFFFFF=carry
}

// After ADDS_W/ADCS_W/SUBS_W/SBCS_W, write ARM carry flag → XER.CA.
// Clobbers W2, W3. Must be called immediately after the flag-setting op.
static void emit_update_xer_ca(Arm64Emitter &e)
{
    e.CSET_W(W2, A64_CS);            // W2 = carry (reads flags before anything else)
    e.LDR_W(W3, PPC_PTR, OFF_XER);
    e.BFI_W(W3, W2, 29, 1);          // insert carry into XER.CA (bit 29), clear old
    e.STR_W(W3, PPC_PTR, OFF_XER);
}

// Call an external C function.  When the function is in the pointer table at
// the start of the code buffer, emit ADRP + LDR_X (2 instructions) instead of
// MOV_X64 (3-4 instructions), saving 1-2 ARM instructions per C call.
static void emit_call(Arm64Emitter &e, uint64_t fn_addr)
{
    if (g_fn_tbl) {
        // Linear scan of the table (13 entries, compile-time only — not hot)
        void **tbl = (void **)g_fn_tbl;
        for (int i = 0; i < (int)JitArm64::FN_TABLE_ENTRIES; i++) {
            if ((uint64_t)tbl[i] == fn_addr) {
                // Load fn pointer via PC-relative ADRP + LDR_X (always within ±4 GB)
                uint64_t entry_addr  = (uint64_t)(g_fn_tbl + i * 8);
                int64_t  page_off    = (int64_t)(entry_addr & ~0xFFFULL)
                                     - (int64_t)((uint64_t)e.ptr() & ~0xFFFULL);
                uint32_t byte_in_pg  = (uint32_t)(entry_addr & 0xFFF);
                e.ADRP(X16, page_off);
                e.LDR_X(X16, X16, byte_in_pg);
                e.BLR(X16);
                return;
            }
        }
    }
    // Fallback: full 64-bit constant load
    e.MOV_X64(X16, fn_addr);
    e.BLR(X16);
}

// Store pc and npc before a fallback instruction call
static void emit_set_pc_npc(Arm64Emitter &e, uint32_t inst_pc)
{
    e.MOV_W32(W0, inst_pc);
    e.ADD_W_IMM(W1, W0, 4);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);
}

// Emit a fallback call to ppc_dispatch_opcode(opcode)
static void emit_fallback(Arm64Emitter &e, uint32_t opcode, uint32_t inst_pc)
{
    emit_set_pc_npc(e, inst_pc);
    e.MOV_W32(W0, opcode);
    emit_call(e, (uint64_t)(void *)&ppc_dispatch_opcode);
}

// ---------------------------------------------------------------------------
// Effective address helpers
// ---------------------------------------------------------------------------

// D-form (immediate): EA = (rA==0 ? 0 : REG(rA)) + simm16  → W0
static void emit_load_ea_imm(Arm64Emitter &e, int rA, int16_t simm)
{
    if (rA == 0) {
        emit_load_simm16(e, W0, simm);
    } else {
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);
    }
}

// X-form (register): EA = (rA==0 ? 0 : REG(rA)) + REG(rB)  → Wd  (uses W1 as scratch)
static void emit_load_ea_reg(Arm64Emitter &e, int Wd, int rA, int rB)
{
    if (rA == 0) {
        emit_load_gpr(e, Wd, rB);
    } else {
        emit_load_gpr(e, Wd, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(Wd, Wd, W1);
    }
}

// ---------------------------------------------------------------------------
// Memory load/store translators
// ---------------------------------------------------------------------------

// D-form loads: opcodes 32 (lwz), 33 (lwzu), 34 (lbz), 35 (lbzu),
//               40 (lhz), 41 (lhzu), 42 (lha), 43 (lhau)
static bool translate_load_imm(Arm64Emitter &e, uint32_t op, int opcode)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    bool update = (opcode & 1);   // odd opcodes in each pair are the update form

    if (update) {
        // EA = REG(rA) + simm; REG(rA) = EA before memory access
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);
        emit_store_gpr(e, W0, rA);      // rA = EA (W0 still holds EA)
    } else {
        emit_load_ea_imm(e, rA, simm);  // W0 = EA
    }

    switch (opcode) {
    case 32: case 33:
        emit_call(e, (uint64_t)(void *)&jit_read32); break;
    case 34: case 35:
        emit_call(e, (uint64_t)(void *)&jit_read8);  break;
    case 40: case 41:
        emit_call(e, (uint64_t)(void *)&jit_read16); break;
    case 42: case 43:
        emit_call(e, (uint64_t)(void *)&jit_read16);
        e.SXTH_W(W0, W0);              // sign-extend for lha/lhau
        break;
    default: return false;
    }
    emit_store_gpr(e, W0, rD);
    return true;
}

// D-form stores: opcodes 36 (stw), 37 (stwu), 38 (stb), 39 (stbu),
//                44 (sth), 45 (sthu)
static bool translate_store_imm(Arm64Emitter &e, uint32_t op, int opcode)
{
    int rS    = (op >> 21) & 0x1F;   // source data register
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    bool update = (opcode & 1);

    if (update) {
        // Compute EA; load rS before writeback to handle rS==rA correctly
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);   // W0 = EA
        emit_load_gpr(e, W1, rS);           // W1 = rS (before rA is overwritten)
        emit_store_gpr(e, W0, rA);          // rA = EA
    } else {
        emit_load_ea_imm(e, rA, simm);      // W0 = EA
        emit_load_gpr(e, W1, rS);           // W1 = data
    }

    switch (opcode) {
    case 36: case 37: emit_call(e, (uint64_t)(void *)&jit_write32); break;
    case 38: case 39: emit_call(e, (uint64_t)(void *)&jit_write8);  break;
    case 44: case 45: emit_call(e, (uint64_t)(void *)&jit_write16); break;
    default: return false;
    }
    return true;
}

// Emit the block epilogue: update icount, set pc/npc, restore and return
// Variant: NPC comes from a register (W_npc) rather than a compile-time constant.
// The caller must ensure W_npc is not W0 or W1.
static void emit_epilogue_npc_reg(Arm64Emitter &e, int inst_count, uint32_t last_pc, int W_npc)
{
    e.LDR_W(W0, PPC_PTR, OFF_ICOUNT);
    e.SUB_W_IMM(W0, W0, inst_count);
    e.STR_W(W0, PPC_PTR, OFF_ICOUNT);
    e.MOV_W32(W1, last_pc);
    e.STP_W(W1, W_npc, PPC_PTR, OFF_PC);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    e.RET();
}

static void emit_epilogue(Arm64Emitter &e, int inst_count, uint32_t last_pc, uint32_t next_pc)
{
    // ppc.icount -= inst_count
    e.LDR_W(W0, PPC_PTR, OFF_ICOUNT);
    if (inst_count <= 4095)
        e.SUB_W_IMM(W0, W0, inst_count);
    else {
        e.MOV_W32(W1, inst_count);
        e.SUB_W(W0, W0, W1);
    }
    e.STR_W(W0, PPC_PTR, OFF_ICOUNT);

    // ppc.pc = last_pc; ppc.npc = next_pc (store pair)
    e.MOV_W32(W0, last_pc);
    e.MOV_W32(W1, next_pc);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);

    e.LDP_post(PPC_PTR, 30, A64_SP, 16);  // restore X19, X30
    e.RET();
}

// Tail-call epilogue for a statically-known block target already compiled.
// Updates icount/pc/npc, mirrors the dispatch-loop dec_trigger and interrupt
// checks, then — if cycles remain and nothing is pending — restores the frame
// and jumps directly to target_fn (no dispatch-loop roundtrip).
// Both target_fn and this code must be within the same 16 MB code buffer. ✓
//
// Register usage: W4 = new_icount (preserved across the checks).
static void emit_epilogue_chained(Arm64Emitter &e, int inst_count, uint32_t last_pc,
                                   uint32_t next_pc, void *target_fn)
{
    // W4 = new_icount (keep for comparisons below)
    e.LDR_W(W4, PPC_PTR, OFF_ICOUNT);
    if (inst_count <= 4095)
        e.SUB_W_IMM(W4, W4, (uint32_t)inst_count);
    else {
        e.MOV_W32(W0, (uint32_t)inst_count);
        e.SUB_W(W4, W4, W0);
    }
    e.STR_W(W4, PPC_PTR, OFF_ICOUNT);

    e.MOV_W32(W0, last_pc);
    e.MOV_W32(W1, next_pc);
    e.STP_W(W0, W1, PPC_PTR, OFF_PC);

    // Mirror dispatch-loop: if (new_icount <= dec_trigger_cycle) interrupt_pending |= 2
    e.LDR_W(W1, PPC_PTR, (uint32_t)OFF_DEC_TRIGGER);
    e.CMP_W(W4, W1);                                         // W4 vs dec_trigger (signed)
    uint32_t *dec_ok = e.emit_B_COND_placeholder(A64_GT);   // B.GT skip_dec_set
    e.LDR_W(W0, PPC_PTR, (uint32_t)OFF_INT_PENDING);
    e.MOV_W32(W1, 2);
    e.ORR_W(W0, W0, W1);
    e.STR_W(W0, PPC_PTR, (uint32_t)OFF_INT_PENDING);
    e.patch_B_COND(dec_ok, e.ptr());

    // Exit if cycles exhausted or fatal error or interrupt pending
    e.CMP_W_IMM(W4, 0);
    uint32_t *exit1 = e.emit_B_COND_placeholder(A64_LE);    // B.LE slow_exit
    e.LDRB(W0, PPC_PTR, (uint32_t)OFF_FATAL);
    uint32_t *exit2 = e.emit_CBNZ_W_placeholder(W0);        // CBNZ slow_exit
    e.LDR_W(W0, PPC_PTR, (uint32_t)OFF_INT_PENDING);
    uint32_t *exit3 = e.emit_CBNZ_W_placeholder(W0);        // CBNZ slow_exit

    // Fast path: tail call; X0 = PPC_REGS* for callee prologue
    e.MOV_X(0, PPC_PTR);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    int64_t off = (int64_t)target_fn - (int64_t)e.ptr();
    e.B((int)off);

    // Slow exit: return to dispatch loop
    uint32_t *slow_exit = e.ptr();
    e.patch_B_COND(exit1, slow_exit);
    e.patch_CBZ(exit2, slow_exit);
    e.patch_CBZ(exit3, slow_exit);
    e.LDP_post(PPC_PTR, 30, A64_SP, 16);
    e.RET();
}

// ---------------------------------------------------------------------------
// PPC instruction translators
// ---------------------------------------------------------------------------

// Primary opcode 14: addi rD, rA, SIMM  (includes li = addi rD, r0, SIMM)
static bool translate_addi(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (rA == 0) {
        // li rD, simm
        emit_load_simm16(e, W0, simm);
    } else {
        emit_load_gpr(e, W0, rA);
        emit_add_simm16(e, W0, W0, simm);
    }
    emit_store_gpr(e, W0, rD);
    return true;
}

// Primary opcode 15: addis rD, rA, SIMM  (includes lis = addis rD, r0, SIMM)
static bool translate_addis(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t imm = (uint32_t)(int16_t)(op & 0xFFFF) << 16;

    if (rA == 0) {
        e.MOV_W32(W0, imm);
    } else {
        emit_load_gpr(e, W0, rA);
        e.MOV_W32(W1, imm);
        e.ADD_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rD);
    return true;
}

// Primary opcode 24: ori rA, rS, UIMM
static bool translate_ori(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    if (uimm == 0) {
        // nop if rA == rS, else mr
        if (rA != rS) { emit_load_gpr(e, W0, rS); emit_store_gpr(e, W0, rA); }
        return true;
    }
    emit_load_gpr(e, W0, rS);
    e.MOV_W32(W1, uimm);
    e.ORR_W(W0, W0, W1);
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 25: oris rA, rS, UIMM
static bool translate_oris(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = (op & 0xFFFF) << 16;

    emit_load_gpr(e, W0, rS);
    e.MOV_W32(W1, uimm);
    e.ORR_W(W0, W0, W1);
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 26: xori rA, rS, UIMM
static bool translate_xori(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    emit_load_gpr(e, W0, rS);
    e.MOV_W32(W1, uimm);
    e.EOR_W(W0, W0, W1);
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 27: xoris rA, rS, UIMM
static bool translate_xoris(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = (op & 0xFFFF) << 16;

    emit_load_gpr(e, W0, rS);
    e.MOV_W32(W1, uimm);
    e.EOR_W(W0, W0, W1);
    emit_store_gpr(e, W0, rA);
    return true;
}

// Primary opcode 28: andi. rA, rS, UIMM  (always updates CR0)
static bool translate_andi_dot(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    emit_load_gpr(e, W0, rS);
    e.MOV_W32(W1, uimm);
    e.AND_W(W0, W0, W1);
    emit_store_gpr(e, W0, rA);
    emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 29: andis. rA, rS, UIMM
static bool translate_andis_dot(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = (op & 0xFFFF) << 16;

    emit_load_gpr(e, W0, rS);
    e.MOV_W32(W1, uimm);
    e.AND_W(W0, W0, W1);
    emit_store_gpr(e, W0, rA);
    emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 11: cmpi crfD, rA, SIMM  (cmpwi)
static bool translate_cmpi(Arm64Emitter &e, uint32_t op)
{
    int crfD = (op >> 23) & 0x7;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    if (simm >= 0 && (uint32_t)simm <= 4095) {
        e.CMP_W_IMM(W0, (uint32_t)simm);
    } else if (simm < 0 && (uint32_t)(-simm) <= 4095) {
        e.CMN_W_IMM(W0, (uint32_t)(-simm));   // rA - simm = rA + |simm|
    } else {
        e.MOV_W32(W1, (uint32_t)(int32_t)simm);
        e.CMP_W(W0, W1);
    }
    emit_cr_from_flags_signed(e, crfD);
    return true;
}

// Primary opcode 10: cmpli crfD, rA, UIMM  (cmplwi, unsigned)
static bool translate_cmpli(Arm64Emitter &e, uint32_t op)
{
    int crfD = (op >> 23) & 0x7;
    int rA   = (op >> 16) & 0x1F;
    uint32_t uimm = op & 0xFFFF;

    emit_load_gpr(e, W0, rA);
    if (uimm <= 4095)
        e.CMP_W_IMM(W0, uimm);
    else {
        e.MOV_W32(W1, uimm);
        e.CMP_W(W0, W1);
    }
    emit_cr_from_flags_unsigned(e, crfD);
    return true;
}

// Primary opcode 7: mulli rD, rA, SIMM
static bool translate_mulli(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    e.MOV_W32(W1, (uint32_t)(int32_t)simm);
    e.MUL_W(W0, W0, W1);
    emit_store_gpr(e, W0, rD);
    return true;
}

// Primary opcode 21: rlwinm rA, rS, SH, MB, ME
static bool translate_rlwinm(Arm64Emitter &e, uint32_t op)
{
    int rS = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int sh = (op >> 11) & 0x1F;
    int mb = (op >> 6)  & 0x1F;
    int me = (op >> 1)  & 0x1F;
    int rc = op & 1;

    // Peephole: identity (sh=0, mb=0, me=31 → mask=all, no rotate)
    if (sh == 0 && mb == 0 && me == 31) {
        if (rc == 0 && rA == rS) return true;
        emit_load_gpr(e, W0, rS);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // Peephole: logical shift left — ROTL(rS,sh) & ~((1<<sh)-1) = rS << sh
    // Condition: mb==0, me==31-sh  (mask covers [sh..31], lower sh bits zeroed)
    if (sh > 0 && mb == 0 && me == 31 - sh) {
        emit_load_gpr(e, W0, rS);
        e.LSL_W_IMM(W0, W0, sh);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // Peephole: UBFX — me==31 means result starts at bit 0; when mb>=32-sh the source
    // field [32-sh .. 63-sh-mb] of rS doesn't wrap, encodable as UBFM(immr=32-sh, imms=63-sh-mb).
    // Covers LSR (mb==32-sh) and narrower extracts.
    if (sh > 0 && me == 31 && mb >= 32 - sh) {
        emit_load_gpr(e, W0, rS);
        e.UBFM_W(W0, W0, 32 - sh, 63 - sh - mb);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // Peephole: sh=0, me==31, mb>0 — zero the upper mb bits via UBFX from bit 0
    if (sh == 0 && me == 31 && mb > 0) {
        emit_load_gpr(e, W0, rS);
        e.UBFM_W(W0, W0, 0, 31 - mb);   // extract bits [0..31-mb], zero [32-mb..31]
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // General path: compute mask, rotate, AND
    // In standard bit ordering (bit 0 = LSB):
    //   mask spans bits [31-mb .. 31-me]  if mb <= me
    //   mask = ~0 & complement of gap    if mb >  me (wrapping)
    uint32_t mask;
    if (mb <= me) {
        int start = 31 - me;
        int end   = 31 - mb;
        int width = end - start + 1;
        mask = ((width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << start;
    } else {
        int start = 31 - mb + 1;
        int end   = 31 - me - 1;
        int width = end - start + 1;
        uint32_t gap = ((width <= 0) ? 0u : (((width == 32) ? 0xFFFFFFFFu : (1u << width) - 1u) << start));
        mask = ~gap;
    }

    emit_load_gpr(e, W0, rS);

    if (sh > 0)
        e.ROR_W_IMM(W0, W0, 32 - sh);   // ROTL32(rS, sh) = ROR32(rS, 32-sh)

    if (mask != 0xFFFFFFFFu) {
        e.MOV_W32(W1, mask);
        e.AND_W(W0, W0, W1);
    }

    emit_store_gpr(e, W0, rA);

    if (rc) emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 20: rlwimi rA, rS, SH, MB, ME  (rotate left word immediate then mask insert)
static bool translate_rlwimi(Arm64Emitter &e, uint32_t op)
{
    int rS = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int sh = (op >> 11) & 0x1F;
    int mb = (op >> 6)  & 0x1F;
    int me = (op >> 1)  & 0x1F;
    int rc = op & 1;

    uint32_t mask;
    if (mb <= me) {
        int start = 31 - me;
        int end   = 31 - mb;
        int width = end - start + 1;
        mask = ((width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << start;
    } else {
        int start = 31 - mb + 1;
        int end   = 31 - me - 1;
        int width = end - start + 1;
        uint32_t gap = ((width <= 0) ? 0u : (((width == 32) ? 0xFFFFFFFFu : (1u << width) - 1u) << start));
        mask = ~gap;
    }

    emit_load_gpr(e, W0, rS);          // W0 = rS
    if (sh > 0) e.ROR_W_IMM(W0, W0, 32 - sh);  // W0 = ROTL(rS, sh)
    e.MOV_W32(W1, mask);
    e.AND_W(W0, W0, W1);               // W0 = rotated_rS & mask

    emit_load_gpr(e, W2, rA);          // W2 = rA
    e.MOV_W32(W3, ~mask);
    e.AND_W(W2, W2, W3);               // W2 = rA & ~mask
    e.ORR_W(W0, W0, W2);               // W0 = inserted result

    emit_store_gpr(e, W0, rA);
    if (rc) emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 23: rlwnm rA, rS, rB, MB, ME  (rotate left word then AND with mask)
static bool translate_rlwnm(Arm64Emitter &e, uint32_t op)
{
    int rS = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int rB = (op >> 11) & 0x1F;
    int mb = (op >> 6)  & 0x1F;
    int me = (op >> 1)  & 0x1F;
    int rc = op & 1;

    uint32_t mask;
    if (mb <= me) {
        int start = 31 - me;
        int end   = 31 - mb;
        int width = end - start + 1;
        mask = ((width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << start;
    } else {
        int start = 31 - mb + 1;
        int end   = 31 - me - 1;
        int width = end - start + 1;
        uint32_t gap = ((width <= 0) ? 0u : (((width == 32) ? 0xFFFFFFFFu : (1u << width) - 1u) << start));
        mask = ~gap;
    }

    emit_load_gpr(e, W0, rS);
    emit_load_gpr(e, W1, rB);
    e.AND_W(W1, W1, 31);            // W1 = sh = rB & 31
    e.NEG_W(W2, W1);                // W2 = -sh = 32-sh (mod 32) for RORV
    e.ROR_W(W0, W0, W2);            // W0 = ROTL(rS, sh) = ROR(rS, 32-sh)

    if (mask != 0xFFFFFFFFu) {
        e.MOV_W32(W1, mask);
        e.AND_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rA);
    if (rc) emit_set_cr0_from_W0(e);
    return true;
}

// ---------------------------------------------------------------------------
// Opcode-31 sub-operations
// ---------------------------------------------------------------------------

// Forward declarations for X-form load/store helpers (defined after translate_op31)
static bool translate_op31_load_x(Arm64Emitter &e, int rD, int rA, int rB,
                                   void *reader, bool sign_ext);
static bool translate_op31_load_xu(Arm64Emitter &e, int rD, int rA, int rB,
                                    void *reader, bool sign_ext);
static bool translate_op31_store_x(Arm64Emitter &e, int rS, int rA, int rB, void *writer);
static bool translate_op31_store_xu(Arm64Emitter &e, int rS, int rA, int rB, void *writer);

static bool translate_op31(Arm64Emitter &e, uint32_t op)
{
    int rD  = (op >> 21) & 0x1F;
    int rA  = (op >> 16) & 0x1F;
    int rB  = (op >> 11) & 0x1F;
    int rc  = op & 1;
    int subop = (op >> 1) & 0x3FF;

    switch (subop) {
    // cmp crfD, rA, rB  (signed compare)
    case 0: {
        int crfD = (op >> 23) & 0x7;
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.CMP_W(W0, W1);
        emit_cr_from_flags_signed(e, crfD);
        return true;
    }
    // cmpl crfD, rA, rB  (unsigned compare)
    case 32: {
        int crfD = (op >> 23) & 0x7;
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.CMP_W(W0, W1);    // ARM unsigned compare same instruction; flags different
        emit_cr_from_flags_unsigned(e, crfD);
        return true;
    }
    // add rD, rA, rB  (addo = 778: OE ignored, same result)
    case 778:
    case 266:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // addc rD, rA, rB  (rD = rA + rB, XER.CA = carry out; addco = 522)
    case 522:
    case 10:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADDS_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);          // clobbers W2,W3,W4; W0 unchanged
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // adde rD, rA, rB  (rD = rA + rB + XER.CA; addeo = 650)
    case 650:
    case 138:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        emit_load_xer_ca(e, W2);        // W2 = XER.CA
        emit_arm_carry_from_W(e, W2, W3); // ARM C = XER.CA; clobbers W3
        e.ADCS_W(W0, W0, W1);           // W0 = rA + rB + C; new ARM C = carry
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // addze rD, rA  (rD = rA + 0 + XER.CA; addzeo = 714)
    case 714:
    case 202:
        emit_load_gpr(e, W0, rA);
        emit_load_xer_ca(e, W1);
        emit_arm_carry_from_W(e, W1, W2);
        e.ADCS_W(W0, W0, A64_WZR);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // addme rD, rA  (rD = rA + 0xFFFFFFFF + XER.CA; addmeo = 746)
    case 746:
    case 234:
        emit_load_gpr(e, W0, rA);
        emit_load_xer_ca(e, W1);
        emit_arm_carry_from_W(e, W1, W2);
        e.MVN_W(W1, A64_WZR);           // W1 = 0xFFFFFFFF
        e.ADCS_W(W0, W0, W1);           // rA + 0xFFFFFFFF + CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // subf rD, rA, rB  (rD = rB - rA; subfo = 552)
    case 552:
    case 40:
        emit_load_gpr(e, W0, rB);
        emit_load_gpr(e, W1, rA);
        e.SUB_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // subfc rD, rA, rB  (rD = rB - rA, XER.CA = ~borrow = (rB>=rA); subfco = 520)
    case 520:
    case 8:
        emit_load_gpr(e, W0, rB);
        emit_load_gpr(e, W1, rA);
        e.SUBS_W(W0, W0, W1);           // ARM C = 1 if rB >= rA (no borrow) = PPC CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // subfe rD, rA, rB  (rD = ~rA + rB + XER.CA; subfeo = 648)
    // = rB - rA - NOT(XER.CA) in ARM terms (SBC with C=XER.CA)
    case 648:
    case 136:
        emit_load_gpr(e, W0, rB);
        emit_load_gpr(e, W1, rA);
        emit_load_xer_ca(e, W2);
        emit_arm_carry_from_W(e, W2, W3);
        e.SBCS_W(W0, W0, W1);           // W0 = rB - rA - NOT(C) = ~rA + rB + CA
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // subfze rD, rA  (rD = ~rA + 0 + XER.CA = 0 - rA - NOT(CA); subfzeo = 712)
    case 712:
    case 200:
        emit_load_gpr(e, W1, rA);
        emit_load_xer_ca(e, W2);
        emit_arm_carry_from_W(e, W2, W3);
        e.SBCS_W(W0, A64_WZR, W1);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // subfme rD, rA  (rD = ~rA + 0xFFFFFFFF + XER.CA; subfmeo = 744)
    // = 0xFFFFFFFF - rA - NOT(CA) via SBCS
    case 744:
    case 232:
        emit_load_gpr(e, W1, rA);
        emit_load_xer_ca(e, W2);
        emit_arm_carry_from_W(e, W2, W3);
        e.MVN_W(W0, A64_WZR);           // W0 = 0xFFFFFFFF
        e.SBCS_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        emit_update_xer_ca(e);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // neg rD, rA  (nego = 616: OE ignored)
    case 616:
    case 104:
        emit_load_gpr(e, W0, rA);
        e.NEG_W(W0, W0);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // or/mr rA, rS, rB  (note: PPC uses rD/rA/rB encoding, but or uses rS in rD field)
    case 444:
        if (rD == rB) {  // mr rA, rS: rS | rS = rS
            if (rc == 0 && rA == rD) return true;
            emit_load_gpr(e, W0, rD);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.ORR_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // xor rA, rS, rB
    case 316:
        if (rD == rB) {  // xor with self = 0
            e.MOV_W32(W0, 0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.EOR_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // and rA, rS, rB
    case 28:
        if (rD == rB) {  // and with self = identity (mr)
            if (rc == 0 && rA == rD) return true;
            emit_load_gpr(e, W0, rD);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.AND_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // nor rA, rS, rB
    case 124:
        if (rD == rB) {  // nor rA, rS, rS = ~rS
            emit_load_gpr(e, W0, rD);
            e.MVN_W(W0, W0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.ORR_W(W0, W0, W1);
        e.MVN_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // andc rA, rS, rB  (rA = rS & ~rB)
    case 60:
        if (rD == rB) {  // rS & ~rS = 0
            e.MOV_W32(W0, 0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.BIC_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // orc rA, rS, rB  (rA = rS | ~rB)
    case 412:
        if (rD == rB) {  // rS | ~rS = 0xFFFFFFFF
            e.MOV_W32(W0, 0xFFFFFFFFu);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.ORN_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // nand rA, rS, rB
    case 476:
        if (rD == rB) {  // ~(rS & rS) = ~rS
            emit_load_gpr(e, W0, rD);
            e.MVN_W(W0, W0);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.AND_W(W0, W0, W1);
        e.MVN_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // eqv rA, rS, rB  (XNOR: rA = ~(rS ^ rB))
    case 284:
        if (rD == rB) {  // ~(rS ^ rS) = ~0 = 0xFFFFFFFF
            e.MOV_W32(W0, 0xFFFFFFFFu);
            emit_store_gpr(e, W0, rA);
            if (rc) emit_set_cr0_from_W0(e);
            return true;
        }
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.EOR_W(W0, W0, W1);
        e.MVN_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // slw rA, rS, rB
    case 24:
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.AND_W(W1, W1, 31);    // PPC shift is mod 32 but only 5 bits matter
        e.LSL_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // srw rA, rS, rB
    case 536:
        emit_load_gpr(e, W0, rD);
        emit_load_gpr(e, W1, rB);
        e.AND_W(W1, W1, 31);
        e.LSR_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // sraw rA, rS, rB  (arithmetic shift right; also updates XER.CA)
    case 792: {
        emit_load_gpr(e, W0, rD);       // W0 = rS
        emit_load_gpr(e, W1, rB);       // W1 = rB
        e.AND_W(W1, W1, 63);            // W1 = sh = rB & 63

        // Result: for sh>=32, sign_ext(rS); for sh<32, ASRV(rS, sh)
        // ARM64 ASRV_W uses W1[4:0] (mod 32) — correct only for sh < 32
        e.ASR_W_IMM(W2, W0, 31);        // W2 = sign_ext(rS) [used when sh >= 32]
        e.ASR_W(W3, W0, W1);            // W3 = rS >> (sh & 31)
        e.CMP_W_IMM(W1, 32);            // compare sh vs 32
        e.CSEL_W(W2, W2, W3, A64_CS);  // result = (sh >= 32) ? sign_ext : shifted

        // XER.CA: lost = rS & mask; CA = (rS < 0) AND (lost != 0)
        // mask = (1<<sh)-1 for sh<32; all-ones for sh>=32
        e.MOV_W32(W3, 1);
        e.LSL_W(W3, W3, W1);            // W3 = 1 << (sh & 31) [mask helper]
        e.SUB_W_IMM(W3, W3, 1);         // W3 = (1<<sh)-1 [small-shift mask]
        e.MVN_W(W4, A64_WZR);           // W4 = 0xFFFFFFFF [large-shift mask]
        e.CMP_W_IMM(W1, 32);            // re-check sh >= 32
        e.CSEL_W(W3, W4, W3, A64_CS);  // W3 = (sh >= 32) ? 0xFFFFFFFF : mask
        e.AND_W(W3, W0, W3);            // W3 = rS & mask (bits shifted out)

        e.TST_W(W0, W0);                // set N if rS < 0
        e.CSET_W(W4, A64_MI);           // W4 = 1 if rS < 0
        e.TST_W(W3, W3);                // set Z if W3 == 0
        e.CSET_W(W3, A64_NE);           // W3 = 1 if lost bits != 0
        e.AND_W(W3, W3, W4);            // W3 = CA

        e.LDR_W(W4, PPC_PTR, OFF_XER);
        e.MOV_W32(W0, ~0x20000000U);    // W0 = ~XER_CA_MASK
        e.AND_W(W4, W4, W0);            // clear CA bit
        e.LSL_W_IMM(W3, W3, 29);        // W3 = CA << 29
        e.ORR_W(W4, W4, W3);            // set CA bit if needed
        e.STR_W(W4, PPC_PTR, OFF_XER);

        emit_store_gpr(e, W2, rA);
        if (rc) {
            e.MOV_W(W0, W2);
            emit_set_cr0_from_W0(e);
        }
        return true;
    }

    // srawi rA, rS, SH  (arithmetic shift right immediate; also updates XER.CA)
    case 824: {
        int sh = (op >> 11) & 0x1F;
        emit_load_gpr(e, W0, rD);   // W0 = rS (in rD field)
        if (sh == 0) {
            // No shift: result = rS, XER.CA = 0
            emit_store_gpr(e, W0, rA);
            // clear XER.CA
            e.LDR_W(W1, PPC_PTR, OFF_XER);
            e.MOV_W32(W2, ~0x20000000U);
            e.AND_W(W1, W1, W2);
            e.STR_W(W1, PPC_PTR, OFF_XER);
        } else {
            // XER.CA = (rS < 0) && ((rS & ((1<<sh)-1)) != 0)
            // Extract the bits that would be shifted out
            e.AND_W(W1, W0, (1u << sh) - 1u); // W1 = lost bits
            e.ASR_W_IMM(W0, W0, sh);           // W0 = result
            // CA = (original_rS < 0) && (lost != 0)
            // Use the original sign: bit 31 before shift is now bit (31-sh) in W0
            // Recover: original bit 31 = W0[31] (sign of result same as original)
            e.TST_W(W0, W0);                   // set N from result (sign preserved)
            e.CSET_W(W2, A64_MI);              // W2 = 1 if negative
            e.TST_W(W1, W1);
            e.CSET_W(W3, A64_NE);              // W3 = 1 if lost bits != 0
            e.AND_W(W3, W3, W2);               // W3 = CA
            emit_store_gpr(e, W0, rA);
            // Update XER.CA
            e.LDR_W(W1, PPC_PTR, OFF_XER);
            e.MOV_W32(W2, ~0x20000000U);
            e.AND_W(W1, W1, W2);
            e.LSL_W_IMM(W3, W3, 29);
            e.ORR_W(W1, W1, W3);
            e.STR_W(W1, PPC_PTR, OFF_XER);
        }
        if (rc) emit_set_cr0_from_W0(e);
        return true;
    }

    // extsh rA, rS  (sign extend halfword)
    case 922:
        emit_load_gpr(e, W0, rD);
        e.SXTH_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // extsb rA, rS  (sign extend byte)
    case 954:
        emit_load_gpr(e, W0, rD);
        e.SXTB_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // cntlzw rA, rS
    case 26:
        emit_load_gpr(e, W0, rD);
        e.CLZ_W(W0, W0);
        emit_store_gpr(e, W0, rA);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mulhwu rD, rA, rB  (unsigned multiply high: upper 32 bits of 64-bit product)
    case 11:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.UMULL_X(0, W0, W1);        // X0 = (uint64)rA * (uint64)rB
        e.LSR_X_IMM(0, 0, 32);       // X0 >>= 32 (upper 32 bits now in W0)
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mulhw rD, rA, rB  (signed multiply high)
    case 75:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.SMULL_X(0, W0, W1);        // X0 = (int64)rA * (int64)rB
        e.LSR_X_IMM(0, 0, 32);       // X0 >>= 32
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mullw rD, rA, rB  (mullwo = 747: OE ignored)
    case 747:
    case 235:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.MUL_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // divwu rD, rA, rB  (unsigned; divwuo = 971: OE ignored)
    case 971:
    case 459:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.UDIV_W(W0, W0, W1);          // ARM64: divide-by-zero gives 0 (PPC: undefined)
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // divw rD, rA, rB  (signed; divwo = 1003: OE ignored)
    case 1003:
    case 491:
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.SDIV_W(W0, W0, W1);
        emit_store_gpr(e, W0, rD);
        if (rc) emit_set_cr0_from_W0(e);
        return true;

    // mfcr rD: pack all 8 CR fields into a 32-bit value
    case 19: {
        e.LDRB(W0, PPC_PTR, OFF_CR + 0);
        for (int i = 1; i < 8; i++) {
            e.LSL_W_IMM(W0, W0, 4);
            e.LDRB(W1, PPC_PTR, OFF_CR + i);
            e.ORR_W(W0, W0, W1);
        }
        emit_store_gpr(e, W0, rD);
        return true;
    }

    // mtcrf CRM, rS: unpack 32-bit value into CR fields selected by CRM
    case 144: {
        int crm = (op >> 12) & 0xFF;
        emit_load_gpr(e, W0, rD);          // rS is in rD field
        for (int i = 0; i < 8; i++) {
            if (!(crm & (0x80 >> i))) continue;  // bit not set: skip this field
            // Extract nibble i from W0: bits [31-4i : 28-4i]
            e.UBFM_W(W1, W0, 28 - 4 * i, 31 - 4 * i);
            e.STRB(W1, PPC_PTR, OFF_CR + i);
        }
        return true;
    }

    // mfmsr rD  (move from machine state register)
    case 83:
        e.LDR_W(W0, PPC_PTR, OFF_MSR);
        emit_store_gpr(e, W0, rD);
        return true;

    // mfspr rD, SPR: inline common SPRs; fall back for others
    case 339: {
        int spr = ((op >> 6) & 0x3E0) | ((op >> 16) & 0x1F);
        switch (spr) {
        case 1:   e.LDR_W(W0, PPC_PTR, OFF_XER);           emit_store_gpr(e, W0, rD); return true;
        case 8:   e.LDR_W(W0, PPC_PTR, OFF_LR);            emit_store_gpr(e, W0, rD); return true;
        case 9:   e.LDR_W(W0, PPC_PTR, OFF_CTR);           emit_store_gpr(e, W0, rD); return true;
        case 22:  e.LDR_W(W0, PPC_PTR, OFF_DEC);           emit_store_gpr(e, W0, rD); return true;
        case 26:  e.LDR_W(W0, PPC_PTR, OFF_SRR0);          emit_store_gpr(e, W0, rD); return true;
        case 27:  e.LDR_W(W0, PPC_PTR, OFF_SRR1);          emit_store_gpr(e, W0, rD); return true;
        case 272: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 0);      emit_store_gpr(e, W0, rD); return true;
        case 273: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 4);      emit_store_gpr(e, W0, rD); return true;
        case 274: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 8);      emit_store_gpr(e, W0, rD); return true;
        case 275: e.LDR_W(W0, PPC_PTR, OFF_SPRG + 12);     emit_store_gpr(e, W0, rD); return true;
        case 268: emit_call(e, (uint64_t)(void *)&jit_read_tbl); emit_store_gpr(e, W0, rD); return true;
        case 269: emit_call(e, (uint64_t)(void *)&jit_read_tbu); emit_store_gpr(e, W0, rD); return true;
        default:  return false;
        }
    }

    // mftb rD, TBR  (read timebase: TBR=268→TBL, TBR=269→TBU)
    case 371: {
        int tbr = ((op >> 6) & 0x3E0) | ((op >> 16) & 0x1F);
        if (tbr == 268) {
            emit_call(e, (uint64_t)(void *)&jit_read_tbl);
            emit_store_gpr(e, W0, rD);
            return true;
        } else if (tbr == 269) {
            emit_call(e, (uint64_t)(void *)&jit_read_tbu);
            emit_store_gpr(e, W0, rD);
            return true;
        }
        return false;
    }

    // mtspr SPR, rS: inline common SPRs
    case 467: {
        int spr = ((op >> 6) & 0x3E0) | ((op >> 16) & 0x1F);
        switch (spr) {
        case 1:   emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_XER);       return true;
        case 8:   emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_LR);        return true;
        case 9:   emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_CTR);       return true;
        case 22:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_DEC);       return true;
        case 26:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SRR0);      return true;
        case 27:  emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SRR1);      return true;
        case 272: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 0);  return true;
        case 273: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 4);  return true;
        case 274: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 8);  return true;
        case 275: emit_load_gpr(e, W0, rD); e.STR_W(W0, PPC_PTR, OFF_SPRG + 12); return true;
        default:  return false;
        }
    }

    // mtmsr rS  (write machine state register)
    case 146:
        emit_load_gpr(e, W0, rD);   // rS is in rD field
        e.STR_W(W0, PPC_PTR, OFF_MSR);
        return true;

    // mcrxr crfD — copy XER[SO,OV,CA,0] to CR field crfD, then clear XER[31:28]
    case 512: {
        int crfD = (op >> 23) & 0x7;
        e.LDR_W(W0, PPC_PTR, OFF_XER);
        e.LSR_W_IMM(W1, W0, 28);                   // W1 = SO:OV:CA:0 (bits 3:0)
        e.STRB(W1, PPC_PTR, OFF_CR + crfD);
        e.MOV_W32(W2, ~0xF0000000U);               // mask off top nibble
        e.AND_W(W0, W0, W2);
        e.STR_W(W0, PPC_PTR, OFF_XER);
        return true;
    }

    // tw rA, rB / twi — trap: NOP (traps don't occur in normal game code)
    case 4:
        return true;

    // Cache and TLB management — all NOPs in emulator (no real cache/TLB to manage)
    case 54:    // dcbst
    case 86:    // dcbf
    case 246:   // dcbtst
    case 278:   // dcbt
    case 470:   // dcbi
    case 982:   // icbi
    case 306:   // tlbie
    case 370:   // tlbia
    case 566:   // tlbsync
        return true;

    // sync / lwsync: memory barrier — NOP in emulator
    case 598:
        return true;

    // eieio: enforce in-order execution of I/O — NOP in emulator
    case 854:
        return true;

    // dcbz: data cache block zero — NOP (no cache to zero in emulator)
    case 1014:
        return true;

    // ---- Indexed loads (X-form) ----
    // lwzx rD, rA, rB
    case 23:  return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read32, false);
    // lwzux rD, rA, rB
    case 55:  return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read32, false);
    // lbzx rD, rA, rB
    case 87:  return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read8, false);
    // lbzux rD, rA, rB
    case 119: return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read8, false);
    // lhzx rD, rA, rB
    case 279: return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read16, false);
    // lhzux rD, rA, rB
    case 311: return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read16, false);
    // lhax rD, rA, rB  (sign-extend)
    case 343: return translate_op31_load_x(e, rD, rA, rB, (void *)&jit_read16, true);
    // lhaux rD, rA, rB
    case 375: return translate_op31_load_xu(e, rD, rA, rB, (void *)&jit_read16, true);

    // ---- Indexed stores (X-form, rS is in rD field) ----
    // stwx rS, rA, rB
    case 151: return translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write32);
    // stwux rS, rA, rB
    case 183: return translate_op31_store_xu(e, rD, rA, rB, (void *)&jit_write32);
    // stbx rS, rA, rB
    case 215: return translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write8);
    // stbux rS, rA, rB
    case 247: return translate_op31_store_xu(e, rD, rA, rB, (void *)&jit_write8);
    // sthx rS, rA, rB
    case 407: return translate_op31_store_x(e, rD, rA, rB, (void *)&jit_write16);
    // sthux rS, rA, rB
    case 439: return translate_op31_store_xu(e, rD, rA, rB, (void *)&jit_write16);

    // ---- Indexed FP loads (X-form) ----
    // lfsx rD, rA, rB
    case 535: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_call(e, (uint64_t)(void *)&jit_read32);
        e.FMOV_S_W(D0, W0);
        e.FCVT_D_S(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;
    }
    // lfsux rD, rA, rB
    case 567: {
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        emit_call(e, (uint64_t)(void *)&jit_read32);
        e.FMOV_S_W(D0, W0);
        e.FCVT_D_S(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;
    }
    // lfdx rD, rA, rB
    case 599: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_call(e, (uint64_t)(void *)&jit_read64);
        e.FMOV_D_X(D0, 0);
        emit_store_fpr(e, D0, rD);
        return true;
    }
    // lfdux rD, rA, rB
    case 631: {
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        emit_call(e, (uint64_t)(void *)&jit_read64);
        e.FMOV_D_X(D0, 0);
        emit_store_fpr(e, D0, rD);
        return true;
    }

    // ---- Indexed FP stores (X-form, rS in rD field) ----
    // stfsx rS, rA, rB
    case 663: {
        emit_load_fpr(e, D0, rD);
        e.FCVT_S_D(D0, D0);
        emit_load_ea_reg(e, W0, rA, rB);  // may clobber W1; D0 is safe
        e.FMOV_W_S(W1, D0);
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }
    // stfsux rS, rA, rB
    case 695: {
        emit_load_fpr(e, D0, rD);
        e.FCVT_S_D(D0, D0);
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        e.FMOV_W_S(W1, D0);
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }
    // stfdx rS, rA, rB
    case 727: {
        emit_load_fpr(e, D0, rD);
        emit_load_ea_reg(e, W0, rA, rB);  // D0 safe; W0=EA
        e.FMOV_X_D(1, D0);               // X1 = raw double bits
        emit_call(e, (uint64_t)(void *)&jit_write64);
        return true;
    }
    // stfdux rS, rA, rB
    case 759: {
        emit_load_fpr(e, D0, rD);
        emit_load_gpr(e, W0, rA);
        emit_load_gpr(e, W1, rB);
        e.ADD_W(W0, W0, W1);
        emit_store_gpr(e, W0, rA);
        e.FMOV_X_D(1, D0);               // X1 = raw double bits
        emit_call(e, (uint64_t)(void *)&jit_write64);
        return true;
    }

    // stfiwx frS, rA, rB — store lower 32 bits of FPR as integer word (from fctiwz result)
    case 983: {
        // EA first (may clobber W1), then load data into W1
        emit_load_ea_reg(e, W0, rA, rB);
        e.LDR_W(W1, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }

    // ---- Byte-reverse indexed loads ----
    // lwbrx rD, rA, rB — load word byte-reversed
    case 534: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_call(e, (uint64_t)(void *)&jit_read32);
        e.REV_W(W0, W0);
        emit_store_gpr(e, W0, rD);
        return true;
    }
    // lhbrx rD, rA, rB — load halfword byte-reversed (zero-extend)
    case 790: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_call(e, (uint64_t)(void *)&jit_read16);
        e.REV16_W(W0, W0);                  // 0x0000AABB → 0x0000BBAA
        emit_store_gpr(e, W0, rD);
        return true;
    }

    // ---- Byte-reverse indexed stores ----
    // stwbrx rS, rA, rB — store word byte-reversed
    case 662: {
        emit_load_ea_reg(e, W0, rA, rB);    // EA (may clobber W1)
        emit_load_gpr(e, W1, rD);           // rD field = rS for stores
        e.REV_W(W1, W1);
        emit_call(e, (uint64_t)(void *)&jit_write32);
        return true;
    }
    // sthbrx rS, rA, rB — store halfword byte-reversed
    case 918: {
        emit_load_ea_reg(e, W0, rA, rB);
        emit_load_gpr(e, W1, rD);
        e.REV16_W(W1, W1);
        emit_call(e, (uint64_t)(void *)&jit_write16);
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Opcode-31 indexed load/store sub-operations (X-form)
// These are added inline to translate_op31; declared here as forward refs aren't needed.
// ---------------------------------------------------------------------------

// Helper for X-form indexed load: emit EA→W0, call reader, sign-ext if lha, store to rD
static bool translate_op31_load_x(Arm64Emitter &e, int rD, int rA, int rB,
                                   void *reader, bool sign_ext)
{
    emit_load_ea_reg(e, W0, rA, rB);
    emit_call(e, (uint64_t)reader);
    if (sign_ext) e.SXTH_W(W0, W0);
    emit_store_gpr(e, W0, rD);
    return true;
}

// Helper for X-form indexed load with update: ea=rA+rB, rA=ea, load, store to rD
static bool translate_op31_load_xu(Arm64Emitter &e, int rD, int rA, int rB,
                                    void *reader, bool sign_ext)
{
    emit_load_gpr(e, W0, rA);
    emit_load_gpr(e, W1, rB);
    e.ADD_W(W0, W0, W1);                // W0 = EA
    emit_store_gpr(e, W0, rA);          // rA = EA
    emit_call(e, (uint64_t)reader);
    if (sign_ext) e.SXTH_W(W0, W0);
    emit_store_gpr(e, W0, rD);
    return true;
}

// Helper for X-form indexed store: EA→W0, data→W1, call writer
static bool translate_op31_store_x(Arm64Emitter &e, int rS, int rA, int rB, void *writer)
{
    emit_load_ea_reg(e, W0, rA, rB);    // W0 = EA (uses W1 as scratch)
    emit_load_gpr(e, W1, rS);           // W1 = data (after scratch use is done)
    emit_call(e, (uint64_t)writer);
    return true;
}

// Helper for X-form indexed store with update: rS loaded first to handle rS==rA
static bool translate_op31_store_xu(Arm64Emitter &e, int rS, int rA, int rB, void *writer)
{
    emit_load_gpr(e, W2, rS);           // W2 = data (save before potential rA overwrite)
    emit_load_gpr(e, W0, rA);
    emit_load_gpr(e, W1, rB);
    e.ADD_W(W0, W0, W1);                // W0 = EA
    emit_store_gpr(e, W0, rA);          // rA = EA
    e.MOV_W(W1, W2);                    // W1 = data
    emit_call(e, (uint64_t)writer);
    return true;
}

// ---------------------------------------------------------------------------
// Branch translation helpers
// These always set npc and return true (they terminate the block).
// ---------------------------------------------------------------------------

// Primary opcode 16: bc BO, BI, BD [, AA] [, LK]  — fully inlined
// Returns true if block is terminated (always for bc).
// taken_fn / not_taken_fn: if non-null, use chained epilogue for that path.
static bool translate_bc(Arm64Emitter &e, uint32_t op, uint32_t pc, int inst_count,
                          void *taken_fn = nullptr, void *not_taken_fn = nullptr)
{
    int bo = (op >> 21) & 0x1F;
    int bi = (op >> 16) & 0x1F;
    int aa = (op >>  1) & 1;
    int lk = op & 1;
    int32_t bd = (int32_t)((op & 0xFFFC) << 16) >> 16;  // sign-extend 14-bit field
    uint32_t taken_target = aa ? (uint32_t)bd : (pc + (uint32_t)bd);

    bool ctr_relevant  = !(bo & 0x04);
    bool cond_relevant = !(bo & 0x10);
    bool ctr_zero      = !(bo & 0x02);   // true → branch if CTR==0 after decrement
    bool cond_on_clear = !(bo & 0x08);   // true → branch if CR bit CLEAR (bo1=0); false → SET

    // W0 = running "taken" flag (1=taken, 0=not taken)
    e.MOV_W32(W0, 1);

    if (ctr_relevant) {
        e.LDR_W(W1, PPC_PTR, OFF_CTR);
        e.SUBS_W_IMM(W1, W1, 1);                           // decrement + set flags
        e.STR_W(W1, PPC_PTR, OFF_CTR);
        e.CSET_W(W2, ctr_zero ? A64_EQ : A64_NE);
        e.AND_W(W0, W0, W2);
    }

    if (cond_relevant) {
        int crfD  = bi / 4;
        int crbit = bi % 4;                                 // 0=LT,1=GT,2=EQ,3=SO
        e.LDRB(W1, PPC_PTR, OFF_CR + crfD);
        e.UBFM_W(W1, W1, 3 - crbit, 3 - crbit);           // extract bit → W1[0]
        e.CMP_W_IMM(W1, 0);
        // cond_on_clear=true → branch when bit=0 → W2=1 when EQ; false → branch when bit=1 → NE
        e.CSET_W(W2, cond_on_clear ? A64_EQ : A64_NE);
        e.AND_W(W0, W0, W2);
    }

    if (lk) {
        e.MOV_W32(W1, pc + 4);
        e.STR_W(W1, PPC_PTR, OFF_LR);
    }

    // CBZ W0 → not-taken path
    uint32_t *not_taken_patch = e.emit_CBZ_W_placeholder(W0);

    // Taken path
    if (taken_fn)
        emit_epilogue_chained(e, inst_count + 1, pc, taken_target, taken_fn);
    else
        emit_epilogue(e, inst_count + 1, pc, taken_target);

    // Not-taken path (patch CBZ here)
    e.patch_CBZ(not_taken_patch, e.ptr());
    if (not_taken_fn)
        emit_epilogue_chained(e, inst_count + 1, pc, pc + 4, not_taken_fn);
    else
        emit_epilogue(e, inst_count + 1, pc, pc + 4);

    return true;
}

// Primary opcode 18: b BD [, AA] [, LK]
// Returns the branch target; emits LR update if LK.
static uint32_t translate_b(Arm64Emitter &e, uint32_t op, uint32_t pc)
{
    int aa = (op >> 1) & 1;    // absolute address
    int lk = op & 1;            // link (bl)
    int32_t bd = (int32_t)((op & 0x03FFFFFC) << 6) >> 6;  // sign-extend 26-bit

    uint32_t target = aa ? (uint32_t)bd : (pc + (uint32_t)bd);

    if (lk) {
        e.MOV_W32(W0, pc + 4);
        e.STR_W(W0, PPC_PTR, OFF_LR);
    }
    return target;
}

// bclr: primary 19, subop 16 (branch to LR)
// bcctr: primary 19, subop 528 (branch to CTR)
// These always terminate the block.

// ---------------------------------------------------------------------------
// Additional primary-opcode translators
// ---------------------------------------------------------------------------

// Shared helper: compute EA = (rA==0 ? 0 : REG(rA)) + offset32  → W0
static void emit_load_ea_off32(Arm64Emitter &e, int rA, int32_t off)
{
    if (rA == 0) {
        e.MOV_W32(W0, (uint32_t)off);
    } else {
        emit_load_gpr(e, W0, rA);
        if      (off == 0)                          { /* nothing */ }
        else if (off > 0 && off <= 4095)            e.ADD_W_IMM(W0, W0, (uint32_t)off);
        else if (off < 0 && -off <= 4095)           e.SUB_W_IMM(W0, W0, (uint32_t)(-off));
        else { e.MOV_W32(W1, (uint32_t)off); e.ADD_W(W0, W0, W1); }
    }
}

// Primary opcode 8: subfic rD, rA, SIMM  (rD = SIMM - rA, XER.CA = (SIMM >= rA unsigned))
static bool translate_subfic(Arm64Emitter &e, uint32_t op)
{
    int rD = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    e.MOV_W32(W1, (uint32_t)(int32_t)simm);
    e.SUBS_W(W0, W1, W0);              // W0 = SIMM - rA; ARM C = 1 if SIMM >= rA (unsigned) = CA
    emit_store_gpr(e, W0, rD);
    emit_update_xer_ca(e);
    return true;
}

// Primary opcodes 12/13: addic[.] rD, rA, SIMM  (rD = rA+SIMM, XER.CA = carry out)
static bool translate_addic(Arm64Emitter &e, uint32_t op, bool update_cr)
{
    int rD = (op >> 21) & 0x1F;
    int rA = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    emit_load_gpr(e, W0, rA);
    if (simm >= 0 && (uint32_t)simm <= 4095) {
        e.ADDS_W_IMM(W0, W0, (uint32_t)simm);
    } else if (simm < 0 && (uint32_t)(-simm) <= 4095) {
        e.SUBS_W_IMM(W0, W0, (uint32_t)(-simm));   // rA + simm = rA - |simm|; C = NOT borrow = CA
    } else {
        e.MOV_W32(W1, (uint32_t)(int32_t)simm);
        e.ADDS_W(W0, W0, W1);
    }
    emit_store_gpr(e, W0, rD);
    emit_update_xer_ca(e);
    if (update_cr) emit_set_cr0_from_W0(e);
    return true;
}

// Primary opcode 46: lmw rD, d(rA)  — load registers rD..r31 from consecutive memory
static bool translate_lmw(Arm64Emitter &e, uint32_t op)
{
    int rD   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    int n    = 32 - rD;

    for (int i = 0; i < n; i++) {
        emit_load_ea_off32(e, rA, (int32_t)simm + i * 4);  // W0 = EA
        emit_call(e, (uint64_t)(void *)&jit_read32);
        emit_store_gpr(e, W0, rD + i);
    }
    return true;
}

// Primary opcode 47: stmw rS, d(rA)  — store registers rS..r31 to consecutive memory
static bool translate_stmw(Arm64Emitter &e, uint32_t op)
{
    int rS   = (op >> 21) & 0x1F;
    int rA   = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);
    int n    = 32 - rS;

    for (int i = 0; i < n; i++) {
        emit_load_ea_off32(e, rA, (int32_t)simm + i * 4);  // W0 = EA
        emit_load_gpr(e, W1, rS + i);                       // W1 = data
        emit_call(e, (uint64_t)(void *)&jit_write32);
    }
    return true;
}

// ---------------------------------------------------------------------------
// FP load/store helpers: EA in W0 on entry; may clobber W0-W4, D0-D3
// ---------------------------------------------------------------------------

// Compute EA (D-form) into W0.  rA==0 → EA = simm16 only.
// (Reuses emit_load_ea_imm which is already defined above)

// lfs  rD, simm(rA): load float, convert to double, store in FPR
static bool translate_lfs(Arm64Emitter &e, uint32_t op, bool update)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;  // lfsu rA=0 is invalid

    emit_load_ea_imm(e, rA, simm);   // W0 = EA

    if (update) {
        emit_store_gpr(e, W0, rA);   // rA = EA (write-back)
    }
    emit_call(e, (uint64_t)(void *)&jit_read32);  // W0 = float bits
    e.FMOV_S_W(D0, W0);              // D0(S0) = reinterpret as float
    e.FCVT_D_S(D0, D0);              // D0 = (double)float
    emit_store_fpr(e, D0, rD);
    return true;
}

// lfd  rD, simm(rA): load 64-bit double directly into FPR
static bool translate_lfd(Arm64Emitter &e, uint32_t op, bool update)
{
    int rD    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;

    emit_load_ea_imm(e, rA, simm);   // W0 = EA
    if (update) {
        emit_store_gpr(e, W0, rA);
    }
    // Store EA for the 64-bit read call (W0 = addr)
    emit_call(e, (uint64_t)(void *)&jit_read64);  // X0 = 64-bit data
    // jit_read64 returns UINT64 in X0; move bits into FP register
    e.FMOV_D_X(D0, 0);               // D0 = reinterpret X0 as double
    emit_store_fpr(e, D0, rD);
    return true;
}

// stfs rS, simm(rA): convert FPR double to float, store 32-bit float
static bool translate_stfs(Arm64Emitter &e, uint32_t op, bool update)
{
    int rS    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;

    // Load FPR and convert to single-precision first (kept in D0)
    emit_load_fpr(e, D0, rS);        // D0 = FPR[rS] (double)
    e.FCVT_S_D(D0, D0);              // S0/D0 = (float)D0 — round to single

    // Compute EA into W0 (emit_load_ea_imm may clobber W1 for large offsets)
    emit_load_ea_imm(e, rA, simm);   // W0 = EA
    if (update) {
        emit_store_gpr(e, W0, rA);
    }

    // Now move single-precision bits into W1 (D0 not touched by emit_load_ea_imm)
    e.FMOV_W_S(W1, D0);              // W1 = float bits
    emit_call(e, (uint64_t)(void *)&jit_write32);
    return true;
}

// stfd rS, simm(rA): store 64-bit double
static bool translate_stfd(Arm64Emitter &e, uint32_t op, bool update)
{
    int rS    = (op >> 21) & 0x1F;
    int rA    = (op >> 16) & 0x1F;
    int16_t simm = (int16_t)(op & 0xFFFF);

    if (update && rA == 0) return false;

    // Load FPR into D0 first (kept there while we compute EA into W0)
    emit_load_fpr(e, D0, rS);        // D0 = FPR[rS]

    // Compute EA into W0 (may clobber W1, but D0 is unaffected)
    emit_load_ea_imm(e, rA, simm);   // W0 = EA
    if (update) {
        emit_store_gpr(e, W0, rA);
    }

    // Move 64-bit FPR bits into X1 (second arg); X0 already has EA from W0
    e.FMOV_X_D(1, D0);               // X1 = raw double bits
    emit_call(e, (uint64_t)(void *)&jit_write64);
    return true;
}

// ---------------------------------------------------------------------------
// Opcode 63: floating-point double-precision arithmetic
// ---------------------------------------------------------------------------
static bool translate_op63(Arm64Emitter &e, uint32_t op)
{
    int rD  = (op >> 21) & 0x1F;
    int rA  = (op >> 16) & 0x1F;
    int rB  = (op >> 11) & 0x1F;
    int rC  = (op >> 6)  & 0x1F;  // used by fmadd/fmsub family
    int sub = (op >> 1) & 0x3FF;
    (void)rA;  // some sub-ops don't use rA

    switch (sub) {
    case 72:  // fmr rD, rB  (copy FPR)
        emit_load_fpr(e, D0, rB);
        emit_store_fpr(e, D0, rD);
        return true;

    case 40:  // fneg rD, rB
        emit_load_fpr(e, D0, rB);
        e.FNEG_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 264: // fabs rD, rB
        emit_load_fpr(e, D0, rB);
        e.FABS_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 136: // fnabs rD, rB  (force-negative abs)
        emit_load_fpr(e, D0, rB);
        e.FABS_D(D0, D0);
        e.FNEG_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 12:  // frsp rD, rB  (round to single precision)
        emit_load_fpr(e, D0, rB);
        e.FCVT_S_D(D0, D0);   // round to single
        e.FCVT_D_S(D0, D0);   // extend back to double
        emit_store_fpr(e, D0, rD);
        return true;

    case 14:  // fctiw rD, rB  (round to integer using FPSCR rounding mode)
    // We ignore FPSCR rounding mode and truncate (same as fctiwz) — acceptable for game code
    case 15:  // fctiwz rD, rB  (convert double to int32, sign-extend to 64 bits in FPR)
    {
        // PPC stores result as sign-extended int64 in FPR.id
        emit_load_fpr(e, D0, rB);
        e.FCVTZS_W_D(W0, D0);           // W0 = (int32_t)D0
        e.ASR_W_IMM(W1, W0, 31);        // W1 = sign extension (0 or 0xFFFFFFFF)
        e.STR_W(W0, PPC_PTR, OFF_FPR + rD * 8);      // lower 32 bits
        e.STR_W(W1, PPC_PTR, OFF_FPR + rD * 8 + 4);  // upper 32 bits (sign ext)
        return true;
    }

    case 21:  // fadd rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FADD_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        return true;

    case 20:  // fsub rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FSUB_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        return true;

    case 25:  // fmul rD, rA, rC  (note: uses rC not rB!)
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        e.FMUL_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        return true;

    case 18:  // fdiv rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FDIV_D(D0, D0, D1);
        emit_store_fpr(e, D0, rD);
        return true;

    case 22:  // fsqrt rD, rB
        emit_load_fpr(e, D0, rB);
        e.FSQRT_D(D0, D0);
        emit_store_fpr(e, D0, rD);
        return true;

    case 29:  // fmadd rD, rA, rC, rB  (rD = rA*rC + rB)
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FMADD_D(D0, D0, D1, D2);   // D0 = D2 + D0*D1 = rB + rA*rC
        emit_store_fpr(e, D0, rD);
        return true;

    case 28:  // fmsub rD, rA, rC, rB  (rD = rA*rC - rB)
        // ARM FNMSUB: -(Da - Dn*Dm) = Dn*Dm - Da = rA*rC - rB ✓
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FNMSUB_D(D0, D0, D1, D2);  // D0*D1 - D2 = rA*rC - rB
        emit_store_fpr(e, D0, rD);
        return true;

    case 31:  // fnmadd rD, rA, rC, rB  (rD = -(rA*rC + rB))
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FNMADD_D(D0, D0, D1, D2);  // -(D2 + D0*D1) = -(rB + rA*rC) ✓
        emit_store_fpr(e, D0, rD);
        return true;

    case 30:  // fnmsub rD, rA, rC, rB  (rD = -(rA*rC - rB) = rB - rA*rC)
        // ARM FMSUB: Da - Dn*Dm = rB - rA*rC ✓
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FMSUB_D(D0, D0, D1, D2);   // D2 - D0*D1 = rB - rA*rC
        emit_store_fpr(e, D0, rD);
        return true;

    case 23: {// fsel frD, frA, frC, frB  (frD = frA >= 0.0 ? frC : frB; NaN → frB)
        // Load frA first; FCMP_D_ZERO sets GE=true when frA >= 0 and frA is not NaN
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FCMP_D_ZERO(D0);
        e.FCSEL_D(D0, D1, D2, A64_GE);   // D0 = (GE) ? frC : frB
        emit_store_fpr(e, D0, rD);
        return true;
    }

    case 26: {  // frsqrte frD, frB — reciprocal square root estimate
        emit_load_fpr(e, D0, rB);
        emit_call(e, (uint64_t)(void *)&jit_frsqrte);
        emit_store_fpr(e, D0, rD);
        return true;
    }

    case 583: {  // mffs frD — move FPSCR to FPR (lower 32 bits)
        e.LDR_W(W0, PPC_PTR, OFF_FPSCR);
        e.STR_W(W0, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8));
        e.MOV_W32(W1, 0);
        e.STR_W(W1, PPC_PTR, (uint32_t)(OFF_FPR + rD * 8 + 4));
        return true;
    }

    case 711:   // mtfsf FLM, frB — update FPSCR fields from FPR (NOP: JIT doesn't model FPSCR)
    case 134:   // mtfsfi crfD, IMM — set FPSCR field from immediate (NOP)
    case 70:    // mtfsb0 crbD — clear FPSCR bit (NOP)
    case 38:    // mtfsb1 crbD — set FPSCR bit (NOP)
        return true;

    case 64: {  // mcrfs crfD, crfS — copy FPSCR field to CR field
        int crfD = (op >> 23) & 0x7;
        int crfS = (op >> 18) & 0x7;
        e.LDR_W(W0, PPC_PTR, OFF_FPSCR);
        e.LSR_W_IMM(W1, W0, 28 - crfS * 4);
        e.MOV_W32(W2, 0xF);
        e.AND_W(W1, W1, W2);
        e.STRB(W1, PPC_PTR, OFF_CR + crfD);
        return true;
    }

    case 32:  // fcmpo crD, rA, rB  (ordered FP compare)
    case 0:   // fcmpu crD, rA, rB  (unordered FP compare)
    {
        int crfD = (op >> 23) & 0x7;
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FCMP_D(D0, D1);            // sets NZCV: N=LT, Z=EQ, C=!LT (ordered), V=unordered
        // Build PPC CR field: bit3=LT, bit2=GT, bit1=EQ, bit0=FU (unordered)
        e.CSET_W(W0, A64_MI);        // W0 = 1 if LT (N flag)
        e.CSET_W(W1, A64_GT);        // W1 = 1 if GT
        e.CSET_W(W2, A64_EQ);        // W2 = 1 if EQ (Z flag)
        e.CSET_W(W3, A64_VS);        // W3 = 1 if unordered (V flag)
        e.LSL_W_IMM(W0, W0, 3);
        e.ORR_W_LSL(W0, W0, W1, 2);
        e.ORR_W_LSL(W0, W0, W2, 1);
        e.ORR_W(W0, W0, W3);
        e.STRB(W0, PPC_PTR, OFF_CR + crfD);
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Opcode 59: floating-point single-precision arithmetic
// Same as op63 but results are rounded to single precision after each op.
// ---------------------------------------------------------------------------
static bool translate_op59(Arm64Emitter &e, uint32_t op)
{
    int rD  = (op >> 21) & 0x1F;
    int rA  = (op >> 16) & 0x1F;
    int rB  = (op >> 11) & 0x1F;
    int rC  = (op >> 6)  & 0x1F;
    int sub = (op >> 1) & 0x3FF;

    // Helper macro pattern: compute in double, round to single, convert back
#define STORE_SP(Dd, ppc_fpr) \
    do { e.FCVT_S_D(Dd, Dd); e.FCVT_D_S(Dd, Dd); emit_store_fpr(e, Dd, ppc_fpr); } while(0)

    switch (sub) {
    case 21:  // fadds rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FADD_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 20:  // fsubs rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FSUB_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 25:  // fmuls rD, rA, rC
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        e.FMUL_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 18:  // fdivs rD, rA, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rB);
        e.FDIV_D(D0, D0, D1);
        STORE_SP(D0, rD);
        return true;

    case 22:  // fsqrts rD, rB
        emit_load_fpr(e, D0, rB);
        e.FSQRT_D(D0, D0);
        STORE_SP(D0, rD);
        return true;

    case 29:  // fmadds rD, rA, rC, rB
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FMADD_D(D0, D0, D1, D2);
        STORE_SP(D0, rD);
        return true;

    case 28:  // fmsubs rD, rA, rC, rB  (rA*rC - rB)
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FNMSUB_D(D0, D0, D1, D2);  // D0*D1 - D2 = rA*rC - rB
        STORE_SP(D0, rD);
        return true;

    case 31:  // fnmadds rD, rA, rC, rB  (-(rA*rC + rB))
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FNMADD_D(D0, D0, D1, D2);
        STORE_SP(D0, rD);
        return true;

    case 30:  // fnmsubs rD, rA, rC, rB  (rB - rA*rC)
        emit_load_fpr(e, D0, rA);
        emit_load_fpr(e, D1, rC);
        emit_load_fpr(e, D2, rB);
        e.FMSUB_D(D0, D0, D1, D2);   // D2 - D0*D1 = rB - rA*rC
        STORE_SP(D0, rD);
        return true;

    case 24: {  // fres frD, frB — reciprocal estimate (single precision)
        emit_load_fpr(e, D0, rB);
        emit_call(e, (uint64_t)(void *)&jit_fres);
        emit_store_fpr(e, D0, rD);
        return true;
    }

    default:
        return false;
    }
#undef STORE_SP
}

// ---------------------------------------------------------------------------
// Main compilation function
// ---------------------------------------------------------------------------

JitBlock *JitArm64::compile(uint32_t start_pc)
{
    jit_sync_fetch(start_pc);   // ensure ppc.cur_fetch is valid for ppc_read_opcode_at
    compute_offsets();

    // Check available buffer space (rough estimate: 128 instructions * 32 ARM ops each)
    if (m_code_pos + 128 * 32 * 4 > CODE_BUF_SIZE) {
        // Buffer nearly full: evict all blocks and reuse from start
        flush();
    }

    uint32_t *write_base = (uint32_t *)(m_write_buf + m_code_pos);
    size_t cap = (CODE_BUF_SIZE - m_code_pos) / 4;
    Arm64Emitter e(write_base, cap);

    // -----------------------------------------------------------------------
    // Prologue — X0 = PPC_REGS* passed by caller (saves 2-4 instructions vs MOV_X64)
    // -----------------------------------------------------------------------
    e.STP_pre(PPC_PTR, 30, A64_SP, -16);   // save X19 (ppc ptr) and X30 (LR)
    e.MOV_X(PPC_PTR, 0);                    // X19 = X0 (PPC_REGS* argument)

    // -----------------------------------------------------------------------
    // Block body
    // -----------------------------------------------------------------------
    uint32_t pc = start_pc;
    int inst_count = 0;
    bool terminated = false;
    uint32_t exit_npc = start_pc;   // fallthrough target

    while (inst_count < MAX_BLOCK_INSTS && !terminated && !e.full()) {
        uint32_t op = ppc_read_opcode_at(pc);
        if (op == 0) {
            // Opcode 0 = illegal / unreadable; bail
            break;
        }

        int primary = op >> 26;
        bool handled = false;

        switch (primary) {
        case  3: handled = true; break;  // twi — trap word immediate, NOP in emulator
        case  7: handled = translate_mulli(e, op);      break;
        case  8: handled = translate_subfic(e, op);     break;
        case 10: handled = translate_cmpli(e, op);      break;
        case 11: handled = translate_cmpi(e, op);       break;
        case 12: handled = translate_addic(e, op, false); break;
        case 13: handled = translate_addic(e, op, true);  break;
        case 14: handled = translate_addi(e, op);       break;
        case 15: handled = translate_addis(e, op);      break;
        case 20: handled = translate_rlwimi(e, op);     break;
        case 21: handled = translate_rlwinm(e, op);     break;
        case 23: handled = translate_rlwnm(e, op);      break;
        case 24: handled = translate_ori(e, op);        break;
        case 25: handled = translate_oris(e, op);       break;
        case 26: handled = translate_xori(e, op);       break;
        case 27: handled = translate_xoris(e, op);      break;
        case 28: handled = translate_andi_dot(e, op);   break;
        case 29: handled = translate_andis_dot(e, op);  break;
        case 31: handled = translate_op31(e, op);       break;

        // D-form loads
        case 32: case 33: case 34: case 35:
        case 40: case 41: case 42: case 43:
            handled = translate_load_imm(e, op, primary);
            break;

        // D-form stores
        case 36: case 37: case 38: case 39:
        case 44: case 45:
            handled = translate_store_imm(e, op, primary);
            break;

        // Load/store multiple
        case 46: handled = translate_lmw(e, op);  break;
        case 47: handled = translate_stmw(e, op); break;

        // FP D-form loads
        case 48: handled = translate_lfs(e, op, false);  break;  // lfs
        case 49: handled = translate_lfs(e, op, true);   break;  // lfsu
        case 50: handled = translate_lfd(e, op, false);  break;  // lfd
        case 51: handled = translate_lfd(e, op, true);   break;  // lfdu

        // FP D-form stores
        case 52: handled = translate_stfs(e, op, false); break;  // stfs
        case 53: handled = translate_stfs(e, op, true);  break;  // stfsu
        case 54: handled = translate_stfd(e, op, false); break;  // stfd
        case 55: handled = translate_stfd(e, op, true);  break;  // stfdu

        // FP arithmetic
        case 59: handled = translate_op59(e, op); break;
        case 63: handled = translate_op63(e, op); break;

        case 17: {
            // sc — system call: interpreter sets ppc.npc to exception handler
            emit_set_pc_npc(e, pc);
            e.MOV_W32(W0, op);
            emit_call(e, (uint64_t)(void *)&ppc_dispatch_opcode);
            // NPC is now whatever the sc handler set; read it and return
            e.LDR_W(W3, PPC_PTR, OFF_NPC);
            emit_epilogue_npc_reg(e, inst_count + 1, pc, W3);
            terminated = true;
            handled    = true;
            break;
        }

        case 16: {
            // bc: fully inlined conditional branch — always terminates the block.
            // Look up taken/not-taken targets for block chaining.
            {
                int32_t bd = (int32_t)((op & 0xFFFC) << 16) >> 16;
                int aa16   = (op >> 1) & 1;
                uint32_t taken_target   = aa16 ? (uint32_t)bd : (pc + (uint32_t)bd);
                uint32_t not_taken_target = pc + 4;
                void *taken_fn     = nullptr;
                void *not_taken_fn = nullptr;
                auto it = m_cache.find(taken_target);
                if (it != m_cache.end()) taken_fn = (void *)it->second.fn;
                it = m_cache.find(not_taken_target);
                if (it != m_cache.end()) not_taken_fn = (void *)it->second.fn;
                handled = translate_bc(e, op, pc, inst_count, taken_fn, not_taken_fn);
            }
            terminated = true;
            break;
        }

        case 18: {
            // b / bl / ba / bla.  Chain non-linking branches to already-compiled targets.
            uint32_t target = translate_b(e, op, pc);
            int lk18 = op & 1;
            void *target_fn = nullptr;
            if (!lk18) {
                auto it = m_cache.find(target);
                if (it != m_cache.end()) target_fn = (void *)it->second.fn;
            }
            if (target_fn)
                emit_epilogue_chained(e, inst_count + 1, pc, target, target_fn);
            else
                emit_epilogue(e, inst_count + 1, pc, target);
            terminated = true;
            handled = true;
            break;
        }

        case 19: {
            // CR logical ops, bclr, bcctr, isync
            int subop = (op >> 1) & 0x3FF;
            int crBD = (op >> 21) & 0x1F;
            int crBA = (op >> 16) & 0x1F;
            int crBB = (op >> 11) & 0x1F;
            switch (subop) {
            case 33:   // crnor   crBD = !(crBA | crBB)
            case 129:  // crandc  crBD = crBA & ~crBB
            case 193:  // crxor   crBD = crBA ^ crBB
            case 225:  // crnand  crBD = !(crBA & crBB)
            case 257:  // crand   crBD = crBA & crBB
            case 289:  // creqv   crBD = !(crBA ^ crBB)
            case 417:  // crorc   crBD = crBA | ~crBB
            case 449: {// cror    crBD = crBA | crBB
                emit_load_cr_bit(e, W0, crBA);
                emit_load_cr_bit(e, W1, crBB);
                switch (subop) {
                case  33: e.ORR_W(W0, W0, W1);  e.MVN_W(W0, W0); e.AND_W(W0, W0, 1); break; // NOR
                case 129: e.MVN_W(W1, W1); e.AND_W(W0, W0, 1); e.AND_W(W1, W1, 1); e.AND_W(W0, W0, W1); break; // ANDC
                case 193: e.EOR_W(W0, W0, W1); break;
                case 225: e.AND_W(W0, W0, W1);  e.MVN_W(W0, W0); e.AND_W(W0, W0, 1); break; // NAND
                case 257: e.AND_W(W0, W0, W1); break;
                case 289: e.EOR_W(W0, W0, W1);  e.MVN_W(W0, W0); e.AND_W(W0, W0, 1); break; // EQV
                case 417: e.MVN_W(W1, W1); e.AND_W(W1, W1, 1); e.ORR_W(W0, W0, W1); break; // ORC
                case 449: e.ORR_W(W0, W0, W1); break;
                default: break;
                }
                emit_store_cr_bit(e, W0, W2, crBD);
                handled = true;
                break;
            }
            case 150:
                // isync — NOP
                handled = true;
                break;
            case 50: {  // rfi — return from interrupt (sets PC/MSR from SRR0/SRR1)
                emit_set_pc_npc(e, pc);
                e.MOV_W32(W0, op);
                emit_call(e, (uint64_t)(void *)&ppc_dispatch_opcode);
                e.LDR_W(W3, PPC_PTR, OFF_NPC);
                emit_epilogue_npc_reg(e, inst_count + 1, pc, W3);
                terminated = true;
                handled    = true;
                break;
            }
            case 0: {   // mcrf crfD, crfS — copy one CR field to another
                int crfD = (op >> 23) & 0x7;
                int crfS = (op >> 18) & 0x7;
                e.LDRB(W0, PPC_PTR, OFF_CR + crfS);
                e.STRB(W0, PPC_PTR, OFF_CR + crfD);
                handled = true;
                break;
            }
            default:
                break;
            }
            if (!handled && (subop == 16 || subop == 528)) {
                // bclr (16) / bcctr (528): branch to LR / CTR, optionally conditional
                int bo = (op >> 21) & 0x1F;
                int bi = (op >> 16) & 0x1F;
                int lk = op & 1;
                bool is_ctr       = (subop == 528);
                bool ctr_relevant = !(bo & 0x04);  // decrement + check CTR
                bool cond_relevant= !(bo & 0x10);  // check CR bit
                bool ctr_zero     = !(bo & 0x02);  // branch when CTR==0 (vs !=0)
                bool cond_on_clear= !(bo & 0x08);  // branch when CR bit CLEAR (vs SET)

                // W3 = target (LR or CTR); load early before LK might overwrite LR
                e.LDR_W(W3, PPC_PTR, is_ctr ? OFF_CTR : OFF_LR);

                // W0 = running "taken" flag (1=taken, 0=not taken)
                e.MOV_W32(W0, 1);

                if (ctr_relevant) {
                    e.LDR_W(W1, PPC_PTR, OFF_CTR);
                    e.SUBS_W_IMM(W1, W1, 1);
                    e.STR_W(W1, PPC_PTR, OFF_CTR);
                    e.CSET_W(W2, ctr_zero ? A64_EQ : A64_NE);
                    e.AND_W(W0, W0, W2);
                    // bcctr: target is post-decrement CTR
                    if (is_ctr) e.MOV_W(W3, W1);
                }

                if (cond_relevant) {
                    int crfD  = bi / 4;
                    int crbit = bi % 4;
                    e.LDRB(W1, PPC_PTR, OFF_CR + crfD);
                    e.UBFM_W(W1, W1, 3 - crbit, 3 - crbit);
                    e.CMP_W_IMM(W1, 0);
                    e.CSET_W(W2, cond_on_clear ? A64_EQ : A64_NE);
                    e.AND_W(W0, W0, W2);
                }

                // LK: if taken or unconditional, save PC+4 to LR
                if (lk) {
                    e.MOV_W32(W1, pc + 4);
                    e.STR_W(W1, PPC_PTR, OFF_LR);
                }

                // Mask target to 4-byte boundary (matches interpreter)
                e.MOV_W32(W4, 3);
                e.BIC_W(W3, W3, W4);

                // Select NPC: W3 = taken target, W4 = fall-through
                e.MOV_W32(W4, pc + 4);
                // CMP W0, 0 → CSEL W3, W3, W4, NE  (W3 = taken ? LR/CTR : pc+4)
                e.CMP_W_IMM(W0, 0);
                e.CSEL_W(W3, W3, W4, A64_NE);

                emit_epilogue_npc_reg(e, inst_count + 1, pc, W3);
                terminated = true;
                handled    = true;
            }
            break;
        }  // case 19

        default:
            break;
        }

        if (!handled) {
            // Fallback: call interpreter for this instruction
            emit_fallback(e, op, pc);
        }

        inst_count++;
        exit_npc = pc + 4;
        pc += 4;
    }

    if (inst_count == 0) return nullptr;    // couldn't compile anything

    // -----------------------------------------------------------------------
    // Epilogue (fall-through case or instruction limit hit)
    // -----------------------------------------------------------------------
    if (!terminated) {
        emit_epilogue(e, inst_count, pc - 4, exit_npc);
    }

    // -----------------------------------------------------------------------
    // Flush I-cache
    // -----------------------------------------------------------------------
    void *block_start = write_base;
    void *block_end   = e.ptr();

    if (!m_dual_map) {
        // If RWX, just flush I-cache
        __builtin___clear_cache((char *)block_start, (char *)block_end);
    } else {
        // RW → protect as RX, then flush
        size_t sz = (uint8_t *)block_end - (uint8_t *)block_start;
        sz = (sz + 4095) & ~(size_t)4095;    // round up to page
        mprotect(block_start, sz, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)m_code_buf + m_code_pos,
                                (char *)m_code_buf + m_code_pos + sz);
    }

    m_code_pos += e.size();

    // -----------------------------------------------------------------------
    // Register block in cache
    // -----------------------------------------------------------------------
    JitBlock blk;
    blk.start_pc   = start_pc;
    blk.end_pc     = pc;
    blk.inst_count = inst_count;
    blk.fn         = (void (*)(PPC_REGS *))block_start;

    m_cache[start_pc] = blk;
    return &m_cache[start_pc];
}

#endif // __aarch64__
