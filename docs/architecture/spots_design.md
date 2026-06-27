# Spots subsystem design (DX spots on the panadapter + console import)

**Status:** LOCKED design, not yet built. Supersedes the original narrow scope of
issue #182 ("grab a spot as call import → console contact row"), which becomes the
console-import slice of this larger subsystem.

**Provenance:** design lifted from the operator's own SDRLogger+ / hamlog app
(`Y:/Claude local/hamlog/`, Flask + JS). We port the proven logic Lyra-native
(C++/Qt6); hamlog file:line anchors are recorded here for reference only and do not
appear in shipped code/strings (open-attribution rule: operator-focused UI text, no
provenance leakage).

**Companion features:** [[project_lyra_cpp_cw_macros]] (#176 CW console contact row +
`{CALL}` token consumer), #181 (decoder→console import, SHIPPED), #173 CW decoder.

---

## 1. Scope decision (operator-locked)

Lyra is a **transceiver, not a logger**. Anything that needs a QSO database is OUT.

**CUT (logger-dependent):** worked-before status, new-entity / new-zone / hot-list
category coloring, DXCC-prefix / CQ-zone / continent *filters*. (hamlog has all of
these; they require `/api/worked_before_batch` against a log — no place in Lyra.)

**KEEP (transceiver-relevant):** band filter, type/mode filter, age, on/off toggles,
clutter caps, and a *static-table* visual layer (country/region from callsign prefix —
no log dependency; see §6).

---

## 2. Architecture — source-agnostic spot bus

```
[TCI feeder]   [SpotHole REST]   [DX-cluster telnet]      ← pluggable sources (§3)
       \             |                  /
        →   normalized Spot model (one struct, §4) — the "spot bus"
                     |
   FILTER stage:  band → type → age                       (§5)
                     |
   CLUTTER stage: per-frequency bucket cap → global cap   (§5.4 — NEW design)
                     |
        ┌────────────┴────────────┐
   panadapter overlay        spot list  ──► #182 import call into CW-console contact row
   (visual layer §6)
```

Sources are **pluggable feeders**; everything downstream (filter, clutter, overlay,
import) is **source-agnostic** — it reads the bus, never the source. Adding/removing a
source is a feeder on/off, nothing else changes. Master toggles: **Spots on/off** +
**per-source on/off**.

hamlog already proves this model: both its sources normalize into one `allSpots` array
(hamlog `index.html:5040-5068`), capped at 200 in memory.

---

## 3. The three sources

### 3.1 TCI (inbound from SDRLogger+)
SDRLogger+ already emits spots over TCI. In hamlog the TCI spot route is the *consumer*
side (pushes markers TO Thetis + a `freq→call` registry, `main.py:3808-3832`). For Lyra
standalone we run TCI as a real **inbound** feeder (SDRLogger+ → Lyra). Small inversion,
not a rewrite. Lyra already speaks TCI (CAT/TCI infrastructure in place).

### 3.2 SpotHole (REST aggregator, standalone)
Public REST aggregator at `https://spothole.app/api/v1/spots`. **Not a single feed** —
it bundles cluster + POTA + skimmer spots **across all modes**, with a `source` param so
the user selects sub-feeds (Cluster, POTA, …). Standalone: internet only, no SDRLogger+,
no telnet node to configure.

- hamlog poll interval: **30 s** (`main.py:4570`).
- Initial `max_age` on connect: **600 s** (`main.py:4527`).
- Query params: `max_age`, `band` (comma-sep), `source`.
- Spot JSON fields: `freq` (Hz), `band`, `dxCall`, `mode`, `spotter`, `comment`,
  `received_time`, `snr`/`signal` (optional).

Lyra: a `QNetworkAccessManager` poller every 30 s (configurable), parse JSON → bus.

### 3.3 DX-cluster telnet (user-selected node, standalone)
For operators who want a specific node (e.g. VE7CC / CC-Cluster, DXSpider, AR-Cluster).

**Config fields:**
- **Host** (e.g. `dxc.ve7cc.net`)
- **Port** (e.g. `23` / `7300`; varies by node)
- **Login callsign** — **defaults to Lyra MYCALL, fully overridable.** This is a
  *separate field*, NOT a reuse of MYCALL: the login call may be a friend's login, a
  **club call**, or a node-required registered call. Some nodes gate on it.
- *(optional, advanced)* **login-commands** line — free text sent right after login
  (`set/ft8`, `set/filter …`, etc.). Leave blank = default node behavior.

**Connect flow:** open `QTcpSocket` → node prompts → send login call → (optional extra
commands) → parse `DX de <spotter>: <freq> <dxcall> <comment> <time>` lines → bus.
(hamlog telnet helper: `main.py:5022`.)

Server-side cluster filters exist but we treat our **client-side filter chain (§5) as
canonical** so a flooding node behaves the same as SpotHole.

---

## 4. Normalized spot model (the bus struct)

Mirror hamlog's unified model (`index.html:5040-5068`):

```cpp
struct Spot {
    QString  call;        // DX callsign, uppercased
    QString  band;        // "20m" etc. — computed from freq if absent (§5.1)
    QString  mode;        // "FT8","CW","USB","LSB",...
    double   freqHz;      // precision-preserving
    QString  spotter;     // who reported it
    QString  comment;
    QString  source;      // "tci" | "spothole" | "telnet:<host>"  (for per-source on/off)
    qint64   tsMs;        // arrival monotonic ms — drives age (§5.3)
    int      snrDb;       // optional (skimmer/RBN-style)
    // visual-layer derived (§6), filled by the resolver:
    QString  entity;      // country / DXCC entity name
    QString  entityAbbr;  // short prefix/abbrev for the label
    QString  continent;   // NA/EU/AS/SA/AF/OC
};
```

- **Memory cap:** keep the in-memory bus bounded (hamlog uses 200). Ring/deque.
- **Dedup on ingest:** **call + band** → update in place (refresh ts + freq), as hamlog
  does (`index.html:5042`). NB this is the only dedup hamlog has — see §5.4.

---

## 5. Filter + clutter chain (source-agnostic)

Order: **band → type → age → clutter caps**, all gated by the on/off toggles.

### 5.1 Band filter
Show only the **current band**'s spots ("on 20 m, only 20 m"). Current band is derived
from the RX1 dial via a band-edge table (hamlog `freqKhzToBand()`, `index.html:4672-4688`,
13 bands 160m–70cm). Setting "current band = none/all" shows every band. Lyra already
knows the dial freq; reuse/extend any existing band table.

### 5.2 Type / mode filter
Include/exclude by mode (no FT8 / CW-only / etc.). Case-insensitive whitelist with the
**`SSB → {SSB,USB,LSB}` expansion** (cluster spots are tagged USB/LSB, hamlog
`index.html:5275`). Default = empty = all modes. Filterable set from hamlog parsing:
FT8 FT4 MSK144 Q65 FST4(W) JT65 JT9 CW SSB USB LSB RTTY JS8 AM FM DIGI DATA WSPR PSK31
VARAC OLIVIA HELL SSTV PACKET.

### 5.3 Age / expiry
Drop spots older than N minutes. hamlog: default **10 min**, options 5/10/15/30, pruned
every **30 s** (`index.html:2430,4744,5148`). A `QTimer` prune + a per-render age check.

### 5.4 Clutter caps — TWO levels
**Global cap** (PORT): max spots shown on the display total. hamlog default **25**,
range **1–100** (`index.html:2433,5306`). Hard slice after sort.

**Per-frequency bucket cap (NEW — hamlog does NOT do this).** hamlog dedups by
**call+band only**, so a busy FT8 watering hole stacks many calls on one frequency. This
is the operator's specific ask ("don't want 10 FT8 spots cluttering one frequency").
Design:
- Bin spots by frequency; **bucket width is mode-aware** (tight ~±200 Hz for FT8/digital,
  wider for SSB) to match real signal spacing.
- Keep the **N most-recent (or strongest-SNR) per bucket**, configurable, default ~3.
- Collapse the remainder into a **"+K more" badge** on the marker.
- Runs *on top of* the global cap.

---

## 6. Visual layer (panadapter overlay) — all optional, default-off

The panadapter stays clean unless the operator opts in. Powered by ONE static asset:

**Callsign-prefix → {entity, abbrev, continent, color} resolver.** Standard source =
**cty.dat** (AD1C country file): free, comprehensive, periodically updated. Bundle a
snapshot, allow refresh. **Static table — zero log dependency** (this is what makes
country/region OK after cutting worked-before).

### 6.1 Color-mode picker
- **Single color** (default)
- **By mode** — FT8/CW/SSB/digital tints
- **By region** — continent (NA/EU/AS/SA/AF/OC)
- **By country/DXCC** — finer (uses the fuller prefix table)
- + a small **legend** so colors are readable ("sorted colors").

### 6.2 Country/region label
Optional toggle: prepend the entity abbreviation before the call on the marker, e.g.
`DL · DJ5XX`, `JA · JA1XX`. Independent on/off from coloring. Same resolver.

### 6.3 Marker rendering / interaction
- Map `freqHz → x` on the panadapter span; draw a tick + label.
- Click-to-tune (QSY to the spot).
- **Click-to-import (#182):** push the spot's call (and freq/mode) into the **CW-console
  contact row** — the same row #176 macros tokenize (`{CALL}` etc.) and #181 fills from
  the decoder. hamlog equivalent: `fillFromSpot()` (`index.html:5494`).

---

## 7. Settings surface (sketch)

- **Spots** master on/off.
- **Sources:** TCI on/off · SpotHole on/off (+ sub-feed `source` picker, poll interval) ·
  Telnet on/off (host, port, login-call [default MYCALL], optional login-commands).
- **Filters:** band (current-only / all), mode include/exclude, age (5/10/15/30).
- **Clutter:** global max (1–100, dflt 25), per-bucket max (dflt ~3), bucket width policy.
- **Visual:** color mode (single/mode/region/country) + color pickers + legend toggle;
  country-abbrev label toggle.

---

## 8. Build order + status

1. ✅ Spot model + bus + dedup + memory cap. — `SpotStore` (call-keyed dedup,
   cap 200, age-out, own-call highlight/notify/new-spot colour).
2. ✅ TCI source end-to-end — `tci_server` → `SpotStore::addSpot`, tagged
   `source="tci"`.
3. 🟡 Filter chain — **mode** display filter ✅ (`SpotStore::modeShown`, family-
   aware, Settings "Show modes"), **age** + memory cap ✅. Band display filter
   DEFERRED (redundant on the panadapter — only in-span spots draw anyway;
   belongs with the spot-list view). Global "max shown after sort" cap TODO.
4. ✅ Panadapter overlay + click-to-tune (`spotsInSpan` + `activate`) +
   **#182 console import** — spot-click sets `CwMacros.hisCall` (the CW-console
   contact row / `{CALL}` token).
5. ✅ SpotHole REST feeder — `SpotHoleFeeder` (poll `spothole.app/api/v1/spots`,
   tagged `source="spothole"`, per-source on/off via `clearSource`). Settings:
   enable, sub-source, current-band-only, poll interval, initial age, refresh.
6. ⬜ Telnet feeder (host/port/login-call).
7. ⬜ Per-frequency bucket cap (the new clutter piece).
8. 🟡 Visual layer — DXCC country abbrev + coloring exist (`dxcc.*`); cty.dat
   color-mode picker (mode/region/country) + legend still TODO.

**Bus now has TWO live sources (TCI + SpotHole).** Remaining for the full
subsystem: band/mode display filters + global cap (3), console import (4/#182),
telnet feeder (6), per-freq bucket cap (7), color-mode picker + legend (8).
