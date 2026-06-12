// Lyra-cpp — NetworkProto1.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/networkproto1.c (PARTIAL —
//   sendProtocol1Samples only, networkproto1.c:1204-1267; the C&C
//   writer WriteMainLoop_HL2 equivalent lives at
//   wire/FrameComposer.cpp per the #121/#122 monolithic fold)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015-2016 Doug Wigley (W5WC)
// License: GNU General Public License v2 or later
//
// ============================================================
// DIRECT PORT 2026-06-12 (P4.a — DORMANT until P4.b)
// ============================================================
//
// The EP2 writer thread: waits for BOTH outbound buffers (LR from
// the RX-audio OutBound(0) chain, IQ from the TX OutBound(1)
// chain), applies the !XmitBit zero posture + the optional L/R
// swap, quantizes both streams (16-bit BE) with the HL2 CW
// state-bit overlay, and hands the packed buffer to the per-family
// main loop.  DORMANT at P4.a: nothing calls _beginthreadex on it
// until P4.b's open() (the reference start site is StartAudio,
// netInterface.c:66, paired with the semaphore-quartet creation at
// :68-71).
//
// Only Lyra-cpp packaging differences (NOT code changes), each
// marked inline:
//   * `lyra::wire` namespace (reference is global C).
//   * `HPSDRModel == HPSDRModel_HERMESLITE` ->
//     `hpsdrModel == HPSDRModel::HERMESLITE` (the documented
//     RadioNet.h enum-class mapping).
//   * `WriteMainLoop_HL2(prn->OutBufp)` ->
//     `write_main_loop_hl2(prn->OutBufp)` — Lyra's monolithic
//     WriteMainLoop_HL2 equivalent (wire/FrameComposer.cpp, the
//     #121/#122 fold; byte-shape verified there).
//   * The generic `WriteMainLoop(prn->OutBufp)` (non-HL2 / ANAN
//     P1) is carried as DEFERRED reference text — no non-HL2
//     hardware to bench a port against (same posture as the
//     Ep2SendThread else-branch it supersedes).
//   * `(AVRT_PRIORITY) 2` cast — C++ enum strictness (the
//     CmBuffs.cpp / AAMix.cpp / ObBuffs.cpp precedent).
//
// See NOTICE.md and CREDITS.md (repo root) for full attribution.

#include "wire/RadioNet.h"       // prn + XmitBit + io_keep_running + hpsdrModel
#include "wire/CMaster.h"        // pcm (the eer overwrite gate reads xmtr[0].peer->run)
#include "wire/FrameComposer.h"  // write_main_loop_hl2 (the WriteMainLoop_HL2 equivalent)

namespace lyra::wire {

// Reference networkproto1.c:1204-1267 (verbatim):

#pragma warning(suppress: 4100)  // 'n' unreferenced — verbatim reference signature (C source; MSVC C++ warns)
DWORD WINAPI sendProtocol1Samples(LPVOID n)
{
	DWORD taskIndex = 0;
	HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
	if (hTask != 0) AvSetMmThreadPriority(hTask, (AVRT_PRIORITY) 2);  // verbatim `2`; cast = C++ enum strictness
	else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	int i, j, k;
	short temp;
	double swap;
	double *pbuffs [2];
	pbuffs[0] = prn->outLRbufp;
	pbuffs[1] = prn->outIQbufp;

	while (io_keep_running != 0)
	{
		WaitForMultipleObjects(2, prn->hsendEventHandles, TRUE, INFINITE);
		// if ((nddc == 2) || (nddc == 4))
		if (pcm->xmtr[0].peer->run && XmitBit)
		{
			// if eer/etr mode and transmitting, overwrite LR data with EER data
			memcpy(prn->outLRbufp, prn->outIQbufp + 256, sizeof(complex) * 126);
		}
		if (!XmitBit) memset(prn->outIQbufp, 0, sizeof(complex) * 126);
		// WriteAudio (30.0, 48000, 126, prn->outIQbufp, 3);
		// WriteAudio (60.0, 48000, 126, prn->outLRbufp, 3);

		if (prn->swap_audio_channels)				// To cater for different firmware at the hardware, allow control of audio channels swapping
		{
			for (i = 0; i < 4 * 63; i += 2)			// swap L & R audio; firmware bug fix
			{
				swap = pbuffs[0][i + 0];
				pbuffs[0][i + 0] = pbuffs[0][i + 1];
				pbuffs[0][i + 1] = swap;
			}
		}

		for (i = 0; i < 2 * 63; i++)				// for each sample from both sets, 8 bytes per
			for (j = 0; j < 2; j++)					// for a sample from each set, 4 bytes per
				for (k = 0; k < 2; k++)				// for each component of the sample, 2 per
				{
					temp = pbuffs[j][i * 2 + k] >= 0.0 ? (short)floor(pbuffs[j][i * 2 + k] * 32767.0 + 0.5) :
						(short)ceil(pbuffs[j][i * 2 + k] * 32767.0 - 0.5);
					if (prn->cw.cw_enable && j == 1)
						if (hpsdrModel == HPSDRModel::HERMESLITE)  // reference: `HPSDRModel == HPSDRModel_HERMESLITE` — documented enum-class mapping
							temp = (prn->tx[0].cwx_ptt << 3 |	// MI0BOT: Bit 3 in HL2 is used to signal PTT for CWX
						    		prn->tx[0].dot << 2 |
									prn->tx[0].dash << 1 |
						    		prn->tx[0].cwx) & 0b00001111;
						else
							temp = (prn->tx[0].dot << 2 |
									prn->tx[0].dash << 1 |
						    		prn->tx[0].cwx) & 0b00000111;
					prn->OutBufp[8 * i + 4 * j + 2 * k + 0] = (char)((temp >> 8) & 0xff);
					prn->OutBufp[8 * i + 4 * j + 2 * k + 1] = (char)(temp & 0xff);
				}

		if (hpsdrModel == HPSDRModel::HERMESLITE)	// MI0BOT: Different write loop for HL2
			write_main_loop_hl2(prn->OutBufp);      // reference: `WriteMainLoop_HL2(prn->OutBufp);` — the FrameComposer monolithic equivalent
		else
		{
			// DEFERRED [non-HL2 / ANAN P1 — no hardware to bench] —
			// reference networkproto1.c:1264:
			//   WriteMainLoop(prn->OutBufp);
		}
	}
	return 0;
}

}  // namespace lyra::wire
