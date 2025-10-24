// KY-038 Noise Meter (FAST, no flicker, robust sensor-detect, alternate pins)
// Arduino Mega + MCUFRIEND 2.8" TFT (UNO-shield)
// KY-038: AO->A11, DO->D24 (active-LOW with INPUT_PULLUP), VCC->5V, GND->GND
// Optional LED: D13

#include <MCUFRIEND_kbv.h>
#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <math.h>
#include <string.h>

MCUFRIEND_kbv tft;

// ---------- Pins ----------
const int PIN_MIC_AO = A11;  // analog
const int PIN_MIC_DO = 24;   // digital (INPUT_PULLUP, active-LOW)
const int PIN_LED    = 13;

// ---------- dB config ----------
const float ADC_REF_V   = 5.0f;
const float ADC_LSB_V   = ADC_REF_V / 1023.0f;
const float DB_REL_REF_COUNTS = 1023.0f; // full-scale reference

// ---------- Sampling ----------
const int   NUM_SAMPLES   = 12;   // fast window

// ---------- UI ----------
#define COL_BLACK 0x0000
#define COL_WHITE 0xFFFF
#define COL_GREY  0x7BEF
#define COL_GREEN 0x07E0
#define COL_YELL  0xFFE0
#define COL_RED   0xF800
#define COL_CYAN  0x07FF

const int METER_X=20, METER_Y=90, METER_W=280, METER_H=24;
const int BIGDB_Y=44, LABEL_Y2=METER_Y+METER_H+20;
const int CATEGORY_Y = LABEL_Y2 + 22;
const int SCALE_Y = METER_Y + METER_H + 2;

const uint32_t BAR_FRAME_MS  = 5;
const uint32_t TEXT_FRAME_MS = 30;
const float    TEXT_EPS      = 0.5f;

float dcMean=512.0f, envelopeCounts=0.0f, combinedEMA=0.0f;
const float DC_BETA=0.03f, ENV_DECAY=0.88f, ENV_MIX=0.60f, COMB_EMA_ALPHA=0.45f;

uint32_t lastBarMs=0, lastTextMs=0, lastTrigMs=0;
bool ledOn=false;
int prevBarPixels=0;
float lastBigDBShown=1e9;
int lastDOState = HIGH;
int lastCategoryIdx = -1;

// ---------- Sensor-disconnect detection ----------
int   badWindows = 0;
const int BAD_WINDOWS_LIMIT = 25;
bool  sensorOK = true;

// ---------------------- UI ----------------------

// 🎤 Draw microphone icon — near bottom-right
void drawMicrophoneSymbol() {
  int micBaseX = 280; 
  int micBaseY = 220;  // slightly above bottom

  // Mic head (oval)
  tft.fillRoundRect(micBaseX, micBaseY - 40, 24, 35, 8, COL_CYAN);
  tft.drawRoundRect(micBaseX, micBaseY - 40, 24, 35, 8, COL_WHITE);

  // Mic stem
  tft.fillRect(micBaseX + 10, micBaseY - 5, 4, 12, COL_WHITE);

  // Mic base stand
  tft.fillRect(micBaseX - 5, micBaseY + 7, 34, 4, COL_WHITE);

  // Simulated curved bracket (approximation using short lines)
  int cx = micBaseX + 12;
  int cy = micBaseY - 8;
  for (int angle = -90; angle <= 90; angle += 15) {
    float rad = angle * 3.14159 / 180.0;
    int x1 = cx + 20 * cos(rad);
    int y1 = cy + 10 * sin(rad);
    int x2 = cx + 20 * cos(rad + 0.15);
    int y2 = cy + 10 * sin(rad + 0.15);
    tft.drawLine(x1, y1, x2, y2, COL_WHITE);
  }
}

// Draw 4 thick white dividers (0/20/40/60/80)
void drawBarDividers(){
  for (int i=1; i<=3; ++i){
    int x = METER_X + (METER_W * i)/4;
    for (int dx=-1; dx<=1; ++dx){
      tft.drawLine(x+dx, METER_Y-3, x+dx, METER_Y + METER_H + 3, COL_WHITE);
    }
  }
}

// Centered tick-label helper (uses classic 6x8 font scaled by size)
void drawCenteredLabel(int xCenter, int y, const char* txt, int size=2) {
  int w = strlen(txt) * 6 * size;
  tft.setCursor(xCenter - w/2, y);
  tft.print(txt);
}

// Draw numeric tick labels 0,20,40,60,80 under bar
void drawScaleLabels() {
  tft.setTextSize(2);
  tft.setTextColor(COL_WHITE, COL_BLACK);

  for (int i=0; i<=4; ++i) {
    int x = METER_X + (METER_W * i)/4;         // tick position
    const char* label =
      (i==0) ? "0" :
      (i==1) ? "20" :
      (i==2) ? "40" :
      (i==3) ? "60" : "80";
    drawCenteredLabel(x, SCALE_Y+12, label, 2);
  }
}

void drawStaticUI(){
  tft.fillScreen(COL_BLACK);
  tft.setRotation(1);

  tft.setTextSize(2); tft.setTextColor(COL_WHITE, COL_BLACK);
  tft.setCursor(10,10); tft.print("KY-038 Sound Meter");

  // Big dB number
  tft.setTextSize(3); tft.setCursor(10, BIGDB_Y); tft.print("dB: --.-");

  // Bar outline
  tft.fillRect(METER_X, METER_Y, METER_W, METER_H, COL_BLACK);
  tft.drawRect(METER_X-1, METER_Y-1, METER_W+2, METER_H+2, COL_WHITE);
  drawBarDividers();
  drawScaleLabels();  // <-- add labels 0,20,40,60,80

  // Category placeholder
  tft.fillRect(8, CATEGORY_Y-1, 304, 20, COL_BLACK);
  tft.setCursor(10, CATEGORY_Y); tft.print("Cat: ---");

  // DO placeholder
  tft.setCursor(220, CATEGORY_Y); tft.print("DO: -");

  // 🎤 Microphone icon (bottom-right)
  drawMicrophoneSymbol();
  
  prevBarPixels = 0;
  lastBigDBShown = 1e9;
  lastDOState = HIGH;
  lastCategoryIdx = -1;
}

// returns false if this window looks disconnected; true if valid
bool sampleWindow(float &combinedCounts){
  double accSq = 0.0;
  int peakAbs = 0, minV = 1023, maxV = 0;

  for (int i=0;i<NUM_SAMPLES;i++){
    int s = analogRead(PIN_MIC_AO);
    s = constrain(s, 0, 1023);
    if (s < minV) minV = s;
    if (s > maxV) maxV = s;
    dcMean += DC_BETA * (s - dcMean);
    int c = s - (int)dcMean;
    int a = abs(c);
    if (a > peakAbs) peakAbs = a;
    accSq += (double)c * (double)c;
  }

  bool bad = false;
  if ((maxV - minV) <= 1) bad = true;
  if (maxV >= 1022 || minV <= 1) bad = true;

  if (bad) {
    badWindows++;
    if (badWindows >= BAD_WINDOWS_LIMIT) sensorOK = false;
    digitalWrite(PIN_LED, (millis()/400)%2);
    return false;
  } else {
    badWindows = 0;
    if (!sensorOK) {
      sensorOK = true;
      combinedEMA = 0.0f;
      envelopeCounts = 0.0f;
    }
    digitalWrite(PIN_LED, LOW);
  }

  float rms = sqrt(accSq / (double)NUM_SAMPLES);
  float peakLike = (float)peakAbs;
  if (peakLike > envelopeCounts) envelopeCounts = peakLike;
  else envelopeCounts *= ENV_DECAY;

  float combined = (1.0f - ENV_MIX) * rms + (ENV_MIX) * envelopeCounts;
  if (combinedEMA <= 0.0f) combinedEMA = combined;
  combinedEMA += COMB_EMA_ALPHA * (combined - combinedEMA);
  combinedCounts = combinedEMA;
  return true;
}

void drawBar_NoFlicker(float dBpos){
  dBpos = constrain(dBpos, 0.0f, 80.0f);
  int w = (int)(dBpos * (float)METER_W / 80.0f);
  w = constrain(w, 0, METER_W);

  if (w > prevBarPixels){
    float pct = 100.0f * w / (float)METER_W;
    uint16_t col = (pct > 60.0f) ? COL_RED : (pct > 40.0f ? COL_YELL : COL_GREEN);
    tft.fillRect(METER_X + prevBarPixels, METER_Y, w - prevBarPixels, METER_H, col);
  } else if (w < prevBarPixels){
    tft.fillRect(METER_X + w, METER_Y, prevBarPixels - w, METER_H, COL_BLACK);
  }
  prevBarPixels = w;

  // Keep outline & dividers on top
  tft.drawRect(METER_X-1, METER_Y-1, METER_W+2, METER_H+2, COL_WHITE);
  drawBarDividers();
}

int categoryFromDPos(float dBpos){
  float pct = (dBpos / 80.0f) * 100.0f;
  if (pct > 60.0f) return 2;
  if (pct > 40.0f) return 1;
  return 0;
}
const char* categoryLabel(int idx){
  switch(idx){
    case 0: return "Quiet";
    case 1: return "Moderate";
    default: return "Loud";
  }
}
uint16_t categoryColor(int idx){
  return (idx==2) ? COL_RED : ((idx==1) ? COL_YELL : COL_GREEN);
}

void drawCategoryFromBar(float dBpos){
  int idx = categoryFromDPos(dBpos);
  if (idx == lastCategoryIdx) return;
  const char* label = categoryLabel(idx);
  uint16_t col = categoryColor(idx);

  // clear and draw
  tft.fillRect(8, CATEGORY_Y-1, 304, 20, COL_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(col, COL_BLACK);
  tft.setCursor(10, CATEGORY_Y);
  tft.print("Cat: ");
  tft.print(label);
  lastCategoryIdx = idx;
}

void drawText_NoFlicker(float dBpos, int doState){
  static char bigDBBuffer[16] = "";
  if (fabs(dBpos - lastBigDBShown) >= TEXT_EPS){
    char newBuf[16];
    dtostrf(dBpos, 6, 1, newBuf);
    if (strcmp(newBuf, bigDBBuffer) != 0) {
      strcpy(bigDBBuffer, newBuf);
      tft.setTextSize(3); tft.setTextColor(COL_WHITE, COL_BLACK);
      tft.setCursor(10, BIGDB_Y);
      tft.print("dB:");
      tft.print(bigDBBuffer);
      lastBigDBShown = dBpos;
    }
  }

  if (doState != lastDOState) {
    tft.setTextSize(2);
    tft.setTextColor(COL_WHITE, COL_BLACK);
    tft.setCursor(220, CATEGORY_Y);
    tft.print("DO: ");
    tft.print(doState == LOW ? "H" : "L");
    lastDOState = doState;
  }
}

void handleDigitalTrigger(int &doState){
  doState = digitalRead(PIN_MIC_DO);
  uint32_t now = millis();
  if (doState == LOW){
    if (!ledOn) {
      digitalWrite(PIN_LED, HIGH);
      ledOn = true;
      lastTrigMs = now;
    }
  }
  if (ledOn && (now - lastTrigMs > 120)){
    digitalWrite(PIN_LED, LOW);
    ledOn = false;
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(PIN_MIC_AO, INPUT);
  pinMode(PIN_MIC_DO, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  uint16_t ID = tft.readID();
  tft.begin(ID);
  tft.setRotation(1);
  drawStaticUI();

  Serial.println("KY-038 Sound Meter initialized");
}

void loop(){
  float comb = 0.0f;
  bool ok = sampleWindow(comb);

  float dBrelNeg = 20.0f * log10(max(comb / DB_REL_REF_COUNTS, 0.001f));
  dBrelNeg = constrain(dBrelNeg, -80.0f, 0.0f);
  float dBpos = dBrelNeg + 80.0f;

  int doState = HIGH;
  handleDigitalTrigger(doState);

  const uint32_t now = millis();
  if (now - lastBarMs >= BAR_FRAME_MS){
    drawBar_NoFlicker(dBpos);
    lastBarMs = now;
  }
  if (now - lastTextMs >= TEXT_FRAME_MS){
    drawText_NoFlicker(dBpos, doState);
    drawCategoryFromBar(dBpos);
    lastTextMs = now;
  }
}
