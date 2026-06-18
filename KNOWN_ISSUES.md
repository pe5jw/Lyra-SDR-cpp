# Lyra — Known issues & what's not built yet

Lyra is in **active development**. It's a capable daily-driver transceiver
for the Hermes Lite 2 / 2+, but it isn't finished — this page sets
expectations so you know what's a real bug versus a feature that simply
hasn't landed yet. (Pin or link this in Discord.)

Current release: **v0.4.0**. Always grab the latest from the
[Releases page](https://github.com/N8SDR1/Lyra-SDR-cpp/releases).

---

## Not implemented yet (please don't file these as bugs)

These are on the roadmap, not broken:

* **CW transmit** — CW *receive* works fully; CW *keying* is not implemented
  yet (internal keyer, sidetone, message memories are designed and coming).
* **Dual receiver (RX2)** — single receiver today; stereo-split / SPLIT
  pile-up operation is planned.
* **PureSignal** — adaptive pre-distortion is on the roadmap, not present.
* **macOS / Linux** — Windows only for now; the codebase is built to port
  later.

## Expected behavior (not bugs)

* **First launch is slow / unresponsive for a minute.** Lyra does a one-time
  FFT optimization tuned to your machine and caches it; subsequent launches
  are fast. (Settings → can rebuild this cache if you change hardware.)
* **Panadapter looks "wide/smeared" while transmitting.** During TX the RX
  front-end is deliberately attenuated to protect it; the panadapter is fed
  from that protected receiver, so the on-screen trace is not a measure of
  your transmitted signal quality. Use an external monitor (a second SDR, or
  a watt/SWR meter) to judge the actual transmitted signal.

---

## Reporting a bug

Open a [GitHub issue](https://github.com/N8SDR1/Lyra-SDR-cpp/issues) or post
in [Discord](https://discord.gg/nbJEqvFQ). To make it actionable, include:

1. **Lyra version** (Help → About, or the installer filename).
2. **Windows version**, and whether your radio is an **HL2 or HL2+**.
3. **What you did and what happened** — steps to reproduce.
4. The in-app **diagnostic log** (open the diagnostic log viewer in Lyra and
   copy the relevant lines).

Thanks for testing Lyra! — N8SDR
