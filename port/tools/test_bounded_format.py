"""Canary regression for GPU diagnostics, including the production boot log.

The original snprintf return convention made the 384-byte boot diagnostic
overwrite subsequent host stack locals. The negative control restores that
convention in the extracted diagnostic and must corrupt its trailing canary.
No game or live process is involved.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include "bounded_format.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

template<size_t Capacity> struct Guard {
    std::array<unsigned char,32> before;
    char buffer[Capacity];
    // Keep negative-control writes inside the enclosing test allocation, while
    // detecting every byte written beyond the diagnostic's actual capacity.
    std::array<unsigned char,2048> after;
    Guard() { std::memset(this,0xA5,sizeof(*this)); }
    bool Intact() const {
        for(auto v:before) if(v!=0xA5) return false;
        for(auto v:after) if(v!=0xA5) return false;
        return true;
    }
};
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); return 1; } } while(0)
std::array<uint8_t,4096> guest{};
uint8_t* g_base=guest.data();
std::unordered_map<uint32_t,uint32_t> words;
uint32_t g_opcodeSeen[128]{};
uint32_t GRead32(uint8_t*, uint32_t address) {
    auto found=words.find(address);
    return found==words.end() ? 0xAABBCCDD : found->second;
}
void CaptureDiagnostic(const char* text, FILE*) {
    // The production caller expects a terminated string, even if it was clipped.
    if(std::strlen(text)>600) std::abort();
}
bool CheckBootDiagnostic() {
@BOOT@
    if(!guard_sl.Intact()) {
        std::fprintf(stderr,"FAIL production boot diagnostic canary, offset=%d capacity=%zu\n",sn,sizeof(sl));
        return false;
    }
    if(sn!=sizeof(sl)-1 || sl[sn]!='\0' || std::strlen(sl)!=size_t(sn)) return false;
    return true;
}
std::string OpcodeDiagnostic() {
@OPCODES@
    return ops;
}
int main() {
    Guard<16> guard;
    auto& text=guard.buffer;
    CHECK(BoundedSnprintf(nullptr,0,"ignored %d",42)==0);
    CHECK(BoundedSnprintf(text,1,"abc")==0 && text[0]=='\0' && guard.Intact());
    CHECK(BoundedSnprintf(text,sizeof(text),"%s","123456789012345")==15);
    CHECK(std::strcmp(text,"123456789012345")==0 && guard.Intact());
    int offset=0;
    for(unsigned i=0;i<1000;++i) {
        offset+=BoundedSnprintf(text+offset,sizeof(text)-offset," %08X:%u",i,0xFFFFFFFFu);
        CHECK(offset>=0 && size_t(offset)<sizeof(text) && guard.Intact());
        CHECK(text[offset]=='\0' && std::strlen(text)==size_t(offset));
    }
    CHECK(offset==15);
    Guard<1> one;
    for(unsigned i=0;i<10;++i)
        CHECK(BoundedSnprintf(one.buffer,1,"%1000u",i)==0 && one.buffer[0]=='\0' && one.Intact());
    // Populate every production pointer branch; wide decimal fields are deliberate.
    guest.fill(255);
    words={{0x82EE7904,0x100},{0x82E6B558,0x200},{0x82E6B55C,0x300},
           {0x30C,0x400},{0x408,0x500},{0x500,0x600},{0x310,0x700},
           {0x700,0x820E7D70}};
    CHECK(CheckBootDiagnostic());
    CHECK(OpcodeDiagnostic().empty());
    for(auto& count:g_opcodeSeen) count=0xFFFFFFFF;
    const auto opcodes=OpcodeDiagnostic();
    CHECK(!opcodes.empty() && opcodes.size()<256);
    std::printf("PASS %u bounded-format checks: repeated truncation, zero/one capacity, exact fit, production boot diagnostic canaries\n",checks);
}
'''


def main():
    video = (ROOT / "port/src/runtime/kernel/kernel_video.cpp").read_text(encoding="utf-8-sig")
    if "std::snprintf(" in video:
        raise AssertionError("GPU formatting bypasses the bounded helper")
    begin = video.index("uint32_t sm = GRead32(g_base, 0x82EE7904u);")
    final = "std::fputs(sl, stderr);"
    end = video.index(final, begin) + len(final)
    production = video[begin:end]
    production = production.replace("char sl[384];", "Guard<384> guard_sl; auto& sl=guard_sl.buffer;")
    production = production.replace("char ol[320];", "Guard<320> guard_ol; auto& ol=guard_ol.buffer;")
    production = production.replace("std::fputs(ol, stderr);", "if(!guard_ol.Intact()) return false; CaptureDiagnostic(ol, stderr);")
    production = production.replace(final, "CaptureDiagnostic(sl, stderr);")
    begin_ops = video.index("char ops[256]")
    end_ops = video.index("// Screen-flow state:", begin_ops)
    harness = HARNESS.replace("@OPCODES@", video[begin_ops:end_ops])
    with tempfile.TemporaryDirectory(prefix="ttt2-bounded-format-") as temp:
        folder = Path(temp)
        includes = [ROOT / "port/src/runtime/kernel"]
        exe = compile_one(folder, "format", harness.replace("@BOOT@", production), includes)
        subprocess.run([str(exe)], check=True, timeout=10)
        old = production.replace("BoundedSnprintf(", "std::snprintf(")
        assert old != production
        old_exe = compile_one(folder, "format_old", harness.replace("@BOOT@", old), includes)
        result = subprocess.run([str(old_exe)], capture_output=True, text=True, timeout=10)
        if result.returncode != 1 or "FAIL production boot diagnostic canary" not in result.stderr:
            raise AssertionError(f"Original formatter did not reproduce corruption: {result.returncode}\n{result.stderr}")
        print("PASS negative control: original production snprintf accumulation corrupts boot diagnostic canary")


if __name__ == "__main__":
    main()
