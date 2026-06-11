// Verification scratch — proves the two "C++ collision" claims that
// excused retrofit deviations are FALSE, i.e. the reference shapes
// compile verbatim in C++23:
//
//   1. `typedef double complex[2];` at namespace scope does NOT
//      collide with std::complex (std::complex lives in namespace
//      std; innermost-scope lookup wins inside lyra::wire even
//      under a using-directive).
//   2. `typedef struct _cmb { ... } cmb, *CMB;` is valid C++.
//   3. Raw function-pointer field + plain free-function registration
//      (the reference Outbound shape) compiles and dispatches.

#include <complex>   // deliberately included to provoke the claimed collision
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace lyra::wire {

typedef double complex[2];                       // claim 1
static_assert(sizeof(complex) == 2 * sizeof(double));

typedef struct _cmb                              // claim 2
{
    int   id;
    double* r1_baseptr;
    volatile long run;
    HANDLE Sem_BuffReady;
    CRITICAL_SECTION csOUT;
    void (*Outbound)(int id, int nsamples, double* buff);  // claim 3
} cmb, *CMB;

void OutBound(int id, int nsamples, double* in)  // free fn, reference shape
{
    (void)id; (void)nsamples; (void)in;
}

int selftest()
{
    CMB a = (CMB) calloc(1, sizeof(cmb));
    a->r1_baseptr = (double*) calloc(16, sizeof(complex));
    std::memcpy(a->r1_baseptr, a->r1_baseptr, 4 * sizeof(complex));
    a->Outbound = OutBound;                      // raw fn-ptr registration
    (*a->Outbound)(1, 4, a->r1_baseptr);         // reference dispatch shape
    std::complex<double> z{1.0, 2.0};            // std::complex coexists fine
    free(a->r1_baseptr);
    free(a);
    return static_cast<int>(z.real());
}

}  // namespace lyra::wire

int main() { return lyra::wire::selftest() == 1 ? 0 : 1; }
