#pragma once
// Bounded raw Xenos pixel shader -> SM5 HLSL used by the headless oracle and
// opt-in movie backend. Include after the renderer instruction/texture types.
// Unsupported operations throw before any renderer surface can be modified.
#include <set>
#include <string>
#include <sstream>
#include <stdexcept>
namespace ttt2_d3d11 {
inline void Require(bool ok,const std::string& why) {if(!ok)throw std::runtime_error(why);}
struct Translation { std::string hlsl; std::set<unsigned> textures; unsigned operations=0; };
inline std::string Operand(const Alu& a,int n) {
    unsigned reg=a.srcRegister(n);bool temp=a.srcSelect(n);
    Require(!temp || !(reg&0x40),"Relative register source unsupported");
    std::string s=(temp?"r[":"c[")+std::to_string(temp?(reg&63):reg)+"].";
    for(int k=0;k<4;++k)s+="xyzw"[((a.srcSwizzle(n)>>(2*k))+k)&3];
    if(temp?(reg&128):a.absConstants())s="abs("+s+")";
    if(a.srcNegate(n))s="(-"+s+")";return s;
}
inline std::string ScalarOperand(const Alu& a,int which) {
    unsigned reg=a.srcRegister(3);bool temp=a.srcSelect(3);
    Require(!temp || !(reg&64),"Relative scalar register unsupported");
    unsigned component=which==0?((a.srcSwizzle(3)>>6)+3)&3:a.srcSwizzle(3)&3;
    std::string s=(temp?"r[":"c[")+std::to_string(temp?(reg&63):reg)+"]."+"xyzw"[component];
    if(temp?(reg&128):a.absConstants())s="abs("+s+")";
    if(a.srcNegate(3))s="(-"+s+")";return s;
}
inline std::string AlphaTest(const uint32_t* regs) {
    if(!(regs[0x2202]&8))return "";
    const char* test[]={"false","o.a < alphaRef","o.a == alphaRef","o.a <= alphaRef","o.a > alphaRef","o.a != alphaRef","o.a >= alphaRef","true"};
    return "if (!("+std::string(test[regs[0x2202]&7]) + ")) discard;\n";
}
inline Translation Translate(const std::vector<uint32_t>& code,const uint32_t* regs,bool nativeFilter) {
    Require(code.size()>=3 && code.size()<=8192 && code.size()%3==0,"Invalid shader length");
    Translation out;std::ostringstream body;unsigned cfEnd=unsigned(code.size());bool ended=false,exported=false;
    for(unsigned off=0;off+3<=cfEnd;off+=3) {
        Cf pair[2];DecodeCfPair(&code[off],pair[0],pair[1]);
        for(const auto& cf:pair)if(cf.isExec() && cf.addr())cfEnd=std::min(cfEnd,cf.addr()*3);
    }
    for(unsigned off=0;off+3<=cfEnd && !ended;off+=3) {
        Cf pair[2];DecodeCfPair(&code[off],pair[0],pair[1]);
        for(const auto& cf:pair) {
            if(cf.op()==CF_NOP || cf.op()==CF_ALLOC)continue;
            Require(cf.op()==CF_EXEC || cf.op()==CF_EXEC_END,"Conditional/loop/jump CF unsupported");
            Require(cf.addr()*3>=cfEnd && uint64_t(cf.addr()+cf.count())*3<=code.size(),"EXEC outside instruction range");
            for(unsigned i=0;i<cf.count();++i) {
                const uint32_t* w=&code[(cf.addr()+i)*3];++out.operations;
                if((cf.seq()>>(i*2))&1) {
                    TFetch t{w[0],w[1],w[2]};
                    Require(t.opcode()==1 && !(w[0]&((1u<<11)|(1u<<18))),"Only nonrelative texture fetch is supported");
                    Require(!(w[1]&0xC0000000u) && w[2]==0x4000,"Only plain unpredicated 2D fetch without LOD/offset is supported");
                    Require(t.magFilter()==0 || t.magFilter()==1 || t.magFilter()==3,"Unsupported filter override");
                    out.textures.insert(t.constIndex());
                    const std::string tn="tex"+std::to_string(t.constIndex());
                    body<<"{ float2 uv=r["<<t.srcRegister()<<"]."<<"xyzw"[t.srcSwizzle()&3]<<"xyzw"[(t.srcSwizzle()>>2)&3]<<";\n";
                    if(t.coordDenorm())body<<"uint tw,th; "<<tn<<".GetDimensions(tw,th); uv/=float2(tw,th);\n";
                    if(nativeFilter)body<<"float4 f="<<tn<<".SampleLevel("<<(t.magFilter()?"linearClamp":"pointClamp")<<",uv,0);\n";
                    else body<<"float4 f=load"<<(t.magFilter()?"Linear":"Point")<<"("<<tn<<",uv);\n";
                    Texture texture=DecodeTexture(regs,t.constIndex());
                    if(texture.format==FMT_8) {
                        // A native R8 SRV supplies (r,0,0,1); the production
                        // Xbox fetch expands luminance before its constant swizzle.
                        body<<"float4 expanded=float4(f.r,f.r,f.r,1); f=float4(";
                        for(unsigned k=0;k<4;++k){if(k)body<<",";unsigned s=(texture.swizzle>>(k*3))&7;body<<(s<4?std::string("expanded.")+"xyzw"[s]:s==4?"0.0":s==5?"1.0":std::string("expanded.")+"xyzw"[k]);}
                        body<<");\n";
                    }
                    for(unsigned k=0;k<4;++k) {
                        unsigned sw=(t.dstSwizzle()>>(k*3))&7;
                        Require(sw!=6,"Reserved texture destination swizzle");
                        if(sw<6)body<<"r["<<t.dstRegister()<<"]."<<"xyzw"[k]<<"="<<(sw<4?std::string("f.")+"xyzw"[sw]:sw==4?"0.0":"1.0")<<";\n";
                    }body<<"}\n";
                } else {
                    Alu a{w[0],w[1],w[2]};
                    Require(!(w[0]&(1u<<6)) && (a.exportData() || !(w[0]&(1u<<14))) && !a.isPredicated() && !a.carRelative() && !a.const0Relative() && !a.const1Relative(),"Relative/predicated ALU unsupported");
                    unsigned vm=a.vectorWriteMask(),sm=a.scalarWriteMask(),one=a.exportData()?(vm&sm):0;
                    unsigned zero=a.exportData() && (w[0]&(1<<14))?(~(vm|sm)&15):0;
                    if(a.exportData()){vm&=~sm;sm&=~a.vectorWriteMask();}
                    // The current CPU oracle evaluates scalar operands after
                    // its vector write. Reject coissue aliases rather than
                    // claiming parity for that broader instruction class.
                    Require(!sm || !vm || a.exportData() || !a.srcSelect(3) ||
                        (a.srcRegister(3)&63)!=a.vectorDest(),"Scalar/vector coissue alias unsupported");
                    Require(a.scalarOpcode()==S_RETAIN_PREV || a.scalarOpcode()==S_SUBS || a.scalarOpcode()==S_ADDS || a.scalarOpcode()==S_MULS || a.scalarOpcode()==S_MAXS,"Scalar ALU opcode unsupported");
                    Require(a.scalarOpcode()!=S_RETAIN_PREV || sm==0,"RetainPrev scalar write unsupported");
                    Require(a.vectorOpcode()<=V_MIN || a.vectorOpcode()==V_MAD,"Only ADD/MUL/MAX/MIN/MAD vector ALU supported");
                    Require(!a.exportData() || a.vectorDest()==0,"Only color0 export supported");
                    std::string x=Operand(a,1),y=Operand(a,2),expr;
                    if(a.vectorOpcode()==V_ADD)expr="("+x+"+"+y+")";
                    else if(a.vectorOpcode()==V_MUL)expr="("+x+"*"+y+")";
                    else if(a.vectorOpcode()==V_MAD)expr="("+x+"*"+y+"+"+Operand(a,3)+")";
                    else expr=std::string(a.vectorOpcode()==V_MAX?"max(":"min(")+x+","+y+")";
                    if(a.vectorSaturate())expr="saturate("+expr+")";
                    body<<"{ precise float4 v="<<expr<<";\n";
                    if(sm) {
                        auto a0=ScalarOperand(a,0),a1=ScalarOperand(a,1);std::string scalar;
                        if(a.scalarOpcode()==S_SUBS)scalar="("+a0+"-"+a1+")";
                        else if(a.scalarOpcode()==S_ADDS)scalar="("+a0+"+"+a1+")";
                        else if(a.scalarOpcode()==S_MULS)scalar="("+a0+"*"+a1+")";
                        else scalar="max("+a0+","+a1+")";
                        if(a.scalarSaturate())scalar="saturate("+scalar+")";
                        body<<"precise float s="<<scalar<<";\n";
                    }
                    for(unsigned k=0;k<4;++k) {
                        std::string vd=a.exportData()?"o":"r["+std::to_string(a.vectorDest())+"]";
                        std::string sd=a.exportData()?"o":"r["+std::to_string(a.scalarDest())+"]";
                        if(vm&(1<<k))body<<vd<<"."<<"xyzw"[k]<<"=v."<<"xyzw"[k]<<";\n";
                        if(sm&(1<<k))body<<sd<<"."<<"xyzw"[k]<<"=s;\n";
                        if(one&(1<<k))body<<"o."<<"xyzw"[k]<<"=1.0;\n";
                        if(zero&(1<<k))body<<"o."<<"xyzw"[k]<<"=0.0;\n";
                    }
                    body<<"}\n";exported|=a.exportData() && (vm|sm|one|zero);
                }
            }if(cf.op()==CF_EXEC_END){ended=true;break;}
        }
    }
    Require(ended && exported,"Shader has no complete color export");
    std::string pre=R"(
cbuffer PixelConstants : register(b0) { float4 c[256]; float alphaRef; };
SamplerState linearClamp:register(s0); SamplerState pointClamp:register(s1);
struct PixelInput { float4 pos:SV_Position; float4 a[16]:TEXCOORD0; };
float4 texel(Texture2D<float4> t,int2 p,int2 size) {return t.Load(int3(clamp(p,int2(0,0),size-1),0));}
float4 loadPoint(Texture2D<float4> t,float2 uv) {uint w,h;t.GetDimensions(w,h);return texel(t,int2(floor(uv*float2(w,h))),int2(w,h));}
float4 loadLinear(Texture2D<float4> t,float2 uv) {
 uint w,h;t.GetDimensions(w,h);precise float2 f=uv*float2(w,h)-0.5;int2 p=int2(floor(f));precise float2 b=f-float2(p);int2 s=int2(w,h);
 precise float4 a=texel(t,p,s),z=texel(t,p+int2(1,0),s),c0=texel(t,p+int2(0,1),s),d=texel(t,p+1,s);
 return (a*(1-b.x)+z*b.x)*(1-b.y)+(c0*(1-b.x)+d*b.x)*b.y;
}
)";
    for(unsigned t:out.textures)pre+="Texture2D<float4> tex"+std::to_string(t)+":register(t"+std::to_string(t)+");\n";
    out.hlsl=pre+"float4 main(PixelInput input):SV_Target { float4 r[64]; [unroll] for(int j=0;j<64;++j)r[j]=0; [unroll] for(int j=0;j<16;++j)r[j]=input.a[j]; float4 o=0;\n"+body.str()+AlphaTest(regs)+"return o; }\n";
    return out;
}

} // namespace ttt2_d3d11
