# Step 14 Stage 2b — Design Paper

**Status:** PLAN-BEFORE-CODE — awaiting 2-agent red-team check + operator sign-off.
**Operator directive (2026-06-07):** "Sub staged is fine as long as what we end up with is a 'Does as the reference does' outcome."

This document is the complete plan-paper. It must be reviewed by 2 independent senior agents before any code is written, per the same discipline that caught the §3.9 patches + the init-sem release deviation.

---

## Scope

Stage 2b is the **wire-LIVE EP6 migration commit**. It retires `HL2Stream::rxWorker_` jthread + `rxWorkerLoop` body and brings `Ep6RecvThread` LIVE as the production EP6 recv path. It also migrates `txWorker_::sendDatagram` from its private `txSeq_` counter onto the shared `g_metis_out_seq_num` (via `lyra::wire::metis_write_frame()`) so priming + steady-state share ONE outbound seq stream — the PureSignal-load-bearing reference posture (`networkproto1.c:30, 221, 231` — single `MetisOutBoundSeqNum`).

Stage 2a already landed the Ep6RecvThread reference-position prep + HW PTT sink scaffolding (wire-INERT, zero production behavior delta). Stage 2b is the LIVE commit.

After Stage 2b the original plan's Stages 3, 4, 5, 6 are absorbed (rxWorker retires; Router sink, mic sink, telemetry-via-`prn->...`, HW-PTT-via-sink all wire LIVE on this commit). Stage 7+ remain ahead (TX-side migration).

## Reference-faithful end-state guarantee

Per operator directive, the END STATE must be "do as reference does." This design lists every Lyra surface that touches the LIVE EP6 path + its reference cite (where one exists) or its Lyra-native classification (where no reference exists).

The reference does NOT have:
- A Q_OBJECT facade wrapping the wire layer
- The Lyra-native operator-facing instruments (rx1DbFs, micDbFs, framing-error counter, total/window dg counters)
- A QML-binding atomic layer

These are Lyra-native UX additions that the reference's C-source UI handles entirely differently (C# WinForms, no equivalent translation). The Lyra-native classification per §3.9 / Rule 26 means: each one needs an explicit acceptance in this design (not silently introduced).

---

## File touch list with file:line cites

### 1. `src/wire/Ep6RecvThread.{h,cpp}` — Add 3 TU-scope counters + accessors

**Why:** rxWorkerLoop owned `totalDg_`/`windowDg_`/`framingErrors_`. None of these have reference equivalents in `_radionet` (reference has `SeqError` file-scope global → already mirrored as `g_seq_error`; total + framing are Lyra-native operator instruments).

**New TU-scope statics in `Ep6RecvThread.cpp` anonymous namespace** (sibling of `g_seq_error`, `g_metis_last_recv_seq`, `g_control_bytes_in`, `g_fpga_read_bufp`):
```cpp
std::atomic<std::int64_t> g_total_datagrams{0};   // monotonic counter — operator stats
std::atomic<std::int64_t> g_window_datagrams{0};  // window counter — drained by stats tick
std::atomic<std::int64_t> g_framing_errors{0};    // sync/size/header failures
```

**Write sites** (already exist in `run_loop` recv block at Ep6RecvThread.cpp:461-495 + process_usb_frame sync check at :576):
- After successful `recv` + size + header + sync check + `memcpy` (line ~494): `g_total_datagrams.fetch_add(1)`, `g_window_datagrams.fetch_add(1)` BEFORE `process_datagram(...)`
- Each `continue` on framing failure (lines 470-471, 476-478, 479-482): `g_framing_errors.fetch_add(1)`

**Accessors** (new free functions in `lyra::wire`):
```cpp
std::int64_t ep6_total_datagrams();   // monotonic
std::int64_t ep6_drain_window_datagrams();  // atomic exchange-to-0 for stats tick
std::int64_t ep6_seq_errors();        // returns g_seq_error (currently `int` — promote to atomic<int64_t>)
std::int64_t ep6_framing_errors();    // monotonic
```

The seq-error counter currently lives as `int g_seq_error` in the same anonymous namespace. Stage 2b promotes it to `std::atomic<std::int64_t>` to match accessor signature + safe multi-thread read.

**Lyra-native classification:** these counters are Lyra-native operator instruments. Reference's `SeqError` is the only one with direct reference equivalent. Other 3 are operator UX additions. ACCEPTABLE LYRA-NATIVE per Rule 26 (operator-facing observability has no reference equivalent in any case).

### 2. `src/wire/Ep6RecvThread.cpp::run_loop` — Initialize the new counters

In the existing per-thread init block (after the §3.9-style mic_decimation_count = 0 line):
```cpp
g_total_datagrams.store(0);
g_window_datagrams.store(0);
g_framing_errors.store(0);
// g_seq_error already initialized to 0 in the existing block
```

### 3. `src/hl2_stream.h` — Add Ep6RecvThread member + getter; delete obsolete members + decls

**Add** (in the private members block, near the other wire-layer state):
```cpp
lyra::wire::Ep6RecvThread ep6Thread_;
```

**Add** (public accessor for Hl2Ep6MicSource + other consumers that need to register sinks):
```cpp
lyra::wire::Ep6RecvThread& ep6Thread() noexcept { return ep6Thread_; }
```

**Delete**:
- `std::jthread rxWorker_;` (line ~965 area)
- `void rxWorkerLoop(std::stop_token, SocketHandle);` declaration (line 979)
- `void setIqSink(std::function<void(const double *, int)> sink) {...}` inline impl (line 589) + `iqSink_` member
- `void setMicConsumer(std::function<void(const float *, int)> cb) {...}` inline impl (line 608) + `micConsumer_` + `micConsumerMtx_` members
- `std::atomic<quint32> txSeq_{0};` member (line 1108)
- `std::atomic<int> telExciterRaw_{-1};` (line 1175)
- `std::atomic<int> telFwdRaw_{-1};` (line 1176)
- `std::atomic<int> telRevRaw_{-1};` (line 1177)
- `std::atomic<int> telUserAdc0Raw_{-1};` (line 1178)
- `std::atomic<bool> adcOverloadNow_{false};` (find + delete)

**Add `#include "wire/Ep6RecvThread.h"`** at the top.

**Lyra-native classification:** the `ep6Thread_` member + getter is the new owner of the EP6 recv path — replaces `rxWorker_`. The deletion of `iqSink_`/`micConsumer_`/`txSeq_`/telemetry atomics is the reference-alignment work (single owner of state, no parallel local copies).

### 4. `src/hl2_stream.cpp::open()` — Wire LIVE

**Reference-faithful flow** (`netInterface.c:32-94` + `networkproto1.c:240-261`):

```cpp
void HL2Stream::open(const QString &ip) {
    [existing UDP socket bind, stats baseline, state clear — UNCHANGED]
    [existing wire-init block at 698-717: create_rnet(), metis_wire_bind, outbound_init — UNCHANGED]

    // ---- NEW: hoist START packet from txWorkerLoop ----
    // Reference: SendStartToMetis() at netInterface.c:50, BEFORE EP6
    // read thread spawn at :61.  Lyra was sending from inside
    // txWorkerLoop (line 2511-2526); Stage 2b hoists it here.
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port   = htons(kRadioPort);
        ::inet_pton(AF_INET, ip.toLatin1().constData(), &dest.sin_addr);
        const QByteArray startPkt = buildControlPacket(true);
        const int n = ::sendto(static_cast<SOCKET>(socket_),
                               startPkt.constData(), startPkt.size(), 0,
                               reinterpret_cast<sockaddr*>(&dest),
                               sizeof(dest));
        if (n != startPkt.size()) {
            const int err = ::WSAGetLastError();
            onFatalError(QStringLiteral("START: %1").arg(winsockError(err)));
            return;
        }
        emit logLine(QStringLiteral(
            "  START sent (0xEFFE 0x04 0x01 + 60 zeros)"));
    }

    // ---- NEW: wire sinks on ep6Thread_ BEFORE start() ----
    ep6Thread_.set_router(lyra::wire::router_instance(0), /*router_id=*/0);
    // Mic sink: Hl2Ep6MicSource registers via stream.ep6Thread() later;
    // leave null here so the sink stays unbound until the source ctor runs.
    // Telemetry sink: HL2 telemetry writes prn->... direct inside
    //                 decode_status_header; no sink callback needed for
    //                 the QML accessors (they read prn->... post-Stage-2b).
    // HW PTT sink: register the edge-detect + opt-in-gate + invokeMethod
    //              forwarder that rxWorkerLoop used to do inline.
    ep6Thread_.set_hw_ptt_sink([this](bool ptt_in_now) {
        // EP6 thread context (NOT Qt main).  Single-writer for lastPttIn_.
        if (ptt_in_now != lastPttIn_) {
            lastPttIn_ = ptt_in_now;
            if (hwPttEnabled_.load(std::memory_order_acquire)) {
                QMetaObject::invokeMethod(
                    this, "requestMoxFromHwPtt",
                    Qt::QueuedConnection,
                    Q_ARG(bool, ptt_in_now));
            }
        }
    });

    // ---- NEW: spawn Ep6RecvThread (blocks on init-sem until thread alive) ----
    // Reference: _beginthreadex(MetisReadThreadMain) + WaitForSingleObject
    //            at netInterface.c:61-63.
    ep6Thread_.start(static_cast<int>(socket_));

    // ---- DELETED: rxWorker_ spawn at lines 723-725 (rxWorkerLoop retires) ----
    // ---- DELETED: txSeq_.store(0) at line 606 (txSeq_ member deleted) ----

    // ---- Spawn txWorker_ (TX path stays LIVE until Stage 7+) ----
    const SocketHandle sh = socket_;
    txWorker_ = std::jthread([this, sh, ip](std::stop_token stop) {
        txWorkerLoop(std::move(stop), sh, ip);
    });
}
```

**Sink registration sub-point:** `ep6Thread_.set_router(router_instance(0), 0)` sets the Router consumer for IQ dispatch. The Router itself is constructed elsewhere (via the global instance registry); call_idx=0 (WDSP) + call_idx=1 (HL2Stream RX1 dBFS RMS instrument) get registered separately in `main.cpp` (see file 7 below).

### 5. `src/hl2_stream.cpp::close()` — Replace rxWorker join with ep6Thread_.stop()

Lines 794-802 (current rxWorker_ stop/join):
```cpp
qWarning("[shutdown] HL2Stream::close request_stop on rx+tx workers");
if (rxWorker_.joinable()) rxWorker_.request_stop();
if (txWorker_.joinable()) txWorker_.request_stop();
qWarning("[shutdown] HL2Stream::close rxWorker_.join() - start");
if (rxWorker_.joinable()) rxWorker_.join();
qWarning("[shutdown] HL2Stream::close rxWorker_.join() - done");
qWarning("[shutdown] HL2Stream::close txWorker_.join() - start");
if (txWorker_.joinable()) txWorker_.join();
qWarning("[shutdown] HL2Stream::close txWorker_.join() - done");
```

Replace with:
```cpp
qWarning("[shutdown] HL2Stream::close request_stop on tx worker + ep6 thread");
if (txWorker_.joinable()) txWorker_.request_stop();
qWarning("[shutdown] HL2Stream::close ep6Thread_.stop() - start");
ep6Thread_.stop();   // joins the EP6 thread + clears wire event
qWarning("[shutdown] HL2Stream::close ep6Thread_.stop() - done");
qWarning("[shutdown] HL2Stream::close txWorker_.join() - start");
if (txWorker_.joinable()) txWorker_.join();
qWarning("[shutdown] HL2Stream::close txWorker_.join() - done");
```

`ep6Thread_.stop()` is bounded by the same `prn->wdt ? 3000 : WSA_INFINITE` discipline as the reference (per Ep6RecvThread.cpp:399-402). Matches reference shutdown posture per `network.c:1443+` IOThreadStop pattern.

### 6. `src/hl2_stream.cpp::txWorkerLoop` — Migrate to `metis_write_frame()`

**Two changes:**

#### 6a. Delete the open-time START send block (lines 2506-2526)

The START packet is hoisted to `open()` (file 4 above). txWorkerLoop body proceeds directly from its entry to the existing priority/timer setup (line 2541+).

The lines to delete:
```cpp
// Send START.  Done from the TX thread (not from main or RX)
// so every host→radio packet originates from one operation
// context...
const QByteArray startPkt = buildControlPacket(true);
if (::sendto(s, startPkt.constData(), startPkt.size(), 0,
             reinterpret_cast<sockaddr*>(&dest),
             sizeof(dest)) != startPkt.size()) {
    const int err = ::WSAGetLastError();
    QMetaObject::invokeMethod(this, [this, err]() {
        onFatalError(QStringLiteral("START: %1").arg(winsockError(err)));
    }, Qt::QueuedConnection);
    return;
}
QMetaObject::invokeMethod(this, [this]() {
    emit logLine(QStringLiteral(
        "  START sent (0xEFFE 0x04 0x01 + 60 zeros), "
        "EP2 keepalive engaging @380 Hz"));
}, Qt::QueuedConnection);
```

Also delete the local `sockaddr_in dest{}` setup at lines 2501-2504 (no longer needed — `metis_write_frame()` uses the bound `g_metis_addr_be`). And delete the parameter `QString ip` from `txWorkerLoop` (also via header decl).

Wait — `dest` is also used by `::sendto` inside `sendDatagram` at line 2727. So if we delete the local dest, we have to delete the sendto too — which is what change 6b does. So delete dest setup as part of 6b.

#### 6b. Rewrite `sendDatagram` to call `metis_write_frame()`

Current `sendDatagram` lambda (lines 2550-2735):
```cpp
auto sendDatagram = [&](const qint16 *audio) {
    const quint32 seq = txSeq_.fetch_add(1, std::memory_order_relaxed);
    pktBytes[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    pktBytes[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    pktBytes[6] = static_cast<std::uint8_t>((seq >>  8) & 0xFF);
    pktBytes[7] = static_cast<std::uint8_t>( seq        & 0xFF);

    [... composeCC + audio packing into pktBytes[8..1031] UNCHANGED ...]

    const int n = ::sendto(s, pkt.constData(), pkt.size(), 0,
                           reinterpret_cast<sockaddr*>(&dest),
                           sizeof(dest));
    if (n != pkt.size()) {
        txSendErrors_.fetch_add(1, std::memory_order_relaxed);
    } else {
        txTotalDg_.fetch_add(1, std::memory_order_relaxed);
        txWindowDg_.fetch_add(1, std::memory_order_relaxed);
    }
};
```

New `sendDatagram`:
```cpp
auto sendDatagram = [&](const qint16 *audio) {
    // ---- DELETED: manual HPSDR header build (lines 2551-2555) ----
    //   txSeq_ retired; g_metis_out_seq_num (shared with priming) advances
    //   inside metis_write_frame() via the reference's post-increment.

    [... composeCC + audio packing into pktBytes[8..1031] UNCHANGED ...]

    // metis_write_frame builds the 8-byte HPSDR header internally
    // (sync + endpoint + BE seq from g_metis_out_seq_num), memcpys
    // the 1024-byte payload, holds prn->sndpkt around sendto.
    // Reference parity: MetisWriteFrame at networkproto1.c:216-237.
    // Payload pointer is pktBytes + 8 because the keepalive template
    // is still 1032 bytes total (bytes 0..7 = vestigial; not read by
    // metis_write_frame).
    const int n = lyra::wire::metis_write_frame(0x02, pktBytes + 8);
    if (n < 0) {
        txSendErrors_.fetch_add(1, std::memory_order_relaxed);
    } else {
        txTotalDg_.fetch_add(1, std::memory_order_relaxed);
        txWindowDg_.fetch_add(1, std::memory_order_relaxed);
    }
};
```

Plus delete `sockaddr_in dest{};` + `::inet_pton(...)` setup at lines 2501-2504 (no longer used).

Plus delete the `QString ip` parameter from `txWorkerLoop(std::stop_token stop, SocketHandle sh, QString ip)` — `ip` was only used for the `dest` setup. Update the call site in `open()` to drop the `ip` arg. Update the function declaration in `hl2_stream.h`.

**Wire behavior** post-2b: priming + steady-state share `g_metis_out_seq_num`. Reference posture exactly per `networkproto1.c:30`.

### 7. `src/hl2_stream.cpp::rxWorkerLoop` — DELETE entire function body

Lines 1013-1388 (~376 lines). Function declaration in `hl2_stream.h:979` also deleted (file 3 above). All the responsibilities re-homed in file 1 (counters) + file 4 (sink wiring) + file 9 (accessor switch).

### 8. `src/hl2_stream.cpp` accessors — Switch telemetry getters to `prn->...` direct

#### 8a. `hl2TempC()` (line 958)

Current:
```cpp
double HL2Stream::hl2TempC() const {
    const int raw = telExciterRaw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    return (3.26 * (raw / 4096.0) - 0.5) / 0.01;
}
```

New:
```cpp
double HL2Stream::hl2TempC() const {
    if (lyra::wire::prn == nullptr) return kNaN;
    // Per console.cs:24937-24941 HL2 reinterpretation: the C0=0x08 slot
    // C1:C2 carries temp (NOT exciter_power) on HL2/HL2+.  Reference
    // writes raw to prn->tx[0].exciter_power; Lyra accessor applies the
    // HL2 reinterpretation here.  Raw default 0 from create_rnet
    // produces -50°C until first telemetry frame (~26ms after first
    // datagram) — matches reference UX (Thetis-side reads same int=0).
    const int raw = lyra::wire::prn->tx[0].exciter_power;
    return (3.26 * (raw / 4096.0) - 0.5) / 0.01;
}
```

#### 8b. `paCurrentA()` (line 989)

Current:
```cpp
double HL2Stream::paCurrentA() const {
    const int raw = telUserAdc0Raw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    return ((3.26 * (raw / 4096.0)) / 50.0) / 0.04 / (1000.0 / 1270.0);
}
```

New:
```cpp
double HL2Stream::paCurrentA() const {
    if (lyra::wire::prn == nullptr) return kNaN;
    // Per console.cs:24937-24941 HL2 reinterpretation: the C0=0x10 slot
    // C3:C4 carries PA current (NOT user_adc0) on HL2/HL2+.  Reference
    // writes raw to prn->user_adc0; Lyra accessor applies the HL2
    // reinterpretation here.
    const int raw = lyra::wire::prn->user_adc0;
    return ((3.26 * (raw / 4096.0)) / 50.0) / 0.04 / (1000.0 / 1270.0);
}
```

#### 8c. `hl2SupplyV()` (line ~970 area)

No change — reference does NOT display supply voltage on HL2 (`console.cs:26758-26761` reuses the status-bar slot for °C). Returns kNaN unconditionally; matches reference.

#### 8d. `fwdPowerW()` + `revPowerW()` (lines 994, 1002)

Current:
```cpp
double HL2Stream::fwdPowerW() const {
    const int raw = telFwdRaw_.load(std::memory_order_relaxed);
    ...
}
```

New:
```cpp
double HL2Stream::fwdPowerW() const {
    if (lyra::wire::prn == nullptr) return kNaN;
    const int raw = lyra::wire::prn->tx[0].fwd_power;
    ...  // existing formula UNCHANGED
}

double HL2Stream::revPowerW() const {
    if (lyra::wire::prn == nullptr) return kNaN;
    const int raw = lyra::wire::prn->tx[0].rev_power;
    ...
}
```

**Lyra-native classification:** the dropping of "< 0 sentinel" for "no telemetry yet" matches reference behavior exactly (reference reads `prn->tx[0].exciter_power = 0` at startup until first telemetry frame arrives, ~26ms latency, gives -50°C briefly). REFERENCE-FAITHFUL.

### 9. `src/hl2_stream.cpp` Auto-LNA + log line — Switch to `prn->adc[0].adc_overload`

#### 9a. Auto-LNA read (line 494)

Current:
```cpp
const bool ov = adcOverloadNow_.load(std::memory_order_relaxed);
```

New:
```cpp
const bool ov = (lyra::wire::prn != nullptr &&
                 lyra::wire::prn->adc[0].adc_overload != 0);
```

`prn->adc[0].adc_overload` is written by `Ep6RecvThread::decode_status_header` already (Ep6RecvThread.cpp:753 + :798) — single-frame assignment per reference HL2 posture (`networkproto1.c:502`).

#### 9b. open() init (line 661)

Current:
```cpp
adcOverloadNow_.store(false, std::memory_order_relaxed);
```

New: DELETE (atomic retired). The `prn->adc[0].adc_overload` field defaults to 0 from `create_rnet()`.

#### 9c. Raw-int telemetry log (lines 925-926)

Current:
```cpp
.arg(telExciterRaw_.load()).arg(telFwdRaw_.load())
.arg(telRevRaw_.load()).arg(telUserAdc0Raw_.load())
```

New:
```cpp
.arg(lyra::wire::prn ? lyra::wire::prn->tx[0].exciter_power : 0)
.arg(lyra::wire::prn ? lyra::wire::prn->tx[0].fwd_power     : 0)
.arg(lyra::wire::prn ? lyra::wire::prn->tx[0].rev_power     : 0)
.arg(lyra::wire::prn ? lyra::wire::prn->user_adc0           : 0)
```

### 10. `src/hl2_stream.cpp` seq/framing/total dg counters — Switch to Ep6 accessors

The `seqErrors_`, `framingErrors_`, `totalDg_`, `windowDg_` atomic members on HL2Stream become FACADES that read from `lyra::wire::ep6_*()` accessors. Two sub-options:

**Option A (cleaner):** Delete the HL2Stream atomic members entirely. Q_PROPERTY getters call `lyra::wire::ep6_seq_errors()` etc. directly.

**Option B (preserves API surface):** Keep the atomic members. `onStatsTick` reads from `lyra::wire::ep6_drain_window_datagrams()` etc. and stores into the HL2Stream atomics. QML reads the HL2Stream atomic as today.

Recommend **Option A** — Lyra-native shadow atomics are exactly the kind of duplicate state the reference posture rejects. Direct facade is cleaner.

Implementation:
```cpp
qint64 HL2Stream::totalDatagrams() const {
    return lyra::wire::ep6_total_datagrams();
}
qint64 HL2Stream::seqErrors() const {
    return lyra::wire::ep6_seq_errors();
}
qint64 HL2Stream::framingErrors() const {
    return lyra::wire::ep6_framing_errors();
}
```

`windowDg_` is consumed by `onStatsTick` to compute dg/s — needs the drain accessor:
```cpp
// In onStatsTick:
const qint64 w = lyra::wire::ep6_drain_window_datagrams();
// existing rate computation
```

`open()` line 585-586 zeroing of these atomics goes away; the TU-scope counters are zeroed inside `run_loop` per-thread init.

### 11. `src/hl2_stream.cpp` RX1 dBFS RMS — Register as Router side-tap

**The new mechanism:** Router supports multiple call_idx slots per port. Stage 2b uses call_idx=0 for WDSP (the production audio path) and call_idx=1 for HL2Stream's RX1 dBFS RMS instrument. `Router::set_call_count(2)` enables both.

This is invoked from `main.cpp` (where the operator wiring lives) — see file 12 below. HL2Stream provides a public method to be called by the registered side-tap lambda:

**Add to `hl2_stream.h` public methods:**
```cpp
// Stage 2b — RX1 dBFS RMS instrument side-tap.  Called from a Router
// sink registered at port=0, call_idx=1.  Accumulates magnitude² over
// kRmsWindowSamples per the OLD rxWorkerLoop behavior.  Single-thread
// caller (EP6 recv thread); rx1DbFs_ atomic is published at window end.
void accumulateRx1Rms(int n_samples, const double* iq_pairs);
```

**Add to `hl2_stream.cpp`:**
```cpp
void HL2Stream::accumulateRx1Rms(int n_samples, const double* iq_pairs) {
    // Same logic as OLD rxWorkerLoop:1309-1319.  Single-threaded by
    // contract (EP6 thread only writer; Router::xrouter dispatches
    // synchronously, no contention).  Accumulator state is a member.
    for (int i = 0; i < n_samples; ++i) {
        const double iv = iq_pairs[2 * i + 0];
        const double qv = iq_pairs[2 * i + 1];
        rx1RmsAcc_ += iv * iv + qv * qv;
        if (++rx1RmsCount_ >= kRmsWindowSamples) {
            const double meanSq = rx1RmsAcc_ /
                                  static_cast<double>(kRmsWindowSamples);
            const double db = (meanSq > 0.0)
                ? 10.0 * std::log10(meanSq) : -200.0;
            rx1DbFs_.store(db, std::memory_order_relaxed);
            rx1RmsAcc_ = 0.0;
            rx1RmsCount_ = 0;
        }
    }
}
```

**Member additions** in hl2_stream.h:
```cpp
double rx1RmsAcc_  = 0.0;
int    rx1RmsCount_ = 0;
static constexpr int kRmsWindowSamples = 9600;
```

**Lyra-native classification:** Router's `ncalls` axis IS reference-architectural (xrouter at router.c:84-105 loops over `function[bport][i][ctrl]` for `i = 0..ncalls-1`). Reference uses it for the PS DDC2/DDC3 twist fanout. Lyra-native usage of call_idx=1 for an instrument is consistent with the reference table shape. ACCEPTABLE per the §5.8 / Router signed deviation framework.

### 12. `src/main.cpp:252` — Replace setIqSink with Router::register_sink

Current (line 252-254):
```cpp
stream->setIqSink([wdspEngine](const double *iq, int n) {
    wdspEngine->feedIq(iq, n);
});
```

New:
```cpp
// Stage 2b — Migrate IQ dispatch to Router::register_sink.  The Router
// table (port × call_idx × ctrl_word) replaces the single-callback
// iqSink_.  WDSP at call_idx=0; HL2Stream's RX1 dBFS RMS instrument at
// call_idx=1 (the side-tap that replaces the rxWorkerLoop accumulator).
//
// Reference: xrouter dispatch at networkproto1.c:550 (HL2 nddc=4 source=0
// = DDC0 = RX1).  Reference uses ncalls=1 by default; Lyra needs
// ncalls=2 for the instrument side-tap.
auto* router = lyra::wire::router_instance(0);
lyra::wire::register_sink(router, /*port=*/0, /*call_idx=*/0,
                          /*ctrl_word=*/0,
    [wdspEngine](int n, const double* iq) {
        wdspEngine->feedIq(iq, n);  // arg order flipped vs old sink
    });
lyra::wire::register_sink(router, /*port=*/0, /*call_idx=*/1,
                          /*ctrl_word=*/0,
    [stream](int n, const double* iq) {
        stream->accumulateRx1Rms(n, iq);
    });
lyra::wire::set_call_count(router, 2);
```

**Reference cite check:** the Router table indexing `sinks[port][call_idx][ctrl_word]` is a direct port of reference's `function[port][i][ctrl]` table at `router.c:71-108`. Multi-call dispatch loop at router.c:84-105 calls each `i < ncalls`. REFERENCE-FAITHFUL mechanism; Lyra-native use of call_idx=1 for the dBFS instrument.

### 13. `src/mic_source.cpp` — Switch setMicConsumer → ep6Thread().set_mic_sink

#### 13a. Constructor (line 7-21)

Current:
```cpp
Hl2Ep6MicSource::Hl2Ep6MicSource(lyra::ipc::HL2Stream &stream)
    : stream_(stream)
{
    stream_.setMicConsumer(
        [this](const float *samples, int n) {
            if (consumer_) {
                consumer_(samples, n);
            }
        });
}
```

New:
```cpp
Hl2Ep6MicSource::Hl2Ep6MicSource(lyra::ipc::HL2Stream &stream)
    : stream_(stream)
{
    // Stage 2b — register on Ep6RecvThread's mic sink directly.  Signature
    // change: new sink delivers (int n_samples, const double* iq_pairs)
    // where iq_pairs is I=mic-normalized, Q=0.0 (per reference's
    // Inbound(inid(1,0), n, prn->TxReadBufp) at networkproto1.c:579).
    // This shim converts to the existing (const float*, int) consumer
    // contract and also accumulates the Q6.5 mic dBFS RMS instrument
    // (formerly in rxWorkerLoop, now owned here as the mic stream's
    // single owner).
    stream_.ep6Thread().set_mic_sink(
        [this](int n_samples, const double* iq_pairs) {
            // Convert double→float, extract I-channel (Q is 0 per ref).
            if (n_samples <= 0) return;
            // Stack buffer sized to Ep6's kMaxSprPerFrame=64 — generous
            // for the 19-sample-per-frame nddc=4 case.
            float mono[256];
            const int n = std::min(n_samples, 256);
            for (int i = 0; i < n; ++i) {
                mono[i] = static_cast<float>(iq_pairs[2 * i + 0]);
            }
            // Q6.5 mic dBFS RMS instrument — moved here from rxWorkerLoop.
            // Accumulates over the same kMicWindowSamples count
            // (operator-visible behavior is identical; sample rate
            // post-decimation is 48 kHz so the window time grows
            // proportionally vs OLD wire-rate accumulation —
            // acceptable Lyra-native variation, same dBFS reading).
            updateMicDbFs(mono, n);
            // Forward to operator-registered consumer.
            if (consumer_) {
                consumer_(mono, n);
            }
        });
}
```

#### 13b. Destructor (line 23-47)

Current calls `stream_.setMicConsumer({})`. New:
```cpp
Hl2Ep6MicSource::~Hl2Ep6MicSource()
{
    qWarning("[shutdown] ~Hl2Ep6MicSource ENTRY");
    stream_.ep6Thread().set_mic_sink({});
    qWarning("[shutdown] ~Hl2Ep6MicSource EXIT");
}
```

Note: `set_mic_sink({})` on Ep6RecvThread MAY need its own thread-safety pattern (the current `set_mic_sink` impl at Ep6RecvThread.cpp:286 is a plain `std::move` — NOT thread-safe). The OLD `setMicConsumer({})` blocked via `micConsumerMtx_`. Stage 2b needs to add this thread safety to `Ep6RecvThread::set_mic_sink` OR document that the destructor must run after `ep6Thread_.stop()`.

**Sub-question:** thread-safety pattern for `set_mic_sink({})` during teardown — recommend the same `std::mutex` pattern HL2Stream used (`micConsumerMtx_` analog inside Ep6RecvThread). Adds one mutex + lock_guard inside `process_usb_frame` mic fire and inside `set_mic_sink`.

#### 13c. Add `updateMicDbFs` method to Hl2Ep6MicSource

New header member + impl:
```cpp
// mic_source.h:
private:
    double micAcc_   = 0.0;
    int    micCount_ = 0;
    int    micLogCounter_ = 0;
    static constexpr int kMicWindowSamples = 9600;
    static constexpr int kMicLogEveryWindows = 20;
    void updateMicDbFs(const float* mono, int n);

// mic_source.cpp:
void Hl2Ep6MicSource::updateMicDbFs(const float* mono, int n) {
    for (int i = 0; i < n; ++i) {
        const double v = mono[i];
        micAcc_ += v * v;
        if (++micCount_ >= kMicWindowSamples) {
            const double meanSq = micAcc_ /
                                  static_cast<double>(kMicWindowSamples);
            const double db = (meanSq > 0.0)
                ? 10.0 * std::log10(meanSq) : -200.0;
            stream_.setMicDbFsFromSource(db);
            micAcc_ = 0.0;
            micCount_ = 0;
            if (++micLogCounter_ >= kMicLogEveryWindows) {
                stream_.safetyLog(QStringLiteral(
                    "Q6.5 mic bench: %1 dBFS")
                    .arg(db, 0, 'f', 1));
                micLogCounter_ = 0;
            }
        }
    }
}
```

**New HL2Stream public method** `setMicDbFsFromSource(double db)` — writes the `micDbFs_` atomic. Single-line setter; one external caller (Hl2Ep6MicSource).

**Lyra-native classification:** the mic dBFS RMS instrument is Lyra-native — reference has no equivalent. Moving its ownership to Hl2Ep6MicSource (which already owns the mic stream) is the correct architectural fit. ACCEPTABLE LYRA-NATIVE.

---

## Open sub-questions for the 2-agent red-team

1. **Thread safety on `Ep6RecvThread::set_mic_sink({})` during teardown** — does the EP6 thread join order in `HL2Stream::close()` (file 5) guarantee no mic callback in flight when `Hl2Ep6MicSource::~Hl2Ep6MicSource()` runs? Or do we need an internal mutex on `set_mic_sink` matching the old `micConsumerMtx_` pattern?

2. **Router::set_call_count(2) timing** — what happens if `xrouter` is already dispatching when set_call_count is mutated? The Router has `cs_update` (line 69) for "update sites" — does this protect runtime ncalls changes? Need verification before relying on it.

3. **`prn->adc[0].adc_overload` value semantics** — currently a Lyra `int`. The Auto-LNA `_evaluate_pullup` consumer compares `ov != 0`. Reference single-frame assignment per `networkproto1.c:502` writes raw `ControlBytesIn[1] & 0x01` (0 or 1). The case-0x20 path at `networkproto1.c:519-523` writes `(ControlBytesIn[2] & 1) << 1` (0 or 2) for `adc[1]` and `(ControlBytesIn[3] & 1) << 2` (0 or 4) for `adc[2]`. For HL2 the operator only cares about `adc[0]`. `(prn->adc[0].adc_overload != 0)` is the correct boolean read for HL2. CONFIRMED.

4. **`txWorker_` END-OF-DATAGRAM error log path** — when `metis_write_frame` fails (returns < 0), the OLD code emitted `txSendErrors_`. New code does the same. But `metis_write_frame` ALSO logs "sendto failed with error:%d" to stderr internally (MetisFrame.cpp:103). Is that double-logging acceptable, or should we suppress one side?

5. **Build verification** — Stage 2b touches ~7 files with ~400 lines deleted + ~150 added. Build will likely surface several integration issues (references to deleted members from places I haven't grep'd yet). Plan: iterative build-fix cycle is acceptable for Stage 2b given size.

---

## Test plan

### Audit #1 (Claude side-by-side parity table)

Cite table after edits, file:line on both Lyra and reference sides for every change. Specific items to cite:
- EP6 recv flow vs `MetisReadDirect` + `MetisReadThreadMainLoop_HL2`
- Per-DDC IQ dispatch vs `networkproto1.c:550`
- HW PTT shadow vs `networkproto1.c:496`
- Telemetry slot decode vs `networkproto1.c:498-524` (already in Ep6RecvThread; no change in 2b)
- Telemetry accessors read `prn->tx[0].exciter_power` etc. per reference write sites
- `metis_write_frame` shared counter usage vs `MetisOutBoundSeqNum` post-increment at `networkproto1.c:231`
- `close()` stop ordering vs `IOThreadStop` pattern

### Audit #2 (operator bench)

This is the LARGE bench gate per STEP14_PLAN.md Stage 2 +  the §15.21 audit's operator-facing-surface coverage:

1. **Cold open**: Lyra opens stream, RX comes up cleanly, no fatal-error path
2. **RX audio**: tune to known signal (e.g. AM broadcaster, contest band), confirm audio through AK4951
3. **Panadapter**: signal at right place, noise floor correct, no chunkiness
4. **Telemetry banner**: T / V / PA fields update from prn->...
5. **Auto-LNA**: noise floor / overload back-off works (verify via Settings indicator + meter)
6. **HW PTT** (if opted-in): foot switch keys MOX bit
7. **rx1DbFs Q_PROPERTY**: drives the QML banner; updates ~20Hz
8. **micDbFs Q_PROPERTY**: drives the QML banner + "Q6.5 mic bench" safetyLog ~1Hz
9. **Seq error counter**: ~0 in steady state (or ~once per 3:27 wrap on HL2+ ak4951v4 per Rule 24)
10. **Framing error counter**: 0 in steady state
11. **Total dg counter**: ~5050/s
12. **MOX bit flip without PA**: brief MOX press confirms wire flip via the EP2 frame (PA stays at idle ~0.2A)
13. **30-min soak**: no audio glitches, no abnormal counter increments
14. **×5 stop/restart cycles**: clean each time (no dead-RX, no leaked mic, no spurious MOX — cb58bcb invariant holds)

### Rollback

Single `git revert <stage-2b-sha>` returns to Stage 2a state (commit `39f3fa9`). If 2a needs reverting too: `git reset --hard 4a2ff97` returns to pre-Stage-2 state (per the `_backups/lyra-cpp-2026-06-07-pre-step14-stage2.bundle` bundle).

---

## Lyra-native acceptance summary

The Stage 2b end-state has the following Lyra-native classifications (every deviation from "do as reference" verbatim is enumerated here for operator review):

| Item | Classification | Justification |
|---|---|---|
| `Ep6RecvThread` Q_OBJECT-less class | Lyra-native (reference is C, no Q_OBJECT) | C↔C++23 idiom translation per Rule 26 |
| `ep6_total_datagrams`/`ep6_framing_errors` TU-scope counters | Lyra-native instruments | Reference C# UI uses different observability; operator-facing UX has no reference port |
| `accumulateRx1Rms` registered at Router call_idx=1 | Lyra-native instrument | Router's `ncalls` axis IS reference-architectural; usage of call_idx=1 for an instrument is consistent with table shape |
| `Hl2Ep6MicSource` owning the mic dBFS RMS | Lyra-native instrument | Reference has no equivalent; ownership-with-the-mic-stream is cleanest fit |
| `set_hw_ptt_sink` callback indirection | Lyra-native (reference reads `prn->ptt_in` direct elsewhere) | Q_OBJECT consumer boundary; acceptable C↔C++23 idiom |
| HW PTT `Qt::QueuedConnection` invokeMethod | Lyra-native (reference is single-threaded C# UI) | Qt threading boundary; necessary for Lyra's QML target |
| `metis_write_frame(0x02, pktBytes + 8)` skip-header migration | REFERENCE-FAITHFUL | Single shared `MetisOutBoundSeqNum` per reference posture |
| Telemetry accessors reading `prn->...` direct | REFERENCE-FAITHFUL | Matches reference's `_radionet` single source of truth |
| Auto-LNA reading `prn->adc[0].adc_overload` direct | REFERENCE-FAITHFUL | Same |
| `-1` "no telemetry yet" sentinel REMOVED | REFERENCE-FAITHFUL | Reference reads int=0; brief -50°C startup glitch matches reference UX |
| Init-sem release at MMCSS-classified point | REFERENCE-FAITHFUL (already shipped in 2a) | Matches `networkproto1.c:249` |

All Lyra-native items above are operator-facing instruments or C↔C++23 idiom translations — none introduce protocol-level deviations from reference wire behavior. **End-state wire behavior is byte-identical to reference posture** for HL2/HL2+ ak4951v4.

---

**Awaiting:** 2-agent red-team check on this design paper. Then operator sign-off. Then code.
