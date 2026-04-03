#include "statusline.h"
#include "../apps/apps.h"
#include "../driver/eeprom.h"
#include "../driver/si473x.h"
#include "../driver/st7565.h"
#include "../helper/bands.h"
#include "../helper/channels.h"
#include "../helper/measurements.h"
#include "../helper/numnav.h"
#include "../scheduler.h"
#include "components.h"
#include "graphics.h"
#include <stdio.h>
#include <string.h>

static uint32_t lastEepromWrite = 0;
static uint32_t lastTickerUpdate = 0;

static char statuslineText[32] = {0};
static char statuslineTicker[32] = {0};
static bool showDirectIcon = false; /* 直频时用图形画 |->|，避免字体竖线断成两段 */

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
    /* 文字从 20 起；功率在直频符号右侧绘制 */
    textLeft = 20;
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

  /* 仅 vfo app 主界面：右侧图标等间距重排；其他页面保持原逻辑 */
  if (gCurrentApp == APP_VFO1) {
#define ICON_SLOT_W 10
    int16_t x = (int16_t)(LCD_WIDTH - 1 - 22);
    for (int8_t i = (int8_t)idx - 1; i >= 0; i--) {
      char slot[2] = {icons[i], '\0'};
      PrintSymbolsEx((uint8_t)x, BASE_Y, POS_R, C_FILL, "%s", slot);
      x -= ICON_SLOT_W;
    }
  } else {
    PrintSymbolsEx(LCD_WIDTH - 1 - 22, BASE_Y, POS_R, C_FILL, "%s", icons);
  }

  if (gIsNumNavInput) {
    PrintSmall(textLeft, BASE_Y, "Select: %s", gNumNavInput);
  } else {
    static const char *const powerStr[] = {"UL", "L", "M", "H"};
    uint8_t pw = (uint8_t)ctx->power;
    if (pw > 3)
      pw = 0;
    uint16_t powerW = Graphics_GetSmallTextWidth(powerStr[pw]);
#define STATUS_GAP 2 /* 中间区域各元素统一间距 */

    if (gCurrentApp == APP_VFO1 && showDirectIcon) {
      /* 直频 |->| → [GAP] → 功率 → [紧贴] → 状态文字；两条竖线下方减少 1px */
      const uint8_t lineH = 5;
      const uint8_t lineY = BASE_Y - 4; /* 上移 4 像素，不越过横线 */
      const uint8_t iconX = textLeft;
      uint16_t arrowW = Graphics_GetSmallTextWidth("->");
      DrawVLine(iconX, lineY, lineH, C_FILL);
      PrintSmall(iconX + 2, BASE_Y, "->");
      DrawVLine(iconX + 2 + arrowW + 1, lineY, lineH, C_FILL);
      uint8_t directW = 2 + arrowW + 1 + 0; /* 直频图标总宽，右侧不留空 */
      uint8_t powerX = iconX + directW + 1 + 2;  /* 与图标间隔 1px，再右移 2px 避免与直频重合 */
      PrintSmall(powerX, BASE_Y, "%s", powerStr[pw]);
      PrintSmall(powerX + powerW - 2, BASE_Y, "%s",  /* 功率右侧整体左移 2px */
                 statuslineTicker[0] ? statuslineTicker : statuslineText);
    } else if (gCurrentApp == APP_VFO1) {
      /* 无直频时整体前移：功率从 textLeft 起 → [GAP] → 状态文字 */
      PrintSmall(textLeft, BASE_Y, "%s", powerStr[pw]);
      PrintSmall(textLeft + powerW + STATUS_GAP, BASE_Y, "%s",
                 statuslineTicker[0] ? statuslineTicker : statuslineText);
    } else {
      PrintSmall(textLeft, BASE_Y, "%s",
                 statuslineTicker[0] ? statuslineTicker : statuslineText);
    }
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
    showDirectIcon = direct;
    if (bandwidth[0] == '\0') {
      /* WFM 等无带宽时不显示带宽；直频时不再在字符串里写 "|->| "，由 render 用图形画竖线 */
      STATUSLINE_SetText("%s %s %s %s",
                        direct ? " " : "", squelch_type, squelch_value,
                        step);
    } else {
      STATUSLINE_SetText("%s %s %s %s %s",
                        direct ? " " : "", bandwidth,
                        squelch_type, squelch_value, step);
    }
  } else {
    showDirectIcon = false;
    if (bandwidth[0] == '\0') {
      STATUSLINE_SetText("%s %s %s %s", gain, squelch_type, squelch_value,
                         modulation);
    } else {
      STATUSLINE_SetText("%s %s %s %s %s", gain, bandwidth, squelch_type,
                        squelch_value, modulation);
    }
  }
}
