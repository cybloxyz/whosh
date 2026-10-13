#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <ESP_I2S.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Pin ----------------
#define PIN_TOUCH     1
#define PIN_SD_CS     2
#define PIN_SD_SCK    4
#define PIN_SD_MOSI   6
#define PIN_SD_MISO   7
#define PIN_MIC_SD    5    
#define PIN_I2S_BCLK  11
#define PIN_I2S_WS    12
#define PIN_AMP_DIN   10 
#define PIN_OLED_SCL  9
#define PIN_OLED_SDA  8

// ---------------- OLED ----------------
#define SCREEN_W   128
#define SCREEN_H   64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------------- I2S ----------------
#define SAMPLE_RATE 16000
I2SClass i2s;

// ---------------- State ----------------
bool oledOk = false;
bool sdOk   = false;
bool i2sOk  = false;
bool lastTouch = false;
uint32_t lastDraw = 0;
int micLevel = 0;  // 0..100

// ---------------- Beep ----------------
void playBeep(uint16_t freq, uint16_t durMs) {
  if (!i2sOk) return;
  const int N = 128;
  int16_t buf[N];
  static float phase = 0.0f;
  const float step = 2.0f * PI * freq / SAMPLE_RATE;
  uint32_t totalSamples = (uint32_t)SAMPLE_RATE * durMs / 1000;
  uint32_t done = 0;

  while (done < totalSamples) {
    for (int i = 0; i < N; i++) {
      buf[i] = (int16_t)(sinf(phase) * 8000);
      phase += step;
      if (phase > 2.0f * PI) phase -= 2.0f * PI;
    }
    i2s.write((uint8_t *)buf, sizeof(buf));
    done += N;
  }
  memset(buf, 0, sizeof(buf));
  i2s.write((uint8_t *)buf, sizeof(buf));
}

// ---------------- Mic ----------------
void updateMic() {
  if (!i2sOk) return;
  int16_t samples[256];
  size_t n = i2s.readBytes((char *)samples, sizeof(samples));
  size_t cnt = n / sizeof(int16_t);
  if (cnt == 0) return;

  long sum = 0;
  for (size_t i = 0; i < cnt; i++) sum += samples[i];
  int mean = sum / (long)cnt;

  long acc = 0;
  for (size_t i = 0; i < cnt; i++) acc += abs((int)samples[i] - mean);
  int avg = acc / (long)cnt;

  const int maxAvg = 3000;
  int lvl = constrain(map(avg, 0, maxAvg, 0, 100), 0, 100);
  micLevel = (micLevel * 3 + lvl) / 4;  
}

// ---------------- Display ----------------
void drawUI(bool touched) {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("lilfish");
  display.setCursor(70, 0);
  display.print(sdOk ? "SD:OK" : "SD:--");

  display.setCursor(0, 14);
  display.print("Touch: ");
  display.print(touched ? "YA" : "-");

  display.setCursor(0, 28);
  display.print("Mic:");
  display.drawRect(28, 27, 98, 10, SSD1306_WHITE);
  int w = map(micLevel, 0, 100, 0, 94);
  display.fillRect(30, 29, w, 6, SSD1306_WHITE);

  display.setCursor(0, 46);
  display.print(i2sOk ? "I2S OK" : "I2S GAGAL");

  display.display();
}

// ---------------- Init ----------------
void initOLED() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  Serial.println(oledOk ? "[OLED] OK" : "[OLED] GAGAL (cek alamat 0x3C / wiring)");
  if (oledOk) {
    display.clearDisplay();
    display.display();
  }
}

void initSD() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, SPI, 10000000);
  if (sdOk) {
    Serial.printf("[SD] OK, ukuran: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
  } else {
    Serial.println("[SD] GAGAL (kartu belum dipasang / format FAT32?)");
  }
}

void initI2S() {
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DIN, PIN_MIC_SD, -1);
  i2sOk = i2s.begin(I2S_MODE_STD, SAMPLE_RATE,
                    I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_MONO,
                    I2S_STD_SLOT_LEFT);
  Serial.println(i2sOk ? "[I2S] OK" : "[I2S] GAGAL");
}

// ---------------- Esp ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== lilfish boot ===");

  pinMode(PIN_TOUCH, INPUT); 

  initOLED();
  initSD();
  initI2S();

  drawUI(false);
  playBeep(880, 120); 
}

void loop() {
  bool touched = digitalRead(PIN_TOUCH) == HIGH;

  if (touched && !lastTouch) {
    Serial.println("[TOUCH] disentuh");
    playBeep(1200, 150);
  }
  lastTouch = touched;

  updateMic();

  if (millis() - lastDraw > 80) {
    lastDraw = millis();
    drawUI(touched);
  }
}
