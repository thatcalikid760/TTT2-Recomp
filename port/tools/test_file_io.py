"""Exercise production file I/O on real Windows temp files, with native NtReadFile as oracle.

No game runs or game files are used. The complete production translation unit is
compiled with only the guest register/memory and completion interfaces stubbed.
MS references: ZwReadFile atomic seek/read and FILE_USE_FILE_POINTER_POSITION;
SetFilePointerEx requires synchronization of shared seek/read sequences.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <vector>
#include <filesystem>
#include <string>
union Register { uint32_t u32; uint64_t u64; };
struct PPCContext { Register r1{},r3{},r4{},r5{},r6{},r7{},r8{},r9{},r10{}; };
#define PPC_FUNC(name) void name(PPCContext& ctx,uint8_t* base)
constexpr uint32_t STATUS_SUCCESS=0,STATUS_UNSUCCESSFUL=0xC0000001u;
void KTrace(const char*) {}
bool ResolveSaveFile(const std::string&,std::filesystem::path&,std::filesystem::path&,uint32_t&) { return false; }
bool XamGuestRange(uint32_t a,uint64_t n,uint32_t align=4) {return a&&!(a&(align-1))&&uint64_t(a)+n<=0x100000000ull;}
uint32_t GRead32(uint8_t* p,uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(p+a)); }
uint16_t GRead16(uint8_t* p,uint32_t a) { return __builtin_bswap16(*reinterpret_cast<uint16_t*>(p+a)); }
uint8_t GRead8(uint8_t* p,uint32_t a) { return p[a]; }
void GWrite32(uint8_t* p,uint32_t a,uint32_t v) { *reinterpret_cast<uint32_t*>(p+a)=__builtin_bswap32(v); }
void GWrite16(uint8_t* p,uint32_t a,uint16_t v) { *reinterpret_cast<uint16_t*>(p+a)=__builtin_bswap16(v); }
void GWrite8(uint8_t* p,uint32_t a,uint8_t v) { p[a]=v; }
std::atomic<unsigned> signals{0},apcs{0};
void KernelSignalEventHandle(uint32_t h) { if(h!=777) std::abort(); ++signals; }
void KernelQueueGuestCall(uint32_t fn,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3) {
    if(fn!=1234||a0!=5678||a1!=64||a2||a3) std::abort(); ++apcs;
}

// Test-only scheduling/failure hooks still perform real Windows file operations.
// A brief yield after seeking makes the original race reproducible.
std::atomic<bool> stress{false};
std::atomic<DWORD> nextSeekError{0};
std::atomic<unsigned> hostReads{0};
HANDLE pauseHandle=nullptr,enteredRead=nullptr,releaseRead=nullptr;
BOOL TestSetFilePointerEx(HANDLE h,LARGE_INTEGER off,PLARGE_INTEGER out,DWORD whence) {
    if(DWORD error=nextSeekError.exchange(0)) { SetLastError(error); return FALSE; }
    BOOL ok=::SetFilePointerEx(h,off,out,whence);
    if(ok&&stress&&whence==FILE_BEGIN) Sleep(1);
    return ok;
}
BOOL TestReadFile(HANDLE h,LPVOID out,DWORD size,LPDWORD read,LPOVERLAPPED ov) {
    ++hostReads;
    if(h==pauseHandle) { SetEvent(enteredRead); WaitForSingleObject(releaseRead,5000); }
    return ::ReadFile(h,out,size,read,ov);
}
#define SetFilePointerEx TestSetFilePointerEx
#define ReadFile TestReadFile
@PRODUCTION@
#undef SetFilePointerEx
#undef ReadFile

using Memory=std::array<uint8_t,512>;
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); return 1; } } while(0)
void put64(Memory& m,uint32_t addr,uint64_t value) {
    GWrite32(m.data(),addr,uint32_t(value>>32)); GWrite32(m.data(),addr+4,uint32_t(value));
}
uint64_t get64(Memory& m,uint32_t addr) { return (uint64_t(GRead32(m.data(),addr))<<32)|GRead32(m.data(),addr+4); }
uint32_t openGuest(const char* name) {
    Memory m{}; std::memcpy(m.data()+320,name,std::strlen(name));
    GWrite32(m.data(),16+4,32); GWrite16(m.data(),32,uint16_t(std::strlen(name))); GWrite32(m.data(),32+4,320);
    PPCContext c; c.r3.u32=96; c.r5.u32=16; c.r6.u32=64;
    __imp__NtOpenFile(c,m.data()); return c.r3.u32 ? 0 : GRead32(m.data(),96);
}
uint32_t readGuest(Memory& m,uint32_t h,int64_t offset,uint32_t length,bool nullOffset=false,bool completion=true) {
    put64(m,32,uint64_t(offset));
    PPCContext c; c.r3.u32=h;c.r4.u32=completion?777:0;c.r5.u32=completion?1234:0;c.r6.u32=5678;
    c.r7.u32=64;c.r8.u32=128;c.r9.u32=length;c.r10.u32=nullOffset?0:32;
    __imp__NtReadFile(c,m.data()); return c.r3.u32;
}
uint32_t setGuest(Memory& m,uint32_t h,int64_t offset,uint32_t length=8) {
    put64(m,32,uint64_t(offset)); PPCContext c;
    c.r3.u32=h;c.r4.u32=64;c.r5.u32=32;c.r6.u32=length;c.r7.u32=14;
    __imp__NtSetInformationFile(c,m.data()); return c.r3.u32;
}
uint32_t queryGuest(Memory& m,uint32_t h,uint32_t cls=14) {
    PPCContext c;c.r3.u32=h;c.r4.u32=64;c.r5.u32=80;c.r6.u32=cls==14?8:24;c.r7.u32=cls;
    __imp__NtQueryInformationFile(c,m.data()); return c.r3.u32;
}
int64_t position(HANDLE h) { LARGE_INTEGER z{},out{}; return ::SetFilePointerEx(h,z,&out,FILE_CURRENT)?out.QuadPart:-999; }
bool seek(HANDLE h,int64_t off) { LARGE_INTEGER value;value.QuadPart=off;return ::SetFilePointerEx(h,value,nullptr,FILE_BEGIN); }
struct NativeIosb { union { LONG status; void* pointer; }; ULONG_PTR information; };
using NativeRead=LONG (NTAPI*)(HANDLE,HANDLE,void*,void*,NativeIosb*,void*,ULONG,LARGE_INTEGER*,ULONG*);

int race(uint32_t handle) {
    std::atomic<bool> start{false}; std::atomic<unsigned> failures{0};
    auto worker=[&](int64_t offset,const char* expected) {
        Memory m; while(!start) std::this_thread::yield();
        for(unsigned n=0;n<200;++n) {
            m.fill(0xA5);
            if(readGuest(m,handle,offset,3,false,false)!=0 || GRead32(m.data(),68)!=3 ||
               std::memcmp(m.data()+128,expected,3) || m[127]!=0xA5 || m[131]!=0xA5) ++failures;
        }
    };
    stress=true;
    std::thread a(worker,0,"012"),b(worker,5,"567");
    start=true;a.join();b.join();stress=false;
    CHECK(failures==0);
    std::printf("PASS 400 concurrent offset reads on one real file handle\n");
    return 0;
}

int main(int argc,char** argv) {
    if(argc<2) return 2;
    _putenv_s("TTT2_VFS_ROOT",argv[1]);
    uint32_t handle=openGuest("game:\\data000.bin"); CHECK(handle!=0);
    if(argc>2) return race(handle);
    auto file=GetFile(handle); CHECK(file!=nullptr);
    std::string filename=std::string(argv[1])+"/data000.bin";
    HANDLE native=CreateFileA(filename.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    auto nativeRead=reinterpret_cast<NativeRead>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtReadFile"));
    CHECK(native!=INVALID_HANDLE_VALUE&&event&&nativeRead);
    struct Case { int64_t off; uint32_t length; bool nullOffset=false; bool invalidHandle=false; };
    const Case cases[]={
        {0,3},{3,4},{8,5},{10,5},{15,5},{INT64_MAX,5},
        {0,0},{3,0},{10,0},{15,0},{INT64_MAX,0},
        {-2,3},{0,3,true},{-2,0},{0,0,true},
        {-1,3},{-3,3},{INT64_MIN,3},{-1,0},{-3,0},
        {0,3,false,true},{0,0,false,true}};
    for(const auto& c:cases) {
        CHECK(seek(native,2)&&seek(file->handle,2));
        ResetEvent(event); Memory m; m.fill(0xA5);
        std::array<uint8_t,32> oracle;oracle.fill(0xA5);
        NativeIosb iosb; std::memset(&iosb,0xA5,sizeof(iosb));
        LARGE_INTEGER offset;offset.QuadPart=c.off;
        uint32_t wanted=uint32_t(nativeRead(c.invalidHandle?nullptr:native,event,nullptr,nullptr,
            &iosb,oracle.data(),c.length,c.nullOffset?nullptr:&offset,nullptr));
        unsigned priorSignals=signals,priorApcs=apcs;
        uint32_t got=readGuest(m,c.invalidHandle?0xDEADBEEF:handle,c.off,c.length,c.nullOffset);
        if(got!=wanted) std::fprintf(stderr,"case off=%lld len=%u got=%08X wanted=%08X\n",c.off,c.length,got,wanted);
        CHECK(got==wanted);
        CHECK(GRead32(m.data(),64)==uint32_t(iosb.status));
        CHECK(GRead32(m.data(),68)==uint32_t(iosb.information));
        CHECK(!std::memcmp(m.data()+128,oracle.data(),oracle.size()));
        CHECK(position(file->handle)==position(native));
        bool signalled=WaitForSingleObject(event,0)==WAIT_OBJECT_0;
        CHECK(signals-priorSignals==unsigned(signalled));
        CHECK(apcs-priorApcs==unsigned(signalled));
        CHECK(m[63]==0xA5&&m[72]==0xA5&&m[127]==0xA5&&m[160]==0xA5);
    }
    Memory m;m.fill(0xA5);
    CHECK(setGuest(m,handle,15)==0); CHECK(position(file->handle)==15);
    CHECK(queryGuest(m,handle)==0&&get64(m,80)==15);
    CHECK(setGuest(m,handle,-2)==IO_STATUS_INVALID_PARAMETER&&position(file->handle)==15);
    CHECK(setGuest(m,handle,0,4)==IO_STATUS_INVALID_PARAMETER&&position(file->handle)==15);
    CHECK(queryGuest(m,handle,5)==0&&get64(m,80+8)==10);
    CHECK(queryGuest(m,0xDEADBEEF)==IO_STATUS_INVALID_HANDLE);
    CHECK(setGuest(m,0xDEADBEEF,0)==IO_STATUS_INVALID_HANDLE);

    // A genuine host ReadFile failure: an existing write-only file handle.
    std::string deniedName=std::string(argv[1])+"/denied.bin";
    HANDLE denied=CreateFileA(deniedName.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    CHECK(denied!=INVALID_HANDLE_VALUE);
    const uint32_t deniedGuest=0xE0001000;
    g_files[deniedGuest]=std::make_shared<FileRecord>(denied,"denied.bin");
    m.fill(0xA5);CHECK(readGuest(m,deniedGuest,3,4)==0xC0000022u);
    CHECK(GRead32(m.data(),64)==0xC0000022u&&GRead32(m.data(),68)==0);
    CHECK(m[128]==0xA5&&position(denied)==0);
    CHECK(TryCloseGuestFile(deniedGuest));
    // Inject a failed seek to prove ReadFile is never attempted afterward.
    unsigned beforeReads=hostReads;
    nextSeekError=ERROR_INVALID_PARAMETER;m.fill(0xA5);
    CHECK(readGuest(m,handle,3,4)==IO_STATUS_INVALID_PARAMETER);
    CHECK(hostReads==beforeReads&&m[128]==0xA5&&GRead32(m.data(),68)==0);
    nextSeekError=ERROR_INVALID_HANDLE;
    CHECK(setGuest(m,handle,3)==IO_STATUS_INVALID_HANDLE);
    nextSeekError=ERROR_INVALID_PARAMETER;
    CHECK(queryGuest(m,handle)==IO_STATUS_INVALID_PARAMETER);
    CHECK(race(handle)==0);

    // Close while a production read is inside ReadFile. The map entry disappears
    // immediately, but the retained FileRecord keeps the native handle alive.
    uint32_t closing=openGuest("game:\\data000.bin");CHECK(closing!=0);
    pauseHandle=GetFile(closing)->handle;
    enteredRead=CreateEventW(nullptr,TRUE,FALSE,nullptr); releaseRead=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    Memory pending;pending.fill(0xA5);uint32_t pendingStatus=0xFFFFFFFF;
    std::thread reader([&]{pendingStatus=readGuest(pending,closing,5,3,false,false);});
    CHECK(WaitForSingleObject(enteredRead,5000)==WAIT_OBJECT_0);
    CHECK(TryCloseGuestFile(closing)&&!GetFile(closing));
    DWORD flags=0;CHECK(GetHandleInformation(pauseHandle,&flags));
    SetEvent(releaseRead);reader.join();
    CHECK(pendingStatus==0&&!std::memcmp(pending.data()+128,"567",3));
    CHECK(!GetHandleInformation(pauseHandle,&flags)&&GetLastError()==ERROR_INVALID_HANDLE);
    pauseHandle=nullptr;CloseHandle(enteredRead);CloseHandle(releaseRead);
    CloseHandle(native);CloseHandle(event);
    CHECK(TryCloseGuestFile(handle));file.reset();
    std::printf("PASS %u file I/O checks: native NtReadFile status/bytes/position/event oracle, canaries, errors, position API, close lifetime\n",checks);
}
'''


def main():
    source = (ROOT / "port/src/runtime/kernel/kernel_io.cpp").read_text(encoding="utf-8-sig")
    for name in ("kernel_shared.h", "kernel_trace.h", "kernel_objects.h", "save_store.h", "kernel_content.h"):
        source = source.replace(f'#include "{name}"', "")
    assert "TTT2_NO_BADREAD" not in source
    with tempfile.TemporaryDirectory(prefix="ttt2-file-io-") as temp:
        folder = Path(temp)
        (folder / "data000.bin").write_bytes(b"0123456789")
        (folder / "denied.bin").write_bytes(b"0123456789")
        executable = compile_one(folder, "file_io", HARNESS.replace("@PRODUCTION@", source), [])
        result = subprocess.run([str(executable), str(folder)], text=True, capture_output=True, timeout=20)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout, end="")
        unlocked = source.replace("std::lock_guard fileLock(file->mutex);", "/* Negative control: missing file-position lock */")
        assert unlocked != source
        executable = compile_one(folder, "file_io_unlocked", HARNESS.replace("@PRODUCTION@", unlocked), [])
        result = subprocess.run([str(executable), str(folder), "race-only"], text=True, capture_output=True, timeout=20)
        if result.returncode == 0 or "failures==0" not in result.stderr:
            raise RuntimeError("Negative control failed to detect the original seek/read race:\n" + result.stdout + result.stderr)
        print("PASS negative control: removing the production lock corrupts concurrent offset reads")


if __name__ == "__main__":
    main()
