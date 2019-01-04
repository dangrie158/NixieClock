#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

#include <WiFiManager.h>

#include <NtpClientLib.h>
#include <TimeLib.h>

#include <Ticker.h>

#include "config.h"

// enable uint64 support for UNIX timestamps
// and avoid the Y2038 problem
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

// enable debug messages to the serial port
#define DEBUG

// number of HV shift registers on the serial data bus
#define NUM_CHIPS 6

// number of digit tubes in the clock
#define NUM_DIGITS 4

// convert a chip and pin number to the corresponding bit
// in the serial output stream of the shift registers
// data is stored in host order (native)
#define P2B(CHIP, PIN) ((((CHIP)-1) * 8) + (PIN))

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

// latch pin of TPIC6B595 connected to D1
const uint8_t latchPin = 5;
// clock pin of TPIC6B595 connected to D2
const uint8_t clockPin = 4;
// data pin of TPIC6B595 connected to D3
const uint8_t dataPin = 0;

// the mapping of [digit][number to display] to bitnumber in the serial output stream
const uint8_t digitsPinmap[NUM_DIGITS][10]{
    {P2B(1, 5), P2B(1, 3), P2B(1, 2), P2B(1, 1), P2B(1, 0), P2B(1, 7), P2B(1, 6), P2B(2, 6), P2B(2, 5), P2B(2, 4)},
    {P2B(3, 4), P2B(2, 3), P2B(2, 2), P2B(2, 1), P2B(2, 7), P2B(2, 0), P2B(3, 0), P2B(3, 1), P2B(3, 2), P2B(3, 3)},
    {P2B(5, 7), P2B(4, 4), P2B(4, 3), P2B(4, 5), P2B(4, 2), P2B(4, 6), P2B(4, 1), P2B(4, 7), P2B(4, 0), P2B(5, 1)},
    {P2B(6, 0), P2B(5, 4), P2B(5, 3), P2B(5, 5), P2B(5, 6), P2B(5, 2), P2B(6, 5), P2B(6, 3), P2B(6, 2), P2B(6, 1)}};

// mapping of [led number] to bit in the serial output stream
const uint8_t ledsPinmap[2] = {P2B(3, 6), P2B(3, 7)};

// mapping of [number digit dot] to bit in the serial output stream
const uint8_t dotsPinmap[NUM_DIGITS] = {P2B(1, 4), P2B(3, 5), P2B(5, 0), P2B(6, 4)};

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
  http.begin(timezoneDbUrl);

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
    StaticJsonBuffer<responseCapacity> jsonBuffer;
    JsonObject &json = jsonBuffer.parseObject(response);

#ifdef DEBUG
    Serial.println("Parsed JSON:");
    json.printTo(Serial);
    Serial.println();
#endif
    // dst is a string, not a boolean
    // convert it to a boolean by string comparison
    out->dstInEffect = json.get<String>("dst").equals("1");

    // gmtOffset is specified in seconds
    out->offset = json.get<int32_t>("gmtOffset");

    // get the validUntil as time_t(uint64_t) as this is the
    // (potential) size of a UNIX timestamp
    out->validUntil = json.get<time_t>("zoneEnd");

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
    }else{
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
 * @brief Display the passed time on the nixie display
 * Shifts out the bit pattern to display the passed 
 * time digits on the nixie display
 * 
 * @param time time to display 
 */
void display(tmElements_t time, uint8_t dots = 0x00)
{
  uint64_t serialStream = 0;

  serialStream |= 1ull << digitsPinmap[0][time.Hour / 10];
  serialStream |= 1ull << digitsPinmap[1][time.Hour % 10];
  serialStream |= 1ull << digitsPinmap[2][time.Minute / 10];
  serialStream |= 1ull << digitsPinmap[3][time.Minute % 10];

  // set the outputs for the passed dot-state
  for (uint8_t dot = 0; dot < NUM_DIGITS; dot++)
  {
    if ((dots >> dot) & 0b1)
    {
      serialStream |= 1ull << dotsPinmap[dot];
    }
  }

  // blink the LED seperator every other second
  if (time.Second % 2)
  {
    serialStream |= (1ull << ledsPinmap[0]);
    serialStream |= (1ull << ledsPinmap[1]);
  }

#ifdef DEBUG
  Serial.print("Displaying ");
  Serial.print(time.Hour);
  Serial.print(time.Second % 2 ? ":" : " ");
  Serial.println(String(time.Minute));
#endif

  // set latch pin low to avoid displaying the shifting of data
  digitalWrite(latchPin, LOW);

  // shift out the data big-endian (MSByte and MSBit) first
  // (network order)
  for (int8_t chip = NUM_CHIPS - 1; chip >= 0; chip--)
  {
    uint8_t dataByte = (serialStream >> (chip * 8)) & 0xFF;
    shiftOut(dataPin, clockPin, MSBFIRST, dataByte);
#ifdef DEBUG
    Serial.print("Chip ");
    Serial.print(chip);
    Serial.print(" set to state ");
    Serial.println(String(dataByte, BIN));
#endif
  }

  // latch the temp register into the output
  digitalWrite(latchPin, HIGH);
}

void displayPasscode(const char *apPasscode)
{
  static uint8_t currentDot = 0;
  static bool direction = true;

  // increment the currently lit dot in the current "direction"
  direction ? currentDot++ : currentDot--;

  //change direction if we hit the first or last digit
  if (currentDot == NUM_DIGITS - 1 || currentDot == 0)
  {
    direction = !direction;
  }

  //convert the passcode into a tmElements_t
  tmElements_t passcodeTime = {0,
                               (uint8_t)((apPasscode[2] - '0') * 10 + (apPasscode[3] - '0')),
                               (uint8_t)((apPasscode[0] - '0') * 10 + (apPasscode[1] - '0'))};

  // update the display
  uint8_t dotState = 0x01 << currentDot;
  display(passcodeTime, dotState);
}

void displayTest()
{
  // count up, colon off, dots alternate
  for (int8_t i = 0; i < 10; i++)
  {
    uint8_t doubleDigit = (i * 10) + i;
    tmElements_t testDisplay = {0, doubleDigit, doubleDigit};
    display(testDisplay, i % 2 ? 0b1111 : 0b0000);
    delay(300);
  }

  // count down, colon on, dots alternate
  for (int8_t i = 9; i >= 0; i--)
  {
    uint8_t doubleDigit = (i * 10) + i;
    tmElements_t testDisplay = {1, doubleDigit, doubleDigit};
    display(testDisplay, i % 2 ? 0b1111 : 0b0000);
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
  connectingTicker.attach(0.5, displayPasscode, apPasscode.c_str());
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
  if (currentTime != lastUpdate)
  {
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
    display(timeElements);

    lastUpdate = currentTime;
  }
}