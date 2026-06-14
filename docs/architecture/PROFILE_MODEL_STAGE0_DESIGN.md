# Stage 0 — Profile model (design, grounded)

**Status:** DESIGN (2026-06-14), awaiting operator sign-off before code.
Stage 0 of `AUDIO_IO_AND_PROFILES_PLAN.md`: the profile **container** —
no host audio yet.  Wires the EXISTING operator TX/RX state through a
named-profile model so "switch profile → recall" works; reserves the
future fields (VAC/EQ/Combinator) in the schema so no migration later.
Touches QSettings + existing setters + UI ONLY — NOT the wire/RT path,
so no charter red-team; standard operator bench gate.

---

## 1. Where the state lives (grounded, the apply targets)

| Field | Home (verified) | Setter |
|---|---|---|
| mode | `Prefs.mode` (prefs.h:164) | `setMode` |
| rxBandwidth | `Prefs.rxBandwidth` (:165) | `setRxBandwidth` |
| txBandwidth | `Prefs.txBandwidth` (:173) | `setTxBandwidth` (→ `HL2Stream::setTxBwHz`) |
| bwLocked | `Prefs.bwLocked` (:180) | `setBwLocked` |
| filterLow | `Prefs.filterLow` (:189) | `setFilterLow` |
| micSource | `Prefs.micSource` (:282) | `setMicSource` (→ `applyTciTxSource` gate, main.cpp) |
| useTuneDrive / tuneDrivePct | `Prefs` (:51/:53) | `setUseTuneDrive` / `setTuneDrivePct` |
| tciRxGainDb / tciTxGainDb | `Prefs` (:62/:72) | setters |
| txDbMin / txDbMax (TX pan) | `Prefs` (:38/:40) | setters |
| micGainDb | `HL2Stream.micGainDb` (hl2_stream.h:370) | `setMicGainDb` |
| txDriveLevel / TX power % | `HL2Stream.txDriveLevel` (:228) + `Radio.set_tx_power_pct` | setters |
| agcMode | `WdspEngine.agcMode` (wdsp_engine.h:157) | `setAgcMode` |
| autoMuteOnTx | `WdspEngine.autoMuteOnTx` | `setAutoMuteOnTx` |
| ATT-on-TX enable/dB | radio policy (`_att_on_tx_enabled`/`_ATT_ON_TX_DB`) | (operator-facing setter pending) |
| txTimeout sec / bypass | `HL2Stream` (`txTimeoutSec`/`txTimeoutBypass`) | setters |
| micBoost (20 dB) | Prefs/stream (verify at code time) | setter |

State is spread across **Prefs + HL2Stream + WdspEngine** → the
`ProfileManager` holds pointers to all three (constructed in main.cpp
next to them, like the other coordinators).

### Explicitly NOT in a profile (global / safety)
- **PA enable** — deliberate operator safety gate (§15.26 PART C); must
  NOT flip via a profile recall.  Stays global.
- **HW-PTT-input enable** (`hwPttEnabled`, `ff5f128`) + **space-bar PTT**
  (`spaceBarPttEnabled`) — input-method prefs, global not per-profile.
- Discovery/IP, audio-OUT device-not-yet-existing, panadapter visuals.
(Operator can override these classifications — see §6.)

---

## 2. Data model

```cpp
// src/profile/Profile.h  (new src/profile/ package)
struct Profile {
    QString name;
    int     schemaVersion = 1;
    // --- Stage-0 LIVE fields (captured/applied now) ---
    QString mode;                 // "" = don't touch (per-field include)
    int     rxBandwidth = -1;     // -1 sentinel = field not stored/include-off
    int     txBandwidth = -1;
    bool    bwLocked = false;     bool has_bwLocked = false;
    int     filterLow = -1;
    QString micSource;            // mic1 / tci  (vac1/vac2 reserved)
    double  micGainDb = NAN;
    bool    useTuneDrive = false; bool has_useTuneDrive = false;
    int     tuneDrivePct = -1;
    int     txPowerPct = -1;
    double  tciRxGainDb = NAN, tciTxGainDb = NAN;
    QString agcMode;
    bool    autoMuteOnTx = false; bool has_autoMuteOnTx = false;
    bool    attOnTxEnabled = false; bool has_attOnTx = false; int attOnTxDb = -1;
    int     txTimeoutSec = -1;    bool txTimeoutBypass = false; bool has_txTimeout = false;
    bool    micBoost = false;     bool has_micBoost = false;
    // --- RESERVED (schema slots, inert until the feature ships) ---
    // vac1{enable,driver,inDev,outDev,rateHz,gainRxDb,gainTxDb,autoEnableDigital,...}
    // vac2{...}; eq[]; combinator{}; tube{}; deEsser{}; phrot{}; cessb; amCarrier;
    // monitorAfLevel; leveler{}; plate{}
};
```

**Per-field "include" via sentinels** (NAN / -1 / has_* flags): mirrors
Thetis's per-category "Restore … from profile" opt-ins (e.g. Restore-VAC
is opt-in).  A field left at sentinel is "profile doesn't manage this —
leave live value alone."  Default new profiles capture everything;
operator can clear includes per field.

---

## 3. Storage — QSettings JSON (locked)

```
profiles/order        = StringList  ["Default","Digi","AM-ESSB",...]   (UI order)
profiles/active       = "Digi"
profiles/item/<name>  = QString (compact JSON of the Profile struct)
profiles/modeBind/<MODE> = "<profileName>"   // per-mode auto-recall (DIGU→Digi)
profiles/restoreVacDevice = bool   // global opt-in (Thetis chkRestoreVAC*)
```
Serialize via `QJsonObject`/`QJsonDocument::Compact` → string into one
QSettings key per profile (no SQLite dep; matches the #49 spec + the
existing Prefs/QSettings convention).  `schemaVersion` gates future
field migrations.

---

## 4. ProfileManager (the engine) — `src/profile/ProfileManager.{h,cpp}`

`QObject`, constructed in main.cpp with `(prefs, stream, wdspEngine)`.

- `QStringList profileNames()` / `QString activeName()`
- `Profile capture() const` — read CURRENT live state across the 3
  objects into a Profile (the "Save" path).
- `void apply(const Profile&)` — push every non-sentinel field through
  its setter **in one ordered pass**; `_applying=true` guard so the
  per-setter signals don't mark the profile dirty mid-apply.
  Apply ORDER: mode → BW(rx/tx)+lock+filterLow → micSource (via the
  gate) → gains/agc/autoMute/att/timeout.  **Guarded: refuse/defer
  apply while `dispatch_state.mox` (no source/BW switch mid-TX)** —
  apply on the next RX-rest edge (reuse the §15.25 discipline).
- `save(name)` (= capture()+store), `load(name)` (= fetch+apply+setActive),
  `remove(name)`, `rename`, `exportToFile(name,path)`, `importFromFile(path)`,
  `setDefault(name)`.
- **Dirty tracking:** compare live `capture()` vs stored active profile →
  `bool isModified()` + `modifiedChanged(bool)` signal (drives the
  orange-banner-equivalent).  Optional **auto-save-on-change** toggle
  (Thetis parity).
- **Per-mode binding:** connect `Prefs.modeChanged`; if
  `profiles/modeBind/<newMode>` set AND not mid-TX, `load(boundName)`.
  ⚠ Precedence note: this interacts with existing **per-band memory**
  (#74 per-band TUN drive). Decide precedence (proposal: per-mode
  profile binding is opt-in and, when set for a mode, wins for the
  fields it includes; per-band memory fills the rest).  Flag for §6.

---

## 5. UI — two surfaces, one model

**(a) Settings → Profiles tab** (`buildProfilesTab()`, registered in the
`tabs_->addTab` block ~settingsdialog.cpp:79-103, after TX):
- Profile list (reorderable) + **Save / Save As / Delete / Rename /
  Export / Import / Set Default**
- Per-field **include** checkboxes (the Thetis "Restore … from profile"
  idiom) grouped (TX / Audio-route / DSP)
- **Per-mode binding** table (mode → profile dropdown)
- Global **Restore-VAC-device-from-profile** toggle + auto-save-on-change
- "Profile modified — Save to store" banner (orange-equivalent)

**(b) Front-panel `ProfilePanel` dock** (`GlassPanel`, sibling of
`TxPanel`; dockable, hidden-by-default like the §15.26 lazy TX dock):
- Profile **dropdown** (one-click recall) + **Save** + **modified ●**
  indicator.  Thin view over ProfileManager; no logic of its own.
- This IS task **#55**; the Settings tab is task **#49**.

---

## 6. Open schema decisions (need operator nod before code)
1. **Field classification** — confirm PA-enable + PTT-input-method +
   space-bar stay GLOBAL (not per-profile).  Anything else you want
   global vs per-profile?
2. **Per-field include vs whole-profile apply** — ship the per-field
   "include" sentinels (Thetis-like granularity), or simplest first
   (a profile always captures+applies the full Stage-0 set, add
   per-field includes later)?  (Recommend: whole-set first, includes
   as a fast-follow — less UI, gets recall working sooner.)
3. **Per-mode binding vs per-band memory precedence** (§4 note) — OK
   with "bound profile wins for its included fields; per-band memory
   fills the rest"?
4. **Ship a few default profiles?** (e.g. `SSB`, `Digi(TCI)`, `AM`) like
   Thetis's Default/Default-DX/Digi/AM, or start empty + operator builds.

On your nod (esp. #2 — it sizes the first commit), I build Stage 0:
`src/profile/` (Profile + ProfileManager) + QSettings store + the
Settings→Profiles tab + the ProfilePanel dock, wired to the existing
fields, with a unit test for capture/apply/round-trip + an operator
bench (make a profile, switch modes, confirm recall).
