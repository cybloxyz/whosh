#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <driver/i2s.h>
#include <math.h>

enum State { S_EYES, S_MENU, S_JUMP, S_REACT, S_MIC, S_STATUS };
enum Mood {
  M_NORMAL, M_HAPPY, M_SAD, M_ANGRY, M_SLEEPY, M_SURPRISED, M_WINK,
  M_SUSPICIOUS, M_BORED, M_CONFUSED, M_SQUINT, M_CRY, M_SPARKLE, M_DIZZY,
  M_SMUG, M_SCAN, M_KO, M_RAGE, M_SLEEP, M_COUNT
};

// ---------------- PIN ----------------
#define OLED_SDA   8
#define OLED_SCL   9
#define TOUCH_PIN  5
#define I2S_SDMIC  1
#define I2S_BCLK   11
#define I2S_WS     12
#define I2S_DOUT   10
#define SD_MISO    43
#define SD_MOSI    44
#define SD_SCK     2
#define SD_CS      4

#define WAKE_MARGIN   3.0f    // ambang = noise * ini + WAKE_ABS
#define WAKE_ABS      120.0f
#define ZCR_SH        0.17f   // "sh" di ujung kata -> zero crossing tinggi
#define LOUD_RMS      7000.0f // tepuk tangan / suara keras

#define AUTO_SLEEP_MS 0

// ---------------- OLED ----------------
Adafruit_SH1106G display(128, 64, &Wire, -1);
#define W SH110X_WHITE
#define B SH110X_BLACK

// ---------------- AUDIO ----------------
#define SR 16000
#define FR 256                       // frame per chunk (16 ms)
#define I2S_PORT I2S_NUM_0

enum Snd { SND_NONE, SND_HELLO, SND_HAPPY, SND_TAP, SND_JUMP, SND_OVER, SND_GO, SND_SELECT, SND_STARTUP };
struct Note { uint16_t f, ms; };
const Note N_HAPPY[]   = {{523,90},{659,90},{784,90},{1047,170},{0,0}};
const Note N_TAP[]     = {{880,40},{0,0}};
const Note N_JUMP[]    = {{600,40},{900,60},{0,0}};
const Note N_OVER[]    = {{440,120},{330,120},{220,260},{0,0}};
const Note N_GO[]      = {{1200,120},{0,0}};
const Note N_SELECT[]  = {{700,50},{1000,80},{0,0}};
const Note N_STARTUP[] = {{392,80},{523,80},{659,80},{784,80},{1047,200},{0,0}};

volatile int   soundReq  = SND_NONE;
volatile bool  wakeFlag  = false;
volatile bool  loudFlag  = false;
volatile float micLevel  = 0;
volatile float dbgNoise  = 0;
volatile float dbgThr    = 0;
volatile int   dbgState  = 0;
volatile int   vol       = 6;        
bool sdOk = false;

// ---------------- STATE ----------------
State state = S_EYES;
Preferences prefs;
int   hiJump = 0;
int   bestReact = 0;     
float happy = 70;        
bool  dirty = false;

// ---------------- INPUT ----------------
bool touchPrev = false;
uint32_t pressT = 0, lastShort = 0, tapDeadline = 0;
bool pendingTap = false;
bool pressEdge = false, shortRel = false, longRel = false, tapEvt = false, dblEvt = false, holding = false;

void readTouch() {
  bool t = digitalRead(TOUCH_PIN);
  uint32_t now = millis();
  if (t && !touchPrev) { pressT = now; pressEdge = true; }
  if (!t && touchPrev) {
    uint32_t dur = now - pressT;
    if (dur < 500) {
      shortRel = true;
      if (now - lastShort < 350) { dblEvt = true; pendingTap = false; lastShort = 0; }
      else { pendingTap = true; tapDeadline = now + 350; lastShort = now; }
    } else longRel = true;
  }
  if (pendingTap && now > tapDeadline) { pendingTap = false; tapEvt = true; }
  holding = t && (now - pressT > 500);
  touchPrev = t;
}
void clearEvents() { pressEdge = shortRel = longRel = tapEvt = dblEvt = false; }

const Note* seq = nullptr; int ni = 0; uint32_t nLeft = 0; float ph = 0;
File wav; bool wavOn = false; bool playing = false;

bool wavSeekData(File &f) {
  f.seek(12);
  uint8_t h[8];
  while (f.read(h, 8) == 8) {
    uint32_t sz = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
    if (!memcmp(h, "data", 4)) return true;
    f.seek(f.position() + sz);
  }
  return false;
}

void startSound(int id) {
  if (wavOn) { wav.close(); wavOn = false; }
  seq = nullptr; playing = false;
  if (id == SND_HELLO && sdOk && SD.exists("/hello.wav")) {
    wav = SD.open("/hello.wav");
    if (wav && wavSeekData(wav)) { wavOn = true; playing = true; return; }
    if (wav) wav.close();
  }
  switch (id) {
    case SND_HELLO: case SND_HAPPY: seq = N_HAPPY; break;
    case SND_TAP: seq = N_TAP; break;
    case SND_JUMP: seq = N_JUMP; break;
    case SND_OVER: seq = N_OVER; break;
    case SND_GO: seq = N_GO; break;
    case SND_SELECT: seq = N_SELECT; break;
    case SND_STARTUP: seq = N_STARTUP; break;
  }
  if (seq) { ni = 0; nLeft = (uint32_t)seq[0].ms * SR / 1000; ph = 0; playing = true; }
}

void fillTx(int32_t *tx, int n) {
  float g = vol / 10.0f;
  if (wavOn) {
    static int16_t buf[FR];
    int got = wav.read((uint8_t *)buf, n * 2) / 2;
    for (int i = 0; i < n; i++) {
      int32_t s = (i < got) ? (int32_t)(buf[i] * g) : 0;
      tx[2 * i] = tx[2 * i + 1] = s * 65536;
    }
    if (got < n) { wav.close(); wavOn = false; playing = false; }
    return;
  }
  for (int i = 0; i < n; i++) {
    float s = 0;
    if (seq) {
      const Note &nt = seq[ni];
      if (nt.f) { s = sinf(ph) * 6000.0f * g; ph += 6.2831853f * nt.f / SR; if (ph > 6.2831853f) ph -= 6.2831853f; }
      if (--nLeft == 0) {
        ni++;
        if (seq[ni].ms == 0) { seq = nullptr; playing = false; }
        else nLeft = (uint32_t)seq[ni].ms * SR / 1000;
      }
    }
    tx[2 * i] = tx[2 * i + 1] = (int32_t)(s * 65536.0f);
  }
}

enum { WK_IDLE, WK_HEY, WK_GAP, WK_SH };
void processMic(const int32_t *rx, int n) {
  static float dc = 0, noise = 60;
  static int st = WK_IDLE, len = 0, low = 0, gap = 0, zn = 0;
  static float zbuf[64];
  static uint32_t cooldown = 0, loudCool = 0;

  float sum = 0, prev = 0; int zc = 0;
  for (int i = 0; i < n; i++) {
    float raw = (float)(rx[2 * i] >> 16);
    dc += (raw - dc) * 0.002f;
    float s = raw - dc;
    sum += s * s;
    if (fabsf(s) > 8) { if (prev != 0 && ((s > 0) != (prev > 0))) zc++; prev = s; }
  }
  float rms = sqrtf(sum / n);
  float zcr = (float)zc / n;
  micLevel = rms;

  uint32_t now = millis();
  if (playing || now < cooldown) { st = WK_IDLE; dbgState = 0; return; }   

  if (st == WK_IDLE && rms < noise * 2 + 30) noise += (rms - noise) * 0.02f;
  float thr = noise * WAKE_MARGIN + WAKE_ABS;
  dbgNoise = noise; dbgThr = thr;

  if (rms > LOUD_RMS && now > loudCool) { loudFlag = true; loudCool = now + 2500; }

  bool act = rms > ((st == WK_IDLE || st == WK_GAP) ? thr : thr * 0.6f);
  switch (st) {
    case WK_IDLE:
      if (act && zcr < 0.22f) { st = WK_HEY; len = 1; low = 0; }
      break;
    case WK_HEY:
      if (act) { len++; low = 0; if (len > 30) st = WK_IDLE; }
      else if (++low >= 3) { if (len >= 5 && len <= 30) { st = WK_GAP; gap = 0; } else st = WK_IDLE; }
      break;
    case WK_GAP:
      if (act) { st = WK_SH; len = 1; low = 0; zn = 0; zbuf[zn++] = zcr; }
      else if (++gap > 26) st = WK_IDLE;
      break;
    case WK_SH:
      if (act) { len++; low = 0; if (zn < 64) zbuf[zn++] = zcr; if (len > 60) st = WK_IDLE; }
      else if (++low >= 3) {
        if (len >= 8 && len <= 60 && zn > 3) {
          int from = zn * 2 / 3; float m = 0;
          for (int i = from; i < zn; i++) m += zbuf[i];
          m /= (zn - from);
          Serial.printf("[wake] kandidat len=%d zcrAkhir=%.3f (butuh >%.2f)\n", len, m, ZCR_SH);
          if (m > ZCR_SH) { wakeFlag = true; cooldown = now + 3000; }
        }
        st = WK_IDLE;
      }
      break;
  }
  dbgState = st;
}

void audioTask(void *) {
  static int32_t rx[FR * 2], tx[FR * 2];
  for (;;) {
    if (soundReq != SND_NONE) { int id = soundReq; soundReq = SND_NONE; startSound(id); }
    size_t br = 0;
    i2s_read(I2S_PORT, rx, sizeof(rx), &br, portMAX_DELAY);
    int n = br / 8;
    if (n > 0) processMic(rx, n);
    if (playing && n > 0) {
      fillTx(tx, n);
      size_t bw = 0;
      i2s_write(I2S_PORT, tx, n * 8, &bw, portMAX_DELAY);
    }
  }
}

void i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = FR;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_BCLK;
  pins.ws_io_num = I2S_WS;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num = I2S_SDMIC;
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

struct Eye { float w, h, r, lidTop, slant, lidBot, pupil; };
Eye ec[2], et[2];
Mood mood = M_NORMAL, pendingMood = M_NORMAL; bool hasPending = false;
float lx = 0, ly = 0, lxT = 0, lyT = 0;
float eyeShift = 0, eyeShiftT = 0;
float blinkF = 1; bool blinking = false; int blinkLeft = 0;
uint32_t blinkStart = 0, blinkDur = 170;
uint32_t nextBlink = 2000, nextLook = 1000, moodUntil = 0;
uint32_t greetUntil = 0, lastAct = 0;
bool asleep = false;

void setEye(int i, float w, float h, float r, float lt, float sl, float lb, float p) {
  et[i] = {w, h, r, lt, sl, lb, p};
}
void setBoth(float w, float h, float r, float lt, float sl, float lb, float p) {
  setEye(0, w, h, r, lt, sl, lb, p); setEye(1, w, h, r, lt, sl, lb, p);
}

void applyMood(Mood m) {
  mood = m;
  switch (m) {
    case M_SAD: case M_CRY:    setBoth(30, 30, 8, 0.08f, -0.55f, 0, 0); break;
    case M_ANGRY: case M_RAGE: setBoth(32, 28, 6, 0, 0.55f, 0, 0); break;
    case M_SLEEPY:     setBoth(30, 34, 8, 0.5f, 0, 0, 0); break;
    case M_SURPRISED:  setBoth(26, 42, 13, 0, 0, 0, 5); break;
    case M_SUSPICIOUS: setBoth(32, 30, 6, 0.42f, 0, 0.28f, 0); break;
    case M_BORED:      setBoth(30, 34, 8, 0.45f, 0, 0, 0); break;
    case M_CONFUSED:   setEye(0, 30, 36, 9, 0, 0, 0, 0); setEye(1, 20, 22, 7, 0.1f, 0, 0, 0); break;
    case M_SPARKLE:    setBoth(34, 38, 12, 0, 0, 0, 11); break;
    case M_HAPPY:      setBoth(32, 36, 10, 0, 0, 0.38f, 0); break;           
    case M_WINK:       setEye(0, 30, 34, 8, 0, 0, 0, 0); setEye(1, 30, 4, 2, 0, 0, 0, 0); break;
    case M_SMUG:       setBoth(34, 30, 8, 0.35f, 0, 0.35f, 0); break;      
    case M_SCAN:       setBoth(22, 40, 6, 0, 0, 0, 0); break;
    case M_SLEEP:      ec[0].h = ec[1].h = 3; setBoth(30, 3, 2, 0, 0, 0, 0); break;
    default:           setBoth(30, 34, 8, 0, 0, 0, 0); break;  
  }
}

void initEyes() {
  applyMood(M_NORMAL);
  ec[0] = et[0]; ec[1] = et[1];
  ec[0].h = ec[1].h = 3;                
}

void startBlink(int n, uint32_t dur) {
  blinking = true; blinkStart = millis(); blinkDur = dur; blinkLeft = n;
}

void setMood(Mood m) {
  pendingMood = m; hasPending = true;
  if (!blinking) startBlink(1, 150);
}
Mood targetMood() { return hasPending ? pendingMood : mood; }

void coverTri(int x, int top, int w, int cut, bool left) {
  if (left) display.fillTriangle(x - 1, top - 1, x + w + 1, top - 1, x - 1, top + cut, B);
  else      display.fillTriangle(x - 1, top - 1, x + w + 1, top - 1, x + w + 1, top + cut, B);
}

void thickLine(int x0, int y0, int x1, int y1) {
  display.drawLine(x0, y0, x1, y1, W);
  display.drawLine(x0 + 1, y0, x1 + 1, y1, W);
  display.drawLine(x0, y0 + 1, x1, y1 + 1, W);
}

void drawSpiral(int cx, int cy, float rot) {
  float px = cx, py = cy;
  for (int i = 1; i <= 36; i++) {
    float a = rot + i * 0.5f, r = i * 0.36f;
    float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
    display.drawLine((int)px, (int)py, (int)x, (int)y, W);
    display.drawLine((int)px + 1, (int)py, (int)x + 1, (int)y, W);
    px = x; py = y;
  }
}

void drawEye(int e, int cx, int y) {
  Eye &p = ec[e];
  float breathe = (mood == M_SLEEP) ? 0 : 1.2f * sinf(millis() / 800.0f);
  int h = (int)((p.h + breathe) * blinkF); if (h < 3) h = 3;
  int w = (int)p.w;
  int x = cx - w / 2, top = y - h / 2;
  int r = min((int)p.r, min(w, h) / 2);
  display.fillRoundRect(x, top, w, h, r, W);
  float lt = p.lidTop;
  if (mood == M_SLEEPY) lt += 0.18f * sinf(millis() / 650.0f);   
  if (lt > 0.01f) display.fillRect(x - 1, top - 1, w + 2, (int)(h * lt) + 1, B);
  if (p.lidBot > 0.01f) { int lb = (int)(h * p.lidBot); display.fillRect(x - 1, top + h - lb, w + 2, lb + 1, B); }
  if (fabsf(p.slant) > 0.02f) {
    int cut = (int)(fabsf(p.slant) * h);
    bool left = (p.slant > 0) ? (e == 1) : (e == 0);  
    coverTri(x, top, w, cut, left);
  }
  if (p.pupil > 0.5f) {
    int pr = (int)p.pupil, px = cx + (int)(lx * 0.4f), py = y + (int)(ly * 0.4f);
    display.fillCircle(px, py, pr, B);
    if (mood == M_SPARKLE) { display.fillCircle(px - 4, py - 4, 3, W); display.fillCircle(px + 4, py + 4, 1, W); }
  }
}

void drawFace(bool greeting) {
  display.clearDisplay();
  uint32_t now = millis();
  int jx = 0, jy = 0;
  if (mood == M_RAGE) { jx = random(-2, 3); jy = random(-1, 2); }
  int bob = (mood == M_HAPPY || mood == M_SPARKLE) ? (int)(2 * sinf(now / 180.0f)) : 0;
  int cx[2] = {38 + (int)lx + jx, 90 + (int)lx + jx};
  int y = 30 + (int)eyeShift + (int)ly + jy + bob;
  bool special = false;

  for (int e = 0; e < 2; e++) {
    switch (mood) {
      case M_SQUINT:                            
        special = true;
        if (e == 0) { thickLine(cx[e] - 12, y - 10, cx[e] + 8, y); thickLine(cx[e] + 8, y, cx[e] - 12, y + 10); }
        else        { thickLine(cx[e] + 12, y - 10, cx[e] - 8, y);    thickLine(cx[e] - 8, y, cx[e] + 12, y + 10); }
        break;
      case M_KO:                                   
        special = true;
        thickLine(cx[e] - 10, y - 10, cx[e] + 10, y + 10);
        thickLine(cx[e] - 10, y + 10, cx[e] + 10, y - 10); break;
      case M_DIZZY:                                 
        special = true;
        drawSpiral(cx[e], y, (e == 0 ? 1 : -1) * now / 150.0f); break;
      case M_SLEEP: {                              
        int br = (int)(1.5f * sinf(now / 700.0f));
        display.fillRoundRect(cx[e] - 14, y + 2 + br, 28, 4, 2, W); break;
      }
      default: drawEye(e, cx[e], y); break;
    }
  }

  if (special && blinking && blinkF < 0.98f) {
    int c = (int)((1 - blinkF) * 22);
    display.fillRect(0, y - 22, 128, c, B);
    display.fillRect(0, y + 22 - c, 128, c, B);
  }

  if (mood == M_CRY) {                             
    for (int e = 0; e < 2; e++) {
      float ph = ((now + e * 450) % 1100) / 1100.0f;
      int tx = cx[e] + (e == 0 ? -11 : 11);
      int ty = y + 14 + (int)(ph * (62 - (y + 14)));
      display.fillCircle(tx, ty, 2, W);
      display.fillTriangle(tx - 2, ty - 1, tx + 2, ty - 1, tx, ty - 6, W);
    }
  }
  if (mood == M_CONFUSED && ((now / 400) % 2)) {
    display.setTextSize(2); display.setTextColor(W); display.setCursor(110, 2); display.print("?");
  }
  if (mood == M_SLEEP) {                           
    display.setTextSize(1); display.setTextColor(W);
    for (int k = 0; k < 3; k++) {
      float ph = fmodf(now / 2400.0f + k / 3.0f, 1.0f);
      display.setCursor(92 + (int)(ph * 26), 22 - (int)(ph * 20));
      display.print(k % 2 ? "z" : "Z");
    }
  }
  if (greeting) {
    display.setTextSize(1); display.setTextColor(W);
    display.setCursor(4, 44);  display.print("hey it's a good day");
    display.setCursor(4, 54);  display.print("isn't it? \\(^o^)/");
  }
}

void updateEyeAnim() {
  uint32_t now = millis();
  for (int i = 0; i < 2; i++) {
    float k = 0.3f;
    ec[i].w += (et[i].w - ec[i].w) * k;           ec[i].h += (et[i].h - ec[i].h) * k;
    ec[i].r += (et[i].r - ec[i].r) * k;           ec[i].lidTop += (et[i].lidTop - ec[i].lidTop) * k;
    ec[i].slant += (et[i].slant - ec[i].slant) * k; ec[i].lidBot += (et[i].lidBot - ec[i].lidBot) * k;
    ec[i].pupil += (et[i].pupil - ec[i].pupil) * k;
  }
  lx += (lxT - lx) * 0.25f; ly += (lyT - ly) * 0.25f;
  eyeShift += (eyeShiftT - eyeShift) * 0.25f;

  if (now > nextLook) {
    switch (mood) {
      case M_SCAN:     lxT = (lxT > 0) ? -10 : 10; lyT = 0; nextLook = now + 450; break;
      case M_BORED:    lxT = random(0, 2) ? -9 : 9; lyT = -4; nextLook = now + random(1800, 3000); break;
      case M_SLEEPY:   lxT = 0; lyT = 3; nextLook = now + 1500; break;
      case M_SLEEP:    lxT = 0; lyT = 0; nextLook = now + 1500; break;
      case M_SAD: case M_CRY: lxT = random(-3, 4); lyT = 4; nextLook = now + 2000; break;
      case M_CONFUSED: lxT = random(0, 2) ? -8 : 8; lyT = -3; nextLook = now + random(600, 1200); break;
      case M_SURPRISED: case M_SPARKLE: lxT = random(-3, 4); lyT = random(-2, 3); nextLook = now + 1500; break;
      case M_SUSPICIOUS: lxT = random(0, 2) ? -9 : 9; lyT = 0; nextLook = now + random(1500, 2800); break;
      default: lxT = random(-9, 10); lyT = random(-5, 6); nextLook = now + random(1200, 3500); break;
    }
  }

  if (!blinking && now > nextBlink && mood != M_SLEEP) {
    int r = random(100);
    startBlink(r < 20 ? 2 : 1, random(100) < 12 ? 340 : 170);
  }
  if (blinking) {
    float t = (now - blinkStart) / (float)blinkDur;
    if (t < 0) { blinkF = 1; }
    else {
      if (t >= 0.5f && hasPending) { applyMood(pendingMood); hasPending = false; }
      if (t >= 1) {
        if (blinkLeft > 1) { blinkLeft--; blinkStart = now + 70; blinkF = 1; }
        else { blinking = false; blinkF = 1; nextBlink = now + random(2000, 5500); }
      } else blinkF = max(0.06f, fabsf(1 - 2 * t));
    }
  }
}

void startGreeting() {
  setMood(M_HAPPY);
  greetUntil = millis() + 4500;
  moodUntil = greetUntil + 1500;
  eyeShiftT = -8;
  lxT = 0; lyT = 0;
  happy = min(100.0f, happy + 8);
  soundReq = SND_HELLO;
  asleep = false;
}

void autoMood() {
  static const Mood good[] = {M_NORMAL, M_NORMAL, M_NORMAL, M_HAPPY, M_HAPPY, M_SURPRISED, M_WINK,
                              M_SUSPICIOUS, M_BORED, M_CONFUSED, M_SQUINT, M_SPARKLE, M_DIZZY,
                              M_SMUG, M_SCAN, M_SLEEPY, M_ANGRY, M_SAD, M_KO};
  static const Mood bad[]  = {M_SAD, M_CRY, M_SAD, M_SLEEPY, M_BORED, M_ANGRY, M_RAGE, M_NORMAL};
  Mood m;
  do {
    if (happy < 30) m = bad[random(0, (int)(sizeof(bad) / sizeof(bad[0])))];
    else            m = good[random(0, (int)(sizeof(good) / sizeof(good[0])))];
  } while (m == targetMood());
  setMood(m);
  bool shortOne = (m == M_DIZZY || m == M_SQUINT || m == M_SCAN || m == M_KO || m == M_RAGE || m == M_SURPRISED);
  moodUntil = millis() + (shortOne ? random(2200, 3800) : random(3500, 8000));
}

void centerText(const char *s, int y, int size) {
  int16_t x1, y1; uint16_t tw, th;
  display.setTextSize(size);
  display.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  display.setCursor((128 - tw) / 2, y);
  display.print(s);
}

void saveAll() {
  prefs.putInt("hiJump", hiJump);
  prefs.putInt("react", bestReact);
  prefs.putInt("vol", vol);
  prefs.putFloat("happy", happy);
  dirty = false;
}

uint32_t lastDecay = 0;
void runEyes() {
  uint32_t now = millis();

  if (pressEdge) {
    lastAct = now;
    if (asleep) { asleep = false; setMood(M_SURPRISED); moodUntil = now + 1500; soundReq = SND_TAP; }
  }
  if (wakeFlag) { wakeFlag = false; lastAct = now; startGreeting(); }
  if (loudFlag) { loudFlag = false; lastAct = now; if (!asleep || true) { asleep = false; setMood(M_SURPRISED); moodUntil = now + 1500; } }

  if (dblEvt) { saveAll(); state = S_MENU; return; }

  if (tapEvt && now > greetUntil) {                      
    const Mood r[] = {M_HAPPY, M_SURPRISED, M_WINK, M_SPARKLE, M_SQUINT, M_CONFUSED, M_DIZZY, M_SMUG};
    setMood(r[random(0, 8)]);
    moodUntil = now + 2500;
    happy = min(100.0f, happy + 4); dirty = true;
    soundReq = SND_TAP;
  }
  if (holding && now > greetUntil) {                
    if (targetMood() != M_SMUG) { setMood(M_SMUG); soundReq = SND_HAPPY; }
    moodUntil = now + 2000;
    happy = min(100.0f, happy + 0.05f); dirty = true;
    lastAct = now;
  }

  if (now - lastDecay > 20000) {                       
    lastDecay = now;
    if (happy > 60) happy -= 1; else if (happy < 60) happy += 1;
    dirty = true;
  }

  bool greeting = now < greetUntil;
  if (!greeting) eyeShiftT = 0;

  if (AUTO_SLEEP_MS > 0 && !asleep && now - lastAct > AUTO_SLEEP_MS) { asleep = true; setMood(M_SLEEP); }
  if (!asleep && !greeting && !holding && now > moodUntil) autoMood();

  updateEyeAnim();
  drawFace(greeting);
}

int menuIdx = 0;
const int MENU_N = 6;
void enterGame(State s);

void runMenu() {
  if (shortRel) { menuIdx = (menuIdx + 1) % MENU_N; soundReq = SND_TAP; }
  if (longRel) {
    soundReq = SND_SELECT;
    switch (menuIdx) {
      case 0: enterGame(S_JUMP); return;
      case 1: enterGame(S_REACT); return;
      case 2: enterGame(S_MIC); return;
      case 3: state = S_STATUS; return;
      case 4: { const int steps[] = {0, 3, 6, 10}; int k = 0;
                for (int i = 0; i < 4; i++) if (steps[i] == vol) k = i;
                vol = steps[(k + 1) % 4]; dirty = true; break; }
      case 5: state = S_EYES; lastAct = millis(); saveAll(); return;
    }
  }
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(W);
  char buf[24];
  const char *names[] = {"Game Lompat", "Game Refleks", "Mic Meter", "Status Pet", "Volume", "Kembali"};
  for (int i = 0; i < MENU_N; i++) {
    display.setCursor(6, 2 + i * 10);
    if (i == menuIdx) display.print("> "); else display.print("  ");
    if (i == 4) { snprintf(buf, sizeof(buf), "Volume: %d", vol); display.print(buf); }
    else display.print(names[i]);
  }
}

// ============================================================
//  GAME 1: LOMPAT
// ============================================================
#define GROUND 56
float pY, pVy; bool onGround, jAlive; float jScore, jSpeed;
float obX[2]; int obH[2]; int obW[2];

void jumpReset() {
  pY = GROUND - 8; pVy = 0; onGround = true; jAlive = true; jScore = 0; jSpeed = 3.0f;
  obX[0] = 150; obX[1] = 260;
  for (int i = 0; i < 2; i++) { obH[i] = random(8, 17); obW[i] = random(5, 9); }
}

void runJump() {
  if (longRel) { state = S_MENU; saveAll(); return; }
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(W);

  if (jAlive) {
    if (pressEdge && onGround) { pVy = -6.2f; onGround = false; soundReq = SND_JUMP; }
    pVy += 0.55f; pY += pVy;
    if (pY >= GROUND - 8) { pY = GROUND - 8; pVy = 0; onGround = true; }
    jSpeed = min(5.5f, 3.0f + jScore * 0.04f);
    for (int i = 0; i < 2; i++) {
      obX[i] -= jSpeed;
      if (obX[i] + obW[i] < 0) {
        float far = max(obX[0], obX[1]);
        obX[i] = far + random(70, 111) + jSpeed * 12;
        obH[i] = random(8, 17); obW[i] = random(5, 9);
        jScore += 1;
      }

      if (obX[i] < 16 + 7 && obX[i] + obW[i] > 16 + 1 && pY + 7 > GROUND - obH[i]) {
        jAlive = false; soundReq = SND_OVER;
        if ((int)jScore > hiJump) { hiJump = (int)jScore; dirty = true; }
        happy = min(100.0f, happy + 2); saveAll();
      }
    }
  } else if (pressEdge) { jumpReset(); soundReq = SND_GO; }

  display.drawFastHLine(0, GROUND, 128, W);
  for (int i = 0; i < 2; i++) display.fillRect((int)obX[i], GROUND - obH[i], obW[i], obH[i], W);
  // pemain: kotak kecil dengan mata
  display.fillRoundRect(16, (int)pY, 8, 8, 2, W);
  display.drawPixel(21, (int)pY + 2, B);
  char b[20];
  snprintf(b, sizeof(b), "Skor %d", (int)jScore); display.setCursor(0, 0); display.print(b);
  snprintf(b, sizeof(b), "Hi %d", hiJump); display.setCursor(128 - strlen(b) * 6, 0); display.print(b);
  if (!jAlive) { centerText("GAME OVER", 22, 1); centerText("tap = ulang", 34, 1); }
}

// ============================================================
//  GAME 2: REFLEKS
// ============================================================
enum { R_IDLE, R_WAIT, R_GO, R_RESULT, R_EARLY };
int rState = R_IDLE; uint32_t rT = 0, rGoAt = 0; int rLast = 0;

void runReact() {
  if (longRel) { state = S_MENU; saveAll(); return; }
  uint32_t now = millis();
  display.clearDisplay(); display.setTextColor(W);
  char b[24];

  switch (rState) {
    case R_IDLE:
      centerText("REFLEKS", 6, 2);
      centerText("tap kalau muncul TAP!", 30, 1);
      centerText("tap untuk mulai", 46, 1);
      if (pressEdge) { rState = R_WAIT; rGoAt = now + random(1500, 5000); soundReq = SND_TAP; }
      break;
    case R_WAIT:
      centerText("tunggu...", 26, 2);
      if (pressEdge) { rState = R_EARLY; soundReq = SND_OVER; }
      else if (now > rGoAt) { rState = R_GO; rT = now; soundReq = SND_GO; }
      break;
    case R_GO:
      display.fillRect(0, 0, 128, 64, W);
      display.setTextColor(B);
      centerText("TAP!", 20, 3);
      if (pressEdge) {
        rLast = now - rT; rState = R_RESULT;
        if (bestReact == 0 || rLast < bestReact) { bestReact = rLast; dirty = true; }
        happy = min(100.0f, happy + 2); saveAll();
        soundReq = SND_HAPPY;
      }
      break;
    case R_RESULT:
      snprintf(b, sizeof(b), "%d ms", rLast); centerText(b, 10, 2);
      snprintf(b, sizeof(b), "terbaik: %d ms", bestReact); centerText(b, 34, 1);
      centerText(rLast < 250 ? "KILAT!  (^o^)" : (rLast < 400 ? "bagus  (^_^)" : "ayo lagi  (-_-)"), 46, 1);
      if (pressEdge) { rState = R_IDLE; }
      break;
    case R_EARLY:
      centerText("KECEPATAN!", 14, 2);
      centerText("tap = coba lagi", 40, 1);
      if (pressEdge) rState = R_IDLE;
      break;
  }
}

// ============================================================
//  MIC METER (juga buat kalibrasi wake word)
// ============================================================
uint8_t hist[128]; int histI = 0;
void runMic() {
  if (shortRel || longRel) { state = S_MENU; return; }
  float r = micLevel;
  float h = sqrtf(min(r, 4000.0f) / 4000.0f) * 38.0f;
  hist[histI] = (uint8_t)h; histI = (histI + 1) % 128;
  display.clearDisplay(); display.setTextColor(W); display.setTextSize(1);
  for (int x = 0; x < 128; x++) {
    int v = hist[(histI + x) % 128];
    display.drawFastVLine(x, 63 - v, v, W);
  }
  char b[32];
  snprintf(b, sizeof(b), "lvl %d  noise %d", (int)r, (int)dbgNoise); display.setCursor(0, 0); display.print(b);
  snprintf(b, sizeof(b), "ambang %d  st %d", (int)dbgThr, (int)dbgState); display.setCursor(0, 10); display.print(b);
}

// ============================================================
//  STATUS
// ============================================================
void runStatus() {
  if (shortRel || longRel) { state = S_MENU; return; }
  display.clearDisplay(); display.setTextColor(W); display.setTextSize(1);
  char b[28];
  display.setCursor(0, 0); display.print("Happy");
  display.drawRect(36, 0, 90, 8, W);
  display.fillRect(38, 2, (int)(86 * happy / 100.0f), 4, W);
  snprintf(b, sizeof(b), "Skor lompat : %d", hiJump); display.setCursor(0, 16); display.print(b);
  if (bestReact) snprintf(b, sizeof(b), "Refleks     : %dms", bestReact);
  else snprintf(b, sizeof(b), "Refleks     : -");
  display.setCursor(0, 26); display.print(b);
  snprintf(b, sizeof(b), "Volume      : %d", vol); display.setCursor(0, 36); display.print(b);
  display.setCursor(0, 46); display.print(sdOk ? "SD card     : OK" : "SD card     : tidak ada");
  display.setCursor(0, 56); display.print(sdOk && SD.exists("/hello.wav") ? "hello.wav   : ada" : "hello.wav   : jingle");
}

void enterGame(State s) {
  state = s;
  if (s == S_JUMP) jumpReset();
  if (s == S_REACT) rState = R_IDLE;
  if (s == S_MIC) memset(hist, 0, sizeof(hist));
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
  randomSeed(micros() ^ analogRead(0));

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (!display.begin(0x3C, true)) {
    Serial.println("OLED tidak ketemu (cek alamat 0x3C / kabel)");
    while (true) delay(100);
  }
  display.clearDisplay(); display.setTextColor(W);
  centerText("lilfish", 18, 2);
  centerText("hey whosh!", 44, 1);
  display.display();

  prefs.begin("whosh", false);
  hiJump = prefs.getInt("hiJump", 0);
  bestReact = prefs.getInt("react", 0);
  vol = prefs.getInt("vol", 6);
  happy = prefs.getFloat("happy", 70);

  static SPIClass sdSPI(FSPI);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI, 4000000);
  Serial.println(sdOk ? "SD OK" : "SD gagal (tidak fatal)");

  i2sInit();
  xTaskCreatePinnedToCore(audioTask, "audio", 10240, NULL, 2, NULL, 0);

  soundReq = SND_STARTUP;
  lastAct = millis();
  moodUntil = millis() + 5000;
  initEyes();
  setMood(M_HAPPY);
  delay(900);
}

uint32_t lastFrame = 0, lastSave = 0;
void loop() {
  readTouch();
  uint32_t now = millis();
  if (now - lastFrame < 33) { delay(1); return; }
  lastFrame = now;

  if (state != S_EYES) { wakeFlag = false; loudFlag = false; }

  switch (state) {
    case S_EYES:   runEyes();   break;
    case S_MENU:   runMenu();   break;
    case S_JUMP:   runJump();   break;
    case S_REACT:  runReact();  break;
    case S_MIC:    runMic();    break;
    case S_STATUS: runStatus(); break;
  }
  clearEvents();
  display.display();

  if (dirty && now - lastSave > 120000) { saveAll(); lastSave = now; }
}
