// Standalone unit test for the P0.b ILV direct port (verbatim
// ChannelMaster/ilv.{h,c}).
//
// Build (from repo root, in a vcvars64 shell):
//   cl /std:c++latest /EHsc /O2 /I src ^
//      scratch\test_ilv.cpp src\wire\ILV.cpp src\wire\cmcomm.cpp ^
//      /Fe:scratch\test_ilv.exe
//   .\scratch\test_ilv.exe
//
// ILV.cpp's setters reach the ILV via `pcm->xmtr[xmtr_id].pilv`
// exactly as the reference does; this standalone test defines its
// own `cm`/`pcm` globals (declared extern in wire/CMaster.h) so it
// links without the full wire layer.
//
// Tests cover the verbatim surface end-to-end:
//   * create_ilv + destroy_ilv lifecycle (field stores, outbuff)
//   * SetILV{Run, What, Insize, OutboundId, OutputPointer} via
//     pcm->xmtr[0].pilv + the pSetILV{Run, Insize} direct variants
//   * xilv run=0 bypass fast path (memcpy data[0] -> outbuff,
//     Outbound dispatched with insize)
//   * xilv run=1 interleave: bit-exact 2-stream interleave
//     (what=0b11) vs hand-computed reference
//   * xilv run=1 interleave: bit-exact 4-stream interleave with
//     a gap (what=0b1011: streams 0+1+3, stream 2 skipped) ->
//     output ordering matches reference's i-bit-scan order
//   * xilv Outbound receives obid + k + outbuff contents
//   * SetILVOutputPointer swap is observable on the very next xilv
//
// No external dependencies beyond the project source tree + MSVC.

#include "wire/ILV.h"
#include "wire/CMaster.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// The test's own cmaster globals (CMaster.h declares them extern;
// CMaster.cpp is deliberately NOT linked here).
namespace lyra::wire {
cmaster cm = {0};
CMASTER  pcm = &cm;

// The reference defines these in ilv.c as PORT (dllexport) WITHOUT
// an ilv.h declaration — the external consumer (Thetis C# side)
// re-declares them via DllImport.  This test is the external
// consumer, so it re-declares them the same way.
void SetILVRun(int xmtr_id, int run);
void SetILVWhat(int xmtr_id, int stream, int state);
void SetILVInsize(int xmtr_id, int size);
void SetILVOutboundId(int xmtr_id, int obid);
}

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    } else {
        std::printf("  pass: %s\n", what);
    }
}

void check_eq(double got, double want, const char* what, double tol = 1e-12) {
    const bool ok = std::fabs(got - want) <= tol;
    if (!ok) {
        ++failures;
        std::printf("  FAIL: %s  (got %.15g, want %.15g)\n", what, got, want);
    } else {
        std::printf("  pass: %s\n", what);
    }
}

void check_eq_int(int got, int want, const char* what) {
    if (got != want) {
        ++failures;
        std::printf("  FAIL: %s  (got %d, want %d)\n", what, got, want);
    } else {
        std::printf("  pass: %s\n", what);
    }
}

// Last Outbound() invocation recorded by the test harness.  The
// verbatim port's Outbound is a raw C function pointer, so the
// recorder is a free function writing globals (no captures).
struct OutboundRecord {
    int id = -1;
    int nsamples = 0;
    std::vector<double> buff;
    int invocations = 0;
};

OutboundRecord g_last;
int g_swap_seen_id = -1;

void record_outbound(int id, int nsamples, double* buff) {
    g_last.id = id;
    g_last.nsamples = nsamples;
    g_last.buff.assign(buff, buff + static_cast<std::size_t>(2 * nsamples));
    ++g_last.invocations;
}

void swap_outbound(int id, int /*nsamples*/, double* /*buff*/) {
    g_swap_seen_id = id;
}

void reset_last() {
    g_last = OutboundRecord{};
    g_swap_seen_id = -1;
}

// ====================================================================
void testLifecycle() {
    std::printf("[test] create_ilv / destroy_ilv lifecycle\n");

    using namespace lyra::wire;
    ILV a = create_ilv(/*run*/0,
                       /*outbound_id*/7,
                       /*insize*/4,
                       /*ninputs*/2,
                       /*what*/0,
                       record_outbound);

    check(a != nullptr, "create_ilv returned non-null");
    check_eq_int(a->obid,   7, "obid stored");
    check_eq_int(a->insize, 4, "insize stored");
    check_eq_int(a->nin,    2, "nin stored");
    check_eq_int(static_cast<int>(a->run),  0, "run starts 0");
    check_eq_int(static_cast<int>(a->what), 0, "what starts 0");
    check(a->outbuff != nullptr, "outbuff allocated (nin*insize*sizeof(complex))");
    check(a->Outbound == &record_outbound, "Outbound fn ptr stored verbatim");

    destroy_ilv(a);
    check(true, "destroy_ilv completed (_aligned_free pair)");
}

// ====================================================================
void testSetters() {
    std::printf("[test] setters (pcm->xmtr[0].pilv lookup + intrinsics)\n");

    using namespace lyra::wire;
    ILV a = create_ilv(0, 0, 16, 4, 0, record_outbound);
    pcm->xmtr[0].pilv = a;   // the reference's create_xmtr publish point

    // SetILVRun
    SetILVRun(0, 1);
    check_eq_int(static_cast<int>(a->run & 1), 1, "SetILVRun(1) sets bit 0");
    SetILVRun(0, 0);
    check_eq_int(static_cast<int>(a->run & 1), 0, "SetILVRun(0) clears bit 0");

    // SetILVWhat
    SetILVWhat(0, 3, 1);
    SetILVWhat(0, 7, 1);
    check_eq_int(static_cast<int>(a->what & ((1L<<3)|(1L<<7))),
                 (1<<3)|(1<<7),
                 "SetILVWhat sets two bits");
    SetILVWhat(0, 3, 0);
    check_eq_int(static_cast<int>(a->what & (1L<<3)), 0,
                 "SetILVWhat(state=0) clears bit");
    check_eq_int(static_cast<int>(a->what & (1L<<7)), 1<<7,
                 "SetILVWhat(state=0) leaves other bits");

    // SetILVInsize + SetILVOutboundId
    SetILVInsize(0, 32);
    check_eq_int(a->insize, 32, "SetILVInsize stores");
    SetILVOutboundId(0, 42);
    check_eq_int(a->obid, 42, "SetILVOutboundId stores");

    // pSetILVRun / pSetILVInsize (direct pointer variants)
    pSetILVRun(a, 1);
    check_eq_int(static_cast<int>(a->run & 1), 1, "pSetILVRun(1)");
    pSetILVInsize(a, 64);
    check_eq_int(a->insize, 64, "pSetILVInsize stores");

    // SetILVOutputPointer — swap and confirm via xilv dispatch.
    SetILVOutputPointer(0, swap_outbound);
    pSetILVRun(a, 0);                 // exercise bypass path
    a->what = 0;
    a->insize = 1;
    double zero[2] = {0.0, 0.0};
    double* data_one[1] = { zero };
    xilv(a, data_one);
    check_eq_int(g_swap_seen_id, 42, "SetILVOutputPointer swap took effect");

    pcm->xmtr[0].pilv = nullptr;
    destroy_ilv(a);
}

// ====================================================================
void testBypassPath() {
    std::printf("[test] xilv run=0 bypass (memcpy data[0] -> outbuff)\n");

    using namespace lyra::wire;
    reset_last();
    ILV a = create_ilv(/*run*/0,           // run bit OFF -> bypass
                       /*outbound_id*/5,
                       /*insize*/3,
                       /*ninputs*/1,
                       /*what*/0,
                       record_outbound);

    // Input is one stream of 3 complex samples = 6 doubles.
    double in0[6] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
    double* data[1] = { in0 };

    xilv(a, data);

    check_eq_int(g_last.invocations, 1, "Outbound called once");
    check_eq_int(g_last.id,          5, "Outbound got obid=5");
    check_eq_int(g_last.nsamples,    3, "bypass k == insize");
    check_eq_int(static_cast<int>(g_last.buff.size()), 6, "buff has 2*k doubles");
    for (int i = 0; i < 6; ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), "bypass buff[%d] == in0[%d]", i, i);
        check_eq(g_last.buff[static_cast<std::size_t>(i)], in0[i], label);
    }

    destroy_ilv(a);
}

// ====================================================================
void testInterleave2Stream() {
    std::printf("[test] xilv run=1 interleave (2 streams, what=0b11)\n");

    using namespace lyra::wire;
    reset_last();
    ILV a = create_ilv(/*run*/1,                // run bit ON -> interleave
                       /*outbound_id*/9,
                       /*insize*/3,             // 3 complex samples per stream
                       /*ninputs*/2,
                       /*what*/(1L<<0)|(1L<<1), // both streams enabled
                       record_outbound);

    // Stream 0: I/Q pairs (10, 20), (11, 21), (12, 22)
    // Stream 1: I/Q pairs (30, 40), (31, 41), (32, 42)
    double s0[6] = { 10, 20, 11, 21, 12, 22 };
    double s1[6] = { 30, 40, 31, 41, 32, 42 };
    double* data[2] = { s0, s1 };

    xilv(a, data);

    // Expected interleave (j outer, i inner ascending): for each j in
    // [0, insize), copy stream 0's sample then stream 1's sample.
    // k = 2 * insize = 6, so the Outbound buffer holds 12 doubles
    // ordered: s0[j=0], s1[j=0], s0[j=1], s1[j=1], s0[j=2], s1[j=2].
    const double expected[12] = {
        10, 20,   30, 40,   // j=0
        11, 21,   31, 41,   // j=1
        12, 22,   32, 42,   // j=2
    };

    check_eq_int(g_last.id,       9, "Outbound got obid=9");
    check_eq_int(g_last.nsamples, 6, "k == 2 * insize");
    check_eq_int(static_cast<int>(g_last.buff.size()), 12, "buff has 12 doubles");
    for (int i = 0; i < 12; ++i) {
        char label[64];
        std::snprintf(label, sizeof(label),
                      "interleave[%d] == expected[%d]", i, i);
        check_eq(g_last.buff[static_cast<std::size_t>(i)],
                 expected[i], label);
    }

    destroy_ilv(a);
}

// ====================================================================
void testInterleaveSparseWhat() {
    std::printf("[test] xilv run=1 interleave with gap (what=0b1011)\n");

    using namespace lyra::wire;
    reset_last();
    // Streams 0, 1, 3 enabled; stream 2 skipped.  Reference's i-bit
    // scan goes LSB→MSB, so the output sample ordering per j-step is
    // s0, s1, s3 (stream 2 absent).
    ILV a = create_ilv(/*run*/1,
                       /*outbound_id*/0,
                       /*insize*/2,
                       /*ninputs*/4,
                       /*what*/(1L<<0)|(1L<<1)|(1L<<3),
                       record_outbound);

    double s0[4] = { 100, 200, 101, 201 };
    double s1[4] = { 300, 400, 301, 401 };
    double s2[4] = { 999, 999, 999, 999 };  // must NOT appear in output
    double s3[4] = { 500, 600, 501, 601 };
    double* data[4] = { s0, s1, s2, s3 };

    xilv(a, data);

    // Per j: s0[j], s1[j], s3[j].  Two j-steps × 3 enabled streams =
    // k = 6 tuples = 12 doubles.
    const double expected[12] = {
        100, 200,   300, 400,   500, 600,   // j=0
        101, 201,   301, 401,   501, 601,   // j=1
    };

    check_eq_int(g_last.nsamples, 6, "k == insize * popcount(what) == 2*3 = 6");
    check_eq_int(static_cast<int>(g_last.buff.size()), 12, "buff has 12 doubles");
    for (int i = 0; i < 12; ++i) {
        char label[80];
        std::snprintf(label, sizeof(label),
                      "sparse[%d] == expected[%d] (stream 2 absent)", i, i);
        check_eq(g_last.buff[static_cast<std::size_t>(i)],
                 expected[i], label);
    }

    destroy_ilv(a);
}

} // namespace

int main() {
    std::printf("=== ILV direct port (P0.b) — unit test ===\n");
    testLifecycle();
    testSetters();
    testBypassPath();
    testInterleave2Stream();
    testInterleaveSparseWhat();
    std::printf("\n=== %s — %d failure(s) ===\n",
                failures == 0 ? "ALL PASS" : "FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
