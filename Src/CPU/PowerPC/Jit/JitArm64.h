#pragma once
#ifdef __aarch64__

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include "../ppc_regs.h"

// Forward declarations from the interpreter (defined in ppc.cpp/ppc603.c)
#ifdef __cplusplus
extern "C" {
#endif
extern void      ppc_dispatch_opcode(UINT32 opcode);
extern void      ppc_check_interrupts_jit(void);
extern PPC_REGS *ppc_get_state(void);
extern UINT32    ppc_read_opcode_at(UINT32 pc);
extern void      jit_sync_fetch(UINT32 pc);     // sync ppc.cur_fetch before compilation
// Memory bridges: all PPC memory accesses from JIT go through these
extern UINT32    jit_read8(UINT32 addr);
extern UINT32    jit_read16(UINT32 addr);
extern UINT32    jit_read32(UINT32 addr);
extern UINT64    jit_read64(UINT32 addr);
extern UINT32    jit_read_tbl(void);
extern UINT32    jit_read_tbu(void);
extern void      jit_write8(UINT32 addr, UINT32 data);
extern void      jit_write16(UINT32 addr, UINT32 data);
extern void      jit_write32(UINT32 addr, UINT32 data);
extern void      jit_write64(UINT32 addr, UINT64 data);
// FP estimate helpers: argument and return value in D0 (AAPCS64 FP convention)
extern double    jit_fres(double x);
extern double    jit_frsqrte(double x);
#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------
// JIT block descriptor
// ---------------------------------------------------------------------------
struct JitBlock {
    uint32_t start_pc;
    uint32_t end_pc;          // first PC NOT in this block
    int      inst_count;
    void   (*fn)(PPC_REGS *); // compiled native function; X19 = ppc on entry
};

// ---------------------------------------------------------------------------
// PowerPC → ARM64 JIT
//
// Usage: JitArm64::get().init(); then JitArm64::get().run(cycles);
// JitArm64::get().flush() invalidates all compiled blocks (e.g. on reset).
// ---------------------------------------------------------------------------
class JitArm64
{
public:
    static JitArm64 &get();

    // Initialise the code buffer (mmap). Returns false if JIT is unavailable.
    bool init();
    void shutdown();

    // Invalidate all compiled blocks (must be called on reset / ROM change)
    void flush();

    // Find or compile a block for the given PC. Returns nullptr on failure.
    JitBlock *get_or_compile(uint32_t pc);

    bool is_available() const { return m_code_buf != nullptr; }

private:
    JitArm64() = default;

    JitBlock *compile(uint32_t pc);

    // Code buffer (executable memory region)
    static constexpr size_t CODE_BUF_SIZE = 16 * 1024 * 1024;  // 16 MB
    uint8_t  *m_code_buf   = nullptr;
    uint8_t  *m_write_buf  = nullptr;   // may differ from m_code_buf on W^X systems
    size_t    m_code_pos   = 0;

    // Block cache: guest PC → JitBlock
    std::unordered_map<uint32_t, JitBlock> m_cache;

    // Direct-mapped fast lookup: avoids hash map on repeated block entries.
    // Indexed by (pc >> 2) & (FAST_CACHE_MASK). Null means no entry for that slot.
    static constexpr uint32_t FAST_CACHE_SIZE = 4096;
    static constexpr uint32_t FAST_CACHE_MASK = FAST_CACHE_SIZE - 1;
    JitBlock *m_fast_cache[FAST_CACHE_SIZE] = {};

    bool m_dual_map = false;  // true when write ptr != exec ptr
};

#endif // __aarch64__
