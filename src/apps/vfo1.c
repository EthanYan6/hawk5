#include "vfo1.h"
#include "../dcs.h"
#include "../driver/bk4819.h"
#include "../driver/gpio.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include "../helper/bands.h"
#include "../helper/battery.h"
#include "../helper/channels.h"
#include "../helper/measurements.h"
#include "../helper/numnav.h"
#include "../helper/regs-menu.h"
#include "../helper/scan.h"
#include "../inc/dp32g030/gpio.h"
#include "../misc.h"
#include "../radio.h"
#include "../scheduler.h"
#include "../settings.h"
#include "../ui/components.h"
#include "../ui/graphics.h"
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
  /* 与上下键共用 CHANNELS_Next 的扫描列表游标，需与当前信道对齐 */
  CHANNELS_SetScanlistIndexFromRadio();
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

/* cos/sin*127，偶数度 0..180 步进 2°，索引 ang>>1；表针奇数度在相邻两点间插值 */
static const int8_t SMETER_ICOS91[91] = {
    127, 127, 127, 126, 126, 125, 124, 123, 122, 121, 119, 118, 116, 114, 112,
    110, 108, 105, 103, 100, 97, 94, 91, 88, 85, 82, 78, 75, 71, 67,
    64, 60, 56, 52, 48, 43, 39, 35, 31, 26, 22, 18, 13, 9, 4,
    0, -4, -9, -13, -18, -22, -26, -31, -35, -39, -43, -48, -52, -56, -60,
    -63, -67, -71, -75, -78, -82, -85, -88, -91, -94, -97, -100, -103, -105, -108,
    -110, -112, -114, -116, -118, -119, -121, -122, -123, -124, -125, -126, -126, -127, -127,
    -127
};
static const int8_t SMETER_ISIN91[91] = {
    0, 4, 9, 13, 18, 22, 26, 31, 35, 39, 43, 48, 52, 56, 60,
    63, 67, 71, 75, 78, 82, 85, 88, 91, 94, 97, 100, 103, 105, 108,
    110, 112, 114, 116, 118, 119, 121, 122, 123, 124, 125, 126, 126, 127, 127,
    127, 127, 127, 126, 126, 125, 124, 123, 122, 121, 119, 118, 116, 114, 112,
    110, 108, 105, 103, 100, 97, 94, 91, 88, 85, 82, 78, 75, 71, 67,
    63, 60, 56, 52, 48, 43, 39, 35, 31, 26, 22, 18, 13, 9, 4,
    0
};

#define VFO1_SPLIT_Y       32
#define VFO1_UPPER_H       (VFO1_SPLIT_Y)
#define VFO1_RIGHT_X       64
#define VFO1_UPPER_RIGHT_DY 4
#define VFO1_UPPER_RIGHT_DX 2 /* 相对原布局整体左移 2px（原 4） */
#define VFO1_UR_COLON_GAP   2 /* 冒号与右侧数值间距 */
#define VFO1_LOWER_Y_OFF   10
#define VFO1_LOWER_FREQ_DY 5 /* 接收大数字相对基线额外下移（像素） */
#define VFO1_LOWER_LAYOUT_DY 2 /* 信道行/亚音/主频率纵向位置（相对基线） */
#define VFO1_BIG_FREQ_LIFT 4 /* 大数字相对公式基础上移，避免与底栏 TomThumb 重叠 */
#define VFO1_MAIN_AFTER_TONE_GAP 2 /* 亚音区右缘与信道/频率主区间距（像素） */
/* 底行 y=63：dig_14 底缘宜 ≤58，与 59–63 底栏错开 */
#define VFO1_BOTTOM_BAR_Y 63
#define VFO1_TONE_X        2
#define SMETER_CX          32
#define SMETER_CY          28
#define SMETER_RX          23 /* 横向加宽；纵向保持与原先 RY 一致 */
#define SMETER_RY          18
#define SMETER_LBL_GAP       2 /* 默认刻度字与弧的径向间距 */
#define SMETER_TICK_LEN_LBL   3 /* 有数字的主刻度线长度（沿径向，像素级） */
#define SMETER_TICK_LEN_PLUS2 5 /* 「+2」档主刻度 */
#define SMETER_IDX_LBL_PLUS2 11 /* smeterLblAt 中 "+2" 的下标 */
#define SMETER_LBL_EXTRA_S79 3 /* S、+6 等端点刻度字略外移 */
#define SMETER_LBL_EXTRA_1   2 /* 「1」略外移 */
#define SMETER_LBL_EXTRA_7   2 /* 「7」略外移 */
/* 主刻度 16 点：S,1,2,3,4,5,6,7,8,9,+1,+2,+3,+4,+5,+6（弧上 15 等分） */
#define SMETER_TICK_COUNT 16
#define SMETER_TICK_DIV   15 /* TICK_COUNT-1，整弧等角距 */

static const char vfo1_str_tx[] = "tx:";

/* 第 idx 个主刻度角度：idx=0 → S@180°，idx=15 → +6@0° */
static uint8_t vfo1SmeterMajorAngle(uint8_t idx) {
  if (idx >= SMETER_TICK_COUNT)
    return 0;
  return (uint8_t)((180u * (uint32_t)(SMETER_TICK_DIV - idx)) /
                   (uint32_t)SMETER_TICK_DIV);
}

static void vfo1DrawSmeterArcOutline(int cx, int cy, int rx, int ry) {
  int prevX = cx + (int)(((int32_t)rx * SMETER_ICOS91[0]) >> 7);
  int prevY = cy - (int)(((int32_t)ry * SMETER_ISIN91[0]) >> 7);
  for (uint8_t a = 2; a <= 180; a += 2) {
    uint8_t k = (uint8_t)(a >> 1);
    int px = cx + (int)(((int32_t)rx * SMETER_ICOS91[k]) >> 7);
    int py = cy - (int)(((int32_t)ry * SMETER_ISIN91[k]) >> 7);
    DrawLine(prevX, prevY, px, py, C_FILL);
    prevX = px;
    prevY = py;
  }
}

static void vfo1SmeterTick(int cx, int cy, int rxOut, int ryOut, int rxIn,
                           int ryIn, uint8_t angDeg) {
  uint8_t k = (uint8_t)(angDeg >> 1);
  int x0 = cx + (int)(((int32_t)rxIn * SMETER_ICOS91[k]) >> 7);
  int y0 = cy - (int)(((int32_t)ryIn * SMETER_ISIN91[k]) >> 7);
  int x1 = cx + (int)(((int32_t)rxOut * SMETER_ICOS91[k]) >> 7);
  int y1 = cy - (int)(((int32_t)ryOut * SMETER_ISIN91[k]) >> 7);
  DrawLine(x0, y0, x1, y1, C_FILL);
}

static int16_t vfo1SmeterDbm(void) {
  const bool isUhf = ctx->frequency >= 30 * MHZ;
  const uint8_t *r2dBm = rssi2s[isUhf];
  if (!vfo->msm.rssi)
    return -(int16_t)r2dBm[0];
  int16_t dBm = Rssi2DBm(vfo->msm.rssi);
  if (ctx->radio_type == RADIO_BK4819)
    dBm += (int16_t)BK4819_GetAttenuation();
  return dBm;
}

/* RSSI_MIN..RSSI_MAX 线性对应 180°→0°，与 16 档主刻度（S～+6）在同一弧上均匀一致 */
static uint8_t vfo1SmeterNeedleAngle(void) {
  const uint16_t r = vfo->msm.rssi;
  if (!r)
    return 180;
  int ang = ConvertDomain((int)r, (int)RSSI_MIN, (int)RSSI_MAX, 180, 0);
  if (ang < 0)
    ang = 0;
  if (ang > 180)
    ang = 180;
  return (uint8_t)ang;
}

static void vfo1SmeterLabelOutside(uint8_t angDeg, const char *text,
                                  uint8_t extraGap, int8_t dx, int8_t dy) {
  const int rx = SMETER_RX + SMETER_LBL_GAP + (int)extraGap;
  const int ry = SMETER_RY + SMETER_LBL_GAP + (int)extraGap;
  uint8_t k = (uint8_t)(angDeg >> 1);
  int lx = SMETER_CX + (int)(((int32_t)rx * SMETER_ICOS91[k]) >> 7) + (int)dx;
  int ly = SMETER_CY - (int)(((int32_t)ry * SMETER_ISIN91[k]) >> 7) + (int)dy;
  if (lx < 0)
    lx = 0;
  if (ly < 0)
    ly = 0;
  PrintSmallEx((uint8_t)lx, (uint8_t)ly, POS_C, C_FILL, "%s", text);
}

static void vfo1RenderUpperSmeter(void) {
  /* 与 16 主刻度下标对齐：仅部分位置显示字（其余 NULL） */
  static const char *const smeterLblAt[SMETER_TICK_COUNT] = {
      "S", "1", NULL, "3", NULL, "5", NULL, "7", NULL, "9",
      NULL, "+2", NULL, "4", NULL, "6",
  };
  /* extraGap：越大字越靠外；7/9 略靠向弧线；+2/4 位置用 dx/dy 微调 */
  static const uint8_t smeterLblExtraAt[SMETER_TICK_COUNT] = {
      SMETER_LBL_EXTRA_S79, SMETER_LBL_EXTRA_1, 0, 0, 0, 0, 0,
      0, 0,
      1,
      0, 0, 0, 0, 0,
      SMETER_LBL_EXTRA_S79,
  };

  vfo1DrawSmeterArcOutline(SMETER_CX, SMETER_CY, SMETER_RX, SMETER_RY);

  /* 仅绘制有数字的 9 处主刻度，无中间小刻度、无无字主刻度 */
  for (uint8_t i = 0; i < SMETER_TICK_COUNT; i++) {
    if (smeterLblAt[i] == NULL)
      continue;
    uint8_t t = vfo1SmeterMajorAngle(i);
    uint8_t d =
        (i == SMETER_IDX_LBL_PLUS2) ? SMETER_TICK_LEN_PLUS2 : SMETER_TICK_LEN_LBL;
    vfo1SmeterTick(SMETER_CX, SMETER_CY, SMETER_RX, SMETER_RY,
                   SMETER_RX - d, SMETER_RY - d, t);
  }

  uint8_t ang = vfo1SmeterNeedleAngle();
  uint8_t k = (uint8_t)(ang >> 1);
  int32_t ic, is;
  /* 奇数度在相邻两度采样间线性插值，与刻度角度一致；偶数度或 180° 直接查表 */
  if ((ang & 1u) != 0 && ang < 180) {
    ic = ((int32_t)SMETER_ICOS91[k] + (int32_t)SMETER_ICOS91[k + 1] + 1) >> 1;
    is = ((int32_t)SMETER_ISIN91[k] + (int32_t)SMETER_ISIN91[k + 1] + 1) >> 1;
  } else {
    ic = SMETER_ICOS91[k];
    is = SMETER_ISIN91[k];
  }
  int nx = SMETER_CX + (int)(((int32_t)(SMETER_RX - 2) * ic) >> 7);
  int ny = SMETER_CY - (int)(((int32_t)(SMETER_RY - 2) * is) >> 7);
  DrawLine(SMETER_CX, SMETER_CY, nx, ny, C_FILL);
  PutPixel((uint8_t)SMETER_CX, (uint8_t)SMETER_CY, (uint8_t)C_FILL);

  for (uint8_t i = 0; i < SMETER_TICK_COUNT; i++) {
    if (smeterLblAt[i] != NULL) {
      int8_t dx = 0, dy = (i == 0) ? 2 : 0;
      if (i == 11) {
        dx = 4;
        dy = (int8_t)(dy + 2);
      } else if (i == 13) {
        dx = 2;
        dy = (int8_t)(dy + 2);
      }
      vfo1SmeterLabelOutside(vfo1SmeterMajorAngle(i), smeterLblAt[i],
                             smeterLblExtraAt[i], dx, dy);
    }
  }

  int16_t dBm = vfo1SmeterDbm();
  PrintSmallEx(SMETER_CX, (uint8_t)(SMETER_CY + 5), POS_C, C_FILL, "%+d dBm",
               dBm);
}

static void vfo1CopyParamStr(char *dst, size_t n, ParamType p) {
  strncpy(dst, RADIO_GetParamValueString(ctx, p), n - 1);
  dst[n - 1] = '\0';
}

/* TomThumb yAdvance=6；行与行之间再留 1 像素则总步长 7 会超出上半区，此处用 6 兼顾可读与区域 */
#define VFO1_UR_LINE_STEP 6

static void vfo1PrintSmallLabelThenValue(uint8_t x, uint8_t y, const char *label,
                                         const char *value) {
  PrintSmallEx(x, y, POS_L, C_FILL, "%s", label);
  PrintSmallEx((uint8_t)(x + Graphics_GetSmallTextWidth(label) + VFO1_UR_COLON_GAP),
               y, POS_L, C_FILL, "%s", value);
}

static void vfo1RenderUpperRight(void) {
  if (gIsNumNavInput) {
    uint8_t nx = (uint8_t)(LCD_WIDTH - 2 + VFO1_UPPER_RIGHT_DX);
    if (nx > LCD_WIDTH - 1)
      nx = LCD_WIDTH - 1;
    PrintSmallEx(nx, 12 + VFO1_UPPER_RIGHT_DY, POS_R, C_FILL, "CH:%s",
                 gNumNavInput);
    return;
  }

  char vRadio[16];
  char vPower[16];
  char vMod[16];
  char vSqType[16];
  char vSqVal[16];
  char vStep[16];
  vfo1CopyParamStr(vRadio, sizeof(vRadio), PARAM_RADIO);
  vfo1CopyParamStr(vPower, sizeof(vPower), PARAM_POWER);
  vfo1CopyParamStr(vMod, sizeof(vMod), PARAM_MODULATION);
  vfo1CopyParamStr(vSqType, sizeof(vSqType), PARAM_SQUELCH_TYPE);
  vfo1CopyParamStr(vSqVal, sizeof(vSqVal), PARAM_SQUELCH_VALUE);
  vfo1CopyParamStr(vStep, sizeof(vStep), PARAM_STEP);

  const uint8_t x0 = VFO1_RIGHT_X + 1 + VFO1_UPPER_RIGHT_DX;
  uint8_t y = (uint8_t)(1 + VFO1_UPPER_RIGHT_DY);

  vfo1PrintSmallLabelThenValue(x0, y, "radio:", vRadio);
  y += VFO1_UR_LINE_STEP;
  vfo1PrintSmallLabelThenValue(x0, y, "power:", vPower);
  y += VFO1_UR_LINE_STEP;
  vfo1PrintSmallLabelThenValue(x0, y, "mod:", vMod);
  y += VFO1_UR_LINE_STEP;

  PrintSmallEx(x0, y, POS_L, C_FILL, "%s", "sql:");
  PrintSmallEx((uint8_t)(x0 + Graphics_GetSmallTextWidth("sql:") + VFO1_UR_COLON_GAP),
               y, POS_L, C_FILL, "%s %s", vSqType, vSqVal);
  y += VFO1_UR_LINE_STEP;

  vfo1PrintSmallLabelThenValue(x0, y, "step:", vStep);

  /* 上右底部：是否允许发射；功放 PA。「can」与「tx:」间用固定像素，避免整串「can tx:」里空格过宽 */
  {
    const uint8_t gapCanTx = 2; /* can 与 tx: 之间 */
    const uint8_t gapMid = 2;   /* yes/no 与后一段 tx: 之间（原 4px） */
    TXStatus txst = RADIO_CheckTXAllowed(ctx);
    const char *canV =
        (txst == TX_ON) ? "yes" : TX_STATE_NAMES[txst];
    const char *enV = ctx->tx_state.pa_enabled ? "on" : "off";
    const uint8_t yBottom = 35;
    PrintSmallEx(x0, yBottom, POS_L, C_FILL, "%s", "can");
    uint8_t xc = (uint8_t)(x0 + Graphics_GetSmallTextWidth("can") + gapCanTx);
    PrintSmallEx(xc, yBottom, POS_L, C_FILL, "%s", vfo1_str_tx);
    xc = (uint8_t)(xc + Graphics_GetSmallTextWidth(vfo1_str_tx) +
                   VFO1_UR_COLON_GAP);
    PrintSmallEx(xc, yBottom, POS_L, C_FILL, "%s", canV);
    xc = (uint8_t)(xc + Graphics_GetSmallTextWidth(canV) + gapMid);
    PrintSmallEx(xc, yBottom, POS_L, C_FILL, "%s", vfo1_str_tx);
    xc = (uint8_t)(xc + Graphics_GetSmallTextWidth(vfo1_str_tx) +
                   VFO1_UR_COLON_GAP);
    PrintSmallEx(xc, yBottom, POS_L, C_FILL, "%s", enV);
  }
}

static void vfo1FormatRctTct(char *out, size_t cap, bool isRx) {
  out[0] = '\0';
  uint8_t t =
      isRx ? ctx->rx_code.type : ctx->tx_state.tx_code.type;
  uint8_t v =
      isRx ? ctx->rx_code.value : ctx->tx_state.tx_code.value;
  if (t == CODE_TYPE_OFF)
    return;
  char tmp[16];
  PrintRTXCode(tmp, t, v);
  /* CTCSS：接收 RCT / 发射 TCT；DCS：RDCS / TDCS */
  if (strncmp(tmp, "CT:", 3) == 0) {
    snprintf(out, cap, "%sCT:%s", isRx ? "R" : "T", tmp + 3);
  } else if (strncmp(tmp, "DCS:", 4) == 0) {
    snprintf(out, cap, "%s%s", isRx ? "RDCS:" : "TDCS:", tmp + 4);
  } else {
    snprintf(out, cap, "%s", tmp);
  }
}

/* 按 PrintRTXCode + vfo1FormatRctTct 可能的最长串测宽（TomThumb），与实机亚音对齐 */
static uint8_t vfo1ToneColumnReservedPx(void) {
  uint16_t wDcs = Graphics_GetSmallTextWidth("RDCS:D777N");
  uint16_t wCt = Graphics_GetSmallTextWidth("RCT:254.1");
  uint16_t w = (wDcs > wCt) ? wDcs : wCt;
  if (w > 120)
    w = 120;
  return (uint8_t)w;
}

/* PrintBigDigitsEx 使用 dig_11：数字 xAdvance=10，「.」=4；前导空格不在字库内不占宽 */
static uint16_t vfo1BigDigitsWidthPx(uint32_t f) {
  unsigned mhz = (unsigned)(f / MHZ);
  unsigned nd = 0;
  unsigned t = mhz;
  do {
    nd++;
    t /= 10U;
  } while (t);
  return (uint16_t)(nd * 10u + 4u + 30u);
}

static void vfo1DrawBatteryIcon(uint8_t x0) {
  /* 空心壳 + 按 gBatteryPercent(0–100) 比例填充；内腔 8×3 像素，8 档宽度便于辨认 */
  const uint8_t y0 = 57;
  const uint8_t bodyW = 10;
  const uint8_t bodyH = 5;
  const uint8_t innerW = 8;
  const uint8_t innerH = 3;

  DrawRect(x0, y0, bodyW, bodyH, C_FILL);
  FillRect(x0 + 1, y0 + 1, bodyW - 2, bodyH - 2, C_CLEAR);
  FillRect(x0 + bodyW, y0 + 1, 2, 3, C_FILL);

  uint16_t p = gBatteryPercent;
  if (p > 100)
    p = 100;
  uint8_t fw = (uint8_t)((p * innerW + 50) / 100);
  if (p > 0 && fw == 0)
    fw = 1;
  if (fw > innerW)
    fw = innerW;
  if (fw)
    FillRect(x0 + 1, y0 + 1, fw, innerH, C_FILL);
}

static void vfo1RenderBottomBar(void) {
  const uint8_t y = VFO1_BOTTOM_BAR_Y;
  char buf[16];
  uint8_t xEnd = 0;

  switch (gSettings.batteryStyle) {
  case BAT_CLEAN:
    vfo1DrawBatteryIcon(0);
    xEnd = 12;
    break;
  case BAT_PERCENT:
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)gBatteryPercent);
    PrintSmallEx(0, y, POS_L, C_FILL, "%s", buf);
    xEnd = (uint8_t)Graphics_GetSmallTextWidth(buf);
    break;
  case BAT_VOLTAGE:
    snprintf(buf, sizeof(buf), "%u.%02uV", (unsigned)(gBatteryVoltage / 100),
             (unsigned)(gBatteryVoltage % 100));
    PrintSmallEx(0, y, POS_L, C_FILL, "%s", buf);
    xEnd = (uint8_t)Graphics_GetSmallTextWidth(buf);
    break;
  default:
    break;
  }

  char txf[16];
  mhzToS(txf, RADIO_GetParam(ctx, PARAM_TX_FREQUENCY_FACT));
  if (gChargingWithTypeC)
    PrintSmallEx((uint8_t)(xEnd + 1), y, POS_L, C_FILL, "%c", '+');
  {
    const uint8_t wLab = (uint8_t)Graphics_GetSmallTextWidth(vfo1_str_tx);
    const uint8_t wF = (uint8_t)Graphics_GetSmallTextWidth(txf);
    const uint8_t gap = 1;
    const uint8_t total = (uint8_t)(wLab + gap + wF);
    uint8_t x0 = (uint8_t)(LCD_WIDTH - total);
    PrintSmallEx(x0, y, POS_L, C_FILL, "%s", vfo1_str_tx);
    PrintSmallEx((uint8_t)(x0 + wLab + gap), y, POS_L, C_FILL, "%s", txf);
  }
}

static void vfo1RenderLower(uint32_t f) {
  if (gMonitorMode) {
    UI_RSSIBar((uint8_t)(LCD_HEIGHT - 10 + VFO1_LOWER_Y_OFF));
    vfo1RenderBottomBar();
    return;
  }

  const uint8_t yLo =
      (uint8_t)(32 + VFO1_LOWER_Y_OFF + VFO1_LOWER_LAYOUT_DY);
  /* 接收/发射亚音：行位置固定，有内容才绘制文字 */
  const uint8_t yRct =
      (uint8_t)(33 + VFO1_LOWER_Y_OFF + VFO1_LOWER_LAYOUT_DY);
  const uint8_t yTct =
      (uint8_t)(40 + VFO1_LOWER_Y_OFF + VFO1_LOWER_LAYOUT_DY);
  const uint8_t yCh = yLo;
  const uint8_t toneColW = vfo1ToneColumnReservedPx();
  const uint8_t xMain =
      (uint8_t)(VFO1_TONE_X + toneColW + VFO1_MAIN_AFTER_TONE_GAP);

  char rct[20];
  char tct[20];
  vfo1FormatRctTct(rct, sizeof(rct), true);
  vfo1FormatRctTct(tct, sizeof(tct), false);

  if (rct[0])
    PrintSmallEx(VFO1_TONE_X, yRct, POS_L, C_FILL, "%s", rct);
  if (tct[0])
    PrintSmallEx(VFO1_TONE_X, yTct, POS_L, C_FILL, "%s", tct);

  if (vfo->mode == MODE_CHANNEL) {
    PrintMediumEx(xMain, yCh, POS_L, C_FILL, "%03u %s", vfo->channel_index + 1,
                   ctx->name);
  } else {
    PrintMediumEx(xMain, yCh, POS_L, C_FILL, "VFO");
  }

  /* 大数字为 dig_14：像素约在 [y-13,y]；底栏约占 y=59–63，故 yFb≤58 */
  uint8_t yFb = (uint8_t)(44 + VFO1_LOWER_Y_OFF + VFO1_LOWER_FREQ_DY +
                          VFO1_LOWER_LAYOUT_DY - VFO1_BIG_FREQ_LIFT);
  {
    const uint8_t yFbMax = (uint8_t)(VFO1_BOTTOM_BAR_Y - 5);
    if (yFb > yFbMax)
      yFb = yFbMax;
  }

  PrintBigDigitsEx(xMain, yFb, POS_L, C_FILL, "%4u.%03u", f / MHZ,
                   f / 100 % 1000);
  {
    const uint8_t yFrac = (uint8_t)(yFb - 4);
    uint16_t bigW = vfo1BigDigitsWidthPx(f);
    uint16_t xFrac = (uint16_t)xMain + bigW + 1u;
    if (xFrac >= LCD_WIDTH)
      xFrac = (uint16_t)(LCD_WIDTH - 1);
    /* 后两位紧跟大数字宽度，短 MHz（如 99.xx）时不会甩到最右侧 */
    PrintMediumEx((uint8_t)xFrac, yFrac, POS_L, C_FILL, "%02u",
                  (unsigned)(f % 100));
    if (ctx->tx_state.is_active)
      PrintSmallEx(LCD_WIDTH - 1, yFrac, POS_R, C_FILL, "TX");
    else if (vfo->msm.open)
      PrintSmallEx(LCD_WIDTH - 1, yFrac, POS_R, C_FILL, "RX");
  }

  vfo1RenderBottomBar();
}

void VFO1_render(void) {
  uint32_t f = RADIO_GetParam(
      ctx, ctx->tx_state.is_active ? PARAM_TX_FREQUENCY_FACT : PARAM_FREQUENCY);

  vfo1RenderUpperSmeter();
  vfo1RenderUpperRight();
  vfo1RenderLower(f);

  REGSMENU_Draw();
}
