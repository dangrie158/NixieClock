#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

#include <WiFiManager.h>
#include <WiFiClient.h>

#include <NtpClientLib.h>
#include <TimeLib.h>

#include <Ticker.h>

#define VARIANT 12
#include "config.h"
#include "pins.h"

// enable uint64 support for UNIX timestamps
// and avoid the Y2038 problem
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

// disable debug messages to the serial port
#undef DEBUG
// enable debug messages in display function (very slow!!) not working really well with PWM
#undef DEBUG_DISPLAY

typedef struct
{
  /**
   * @brief dstInEffect a boolean indicating whether DST is currently active
   */
  bool dstInEffect;

  /**
   * @brief the current offset to GMT
   */
  int32_t offset;

  /**
   * @brief the currently requested data is valid until this unix timestamp (UTC)
   */
  time_t validUntil;
} TzInfo;

// The WiFiManager Object handles the auto-connect feature
WiFiManager wifiManager;
WiFiClient client;

// Actual loop rate is measuread at ~5.6kHz (177µs) with a 100µs space
// when setting a new value to the display (DEBUG set, DEBUG_DISPLAY cleared).
// With 10 bigthness levels, the PWM frequency is 1ms which corresponds to a
// 500Hz refresh rate which is absolutley invisible
const uint8_t maxBrightness = 50;

// 20% duty cycle at nighttime
const uint8_t lowBrightness = (maxBrightness / 10) * 2;

// begin of the low-brightness period
const uint8_t beginLowBrightnessHour = 21;

// end of the low-brightness period
const uint8_t endLowBrightnessHour = 6;

const String timezoneName = "Europe/Berlin";
// API Key is defined in include/config.h
// which is ignored by git to keep the key private
const String timezoneDbUrl = String("http://api.timezonedb.com/v2.1/get-time-zone?format=json&key=") + TZDB_API_KEY + String("&by=zone&zone=") + timezoneName;
// a normal response from the timezonedb has 13 objects
// required size calculated with https://arduinojson.org/v5/assistant/
const int responseCapacity = JSON_OBJECT_SIZE(13) + 260;

// The object to store the current timezone info queried at the start of
// the sketch or at runtime if the object was no longer valid
TzInfo tzInfo;

// A ticker used to display an animation on the display while connecting
// to the WiFi
Ticker connectingTicker;

// the serial stream that gets shifted out to the display
uint64_t displaySerialStream;

// current brighnesslevel of the digits
uint8_t digitBrightness = maxBrightness;

// current brightnesslevel of the colon
uint8_t ledBrightness = maxBrightness;

/**
 * @brief Get Timezone Info from the timezonedb.com API
 *
 * @param[out] out the struc containing the new timezone data
 * @return true if the request was successful and the out params are written
 * @return false otherwise
 */
bool getTzInfo(TzInfo *out)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  HTTPClient http;
  http.begin(client, timezoneDbUrl);

  // check the return code
  if (http.GET() != HTTP_CODE_OK)
  {
#ifdef DEBUG
    Serial.print("GET failed for URL ");
    Serial.println(timezoneDbUrl);
    Serial.println(http.getString());
#endif

    http.end();
    return false;
  }
  else
  {
    String response = http.getString();
    http.end();
#ifdef DEBUG
    Serial.println("GET to timezonedb.com API succeeded. Reponse:");
    Serial.println(response);
#endif
    // parse the request response payload
    StaticJsonDocument<responseCapacity> document;
    deserializeJson(document, response);

#ifdef DEBUG
    Serial.println("Parsed JSON:");
    serializeJson(document, Serial);
    Serial.println();
#endif
    // dst is a string, not a boolean
    // convert it to a boolean by string comparison
    out->dstInEffect = document["dst"].as<String>().equals("1");

    // gmtOffset is specified in seconds
    out->offset = document["gmtOffset"].as<int32_t>();

    // get the validUntil as time_t(uint64_t) as this is the
    // (potential) size of a UNIX timestamp
    out->validUntil = document["zoneEnd"].as<time_t>();

    return true;
  }
}

/**
 * @brief make sure the timezone info is up-to-date
 *
 * if the request to the timezonedb.com API fails,
 * the ESP is rebooted. This gives it the chance to
 * reconnect to the WiFi or restart in AP mode
 *
 */
void updateTimeZoneInfo()
{
  if (!getTzInfo(&tzInfo))
  {
#ifdef DEBUG
    Serial.println("Failed to get TZ Info!");
    Serial.println("Rebooting in 5 seconds");
#endif
    delay(5000);

    // Best bet is to do a clean reboot so we can reconnect
    // if we lost the WiFi connection or start the AP
    // if the credentials are no longer valid
    ESP.restart();
  }
  else
  {
    // offset is specified in seconds, convert it to
    // whole hours and seperate minutes for the NTPClient
    if (!NTP.setTimeZone(tzInfo.offset / SECS_PER_HOUR, tzInfo.offset % SECS_PER_HOUR))
    {
#ifdef DEBUG
      Serial.println("Failed to set timezone!");
#endif
    }
    else
    {
#ifdef DEBUG
      Serial.print("New Timezone offset: ");
      Serial.print(NTP.getTimeZone());
      Serial.print(" hours and ");
      Serial.print(NTP.getTimeZoneMinutes());
      Serial.println(" minutes.");
#endif
    }
  }
}

/**
 * @brief shift out the actual display data to the shift registers
 *
 * This method is seperate from setDisplay() because it needs to be fast
 * to be able to simulate a PWM with a 50% duty cycle
 */
void updateDisplay()
{
  // count the number of cycles the digits and dots are on
  static uint8_t digitPwmCounter = 0;
  static uint8_t ledPwmCounter = 0;

  // the actual stream to shift out this cycle (with potential masking)
  uint64_t maskedStream = displaySerialStream;

  // mask out the digits if the counter reaches the brightness level
  if (digitPwmCounter >= digitBrightness)
  {
    maskedStream &= ~digitsMask;
  }
  // increase the counter
  digitPwmCounter = (digitPwmCounter + 1) % (maxBrightness + 1);

  // mask out the digits if the counter reaches the brightness level
  if (ledPwmCounter >= ledBrightness)
  {
    maskedStream &= ~ledsMask;
  }
  // increase the counter
  ledPwmCounter = (ledPwmCounter + 1) % (maxBrightness + 1);

  // set latch pin low to avoid displaying the shifting of data
  digitalWrite(latchPin, LOW);

  // shift out the data big-endian (MSByte and MSBit) first
  // (network order)
  for (int8_t chip = numChips - 1; chip >= 0; chip--)
  {
    uint8_t dataByte = (maskedStream >> (chip * 8)) & 0xFF;
    shiftOut(dataPin, clockPin, MSBFIRST, dataByte);
#ifdef DEBUG_DISPLAY
    Serial.print("Chip ");
    Serial.print(chip);
    Serial.print(" set to state ");
    Serial.println(String(dataByte, BIN));
#endif
  }

  // latch the temp register into the output
  digitalWrite(latchPin, HIGH);
}

/**
 * @brief Display the passed time on the nixie display
 * Shifts out the bit pattern to display the passed
 * time digits on the nixie display
 *
 * @param time time to display
 */
void setDisplay(tmElements_t time, uint8_t dots = 0x00)
{
  // reset the display stream
  displaySerialStream = 0x00;

  // set the 4 digits
  displaySerialStream |= 1ull << digitsPinmap[0][time.Hour / 10];
  displaySerialStream |= 1ull << digitsPinmap[1][time.Hour % 10];
  displaySerialStream |= 1ull << digitsPinmap[2][time.Minute / 10];
  displaySerialStream |= 1ull << digitsPinmap[3][time.Minute % 10];

  // set the outputs for the passed dot-state
  for (uint8_t dot = 0; dot < numDigits; dot++)
  {
    if ((dots >> dot) & 0b1)
    {
      displaySerialStream |= 1ull << dotsPinmap[dot];
    }
  }

  // blink the LED seperator every other second
  if (time.Second % 2)
  {
    displaySerialStream |= (1ull << ledsPinmap[0]);
    displaySerialStream |= (1ull << ledsPinmap[1]);
  }

#ifdef DEBUG
  Serial.print("setting display to ");
  Serial.print(time.Hour);
  Serial.print(time.Second % 2 ? ":" : " ");
  Serial.println(String(time.Minute));
#endif

  // make sure the updated display state is actually displayed
  updateDisplay();
}

void displayPasscode(char *apPasscode)
{
  static uint8_t currentDot = 0;
  static bool direction = true;

  // increment the currently lit dot in the current "direction"
  direction ? currentDot++ : currentDot--;

  // change direction if we hit the first or last digit
  if (currentDot == numDigits - 1 || currentDot == 0)
  {
    direction = !direction;
  }

  // convert the passcode into a tmElements_t
  tmElements_t passcodeTime = {0,
                               (uint8_t)((apPasscode[2] - '0') * 10 + (apPasscode[3] - '0')),
                               (uint8_t)((apPasscode[0] - '0') * 10 + (apPasscode[1] - '0'))};

  // update the display
  uint8_t dotState = 0x01 << currentDot;
  setDisplay(passcodeTime, dotState);
}

void displayTest()
{
  // count up, colon off, dots alternate
  for (int8_t i = 0; i < 10; i++)
  {
    uint8_t doubleDigit = (i * 10) + i;
    tmElements_t testDisplay = {0, doubleDigit, doubleDigit};
    setDisplay(testDisplay, i % 2 ? 0b1111 : 0b0000);
    delay(300);
  }

  // count down, colon on, dots alternate
  for (int8_t i = 9; i >= 0; i--)
  {
    uint8_t doubleDigit = (i * 10) + i;
    tmElements_t testDisplay = {1, doubleDigit, doubleDigit};
    setDisplay(testDisplay, i % 2 ? 0b1111 : 0b0000);
    delay(300);
  }
}

void setup()
{
  // setup GPIO data directions
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);

#ifdef DEBUG
  // setup the debug serial port
  Serial.begin(115200);
#endif

  // set the outputs to a known state ASAP
  displayTest();

  // auto-manage wifi configuration

  // create a random numeric 4-digit password
  String apPasscode = String(random(1000, 10000), DEC);

#ifdef DEBUG
  Serial.print("Random accesspoint password is: ");
  Serial.println(apPasscode);
#endif

  // start to display the connecting animation and passcode
  connectingTicker.attach(0.5, displayPasscode, const_cast<char *>(apPasscode.c_str()));
  if (!MDNS.begin("nixie"))
  {
#ifdef DEBUG
    Serial.println("Error setting up MDNS responder!");
#endif
  }

  // repeat the passcode twice to get the 8 char minimum length
  wifiManager.autoConnect("Nixie Clock", (apPasscode + apPasscode).c_str());
  connectingTicker.detach();

  NTP.begin("pool.ntp.org");

  // initially get the timezone info
  updateTimeZoneInfo();

  NTP.setInterval(63);
}

void loop()
{
  static time_t lastUpdate = 0;

  time_t currentTime = now();

  // a second passed, update all the data and display
  if (currentTime != lastUpdate)
  {
    // make sure the display is always on before entering this (slow) loop
    // so visible lanking of the display is avoided
    // activate maximum brightness to ensure a display update
    // will turn on the display and then update the display
    digitBrightness = maxBrightness;
    ledBrightness = maxBrightness;
    updateDisplay();

#ifdef DEBUG
    Serial.print("current time: ");
    Serial.println(NTP.getTimeDateString());
#endif

    // handle updates to the timezone info if it is no longer valid
    if (tzInfo.validUntil < currentTime)
    {
#ifdef DEBUG
      Serial.println("TZ Info no longer valid, updating...");
#endif

      updateTimeZoneInfo();
    }
#ifdef DEBUG
    else
    {
      Serial.print("TZ Info still valid for ");
      Serial.print(tzInfo.validUntil - currentTime);
      Serial.print(" seconds or until: ");
      Serial.println(NTP.getTimeDateString(tzInfo.validUntil));

      Serial.print("current offset: ");
      Serial.print(tzInfo.offset);
      Serial.print(" seconds, ");
      Serial.println(tzInfo.dstInEffect ? "DST active" : "DST inactive");
    }
#endif

    tmElements_t timeElements;
    breakTime(currentTime, timeElements);
    setDisplay(timeElements);

    lastUpdate = currentTime;

    // set the brightness depending on the current time.
    if (timeElements.Hour >= beginLowBrightnessHour || timeElements.Hour < endLowBrightnessHour)
    {
      // set low-brightness mode
      ledBrightness = lowBrightness;
      digitBrightness = lowBrightness;
    }
    else
    {
      // clear low-brightness mode
      ledBrightness = maxBrightness;
      digitBrightness = maxBrightness;
    }
  }

  // update the display every cycle to be as fast as possible
  // this should not be done with a ticker as the minimum resolution is
  // 1ms, however an update only takes <=200µs, thus giving a better
  // resolution for PWM mode.
  updateDisplay();
}
