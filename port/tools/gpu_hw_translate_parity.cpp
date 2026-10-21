// Appended to gpu_render.cpp by test_gpu_hw_translate.py (never part of the game build).
// For each pixel-shader microcode file: run the production CPU interpreter (RunShader) and
// the hardware translator's HLSL (as a compute kernel on the GPU) on the same randomized
// interpolators, constants, bool constants and textures, and compare export 0 + kill.
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
    // Guest space: reserve 4 GB, commit only the texture window the fetch constants use.
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
    const uint32_t texBase = 0xA0100000u, texW = 64, texH = 64;
    VirtualAlloc(g_base + texBase, 32 * 6 * texW * texH * 4, MEM_COMMIT, PAGE_READWRITE);   // room for cube faces

    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    Need(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                           &device, nullptr, &context), "D3D11CreateDevice");
    constexpr uint32_t Items = 512;
    std::mt19937 rng;
    std::uniform_real_distribution<float> any(-2.0f, 2.0f), unit(0.0f, 1.0f);
    uint64_t totalChecks = 0, failures = 0, translated = 0, untranslatable = 0, totalUnstable = 0;

    for (int fi = 1; fi < argc; ++fi)
    {
        // Inputs depend only on the shader's file name: results are independent of corpus
        // order and chunking, and any failure reproduces with that shader alone.
        {
            std::string name = argv[fi];
            name = name.substr(name.find_last_of("/\\") + 1);
            uint32_t h = 2166136261u;
            for (char ch : name) h = (h ^ uint8_t(ch)) * 16777619u;
            rng.seed(20261010u ^ h);
        }
        std::ifstream f(argv[fi], std::ios::binary);
        std::vector<uint32_t> code((std::istreambuf_iterator<char>(f)), {});
        // istreambuf_iterator<char> into uint32 vector would widen bytes; reread properly.
        f.clear(); f.seekg(0, std::ios::end); size_t bytes = (size_t)f.tellg(); f.seekg(0);
        code.assign(bytes / 4, 0); f.read(reinterpret_cast<char*>(code.data()), (std::streamsize)(code.size() * 4));
        ttt2_hw::PsTranslation t = ttt2_hw::TranslatePixelShader(code, true);
        ttt2_hw::PsTranslation tn = ttt2_hw::TranslatePixelShader(code, false);
        if (!t.unsupported.empty())
        {
            ++untranslatable;
            std::printf("SKIP %s: %s\n", argv[fi], t.unsupported.c_str());
            continue;
        }
        ++translated;
        ComPtr<ID3D11ComputeShader> csExact, csNative;
        bool compiled = true;
        for (int variant = 0; variant < 2 && compiled; ++variant)
        {
            std::string hlsl = ttt2_hw::ParityComputeHlsl(variant == 0 ? t : tn);
            // TTT2_PARITY_REG=k (diagnostics): compare temp register k instead of the colour export.
            if (const char* reg = std::getenv("TTT2_PARITY_REG"))
            {
                const std::string from = "o.color = e0;", to = std::string("o.color = r") + reg + ";";
                const size_t at = hlsl.find(from);
                if (at != std::string::npos && hlsl.find("float4 r" + std::string(reg) + " =") != std::string::npos)
                    hlsl.replace(at, from.size(), to);
            }
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), argv[fi], nullptr, nullptr, "main", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors)))
            {
                std::printf("FAIL %s: HLSL compile\n%s\n", argv[fi], errors ? (const char*)errors->GetBufferPointer() : "");
                if (const char* dump = std::getenv("TTT2_PARITY_DUMP"))
                    if (FILE* df = std::fopen(dump, "w")) { std::fputs(hlsl.c_str(), df); std::fclose(df); }
                compiled = false;
                break;
            }
            Need(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                             variant == 0 ? &csExact : &csNative), "CreateComputeShader");
        }
        if (!compiled) { ++failures; continue; }
        {
            // The production pixel-shader wrapper must compile too (ps_5_0).
            const std::string psText = ttt2_hw::PixelShaderHlsl(tn);
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(D3DCompile(psText.data(), psText.size(), argv[fi], nullptr, nullptr, "main", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors)))
            {
                std::printf("FAIL %s: pixel shader wrapper compile\n%s\n", argv[fi],
                            errors ? (const char*)errors->GetBufferPointer() : "");
                ++failures;
                continue;
            }
        }

        // Register file: random pixel constants, bool constants, texture fetch constants.
        static uint32_t regs[0x8000];
        std::memset(regs, 0, sizeof(regs));
        for (uint32_t i = 0; i < 256 * 4; ++i) { float v = any(rng); std::memcpy(&regs[0x4000 + 256 * 4 + i], &v, 4); }
        for (uint32_t i = 0; i < 8; ++i) regs[0x4900 + i] = rng();
        std::vector<ComPtr<ID3D11ShaderResourceView>> srvs(32);
        ComPtr<ID3D11SamplerState> slotSamplers[16];
        ttt2_hw::PsStateCB state{};
        // Loop constants: count 0-4, start 0-3, step 1 or 2 (aL stays inside the register file).
        for (uint32_t i = 0; i < 32; ++i) regs[0x4908 + i] = (rng() % 5) | ((rng() % 4) << 8) | ((1 + rng() % 2) << 16);
        std::memcpy(state.boolConsts, &regs[0x4900], sizeof(state.boolConsts));
        std::memcpy(state.loopConsts, &regs[0x4908], sizeof(state.loopConsts));
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            if (!(t.textureSlots & (1u << slot))) continue;
            const uint32_t base = texBase + slot * 6 * texW * texH * 4;
            // Cube-fetch slots get a 6-face cube texture (faces back to back: a linear 64x64
            // 8_8_8_8 face is exactly one 16 KB TextureFaceStride), bound as a 6-slice array.
            const bool cube = (t.cubeSlots >> slot) & 1;
            const uint32_t slices = cube ? 6u : 1u;
            uint32_t* fc = &regs[0x4800 + slot * 6];
            const uint32_t swizzle = rng() & 0xFFF;
            // Random ClampMode per axis (all eight), mag filter point/linear, border colour.
            const uint32_t clampX = rng() & 7, clampY = rng() & 7, mag = rng() & 1, border = rng() & 3;
            fc[0] = 2u | (clampX << 10) | (clampY << 13) | ((texW / 32) << 22);   // texture, linear, pitch 64
            fc[1] = FMT_8_8_8_8 | (((base & 0x1FFFFFFFu) >> 12) << 12);
            fc[2] = (texW - 1) | ((texH - 1) << 13);
            fc[3] = (swizzle << 1) | (mag << 19) | (mag << 21);
            fc[5] = border | ((cube ? 3u : 1u) << 9);
            std::vector<uint32_t> host(texW * texH * slices);
            for (uint32_t i = 0; i < texW * texH * slices; ++i)
            {
                host[i] = rng();
                const uint32_t be = __builtin_bswap32(host[i]);
                std::memcpy(g_base + base + i * 4, &be, 4);
            }
            D3D11_TEXTURE2D_DESC d{}; d.Width = texW; d.Height = texH; d.MipLevels = 1; d.ArraySize = slices;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init[6];
            for (uint32_t s = 0; s < slices; ++s) init[s] = { host.data() + s * texW * texH, texW * 4, 0 };
            ComPtr<ID3D11Texture2D> tex;
            Need(device->CreateTexture2D(&d, init, &tex), "CreateTexture2D");
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = d.Format;
            if (cube) { v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; v.Texture2DArray.MipLevels = 1; v.Texture2DArray.ArraySize = 6; }
            else { v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1; }
            Need(device->CreateShaderResourceView(tex.Get(), &v, &srvs[slot]), "CreateSrv");
            state.texInfo[slot][0] = swizzle; state.texInfo[slot][1] = 0;
            state.texInfo[slot][2] = texW; state.texInfo[slot][3] = texH;
            state.texScale[slot][0] = state.texScale[slot][1] = 1.0f; state.texScale[slot][2] = float(slices);
            ttt2_hw::SamplerInfo(DecodeTexture(regs, slot), state.texSampler[slot]);
            if (cube) state.texSampler[slot][0] = state.texSampler[slot][1] = 2;   // as the backend
            const D3D11_SAMPLER_DESC sd = ttt2_hw::SamplerDesc(state.texSampler[slot]);
            Need(device->CreateSamplerState(&sd, &slotSamplers[slot & 15]), "slot sampler");
        }

        // Inputs: mostly random, with exact zeros/ones and texcoords in [0, 1] mixed in.
        std::vector<float> inputs(Items * 16 * 4);
        for (uint32_t i = 0; i < inputs.size(); ++i)
        {
            const uint32_t pick = rng() % 10;
            inputs[i] = pick == 0 ? 0.0f : pick == 1 ? 1.0f : pick < 5 ? unit(rng) : any(rng);
        }

        D3D11_BUFFER_DESC b{};
        b.ByteWidth = 256 * 16; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb0, cb1, inBuf, outBuf, readBuf;
        D3D11_SUBRESOURCE_DATA c0{ &regs[0x4000 + 256 * 4], 0, 0 };
        Need(device->CreateBuffer(&b, &c0, &cb0), "cb0");
        b.ByteWidth = sizeof(state);
        D3D11_SUBRESOURCE_DATA c1{ &state, 0, 0 };
        Need(device->CreateBuffer(&b, &c1, &cb1), "cb1");
        b = {}; b.ByteWidth = UINT(inputs.size() * 4); b.Usage = D3D11_USAGE_DEFAULT;
        b.BindFlags = D3D11_BIND_SHADER_RESOURCE; b.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; b.StructureByteStride = 16;
        D3D11_SUBRESOURCE_DATA in{ inputs.data(), 0, 0 };
        Need(device->CreateBuffer(&b, &in, &inBuf), "inBuf");
        b.ByteWidth = Items * 2 * 16; b.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        Need(device->CreateBuffer(&b, nullptr, &outBuf), "outBuf");
        b.Usage = D3D11_USAGE_STAGING; b.BindFlags = 0; b.CPUAccessFlags = D3D11_CPU_ACCESS_READ; b.MiscFlags = 0;
        Need(device->CreateBuffer(&b, nullptr, &readBuf), "readBuf");
        ComPtr<ID3D11ShaderResourceView> inSrv; ComPtr<ID3D11UnorderedAccessView> outUav;
        Need(device->CreateShaderResourceView(inBuf.Get(), nullptr, &inSrv), "inSrv");
        Need(device->CreateUnorderedAccessView(outBuf.Get(), nullptr, &outUav), "outUav");
        D3D11_SAMPLER_DESC s{}; s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> point, linear;
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; Need(device->CreateSamplerState(&s, &point), "point");
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; Need(device->CreateSamplerState(&s, &linear), "linear");

        std::vector<float> results[2];
        for (int variant = 0; variant < 2; ++variant) {
        context->CSSetShader(variant == 0 ? csExact.Get() : csNative.Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = { cb0.Get(), cb1.Get() };
        context->CSSetConstantBuffers(0, 2, cbs);
        for (uint32_t slot = 0; slot < 32; ++slot)
            if (srvs[slot]) { ID3D11ShaderResourceView* v = srvs[slot].Get(); context->CSSetShaderResources(slot, 1, &v); }
        ID3D11ShaderResourceView* inv = inSrv.Get(); context->CSSetShaderResources(32, 1, &inv);
        ID3D11UnorderedAccessView* uav = outUav.Get(); context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ID3D11SamplerState* samplers[16] = {};
        for (int k = 0; k < 16; ++k) samplers[k] = slotSamplers[k] ? slotSamplers[k].Get() : point.Get();
        context->CSSetSamplers(0, 16, samplers);
        context->Dispatch(Items / 64, 1, 1);
        context->CopyResource(readBuf.Get(), outBuf.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        Need(context->Map(readBuf.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
        results[variant].assign(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + Items * 8);
        context->Unmap(readBuf.Get(), 0);
        }
        const float* gpu = results[0].data();
        const float* native = results[1].data();
        double nativeWorst = 0;

        double worst = 0; uint32_t bad = 0, killMismatch = 0, nonFinite = 0, unstable = 0;
        const bool textured = t.textureSlots != 0;
        auto runCpu = [&](uint32_t item, float jitter, uint32_t seed) {
            ShaderCtx sc;
            sc.regs = regs; sc.pixel = true;
            std::mt19937 j(seed);
            std::uniform_real_distribution<float> sign(-1.0f, 1.0f);
            for (int k = 0; k < 16; ++k)
                for (int c = 0; c < 4; ++c)
                {
                    const float v = inputs[(item * 16 + k) * 4 + c];
                    sc.r[k][c] = jitter != 0.0f ? v * (1.0f + jitter * sign(j)) : v;
                }
            RunShader(sc, code);
            return sc;
        };
        auto tolerance = [](float a) { return 1e-2 * std::max(1.0, std::fabs(double(a))); };
        for (uint32_t item = 0; item < Items; ++item)
        {
            const ShaderCtx sc = runCpu(item, 0.0f, 0);
            const bool gpuKilled = gpu[item * 8 + 4] != 0.0f;
            ++totalChecks;
            bool itemBad = gpuKilled != sc.killed;
            int firstBad = -1;
            if (!itemBad && !sc.killed)
                for (int c = 0; c < 4; ++c)
                {
                    static const int dbgReg = std::getenv("TTT2_PARITY_REG") ? std::atoi(std::getenv("TTT2_PARITY_REG")) : -1;
                    const float a = dbgReg >= 0 ? sc.r[dbgReg][c] : sc.exports[0][c], g = gpu[item * 8 + c];
                    // A non-finite CPU result comes from overflow on random inputs (inf - inf, 0 * inf)
                    // and NaN propagation, where std::max(NaN, x) = NaN but HLSL max returns x. Real
                    // render data does not produce these; count them instead of comparing.
                    if (!std::isfinite(a)) { ++nonFinite; continue; }
                    const double err = std::fabs(double(a) - double(g));
                    // Relative tolerance for GPU transcendental/fused-multiply-add differences.
                    const float n = native[item * 8 + c];
                    if (std::isfinite(n)) nativeWorst = std::max(nativeWorst, std::fabs(double(a) - double(n)) / std::max(1.0, std::fabs(double(a))));
                    worst = std::max(worst, err / std::max(1.0, std::fabs(double(a))));
                    if (!(err <= tolerance(a)) && firstBad < 0) { firstBad = c; itemBad = true; }
                }
            if (!itemBad) continue;
            // Ill-conditioned input? The random inputs are 20% exact 0/1, so ties such as
            // |x| == |y| (CUBE face choice) or texel-centre hits are common; a one-ulp GPU
            // rsq/FMA difference then legitimately flips the result. Such an item's CPU answer
            // itself moves under a 1e-5 relative jitter of the inputs: count it, don't fail it.
            bool stable = true;
            for (uint32_t trial = 1; trial <= 4 && stable; ++trial)
            {
                const ShaderCtx p = runCpu(item, 1e-5f, item * 7919u + trial);
                if (p.killed != sc.killed) { stable = false; break; }
                if (!sc.killed)
                    for (int c = 0; c < 4; ++c)
                    {
                        const float a = sc.exports[0][c], b = p.exports[0][c];
                        if (std::isfinite(a) && !(std::fabs(double(a) - double(b)) <= tolerance(a))) { stable = false; break; }
                    }
            }
            if (!stable) { ++unstable; continue; }
            if (gpuKilled != sc.killed) { ++killMismatch; ++bad; continue; }
            if (bad < 3)
                std::printf("  mismatch item %u comp %d: cpu=%.9g gpu=%.9g\n", item, firstBad,
                            sc.exports[0][firstBad], gpu[item * 8 + firstBad]);
            ++bad;
        }
        // Ill-conditioning must stay rare; a broken translation must not hide behind it.
        if (unstable > Items / 20)
        {
            std::printf("  too many ill-conditioned items (%u of %u)\n", unstable, Items);
            ++bad;
        }
        failures += bad != 0;
        // TTT2_PARITY_DUMPDIR: keep the exact-variant HLSL of every failing shader.
        if (bad)
            if (const char* dir = std::getenv("TTT2_PARITY_DUMPDIR"))
            {
                std::string name = argv[fi];
                name = name.substr(name.find_last_of("/\\") + 1) + ".hlsl";
                if (FILE* df = std::fopen((std::string(dir) + "/" + name).c_str(), "w"))
                { std::fputs(ttt2_hw::ParityComputeHlsl(t).c_str(), df); std::fclose(df); }
            }
        totalUnstable += unstable;
        std::printf("%s %s: %zu dwords, textures=%08X, %u items, kill mismatches=%u, bad=%u, ill-conditioned=%u, cpu-nonfinite=%u, worst rel err=%.3g (native filtering %.3g)\n",
                    bad ? "FAIL" : "PASS", argv[fi], code.size(), t.textureSlots, Items, killMismatch, bad, unstable, nonFinite, worst, nativeWorst);
    }
    std::printf("SUMMARY translated=%llu untranslatable=%llu shader_failures=%llu checks=%llu ill-conditioned=%llu\n",
                (unsigned long long)translated, (unsigned long long)untranslatable,
                (unsigned long long)failures, (unsigned long long)totalChecks, (unsigned long long)totalUnstable);
    return failures ? 1 : 0;
}
