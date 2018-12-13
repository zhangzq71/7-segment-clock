// Original Code by leonardlee - https://github.com/leoclee
// Modified by Robert Feddeler - https://github.com/RJFeddeler

#include <FS.h>                         // this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>
#include <DNSServer.h>                  // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>           // Local WebServer used to serve the configuration portal
#include <WiFiManager.h>                // https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <TimeLib.h>                    // https://github.com/PaulStoffregen/Time
#include <ArduinoJson.h>                // https://github.com/bblanchon/ArduinoJson/
#include <ESP8266SSDP.h>
#include <NeoPixelBus.h>

#define DIGIT_HOUR          1
#define DIGIT_MINUTE        2

#define LED_COLON1          0
#define LED_COLON2         15
#define LED_HOUR1           7
#define LED_HOUR2           0
#define LED_MINUTE1        15
#define LED_MINUTE2        22

#define PHOTORESISTOR_PIN  A0
#define colorSaturation    128

WiFiManager wifiManager;

NeoGamma<NeoGammaTableMethod> colorGamma;
NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> strip(30);

RgbColor black(0);

HslColor fromColor(black);
HslColor toColor(RgbColor(255, 0, 0));
HslColor currentColor(fromColor);
HslColor savedColor(toColor);

unsigned long lastColorChangeTime = 0;
unsigned long colorSaveInterval = 15000;

bool fading = false;
uint16_t lerp = 0;

int autoDimValue = 500;
const float dimmedValueMax = 0.24f;

ESP8266WebServer server(80);

char googleApiKey[40] = "";
char ipstackApiKey[33] = "";

int tzOffset = 0;
bool isTwelveHour = true;

String ipLatitude = "";
String ipLongitude = "";
String overrideLatitude = "";
String overrideLongitude = "";

String UrlEncode(const String url) {
  String e;
  for (int i = 0; i < url.length(); i++) {
    char c = url.charAt(i);
    if (c == 0x20) {
      e += "%20";            // "+" only valid for some uses
    } else if (isalnum(c)) {
      e += c;
    } else {
      e += "%";
      if (c < 0x10) e += "0";
      e += String(c, HEX);
    }
  }
  return e;
}

void getIPlocation() { // Using ipstack.com to map public IP's location
  
  HTTPClient http;
  String URL = "http://api.ipstack.com/check?fields=latitude,longitude&access_key=" + String(ipstackApiKey); // no host or IP specified returns client's public IP info
  String payload;
  if (!http.begin(URL)) {
    Serial.println(F("getIPlocation: [HTTP] connect failed!"));
  } else {
    int stat = http.GET();
    if (stat > 0) {
      if (stat == HTTP_CODE_OK) {
        payload = http.getString();
        DynamicJsonBuffer jsonBuffer;
        JsonObject& root = jsonBuffer.parseObject(payload);
        if (root.success()) {
          // https://ipstack.com/documentation#errors
          JsonVariant error = root["error"];
          if (error.success()) {
            Serial.println("getIPlocation: [HTTP] error returned in response:");
            error.prettyPrintTo(Serial);
            Serial.println("");
          } else {
            ipLatitude = root["latitude"].as<String>();
            ipLongitude = root["longitude"].as<String>();
            Serial.println("getIPlocation: " + ipLatitude + "," + ipLongitude);
          }
        } else {
          Serial.println(F("getIPlocation: JSON parse failed!"));
          Serial.println(payload);
        }
      } else {
        Serial.printf("getIPlocation: [HTTP] GET reply %d\r\n", stat);
      }
    } else {
      Serial.printf("getIPlocation: [HTTP] GET failed: %s\r\n", http.errorToString(stat).c_str());
    }
  }
  http.end();
} // getIPlocation

const char* mapsHost = "maps.googleapis.com";
int getTimeZoneOffset(time_t now, String latitude, String longitude, const char* key) { // using google maps API, return TimeZone for provided timestamp
  if (latitude == "" || longitude == "") {
    Serial.println("getTimeZoneOffset: 0 (no location)");
    return 0;
  }
  int offset = tzOffset; // default to returning previous offset value, to handle temporary failures
  WiFiClientSecure client;
  Serial.print("connecting to ");
  Serial.println(mapsHost);
  if (!client.connect(mapsHost, 443)) {
    Serial.println("connection failed");
    return offset;
  }

  String url = "/maps/api/timezone/json?location="
               + UrlEncode(latitude + "," + longitude) + "&timestamp=" + String(now) + "&key=" + String(key);
  Serial.print("requesting URL: ");
  Serial.println(url);

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + mapsHost + "\r\n" +
               "Connection: close\r\n\r\n");

  Serial.println("request sent");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      Serial.println("headers received");
      break;
    }
  }
  String line = client.readStringUntil('}') + "}";
  //  Serial.println("reply was:");
  //  Serial.println("==========");
  //  Serial.println(line);
  //  Serial.println("==========");
  //  Serial.println("closing connection");

  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(line);
  if (root.success()) {
    // https://developers.google.com/maps/documentation/timezone/intro#Responses
    String status = root["status"];
    if (status == "OK") {
      offset = int (root["rawOffset"]) + int (root["dstOffset"]);  // combine Offset and dstOffset
      const char* tzname = root["timeZoneName"];
      Serial.printf("getTimeZoneOffset: %s (%d)\r\n", tzname, offset);
    } else {
      Serial.println(F("getTimeZoneOffset: API request was unsuccessful:"));
      root.prettyPrintTo(Serial);
      Serial.println("");
    }
  } else {
    Serial.println(F("getTimeZoneOffset: JSON parse failed!"));
    Serial.println(line);
  }

  return offset;
} // getTimeZoneOffset

unsigned long ntpBegin;

time_t getNtpTime () {
  Serial.print("Synchronize NTP ...");
  ntpBegin = millis();
  // using 0 for timezone because fractional time zones (such as Myanmar, which is UTC+06:30) are unsupported https://github.com/esp8266/Arduino/issues/2543
  // using 0 for dst because  https://github.com/esp8266/Arduino/issues/2505
  // instead, we will add the offset to the returned value
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  //while (millis() - ntpBegin < 5000 && time(nullptr) < 1500000000) {
  while (time(nullptr) < 1500000000) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" OK");

  return time(nullptr) + tzOffset;
}

//callback notifying us of the need to save config
bool shouldSaveConfig = false;
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(PHOTORESISTOR_PIN, INPUT);

  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  strip.Begin();
  strip.ClearTo(black);
  strip.Show();
    
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          // API keys
          // json values could be null! https://arduinojson.org/v5/faq/why-does-my-device-crash-or-reboot/#example-with-strcpy
          strlcpy(googleApiKey, json["googleApiKey"] | googleApiKey, 40);
          strlcpy(ipstackApiKey, json["ipstackApiKey"] | ipstackApiKey, 33);

          // override location
          const char* lat = json["location"]["overrideLatitude"];
          const char* lng = json["location"]["overrideLongitude"];
          if (lat && lng) {
            overrideLatitude = String(lat);
            overrideLongitude = String(lng);
          }

          // color
          JsonVariant hue = json["color"]["h"];
          JsonVariant sat = json["color"]["s"];
          JsonVariant val = json["color"]["v"];
          if (hue.success() && sat.success() && val.success()) {
            // store initial color in toColor... hopefully this is OK
            toColor = HslColor((hue.as<int>() % 360) / 360.0f, constrain(sat.as<int>(), 0, 100) / 100.0f, constrain(val.as<int>(), 0, 100) / 100.0f);
          }
          savedColor = toColor;

          // clock
          JsonVariant clk = json["clock"];
          if (clk.success() && clk.is<int>()) {
            isTwelveHour = clk.as<int>() == 12;
          }

          JsonVariant dim = json["dim"];
          if (dim.success() && dim.is<int>()) {
            autoDimValue = dim.as<int>();
          }
        } else {
          Serial.println("failed to load json config");
        }
      }
    } else {
      Serial.println("config file not found");
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter customGoogleApiKey("googleApiKey", "Google API Key", googleApiKey, 40);
  WiFiManagerParameter customIpstackApiKey("ipstackApiKey", "ipstack API Key", ipstackApiKey, 33);

  //WiFiManager
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.addParameter(&customGoogleApiKey);
  wifiManager.addParameter(&customIpstackApiKey);

  //reset settings - for testing
  //wifiManager.resetSettings();

  if (!wifiManager.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  // match the fade in color's hue and saturation
  currentColor = HslColor(toColor.H, toColor.S, 0.0f);

  if (overrideLatitude == "" || overrideLongitude == "") {
    getIPlocation();
  }
  time_t nowUtc = getNtpTime();
  tzOffset = getTimeZoneOffset(nowUtc, (overrideLatitude != "") ? overrideLatitude : ipLatitude, (overrideLongitude != "") ? overrideLongitude : ipLongitude, googleApiKey);
  setSyncProvider(getNtpTime);
  setSyncInterval(3600);

  server.serveStatic("/", SPIFFS, "/index.html");

  server.on("/color", HTTP_PUT, []() {
    String h = server.arg("h");
    String s = server.arg("s");
    String v = server.arg("v");
    if (h != "" && s != "" && v != "") {
      Serial.print("SERVER.ON('/color'): H=");
      Serial.print(h.toInt());
      Serial.print(", S=");
      Serial.print(s.toInt());
      Serial.print(", V=");
      Serial.println(v.toInt());
      setColor((h.toInt() % 360) / 360.0f, constrain(s.toInt(), 0, 100) / 100.0f, constrain(v.toInt(), 0, 100) / 200.0f);
      server.send(204);
    }
  });

  server.on("/color", HTTP_GET, []() {
    const size_t bufferSize = JSON_OBJECT_SIZE(3); // https://arduinojson.org/v5/assistant/
    DynamicJsonBuffer jsonBuffer(bufferSize);

    JsonObject& root = jsonBuffer.createObject();
    
    root["h"] = (int)(toColor.H * 360.0f);
    root["s"] = (int)(toColor.S * 100.0f);
    root["v"] = (int)(toColor.L * 200.0f);

    String jsonString;
    root.printTo(jsonString);
    server.send(200, "application/json", jsonString);
  });

  server.on("/clock", HTTP_GET, []() {
    server.send(200, "text/plain", isTwelveHour ? "12" : "24");
  });

  server.on("/clock", HTTP_PUT, []() {
    String body = server.arg("plain");
    bool newIsTwelveHour;
    if (body == "12") {
      newIsTwelveHour = true;
      server.send(204);
      if (isTwelveHour != newIsTwelveHour) {
        isTwelveHour = newIsTwelveHour;
        saveConfig();
      }
    } else if (body == "24") {
      newIsTwelveHour = false;
      server.send(204);
      if (isTwelveHour != newIsTwelveHour) {
        isTwelveHour = newIsTwelveHour;
        saveConfig();
      }
    } else {
      server.send(400);
    }
  });

  server.on("/resetWiFi", HTTP_PUT, []() {
    server.send(204);
    wifiManager.resetSettings();
  });

  server.on("/dim", HTTP_PUT, []() {
    String body = server.arg("plain");
    if (body != "") {
      autoDimValue = constrain(body.toInt(), 0, 1000);
      server.send(204);
    }
  });

  server.on("/dim", HTTP_GET, []() {
    server.send(200, "text/plain", String(autoDimValue));
  });
  
  server.on("/location", HTTP_GET, []() {
    const size_t bufferSize = JSON_OBJECT_SIZE(4); // https://arduinojson.org/v5/assistant/
    DynamicJsonBuffer jsonBuffer(bufferSize);

    JsonObject& root = jsonBuffer.createObject();

    root["ipLatitude"] = ipLatitude;
    root["ipLongitude"] = ipLongitude;
    root["overrideLatitude"] = overrideLatitude;
    root["overrideLongitude"] = overrideLongitude;

    String jsonString;
    root.printTo(jsonString);
    server.send(200, "application/json", jsonString);
  });

  server.on("/location", HTTP_PUT, []() {
    String lat = server.arg("overrideLatitude");
    String lng = server.arg("overrideLongitude");
    // TODO some kind of validation
    server.send(204);
    if (lat != overrideLatitude || lng != overrideLongitude) {
      overrideLatitude = lat;
      overrideLongitude = lng;
      refreshTimezoneOffset();
      saveConfig();
    }
  });
  
  server.on("/config", HTTP_GET, []() {
    const size_t bufferSize = JSON_OBJECT_SIZE(3) + 2*JSON_OBJECT_SIZE(4); // https://arduinojson.org/v5/assistant/
    DynamicJsonBuffer jsonBuffer(bufferSize);

    JsonObject& root = jsonBuffer.createObject();

    JsonObject& color = root.createNestedObject("color");
    color["h"] = (int)(toColor.H * 360.0f);
    color["s"] = (int)(toColor.S * 100.0f);
    color["v"] = (int)(toColor.L * 200.0f);
    
    root["clock"] = isTwelveHour ? 12 : 24;
    root["dim"] = autoDimValue;
    JsonObject& location = root.createNestedObject("location");
    location["ipLatitude"] = ipLatitude;
    location["ipLongitude"] = ipLongitude;
    location["overrideLatitude"] = overrideLatitude;
    location["overrideLongitude"] = overrideLongitude;
    root["googleApiKey"] = googleApiKey;

    String jsonString;
    root.printTo(jsonString);
    server.send(200, "application/json", jsonString);
  });

  server.on("/description.xml", HTTP_GET, []() {
    SSDP.schema(server.client());
    // connection hangs for about 2 seconds otherwise
    server.client().stop();
  });

  server.begin();
  Serial.println("HTTP server started");

  // SSDP
  Serial.printf("Starting SSDP...\n");
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("7 Segment LED Clock");
  SSDP.setSerialNumber(ESP.getChipId());
  SSDP.setURL("");
  SSDP.setModelName("7 Segment LED Clock");
  SSDP.setModelNumber("LL-7S");
  SSDP.setModelURL("https://github.com/leoclee/7-segment-clock");
  SSDP.setManufacturer("Leonard Lee");
  SSDP.setManufacturerURL("https://github.com/leoclee");
  SSDP.setDeviceType("urn:schemas-upnp-org:device:7SegmentClock:1");
  SSDP.begin();

  //read updated parameters
  strcpy(googleApiKey, customGoogleApiKey.getValue());
  strcpy(ipstackApiKey, customIpstackApiKey.getValue());
  Serial.println("read updated parameters");
  Serial.println(googleApiKey);
  Serial.println(ipstackApiKey);

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    saveConfig();
  }

  // OTA
  ArduinoOTA.setHostname("7sclock"); // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("OTA Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  } );
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  /*
  Serial.println();
  Serial.print(F("Last reset reason: "));
  Serial.println(ESP.getResetReason());
  Serial.print(F("WiFi Hostname: "));
  Serial.println(WiFi.hostname());
  */
  Serial.print(F("WiFi IP addr: "));
  Serial.println(WiFi.localIP());
  Serial.print(F("WiFi MAC addr: "));
  Serial.println(WiFi.macAddress());
  Serial.print(F("WiFi SSID: "));
  Serial.println(WiFi.SSID());
  /*
  Serial.print(F("ESP Sketch size: "));
  Serial.println(ESP.getSketchSize());
  Serial.print(F("ESP Flash free: "));
  Serial.println(ESP.getFreeSketchSpace());
  Serial.print(F("ESP Flash Size: "));
  Serial.println(ESP.getFlashChipRealSize());
  */
}

int currentMinute = 0;
uint32_t lastLightRead = 0;
boolean first = true;
int ambientLight;

const int8_t dimCounterMax = 4;
int8_t dimCounter = 0;
bool dimmed = false;
float dimmedValue = 0.0f;
int dimProgress = 0;

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  if (timeStatus() != timeNotSet) {
    if (first) { // super dramatic fade in effect
      setColor(toColor);
      first = false;
    }
    
    if (minute() != currentMinute) {
      // check for new offset every hour between midnight and 4AM for DST change
      // see: https://www.timeanddate.com/time/dst/statistics.html#dstuse
      // see: https://en.wikipedia.org/wiki/Daylight_saving_time#Procedure
      if (minute() == 0 && hour() <= 4) {
        refreshTimezoneOffset();
      }
      printTime();
    }
    
    currentMinute = minute();
    updateLeds();
  }
  
  fadeToColor();
  saveColorChange();

  if (dimmed && dimmedValue > dimmedValueMax) {
    if (dimProgress < 512)
      dimmedValue = HslColor::LinearBlend<NeoHueBlendShortestDistance>(currentColor, HslColor(currentColor.H, currentColor.S, dimmedValueMax), ++dimProgress / 512.0f).L;
    else
      dimmedValue = dimmedValueMax;
  }

  if (dimmedValueMax < toColor.L && millis() - lastLightRead >= 100) {
    ambientLight = analogRead(PHOTORESISTOR_PIN);
    lastLightRead = millis();
  
    if (!dimmed && ambientLight < autoDimValue) {
      if (++dimCounter > dimCounterMax) {
        dimCounter = 0;
        dimmed = true;
        dimmedValue = currentColor.L;
        dimProgress = 0;
        Serial.println("Dim: ON");
      }
    }
    else if (dimmed && ambientLight > autoDimValue) {
      if (++dimCounter > dimCounterMax) {
        dimCounter = 0;
        dimmed = false;
        float origValue = currentColor.L;
        currentColor = HslColor(currentColor.H, currentColor.S, dimmedValue);
        setColor(currentColor.H, currentColor.S, origValue);
        Serial.println("Dim: OFF");
      }
    }
    else {
      if (dimCounter > 0)
        dimCounter = 0;
    }
  }
}

// throttle color change induced saving to avoid unnecessary write/erase cycles
void saveColorChange() {
  if (!hslEqual(toColor, savedColor) && ((millis() - lastColorChangeTime) > colorSaveInterval)) {
    Serial.println("config save triggered by color change");
    saveConfig();
  }
}

void refreshTimezoneOffset() {
  // handle the scenario where override location is loaded from json, but then removed during runtime--we will need to do an ip location lookup
  if ((overrideLatitude == "" || overrideLongitude == "") && (ipLatitude == "" || ipLongitude == "")) {
    // if no override location specified, attempt to look up using IP based geolocation
    getIPlocation();
  }
  int newTzOffset = getTimeZoneOffset(now() - tzOffset, (overrideLatitude != "") ? overrideLatitude : ipLatitude, (overrideLongitude != "") ? overrideLongitude : ipLongitude, googleApiKey);
  if (newTzOffset != tzOffset) {
    tzOffset = newTzOffset;
    setTime(time(nullptr) + tzOffset);
  }
}

void saveConfig() {
  Serial.println("saving config");
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["googleApiKey"] = googleApiKey;
  json["ipstackApiKey"] = ipstackApiKey;
  JsonObject& location = json.createNestedObject("location");
  location["overrideLatitude"] = overrideLatitude;
  location["overrideLongitude"] = overrideLongitude;
  JsonObject& color = json.createNestedObject("color");
  
  color["h"] = (int)(toColor.H * 360.0f);
  color["s"] = (int)(toColor.S * 100.0f);
  color["v"] = (int)(toColor.L * 100.0f);
  
  json["clock"] = isTwelveHour ? 12 : 24;
  json["dim"] = autoDimValue;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
  }

  json.printTo(Serial);
  json.printTo(configFile);
  configFile.close();

  savedColor = HslColor(toColor);
}

void updateLeds() {
  strip.ClearTo(black);

  // Colon

  strip.SetPixelColor(LED_COLON1, (dimmed && currentColor.L > dimmedValue) ? colorGamma.Correct(RgbColor(HslColor(currentColor.H, currentColor.S, dimmedValue))) : colorGamma.Correct(RgbColor(currentColor)));
  strip.SetPixelColor(LED_COLON2, (dimmed && currentColor.L > dimmedValue) ? colorGamma.Correct(RgbColor(HslColor(currentColor.H, currentColor.S, dimmedValue))) : colorGamma.Correct(RgbColor(currentColor)));

  int displayHour = isTwelveHour ? hourFormat12() : hour();
  if (displayHour >= 10 || !isTwelveHour) {
    displayDigit(LED_HOUR1, DIGIT_HOUR, displayHour / 10);
  }
  displayDigit(LED_HOUR2, DIGIT_HOUR, displayHour % 10);

  int displayMinute = minute();
  displayDigit(LED_MINUTE1, DIGIT_MINUTE, (displayMinute < 10) ? 0 : displayMinute / 10);
  displayDigit(LED_MINUTE2, DIGIT_MINUTE, displayMinute % 10);

  strip.Show();
}

void displayDigit(int offset, int digitUnit, int value) {
  HslColor c(currentColor);
  if (dimmed && c.L > dimmedValue)
    c.L = dimmedValue;

  c = colorGamma.Correct(RgbColor(c));
    
  switch (value) {
    case 0:
      strip.SetPixelColor(offset + 1, c);
      strip.SetPixelColor(offset + 2, c);
      strip.SetPixelColor(offset + 3, c);
      strip.SetPixelColor(offset + 5, c);
      strip.SetPixelColor(offset + 6, c);
      strip.SetPixelColor(offset + 7, c);
      break;
    case 1:
      if (digitUnit == DIGIT_HOUR) {
        strip.SetPixelColor(offset + 1, c);
        strip.SetPixelColor(offset + 5, c);
      }
      else {
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 7, c);
      }
      break;
    case 2:
      strip.SetPixelColor(offset + 1, c);
      strip.SetPixelColor(offset + 2, c);
      strip.SetPixelColor(offset + 4, c);
      strip.SetPixelColor(offset + 6, c);
      strip.SetPixelColor(offset + 7, c);
      break;
    case 3:
      if (digitUnit == DIGIT_HOUR) {
        strip.SetPixelColor(offset + 1, c);
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
        strip.SetPixelColor(offset + 6, c);
      }
      else {
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 6, c);
        strip.SetPixelColor(offset + 7, c);
      }
      break;
    case 4:
      if (digitUnit == DIGIT_HOUR) {
        strip.SetPixelColor(offset + 1, c);
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
      }
      else {
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
        strip.SetPixelColor(offset + 7, c);
      }
      break;
    case 5:
      strip.SetPixelColor(offset + 2, c);
      strip.SetPixelColor(offset + 3, c);
      strip.SetPixelColor(offset + 4, c);
      strip.SetPixelColor(offset + 5, c);
      strip.SetPixelColor(offset + 6, c);
      break;
    case 6:
      if (digitUnit == DIGIT_HOUR) {
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
        strip.SetPixelColor(offset + 6, c);
        strip.SetPixelColor(offset + 7, c);
      }
      else {
        strip.SetPixelColor(offset + 1, c);
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
        strip.SetPixelColor(offset + 6, c);
      }
      break;
    case 7:
      if (digitUnit == DIGIT_HOUR) {
        strip.SetPixelColor(offset + 1, c);
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 5, c);
      }
      else {
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 6, c);
        strip.SetPixelColor(offset + 7, c);
      }
      break;
    case 8:
      strip.SetPixelColor(offset + 1, c);
      strip.SetPixelColor(offset + 2, c);
      strip.SetPixelColor(offset + 3, c);
      strip.SetPixelColor(offset + 4, c);
      strip.SetPixelColor(offset + 5, c);
      strip.SetPixelColor(offset + 6, c);
      strip.SetPixelColor(offset + 7, c);
      break;
    case 9:
      if (digitUnit == DIGIT_HOUR) {
        strip.SetPixelColor(offset + 1, c);
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
        strip.SetPixelColor(offset + 6, c);
      }
      else {
        strip.SetPixelColor(offset + 2, c);
        strip.SetPixelColor(offset + 3, c);
        strip.SetPixelColor(offset + 4, c);
        strip.SetPixelColor(offset + 5, c);
        strip.SetPixelColor(offset + 6, c);
        strip.SetPixelColor(offset + 7, c);
      }
      break;
  }
}

void printTime()
{
  // digital clock display of the time
  Serial.print(isTwelveHour ? hourFormat12() : hour());
  Serial.print(":");
  printDigits(minute());
  Serial.print(":");
  printDigits(second());
  Serial.println("");

  /*
    // print free heap
    Serial.println("");
    int heap = ESP.getFreeHeap();
    Serial.println(heap);
  */
}

void printDigits(int digits)
{
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}

void setColor(float h, float s, float l) {
  setColor(HslColor(h, s, l));
}

void setColor(HslColor hsl) {
  lastColorChangeTime = millis();

  Serial.print("setting color to CHSV(");
  Serial.print(hsl.H);
  Serial.print(", ");
  Serial.print(hsl.S);
  Serial.print(", ");
  Serial.print(hsl.L);
  Serial.println(")");
  
  fromColor = HslColor(currentColor);
  toColor = HslColor(hsl);
  
  lerp = 0;
  fading = true;
}

bool hslEqual(HslColor color1, HslColor color2) {
  if (color1.H == color2.H && color1.S == color2.S && color1.L == color2.L)
    return true;
  else
    return false;
}

void fadeToColor() {
  if (fading) {
    if (lerp < 512) {
      currentColor = HslColor::LinearBlend<NeoHueBlendShortestDistance>(fromColor, toColor, ++lerp / 512.0f);
    } else {
      fading = false;
      currentColor = HslColor(toColor);
    }
  }
}
