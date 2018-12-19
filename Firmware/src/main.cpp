#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

#include <WiFiManager.h>

#include <NtpClientLib.h>
#include <TimeLib.h>

#include <ArduinoJson.h>

// The WiFiManager Object handles the auto-connect feature
WiFiManager wifiManager;

// a normal response from the timezonedb has 13 objects
const char* timezoneDbUrl = "http://api.timezonedb.com/v2.1/get-time-zone?format=json&key=&by=zone&zone=";
const int responseCapacity = JSON_OBJECT_SIZE(13);

bool getDST(bool* out) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  http.begin("");

  // check the return code
  if (http.GET() != HTTP_CODE_OK) {
    return false;
  }else{
    // parse the request response payload
    StaticJsonBuffer<responseCapacity> jsonBuffer;
    JsonObject& root = jsonBuffer.parse(http.getStream());
  }
  http.end();   //Close connection
}

void setup()
{
  // setup the debug serial port
  Serial.begin(9600);

  // auto-manage wifi configuration
  if (!MDNS.begin("nixie"))
  {
    Serial.println("Error setting up MDNS responder!");
  }
  wifiManager.autoConnect("Nixie Clock");

  NTP.begin ("pool.ntp.org", 1, true, 0);
  NTP.setInterval (63);

}

void loop()
{
  static long last = 0;


  if ((millis () - last) > 5100) {
        //Serial.println(millis() - last);
        last = millis ();
        Serial.print (NTP.getTimeDateString ()); Serial.print (" ");
        Serial.print (NTP.isSummerTime () ? "Summer Time. " : "Winter Time. ");
        Serial.print ("WiFi is ");
        Serial.print (WiFi.isConnected () ? "connected" : "not connected"); Serial.print (". ");
        Serial.print ("Uptime: ");
        Serial.print (NTP.getUptimeString ()); Serial.print (" since ");
        Serial.println (NTP.getTimeDateString (NTP.getFirstSync ()).c_str ());
    }
}