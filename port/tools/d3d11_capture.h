#pragma once
// Optional, bounded diagnostic ONLY. Integration in gpu_render.cpp:
//   #include "../../../tools/d3d11_capture.h"   // immediately before GpuExecuteDraw
//   Ttt2CaptureD3D11Draw(regs,initiator,indexAddr,index32,vsRef,psRef);
// as that function's first statement. This header intentionally reuses the
// renderer's decoded instruction types and address/texture helpers.
//
// Set TTT2_D3D11_CAPTURE to an absolute local output directory. Without it this
// is a cached bool check. At most one complete draw per BC2/BC3/R8 format is saved,
// at most 16 eligible attempts, <=64 MiB per capture. No game memory writes.
// The full payload is reread and compared before publishing a manifest. This
// detects ordinary concurrent mutation; it is not a cross-thread atomic snapshot.
#include <fstream>
#include <array>
#include <sstream>
#include <stdexcept>

namespace ttt2_draw_capture {
struct Segment { uint32_t address;std::vector<uint8_t> bytes;std::string name; };
inline void Check(bool test,const char* why) {if(!test)throw std::runtime_error(why);}
inline std::vector<uint8_t> Read(uint32_t address,size_t size) {
    Check(size && size<=64*1024*1024 && uint64_t(address)+size<=0x100000000ull,"capture range");
    std::vector<uint8_t> data(size);SIZE_T copied=0;
    Check(ReadProcessMemory(GetCurrentProcess(),g_base+address,data.data(),size,&copied) && copied==size,"capture unreadable memory");return data;
}
inline std::vector<uint32_t> Words(const std::vector<uint8_t>& bytes) {
    Check(bytes.size()%4==0,"unaligned shader");std::vector<uint32_t> out(bytes.size()/4);
    for(size_t i=0;i<out.size();++i){uint32_t w;std::memcpy(&w,bytes.data()+i*4,4);out[i]=__builtin_bswap32(w);}return out;
}
struct Instruction { bool fetch;uint32_t words[3]; };
inline std::vector<Instruction> ActiveInstructions(const std::vector<uint32_t>& code) {
    Check(code.size()>=3 && code.size()<=4096 && code.size()%3==0,"shader length");
    unsigned end=unsigned(code.size());bool stopped=false;std::vector<Instruction> result;
    for(unsigned p=0;p+3<=end;p+=3){Cf c[2];DecodeCfPair(&code[p],c[0],c[1]);for(const auto& f:c)if(f.isExec() && f.addr())end=std::min(end,f.addr()*3);}
    for(unsigned p=0;p+3<=end && !stopped;p+=3) {
        Cf c[2];DecodeCfPair(&code[p],c[0],c[1]);for(const auto& f:c) {
            if(f.op()==CF_NOP || f.op()==CF_ALLOC || f.op()==CF_MARK_VS_FETCH_DONE)continue;
            Check(f.op()==CF_EXEC || f.op()==CF_EXEC_END,"non-straight-line shader");
            Check(f.addr()*3>=end && uint64_t(f.addr()+f.count())*3<=code.size(),"shader EXEC range");
            for(unsigned i=0;i<f.count();++i){const auto* w=&code[(f.addr()+i)*3];result.push_back({bool((f.seq()>>(2*i))&1),{w[0],w[1],w[2]}});}
            if(f.op()==CF_EXEC_END){stopped=true;break;}
        }
    }Check(stopped,"shader missing end");return result;
}
inline void Write(const std::filesystem::path& p,const void* bytes,size_t size) {
    std::ofstream f(p,std::ios::binary);f.write(static_cast<const char*>(bytes),size);f.flush();Check(bool(f),"capture disk write");f.close();Check(bool(f),"capture disk close");
}
inline void Capture(const std::filesystem::path& root,const uint32_t* regs,uint32_t initiator,uint32_t indexAddress,bool index32,const GpuShader& vs,const GpuShader& ps) {
    static unsigned attempts=0;static std::set<unsigned> completed;
    if(attempts>=16 || completed.size()>=3)return;
    unsigned count=initiator>>16,source=(initiator>>6)&3,primitive=initiator&63;
    if(!count || count>65535 || (primitive!=4 && primitive!=6 && primitive!=8 && primitive!=13))return;
    if((regs[0x2000]&(3<<16)) || (regs[0x2200]&2) || (regs[0x2205]&3) || (ps.size!=15 && ps.size!=33) || !vs.size || vs.size>4096)return;
    auto psBytes=Read(ps.addr,ps.size*4);auto psInstructions=ActiveInstructions(Words(psBytes));
    const bool yuv=ps.size==33;
    Check(psInstructions.size()==(yuv?8:2),"not supported diagnostic shader length");
    std::vector<Texture> textures;
    for(size_t i=0;i<psInstructions.size();++i) {
        const auto& instruction=psInstructions[i];const auto* w=instruction.words;
        if(instruction.fetch) {
            TFetch tf{w[0],w[1],w[2]};Check(tf.opcode()==1 && w[2]==0x4000 && i<(yuv?3:1),"not plain leading 2D fetch");
            Texture t=DecodeTexture(regs,tf.constIndex());Check(t.valid,"invalid texture");
            if(yuv)Check(t.format==2 && !t.tiled && t.endian==0,"YUV plane outside linear R8 subset");
            else Check(t.format==19 || t.format==20,"not BC2/BC3 UI");
            textures.push_back(t);
        }
    }
    Check(textures.size()==(yuv?3:1),"texture plane count");
    if(!yuv) {const auto* w=psInstructions[1].words;Alu a{w[0],w[1],w[2]};if(!a.exportData() || a.vectorDest()!=0 || a.vectorOpcode()!=V_MUL || a.scalarWriteMask())return;}
    const unsigned captureKey=yuv?2:textures[0].format;
    if(completed.count(captureKey))return;
    ++attempts;
    for(const auto& texture:textures)Check(texture.width<=4096 && texture.height<=4096 && texture.pitch && texture.pitch<=8192,"texture dimensions");
    std::array<uint32_t,0x8000> savedRegs;std::memcpy(savedRegs.data(),regs,sizeof(savedRegs));
    std::vector<Segment> segments;size_t total=0;
    auto add=[&](uint32_t a,size_t size,const std::string& name){Check(total+size<=64*1024*1024,"capture size budget");segments.push_back({a,Read(a,size),name});total+=size;};
    add(vs.addr,vs.size*4,"vs.bin");segments.push_back({ps.addr,std::move(psBytes),"ps.bin"});total+=ps.size*4;
    unsigned maximum=count-1;
    if(source!=2 && indexAddress) {
        add(indexAddress,count*(index32?4:2),"indices.bin");maximum=0;const auto& bytes=segments.back().bytes;
        for(unsigned i=0;i<count;++i){uint32_t index;if(index32){std::memcpy(&index,bytes.data()+i*4,4);index=__builtin_bswap32(index);}else{uint16_t n;std::memcpy(&n,bytes.data()+i*2,2);index=__builtin_bswap16(n);}maximum=std::max(maximum,index);}
    }else Check(source==2 || indexAddress==0,"unsupported index source");
    Check(maximum<=1u<<20,"vertex index bound");
    unsigned lastSlot=96,lastStride=0;bool r0Modified=false;std::map<uint32_t,size_t> vertexRanges;
    for(const auto& ins:ActiveInstructions(Words(segments.front().bytes))) {
        const auto* w=ins.words;
        if(!ins.fetch) {
            Alu a{w[0],w[1],w[2]};if(!a.exportData() && ((a.vectorDest()==0 && a.vectorWriteMask()) || (a.scalarDest()==0 && a.scalarWriteMask())))r0Modified=true;
            continue;
        }
        VFetch f{w[0],w[1],w[2]};
        Check(f.opcode()==0 && !r0Modified && f.srcRegister()==0 && f.srcSwizzle()==0 && !(w[0]&(1<<11)) && !(w[0]&(1<<18)),"non-index vertex fetch");
        unsigned slot=f.isMiniFetch()?lastSlot:f.constIndex()*3+f.constIndexSel();
        unsigned stride=f.isMiniFetch()?lastStride:f.stride();if(!stride)stride=lastStride;
        Check(slot<96 && stride && f.offset()>=0 && (regs[0x4800+slot*2]&3)==3,"invalid vertex fetch");
        if(!f.isMiniFetch()){lastSlot=slot;lastStride=stride;}
        uint32_t a=PhysToGuest(regs[0x4800+slot*2]&~3u);
        // Up to a float4 is read by every currently decoded vertex format.
        uint64_t bytes=(uint64_t(maximum)*stride+unsigned(f.offset()))*4+16;
        Check(bytes<=8*1024*1024 && uint64_t(a)+bytes<=0x100000000ull,"vertex buffer bound");
        vertexRanges[a]=std::max(vertexRanges[a],size_t(bytes));
        if(f.dstRegister()==0)r0Modified=true;
    }
    Check(!vertexRanges.empty(),"missing vertex fetch");unsigned vn=0;
    for(auto [a,n]:vertexRanges)add(a,n,"vertex"+std::to_string(vn++)+".bin");
    for(size_t ti=0;ti<textures.size();++ti) {
        const auto& texture=textures[ti];uint64_t last=0;unsigned bw=(texture.pitch+3)/4;
        if(yuv)last=uint64_t(texture.pitch)*(texture.height-1)+texture.width;
        else for(unsigned y=0;y<(texture.height+3)/4;++y)for(unsigned x=0;x<(texture.width+3)/4;++x) {
            uint64_t block=texture.tiled?GpuTiledOffset2D(x,y,bw,4):uint64_t(y)*bw+x;
            last=std::max(last,(block+1)*16);
        }
        add(texture.base,size_t(last),"texture"+std::to_string(ti)+".bin");
    }
    Check(std::memcmp(savedRegs.data(),regs,sizeof(savedRegs))==0,"registers changed during capture");
    for(const auto& s:segments)Check(Read(s.address,s.bytes.size())==s.bytes,"resource changed during capture");
    unsigned width=regs[0x2000]&0x3FFF,height=(regs[0x2082]>>16)&0x3FFF;
    Check(width && width<=4096 && height && height<=2048,"capture output size");
    auto directory=root/("draw_"+std::to_string(GetCurrentProcessId())+"_f"+std::to_string(captureKey)+"_"+std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root);
    Check(std::filesystem::create_directory(directory),"capture destination already exists");
    Write(directory/"regs.bin",savedRegs.data(),sizeof(savedRegs));
    std::ostringstream manifest;manifest<<"{\n\"version\":1,\"registers\":\"regs.bin\",\"width\":"<<width<<",\"height\":"<<height<<",\"initiator\":"<<initiator<<",\"index_address\":"<<indexAddress<<",\"index32\":"<<(index32?1:0)<<",\"vs_address\":"<<vs.addr<<",\"vs_words\":"<<vs.size<<",\"ps_address\":"<<ps.addr<<",\"ps_words\":"<<ps.size<<",\"snapshot_validation\":\"all resources reread equal before publishing\",\"segments\":[\n";
    for(size_t i=0;i<segments.size();++i){const auto& s=segments[i];Write(directory/s.name,s.bytes.data(),s.bytes.size());if(i)manifest<<",\n";manifest<<"{\"address\":"<<s.address<<",\"path\":\""<<s.name<<"\"}";}
    manifest<<"\n]}\n";auto text=manifest.str();Write(directory/"draw.json.tmp",text.data(),text.size());
    std::filesystem::rename(directory/"draw.json.tmp",directory/"draw.json");completed.insert(captureKey);
    std::fprintf(stderr,"[d3d11-capture] saved %s (%zu bytes, %u indices)\n",directory.string().c_str(),total,count);
}
} // namespace ttt2_draw_capture

inline void Ttt2CaptureD3D11Draw(const uint32_t* regs,uint32_t initiator,uint32_t indexAddress,bool index32,const GpuShader& vs,const GpuShader& ps) {
    static const std::filesystem::path root=[] {
        const char* path=std::getenv("TTT2_D3D11_CAPTURE");
        if(!path || !*path)return std::filesystem::path{};
        std::filesystem::path p(path);return p.is_absolute()?p:std::filesystem::path{};
    }();
    if(root.empty())return;
    static std::mutex captureMutex;std::unique_lock lock(captureMutex,std::try_to_lock);if(!lock)return;
    try {ttt2_draw_capture::Capture(root,regs,initiator,indexAddress,index32,vs,ps);}
    catch(const std::exception& e){static unsigned errors=0;if(errors++<4)std::fprintf(stderr,"[d3d11-capture] skipped: %s\n",e.what());}
}
