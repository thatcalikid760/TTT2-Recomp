// Appended to gpu_render.cpp by test_gpu_hw_vs_translate.py (never part of the game build).
// For each vertex-shader microcode file: run the production CPU interpreter (RunShader, with
// the same PA_CL_VTE window transform as GpuExecuteDraw's shadeVertex) and the hardware
// translator's HLSL (as a compute kernel on the GPU) on the same randomized vertex data,
// constants, bool/loop constants and viewport state; compare the window-space position,
// 1/W, the sixteen interpolators and whether a position was exported.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <random>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;

static void Need(HRESULT hr, const char* what)
{
    if (FAILED(hr)) { std::fprintf(stderr, "FAIL %s %08X\n", what, (unsigned)hr); std::exit(3); }
}

int main(int argc, char** argv)
{
    // Guest space: reserve 4 GB, commit a vertex-data window (physical 0x01000000 -> A1000000).
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
    const uint32_t physWindow = 0x01000000u, slotBytes = 0x100000u;   // 1 MB per fetch slot
    VirtualAlloc(g_base + 0xA0000000u + physWindow, 97 * slotBytes, MEM_COMMIT, PAGE_READWRITE);

    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Need(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                           &device, nullptr, &context), "D3D11CreateDevice");
    constexpr uint32_t Items = 512;
    std::mt19937 rng;
    std::uniform_real_distribution<float> any(-2.0f, 2.0f), unit(0.0f, 1.0f);
    uint64_t totalChecks = 0, failures = 0, translated = 0, untranslatable = 0;
    std::map<std::string, int> skipReasons;

    for (int fi = 1; fi < argc; ++fi)
    {
        {
            std::string name = argv[fi];
            name = name.substr(name.find_last_of("/\\") + 1);
            uint32_t h = 2166136261u;
            for (char ch : name) h = (h ^ uint8_t(ch)) * 16777619u;
            rng.seed(20261011u ^ h);
        }
        std::ifstream f(argv[fi], std::ios::binary | std::ios::ate);
        const size_t bytes = (size_t)f.tellg(); f.seekg(0);
        std::vector<uint32_t> code(bytes / 4, 0);
        f.read(reinterpret_cast<char*>(code.data()), (std::streamsize)(code.size() * 4));
        ttt2_hw::VsTranslation t = ttt2_hw::TranslateVertexShader(code);
        if (!t.unsupported.empty())
        {
            ++untranslatable;
            ++skipReasons[t.unsupported];
            std::printf("SKIP %s: %s\n", argv[fi], t.unsupported.c_str());
            continue;
        }
        ++translated;
        ComPtr<ID3D11ComputeShader> cs;
        {
            const std::string hlsl = ttt2_hw::VsParityComputeHlsl(t);
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), argv[fi], nullptr, nullptr, "main", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors)))
            {
                std::printf("FAIL %s: HLSL compile\n%s\n", argv[fi], errors ? (const char*)errors->GetBufferPointer() : "");
                if (const char* dump = std::getenv("TTT2_PARITY_DUMP"))
                    if (FILE* df = std::fopen(dump, "w")) { std::fputs(hlsl.c_str(), df); std::fclose(df); }
                ++failures;
                continue;
            }
            Need(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs), "cs");
            // The production vertex shader must compile too (vs_5_0).
            const std::string vsText = ttt2_hw::VertexShaderHlsl(t);
            ComPtr<ID3DBlob> vblob, verrors;
            if (FAILED(D3DCompile(vsText.data(), vsText.size(), argv[fi], nullptr, nullptr, "main", "vs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &vblob, &verrors)))
            {
                std::printf("FAIL %s: vertex shader compile\n%s\n", argv[fi], verrors ? (const char*)verrors->GetBufferPointer() : "");
                ++failures;
                continue;
            }
        }

        // Register file: random vertex constants (all 512), bool/loop constants, fetch constants.
        static uint32_t regs[0x8000];
        std::memset(regs, 0, sizeof(regs));
        for (uint32_t i = 0; i < 512 * 4; ++i) { float v = any(rng); std::memcpy(&regs[0x4000 + i], &v, 4); }
        for (uint32_t i = 0; i < 8; ++i) regs[0x4900 + i] = rng();
        for (uint32_t i = 0; i < 32; ++i) regs[0x4908 + i] = (rng() % 5) | ((rng() % 4) << 8) | ((1 + rng() % 2) << 16);
        ttt2_hw::VsStateCB state{};
        std::memcpy(state.boolConsts, &regs[0x4900], sizeof(state.boolConsts));
        std::memcpy(state.loopConsts, &regs[0x4908], sizeof(state.loopConsts));
        // Viewport / VTE: the battle's 3D setting and the UI's pre-divided one, plus random terms.
        static const uint32_t vtes[] = { 0x43F, 0x300, 0x03F, 0x33F };
        state.vte = vtes[rng() % 4];
        state.xs = 320.0f + any(rng); state.xo = 320.0f + any(rng); state.ys = -360.0f + any(rng);
        state.yo = 360.0f + any(rng); state.zs = 0.5f + 0.1f * any(rng); state.zo = 0.5f + 0.1f * any(rng);
        state.pitch = 640.0f; state.rows = 2048.0f; state.noClip = 0;

        // Vertex data: every slot the shader fetches gets its own base; dwords are a mix of
        // small floats, raw random bits and byte-sized integers (colours, indices, weights).
        int64_t lo[96], hi[96];
        bool used[96];
        ttt2_hw::VsFetchRanges(t, 0, Items - 1, lo, hi, used);
        std::vector<uint32_t> vdata;
        bool tooBig = false;
        for (uint32_t slot = 0; slot < 96; ++slot)
        {
            if (!used[slot]) continue;
            const uint32_t phys = physWindow + slot * slotBytes + slotBytes / 2;   // room for negative offsets
            const uint32_t guest = 0xA0000000u + phys;
            if ((hi[slot] - lo[slot]) * 4 > int64_t(slotBytes / 2) || lo[slot] * 4 < -int64_t(slotBytes / 2)) { tooBig = true; break; }
            regs[0x4800 + slot * 2] = phys | 3u;                               // type 3 = vertex fetch constant
            regs[0x4800 + slot * 2 + 1] = 0x02000000u;
            state.vfRegion[slot][0] = int32_t(vdata.size() - lo[slot]);
            state.vfRegion[slot][1] = int32_t(lo[slot]);
            state.vfRegion[slot][2] = int32_t(hi[slot]);
            state.vfRegion[slot][3] = 1;
            for (int64_t d = lo[slot]; d < hi[slot]; ++d)
            {
                const uint32_t pick = rng() % 8;
                uint32_t value;
                if (pick < 6) { float x = pick < 2 ? unit(rng) : any(rng); std::memcpy(&value, &x, 4); }
                else if (pick < 7) value = rng();
                else value = (rng() & 0xFF) | ((rng() & 0xFF) << 8) | ((rng() & 0xFF) << 16) | ((rng() & 0xFF) << 24);
                const uint32_t be = __builtin_bswap32(value);
                std::memcpy(g_base + guest + d * 4, &be, 4);
                vdata.push_back(value);
            }
        }
        if (tooBig) { ++untranslatable; ++skipReasons["fetch range too large for the harness"]; continue; }
        if (vdata.empty()) vdata.push_back(0);

        D3D11_BUFFER_DESC b{};
        b.ByteWidth = 512 * 16; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb0, cb1, dataBuf, outBuf, readBuf;
        D3D11_SUBRESOURCE_DATA c0{ &regs[0x4000], 0, 0 };
        Need(device->CreateBuffer(&b, &c0, &cb0), "cb0");
        b.ByteWidth = sizeof(state);
        D3D11_SUBRESOURCE_DATA c1{ &state, 0, 0 };
        Need(device->CreateBuffer(&b, &c1, &cb1), "cb1");
        b = {}; b.ByteWidth = UINT(vdata.size() * 4); b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_SHADER_RESOURCE; b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA in{ vdata.data(), 0, 0 };
        Need(device->CreateBuffer(&b, &in, &dataBuf), "vdata");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.NumElements = UINT(vdata.size()); sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ComPtr<ID3D11ShaderResourceView> dataSrv;
        Need(device->CreateShaderResourceView(dataBuf.Get(), &sv, &dataSrv), "vdata srv");
        b = {}; b.ByteWidth = Items * 18 * 16; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; b.StructureByteStride = 16;
        Need(device->CreateBuffer(&b, nullptr, &outBuf), "out");
        b.Usage = D3D11_USAGE_STAGING; b.BindFlags = 0; b.CPUAccessFlags = D3D11_CPU_ACCESS_READ; b.MiscFlags = 0;
        Need(device->CreateBuffer(&b, nullptr, &readBuf), "read");
        ComPtr<ID3D11UnorderedAccessView> outUav;
        Need(device->CreateUnorderedAccessView(outBuf.Get(), nullptr, &outUav), "uav");

        context->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = { cb0.Get(), cb1.Get() };
        context->CSSetConstantBuffers(0, 2, cbs);
        ID3D11ShaderResourceView* srv = dataSrv.Get(); context->CSSetShaderResources(0, 1, &srv);
        ID3D11UnorderedAccessView* uav = outUav.Get(); context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch(Items / 64, 1, 1);
        context->CopyResource(readBuf.Get(), outBuf.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        Need(context->Map(readBuf.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
        std::vector<float> gpu(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + Items * 18 * 4);
        context->Unmap(readBuf.Get(), 0);

        uint32_t bad = 0, nonFinite = 0, validMismatch = 0, illConditioned = 0;
        double worst = 0;
        auto tolerance = [](float a) { return 1e-2 * std::max(1.0, std::fabs(double(a))); };
        for (uint32_t item = 0; item < Items; ++item)
        {
            ShaderCtx sc;
            sc.regs = regs; sc.pixel = false;
            sc.r[0][0] = (float)item;
            RunShader(sc, code);
            ++totalChecks;
            const float* g = &gpu[size_t(item) * 18 * 4];
            const bool gValid = g[17 * 4] != 0.0f;
            if (gValid != sc.wrotePos) { ++validMismatch; ++bad; continue; }
            float cpu[17][4] = {};
            // Raw random bits make positions near FLT_MAX: 1/W is then denormal, which GPUs flush
            // to zero (x * 1/W collapses). Real vertex data never gets there - count, don't compare.
            if (sc.wrotePos)
            {
                const V4& p = sc.exports[62];
                bool extreme = false;
                for (int c = 0; c < 4; ++c) extreme |= std::fabs(p[c]) > 1e30f;
                extreme |= (state.vte & 0x400) && std::fabs(p[3]) < 1e-30f;
                if (extreme) { ++illConditioned; continue; }
            }
            if (sc.wrotePos)
            {
                // GpuExecuteDraw shadeVertex: PA_CL_VTE_CNTL transform to window space.
                const V4& p = sc.exports[62];
                const uint32_t vte = state.vte;
                const float w0 = p[3];
                const float inv = (vte & 0x400) ? (w0 == 0.0f ? 1.0f : 1.0f / w0) : w0;
                float x = p[0], y = p[1], z = p[2];
                if (!(vte & 0x100)) { x *= inv; y *= inv; }
                if (!(vte & 0x200)) z *= inv;
                if (vte & 0x01) x *= state.xs;
                if (vte & 0x02) x += state.xo;
                if (vte & 0x04) y *= state.ys;
                if (vte & 0x08) y += state.yo;
                if (vte & 0x10) z *= state.zs;
                if (vte & 0x20) z += state.zo;
                cpu[0][0] = x; cpu[0][1] = y; cpu[0][2] = z; cpu[0][3] = inv;
            }
            for (int k = 0; k < 16; ++k)
                for (int c = 0; c < 4; ++c) cpu[1 + k][c] = (sc.exportMask & (1u << k)) ? sc.exports[k][c] : 0.0f;
            // NaN propagation differs by design (HLSL min/max/cmp drop a NaN operand, the
            // interpreter's std::max keeps it), so one NaN can steer later selects of the same
            // vertex. Random raw bits produce NaNs; real vertex data does not.
            bool nanItem = false;
            for (int r = sc.wrotePos ? 0 : 1; r < 17; ++r)
                for (int c = 0; c < 4; ++c) nanItem |= !std::isfinite(cpu[r][c]);
            if (nanItem) { ++illConditioned; ++nonFinite; continue; }
            int firstBad = -1;
            for (int r = sc.wrotePos ? 0 : 1; r < 17 && firstBad < 0; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    const float a = cpu[r][c], gv = g[r * 4 + c];
                    if (!std::isfinite(a)) { ++nonFinite; continue; }
                    const double err = std::fabs(double(a) - double(gv));
                    worst = std::max(worst, err / std::max(1.0, std::fabs(double(a))));
                    if (!(err <= tolerance(a))) { firstBad = r * 4 + c; break; }
                }
            if (firstBad < 0) continue;
            if (bad < 3)
                std::printf("  mismatch item %u %s comp %d: cpu=%.9g gpu=%.9g\n", item, firstBad < 4 ? "pos" : "interp",
                            firstBad, cpu[firstBad / 4][firstBad % 4], g[firstBad]);
            ++bad;
        }
        failures += bad != 0;
        if (bad)
            if (const char* dir = std::getenv("TTT2_PARITY_DUMPDIR"))
            {
                std::string name = argv[fi];
                name = name.substr(name.find_last_of("/\\") + 1) + ".hlsl";
                if (FILE* df = std::fopen((std::string(dir) + "/" + name).c_str(), "w"))
                { std::fputs(ttt2_hw::VsParityComputeHlsl(t).c_str(), df); std::fclose(df); }
            }
        std::printf("%s %s: %zu dwords, fetches=%zu, vte=%03X, %u items, valid mismatches=%u, bad=%u, ill-conditioned=%u, cpu-nonfinite=%u, worst rel err=%.3g\n",
                    bad ? "FAIL" : "PASS", argv[fi], code.size(), t.fetches.size(), state.vte, Items, validMismatch, bad, illConditioned, nonFinite, worst);
    }
    for (const auto& [reason, count] : skipReasons) std::printf("SKIPPED x%d: %s\n", count, reason.c_str());
    std::printf("SUMMARY translated=%llu untranslatable=%llu shader_failures=%llu checks=%llu\n",
                (unsigned long long)translated, (unsigned long long)untranslatable,
                (unsigned long long)failures, (unsigned long long)totalChecks);
    return failures ? 1 : 0;
}
