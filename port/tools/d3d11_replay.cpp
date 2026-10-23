// Headless D3D11 proof, appended to a temporary copy of production gpu_render.cpp
// by test_d3d11_replay.py. No window, swap chain, game process, or runtime backend.
// The copied renderer supplies the CPU vertex/shader/raster oracle; its only
// instrumentation records the post-VS triangles without changing their values.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <array>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
static double Millis(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
static void Require(bool ok, const std::string& why) { if(!ok) throw std::runtime_error(why); }
static void Hr(HRESULT hr, const char* what) { if(FAILED(hr)) { char b[160]; std::snprintf(b,sizeof(b),"%s: HRESULT %08X",what,unsigned(hr)); throw std::runtime_error(b); } }
static std::vector<std::array<Vertex,3>> replayTriangles;
static bool recordTriangles = true;
static void ReplayRecordTriangle(const Vertex& a,const Vertex& b,const Vertex& c) {
    if(recordTriangles) replayTriangles.push_back({a,b,c});
}
static std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary); Require(bool(f),"Cannot read "+path.string());
    return {(std::istreambuf_iterator<char>(f)),{}};
}
static void WriteBytes(const std::filesystem::path& path,const void* data,size_t count) {
    std::ofstream f(path,std::ios::binary); f.write(static_cast<const char*>(data),count); Require(bool(f),"Cannot write "+path.string());
}
static uint32_t ReadU32(std::ifstream& f) { uint32_t v; f.read(reinterpret_cast<char*>(&v),4); Require(bool(f),"Truncated replay bundle"); return v; }
struct DrawInput {
    uint32_t width,height,initiator,indexAddress,index32;
    GpuShader vs,ps;
    std::array<uint32_t,0x8000> regs;
};
static DrawInput LoadInput(const char* name) {
    DrawInput d{}; std::ifstream f(name,std::ios::binary); Require(bool(f),"Missing replay bundle");
    Require(ReadU32(f)==0x31315244 && ReadU32(f)==1,"Wrong replay bundle version");
    d.width=ReadU32(f);d.height=ReadU32(f);d.initiator=ReadU32(f);d.indexAddress=ReadU32(f);d.index32=ReadU32(f);
    d.vs={ReadU32(f),ReadU32(f)};d.ps={ReadU32(f),ReadU32(f)};uint32_t count=ReadU32(f);
    Require(d.width && d.height && d.width<=4096 && d.height<=2048 && count<=128,"Invalid replay dimensions/segments");
    f.read(reinterpret_cast<char*>(d.regs.data()),sizeof(d.regs));Require(bool(f),"Truncated register file");
    g_base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));Require(g_base,"Guest address reservation failed");
    for(uint32_t i=0;i<count;++i) {
        uint32_t a=ReadU32(f),size=ReadU32(f);Require(size && size<=128*1024*1024 && uint64_t(a)+size<=0x100000000ull,"Invalid memory segment");
        uint64_t start=a&~4095u,end=(uint64_t(a)+size+4095)&~4095ull;
        Require(VirtualAlloc(g_base+start,end-start,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"Guest segment commit failed");
        f.read(reinterpret_cast<char*>(g_base+a),size);Require(bool(f),"Truncated memory segment");
    }
    Require(f.peek()==EOF,"Trailing replay bundle bytes");return d;
}

// Deliberately bounded raw-microcode translation. Unsupported features fail
// before device creation/draw. No instruction bytes or game assets are embedded.
// Instruction layout: local XenosRecomp shader_code.h. Texture dimension is at
// bits14..15 of dword2 (the currently unused CPU accessor uses different bits).
#include "gpu_shader_subset.h"
#include "gpu_d3d11_movie.h"
using ttt2_d3d11::Translation;
using ttt2_d3d11::Translate;
bool replayFailAfterReadback=false;
static ComPtr<ID3DBlob> Compile(const std::string& text,const char* target) {
    ComPtr<ID3DBlob> code,error;HRESULT h=D3DCompile(text.data(),text.size(),"translated_raw_microcode",nullptr,nullptr,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
    if(FAILED(h) && error)throw std::runtime_error(std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()));
    Hr(h,"D3DCompile");return code;
}
struct NativeVertex { float pos[4];float attrs[16][4]; };
static std::vector<NativeVertex> NativeVertices(uint32_t width,uint32_t height) {
    std::vector<NativeVertex> v;
    for(const auto& triangle:replayTriangles)for(const auto& p:triangle) {
        Require(p.valid && std::isfinite(p.invW) && p.invW>0,"Invalid/negative-W vertex outside pilot scope");
        NativeVertex n{};float w=1/p.invW;
        n.pos[0]=(p.x*2/width-1)*w;n.pos[1]=(1-p.y*2/height)*w;n.pos[2]=p.z*w;n.pos[3]=w;
        for(unsigned a=0;a<16;++a)for(unsigned c=0;c<4;++c)n.attrs[a][c]=p.attr[a][c]*w;
        v.push_back(n);
    }return v;
}
static D3D11_BLEND Blend(unsigned v,bool alpha) {
    switch(v) {
    case 0:return D3D11_BLEND_ZERO;case 1:return D3D11_BLEND_ONE;
    case 4:return alpha?D3D11_BLEND_SRC_ALPHA:D3D11_BLEND_SRC_COLOR;
    case 5:return alpha?D3D11_BLEND_INV_SRC_ALPHA:D3D11_BLEND_INV_SRC_COLOR;
    case 6:return D3D11_BLEND_SRC_ALPHA;case 7:return D3D11_BLEND_INV_SRC_ALPHA;
    case 8:return alpha?D3D11_BLEND_DEST_ALPHA:D3D11_BLEND_DEST_COLOR;
    case 9:return alpha?D3D11_BLEND_INV_DEST_ALPHA:D3D11_BLEND_INV_DEST_COLOR;
    case 10:return D3D11_BLEND_DEST_ALPHA;case 11:return D3D11_BLEND_INV_DEST_ALPHA;
    case 12:return D3D11_BLEND_BLEND_FACTOR;case 13:return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16:return D3D11_BLEND_SRC_ALPHA_SAT;
    default:throw std::runtime_error("Unsupported blend factor");
    }
}
static D3D11_BLEND_OP BlendOp(unsigned v) {
    const D3D11_BLEND_OP ops[]={D3D11_BLEND_OP_ADD,D3D11_BLEND_OP_SUBTRACT,D3D11_BLEND_OP_MIN,D3D11_BLEND_OP_MAX,D3D11_BLEND_OP_REV_SUBTRACT};Require(v<5,"Unsupported blend operation");return ops[v];
}
struct NativeResult { std::vector<uint32_t> pixels;double drawMs=0,copyGpuMs=0,readbackWallMs=0,uploadWallMs=0,frameMedianMs=0,frameP95Ms=0;uint64_t uploadBytes=0;std::string adapter; };
static NativeResult RunNative(const DrawInput& input,const Translation& translated,unsigned repetitions) {
    const auto* r=input.regs.data();NativeResult result;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,D3D11_SDK_VERSION,&device,&level,&context),"D3D11CreateDevice hardware");
    ComPtr<IDXGIDevice> dxgi;Hr(device.As(&dxgi),"Query DXGI");ComPtr<IDXGIAdapter> adapter;Hr(dxgi->GetAdapter(&adapter),"GetAdapter");DXGI_ADAPTER_DESC ad{};Hr(adapter->GetDesc(&ad),"Adapter description");
    char name[256];WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,name,sizeof(name),nullptr,nullptr);result.adapter=name;
    const std::string vertexHlsl="struct V {float4 pos:POSITION;float4 a[16]:TEXCOORD0;};struct O {float4 pos:SV_Position;float4 a[16]:TEXCOORD0;}; O main(V v) {O o;o.pos=v.pos;[unroll]for(int i=0;i<16;++i)o.a[i]=v.a[i];return o;}";
    auto vsCode=Compile(vertexHlsl,"vs_5_0"),psCode=Compile(translated.hlsl,"ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Hr(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"CreateVS");Hr(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"CreatePS");
    D3D11_INPUT_ELEMENT_DESC elems[17]{};elems[0]={"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    for(unsigned i=0;i<16;++i)elems[i+1]={"TEXCOORD",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16+i*16,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;Hr(device->CreateInputLayout(elems,17,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout),"CreateInputLayout");
    auto verts=NativeVertices(input.width,input.height);Require(!verts.empty(),"No CPU-oracle triangles");
    D3D11_BUFFER_DESC vbDesc{};vbDesc.ByteWidth=UINT(verts.size()*sizeof(NativeVertex));vbDesc.Usage=D3D11_USAGE_IMMUTABLE;vbDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{verts.data(),0,0};ComPtr<ID3D11Buffer> vb;Hr(device->CreateBuffer(&vbDesc,&vbData,&vb),"Create vertex buffer");
    struct Constants {float values[256][4];float alpha;float padding[3];} constants{};
    std::memcpy(constants.values,&r[0x4400],4096);constants.alpha=AsFloat(r[0x210E]);
    D3D11_BUFFER_DESC cbDesc{};cbDesc.ByteWidth=sizeof(constants);cbDesc.Usage=D3D11_USAGE_IMMUTABLE;cbDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbData{&constants,0,0};ComPtr<ID3D11Buffer> cb;Hr(device->CreateBuffer(&cbDesc,&cbData,&cb),"Create constants");
    std::vector<ComPtr<ID3D11Texture2D>> textureKeep;std::vector<ComPtr<ID3D11ShaderResourceView>> srvKeep;
    struct Plane {ID3D11Texture2D* texture;const uint8_t* bytes;UINT pitch;};std::vector<Plane> planes;
    auto uploadStart=Clock::now();
    for(unsigned slot:translated.textures) {
        Texture t=DecodeTexture(r,slot);Require(t.valid && t.width<=4096 && t.height<=4096,"Invalid texture");
        Require(t.format==FMT_8 || t.format==FMT_DXT1 || t.format==FMT_DXT2_3 || t.format==FMT_DXT4_5 || t.format==FMT_8_8_8_8,"Texture format outside pilot subset");
        bool rawR8=t.format==FMT_8;Require(!rawR8 || (!t.tiled && !t.endian),"Only linear endian0 R8 upload supported");
        std::vector<V4> texels;
        if(!rawR8){texels.resize(size_t(t.width)*t.height);for(unsigned y=0;y<t.height;++y)for(unsigned x=0;x<t.width;++x)texels[size_t(y)*t.width+x]=SampleTexel(t,x,y);}
        D3D11_TEXTURE2D_DESC td{};td.Width=t.width;td.Height=t.height;td.MipLevels=td.ArraySize=1;td.Format=rawR8?DXGI_FORMAT_R8_UNORM:DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.Usage=rawR8?D3D11_USAGE_DEFAULT:D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{rawR8?static_cast<const void*>(g_base+t.base):texels.data(),rawR8?t.pitch:UINT(t.width*sizeof(V4)),0};ComPtr<ID3D11Texture2D> texture;Hr(device->CreateTexture2D(&td,&data,&texture),"Create decoded texture");
        ComPtr<ID3D11ShaderResourceView> srv;Hr(device->CreateShaderResourceView(texture.Get(),nullptr,&srv),"Create texture SRV");ID3D11ShaderResourceView* raw=srv.Get();context->PSSetShaderResources(slot,1,&raw);
        if(rawR8)planes.push_back({texture.Get(),g_base+t.base,t.pitch});
        textureKeep.push_back(texture);srvKeep.push_back(srv);result.uploadBytes+=rawR8?size_t(t.width)*t.height:texels.size()*sizeof(V4);
    }result.uploadWallMs=Millis(uploadStart);
    ComPtr<ID3D11SamplerState> linear,point;D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    Hr(device->CreateSamplerState(&sampler,&linear),"Create linear sampler");sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;Hr(device->CreateSamplerState(&sampler,&point),"Create point sampler");
    ID3D11SamplerState* samplers[]={linear.Get(),point.Get()};context->PSSetSamplers(0,2,samplers);
    D3D11_TEXTURE2D_DESC td{};td.Width=input.width;td.Height=input.height;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;Hr(device->CreateTexture2D(&td,nullptr,&target),"Create render target");ComPtr<ID3D11RenderTargetView> rtv;Hr(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"Create RTV");
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateTexture2D(&td,nullptr,&staging),"Create staging");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.ScissorEnable=TRUE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;Hr(device->CreateRasterizerState(&rd,&raster),"Create raster state");context->RSSetState(raster.Get());
    D3D11_RECT scissor{LONG(r[0x2081]&0x3FFF),LONG((r[0x2081]>>16)&0x3FFF),LONG(r[0x2082]&0x3FFF),LONG((r[0x2082]>>16)&0x3FFF)};
    if(scissor.right<=scissor.left || scissor.bottom<=scissor.top)scissor={0,0,LONG(input.width),LONG(input.height)};context->RSSetScissorRects(1,&scissor);
    D3D11_VIEWPORT vp{0,0,float(input.width),float(input.height),0,1};context->RSSetViewports(1,&vp);
    D3D11_BLEND_DESC bd{};auto& blend=bd.RenderTarget[0];blend.BlendEnable=TRUE;uint32_t b=r[0x2201];
    blend.SrcBlend=Blend(b&31,false);blend.DestBlend=Blend((b>>8)&31,false);blend.BlendOp=BlendOp((b>>5)&7);
    blend.SrcBlendAlpha=Blend((b>>16)&31,true);blend.DestBlendAlpha=Blend((b>>24)&31,true);blend.BlendOpAlpha=BlendOp((b>>21)&7);blend.RenderTargetWriteMask=r[0x2104]&15;
    ComPtr<ID3D11BlendState> blendState;Hr(device->CreateBlendState(&bd,&blendState),"Create blend state");float blendValues[4];for(int i=0;i<4;++i)blendValues[i]=AsFloat(r[0x2105+i]);context->OMSetBlendState(blendState.Get(),blendValues,~0u);
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;ComPtr<ID3D11DepthStencilState> depth;Hr(device->CreateDepthStencilState(&dd,&depth),"Create depth state");context->OMSetDepthStencilState(depth.Get(),0);
    auto rawRtv=rtv.Get();context->OMSetRenderTargets(1,&rawRtv,nullptr);context->IASetInputLayout(layout.Get());auto rawVb=vb.Get();UINT stride=sizeof(NativeVertex),offset=0;context->IASetVertexBuffers(0,1,&rawVb,&stride,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);auto rawCb=cb.Get();context->PSSetConstantBuffers(0,1,&rawCb);
    const float background[]={32/255.f,48/255.f,64/255.f,1};
    auto draw=[&]{context->ClearRenderTargetView(rtv.Get(),background);context->Draw(UINT(verts.size()),0);};
    draw(); // warm shader/pipeline creation outside the timestamp interval
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT,0};ComPtr<ID3D11Query> disjoint;Hr(device->CreateQuery(&qd,&disjoint),"Create disjoint query");
    qd.Query=D3D11_QUERY_TIMESTAMP;ComPtr<ID3D11Query> first,last,copied;Hr(device->CreateQuery(&qd,&first),"Create timestamp");Hr(device->CreateQuery(&qd,&last),"Create timestamp");Hr(device->CreateQuery(&qd,&copied),"Create timestamp");
    context->Begin(disjoint.Get());context->End(first.Get());for(unsigned i=0;i<repetitions;++i)draw();context->End(last.Get());
    auto readStart=Clock::now();context->CopyResource(staging.Get(),target.Get());context->End(copied.Get());context->End(disjoint.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Readback map");result.pixels.resize(size_t(input.width)*input.height);
    for(unsigned y=0;y<input.height;++y)std::memcpy(result.pixels.data()+size_t(y)*input.width,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,input.width*4);
    context->Unmap(staging.Get(),0);result.readbackWallMs=Millis(readStart);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT q{};auto query=[&](ID3D11Query* query,void* data,UINT size){auto start=Clock::now();for(;;){HRESULT h=context->GetData(query,data,size,0);if(h==S_OK)break;Hr(h,"GetData");Require(Millis(start)<10000,"GPU query timeout");std::this_thread::yield();}};
    query(disjoint.Get(),&q,sizeof(q));Require(!q.Disjoint && q.Frequency,"Disjoint GPU timestamps");uint64_t a,z,c;query(first.Get(),&a,8);query(last.Get(),&z,8);query(copied.Get(),&c,8);
    result.drawMs=double(z-a)*1000/q.Frequency/repetitions;result.copyGpuMs=double(c-z)*1000/q.Frequency;
    if(!planes.empty() && planes.size()==translated.textures.size()) {
        // Runtime-shaped conservative measurement: refresh every R8 plane,
        // upload the existing CPU surface even for this overwrite draw, execute,
        // read back, and copy all pixels into the CPU surface. Device/shader/
        // resource allocation remains outside this steady-state interval.
        std::vector<uint32_t> backgroundPixels(size_t(input.width)*input.height,0xFF203040);
        std::vector<double> times;
        for(unsigned frame=0;frame<20;++frame) {
            auto begin=Clock::now();
            for(const auto& p:planes)context->UpdateSubresource(p.texture,0,nullptr,p.bytes,p.pitch,0);
            context->UpdateSubresource(target.Get(),0,nullptr,backgroundPixels.data(),input.width*4,0);
            context->Draw(UINT(verts.size()),0);context->CopyResource(staging.Get(),target.Get());
            Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Streaming readback");
            for(unsigned y=0;y<input.height;++y)std::memcpy(result.pixels.data()+size_t(y)*input.width,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,input.width*4);
            context->Unmap(staging.Get(),0);times.push_back(Millis(begin));
        }
        std::sort(times.begin(),times.end());result.frameMedianMs=times[10];result.frameP95Ms=times[18];
    }
    return result;
}

static NativeResult RunMovieHelper(const DrawInput& input) {
    Require((input.initiator&63)==PRIM_QUAD_LIST && replayTriangles.size()==2,"Runtime helper fixture requires actual four-vertex QUADLIST");
    std::array<Vertex,4> quad={replayTriangles[0][0],replayTriangles[0][1],replayTriangles[0][2],replayTriangles[1][2]};
    auto code=LoadUcode(input.ps);const auto* regs=input.regs.data();
    Surface target;target.px.assign(EDRAM_PIXELS,0xFF203040);target.samplePitch=31337;
    size_t visible=size_t(input.width)*input.height;target.px[visible]=0xDEADBEEF;
    auto start=Clock::now();
    if(!TryD3D11MovieDraw(regs,code,quad.data(),4,target)) {
        for(const auto& v:quad)std::fprintf(stderr,"quad (%g,%g,%g invW=%g) valid=%d\n",v.x,v.y,v.z,v.invW,int(v.valid));
        throw std::runtime_error("Runtime movie helper rejected actual supported draw");
    }
    NativeResult result;result.adapter="production cached D3D11 movie helper";result.uploadWallMs=Millis(start);
    std::vector<double> times;
    for(unsigned i=0;i<24;++i) {
        std::fill(target.px.begin(),target.px.begin()+visible,0xFF203040);
        start=Clock::now();Require(TryD3D11MovieDraw(regs,code,quad.data(),4,target),"Cached runtime draw failed");times.push_back(Millis(start));
    }
    std::sort(times.begin(),times.end());result.frameMedianMs=times[12];result.frameP95Ms=times[22];result.pixels.assign(target.px.begin(),target.px.begin()+visible);
    Require(target.samplePitch==input.width && target.px[visible]==0xDEADBEEF,"Runtime commit touched outside target or lost pitch");
    // State/shader/resource failures must preserve every pixel, pitch and the
    // diagnostic counter, including a failure after a cached device exists.
    unsigned guards=0;
    auto rejected=[&](auto mutate,const char* name) {
        auto r=input.regs;auto q=quad;auto shader=code;mutate(r,q,shader);
        auto pixels=target.px;auto pitch=target.samplePitch;auto lit=GpuLitDraws();
        Require(!TryD3D11MovieDraw(r.data(),shader,q.data(),4,target),std::string("Guard accepted ")+name);
        Require(target.px==pixels && target.samplePitch==pitch && GpuLitDraws()==lit,std::string("Failed draw changed CPU state: ")+name);++guards;
    };
    rejected([](auto& r,auto&,auto&){r[0x2000]|=1<<16;},"MSAA");
    rejected([](auto& r,auto&,auto&){r[0x2200]|=2;},"depth test");
    rejected([](auto& r,auto&,auto&){r[0x2200]|=1;},"stencil");
    rejected([](auto& r,auto&,auto&){r[0x2201]=0x00010706;},"blending");
    rejected([](auto& r,auto&,auto&){r[0x2104]=7;},"write mask");
    rejected([](auto& r,auto&,auto&){r[0x2081]=1;},"scissor");
    rejected([](auto& r,auto&,auto&){r[0x2202]|=8;},"alpha test");
    rejected([](auto& r,auto&,auto&){r[0x4800]|=0x80000000u;},"tiled plane");
    rejected([](auto& r,auto&,auto&){r[0x4801]|=64;},"plane endian");
    rejected([](auto& r,auto&,auto&){r[0x4800]=(r[0x4800]&~(511u<<22))|(1<<22);},"short pitch");
    rejected([](auto&,auto& q,auto&){q[0].x=1;},"partial quad");
    rejected([](auto&,auto& q,auto&){q[0].invW=std::numeric_limits<float>::quiet_NaN();},"nonfinite vertex");
    rejected([](auto&,auto& q,auto&){q[0].invW=std::numeric_limits<float>::denorm_min();},"inverse overflow");
    rejected([](auto&,auto& q,auto&){q[0].invW=.01f;q[0].attr[0][0]=std::numeric_limits<float>::max();},"attribute overflow");
    rejected([](auto&,auto& q,auto&){q[0]=q[1];},"duplicate corner");
    rejected([](auto&,auto&,auto& c){c[1]=(c[1]&~0xF000u)|(7<<12);},"unsupported CF");
    rejected([](auto& r,auto&,auto&){r[0x4801]=(r[0x4801]&4095)|0xE0000000u;},"unreadable plane");
    // Changing a baked swizzle must select a new HLSL/PS cache key; restoring it
    // must reproduce the previous GPU image exactly.
    auto altered=input.regs;altered[0x4803]=(altered[0x4803]&~0x1FFEu)|(0xB6D<<1);
    Require(TryD3D11MovieDraw(altered.data(),code,quad.data(),4,target),"Swizzle cache rebuild failed");
    Require(!std::equal(result.pixels.begin(),result.pixels.end(),target.px.begin()),"Baked texture swizzle cache was stale");
    Require(TryD3D11MovieDraw(regs,code,quad.data(),4,target),"Restored shader draw failed");
    Require(std::equal(result.pixels.begin(),result.pixels.end(),target.px.begin()),"Restored shader cache changed pixels");
    auto preserved=target.px;auto preservedPitch=target.samplePitch;auto preservedLit=GpuLitDraws();
    replayFailAfterReadback=true;
    Require(!TryD3D11MovieDraw(regs,code,quad.data(),4,target),"Post-readback failure was accepted");
    Require(target.px==preserved && target.samplePitch==preservedPitch && GpuLitDraws()==preservedLit,"Post-readback failure partially committed CPU state");
    replayFailAfterReadback=false;
    Require(!D3D11MovieEnabled(),"Device failure did not latch software fallback");
    Require(!TryD3D11MovieDraw(regs,code,quad.data(),4,target),"Disabled backend retried after device failure");
    Require(target.px==preserved && GpuLitDraws()==preservedLit,"Disabled backend changed CPU state");
    std::puts("PASS injected post-readback failure: no partial CPU commit; device failure latched");
    std::printf("PASS runtime helper %u failure-preservation guards and swizzle-cache restoration\n",guards);
    return result;
}

static double EdgeDistance(float x,float y,const Vertex& a,const Vertex& b) {
    double dx=b.x-a.x,dy=b.y-a.y,n=dx*dx+dy*dy;if(n==0)return 1e9;
    double t=((x-a.x)*dx+(y-a.y)*dy)/n;if(t<0 || t>1)return 1e9;
    return std::fabs(dx*(y-a.y)-dy*(x-a.x))/std::sqrt(n);
}
static bool NearEdge(unsigned x,unsigned y) {
    // D3D11 rasterization quantizes vertices to an 8-bit subpixel grid. CPU
    // rasterization uses float edges. A 1/128-pixel band conservatively covers
    // the maximum per-vertex snapping shift, not an arbitrary image border.
    for(const auto& t:replayTriangles)for(unsigned i=0;i<3;++i)if(EdgeDistance(x+.5f,y+.5f,t[i],t[(i+1)%3])<=1./128)return true;
    return false;
}
static int RunReplay(int argc,char** argv) {
    Require(argc>=3,"Usage: replay.exe bundle.bin output-directory [native|manual] [negative-color]");
    DrawInput input=LoadInput(argv[1]);std::filesystem::path output=argv[2];std::filesystem::create_directories(output);
    if(argc>3 && std::string(argv[3])=="capture-only") {
        for(unsigned i=0;i<5;++i)Ttt2CaptureD3D11Draw(input.regs.data(),input.initiator,input.indexAddress,input.index32!=0,input.vs,input.ps);
        std::puts("PASS capture-only calls returned without changing guest state");return 0;
    }
    bool runtime=argc>3 && std::string(argv[3])=="runtime";
    bool native=runtime || argc<4 || std::string(argv[3])=="native",negative=argc>4;
    auto* r=input.regs.data();Require((r[0x2000]&0x3FFF)==input.width && !(r[0x2000]&(3<<16)),"Only matching-pitch single-sample targets supported");
    Require(!(r[0x2200]&2) && !(r[0x2205]&3),"Depth testing/culling outside pilot subset");
    auto translation=Translate(LoadUcode(input.ps),r,native);
    if(negative)translation.hlsl.insert(translation.hlsl.rfind("return o;"),"o.rgb=float3(1,0,1);\n");
    WriteBytes(output/"translated_ps.hlsl",translation.hlsl.data(),translation.hlsl.size());
    GpuFillEdram(r[0x2001]&4095,input.width,input.height,0xFF203040);auto cpuStart=Clock::now();
    GpuExecuteDraw(r,input.initiator,input.indexAddress,input.index32!=0,input.vs,input.ps);double cpuMs=Millis(cpuStart);recordTriangles=false;
    std::vector<uint32_t> cpu(size_t(input.width)*input.height);Require(GpuReadEdram(r[0x2001]&4095,input.width,input.height,cpu.data()),"CPU target missing");
    Require(!replayTriangles.empty(),"No complete triangles: missing vertex resources or invalid draw");
    NativeResult gpu=runtime?RunMovieHelper(input):RunNative(input,translation,32);
    uint64_t exact=0,within=0,edgeOnly=0,bad=0,changed=0,totalError=0;unsigned maxAll=0,maxInterior=0;std::vector<uint32_t> difference(cpu.size());
    for(size_t i=0;i<cpu.size();++i) {
        changed+=cpu[i]!=0xFF203040;unsigned max=0,sum=0;
        for(unsigned c=0;c<4;++c){unsigned delta=unsigned(std::abs(int((cpu[i]>>(c*8))&255)-int((gpu.pixels[i]>>(c*8))&255)));max=std::max(max,delta);sum+=delta;}
        totalError+=sum;maxAll=std::max(maxAll,max);bool edge=max>3 && NearEdge(unsigned(i%input.width),unsigned(i/input.width));
        if(!edge)maxInterior=std::max(maxInterior,max);
        if(!max)++exact;else if(max<=3)++within;else if(edge)++edgeOnly;else ++bad;
        difference[i]=max>3?0xFFFF0000:0xFF000000|(max*85u<<8);
    }
    Require(changed>100,"Oracle draw is empty or too small for a useful comparison");
    WriteBytes(output/"cpu.bgra",cpu.data(),cpu.size()*4);WriteBytes(output/"gpu.bgra",gpu.pixels.data(),gpu.pixels.size()*4);WriteBytes(output/"difference.bgra",difference.data(),difference.size()*4);
    std::ofstream report(output/"result.json");report<<"{\n  \"adapter\": \""<<gpu.adapter<<"\",\n  \"width\": "<<input.width<<", \"height\": "<<input.height<<",\n  \"filter\": \""<<(native?"native linear":"manual float bilinear")<<"\",\n  \"cpu_oracle_ms\": "<<cpuMs<<",\n  \"gpu_draw_clear_ms\": "<<gpu.drawMs<<",\n  \"gpu_copy_ms\": "<<gpu.copyGpuMs<<",\n  \"readback_wait_and_copy_wall_ms\": "<<gpu.readbackWallMs<<",\n  \"frame_upload_draw_readback_median_ms\": "<<gpu.frameMedianMs<<",\n  \"frame_upload_draw_readback_p95_ms\": "<<gpu.frameP95Ms<<",\n  \"decode_and_upload_wall_ms\": "<<gpu.uploadWallMs<<", \"upload_bytes\": "<<gpu.uploadBytes<<",\n  \"triangles\": "<<replayTriangles.size()<<", \"translated_instructions\": "<<translation.operations<<",\n  \"changed_pixels\": "<<changed<<", \"exact_pixels\": "<<exact<<", \"within_3_lsb_pixels\": "<<within<<",\n  \"subpixel_edge_only_pixels\": "<<edgeOnly<<", \"out_of_bounds_pixels\": "<<bad<<",\n  \"max_channel_error\": "<<maxAll<<", \"max_interior_channel_error\": "<<maxInterior<<",\n  \"mean_channel_error\": "<<double(totalError)/(cpu.size()*4)<<", \"passed\": "<<(bad==0?"true":"false")<<"\n}\n";
    std::printf("%s %s: %zu triangles, CPU %.3f ms, GPU %.3f ms, copy %.3f ms, frame median/p95 %.3f/%.3f ms, exact %llu, <=3LSB %llu, edge %llu, bad %llu, max %u/%u\n",bad?"FAIL":"PASS",gpu.adapter.c_str(),replayTriangles.size(),cpuMs,gpu.drawMs,gpu.copyGpuMs,gpu.frameMedianMs,gpu.frameP95Ms,exact,within,edgeOnly,bad,maxAll,maxInterior);
    return bad?1:0;
}
int main(int argc,char** argv) {try{return RunReplay(argc,argv);}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 2;}}
