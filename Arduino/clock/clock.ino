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
#define FASTLED_INTERRUPT_RETRY_COUNT 0 // https://github.com/FastLED/FastLED/issues/367 decided against disabling FASTLED_ALLOW_INTERRUPTS due to WDT resets
#define FASTLED_ESP8266_D1_PIN_ORDER
#include <FastLED.h>

#define NUM_LEDS_HOUR 15
#define NUM_LEDS_MINUTE 31
#define DATA_PIN_HOUR D2
#define DATA_PIN_MINUTE D6

#define PHOTORESISTOR_PIN  A0

// LED
CRGB ledsHour[NUM_LEDS_HOUR];
CRGB ledsMinute[NUM_LEDS_MINUTE];
CHSV fromColor;
CHSV toColor = CHSV(128, 255, 128);
CHSV currentColor = CHSV(0, 0, 0);
CHSV savedColor;
unsigned long lastColorChangeTime = 0;
unsigned long colorSaveInterval = 15000; // number of milliseconds to wait for a color change to trigger a save

uint8_t lerp = 0;
bool fading = false;

int tzOffset = 0;

ESP8266WebServer server(80);

// openssl s_client -connect maps.googleapis.com:443 | openssl x509 -fingerprint -noout
//const char* gMapsCrt = "92:AC:F0:5A:A8:20:A2:0D:D2:4F:20:28:2D:98:00:1F:E1:4B:99:5E";
char googleApiKey[40] = "";
char ipstackApiKey[33] = "";

String ipLatitude = "";
String ipLongitude = "";
String overrideLatitude = "";
String overrideLongitude = "";

bool isTwelveHour = false;

void setColor(int h, int s, int v);
void setColor(CHSV chsv);
void saveConfig();
void updateLeds();
void fadeToColor();
void displayHourDigit(int offset, int digit);
void displayMinuteDigit(int offset, int digit);
void printDigits(int digits);
void refreshTimezoneOffset();
void printTime();
void saveColorChange();

// TODO is this better than https://github.com/zenmanenergy/ESP8266-Arduino-Examples/blob/master/helloWorld_urlencoded/urlencode.ino ?
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
  //client.setInsecure(); // disable cert/fingerprint check
  Serial.print("connecting to ");
  Serial.println(mapsHost);
  if (!client.connect(mapsHost, 443)) {
    Serial.println("connection failed");
    return 8 * 3600;//offset;
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
  configTime(0, 0, "ntp2.aliyun.com", "ntp1.aliyun.com");
  //while (millis() - ntpBegin < 5000 && time(nullptr) < 1500000000) {
  while (time(nullptr) < 1500000000) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" OK");

  return time(nullptr) + tzOffset;
}

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(PHOTORESISTOR_PIN, INPUT);

  // initialize and reset LEDs
  delay(1); // leds sometimes not all resetting properly without this for some reason
  FastLED.addLeds<NEOPIXEL, DATA_PIN_HOUR>(ledsHour, NUM_LEDS_HOUR);
  FastLED.addLeds<NEOPIXEL, DATA_PIN_MINUTE>(ledsMinute, NUM_LEDS_MINUTE);
  fill_solid(ledsHour, NUM_LEDS_HOUR, CRGB::Black);
  fill_solid(ledsMinute, NUM_LEDS_MINUTE, CRGB::Black);
  FastLED.show();

  //read configuration from FS json
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
            toColor = CHSV((hue.as<int>() % 360) * 255 / 360, constrain(sat.as<int>(), 0, 100) * 255 / 100, constrain(val.as<int>(), 0, 100) * 255 / 100);
          }
          savedColor = toColor;

          // clock
          JsonVariant clk = json["clock"];
          if (clk.success() && clk.is<int>()) {
            isTwelveHour = clk.as<int>() == 12;
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
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.addParameter(&customGoogleApiKey);
  wifiManager.addParameter(&customIpstackApiKey);

  //reset settings - for testing
  // wifiManager.resetSettings();

  if (!wifiManager.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  // match the fade in color's hue and saturation
  currentColor = CHSV(toColor.h, toColor.s, 0);

  if (overrideLatitude == "" || overrideLongitude == "") {
    // if no override location specified, attempt to look up using IP based geolocation
    getIPlocation();
  }
  time_t nowUtc = getNtpTime();
  tzOffset = getTimeZoneOffset(nowUtc, (overrideLatitude != "") ? overrideLatitude : ipLatitude, (overrideLongitude != "") ? overrideLongitude : ipLongitude, googleApiKey);
  Serial.printf("tzOffset: %d\n", tzOffset);
  setSyncProvider(getNtpTime);
  setSyncInterval(3600);

  server.serveStatic("/", SPIFFS, "/index.html");

  server.on("/color", HTTP_PUT, []() {
    String h = server.arg("h");
    String s = server.arg("s");
    String v = server.arg("v");
    if (h != "" && s != "" && v != "") {
      setColor(h.toInt(), s.toInt(), v.toInt());
      server.send(204);
    }
  });

  server.on("/color", HTTP_GET, []() {
    const size_t bufferSize = JSON_OBJECT_SIZE(3); // https://arduinojson.org/v5/assistant/
    DynamicJsonBuffer jsonBuffer(bufferSize);

    JsonObject& root = jsonBuffer.createObject();
    root["h"] = toColor.hue * 360 / 255;
    root["s"] = toColor.saturation * 100 / 255;
    root["v"] = toColor.value * 100 / 255;

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
    color["h"] = toColor.hue * 360 / 255;
    color["s"] = toColor.saturation * 100 / 255;
    color["v"] = toColor.value * 100 / 255;
    root["clock"] = isTwelveHour ? 12 : 24;
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

  //pinMode(LED_BUILTIN, OUTPUT);
  //digitalWrite(LED_BUILTIN, HIGH);

  Serial.println();
  Serial.print(F("Last reset reason: "));
  Serial.println(ESP.getResetReason());
  Serial.print(F("WiFi Hostname: "));
  Serial.println(WiFi.hostname());
  Serial.print(F("WiFi IP addr: "));
  Serial.println(WiFi.localIP());
  Serial.print(F("WiFi MAC addr: "));
  Serial.println(WiFi.macAddress());
  Serial.print(F("WiFi SSID: "));
  Serial.println(WiFi.SSID());
  Serial.print(F("ESP Sketch size: "));
  Serial.println(ESP.getSketchSize());
  Serial.print(F("ESP Flash free: "));
  Serial.println(ESP.getFreeSketchSpace());
  Serial.print(F("ESP Flash Size: "));
  Serial.println(ESP.getFlashChipRealSize());

  // TODO access point name
}

int currentMinute = 0;
boolean first = true;
int h = 0, s = 0, v = 0;
int dec_inc_v = 0;
char dec_inc_h = 1;     // decrease or increase, it is increase in startup
int exponent = 0;
double base;
int currentSecond = 0;

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  if (timeStatus() != timeNotSet) {
    if (first) { // super dramatic fade in effect
      setColor(toColor);
      h = 100;//toColor.h;
      s = toColor.s;
      v = toColor.v;
      first = false;
    }

    // if (minute() != currentMinute) {
    if (second() != currentSecond) {
      // check for new offset every hour between midnight and 4AM for DST change
      // see: https://www.timeanddate.com/time/dst/statistics.html#dstuse
      // see: https://en.wikipedia.org/wiki/Daylight_saving_time#Procedure
      if (minute() == 0 && hour() <= 4) {
        refreshTimezoneOffset();
      }

      if (dec_inc_h != 0) {
        if (dec_inc_h >= 1) {
          if (h++ >= 255)
            dec_inc_h = -1;
        } else {
          if (h-- <= 0)
            dec_inc_h = 1;
        }
      }

      // switch (hour())
      // {
      // case 6:
      // case 7:
      // case 8:
      // case 9:
      // case 10:
      //   if (v < 150) {
      //     dec_inc_v = 1;
      //     base = pow(double(150 - v), 1.0 / 300.0);
      //   }
      //   else
      //     dec_inc_v = 0;
      //   break;

      // case 11:
      // case 12:
      // case 13:
      // case 14:
      //   if (v < 200) {
      //     dec_inc_v = 2;
      //     base = pow(double(200 - v), 1.0 / 240.0);
      //   }
      //   else
      //     dec_inc_v = 0;
      //   break;

      // case 15:
      // case 16:
      // case 17:
      //   if (v > 120) {
      //     dec_inc_v = -1;
      //     base = pow(double(v - 120), 1.0 / 180.0);
      //   }
      //   else
      //     dec_inc_v = 0;
      //   break;

      // case 18:
      // case 19:
      // case 20:
      // case 21:
      //   if (v > 2) {
      //     dec_inc_v = -2;
      //     base = pow(double(v - 2), 1.0 / 240.0);
      //   }
      //   else
      //     dec_inc_v = 0;
      //   break;

      // case 22:
      // case 23:
      // case 24:
      // case 0:
      // case 1:
      // case 2:
      // case 3:
      // case 4:
      // case 5:
      //   v = 2;
      //   dec_inc_v = 0;
      //   // dec_inc_h = 0;
      //   break;

      // default:
      //   break;
      // }

      // int diff;
      // if (dec_inc_v == -1) {
      //   exponent = (hour() * 60 + minute()) - (15 * 60);
      //   diff = (int)pow(base, (double)exponent);
      //   v = v - diff;
      // } else if (dec_inc_v == -2) {
      //   exponent = (hour() * 60 + minute()) - (18 * 60);
      //   diff = (int)pow(base, (double)exponent);
      //   v = v - diff;
      // } else if (dec_inc_v == 1) {
      //   exponent = (hour() * 60 + minute()) - (6 * 60);
      //   diff = (int)pow(base, (double)exponent);
      //   v = v + diff;
      // } else if (dec_inc_v == 2) {
      //   exponent = (hour() * 60 + minute()) - (11 * 60);
      //   diff = (int)pow(base, (double)exponent);
      //   v = v + diff;
      // }

      int ambientLight = analogRead(PHOTORESISTOR_PIN);
      if (hour() >= 21)
      {
        if (ambientLight < 10)
          v = 3;
        else
          v = 50;
      }
      else
      {
        v = map(ambientLight, 12, 200, 50, 160);
      }

      s = 255;
      // Serial.printf("h: %d, s: %d, v: %d, %d, %d, %f, %d\n", h, s, v, dec_inc_v, diff, base, exponent);
      Serial.printf("ambient: %d\r\n", ambientLight);
      CHSV clr = CHSV(h, s, v);
      setColor(clr);
      printTime();
    }
    currentMinute = minute();
    currentSecond = second();

    updateLeds();
  }
  fadeToColor();
  //saveColorChange();
}

// throttle color change induced saving to avoid unnecessary write/erase cycles
void saveColorChange() {
  if (toColor != savedColor && ((millis() - lastColorChangeTime) > colorSaveInterval)) {
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
    Serial.printf("--- tzOffset: %d\n", tzOffset);
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
  color["h"] = toColor.hue * 360 / 255;
  color["s"] = toColor.saturation * 100 / 255;
  color["v"] = toColor.value * 100 / 255;
  json["clock"] = isTwelveHour ? 12 : 24;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
  }

  json.printTo(Serial);
  json.printTo(configFile);
  configFile.close();

  savedColor = toColor;
}

void updateLeds() {
  fill_solid(ledsHour, NUM_LEDS_HOUR, CRGB::Black);
  ledsHour[0] = currentColor; // colon
  int displayHour = isTwelveHour ? hourFormat12() : hour();
  if (displayHour >= 10 || !isTwelveHour) {
    displayHourDigit(7, displayHour / 10);
  }
  displayHourDigit(0, displayHour % 10);

  fill_solid (ledsMinute, NUM_LEDS_MINUTE, CRGB::Black);
  ledsMinute[0] = currentColor; // colon
  int displayMinute = minute();
  displayMinuteDigit(0, (displayMinute < 10) ? 0 : displayMinute / 10);
  displayMinuteDigit(7, displayMinute % 10);
  ledsMinute[15] = currentColor; // colon
  ledsMinute[16] = currentColor; // colon
  int displaySecond = second();
  displayMinuteDigit(16, (displaySecond < 10) ? 0 : displaySecond / 10);
  displayMinuteDigit(23, displaySecond % 10);

  FastLED.show();
}

/**
   sets the proper LEDs to the current color to display a digit on the hour (left) side of the controller, assuming that all LEDs for the digit are already set to black
*/
void displayHourDigit(int offset, int digit) {
  switch (digit) {
    case 0:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 3] = currentColor;
      ledsHour[offset + 5] = currentColor;
      ledsHour[offset + 6] = currentColor;
      ledsHour[offset + 7] = currentColor;
      break;
    case 1:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 5] = currentColor;
      break;
    case 2:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 6] = currentColor;
      ledsHour[offset + 7] = currentColor;
      break;
    case 3:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 5] = currentColor;
      ledsHour[offset + 6] = currentColor;
      break;
    case 4:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 3] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 5] = currentColor;
      break;
    case 5:
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 3] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 5] = currentColor;
      ledsHour[offset + 6] = currentColor;
      break;
    case 6:
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 3] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 5] = currentColor;
      ledsHour[offset + 6] = currentColor;
      ledsHour[offset + 7] = currentColor;
      break;
    case 7:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 5] = currentColor;
      break;
    case 8:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 3] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 5] = currentColor;
      ledsHour[offset + 6] = currentColor;
      ledsHour[offset + 7] = currentColor;
      break;
    case 9:
      ledsHour[offset + 1] = currentColor;
      ledsHour[offset + 2] = currentColor;
      ledsHour[offset + 3] = currentColor;
      ledsHour[offset + 4] = currentColor;
      ledsHour[offset + 5] = currentColor;
      ledsHour[offset + 6] = currentColor;
      break;
  }
}

/**
   sets the proper LEDs to the current color to display a digit on the minute (right) side of the controller, assuming that all LEDs for the digit are already set to black
*/
void displayMinuteDigit(int offset, int digit) {
  switch (digit) {
    case 0:
      ledsMinute[offset + 1] = currentColor;
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 5] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 1:
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 2:
      ledsMinute[offset + 1] = currentColor;
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 3:
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 4:
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 5] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 5:
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 5] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      break;
    case 6:
      ledsMinute[offset + 1] = currentColor;
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 5] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      break;
    case 7:
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 8:
      ledsMinute[offset + 1] = currentColor;
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 5] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      ledsMinute[offset + 7] = currentColor;
      break;
    case 9:
      ledsMinute[offset + 2] = currentColor;
      ledsMinute[offset + 3] = currentColor;
      ledsMinute[offset + 4] = currentColor;
      ledsMinute[offset + 5] = currentColor;
      ledsMinute[offset + 6] = currentColor;
      ledsMinute[offset + 7] = currentColor;
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

void setColor(int h, int s, int v) {
  int hue = (h % 360) * 255 / 360;
  int sat = constrain(s, 0, 100) * 255 / 100;
  int val = constrain(v, 0, 100) * 255 / 100;

  setColor(CHSV(hue, sat, val));
}

void setColor(CHSV chsv) {
  lastColorChangeTime = millis();
  Serial.print("setting color to CHSV(");
  Serial.print(chsv.h);
  Serial.print(", ");
  Serial.print(chsv.s);
  Serial.print(", ");
  Serial.print(chsv.v);
  Serial.println(")");
  fromColor = currentColor;
  toColor = chsv;
  lerp = 0;
  fading = true;
}

void fadeToColor() {
  if (fading) {
    if (lerp < 255) {
      currentColor = blend(fromColor, toColor, ++lerp);
    } else {
      fading = false;
    }
  }
}
