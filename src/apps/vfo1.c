#include "vfo1.h"
#include "../dcs.h"
#include "../driver/bk4819.h"
#include "../driver/gpio.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/bands.h"
#include "../helper/channels.h"
#include "../helper/measurements.h"
#include "../helper/numnav.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../inc/dp32g030/gpio.h"
#include "../radio.h"
#include "../scheduler.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
#include "../ui/spectrum.h"
#include "../ui/statusline.h"
#include "apps.h"
#include "chcfg.h"
#include "chlist.h"
#include "finput.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void updateBand(void) {
  uint32_t f = RADIO_GetParam(ctx, PARAM_FREQUENCY);
  if (!BANDS_InRange(f, gCurrentBand) ||
      gCurrentBand.meta.type == TYPE_BAND_DETACHED) {
    BANDS_SelectByFrequency(f, ctx->fixed_bounds);
  }
}

static void setChannel(uint16_t v) {
  RADIO_LoadChannelToVFO(gRadioState, RADIO_GetCurrentVFONumber(gRadioState),
                         v);
}

/* 用户输入为 1-based 信道号（1=第1信道），转为 0-based index 后加载 */
static void setChannelFromUserInput(uint16_t oneBased) {
  setChannel(oneBased - 1);
}

static void tuneTo(uint32_t f, uint32_t _) {
  (void)_;
  RADIO_SetParam(ctx, PARAM_FREQUENCY, f, true);
  RADIO_ApplySettings(ctx);
  updateBand();
}

void VFO1_init(void) {
  gLastActiveLoot = NULL;
  CHANNELS_LoadScanlist(TYPE_FILTER_CH, gSettings.currentScanlist);
  if (vfo->mode == MODE_CHANNEL) {
    setChannel(vfo->channel_index);
  }
  updateBand();

  SCAN_SetMode(SCAN_MODE_SINGLE);
  // SCAN_Init(false);
}

void VFO1_update(void) {}

static bool handleNumNav(KEY_Code_t key) {
  if (gIsNumNavInput) {
    NUMNAV_Input(key);
    return true;
  }

  if (key <= KEY_9) {
    /* 界面显示为 1-based（MR 001/002），输入也用 1-based，确认时再转 0-based index */
    NUMNAV_Init(vfo->channel_index + 1, 1, CHANNELS_GetCountMax());
    gNumNavCallback = setChannelFromUserInput;
    NUMNAV_Input(key); /* 信道模式：首位数字键立即送入 NumNav */
    return true;
  }

  return false;
}

static bool handleFrequencyChange(KEY_Code_t key) {
  if (key != KEY_UP && key != KEY_DOWN)
    return false;

  if (vfo->mode == MODE_CHANNEL) {
    CHANNELS_Next(key == KEY_UP);
  } else {
    RADIO_IncDecParam(ctx, PARAM_FREQUENCY, key == KEY_UP, true);
  }
  updateBand();
  return true;
}

static bool handleSSBFineTune(KEY_Code_t key) {
  if (ctx->radio_type != RADIO_SI4732 || !RADIO_IsSSB(ctx))
    return false;
  if (key != KEY_SIDE1 && key != KEY_SIDE2)
    return false;

  RADIO_AdjustParam(ctx, PARAM_FREQUENCY, key == KEY_SIDE1 ? 5 : -5, true);
  // TODO: SAVE
  return true;
}

static bool handleLongPress(KEY_Code_t key) {
  uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);

  switch (key) {
  case KEY_1:
    gChListFilter = TYPE_FILTER_BAND;
    APPS_run(APP_CH_LIST);
    return true;

  case KEY_2:
    if (gCurrentApp == APP_VFO1) {
      gSettings.iAmPro = !gSettings.iAmPro;
      SETTINGS_Save();
      return true;
    }
    return false;

  case KEY_3:
    RADIO_SaveCurrentVFO(gRadioState);
    RADIO_ToggleVFOMode(gRadioState, vfoN);
    updateBand();
    return true;

  case KEY_4:
    gShowAllRSSI = !gShowAllRSSI;
    return true;

  case KEY_5:
    RADIO_ToggleMultiwatch(gRadioState, !gRadioState->multiwatch_enabled);
    return true;

  case KEY_6:
    RADIO_IncDecParam(ctx, PARAM_POWER, true, true);
    return true;

  case KEY_7:
    RADIO_IncDecParam(ctx, PARAM_STEP, true, true);
    return true;

  case KEY_8:
    RADIO_IncDecParam(ctx, PARAM_TX_OFFSET, true, true);
    return true;

  case KEY_0:
    RADIO_IncDecParam(ctx, PARAM_MODULATION, true, true);
    return true;

  case KEY_SIDE1:
  case KEY_SIDE2:
    SP_NextGraphUnit(key == KEY_SIDE1);
    return true;

  default:
    return false;
  }
}

static bool handleRelease(KEY_Code_t key, Key_State_t state) {
  uint8_t vfoN = RADIO_GetCurrentVFONumber(gRadioState);

  switch (key) {
  case KEY_0:
  case KEY_1:
  case KEY_2:
  case KEY_3:
  case KEY_4:
  case KEY_5:
  case KEY_6:
  case KEY_7:
  case KEY_8:
  case KEY_9:
    /* 频率模式下数字键进入频率输入；信道模式下由 handleNumNav 选信道，不进入频率输入 */
    if (vfo->mode != MODE_VFO)
      return false;
    gFInputCallback = tuneTo;
    FINPUT_setup(0, BK4819_F_MAX, UNIT_MHZ, false);
    APPS_run(APP_FINPUT);
    APPS_key(key, state);
    return true;

  case KEY_F:
    RADIO_SaveVFOToStorage(gRadioState, vfoN, &gChEd);
    gChNum = -1;
    APPS_run(APP_CH_CFG);
    return true;

  case KEY_STAR:
    APPS_run(APP_LOOT_LIST);
    return true;

  case KEY_SIDE1:
    gMonitorMode = !gMonitorMode;
    return true;

  case KEY_SIDE2:
    GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
    return true;

  case KEY_EXIT:
    if (gMonitorMode) {
      gMonitorMode = false;
      return true;
    }
    APPS_exit();
    return true;

  default:
    return false;
  }
}

bool VFO1_key(KEY_Code_t key, Key_State_t state) {
  // return false;
  if (REGSMENU_Key(key, state)) {
    return true;
  }

  // 信道模式下：未输入时首位数字键进入 NumNav；已输入时所有键交给 handleNumNav 持续输入信道号
  if (state == KEY_RELEASED && vfo->mode == MODE_CHANNEL) {
    if (handleNumNav(key)) {
      return true;
    }
  }

  // PTT
  if (key == KEY_PTT && !gIsNumNavInput) {
    RADIO_ToggleTX(ctx, state != KEY_RELEASED);
    return true;
  }

  // Обработка нажатий и удержаний
  if (state == KEY_RELEASED || state == KEY_LONG_PRESSED_CONT) {
    if (handleFrequencyChange(key))
      return true;
    if (handleSSBFineTune(key))
      return true;
  }

  // Длинные нажатия
  if (state == KEY_LONG_PRESSED) {
    return handleLongPress(key);
  }

  // Отпускания
  if (state == KEY_RELEASED) {
    return handleRelease(key, state);
  }

  return false;
}

/* 中部矩形：整体上移 4 像素；其下为信息栏两行（计时、亚音），再下为底部菜单 */
#define RECT_TOP      10
#define RECT_BOTTOM   40
#define RECT_BAR_W    6   /* 左侧长条宽度（有/无信号一致） */
#define RECT_CONTENT_X (RECT_BAR_W + 2)
#define BOTTOM_GAP    5
#define INFO_ROW1_Y   46   /* 计时（上移1px） */
#define INFO_ROW2_Y   52   /* 亚音（下移1px） */
#define BOTTOM_BOX_TOP  55   /* 向上增高 2px，与 INFO 栏衔接 */
#define BOTTOM_BOX_H    10

/* 中间框与底部菜单之间的信息栏：第一行计时，第二行亚音（有则显示 R/T） */
static void renderInfoRows(void) {
  uint32_t sec = GetUptimeSec();
  PrintSmallEx(LCD_WIDTH - 1, INFO_ROW1_Y, POS_R, C_FILL, "%u:%02u", sec / 60, sec % 60);

  bool hasRx = (ctx->rx_code.type != CODE_TYPE_OFF);
  bool hasTx = (ctx->tx_state.tx_code.type != CODE_TYPE_OFF);
  if (hasRx || hasTx) {
    char toneBuf[24];
    char rxStr[16];
    char txStr[16];
    strncpy(rxStr, RADIO_GetParamValueString(ctx, PARAM_RX_CODE),
            sizeof(rxStr) - 1);
    rxStr[sizeof(rxStr) - 1] = '\0';
    strncpy(txStr, RADIO_GetParamValueString(ctx, PARAM_TX_CODE),
            sizeof(txStr) - 1);
    txStr[sizeof(txStr) - 1] = '\0';
    if (hasRx && hasTx)
      snprintf(toneBuf, sizeof(toneBuf), "R%s T%s", rxStr, txStr);
    else if (hasRx)
      snprintf(toneBuf, sizeof(toneBuf), "R%s", rxStr);
    else
      snprintf(toneBuf, sizeof(toneBuf), "T%s", txStr);
    PrintSmallEx(LCD_WIDTH - 1, INFO_ROW2_Y, POS_R, C_FILL, "%s", toneBuf);
  }
}

static void renderCenterBlock(uint32_t f) {
  const uint8_t rectH = RECT_BOTTOM - RECT_TOP + 1;
  const uint8_t rectR = LCD_WIDTH - RECT_BAR_W; /* 框右边界（不含条） */
  /* 左侧长条：有信号时空心，无信号时实心 */
  if (vfo->msm.open) {
    DrawRect(0, RECT_TOP, RECT_BAR_W, rectH, C_FILL);
    /* 空心时右侧框与长条共用一条边线，中间只留 1 像素线 */
    DrawRect(RECT_BAR_W - 1, RECT_TOP, LCD_WIDTH - (RECT_BAR_W - 1), rectH, C_FILL);
  } else {
    FillRect(0, RECT_TOP, RECT_BAR_W, rectH, C_FILL);
    DrawRect(RECT_BAR_W, RECT_TOP, LCD_WIDTH - RECT_BAR_W, rectH, C_FILL);
  }

  /* 框内右上角：平时显示信道号；接收信号时显示 dBm（互斥） */
  FillRect(rectR - 58, RECT_TOP + 1, 58, 8, C_CLEAR);
  if (vfo->msm.open && vfo->msm.rssi) {
    int16_t dBm = Rssi2DBm(vfo->msm.rssi);
    if (ctx->radio_type == RADIO_BK4819)
      dBm += (int16_t)BK4819_GetAttenuation();
    PrintSmallEx(rectR + 2, RECT_TOP + 6, POS_R, C_FILL, "%+d dBm", dBm);
  } else if (vfo->mode == MODE_CHANNEL) {
    PrintSmallEx(rectR + 2, RECT_TOP + 6, POS_R, C_FILL, "%03u",
                 vfo->channel_index + 1);
  }

  /* 第一行：信道名或 VFO 标识（不反显）；下移 3px */
  const uint8_t line1Y = RECT_TOP + 10;
  const uint8_t boxX = RECT_CONTENT_X + 2;
  if (vfo->mode == MODE_CHANNEL) {
    PrintMediumEx(boxX, line1Y, POS_L, C_FILL, "%s", ctx->name);
  } else {
    PrintMediumEx(boxX, line1Y, POS_L, C_FILL, "VFO");
  }
  /* 第二行：大号频率；下移 2px */
  const uint8_t freqY = RECT_TOP + 24;
  /* Motorola R7 主界面：频率数字用常规（不倾斜）并略小一号 */
  PrintBigDigitsEx(boxX, freqY, POS_L, C_FILL,
                       "%4u.%03u", f / MHZ, f / 100 % 1000);
  PrintMediumEx(boxX + 66, freqY - 4, POS_L, C_FILL, "%02u", f % 100);
}

/* 底部两个实心方框：向上增高 2 像素；框内 Menu/调制 文字上移 1 像素 */
static void renderBottomBoxes(void) {
  const uint8_t y = BOTTOM_BOX_TOP;
  const uint8_t h = BOTTOM_BOX_H;
  const uint8_t contentH = h > 2 ? h - 2 : h;
  const uint8_t textY = y + 3 + contentH / 2;  /* 按钮内字下移 1px，按钮高度不变 */
  const uint8_t leftW = 63;
  const uint8_t rightX = 64;
  const uint8_t rightW = 64;
  FillRect(0, y, leftW, h, C_FILL);
  FillRect(rightX, y, rightW, h, C_FILL);
  DrawVLine(63, y, h, C_CLEAR);
  PrintMediumEx(leftW / 2, textY, POS_C, C_INVERT, "Menu");
  PrintMediumEx(rightX + rightW / 2, textY, POS_C, C_INVERT, "%s",
               RADIO_GetParamValueString(ctx, PARAM_MODULATION));
}

static void renderStatusLine(void) {
  if (gIsNumNavInput) {
    return;
  }

  if (!gSettings.mWatch || vfo->is_open) {
    STATUSLINE_RenderRadioSettings();
  } else {
    STATUSLINE_SetText("Radio: %s",
                       RADIO_GetParamValueString(ctx, PARAM_RADIO));
  }
}

void VFO1_render(void) {
  renderStatusLine();

  uint32_t f = RADIO_GetParam(
      ctx, ctx->tx_state.is_active ? PARAM_TX_FREQUENCY_FACT : PARAM_FREQUENCY);

  if (gMonitorMode) {
    SPECTRUM_Y = RECT_TOP + 2;
    SPECTRUM_H = RECT_BOTTOM - RECT_TOP - 2;
    UI_RSSIBar(RECT_BOTTOM - 8);
  } else {
    renderCenterBlock(f);
  }
  renderInfoRows();
  renderBottomBoxes();

  REGSMENU_Draw();
}
