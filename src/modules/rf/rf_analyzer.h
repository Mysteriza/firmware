#ifndef __RF_ANALYZER_H__
#define __RF_ANALYZER_H__

// Frequency Analyzer: sweep the selected range, show the strongest carrier
// prominently (Flipper Zero style) plus a top-N list the user can pick from.
// Picking a frequency sets it as fixed and jumps straight into Scan/Copy.
//
// Personal (my-reaper) feature: not intended for upstream PR.
void rf_analyzer();

#endif
