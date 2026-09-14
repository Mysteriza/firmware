#include "rf_analyzer.h"
#include "core/display.h"
#include "protocols/rf_config.h" // RF_DBG
#include "rf_utils.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <globals.h>

// Frequency Analyzer: read-only frequency meter (Flipper style, simplified).
//
// Always sweeps ALL ranges (that is the point: find where the frequency is).
// Shows one huge frozen number: the strongest carrier found (coarse list
// sweep + fine +-0.30 MHz / 50 kHz refine around the winner). The number
// holds until a strictly stronger signal appears (+3 dB hysteresis), so it
// never jitters. A live bar under it tracks the CURRENT sweep RSSI every
// sample, so each remote press visibly kicks the meter. Nothing is stored,
// nothing is transmitted, the user config is untouched: ESC leaves.
//
// Personal (my-reaper) feature: not intended for upstream PR.

#define RF_ANALYZER_THRESHOLD_DBM -65
#define RF_ANALYZER_HYST_DB 3
#define RF_ANALYZER_FINE_SPAN_MHZ 0.30f
#define RF_ANALYZER_FINE_STEP_MHZ 0.05f
// Fast settle: CC1101 calibrates in ~0.7 ms, AGC needs a couple ms. 2 ms
// keeps a full 57-freq pass around ~200 ms so 1-second remote spam always
// lands inside a pass.
#define RF_ANALYZER_SETTLE_MS 2

// Single RSSI sample on an arbitrary frequency.
static int rf_analyzer_sample(float freq) {
    rf_cc1101_hop(freq);
    tft.drawPixel(0, 0, 0); // shared-SPI with TFT workaround
    vTaskDelay(RF_ANALYZER_SETTLE_MS / portTICK_PERIOD_MS);
    return ELECHOUSE_cc1101.getRssi();
}

void rf_analyzer() {
    if (bruceConfigPins.rfModule != CC1101_SPI_MODULE) {
        displayError("only for CC1101 module", true);
        return;
    }
    if (!initRfModule("rx", bruceConfigPins.rfFreq)) {
        displayError("CC1101 not found!", true);
        return;
    }

    // Huge number: "XXX.XX" (~6 chars) fills the width.
    // Classic font is 6px wide per char at size 1.
    int freqSize = tftWidth / (6 * 6);
    if (freqSize < 2) freqSize = 2;
    if (freqSize > 7) freqSize = 7;

    float frozenFreq = 0.f;
    int frozenRssi = -100;
    bool exitRequested = false;

    // Static frame, painted once. Number + dBm + bar areas repaint only.
    // No bottom hint: ESC still leaves (handled in the loop), so the freed
    // space goes to the frequency digits. Adaptive layout shrinks the
    // digits only until number + dBm + bar fit the panel height.
    drawMainBorderWithTitle("Freq Analyzer");
    const int yTitle = BORDER_PAD_Y + FM * LH + 2;
    const int bottomY = tftHeight - BORDER_PAD_X - 2;
    const int dbmSize = FP + 1;
    const int dbmH = dbmSize * LH + 2;
    const int barH = 14;
    const int barW = tftWidth - 2 * BORDER_PAD_X;
    int yFreq = yTitle;
    int freqH = 0;
    int ySub = yTitle;
    int barY = yTitle;
    while (1) {
        freqH = freqSize * 8 + 6;
        yFreq = yTitle + FP * LH + 10;
        ySub = yFreq + freqH + 6;
        barY = ySub + dbmH + 8;
        if (barY + barH <= bottomY || freqSize <= 2) break;
        freqSize--;
    }
    tft.setTextSize(FP);
    tft.setTextColor(getColorVariation(bruceConfig.priColor), bruceConfig.bgColor);
    tft.drawCentreString("ALL RANGES", tftWidth / 2, yTitle, SMOOTH_FONT);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawCentreString("---.--", tftWidth / 2, yFreq, SMOOTH_FONT);

    // Peak-hold bar with slow decay (VU-meter style): jumps instantly to a
    // stronger sample so each remote press visibly kicks the meter, then
    // falls ~1 dB per sample so the peak stays readable instead of
    // flickering. Maps [-95,-20] dBm to [0,barW]. Color is dynamic:
    // weak = red, mid = yellow, strong = green (higher dBm = stronger).
    int barLevel = -95;
    auto barColor = [&](int dbm) -> uint16_t {
        if (dbm >= -45) return TFT_GREEN;
        if (dbm >= RF_ANALYZER_THRESHOLD_DBM) return TFT_YELLOW;
        return TFT_RED;
    };
    auto drawBar = [&](int rssi, bool decay) {
        if (decay) {
            if (rssi > barLevel) barLevel = rssi;
            else if (barLevel > -95) barLevel -= 1;
        } else {
            barLevel = rssi;
        }
        int v = barLevel;
        if (v < -95) v = -95;
        if (v > -20) v = -20;
        int fill = (v + 95) * barW / 75;
        uint16_t c = barColor(barLevel);
        tft.fillRect(BORDER_PAD_X, barY, fill, barH, c);
        tft.fillRect(BORDER_PAD_X + fill, barY, barW - fill, barH, bruceConfig.bgColor);
    };
    drawBar(-95, false);

    // NOTE: never call check(EscPress) inside the sweep: check() consumes the
    // flag, so an ESC pressed mid-sweep would be eaten and the exit below
    // would miss it. Read the raw flag here, consume it once at the bottom.
    while (!exitRequested) {
        // --- stage 1: coarse sweep over ALL ranges, keep this pass' peak ---
        float peakFreq = 0.f;
        int peakRssi = -100;
        for (int i = 0; i <= 56; i++) {
            if (EscPress) {
                exitRequested = true;
                break;
            }
            float f = subghz_frequency_list[i];
            int rssi = rf_analyzer_sample(f);
            drawBar(rssi, true); // peak-hold: press kicks, then decays
            if (rssi >= RF_ANALYZER_THRESHOLD_DBM && rssi > peakRssi) {
                peakRssi = rssi;
                peakFreq = f;
            }
        }

        // --- stage 2: fine refine around the peak ---
        if (!exitRequested && peakFreq > 0) {
            for (float f = peakFreq - RF_ANALYZER_FINE_SPAN_MHZ;
                 f <= peakFreq + RF_ANALYZER_FINE_SPAN_MHZ + 0.001f;
                 f += RF_ANALYZER_FINE_STEP_MHZ) {
                if (f < 280.f || f > 928.f) continue;
                if (EscPress) {
                    exitRequested = true;
                    break;
                }
                int rssi = rf_analyzer_sample(f);
                drawBar(rssi, true);
                if (rssi > peakRssi) {
                    peakRssi = rssi;
                    peakFreq = f;
                }
            }
            RF_DBG("analyzer peak: freq=%.2f rssi=%d", peakFreq, peakRssi);
        }

        // --- frequency: frozen with hysteresis (stable, never jitters) ---
        if (!exitRequested && peakFreq > 0 &&
            (frozenFreq == 0.f || peakRssi >= frozenRssi + RF_ANALYZER_HYST_DB)) {
            frozenFreq = peakFreq;
            frozenRssi = peakRssi;

            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", frozenFreq);
            tft.fillRect(BORDER_PAD_X, yFreq - 2, barW, freqH, bruceConfig.bgColor);
            tft.setTextSize(freqSize);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawCentreString(buf, tftWidth / 2, yFreq, SMOOTH_FONT);
            tft.setTextSize(FP);
        }

        // --- dBm: always live, follows this pass' peak (no hysteresis) ---
        // Same remote at a different distance reads a different strength,
        // so this number moves even while the frozen frequency stays put.
        // Slightly enlarged with the bar color so strength reads at a glance.
        if (!exitRequested && peakFreq > 0) {
            tft.fillRect(BORDER_PAD_X, ySub - 1, barW, dbmH, bruceConfig.bgColor);
            char sub[32];
            snprintf(sub, sizeof(sub), "%d dBm", peakRssi);
            tft.setTextSize(dbmSize);
            tft.setTextColor(barColor(peakRssi), bruceConfig.bgColor);
            tft.drawCentreString(sub, tftWidth / 2, ySub, SMOOTH_FONT);
            tft.setTextSize(FP);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        }

        // Single consume point for ESC: guaranteed to fire at most one pass
        // after the press, so back always works.
        if (check(EscPress)) exitRequested = true;
        // Swallow SEL (no action) so a stray press doesn't leak into menus.
        check(SelPress);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    check(EscPress); // consume any leftover press before returning to menus

    deinitRfModule();
    returnToMenu = true;
}
