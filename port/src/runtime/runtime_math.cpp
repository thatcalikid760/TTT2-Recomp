// Math helpers the recompiler emits that aren't in the CRT/libc++ (pre-C23).
// PPC round-to-nearest-even instructions (frin, vrfin) map to these.
#include <cmath>

extern "C" float roundevenf(float x) { return std::rintf(x); }   // rint uses round-to-nearest-even
extern "C" double roundeven(double x) { return std::rint(x); }
