#include "statusline.h"
#include "../apps/apps.h"
#include "../driver/eeprom.h"
#include "../driver/si473x.h"
#include "../driver/st7565.h"
#include "../helper/bands.h"
#include "../helper/battery.h"
#include "../helper/channels.h"
#include "../helper/measurements.h"
#include "../helper/numnav.h"
#include "../scheduler.h"
#include "components.h"
#include "graphics.h"
#include <stdio.h>
#include <string.h>

static uint8_t previousBatteryLevel = 255;
static bool showBattery = true;

static uint32_t lastEepromWrite = 0;
static uint32_t lastTickerUpdate = 0;

static char statuslineText[32] = {0};
static char statuslineTicker[32] = {0};

void STATUSLINE_SetText(const char *pattern, ...) {
  char statuslineTextNew[32] = {0};
  va_list args;
  va_start(args, pattern);
  vsnprintf(statuslineTextNew, 31, pattern, args);
  va_end(args);
  if (strcmp(statuslineText, statuslineTextNew)) {
    strcpy(statuslineText, statuslineTextNew);
    gRedrawScreen = true;
  }
}

void STATUSLINE_update(void) {
  // BATTERY_UpdateBatteryInfo();
  uint8_t level = gBatteryPercent / 10;
  if (gBatteryPercent < BAT_WARN_PERCENT) {
    showBattery = !showBattery;
    gRedrawScreen = true;
  } else {
    showBattery = true;
  }
  if (previousBatteryLevel != level) {
    previousBatteryLevel = level;
    gRedrawScreen = true;
  }

  if ((bool)lastEepromWrite != gEepromWrite) {
    lastEepromWrite = gEepromWrite ? Now() : 0;
    gRedrawScreen = true;
  }
  if (lastEepromWrite && Now() - lastEepromWrite > 500) {
    lastEepromWrite = gEepromWrite = false;
    gRedrawScreen = true;
  }

  if (Now() - lastTickerUpdate > 5000) {
    statuslineTicker[0] = '\0';
  }
}

void STATUSLINE_render(void) {
  UI_ClearStatus();

  const uint8_t BASE_Y = 4;

  DrawHLine(0, 6, LCD_WIDTH, C_FILL);

  uint8_t textLeft = 0;
  if (gCurrentApp == APP_VFO1) {
    const uint8_t ax = 0, ay = 0;
    const uint8_t bx = 4, byBase = 5;
    DrawVLine(ax + 1, ay + 1, 4, C_FILL);
    DrawHLine(ax, ay, 3, C_FILL);
    uint16_t rssi = vfo->msm.rssi;
    uint8_t n =
        (rssi <= RSSI_MIN) ? 1 : (rssi >= RSSI_MAX ? 5 : (1 + (rssi - RSSI_MIN) * 4 / (RSSI_MAX - RSSI_MIN)));
    for (uint8_t i = 0; i < 5; i++) {
      uint8_t h = i + 1;
      uint8_t x = bx + i * 3;
      if (i < n)
        FillRect(x, byBase - h, 2, h, C_FILL);
    }
    /* 文字从 20 起；电池左缘=115；图标区右缘=105 */
    textLeft = 20;
  }

  if (showBattery) {
    switch (gSettings.batteryStyle) {
    case BAT_CLEAN:
      UI_Battery(previousBatteryLevel);
      break;
    case BAT_PERCENT:
      PrintSmallEx(LCD_WIDTH - 1, BASE_Y, POS_R, C_INVERT, "%u%%",
                   gBatteryPercent);
      break;
    case BAT_VOLTAGE:
      PrintSmallEx(LCD_WIDTH - 1, BASE_Y, POS_R, C_FILL, "%u.%02uV",
                   gBatteryVoltage / 100, gBatteryVoltage % 100);
      break;
    }
  }

  char icons[8] = {'\0'};
  uint8_t idx = 0;

  if (gEepromWrite) {
    icons[idx++] = SYM_EEPROM_W;
  }

  if (LOOT_Size() == LOOT_SIZE_MAX) {
    icons[idx++] = SYM_LOOT_FULL;
  }

  if (gMonitorMode) {
    icons[idx++] = SYM_MONITOR;
  }

  if (ctx->radio_type == RADIO_BK1080 || isSi4732On) {
    icons[idx++] = SYM_BROADCAST;
  }

  if (gSettings.upconverter) {
    icons[idx++] = SYM_CONVERTER;
  }

  if (gSettings.keylock) {
    icons[idx++] = SYM_LOCK;
  }

  if (gCurrentApp == APP_CH_LIST || gCurrentApp == APP_LOOT_LIST) {
    UI_Scanlists(LCD_XCENTER - 13, 0, gSettings.currentScanlist);
  }

  PrintSymbolsEx(LCD_WIDTH - 1 - 22, BASE_Y, POS_R, C_FILL, "%s", icons);

  if (gIsNumNavInput) {
    PrintSmall(textLeft, BASE_Y, "Select: %s", gNumNavInput);
  } else {
    PrintSmall(textLeft, BASE_Y, "%s",
               statuslineTicker[0] ? statuslineTicker : statuslineText);
  }
}

void STATUSLINE_RenderRadioSettings() {
  char gain[8];
  char bandwidth[8];
  char squelch_type[8];
  char squelch_value[8];
  char modulation[8];
  char step[8];

  strcpy(gain, RADIO_GetParamValueString(ctx, PARAM_GAIN));
  strcpy(bandwidth, RADIO_GetParamValueString(ctx, PARAM_BANDWIDTH));
  strcpy(squelch_type, RADIO_GetParamValueString(ctx, PARAM_SQUELCH_TYPE));
  strcpy(squelch_value, RADIO_GetParamValueString(ctx, PARAM_SQUELCH_VALUE));
  strcpy(modulation, RADIO_GetParamValueString(ctx, PARAM_MODULATION));
  strcpy(step, RADIO_GetParamValueString(ctx, PARAM_STEP));

  if (gCurrentApp == APP_VFO1) {
    uint32_t rxF = RADIO_GetParam(ctx, PARAM_FREQUENCY);
    uint32_t txF = RADIO_GetParam(ctx, PARAM_TX_FREQUENCY_FACT);
    bool direct = (rxF == txF);
    if (bandwidth[0] == '\0') {
      /* WFM 等无带宽时不显示带宽 */
      STATUSLINE_SetText("%s %s %s %s",
                        direct ? "|->| " : "", squelch_type, squelch_value,
                        step);
    } else {
      STATUSLINE_SetText("%s %s %s %s %s",
                        direct ? "|->| " : "", bandwidth,
                        squelch_type, squelch_value, step);
    }
  } else {
    if (bandwidth[0] == '\0') {
      STATUSLINE_SetText("%s %s %s %s", gain, squelch_type, squelch_value,
                         modulation);
    } else {
      STATUSLINE_SetText("%s %s %s %s %s", gain, bandwidth, squelch_type,
                        squelch_value, modulation);
    }
  }
}
