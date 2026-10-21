"""Run the production VdSwap writer against reused synthetic command storage.

The guest call at 0x82741738 passes oldCursor+4 after reserving 64 dwords,
then advances its last-written cursor to oldCursor+256. Therefore the stub
owns exactly [argument, argument+252], including dword 63. The test extracts
the real import body and substitutes only tracing/presentation dependencies.
No game image, generated code, renderer, or full build is required.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]
VIDEO = ROOT / "port/src/runtime/kernel/kernel_video.cpp"

HARNESS = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct Register { uint32_t u32 = 0; };
struct PPCContext { Register r3, r4, r5, r6, r7; };
#define PPC_FUNC(name) void name(PPCContext& ctx, uint8_t* base)
void KTrace(const char*) {}
std::atomic<uint32_t> g_frontBufferPhys{0};
std::atomic<uint32_t> g_frontBufferSwizzle{0};
std::atomic<bool> g_presentStarted{true};
unsigned PresentThread(void*) { return 0; }
void StartBigStackThread(unsigned (*)(void*)) { std::abort(); }
constexpr uint32_t fetchAddress = 0x12345000, decodedSurface = 0xA44A9000;
uint32_t DecodeFrontBuffer(uint32_t address) {
    return address == fetchAddress ? decodedSurface : 0;
}
void GWrite32(uint8_t* base, uint32_t address, uint32_t value) {
    base[address] = uint8_t(value >> 24); base[address+1] = uint8_t(value >> 16);
    base[address+2] = uint8_t(value >> 8); base[address+3] = uint8_t(value);
}
uint32_t GRead32(uint8_t* base, uint32_t address) {
    return uint32_t(base[address]) << 24 | uint32_t(base[address+1]) << 16 |
           uint32_t(base[address+2]) << 8 | base[address+3];
}

@PRODUCTION@

int main() {
    constexpr uint32_t start = 32, words = 64, end = start + words*4;
    const uint32_t poisons[] = {0, 0xC4000000, 0xFFFFFFFF, 0xC2200000,
                               0x3F800000, 0x0BADF00D};
    unsigned checks = 0;
    for (uint32_t poison : poisons) {
        std::array<uint8_t, end+32> memory;
        memory.fill(0xA5);
        auto* base = memory.data();
        for (uint32_t i=0; i<words; ++i) GWrite32(base,start+i*4,poison);
        PPCContext ctx{};
        ctx.r3.u32=start; ctx.r4.u32=fetchAddress;
        ctx.r5.u32=0x02345678; ctx.r7.u32=0x9ABCDEF0;
        __imp__VdSwap(ctx,base);
        const uint32_t marker[] = {0x54535750,0x9ABCDEF0,0x02345678,
                                   fetchAddress,decodedSurface};
        for (uint32_t i=0;i<words;++i) {
            const uint32_t expected=i<5 ? marker[i] : 0x80000000;
            const uint32_t actual=GRead32(base,start+i*4);
            ++checks;
            if(actual!=expected) {
                std::fprintf(stderr,"FAIL dword %u: %08X expected %08X (prefill %08X)\n",
                             i,actual,expected,poison);
                return 1;
            }
        }
        // The preceding cursor word and the next packet are both outside ownership.
        for (uint32_t i=0;i<memory.size();++i) if(i<start || i>=end) {
            ++checks;
            if(memory[i]!=0xA5) {
                std::fprintf(stderr,"FAIL canary byte %u changed\n",i); return 1;
            }
        }
        if(ctx.r3.u32!=0 || g_frontBufferPhys.load()!=decodedSurface) return 2;
        checks+=2;
    }
    std::printf("PASS %u checks: all 64 swap dwords initialized, marker fields, canaries, six recycled-buffer patterns\n",checks);
}
'''


def main():
    source = VIDEO.read_text(encoding="utf-8")
    begin = source.index("PPC_FUNC(__imp__VdSwap)")
    end = source.index("\nPPC_FUNC(", begin + 1)
    production = source[begin:end]
    with tempfile.TemporaryDirectory(prefix="ttt2-vdswap-") as temp:
        folder = Path(temp)
        checker = HARNESS.replace("@PRODUCTION@", production)
        exe = compile_one(folder, "swap", checker, [])
        subprocess.run([str(exe)], check=True, timeout=10)
        # Prove this regression test detects the original 63-word writer.
        old = production.replace("i < 64", "i < 63")
        if old == production:
            raise AssertionError("swap writer loop changed; update negative control")
        old_exe = compile_one(folder, "swap_old", HARNESS.replace("@PRODUCTION@", old), [])
        result = subprocess.run([str(old_exe)], capture_output=True, text=True, timeout=10)
        if result.returncode != 1 or "FAIL dword 63:" not in result.stderr:
            raise AssertionError(f"old writer unexpectedly passed: {result.returncode}\n{result.stderr}")
        print("PASS negative control: original writer fails at uninitialized dword 63")


if __name__ == "__main__":
    main()
