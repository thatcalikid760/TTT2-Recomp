#pragma once
// Minimal tracing for unimplemented kernel imports. Phase-1 scaffolding: each stub
// logs its name once so we can see, at runtime, exactly which imports the game reaches
// and in what order -- the work-list for Phase-2 real implementations.
#include <cstdio>
#include <unordered_set>
#include <string>
#include <mutex>

inline void KernelStub(const char* name)
{
    static std::unordered_set<std::string> seen;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(name).second)
        std::fprintf(stderr, "[kernel-stub] %s (first call)\n", name);
}

// Verbose call trace for implemented imports (toggle with TTT2_KTRACE env var during bring-up).
inline bool KTraceOn()
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("TTT2_KTRACE"); on = (e && *e && *e != '0') ? 1 : 0; }
    return on != 0;
}

void RollbackImportNote(const char* name);   // kernel/rollback.cpp: the online tasks' imports (diagnostics)
inline void KTrace(const char* name)
{
    if (KTraceOn()) std::fprintf(stderr, "[k] %s\n", name);
    RollbackImportNote(name);
}
