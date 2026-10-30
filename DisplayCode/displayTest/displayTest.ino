#define LV_CONF_INCLUDE_SIMPLE
#include "lv_conf.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>

// LCDWIKI ES3C28P-style board pin map, verified against the board manual.
// GPIO 14/15 must not be used for the HX710B: GPIO 15 is touch I2C SCL.
static constexpr int TFT_MOSI_PIN = 11;
static constexpr int TFT_MISO_PIN = 13;
static constexpr int TFT_SCK_PIN = 12;
static constexpr int TFT_CS_PIN = 10;
static constexpr int TFT_DC_PIN = 46;
static constexpr int TFT_BL_PIN = 45;
// LCD reset is shared with ESP32-S3 CHIP_PU/EN. Do not drive a GPIO for it.

static constexpr int TOUCH_SDA_PIN = 16;
static constexpr int TOUCH_SCL_PIN = 15;
static constexpr int TOUCH_RST_PIN = 18;
static constexpr uint8_t TOUCH_I2C_ADDRESS = 0x38;
static constexpr uint8_t DISPLAY_ROTATION = 1;  // landscape, 320x240
static constexpr uint32_t LCD_SPI_HZ = 20000000;
static constexpr uint16_t DISPLAY_WIDTH = 320;
static constexpr uint16_t DISPLAY_HEIGHT = 240;
static constexpr uint16_t LVGL_BUFFER_LINES = 16;

Arduino_DataBus *lcdBus = new Arduino_ESP32SPI(
    TFT_DC_PIN, TFT_CS_PIN, TFT_SCK_PIN, TFT_MOSI_PIN, TFT_MISO_PIN);
Arduino_GFX *lcd = new Arduino_ILI9341(
    lcdBus, -1, DISPLAY_ROTATION, true /* IPS */);

static lv_disp_draw_buf_t drawBuffer;
static lv_color_t drawPixels[DISPLAY_WIDTH * LVGL_BUFFER_LINES];
static lv_disp_drv_t displayDriver;
static lv_indev_drv_t touchDriver;
static lv_obj_t *screenStart = nullptr;
static lv_obj_t *screenDashboard = nullptr;
static uint32_t lastLvglTick = 0;

static void makeNonScrollable(lv_obj_t *obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void styleLabel(lv_obj_t *label, uint32_t color,
                       const lv_font_t *font = &lv_font_montserrat_14) {
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

static lv_obj_t *makeLabel(lv_obj_t *parent, const char *text,
                           uint32_t color,
                           const lv_font_t *font = &lv_font_montserrat_14) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  styleLabel(label, color, font);
  makeNonScrollable(label);
  return label;
}

static lv_obj_t *makePanel(lv_obj_t *parent, int width, int height,
                           int x, int y) {
  lv_obj_t *panel = lv_obj_create(parent);
  makeNonScrollable(panel);
  lv_obj_set_size(panel, width, height);
  lv_obj_set_pos(panel, x, y);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel, 4, LV_PART_MAIN);
  return panel;
}

static void onStartClicked(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    // The display changes screens locally. It does not start a measurement.
    lv_scr_load(screenDashboard);
  }
}

static void onRetestClicked(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    lv_scr_load(screenStart);
  }
}

static void buildStartScreen() {
  screenStart = lv_obj_create(nullptr);
  makeNonScrollable(screenStart);
  lv_obj_set_style_bg_color(screenStart, lv_color_hex(0xFAFAFA), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screenStart, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *tag = makeLabel(screenStart, "DEVICE COMPANION", 0xA85A48);
  lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *title = makeLabel(screenStart, "A clear start.", 0x18181B,
                              &lv_font_montserrat_24);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  // This is a display-only board, so the copy does not imply pump control.
  lv_obj_t *subtitle = makeLabel(screenStart,
      "Press Start to view measurement status.", 0x71717A);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 56);

  lv_obj_t *outerRing = lv_obj_create(screenStart);
  makeNonScrollable(outerRing);
  lv_obj_set_size(outerRing, 140, 140);
  lv_obj_align(outerRing, LV_ALIGN_CENTER, 0, 25);
  lv_obj_set_style_radius(outerRing, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(outerRing, lv_color_hex(0xF4F4F5), LV_PART_MAIN);
  lv_obj_set_style_border_color(outerRing, lv_color_hex(0xE4E4E7), LV_PART_MAIN);
  lv_obj_set_style_border_width(outerRing, 1, LV_PART_MAIN);

  lv_obj_t *middleRing = lv_obj_create(screenStart);
  makeNonScrollable(middleRing);
  lv_obj_set_size(middleRing, 122, 122);
  lv_obj_align(middleRing, LV_ALIGN_CENTER, 0, 25);
  lv_obj_set_style_radius(middleRing, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(middleRing, lv_color_hex(0xFAFAFA), LV_PART_MAIN);
  lv_obj_set_style_border_color(middleRing, lv_color_hex(0xF1D3CE), LV_PART_MAIN);
  lv_obj_set_style_border_width(middleRing, 1, LV_PART_MAIN);

  lv_obj_t *startButton = lv_btn_create(screenStart);
  makeNonScrollable(startButton);
  lv_obj_set_size(startButton, 102, 102);
  lv_obj_align(startButton, LV_ALIGN_CENTER, 0, 25);
  lv_obj_set_style_radius(startButton, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(startButton, lv_color_hex(0xA82824), LV_PART_MAIN);
  lv_obj_set_style_border_width(startButton, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(startButton, 12, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(startButton, lv_color_hex(0xD19A94), LV_PART_MAIN);
  lv_obj_add_event_cb(startButton, onStartClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *buttonText = makeLabel(startButton, "START", 0xFFFFFF,
                                   &lv_font_montserrat_16);
  lv_obj_center(buttonText);
}

static lv_obj_t *makeMetricPanel(lv_obj_t *parent, const char *titleText,
                                 int x, const lv_font_t *valueFont) {
  lv_obj_t *panel = makePanel(parent, 100, 134, x, 38);
  lv_obj_t *title = makeLabel(panel, titleText, 0x0284C7);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);
  lv_obj_t *value = makeLabel(panel, "--", 0x0F172A, valueFont);
  lv_obj_align(value, LV_ALIGN_CENTER, 0, 10);
  return panel;
}

static void buildDashboard() {
  screenDashboard = lv_obj_create(nullptr);
  makeNonScrollable(screenDashboard);
  lv_obj_set_style_bg_color(screenDashboard, lv_color_hex(0xF1F5F9), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screenDashboard, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t *topBar = makePanel(screenDashboard, 308, 30, 6, 4);
  lv_obj_set_style_radius(topBar, 6, LV_PART_MAIN);
  lv_obj_t *mode = makeLabel(topBar, "NIBP MONITOR", 0x475569);
  lv_obj_align(mode, LV_ALIGN_LEFT_MID, 6, 0);

  lv_obj_t *badge = lv_obj_create(topBar);
  makeNonScrollable(badge);
  lv_obj_set_size(badge, 78, 20);
  lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(badge, lv_color_hex(0x64748B), LV_PART_MAIN);
  lv_obj_set_style_border_width(badge, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(badge, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(badge, 2, LV_PART_MAIN);
  lv_obj_t *status = makeLabel(badge, "NO DATA", 0xFFFFFF);
  lv_obj_center(status);

  makeMetricPanel(screenDashboard, "SYS mmHg", 6, &lv_font_montserrat_48);
  makeMetricPanel(screenDashboard, "DIA mmHg", 110, &lv_font_montserrat_48);

  lv_obj_t *mapPanel = makePanel(screenDashboard, 94, 64, 214, 38);
  lv_obj_t *mapTitle = makeLabel(mapPanel, "MAP", 0x64748B);
  lv_obj_align(mapTitle, LV_ALIGN_TOP_LEFT, 4, 2);
  lv_obj_t *mapValue = makeLabel(mapPanel, "--", 0x0284C7,
                                 &lv_font_montserrat_20);
  lv_obj_align(mapValue, LV_ALIGN_BOTTOM_LEFT, 4, -2);

  lv_obj_t *pulsePanel = makePanel(screenDashboard, 94, 66, 214, 106);
  lv_obj_t *pulseTitle = makeLabel(pulsePanel, "BPM", 0xDC2626);
  lv_obj_align(pulseTitle, LV_ALIGN_TOP_LEFT, 4, 2);
  lv_obj_t *pulseValue = makeLabel(pulsePanel, "--", 0xDC2626,
                                   &lv_font_montserrat_20);
  lv_obj_align(pulseValue, LV_ALIGN_BOTTOM_LEFT, 4, -2);

  lv_obj_t *footer = makePanel(screenDashboard, 308, 38, 6, 178);
  lv_obj_t *cuff = makeLabel(footer, "CUFF: -- mmHg", 0x0284C7);
  lv_obj_align(cuff, LV_ALIGN_LEFT_MID, 6, 0);

  lv_obj_t *retestButton = lv_btn_create(footer);
  makeNonScrollable(retestButton);
  lv_obj_set_size(retestButton, 90, 24);
  lv_obj_align(retestButton, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(retestButton, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
  lv_obj_set_style_border_width(retestButton, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(retestButton, 4, LV_PART_MAIN);
  lv_obj_add_event_cb(retestButton, onRetestClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *retestText = makeLabel(retestButton, "RETEST", 0x1E293B);
  lv_obj_center(retestText);
}

static void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area,
                         lv_color_t *colorMap) {
  const int16_t width = area->x2 - area->x1 + 1;
  const int16_t height = area->y2 - area->y1 + 1;
  lcd->draw16bitRGBBitmap(area->x1, area->y1,
                          reinterpret_cast<uint16_t *>(colorMap), width, height);
  lv_disp_flush_ready(driver);
}

static bool readTouchPoint(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TOUCH_I2C_ADDRESS);
  Wire.write(0x02);  // TD_STATUS, followed by P1_XH, P1_XL, P1_YH, P1_YL
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(TOUCH_I2C_ADDRESS, static_cast<uint8_t>(5)) != 5) {
    while (Wire.available()) Wire.read();
    return false;
  }

  const uint8_t touchCount = Wire.read() & 0x0F;
  const uint8_t xHigh = Wire.read();
  const uint8_t xLow = Wire.read();
  const uint8_t yHigh = Wire.read();
  const uint8_t yLow = Wire.read();
  if (touchCount == 0) {
    return false;
  }

  const uint16_t rawX = ((xHigh & 0x0F) << 8) | xLow;
  const uint16_t rawY = ((yHigh & 0x0F) << 8) | yLow;

  // FT6336 reports native 240x320 coordinates. Rotation 1 exchanges axes.
  x = rawY;
  y = rawX;
  return x < DISPLAY_WIDTH && y < DISPLAY_HEIGHT;
}

static void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint16_t x = 0;
  uint16_t y = 0;
  if (readTouchPoint(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static bool initializeTouch() {
  pinMode(TOUCH_RST_PIN, OUTPUT);
  digitalWrite(TOUCH_RST_PIN, LOW);
  delay(10);
  digitalWrite(TOUCH_RST_PIN, HIGH);
  delay(300);

  Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, 400000);
  Wire.beginTransmission(TOUCH_I2C_ADDRESS);
  return Wire.endTransmission() == 0;
}

void setup() {
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);  // documented active-high backlight

  Serial.begin(115200);  // optional diagnostics; firmware does not wait for USB
  lv_init();

  if (!lcd->begin(LCD_SPI_HZ)) {
    Serial.println("LCD initialization failed");
    digitalWrite(TFT_BL_PIN, LOW);
    while (true) delay(1000);
  }

  const bool touchReady = initializeTouch();
  if (!touchReady) {
    Serial.println("Touch controller not detected; display UI will still run");
  }

  lv_disp_draw_buf_init(&drawBuffer, drawPixels, nullptr,
                        DISPLAY_WIDTH * LVGL_BUFFER_LINES);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = DISPLAY_WIDTH;
  displayDriver.ver_res = DISPLAY_HEIGHT;
  displayDriver.flush_cb = flushDisplay;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);

  lv_indev_drv_init(&touchDriver);
  touchDriver.type = LV_INDEV_TYPE_POINTER;
  touchDriver.read_cb = readTouch;
  lv_indev_drv_register(&touchDriver);

  buildStartScreen();
  buildDashboard();
  lv_scr_load(screenStart);
  lastLvglTick = millis();
}

void loop() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - lastLvglTick;
  if (elapsed > 0) {
    lv_tick_inc(elapsed);
    lastLvglTick = now;
  }
  lv_timer_handler();
  delay(5);
}
