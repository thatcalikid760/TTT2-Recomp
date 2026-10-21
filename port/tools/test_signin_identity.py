"""Exercise the production offline identity and the game's real change detector.

The profile list (profiles.cpp) is replaced by a fixed one: the original "TTT2" save's profile
signed in, then a second profile, then nobody.

Generated guest bodies are read locally, compiled temporarily, and never copied
into this test. The old flags-ignoring behavior must reproduce the transition.
"""
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "ppc_config.h"
#include "ppc_context.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
void KTrace(const char*) {}
namespace profiles {
struct Profile { std::string name; uint64_t xuid = 0; };
int g_active=0;
const Profile g_list[]={{"TTT2",0xB13EBABEBABEBABEull},{"TEST",0xE000A199EE5B5563ull},{"Alex",0xE000F2E399AD99AAull}};
int ActiveIndex(){return g_active;}
Profile Active(){return g_active<0?Profile{}:g_list[g_active];}
uint64_t ActiveXuid(){return Active().xuid;}
}
bool User0SignedIn(){return profiles::g_active>=0;}
// xlive.cpp derives the online XUID per machine; a fixed one per profile here.
namespace xlive { uint64_t OnlineXuid(){return 0x000900004E4D0859ull+uint64_t(profiles::g_active);} }
void UserTrace(const char*,uint32_t,uint32_t,uint32_t){}
void GWrite8(uint8_t* base,uint32_t address,uint8_t value){base[address]=value;}
bool g_live=false;bool TitleServerEnabled(){return g_live;}
bool XamGuestRange(uint32_t a,uint64_t n,uint32_t alignment=4){return a&&!(a&(alignment-1))&&uint64_t(a)+n<=0x100000000ull;}
void GWrite32(uint8_t* base,uint32_t address,uint32_t value) {
    value=__builtin_bswap32(value);std::memcpy(base+address,&value,4);
}
uint32_t Read32(uint8_t* base,uint32_t address) {
    uint32_t value;std::memcpy(&value,base+address,4);return __builtin_bswap32(value);
}
PPC_FUNC(sub_82BF7A48);
PPC_FUNC(sub_82BEEAE8);
@IMPORTS@
@GUEST@
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    // Static, 64-byte aligned guest memory: as a stack array clang vectorised a memset into it
    // with 32-byte-aligned AVX stores the 16-byte-aligned frame did not honour (#GP).
    alignas(64) static std::array<uint8_t,16384> memory{};auto* base=memory.data();PPCContext ctx{};
    // An existing offline record, with pointer to its stored XUID.
    GWrite32(base,0x100+12,0x200);GWrite32(base,0x100+16,0);
    GWrite32(base,0x200,0xB13EBABE);GWrite32(base,0x204,0xBABEBABE);
    for(int i=0;i<100;++i) {
        ctx.r1.u64=0x3000;ctx.r3.u64=0x100;
        sub_82B37080(ctx,base);
        CHECK(ctx.r3.u32==0); // No sign-in transition for an unchanged offline user.
        CHECK(ctx.r1.u32==0x3000);
    }
    for(uint32_t user:{0u,1u,2u,3u,4u,0xFFu}) {
        ctx.r3.u64=user;__imp__XamUserGetSigninState(ctx,base);
        CHECK(ctx.r3.u32==(user==0?1u:0u));
        for(uint32_t flags:{0u,1u,2u,3u}) {
            std::memset(base+0x400,0xA5,48);
            ctx.r3.u64=user;ctx.r4.u64=flags;ctx.r5.u64=0x404;
            __imp__XamUserGetSigninInfo(ctx,base);
            CHECK(ctx.r3.u32==(user==0?0u:0x80070525u));
            CHECK(Read32(base,0x400)==0xA5A5A5A5&&Read32(base,0x42C)==0xA5A5A5A5);
            CHECK(Read32(base,0x404)==(user==0&&flags!=1?0xB13EBABEu:0u));
            CHECK(Read32(base,0x408)==(user==0&&flags!=1?0xBABEBABEu:0u));
            CHECK(Read32(base,0x410)==(user==0?1u:0u));
            if(user) for(unsigned j=0;j<40;++j) CHECK(base[0x404+j]==0);
            else CHECK(std::strcmp(reinterpret_cast<char*>(base+0x41C),"TTT2")==0);
        }
    }
    auto saved=memory;ctx.r3.u64=0;ctx.r5.u64=0;
    __imp__XamUserGetSigninInfo(ctx,base);
    CHECK(ctx.r3.u32==0x80070057u&&memory==saved);
    for(uint32_t user:{0u,1u,2u,3u,4u,0xFFu})for(uint32_t mask:{0u,1u,2u,3u,4u,5u,7u,8u}) {
        std::memset(base+0x400,0xA5,16);ctx.r3.u32=user;ctx.r4.u32=mask;ctx.r5.u32=0x404;
        __imp__XamUserGetXUID(ctx,base);
        bool valid=mask==1||mask==2||mask==3||mask==4||mask==7;
        uint32_t expected=user>=4||!valid?0x80070057u:user||mask==4?0x80070525u:0;
        CHECK(ctx.r3.u32==expected);
        CHECK(Read32(base,0x400)==0xA5A5A5A5&&Read32(base,0x40C)==0xA5A5A5A5);
        CHECK(Read32(base,0x404)==(expected?0:0xB13EBABEu)&&Read32(base,0x408)==(expected?0:0xBABEBABEu));
    }
    for(uint32_t bad:{0u,0x401u,0xFFFFFFFCu}) {
        saved=memory;ctx.r3.u32=0;ctx.r4.u32=7;ctx.r5.u32=bad;__imp__XamUserGetXUID(ctx,base);
        CHECK(ctx.r3.u32==0x80070057u&&memory==saved);
    }
    for(uint32_t user:{0u,1u,4u}) {
        ctx={};ctx.r1.u32=0x3000;ctx.r3.u32=user;ctx.r4.u32=0x404;
        sub_82BEEEE0(ctx,base);
        CHECK(ctx.r3.u32==(user==0?0:user==1?1317:87));CHECK(ctx.r1.u32==0x3000);
    }
    // TTT2_TITLE_SERVER=1: user 0 is Live-signed-in with a distinct, stable online XUID.
    g_live=true;
    ctx.r3.u64=0;__imp__XamUserGetSigninState(ctx,base);CHECK(ctx.r3.u32==2);
    for(uint32_t flags:{0u,1u,2u,3u}) {
        std::memset(base+0x400,0xA5,48);ctx.r3.u64=0;ctx.r4.u64=flags;ctx.r5.u64=0x404;
        __imp__XamUserGetSigninInfo(ctx,base);
        CHECK(ctx.r3.u32==0);
        CHECK(Read32(base,0x404)==(flags==1?0x00090000u:0xB13EBABEu));
        CHECK(Read32(base,0x408)==(flags==1?0x4E4D0859u:0xBABEBABEu));
        CHECK(Read32(base,0x40C)==1&&Read32(base,0x410)==2);
    }
    // XUserGetXUID (mask 7) and the online bit give the online XUID while signed in to LIVE.
    for(uint32_t mask:{1u,2u,3u,7u}) {
        std::memset(base+0x400,0xA5,16);ctx.r3.u32=0;ctx.r4.u32=mask;ctx.r5.u32=0x404;
        __imp__XamUserGetXUID(ctx,base);CHECK(ctx.r3.u32==0);
        CHECK(Read32(base,0x404)==((mask&2)?0x00090000u:0xB13EBABEu));
        CHECK(Read32(base,0x408)==((mask&2)?0x4E4D0859u:0xBABEBABEu));
    }
    // Another profile: its own offline XUID and name, and its own online XUID.
    profiles::g_active=2;
    std::memset(base+0x400,0xA5,48);ctx.r3.u64=0;ctx.r4.u64=2;ctx.r5.u64=0x404;
    __imp__XamUserGetSigninInfo(ctx,base);
    CHECK(ctx.r3.u32==0&&Read32(base,0x404)==0xE000F2E3u&&Read32(base,0x408)==0x99AD99AAu);
    CHECK(std::strcmp(reinterpret_cast<char*>(base+0x41C),"Alex")==0);
    ctx.r3.u64=0;ctx.r4.u64=1;ctx.r5.u64=0x404;__imp__XamUserGetSigninInfo(ctx,base);
    CHECK(ctx.r3.u32==0&&Read32(base,0x404)==0x00090000u&&Read32(base,0x408)==0x4E4D0859u+2);
    ctx.r3.u32=0;ctx.r4.u32=1;ctx.r5.u32=0x404;__imp__XamUserGetXUID(ctx,base);
    CHECK(ctx.r3.u32==0&&Read32(base,0x404)==0xE000F2E3u&&Read32(base,0x408)==0x99AD99AAu);
    // Nobody signed in (the title before Select Profile): no user anywhere.
    profiles::g_active=-1;
    ctx.r3.u64=0;__imp__XamUserGetSigninState(ctx,base);CHECK(ctx.r3.u32==0);
    std::memset(base+0x400,0xA5,48);ctx.r3.u64=0;ctx.r4.u64=2;ctx.r5.u64=0x404;
    __imp__XamUserGetSigninInfo(ctx,base);
    CHECK(ctx.r3.u32==0x80070525u);for(unsigned j=0;j<40;++j) CHECK(base[0x404+j]==0);
    ctx.r3.u32=0;ctx.r4.u32=1;ctx.r5.u32=0x404;__imp__XamUserGetXUID(ctx,base);CHECK(ctx.r3.u32==0x80070525u);
    // (The guest detector is only exercised offline: its Live path needs the online profile
    // record the game builds at sign-in, which this harness does not model.)
    std::printf("PASS %u identity checks including real guest sign-in detector, Live mode, a second profile and signed out\n",checks);
}
'''


def body(text, marker):
    start = text.index(marker)
    return text[start:text.index('\n}', start)+2]


def main():
    xam = (ROOT/'port/src/runtime/kernel/kernel_xam.cpp').read_text(encoding='utf-8-sig')
    imports = '\n'.join(body(xam, 'PPC_FUNC(__imp__'+name+')') for name in
                        ('XamUserGetSigninState', 'XamUserGetSigninInfo', 'XamUserGetXUID'))
    names = {'82B37080', '82BF7A48', '82BEEAE8', '82BEEEE0'}
    guest = []
    for path in (ROOT/'re/ppc').glob('ppc_recomp.*.cpp'):
        text = path.read_text(encoding='utf-8-sig')
        for name in names:
            marker = 'PPC_FUNC_IMPL(__imp__sub_'+name+')'
            if marker in text:
                guest.append(body(text, marker).replace(marker, 'PPC_FUNC(sub_'+name+')'))
    assert len(guest) == len(names)
    template = HARNESS.replace('@GUEST@', '\n'.join(guest))
    includes = [ROOT/'re/ppc', ROOT/'tools/XenonRecomp/XenonUtils',
                ROOT/'tools/XenonRecomp/thirdparty/simde']
    with tempfile.TemporaryDirectory(prefix='ttt2-signin-') as tmp:
        for name, code in [('current', imports), ('old_flags', imports.replace(
                'if (!flags || (flags & 2))', 'if (true)'))]:
            exe = compile_one(Path(tmp), name, template.replace('@IMPORTS@', code), includes)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            if name == 'old_flags':
                assert result.returncode and 'ctx.r3.u32==0' in result.stderr, result
                print('PASS negative control: flags-ignoring shim reproduces guest sign-in transition')
            else:
                if result.returncode: raise RuntimeError(result.stdout+result.stderr)
                print(result.stdout, end='')


if __name__ == '__main__':
    main()
