#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

#include <WiFiManager.h>

#include <NtpClientLib.h>
#include <TimeLib.h>

#include "config.h"

// enable uint64 support for UNIX timestamps
// and avoid the Y2038 problem
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>

// enable debug messages to the serial port
#define DEBUG

#define NUM_CHIPS 6
// convert a chip and pin number to the corresponding bit
// in the serial output stream of the shift registers
#define P2B(CHIP, PIN) ((((NUM_CHIPS) - (CHIP)) * 8) + (PIN))

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
const uint8_t digitsPinmap[4][10]{
    {P2B(1, 5), P2B(1, 7), P2B(1, 2), P2B(1, 1), P2B(1, 0), P2B(1, 7), P2B(1, 6), P2B(2, 6), P2B(2, 5), P2B(2, 4)},
    {P2B(3, 4), P2B(2, 3), P2B(2, 2), P2B(2, 1), P2B(2, 7), P2B(2, 0), P2B(3, 0), P2B(3, 1), P2B(3, 2), P2B(3, 3)},
    {P2B(5, 7), P2B(4, 4), P2B(4, 3), P2B(4, 5), P2B(4, 2), P2B(5, 2), P2B(4, 1), P2B(4, 7), P2B(4, 0), P2B(5, 1)},
    {P2B(6, 0), P2B(5, 4), P2B(5, 3), P2B(5, 5), P2B(5, 6), P2B(5, 2), P2B(6, 5), P2B(6, 3), P2B(6, 2), P2B(6, 1)}};

// mapping of [led number] to bit in the serial output stream
const uint8_t ledPinmap[2] = {P2B(3, 6), P2B(3, 7)};

// mapping of [number digit dot] to bit in the serial output stream
const uint8_t dotPinmap[4] = {P2B(1, 4), P2B(3, 5), P2B(5, 0), P2B(6, 4)};

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
    NTP.setTimeZone(tzInfo.offset / SECS_PER_HOUR, tzInfo.offset % SECS_PER_HOUR);
  }
}

/**
 * @brief Display the passed time on the nixie display
 * Shifts out the bit pattern to display the passed 
 * time digits on the nixie display
 * 
 * @param time time to display 
 */
void display(tmElements_t time)
{
  uint64_t serialStream = 0;
  
  serialStream |= 1ull << digitsPinmap[0][time.Hour / 10];
  serialStream |= 1ull << digitsPinmap[1][time.Hour % 10];
  serialStream |= 1ull << digitsPinmap[2][time.Minute / 10];
  serialStream |= 1ull << digitsPinmap[3][time.Minute % 10];

  // blink the LED seperator every other second
  if (time.Second % 2)
  {
    serialStream |= (1ull << ledPinmap[0]);
    serialStream |= (1ull << ledPinmap[1]);
  }

  // set latch pin low to avoid displaying the shifting of data
  digitalWrite(latchPin, LOW);

  for (uint8_t numByte = 0; numByte < NUM_CHIPS; numByte++)
  {
    uint8_t dataByte = (serialStream >> (numByte * 8)) & 0xFF;
    shiftOut(dataPin, clockPin, LSBFIRST, dataByte);
    #ifdef DEBUG
    Serial.print("Chip ");
    Serial.print(NUM_CHIPS - numByte);
    Serial.print(" set to state ");
    Serial.println(String(dataByte, BIN));
    #endif
  }

  // latch the temp register into the output
  digitalWrite(latchPin, HIGH);
}

void setup()
{
#ifdef DEBUG
  // setup the debug serial port
  Serial.begin(115200);
#endif

  // setup GPIO data directions
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);

  // auto-manage wifi configuration
  if (!MDNS.begin("nixie"))
  {
    Serial.println("Error setting up MDNS responder!");
  }
  wifiManager.autoConnect("Nixie Clock");

  // initially get the timezone info
  updateTimeZoneInfo();

  NTP.begin("pool.ntp.org");
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
    }
#endif

    tmElements_t timeElements;
    breakTime(currentTime, timeElements);
    display(timeElements);

    lastUpdate = currentTime;
  }
}