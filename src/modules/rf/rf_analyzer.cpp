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

#define RF_ANALYZER_THRESHOLD_MIN -85
#define RF_ANALYZER_THRESHOLD_MAX -55
#define RF_ANALYZER_THRESHOLD_STEP 5
#define RF_ANALYZER_THRESHOLD_DEFAULT -65
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

    // Large number: "XXX.XX" (~6 chars). One step below fullscreen so the
    // button hints fit. Classic font is 6px wide per char at size 1.
    int freqSize = tftWidth / (6 * 7);
    if (freqSize < 2) freqSize = 2;
    if (freqSize > 6) freqSize = 6;

    float frozenFreq = 0.f;
    int frozenRssi = -100;
    bool exitRequested = false;
    static int thresholdDbm = RF_ANALYZER_THRESHOLD_DEFAULT;

    // Static frame, painted once. Number + dBm + bar areas repaint only.
    // Adaptive layout shrinks the digits only until number + dBm + bar +
    // the hint line all fit the panel height.
    drawMainBorderWithTitle("Freq Analyzer");
    const int yTitle = BORDER_PAD_Y + FM * LH + 2;
    const int bottomY = tftHeight - BORDER_PAD_X - 2;
    const int dbmSize = FP;
    const int dbmH = dbmSize * LH + 2;
    const int barH = 12;
    const int barW = tftWidth - 2 * BORDER_PAD_X;
    const int hintH = FP * LH + 2;
    int yFreq = yTitle;
    int freqH = 0;
    int ySub = yTitle;
    int barY = yTitle;
    int yHint = yTitle;
    while (1) {
        freqH = freqSize * 8 + 6;
        yFreq = yTitle + FP * LH + 8;
        ySub = yFreq + freqH + 4;
        barY = ySub + dbmH + 6;
        yHint = barY + barH + 6;
        if (yHint + hintH <= bottomY || freqSize <= 2) break;
        freqSize--;
    }
    tft.setTextSize(FP);
    tft.setTextColor(getColorVariation(bruceConfig.priColor), bruceConfig.bgColor);
    tft.drawCentreString("ALL RANGES", tftWidth / 2, yTitle, SMOOTH_FONT);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.drawCentreString("---.--", tftWidth / 2, yFreq, SMOOTH_FONT);
    tft.setTextColor(getColorVariation(bruceConfig.priColor), bruceConfig.bgColor);
    tft.drawCentreString("[<] Reset [SEL] THR [ESC] Back", tftWidth / 2, yHint, SMOOTH_FONT);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    // Peak-hold bar with slow decay (VU-meter style): jumps instantly to a
    // stronger sample so each remote press visibly kicks the meter, then
    // falls ~1 dB per sample so the peak stays readable instead of
    // flickering. Maps [-95,-10] dBm to [0,barW]: the CC1101 RSSI register
    // saturates near -11 dBm, and remotes touching the antenna have read
    // -15 dBm, so the top of the scale must cover point-blank signals or
    // the fill overflows the bar area. Color is dynamic: weak = red,
    // mid = yellow, strong = green (higher dBm = stronger).
    int barLevel = -95;
    auto barColor = [&](int dbm) -> uint16_t {
        if (dbm >= -45) return TFT_GREEN;
        if (dbm >= thresholdDbm) return TFT_YELLOW;
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
        if (v > -10) v = -10;
        int fill = (v + 95) * barW / 85;
        uint16_t c = barColor(barLevel);
        tft.fillRect(BORDER_PAD_X, barY, fill, barH, c);
        tft.fillRect(BORDER_PAD_X + fill, barY, barW - fill, barH, bruceConfig.bgColor);
    };
    drawBar(-95, false);

    // Repaint the whole analyzer frame (used after the threshold popup or
    // a reset, which leave foreign pixels on screen).
    auto repaint = [&]() {
        drawMainBorderWithTitle("Freq Analyzer");
        tft.setTextSize(FP);
        tft.setTextColor(getColorVariation(bruceConfig.priColor), bruceConfig.bgColor);
        tft.drawCentreString("ALL RANGES", tftWidth / 2, yTitle, SMOOTH_FONT);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        char fb[16];
        if (frozenFreq > 0) snprintf(fb, sizeof(fb), "%.2f", frozenFreq);
        else snprintf(fb, sizeof(fb), "---.--");
        tft.fillRect(BORDER_PAD_X, yFreq - 2, barW, freqH, bruceConfig.bgColor);
        tft.setTextSize(freqSize);
        tft.drawCentreString(fb, tftWidth / 2, yFreq, SMOOTH_FONT);
        tft.setTextSize(FP);
        tft.fillRect(BORDER_PAD_X, ySub - 1, barW, dbmH, bruceConfig.bgColor);
        tft.setTextColor(getColorVariation(bruceConfig.priColor), bruceConfig.bgColor);
        tft.drawCentreString("[<] Reset [SEL] THR [ESC] Back", tftWidth / 2, yHint, SMOOTH_FONT);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        drawBar(-95, false);
    };

    // NOTE: buttons are polled FIRST each pass, before the ~280 ms sweep.
    // check() suspends the input task + 10 ms debounce, so polling after a
    // long sweep made every press feel ignored until pressed twice. Raw
    // flags are only *read* inside the sweep (never consumed there).
    while (!exitRequested) {
        // --- input first: ESC / reset / threshold popup ---
        if (check(EscPress)) {
            exitRequested = true;
            break;
        }
        if (PrevPress || UpPress) {
            check(PrevPress);
            check(UpPress);
            frozenFreq = 0.f;
            frozenRssi = -100;
            barLevel = -95; // drop the meter too, else a stale peak lingers
            repaint();
            continue; // skip the sweep, measure fresh next pass
        }
        if (check(SelPress)) {
            ELECHOUSE_cc1101.setSidle();
            int idx = (thresholdDbm - RF_ANALYZER_THRESHOLD_MIN) / RF_ANALYZER_THRESHOLD_STEP;
            const int thCount =
                (RF_ANALYZER_THRESHOLD_MAX - RF_ANALYZER_THRESHOLD_MIN) / RF_ANALYZER_THRESHOLD_STEP;
            if (idx < 0) idx = 0;
            if (idx > thCount) idx = thCount;
            std::vector<Option> thOptions;
            for (int th = RF_ANALYZER_THRESHOLD_MIN; th <= RF_ANALYZER_THRESHOLD_MAX;
                 th += RF_ANALYZER_THRESHOLD_STEP) {
                int v = th;
                String label = String(v) + " dBm";
                if (v == RF_ANALYZER_THRESHOLD_DEFAULT) label += " (def)";
                thOptions.emplace_back(label.c_str(), [v]() { thresholdDbm = v; });
            }
            loopOptions(thOptions, idx);
            thOptions.clear();
            ELECHOUSE_cc1101.SetRx();
            repaint();
            continue; // sweep resumes on a clean frame
        }

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
            if (rssi >= thresholdDbm && rssi > peakRssi) {
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

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    check(EscPress); // consume any leftover press before returning to menus

    deinitRfModule();
    returnToMenu = true;
}
