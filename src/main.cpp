/*
  ============================================================
  QUILL CLOCK v1.6
  ============================================================

  Hardware:
    ESP32 Dev Module / ESP32-WROOM-32
    GC9A01 240x240 round TFT

  Display wiring:
    GC9A01       ESP32
    -------------------
    GND          GND
    VCC          3V3
    SCL          GPIO18
    SDA          GPIO23
    RES          GPIO4
    DC           GPIO2
    CS           GPIO5
    BLK          3V3

  Features:
    - Non-blocking Wi-Fi/NTP synchronisation at startup
    - Startup remains responsive if Wi-Fi/NTP is unavailable
    - Automatic startup retry after failed Wi-Fi/NTP attempt
    - NZ timezone with automatic NZST/NZDT
    - Smooth second hand: ~18.18 Hz
    - Minute hand: every 10 seconds
    - Hour hand: every 60 seconds
    - Date repaired after sweep hand crosses it
    - Automatic midnight NTP resynchronisation
    - Midnight NTP sync runs non-blocking
    - Clock continues if Wi-Fi is unavailable
    - Wi-Fi switched off when not required
    - No framebuffer

  ============================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>

#include "secrets.h"

// ============================================================
// Wi-Fi
// ============================================================

// Insert your existing working Wi-Fi credentials.
//const char* WIFI_SSID     = "";
//const char* WIFI_PASSWORD = "";


// ============================================================
// GC9A01 wiring
// ============================================================

constexpr int TFT_CS   = 5;
constexpr int TFT_DC   = 2;
constexpr int TFT_RST  = 4;
constexpr int TFT_SCLK = 18;
constexpr int TFT_MOSI = 23;


Adafruit_GC9A01A tft(
  &SPI,
  TFT_DC,
  TFT_CS,
  TFT_RST
);


// ============================================================
// Display geometry
// ============================================================

constexpr int CX = 120;
constexpr int CY = 120;


// ============================================================
// Hand geometry
// ============================================================

constexpr int HOUR_HAND_LEN   = 54;
constexpr int MINUTE_HAND_LEN = 75;
constexpr int SECOND_HAND_LEN = 82;


// ============================================================
// Update rates
// ============================================================

constexpr uint32_t SECOND_UPDATE_MS = 55;       // ~18.18 Hz
constexpr uint32_t MINUTE_UPDATE_MS = 5000;    // 10 seconds
constexpr uint32_t HOUR_UPDATE_MS   = 60000;    // 1 minute


// ============================================================
// Hand state
// ============================================================

float oldHourAngle   = 0.0f;
float oldMinuteAngle = 0.0f;
float oldSecondAngle = 0.0f;

bool handsInitialised = false;

int oldDay = -1;


// ============================================================
// Background NTP state
// ============================================================

enum NtpSyncState
{
  NTP_IDLE,
  NTP_CONNECTING,
  NTP_WAITING
};


NtpSyncState ntpState = NTP_IDLE;


// Time when current background operation began.
uint32_t ntpStartMillis = 0;


// Set by the ESP32 SNTP callback.
volatile bool ntpSyncComplete = false;


// Calendar day on which the most recent midnight
// sync attempt was started.
int lastNtpAttemptDay = -1;


// Wi-Fi connection timeout.
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;


// NTP response timeout once Wi-Fi is connected.
constexpr uint32_t NTP_TIMEOUT_MS = 15000;

// Retry interval while the clock is still waiting for its first valid time.
constexpr uint32_t STARTUP_RETRY_MS = 30000;

// True once valid time has been obtained and the analogue face is running.
bool clockRunning = false;

// True while the initial boot-time synchronisation is being attempted.
bool startupSync = true;


// Forward declarations
void startNtpSync(bool isStartup);

// Used to schedule another startup attempt without blocking the display.
uint32_t startupRetryMillis = 0;


// ============================================================
// SNTP callback
//
// Called automatically when ESP32 receives an NTP update.
// Keep this routine very short.
// ============================================================

void timeSyncCallback(
  struct timeval* tv)
{
  ntpSyncComplete = true;
}


// ============================================================
// Draw one hand
// ============================================================

void drawHand(
  float angleDeg,
  int length,
  int thickness,
  uint16_t colour)
{
  float angle =
    (angleDeg - 90.0f)
    * DEG_TO_RAD;


  int x =
    CX + round(
      cos(angle) * length);


  int y =
    CY + round(
      sin(angle) * length);


  // Centre line.
  tft.drawLine(
    CX,
    CY,
    x,
    y,
    colour);


  // Second hand is single pixel.
  if (thickness == 0)
    return;


  // Perpendicular angle used to
  // create hand thickness.
  float perpendicular =
    angle + PI / 2.0f;


  for (int i = 1; i <= thickness; i++)
  {
    int dx =
      round(
        cos(perpendicular) * i);


    int dy =
      round(
        sin(perpendicular) * i);


    tft.drawLine(
      CX + dx,
      CY + dy,
      x + dx,
      y + dy,
      colour);


    tft.drawLine(
      CX - dx,
      CY - dy,
      x - dx,
      y - dy,
      colour);
  }
}


// ============================================================
// Hand-angle calculations
// ============================================================

float calcHourAngle(
  int hour,
  int minute)
{
  return
    ((hour % 12)
      + minute / 60.0f)
    * 30.0f;
}


float calcMinuteAngle(
  int minute,
  int second)
{
  return
    (minute + second / 60.0f)
    * 6.0f;
}


float calcSecondAngle(
  int second,
  uint32_t milliseconds)
{
  return
    (second + milliseconds / 1000.0f)
    * 6.0f;
}


// ============================================================
// Static clock face
// ============================================================

void drawStaticFace()
{
  tft.fillScreen(
    GC9A01A_BLACK);


  // ----------------------------------------------------------
  // Outer rings
  // ----------------------------------------------------------

  tft.drawCircle(
    CX,
    CY,
    118,
    GC9A01A_WHITE);


  tft.drawCircle(
    CX,
    CY,
    116,
    GC9A01A_WHITE);


  // ----------------------------------------------------------
  // 60 tick marks
  // ----------------------------------------------------------

  for (int i = 0; i < 60; i++)
  {
    float angle =
      (i * 6.0f - 90.0f)
      * DEG_TO_RAD;


    int outerR = 108;


    int innerR =
      (i % 5 == 0)
      ? 94
      : 102;


    int x1 =
      CX + cos(angle) * innerR;


    int y1 =
      CY + sin(angle) * innerR;


    int x2 =
      CX + cos(angle) * outerR;


    int y2 =
      CY + sin(angle) * outerR;


    uint16_t colour =
      (i % 5 == 0)
      ? GC9A01A_WHITE
      : 0x7BEF;


    tft.drawLine(
      x1,
      y1,
      x2,
      y2,
      colour);
  }


  // ----------------------------------------------------------
  // Numerals
  // ----------------------------------------------------------

  tft.setTextColor(
    GC9A01A_WHITE,
    GC9A01A_BLACK);


  tft.setTextSize(2);


  tft.setCursor(109, 20);
  tft.print("12");


  tft.setCursor(207, 113);
  tft.print("3");


  tft.setCursor(114, 204);
  tft.print("6");


  tft.setCursor(21, 113);
  tft.print("9");
}


// ============================================================
// Date
// ============================================================

void drawDate(
  const struct tm* timeinfo)
{
  char buffer[20];


  strftime(
    buffer,
    sizeof(buffer),
    "%d %b %Y",
    timeinfo);


  tft.setTextSize(1);


  tft.setTextColor(
    0xBDF7,
    GC9A01A_BLACK);


  int16_t x1;
  int16_t y1;

  uint16_t w;
  uint16_t h;


  tft.getTextBounds(
    buffer,
    0,
    0,
    &x1,
    &y1,
    &w,
    &h);


  tft.setCursor(
    CX - w / 2,
    165);


  tft.print(buffer);
}


// ============================================================
// Draw white hands
// ============================================================

void drawWhiteHands()
{
  // Hour
  drawHand(
    oldHourAngle,
    HOUR_HAND_LEN,
    3,
    GC9A01A_WHITE);


  // Minute
  drawHand(
    oldMinuteAngle,
    MINUTE_HAND_LEN,
    2,
    GC9A01A_WHITE);
}


// ============================================================
// Centre pivot
// ============================================================

void drawPivot()
{
  tft.fillCircle(
    CX,
    CY,
    5,
    GC9A01A_RED);


  tft.fillCircle(
    CX,
    CY,
    2,
    GC9A01A_WHITE);
}


// ============================================================
// Clock update engine
// ============================================================

void updateClock()
{
  static uint32_t lastMinuteUpdate = 0;
  static uint32_t lastHourUpdate   = 0;


  uint32_t nowMillis =
    millis();


  // ----------------------------------------------------------
  // Current system time
  // ----------------------------------------------------------

  struct timeval tv;


  gettimeofday(
    &tv,
    nullptr);


  time_t now =
    tv.tv_sec;


  struct tm timeinfo;


  localtime_r(
    &now,
    &timeinfo);


  uint32_t milliseconds =
    tv.tv_usec / 1000;


  int hour =
    timeinfo.tm_hour;


  int minute =
    timeinfo.tm_min;


  int second =
    timeinfo.tm_sec;


  // ==========================================================
  // Initial frame
  // ==========================================================

  if (!handsInitialised)
  {
    oldHourAngle =
      calcHourAngle(
        hour,
        minute);


    oldMinuteAngle =
      calcMinuteAngle(
        minute,
        second);


    oldSecondAngle =
      calcSecondAngle(
        second,
        milliseconds);


    drawWhiteHands();


    drawHand(
      oldSecondAngle,
      SECOND_HAND_LEN,
      0,
      GC9A01A_RED);


    drawPivot();


    drawDate(
      &timeinfo);


    oldDay =
      timeinfo.tm_mday;


    handsInitialised = true;


    lastMinuteUpdate =
      nowMillis;


    lastHourUpdate =
      nowMillis;


    return;
  }


  // ==========================================================
  // Date change
  // ==========================================================

  if (timeinfo.tm_mday != oldDay)
  {
    drawDate(
      &timeinfo);


    oldDay =
      timeinfo.tm_mday;
  }


  // ==========================================================
  // Midnight NTP trigger
  //
  // Any display update during the first 10 seconds after
  // midnight can start the sync.
  //
  // lastNtpAttemptDay prevents repeated attempts.
  // ==========================================================

  if (
    hour == 0 &&
    minute == 0 &&
    second < 10 &&
    lastNtpAttemptDay != timeinfo.tm_yday)
  {
    lastNtpAttemptDay =
      timeinfo.tm_yday;


    if (ntpState == NTP_IDLE)
    {
      Serial.println(
        "Midnight NTP sync requested.");


      startNtpSync(
        false);
    }
  }


  // ==========================================================
  // Hour hand
  // ==========================================================

  if (
    nowMillis - lastHourUpdate
    >= HOUR_UPDATE_MS)
  {
    lastHourUpdate +=
      HOUR_UPDATE_MS;


    // Erase old hand.
    drawHand(
      oldHourAngle,
      HOUR_HAND_LEN,
      3,
      GC9A01A_BLACK);


    // Calculate new position.
    oldHourAngle =
      calcHourAngle(
        hour,
        minute);
  }


  // ==========================================================
  // Minute hand
  //
  // Every 10 seconds
  // ==========================================================

  if (
    nowMillis - lastMinuteUpdate
    >= MINUTE_UPDATE_MS)
  {
    lastMinuteUpdate +=
      MINUTE_UPDATE_MS;


    // Erase old hand.
    drawHand(
      oldMinuteAngle,
      MINUTE_HAND_LEN,
      2,
      GC9A01A_BLACK);


    // Calculate new position.
    oldMinuteAngle =
      calcMinuteAngle(
        minute,
        second);
  }


  // ==========================================================
  // Second hand
  // ==========================================================


  // Erase previous sweep position.
  drawHand(
    oldSecondAngle,
    SECOND_HAND_LEN,
    0,
    GC9A01A_BLACK);


  // Calculate new position.
  oldSecondAngle =
    calcSecondAngle(
      second,
      milliseconds);


  // Repair anything erased by the
  // previous second-hand position.
  drawWhiteHands();


  // Repair date.
  drawDate(
    &timeinfo);


  // Draw new sweep position.
  drawHand(
    oldSecondAngle,
    SECOND_HAND_LEN,
    0,
    GC9A01A_RED);


  // Restore centre.
  drawPivot();
}


// ============================================================
// Start a Wi-Fi / NTP synchronisation
//
// This routine never waits.  It merely starts Wi-Fi and returns
// immediately to loop().  serviceBackgroundNtp() advances the
// state machine.
// ============================================================

void startNtpSync(
  bool isStartup)
{
  if (ntpState != NTP_IDLE)
    return;

  startupSync = isStartup;

  ntpSyncComplete = false;

  WiFi.mode(
    WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD);

  ntpStartMillis =
    millis();

  ntpState =
    NTP_CONNECTING;

  if (isStartup)
  {
    Serial.println(
      "Startup Wi-Fi/NTP sync started in background.");
  }
  else
  {
    Serial.println(
      "Midnight Wi-Fi/NTP sync started in background.");
  }
}


// ============================================================
// Background Wi-Fi / NTP service
//
// Used for both startup and midnight resynchronisation.
// There are no blocking loops here.
// ============================================================

void serviceBackgroundNtp()
{
  // ----------------------------------------------------------
  // If startup has not yet obtained valid time, periodically
  // begin another attempt.
  // ----------------------------------------------------------

  if (
    !clockRunning &&
    ntpState == NTP_IDLE &&
    millis() - startupRetryMillis >= STARTUP_RETRY_MS)
  {
    startNtpSync(true);
  }


  switch (ntpState)
  {
    case NTP_IDLE:
      return;


    // --------------------------------------------------------
    // Waiting for Wi-Fi
    // --------------------------------------------------------

    case NTP_CONNECTING:
    {
      if (
        WiFi.status()
        == WL_CONNECTED)
      {
        Serial.println(
          startupSync
          ? "Startup Wi-Fi connected."
          : "Midnight Wi-Fi connected.");


        ntpSyncComplete =
          false;


        sntp_set_time_sync_notification_cb(
          timeSyncCallback);


        configTzTime(
          TIMEZONE,
          "pool.ntp.org",
          "time.google.com",
          "time.cloudflare.com");


        ntpStartMillis =
          millis();


        ntpState =
          NTP_WAITING;
      }


      else if (
        millis() - ntpStartMillis
        >= WIFI_TIMEOUT_MS)
      {
        Serial.println(
          startupSync
          ? "Startup Wi-Fi unavailable."
          : "Midnight Wi-Fi unavailable.");


        WiFi.disconnect(
          true);


        WiFi.mode(
          WIFI_OFF);


        ntpState =
          NTP_IDLE;


        if (startupSync && !clockRunning)
        {
          startupRetryMillis =
            millis();

          Serial.println(
            "Clock waiting for time; retry scheduled.");
        }
        else
        {
          Serial.println(
            "Clock continuing without sync.");
        }
      }


      break;
    }


    // --------------------------------------------------------
    // Wi-Fi connected; waiting for SNTP callback
    // --------------------------------------------------------

    case NTP_WAITING:
    {
      if (ntpSyncComplete)
      {
        Serial.println(
          startupSync
          ? "Startup NTP sync complete."
          : "Midnight NTP sync complete.");


        ntpSyncComplete =
          false;


        WiFi.disconnect(
          true);


        WiFi.mode(
          WIFI_OFF);


        ntpState =
          NTP_IDLE;


        // First valid time: replace splash with the analogue face.
        if (!clockRunning)
        {
          drawStaticFace();

          handsInitialised =
            false;

          updateClock();

          clockRunning =
            true;

          Serial.println(
            "Analogue clock started.");
        }
      }


      else if (
        millis() - ntpStartMillis
        >= NTP_TIMEOUT_MS)
      {
        Serial.println(
          startupSync
          ? "Startup NTP timeout."
          : "Midnight NTP timeout.");


        WiFi.disconnect(
          true);


        WiFi.mode(
          WIFI_OFF);


        ntpState =
          NTP_IDLE;


        if (startupSync && !clockRunning)
        {
          startupRetryMillis =
            millis();

          Serial.println(
            "Clock waiting for time; retry scheduled.");
        }
        else
        {
          Serial.println(
            "Clock continuing without sync.");
        }
      }


      break;
    }
  }
}


// ============================================================
// Setup
// ============================================================

void setup()
{
  Serial.begin(
    115200);


  delay(500);


  Serial.println();

  Serial.println(
    "QUILL CLOCK v1.6");


  // ----------------------------------------------------------
  // TFT pins
  // ----------------------------------------------------------

  pinMode(
    TFT_CS,
    OUTPUT);


  pinMode(
    TFT_DC,
    OUTPUT);


  pinMode(
    TFT_RST,
    OUTPUT);


  digitalWrite(
    TFT_CS,
    HIGH);


  digitalWrite(
    TFT_DC,
    HIGH);


  // ----------------------------------------------------------
  // Display hardware reset
  // ----------------------------------------------------------

  digitalWrite(
    TFT_RST,
    LOW);


  delay(100);


  digitalWrite(
    TFT_RST,
    HIGH);


  delay(200);


  // ----------------------------------------------------------
  // Hardware SPI
  // ----------------------------------------------------------

  SPI.begin(
    TFT_SCLK,
    -1,
    TFT_MOSI,
    TFT_CS);


  // ----------------------------------------------------------
  // GC9A01
  // ----------------------------------------------------------

  tft.begin();


  tft.setRotation(0);


  tft.fillScreen(
    GC9A01A_BLACK);


  // ----------------------------------------------------------
  // Splash
  // ----------------------------------------------------------

  tft.setTextColor(
    GC9A01A_WHITE);


  tft.setTextSize(2);


  tft.setCursor(
    72,
    92);


  tft.print(
    "QUILL");


  tft.setTextSize(1);


  tft.setCursor(
    72,
    122);


  tft.print(
    "Clock v1.6");


  // ----------------------------------------------------------
  // Initial NTP synchronisation
  //
  // Non-blocking: leave the QUILL splash visible while Wi-Fi
  // and SNTP work in the background.
  // ----------------------------------------------------------

  sntp_set_time_sync_notification_cb(
    timeSyncCallback);


  startupRetryMillis =
    millis();


  startNtpSync(
    true);


  // ----------------------------------------------------------
  // Diagnostics
  // ----------------------------------------------------------

  Serial.print(
    "Free heap: ");


  Serial.println(
    ESP.getFreeHeap());


  Serial.print(
    "Largest block: ");


  Serial.println(
    ESP.getMaxAllocHeap());


  Serial.println(
    "Clock running.");
}


// ============================================================
// Main loop
// ============================================================

void loop()
{
  static uint32_t lastDisplayUpdate = 0;


  uint32_t now =
    millis();


  // ----------------------------------------------------------
  // Clock renderer
  // ----------------------------------------------------------

  if (
    clockRunning &&
    now - lastDisplayUpdate
    >= SECOND_UPDATE_MS)
  {
    lastDisplayUpdate +=
      SECOND_UPDATE_MS;


    updateClock();
  }


  // ----------------------------------------------------------
  // Background Wi-Fi / NTP state machine
  //
  // Returns immediately; does not stop display rendering.
  // ----------------------------------------------------------

  serviceBackgroundNtp();


  // Yield to ESP32 system tasks.
  delay(1);
}