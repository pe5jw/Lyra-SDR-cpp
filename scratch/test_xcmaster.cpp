// Standalone unit test for Stage D Lyra-native dispatch surface.
//
// Build (from repo root, in a vcvars64 shell):
//   cl /std:c++latest /EHsc /O2 /I src ^
//      scratch\test_xcmaster.cpp src\wire\CMaster.cpp ^
//      src\wdsp\ILV.cpp ^
//      /Fe:scratch\test_xcmaster.exe
//   .\scratch\test_xcmaster.exe
//
// (/std:c++latest — this MSVC toolchain does not recognise
// /std:c++23 as a stable flag; the main CMake build handles the
// CMAKE_CXX_STANDARD 23 -> /std:c++latest translation internally.)
//
// **HONEST SCOPE NOTE.**
//
// Stage D's xcmasterTickTx() end-to-end flow is:
//
//     TxChannel::process(mic_in, n)   // wraps WDSP fexchange0
//     -> xilv(pilv[id], tx->outBuffers().data())
//        -> ILV::Outbound callback
//           -> SendpOutboundTx-registered handler (EP2 wire)
//
// The ILV half of this chain is comprehensively tested by
// scratch/test_ilv.cpp (Stage C.2) — bit-exact bypass-mode +
// interleave + sparse-what assertions all green.
//
// The TxChannel half requires the live `lyra::dsp::WdspNative`
// (a Qt QObject loading the wdsp.dll at runtime).  That's too
// heavy for a standalone `cl` build; the end-to-end test belongs
// in the integration harness that lands with the TX wire-layer
// rebuild's consumer side (Task #112).
//
// Stage D.3 (THIS TEST) covers the Lyra-NATIVE pieces Stage D
// added (the pxmtr[] central bank + the xcmaster dispatch
// switch).  The TxChannel + xilv arms are exercised at the
// integration stage when WDSP + Qt are available.
//
// Tests cover:
//   * pxmtr[] starts empty
//   * create_xmtr_hl2(0, tx, ilv) publishes pxmtr[0]
//   * destroy_xmtr_hl2(0, tx) clears pxmtr[0]
//   * destroy_xmtr_hl2(0, OTHER_tx) does NOT clear pxmtr[0]
//     (the create-then-destroy-race protection)
//   * create_xmtr_hl2(out-of-range id) is silent no-op
//   * xcmasterTickTx(0, ...) on a null-tx_channel slot is
//     a safe no-op (resolve_xmtr early-return through the
//     tx_channel==nullptr guard)
//   * xcmasterTickTx(out-of-range id) is a safe no-op
//   * xcmaster(stream=0) routes to case 1 (TX), which performs
//     a no-op for the bare xcmaster() call by design (mic input
//     ownership is operator-explicit — caller uses
//     xcmasterTickTx with explicit mic_in for live use)
//   * xcmaster(stream=99) hits the default switch arm — no-op
//
// No external dependencies beyond the project source tree + MSVC.

// We include TxChannel.h + AAMix.h to provide LINKER STUB
// implementations of the two CMaster.cpp out-of-line dependencies
// (TxChannel::process + SetAAudioMixOutputPointer) -- both
// references in CMaster.cpp's actual TUs, but the D.3 test paths
// NEVER reach the call sites (every test uses an empty pxmtr[]
// slot so resolve_xmtr early-returns; SendpOutboundRx is RX-side
// and isn't exercised either).  Stubs satisfy the linker; the
// runtime safety is by-construction (early-return guards).
//
// TxChannel.h forward-decls WdspNative (full Qt cdef stays out
// of this test); AAMix.h pulls <thread>/<semaphore> but no Qt.
// Pointer-only use of TxChannel keeps WdspNative incomplete-type-
// safe (only ctor/dtor would need the full type; we never
// construct a real TxChannel).
#include "wdsp/TxChannel.h"
#include "wdsp/AAMix.h"
#include "wire/CMaster.h"

#include <cstdio>
#include <cstdint>
#include <functional>

// Linker stubs for the two CMaster.cpp call sites the D.3 test
// paths never reach at runtime (proven by construction --
// resolve_xmtr early-returns on every test path because pxmtr[]
// slots all have tx_channel==nullptr).  These definitions exist
// solely to satisfy the linker.  See the docstring above for the
// scope split between this Lyra-native dispatch test and the
// deferred end-to-end TxChannel+xilv+Outbound integration test.

int lyra::wdsp::TxChannel::process(const double* /*mic_iq*/,
                                   int /*n_samples*/) {
    std::fprintf(stderr,
                 "FATAL: test stub TxChannel::process should never be "
                 "called -- bank early-return guard failed\n");
    std::abort();
}

namespace lyra::wdsp {
void SetAAudioMixOutputPointer(AAMix* /*ptr*/, int /*id*/,
    std::function<void(int, int, double*)> /*Outbound*/) {
    // RX-side SendpOutboundRx path -- D.3 test does not exercise
    // it.  Silent no-op (won't fire; SendpOutboundRx isn't called
    // by any test).
}
} // namespace lyra::wdsp

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

// "Fake" TxChannel pointers — bank operations only check pointer
// identity, never dereference.  Cast known sentinel bit patterns
// to TxChannel* so we can verify which pointer ended up in the
// slot without needing a real TxChannel.
lyra::wdsp::TxChannel* fake_tx_a() {
    return reinterpret_cast<lyra::wdsp::TxChannel*>(
        static_cast<std::uintptr_t>(0xdeadbeef));
}
lyra::wdsp::TxChannel* fake_tx_b() {
    return reinterpret_cast<lyra::wdsp::TxChannel*>(
        static_cast<std::uintptr_t>(0xcafef00d));
}

void resetBank() {
    using namespace lyra::wire;
    for (auto& slot : pxmtr) {
        slot = XmtrSlot{};
    }
}

// ====================================================================
void testBankStartsEmpty() {
    std::printf("[test] pxmtr[] starts empty\n");
    resetBank();

    using namespace lyra::wire;
    static_assert(MAX_EXT_XMTR >= 1, "test requires at least one slot");
    check(pxmtr[0].tx_channel == nullptr, "pxmtr[0].tx_channel default nullptr");
    check(pxmtr[0].ilv_xmtr_id == 0,      "pxmtr[0].ilv_xmtr_id default 0");
}

// ====================================================================
void testCreatePublishesSlot() {
    std::printf("[test] create_xmtr_hl2 publishes pxmtr[xmtr_id]\n");
    resetBank();

    using namespace lyra::wire;
    create_xmtr_hl2(0, fake_tx_a(), /*ilv_xmtr_id=*/0);
    check(pxmtr[0].tx_channel == fake_tx_a(),
          "pxmtr[0].tx_channel == fake_tx_a after create");
    check(pxmtr[0].ilv_xmtr_id == 0,
          "pxmtr[0].ilv_xmtr_id == 0 after create");
}

// ====================================================================
void testDestroyClearsSlotWhenMatch() {
    std::printf("[test] destroy_xmtr_hl2 clears slot iff pointer match\n");
    resetBank();

    using namespace lyra::wire;
    create_xmtr_hl2(0, fake_tx_a(), 0);
    destroy_xmtr_hl2(0, fake_tx_a());
    check(pxmtr[0].tx_channel == nullptr,
          "pxmtr[0].tx_channel cleared after matching destroy");

    // Defensive: destroy with NON-matching pointer must NOT clear.
    resetBank();
    create_xmtr_hl2(0, fake_tx_a(), 0);
    destroy_xmtr_hl2(0, fake_tx_b());     // wrong pointer
    check(pxmtr[0].tx_channel == fake_tx_a(),
          "pxmtr[0] survives destroy w/ non-matching pointer");
}

// ====================================================================
void testCreateOutOfRangeNoop() {
    std::printf("[test] create_xmtr_hl2(out-of-range) is silent no-op\n");
    resetBank();

    using namespace lyra::wire;
    create_xmtr_hl2(-1,           fake_tx_a(), 0);
    create_xmtr_hl2(MAX_EXT_XMTR, fake_tx_a(), 0);
    create_xmtr_hl2(99,           fake_tx_a(), 0);
    check(pxmtr[0].tx_channel == nullptr,
          "pxmtr[0] untouched by out-of-range creates");
}

// ====================================================================
void testDestroyOutOfRangeNoop() {
    std::printf("[test] destroy_xmtr_hl2(out-of-range) is silent no-op\n");
    resetBank();

    using namespace lyra::wire;
    // Should not crash on out-of-range OR nullptr tx_channel arg.
    destroy_xmtr_hl2(-1, fake_tx_a());
    destroy_xmtr_hl2(99, fake_tx_a());
    destroy_xmtr_hl2( 0, nullptr);  // null tx_channel — won't match
    check(true, "no crash on out-of-range / null destroys");
}

// ====================================================================
void testTickTxNullSlotNoop() {
    std::printf("[test] xcmasterTickTx(empty slot) is safe no-op\n");
    resetBank();

    // Slot is empty (resetBank just ran); resolve_xmtr returns
    // nullptr; xcmasterTickTx early-returns.  No mic_in deref.
    using namespace lyra::wire;
    double dummy_mic[1] = {0.0};
    xcmasterTickTx(0, dummy_mic, 1);
    check(true, "xcmasterTickTx with empty pxmtr[0] does not crash");
}

// ====================================================================
void testTickTxOutOfRangeNoop() {
    std::printf("[test] xcmasterTickTx(out-of-range id) is safe no-op\n");
    resetBank();

    using namespace lyra::wire;
    double dummy_mic[1] = {0.0};
    xcmasterTickTx(-1, dummy_mic, 1);
    xcmasterTickTx(MAX_EXT_XMTR, dummy_mic, 1);
    xcmasterTickTx(99, dummy_mic, 1);
    check(true, "xcmasterTickTx with out-of-range id does not crash");
}

// ====================================================================
void testTickTxNullMicSafe() {
    std::printf("[test] xcmasterTickTx with nullptr mic_in + empty slot\n");
    resetBank();

    // With pxmtr[0] empty, resolve_xmtr returns nullptr BEFORE
    // mic_in is touched -- so even a nullptr mic_in is safe here.
    // (The mic_in is only dereferenced inside TxChannel::process,
    // which we never reach with an empty slot.)
    using namespace lyra::wire;
    xcmasterTickTx(0, nullptr, 0);
    check(true, "empty slot guard fires before any mic_in deref");
}

// ====================================================================
void testXcmasterDispatchSwitch() {
    std::printf("[test] xcmaster(int stream) switch dispatch\n");
    resetBank();

    using namespace lyra::wire;
    // stream==0 -> stype()=1 (TX) -> case 1.  Case 1 body for the
    // bare xcmaster() call is intentionally a no-op (mic ownership
    // is operator-explicit; caller uses xcmasterTickTx for live use).
    xcmaster(0);
    check(true, "xcmaster(0) routes to case 1 TX no-op without crash");

    // stream==99 -> stype()=-1 -> default no-op.
    xcmaster(99);
    check(true, "xcmaster(99) hits default switch arm without crash");

    // Negative stream id (out-of-range).
    xcmaster(-1);
    check(true, "xcmaster(-1) hits default switch arm without crash");
}

// ====================================================================
void testTickTxClearedAfterDestroy() {
    std::printf("[test] xcmasterTickTx no-ops after destroy_xmtr_hl2\n");
    resetBank();

    using namespace lyra::wire;
    create_xmtr_hl2(0, fake_tx_a(), 0);
    destroy_xmtr_hl2(0, fake_tx_a());
    // Slot is empty again; tick must early-return safely.
    double dummy_mic[1] = {0.0};
    xcmasterTickTx(0, dummy_mic, 1);
    check(true, "post-destroy tick is safe no-op");
}

} // namespace

int main() {
    std::printf("=== xcmaster port -- Stage D.3 Lyra-native dispatch test ===\n");
    std::printf("(Bank + dispatch coverage; live TxChannel + WDSP integration\n");
    std::printf(" deferred to the rebuild integration harness.)\n\n");

    testBankStartsEmpty();
    testCreatePublishesSlot();
    testDestroyClearsSlotWhenMatch();
    testCreateOutOfRangeNoop();
    testDestroyOutOfRangeNoop();
    testTickTxNullSlotNoop();
    testTickTxOutOfRangeNoop();
    testTickTxNullMicSafe();
    testXcmasterDispatchSwitch();
    testTickTxClearedAfterDestroy();

    std::printf("\n=== %s -- %d failure(s) ===\n",
                failures == 0 ? "ALL PASS" : "FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
