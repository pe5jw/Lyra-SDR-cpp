// Lyra — native real-time Morse (CW) decoder.  Task #173 (CW-5a).
// See CwDecoder.h for the chain overview.  Faithful port of the operator's
// own decoder; the ~35 tuned constants below are carried verbatim because
// the tuning IS the value (envelope/floor/peak time-constants, hysteresis
// bands, AFC lock thresholds, Bayesian learning rates).  Comments are in
// first-principles DSP terms.

#include "dsp/CwDecoder.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lyra::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;

// ── impulse noise blanker ──
constexpr double NB_RATIO = 3.5;    // clamp |mag| above this multiple of the
constexpr double NB_ALPHA = 0.02;   // slow-tracked running average

// ── AFC multi-signal rejection ──
constexpr int    AFC_LOCK_MIN = 3;     // consecutive scans before a jump
constexpr double AFC_JUMP_HZ  = 40.0;  // distance that counts as a new signal

// ── noise-floor holdoff after each mark→space edge ──
constexpr double FLOOR_HOLDOFF_T = 1.2;  // freeze upward tracking for 1.2 dit

// ── asymmetric envelope follower ──
constexpr double ENV_ATK = 0.30;   // fast attack — catches the mark leading edge
constexpr double ENV_DKY = 0.09;   // slow decay — clean mark→space transitions

// ── adaptive noise floor + peak ──
constexpr double FLOOR_RISE = 0.002;
constexpr double FLOOR_FALL = 0.010;
constexpr double PEAK_FALL  = 0.003;

// ── proportional hysteresis (scales with the slicer threshold) ──
constexpr double HYST_ON_RATIO  = 0.15;
constexpr double HYST_OFF_RATIO = 0.30;
constexpr double HYST_ON_MIN    = 0.02;
constexpr double HYST_OFF_MIN   = 0.04;
constexpr double HYST_MARK_END_FLOOR = 0.02;

// ── AFC scan ──
constexpr int AFC_WIN    = 2048;   // Goertzel window (~23 Hz bins at 48 kHz)
constexpr int AFC_PERIOD = 12000;  // rescan cadence in samples (~250 ms)
constexpr int AFC_STEP   = 20;     // scan step in Hz

// ── timing bounds + Bayesian model ──
constexpr double DOT_MIN_MS = 20.0, DOT_MAX_MS = 320.0;
constexpr int    BAYES_MIN  = 5;     // marks before the Gaussian model engages
constexpr double BAYES_ALPHA= 0.15;  // EMA weight per confirmed observation
constexpr double MARK_BOUNDARY = 1.565;  // bootstrap dit/dah split (× dit)

// ── A-slice accuracy additions (NOT field-tuned timing constants; these are
//    additive guards / advisory-output scales and touch none of the above) ──
constexpr int    PEAK_SNAP_PERSIST = 3;    // A5: windows an elevated level must hold
constexpr double AFC_CENTER_BIAS   = 0.5;  // A2: an edge peak needs ~1.5× to win
constexpr double CONF_BOUNDARY_SPAN= 0.5;  // A3: bootstrap margin → full confidence
constexpr double CONF_MARGIN_SCALE = 2.0;  // A3: log-likelihood margin half-confidence
constexpr double CONF_SNR_SPAN     = 4.0;  // A3: (snr/squelch − 1) span to full conf
constexpr double CONF_W_MARGIN     = 0.6;  // A3: margin weight in the blend
constexpr double CONF_W_SNR        = 0.4;  // A3: SNR weight in the blend (sum = 1)
constexpr double CONF_RESCUED_SCALE= 0.5;  // A1/A3: rescued char confidence penalty
constexpr double SNR_HYST          = 0.8;  // A6: gate holds open down to 0.8× squelch

// edit distance == 1 over the dot/dash alphabet (substitution / one insert /
// one delete).  a ≠ b is guaranteed by the caller (a is a table miss, b a key),
// so distance ≥ 1; this returns true iff it is exactly 1.
bool isOneEdit(const std::string& a, const std::string& b) {
    const int la = static_cast<int>(a.size()), lb = static_cast<int>(b.size());
    if (std::abs(la - lb) > 1) return false;
    if (la == lb) {
        int diff = 0;
        for (int i = 0; i < la; ++i)
            if (a[i] != b[i] && ++diff > 1) return false;
        return diff == 1;
    }
    const std::string& s = (la < lb) ? a : b;   // shorter
    const std::string& t = (la < lb) ? b : a;   // longer (one char extra)
    int i = 0, j = 0, skips = 0;
    while (i < static_cast<int>(s.size()) && j < static_cast<int>(t.size())) {
        if (s[i] == t[j]) { ++i; ++j; }
        else if (++skips > 1) return false;
        else ++j;
    }
    return true;   // any unmatched tail of t is the single insertion
}

const std::unordered_map<std::string, char>& morseTable() {
    static const std::unordered_map<std::string, char> t = {
        {".-",'A'},{"-...",'B'},{"-.-.",'C'},{"-..",'D'},{".",'E'},
        {"..-.",'F'},{"--.",'G'},{"....",'H'},{"..",'I'},{".---",'J'},
        {"-.-",'K'},{".-..",'L'},{"--",'M'},{"-.",'N'},{"---",'O'},
        {".--.",'P'},{"--.-",'Q'},{".-.",'R'},{"...",'S'},{"-",'T'},
        {"..-",'U'},{"...-",'V'},{".--",'W'},{"-..-",'X'},{"-.--",'Y'},
        {"--..",'Z'},
        {"-----",'0'},{".----",'1'},{"..---",'2'},{"...--",'3'},{"....-",'4'},
        {".....",'5'},{"-....",'6'},{"--...",'7'},{"---..",'8'},{"----.",'9'},
        {".-.-.-",'.'},{"--..--",','},{"..--..",'?'},{".----.",'\''},
        {"-.-.--",'!'},{"-..-.",'/'},{"-.--.",'('},{"-.--.-",')'},
        {"-...-",'='},{".-.-.",'+'},{"-....-",'-'},{".-..-.",'"'},{".--.-.",'@'},
        {"---...",':'},{"-.-.-.",';'},
        // A8: non-colliding punctuation.  Prosigns (AR/KN/SK …) deliberately
        // omitted — they alias '+' / '(' / etc., so faithful prosign rendering
        // needs char-level context (Tier B); silently aliasing would be wrong.
        {"..--.-",'_'},{"...-..-",'$'},
    };
    return t;
}
} // namespace

CwDecoder::CwDecoder() { reset(); }

// ── configuration setters ────────────────────────────────────────────────
void CwDecoder::setSampleRate(double hz)   { if (hz > 0) sampleRate_ = hz; }
void CwDecoder::setToneHz(double hz)       { toneHz_ = hz; }
void CwDecoder::setSquelch(double snr)     { squelch_ = snr; }
void CwDecoder::setThreshold(double f)     { threshold_ = std::clamp(f, 0.0, 1.0); }
void CwDecoder::setAfcEnabled(bool on)     { afcEnabled_ = on; }
void CwDecoder::setAfcRange(int hz)        { afcRange_ = hz; }
void CwDecoder::setDspFilter(bool on)      { dspFilter_ = on; }
void CwDecoder::setNoiseBlanker(bool on)   { noiseBlanker_ = on; }
void CwDecoder::setTxWpm(int wpm)          { txWpm_ = wpm; }

void CwDecoder::seedTiming() {
    const int   w       = std::clamp(txWpm_, 5, 60);
    const double initDit = std::round(1200.0 / w);
    dotEstMs_     = initDit;
    bDitMu_       = initDit;
    bDitVar_      = std::pow(initDit * 0.30, 2.0);   // 30% CV prior
    bMarkN_       = 0;
    bElemSpaceMu_ = initDit * 0.85;                  // inter-element ≈ 0.85 dit
    bCharSpaceMu_ = 0.0;
    bWordSpaceMu_ = 0.0;
}

void CwDecoder::reset() {
    currentChar_.clear();
    lastEdge_ = 0.0; lastLevel_ = false;
    bufWasMark_.reset(); bufDur_ = 0.0;
    sampleTimeMs_ = 0.0;
    noiseFloor_ = 0.0; peakPower_ = 0.0; peakSnapHold_ = 0;
    charConfSum_ = 0.0; charSnrSum_ = 0.0; charConfN_ = 0;
    envVal_ = 0.0;
    iqI_ = iqQ_ = 0.0; iqCount_ = 0; iqPhaseAcc_ = 0.0;
    mfBuf_.fill(0.0); mfPos_ = 0; mfSum_ = 0.0; mfWinLast_ = 0;
    afcHz_.reset(); afcBuf_.clear(); afcCount_ = 0;
    afcLocked_ = false; afcPrevHz_.reset(); afcLockCount_ = 0;
    qsbFadeFlag_ = false; floorHoldoffEnd_ = 0.0; snrGateOpen_ = false;
    nbRunAvg_ = 0.0;
    lastEmitted_ = '\0';
    rxWpm_ = 0;
    seedTiming();
}

// ── Goertzel single-bin power ─────────────────────────────────────────────
double CwDecoder::goertzel(const float* s, int n, double targetHz) const {
    const int    k     = static_cast<int>(std::lround(n * targetHz / sampleRate_));
    const double omega = 2.0 * kPi * k / n;
    const double coeff = 2.0 * std::cos(omega);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = s[i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

void CwDecoder::updateAfcState(bool locked, double hz) {
    const bool changed = (locked != afcLocked_) ||
                         (locked && std::fabs(hz - afcHz_.value_or(hz)) > 0.5);
    afcLocked_ = locked;
    if (changed && onAfc) onAfc(locked, hz);
}

// AFC: sweep ±range about centerHz, lock to the strongest CW peak.  Requires
// the peak to be ≥3× the scan average (so noise won't lock), and — once
// locked — AFC_LOCK_MIN consecutive scans agreeing on a far new frequency
// before it will jump (multi-signal rejection).  Smooths 0.7 old / 0.3 new.
void CwDecoder::runAfc(const float* s, int n, double centerHz) {
    double bestHz = centerHz, bestScore = -1.0, bestPow = 0.0, totalPow = 0.0;
    int scanCount = 0;
    const double range2 = static_cast<double>(afcRange_) * afcRange_ + 1.0;
    for (int df = -afcRange_; df <= afcRange_; df += AFC_STEP) {
        const double hz = centerHz + df;
        if (hz < 200.0 || hz > 2000.0) continue;
        const double p = goertzel(s, n, hz);
        // A2: bias the pick toward the operator's tuned pitch (centerHz) so a
        // louder adjacent station can't steal the lock.  The weight decays with
        // distance²; an edge peak must be ~(1+AFC_CENTER_BIAS)× stronger to win.
        const double w     = 1.0 / (1.0 + AFC_CENTER_BIAS * (df * df) / range2);
        const double score = p * w;
        if (score > bestScore) { bestScore = score; bestHz = hz; bestPow = p; }
        totalPow += p; ++scanCount;   // lock threshold uses RAW power (unbiased)
    }

    // A4: the 20 Hz scan step is below the ~23 Hz Goertzel bin, so the integer
    // pick is coarse.  Refine bestHz to sub-bin precision with a 3-point
    // parabolic interpolation on the RAW power across the winning bin and its
    // two neighbours.  Only nudges bestHz (≤ ½ step); touches no slicer/timing
    // constant, and the lock threshold above already used the unbiased bestPow.
    if (scanCount > 0 && bestHz - AFC_STEP >= 200.0 && bestHz + AFC_STEP <= 2000.0) {
        const double pm = goertzel(s, n, bestHz - AFC_STEP);
        const double pp = goertzel(s, n, bestHz + AFC_STEP);
        const double denom = pm - 2.0 * bestPow + pp;
        if (denom < 0.0) {   // concave → a real peak; interpolate its vertex
            const double delta = std::clamp(0.5 * (pm - pp) / denom, -0.5, 0.5);
            bestHz += delta * AFC_STEP;
        }
    }

    if (scanCount == 0 || bestPow < (totalPow / scanCount) * 3.0) {
        afcLockCount_ = 0;
        const double hz = afcHz_.value_or(centerHz);
        afcHz_ = hz;
        updateAfcState(false, hz);
        return;
    }

    if (afcHz_ && std::fabs(bestHz - *afcHz_) > AFC_JUMP_HZ) {
        if (afcPrevHz_ && std::fabs(bestHz - *afcPrevHz_) < AFC_JUMP_HZ) ++afcLockCount_;
        else afcLockCount_ = 1;
        afcPrevHz_ = bestHz;
        if (afcLockCount_ < AFC_LOCK_MIN) {
            updateAfcState(true, *afcHz_);   // hold current lock
            return;
        }
        afcLockCount_ = 0;
        afcPrevHz_.reset();
    } else {
        afcLockCount_ = 0;
        afcPrevHz_.reset();
    }

    const double result = afcHz_ ? (*afcHz_ * 0.7 + bestHz * 0.3) : bestHz;
    afcHz_ = result;
    updateAfcState(true, result);
}

// ── Bayesian classifier ──
double CwDecoder::logGaussian(double x, double mu, double varr) {
    const double z = x - mu;
    return -(z * z) / (2.0 * std::max(varr, 1.0));
}

bool CwDecoder::bayesClassifyMark(double ms) const {
    if (bMarkN_ < BAYES_MIN) return ms < bDitMu_ * MARK_BOUNDARY;
    const double dahMu  = bDitMu_ * 3.0;
    const double dahVar = bDitVar_ * 3.0;   // dah spread ≈ 3× dit
    return logGaussian(ms, bDitMu_, bDitVar_) >= logGaussian(ms, dahMu, dahVar);
}

CwDecoder::Space CwDecoder::bayesClassifySpace(double ms) const {
    if (bMarkN_ < BAYES_MIN) {
        if (ms < dotEstMs_ * 1.6) return Space::Elem;   // √(0.85×3) ≈ 1.6
        if (ms < dotEstMs_ * 4.6) return Space::Char;   // √(3×7)    ≈ 4.6
        return Space::Word;
    }
    const double charMu = bCharSpaceMu_ > 0 ? bCharSpaceMu_ : bDitMu_ * 3.0;
    const double wordMu = bWordSpaceMu_ > 0 ? bWordSpaceMu_ : bDitMu_ * 7.0;
    const double llE1 = logGaussian(ms, bElemSpaceMu_, bDitVar_ * 3.0);
    const double llE3 = logGaussian(ms, charMu,        bDitVar_ * 6.0);
    const double llE7 = (ms >= wordMu) ? 0.0 : logGaussian(ms, wordMu, bDitVar_ * 10.0);
    if (llE7 >= llE3 && llE7 >= llE1) return Space::Word;
    if (llE3 >= llE1)                 return Space::Char;
    return Space::Elem;
}

// Update the dit model from a confirmed mark.  Outlier-guarded, EMA mean +
// variance with a speed-scaled floor, and an asymmetric rate: speed-ups
// tracked aggressively, slow-downs conservatively.  Keeps dotEst in sync and
// reports adaptive WPM.
void CwDecoder::bayesLearnMark(double ms, bool isDit) {
    const double ditSample = isDit ? ms : ms / 3.0;
    if (ditSample < bDitMu_ * 0.3 || ditSample > bDitMu_ * 2.5) return;   // outlier
    ++bMarkN_;
    const double baseAlpha = isDit ? BAYES_ALPHA : BAYES_ALPHA * 0.5;
    const double alpha     = ditSample < bDitMu_ ? baseAlpha : baseAlpha * 0.5;
    const double newMu     = bDitMu_ * (1.0 - alpha) + ditSample * alpha;
    const double dev       = ditSample - newMu;
    const double varFloor  = std::pow(bDitMu_ * 0.10, 2.0);
    bDitVar_ = std::max(bDitVar_ * (1.0 - alpha) + dev * dev * alpha, varFloor);
    bDitMu_  = std::clamp(newMu, DOT_MIN_MS, DOT_MAX_MS);
    dotEstMs_= bDitMu_;
    const int rxWpm = static_cast<int>(std::lround(1200.0 / dotEstMs_));
    if (rxWpm != rxWpm_) {
        rxWpm_ = rxWpm;
        if (onWpm) onWpm(rxWpm_);
    }
}

// ── A1: nearest-code rescue ──
// Runs ONLY on a Morse-table miss (a correct character never reaches it).
// Accept the unique table entry at edit-distance 1 (the dominant '?' cause on
// weak signals is one dropped/inserted element); reject ties / distance ≥2 so
// honest unknowns stay '?'.  Returns the rescued char, or '\0' for no rescue.
char CwDecoder::rescueNearest(const std::string& code) const {
    char found = '\0';
    int  count = 0;
    for (const auto& [k, ch] : morseTable()) {
        if (isOneEdit(code, k)) {
            if (++count > 1) return '\0';   // ambiguous — leave it '?'
            found = ch;
        }
    }
    return count == 1 ? found : '\0';
}

// ── A3: per-character confidence (advisory; never alters the decode) ──
// Dit/dah decision margin for one mark, mapped to 0..1.  Once the Gaussian
// model engages it is the (otherwise discarded) |LL_dit − LL_dah| separation;
// while bootstrapping it is the normalised distance from the dit/dah boundary.
double CwDecoder::markConfidence(double ms) const {
    if (bMarkN_ < BAYES_MIN) {
        const double bnd = bDitMu_ * MARK_BOUNDARY;
        const double d   = std::fabs(ms - bnd) / std::max(bnd, 1.0);
        return std::clamp(d / CONF_BOUNDARY_SPAN, 0.0, 1.0);
    }
    const double dahMu  = bDitMu_ * 3.0;
    const double dahVar = bDitVar_ * 3.0;
    const double margin = std::fabs(logGaussian(ms, bDitMu_, bDitVar_)
                                    - logGaussian(ms, dahMu, dahVar));
    return margin / (margin + CONF_MARGIN_SCALE);
}

// Decode-time SNR (peak/floor) mapped to 0..1 about the operator's squelch.
double CwDecoder::snrConfidence() const {
    if (noiseFloor_ <= 0.0) return 0.0;
    const double snr = peakPower_ / noiseFloor_;
    return std::clamp((snr / std::max(squelch_, 1.0) - 1.0) / CONF_SNR_SPAN, 0.0, 1.0);
}

// Blend the per-mark margin and SNR averages over the character; a nearest-code
// rescue (A1) is penalised so the UI can flag it as a guess.
double CwDecoder::charConfidence(bool rescued) const {
    if (charConfN_ <= 0) return 0.0;
    const double m = charConfSum_ / charConfN_;
    const double s = charSnrSum_  / charConfN_;
    double c = CONF_W_MARGIN * m + CONF_W_SNR * s;
    if (rescued) c *= CONF_RESCUED_SCALE;
    return std::clamp(c, 0.0, 1.0);
}

// ── one-edge lookback buffer (noise-blip / flicker merge) ──
void CwDecoder::submitEdge(bool wasMark, double durationMs) {
    const double minMarkMs  = std::max(10.0, bDitMu_ * 0.20);
    const double minSpaceMs = std::max(4.0,  bDitMu_ * 0.08);

    if (bufWasMark_) {
        const bool bm = *bufWasMark_;
        if (bm && bufDur_ < minMarkMs && !wasMark) { bufWasMark_ = false; bufDur_ += durationMs; return; }
        if (bm && bufDur_ < minMarkMs &&  wasMark) { bufWasMark_ = true;  bufDur_ += durationMs; return; }
        if (!bm && bufDur_ < minSpaceMs && wasMark){ bufWasMark_ = true;  bufDur_ += durationMs; return; }
        if (!bm && bufDur_ < minSpaceMs && !wasMark){                     bufDur_ += durationMs; return; }
        processEdge(bm, bufDur_);   // buffered edge is valid — emit it
    }
    bufWasMark_ = wasMark;
    bufDur_     = durationMs;
}

void CwDecoder::processEdge(bool wasMarkBefore, double durationMs) {
    if (durationMs < std::max(8.0, bDitMu_ * 0.15)) return;   // safety floor
    if (wasMarkBefore) {
        if (durationMs > bDitMu_ * 8.0) return;   // implausibly long — discard
        const bool isDit = bayesClassifyMark(durationMs);
        // A3: accumulate the dit/dah decision margin + decode SNR for this mark.
        charConfSum_ += markConfidence(durationMs);
        charSnrSum_  += snrConfidence();
        ++charConfN_;
        currentChar_ += isDit ? '.' : '-';
        bayesLearnMark(durationMs, isDit);
    } else {
        const Space sp = bayesClassifySpace(durationMs);
        if (sp == Space::Word) {
            flushChar();
            // A7: unify the two word-gap emitters.  The process() long-gap
            // timeout (sp > 7.5× dit) already emits the space for a real word
            // gap before the next mark arrives here; guard with the SAME
            // lastEmitted_ test so this edge path can't append a duplicate.
            // The bWordSpaceMu_ training below still runs once per gap.
            if (lastEmitted_ != '\0' && lastEmitted_ != ' ')
                appendDecoded(' ', 1.0);
        }
        else if (sp == Space::Char) { flushChar(); }
        if (bMarkN_ >= BAYES_MIN) {
            const double spAlpha = BAYES_ALPHA * 0.5;
            if (sp == Space::Elem) {
                const double v = bElemSpaceMu_ * (1.0 - spAlpha) + durationMs * spAlpha;
                bElemSpaceMu_ = std::clamp(v, bDitMu_ * 0.4, bDitMu_ * 1.2);
            } else if (sp == Space::Char) {
                const double v = bCharSpaceMu_ > 0
                    ? bCharSpaceMu_ * (1.0 - spAlpha) + durationMs * spAlpha : durationMs;
                bCharSpaceMu_ = std::clamp(v, bDitMu_ * 2.0, bDitMu_ * 8.0);
            } else {
                const double v = bWordSpaceMu_ > 0
                    ? bWordSpaceMu_ * (1.0 - spAlpha) + durationMs * spAlpha : durationMs;
                bWordSpaceMu_ = std::clamp(v, bDitMu_ * 4.0, bDitMu_ * 20.0);
            }
        }
    }
}

void CwDecoder::flushChar() {
    if (currentChar_.empty()) return;
    char   out;
    double conf;
    auto it = morseTable().find(currentChar_);
    if (it != morseTable().end()) {                 // direct hit
        out  = it->second;
        conf = charConfidence(/*rescued=*/false);
    } else if (char r = rescueNearest(currentChar_)) {  // A1: unique dist-1 rescue
        out  = r;
        conf = charConfidence(/*rescued=*/true);
    } else {                                        // honest unknown
        out  = '?';
        conf = 0.0;
    }
    appendDecoded(out, conf);
    currentChar_.clear();
    charConfSum_ = 0.0; charSnrSum_ = 0.0; charConfN_ = 0;
}

void CwDecoder::appendDecoded(char ch, double confidence) {
    lastEmitted_ = ch;
    if (onChar) onChar(ch, confidence);
}

// ── hot path ──────────────────────────────────────────────────────────────
void CwDecoder::process(const float* mono, int nframes) {
    if (nframes <= 0) return;

    // AFC accumulation — periodic Goertzel sweep about the configured tone.
    if (afcEnabled_) {
        afcBuf_.insert(afcBuf_.end(), mono, mono + nframes);
        afcCount_ += nframes;
        if (afcCount_ >= AFC_PERIOD && static_cast<int>(afcBuf_.size()) >= AFC_WIN) {
            runAfc(afcBuf_.data() + (afcBuf_.size() - AFC_WIN), AFC_WIN, toneHz_);
            afcCount_ = 0;
            if (static_cast<int>(afcBuf_.size()) > AFC_WIN * 2)
                afcBuf_.erase(afcBuf_.begin(), afcBuf_.end() - AFC_WIN);
        }
    }

    // IQ window ~0.7 ms; detection tone = AFC lock if present else configured.
    const int    iqWin   = std::clamp(static_cast<int>(std::lround(sampleRate_ / 1400.0)), 4, 48);
    const double detHz   = (afcEnabled_ && afcHz_) ? *afcHz_ : toneHz_;
    const double msPerWin= (iqWin / sampleRate_) * 1000.0;

    // Matched-filter window adapts to dit duration (tighter with DSP filter on).
    const double mfRatio = dspFilter_ ? 0.55 : 0.30;
    const int    mfWinNew= std::clamp(
        static_cast<int>(std::lround(dotEstMs_ * mfRatio / msPerWin)), 2, kMfMax);
    if (mfWinNew != mfWinLast_) {
        mfWin_ = mfWinNew; mfWinLast_ = mfWinNew;
        mfBuf_.fill(0.0); mfSum_ = 0.0; mfPos_ = 0;
    }

    // Per-block oscillator rotation (no trig in the inner loop).
    const double dphi  = 2.0 * kPi * detHz / sampleRate_;
    const double cosDp = std::cos(dphi);
    const double sinDp = std::sin(dphi);
    double oscCos = std::cos(iqPhaseAcc_);
    double oscSin = std::sin(iqPhaseAcc_);

    for (int i = 0; i < nframes; ++i) {
        const double x = mono[i];
        iqI_ += x * oscCos;
        iqQ_ += x * oscSin;
        ++iqCount_;
        const double nc = oscCos * cosDp - oscSin * sinDp;
        oscSin = oscSin * cosDp + oscCos * sinDp;
        oscCos = nc;

        if (iqCount_ < iqWin) continue;

        double mag = std::sqrt(iqI_ * iqI_ + iqQ_ * iqQ_) / iqWin;
        iqI_ = iqQ_ = 0.0; iqCount_ = 0;

        if (noiseBlanker_) {
            if (nbRunAvg_ == 0.0) nbRunAvg_ = mag;
            else nbRunAvg_ = nbRunAvg_ * (1.0 - NB_ALPHA) + mag * NB_ALPHA;
            if (nbRunAvg_ > 0.0 && mag > nbRunAvg_ * NB_RATIO) mag = nbRunAvg_ * NB_RATIO;
        }

        envVal_ = mag > envVal_
            ? envVal_ * (1.0 - ENV_ATK) + mag * ENV_ATK
            : envVal_ * (1.0 - ENV_DKY) + mag * ENV_DKY;

        // matched filter — O(1) running mean over mfWin_ windows
        mfSum_ -= mfBuf_[mfPos_];
        mfBuf_[mfPos_] = envVal_;
        mfSum_ += envVal_;
        mfPos_ = (mfPos_ + 1) % mfWin_;
        const double mfVal = mfSum_ / mfWin_;

        // adaptive noise floor — 3-mode (space/holdoff/mark) one- or two-way EMA
        if (!lastLevel_ || noiseFloor_ == 0.0) {
            const bool inHoldoff = sampleTimeMs_ < floorHoldoffEnd_;
            if (mfVal < noiseFloor_ || noiseFloor_ == 0.0) {
                noiseFloor_ = noiseFloor_ == 0.0
                    ? mfVal : noiseFloor_ * (1.0 - FLOOR_FALL) + mfVal * FLOOR_FALL;
            } else if (!inHoldoff) {
                noiseFloor_ = noiseFloor_ * (1.0 - FLOOR_RISE) + mfVal * FLOOR_RISE;
            }
        } else if (mfVal < noiseFloor_) {
            noiseFloor_ = noiseFloor_ * (1.0 - FLOOR_FALL) + mfVal * FLOOR_FALL;
        }

        // peak tracker with QSB / word-gap snap.  A5: during a no-signal gap a
        // single static crash / key-click must NOT hijack the AGC reference.
        // Gate the upward snap on the RAW per-window magnitude (which falls back
        // to noise the window after an impulse) staying above squelch for a few
        // consecutive windows — a real signal re-emerging satisfies this in
        // ~2 ms, a lone impulse cannot (the slow-decay envelope alone would, so
        // gating on it is wrong).  Steady-state (mid-character) is the else path
        // below and is unchanged.
        if (envVal_ > peakPower_) {
            const bool longSpace = !lastLevel_ && (sampleTimeMs_ - lastEdge_) > dotEstMs_ * 5.0;
            if (qsbFadeFlag_ || longSpace) {
                if (mag > noiseFloor_ * squelch_) {
                    if (++peakSnapHold_ >= PEAK_SNAP_PERSIST) {
                        peakPower_   = envVal_;
                        qsbFadeFlag_ = false;
                        peakSnapHold_= 0;
                    }
                    // else: hold this window's spike, keep the established peak.
                } else {
                    peakSnapHold_ = 0;   // impulse: raw magnitude already gone
                }
            } else {
                peakPower_   = envVal_;   // normal fast-attack on a mark
                peakSnapHold_= 0;
            }
        } else {
            peakPower_ = std::max(noiseFloor_, peakPower_ * (1.0 - PEAK_FALL));
            if (peakPower_ < noiseFloor_ * 2.0) qsbFadeFlag_ = true;
            peakSnapHold_ = 0;
        }

        // normalise + SNR gate.  A6: hysteresis — open at squelch× floor, hold
        // open until the peak fades to SNR_HYST× that (≈1.2× floor at the 1.5
        // default) so a QSB dip mid-character doesn't chop the mark.  The
        // slicer below still requires mfVal to clear threshold within
        // [floor,peak], so an open gate over noise stays a space (norm≈0) — no
        // false marks.  Inert on clean signal (peak ≫ floor latches it open).
        const double range = peakPower_ - noiseFloor_;
        if (noiseFloor_ > 0.0) {
            snrGateOpen_ = snrGateOpen_
                ? peakPower_ >= noiseFloor_ * squelch_ * SNR_HYST
                : peakPower_ >= noiseFloor_ * squelch_;
        } else {
            snrGateOpen_ = false;
        }
        const bool   snrOk = snrGateOpen_;
        const double norm  = (snrOk && range > 0.0)
            ? std::clamp((mfVal - noiseFloor_) / range, 0.0, 1.0) : 0.0;

        // proportional hysteresis slicer
        const double hystOn  = std::max(HYST_ON_MIN,  threshold_ * HYST_ON_RATIO);
        const double hystOff = std::max(HYST_OFF_MIN, threshold_ * HYST_OFF_RATIO);
        const bool isMark = lastLevel_
            ? norm >= std::max(HYST_MARK_END_FLOOR, threshold_ - hystOff)
            : norm >= std::min(0.98, threshold_ + hystOn);

        sampleTimeMs_ += msPerWin;
        const double nowMs = sampleTimeMs_;

        // edge detection + one-edge lookback
        if (isMark != lastLevel_) {
            submitEdge(lastLevel_, nowMs - lastEdge_);
            if (lastLevel_ && !isMark)
                floorHoldoffEnd_ = nowMs + dotEstMs_ * FLOOR_HOLDOFF_T;
            lastEdge_ = nowMs;
            lastLevel_ = isMark;
        }
        if (!lastLevel_) {
            const double sp = nowMs - lastEdge_;
            if (sp > dotEstMs_ * 5.0) {   // long-space timeout flush
                if (bufWasMark_) { processEdge(*bufWasMark_, bufDur_); bufWasMark_.reset(); }
                if (!currentChar_.empty()) flushChar();
            }
            if (sp > dotEstMs_ * 7.5 && lastEmitted_ != '\0' && lastEmitted_ != ' ')
                appendDecoded(' ', 1.0);
        }
    }

    iqPhaseAcc_ = std::atan2(oscSin, oscCos);
    if (iqPhaseAcc_ < 0.0) iqPhaseAcc_ += 2.0 * kPi;
}

} // namespace lyra::dsp
