/**
   @file ESP8266_Router.ino

   @author Rok Rodic (http://www.rodic.si/)
   @date 2017-08-30

   ROSA BootCon alias Router reseter
   Select: NODEMCU 1.0 4M(1M SPIFFS) 80MHz 
   Select: (for Sonoff S26) Generic ESP8266 Module, Crystal freq 26MHz, flashmode DOUT!!!!, freq 40MHz, CPU 80MHz, flash 1M(64k SPIFFS or none), 
    There is a button attached to GPIO0 and two LEDs, a green one connected to GPIO13 and a red one to the GPIO12, like the relay, so whenever the relay is closed the LED will be lit.
    Press button when connecting Vcc in order to program!!!
    GPIO0  button
    GPIO1  rxd
    GPIO3  txd
    GPIO12 red led and Relay (0 = Off, 1 = On)
    GPIO13 green led (0 = On, 1 = Off)
    esptool.py --port COM3 write_flash 0x00000 firmware.bin
    esptool.py --port COM3 erase_flash

   changelog:
    - ArduinoOTA support
    - WifiManager for network setup
    - SSDP for automatic detection
    - button reset
    - MDNS + http firmware upload
    - watchdog
    - basic logic / timings
*/
#define CheckPeriod 5*60*1000 //in milliseconds
#define ResetPeriod 15*60*1000 //in milliseconds
#define TimeToWaitForRouterReset 2*60*1000 //in milliseconds

const char* ConfigPassword = "password1";
const String TargetHTTP = "http://plain-text-ip.com/";
const char* firmware_update_path = "/firmware";
char firmware_update_username[] = "admin";
char firmware_update_password[] = "admin";
const char* host = "RouterRebooter";

  #include <ESP8266WiFi.h>                  //ESP8266 Core WiFi Library
  #include <WiFiClient.h>
  #include <DNSServer.h>                    //Local DNS Server used for redirecting all requests to the configuration portal
  #include <ESP8266WebServer.h>             //Local WebServer used to serve the configuration portal
  #include <WiFiManager.h>                  //https://github.com/tzapu/WiFiManager WiFi Configuration Magic  
  #include <ESP8266HTTPClient.h>
  #include <ESP8266SSDP.h>
  #include <ESP8266mDNS.h>
  #include <ESP8266HTTPUpdateServer.h>
  //#include <ESP8266NetBIOS.h>
  //#include <ESP8266LLMNR.h>
  //#include <Arduino.h>
  //#include <GDBStub.h>
  #include <ESP8266WiFiMulti.h>
  #include <ArduinoOTA.h>
  #include "Bounce2.h"

const char* Intro_string = "Sonoff S26 router rebooter";
#define KeyPin 0
#define ResetPin 12       /* red LED and relay */
#define LEDPin 13         /* green LED */
unsigned long nextTime = 0;
unsigned long LastConnect = 2*60*1000; //in milliseconds
#define DBG_PORT Serial
WiFiManager wifiManager;
ESP8266WiFiMulti WiFiMulti;
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
HTTPClient http;
#define OFF 1 //for LED
#define ON  0 //for LED
#define ROFF 0 //for RELAY
#define RON  1 //for RELAY
unsigned long currentMillis;
unsigned long RestartTime = 0;
const int RELAY_PIN = ResetPin;
const int BUTTON_PIN = KeyPin;
const int LED_PIN = LEDPin;
int led_state = LOW;
Bounce debouncer = Bounce();
String IPString = "";
String StrA = "<!DOCTYPE html><html><body><h2>RouterRebooter - A very cheap router resetter</h2><a href=\"/firmware\">Firmware update</a>.</br></br><a href=\"http://www.rodic.si/\">Author's homepage</a>.</body></html>";

void setup() {
  DBG_PORT.begin(115200);
  DBG_PORT.println();
 // DBG_PORT.setDebugOutput(true);
  DBG_PORT.println(F("Booting"));
  
  ESP.wdtEnable(8000);
  ESP.wdtFeed();
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, RON);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, ON);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  debouncer.attach(BUTTON_PIN, INPUT_PULLUP);
  debouncer.interval(25); // interval in ms

  DBG_PORT.printf("Sketch size: %u\n", ESP.getSketchSize());
  DBG_PORT.printf("Free size: %u\n", ESP.getFreeSketchSpace());
  DBG_PORT.printf("Heap: %u\n", ESP.getFreeHeap());
  DBG_PORT.printf("Boot Vers: %u\n", ESP.getBootVersion());
  DBG_PORT.printf("CPU: %uMHz\n", ESP.getCpuFreqMHz());
  DBG_PORT.printf("SDK: %s\n", ESP.getSdkVersion());
  DBG_PORT.printf("Chip ID: %u\n", ESP.getChipId());
  DBG_PORT.printf("Flash ID: %u\n", ESP.getFlashChipId());
  DBG_PORT.printf("Flash Size: %u\n", ESP.getFlashChipRealSize());
  DBG_PORT.printf("Vcc: %u\n", ESP.getVcc());
  DBG_PORT.println(Intro_string);
  DBG_PORT.print(F("Chip ID: 0x"));
  DBG_PORT.println(ESP.getChipId(), HEX);
  DBG_PORT.print(F("Reset reason: "));
  DBG_PORT.println(ESP.getResetReason());
  DBG_PORT.println();
  DBG_PORT.println("Starting WiFi...");
  
 /* 
  *  If ESP is freshly powered on flash green led for 20 seconds 
  *  If power button is pressed during this time the saved wifi credentials will be erased and the wifimanager portal started
  *  If you are testing on a nodemcu board it seems to reset the board after power on so it shows a reason of "External System"
  */
  int delay_time = 250;

  if (ESP.getResetReason() == "Power on" || ESP.getResetReason() == "External System") {
    for (int i = 1; i <= 80; i++) {    
       debouncer.update();
       
       if ( debouncer.fell() ) {  // Call code if button transitions from HIGH to LOW
         wifiManager.resetSettings();
         break;
       }
       led_state = !led_state; // Toggle LED state
       digitalWrite(LED_PIN,led_state);
       ESP.wdtFeed();
       yield();
       delay(delay_time);
      }
      
  }
  WiFi.hostname(host);              // Set hostname so it shows in router page instead of ESP_xxxxxx
  wifiManager.setTimeout(180);
  wifiManager.autoConnect("ROUTER-REBOOTER", ConfigPassword);

  DBG_PORT.print("WIFI connecting");

  http.setReuse(true);
  byte cnt = 0;
  while ((WiFi.status() != WL_CONNECTED) && (cnt < 100)) {
    delay(500);
    DBG_PORT.print(".");
    cnt++;
    yield();
    ESP.wdtFeed();
  }
  
  DBG_PORT.print(F("IP address: "));
  IPString = WiFi.localIP().toString();
  DBG_PORT.println(IPString);

  DBG_PORT.printf("Starting ArduinoOTA...\n");
  ArduinoOTA.setHostname(host);
  ArduinoOTA.setPassword(firmware_update_password);

  ArduinoOTA.onStart([]() {
    DBG_PORT.println("Start");
    digitalWrite(LED_PIN,ON);
  });
  ArduinoOTA.onEnd([]() {
    DBG_PORT.println("\nEnd");
    digitalWrite(LED_PIN,OFF);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    DBG_PORT.printf("Progress: %u%%\r", (progress / (total / 100)));
    led_state = !led_state; // Toggle LED state
    digitalWrite(LED_PIN,led_state);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    DBG_PORT.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) DBG_PORT.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) DBG_PORT.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) DBG_PORT.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) DBG_PORT.println("Receive Failed");
    else if (error == OTA_END_ERROR) DBG_PORT.println("End Failed");
  });
  ArduinoOTA.begin();
  DBG_PORT.println("OTA ready");


  httpServer.on("/index.html", [](){
    httpServer.send(200, "text/html", StrA);
  });
  httpServer.on("/", [](){
    httpServer.send(200, "text/html", StrA);
  }); 
  httpServer.on("/description.xml", HTTP_GET, [](){
    SSDP.schema(httpServer.client());
  });

  DBG_PORT.printf("Starting MDNS...\n");
  MDNS.begin(host);
  httpUpdater.setup(&httpServer, firmware_update_path, firmware_update_username, firmware_update_password);

  MDNS.addService("http", "tcp", 80);
  DBG_PORT.printf("HTTPUpdateServer ready! Open http://%s.local%s in your browser and login with username '%s' and password '%s'\n", host, firmware_update_path, firmware_update_username, firmware_update_password);

  httpServer.begin();

  DBG_PORT.printf("Starting SSDP...\n");
  SSDP.setSchemaURL("description.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("Router Rebooter");
  SSDP.setSerialNumber("000000441452");
  SSDP.setURL("index.html");
  SSDP.setModelName("Sonoff");
  SSDP.setModelNumber("S26G");
  SSDP.setModelURL("https://sonoff.tech/product/wifi-smart-plugs/s26");
  SSDP.setManufacturer("Rok Rodic");
  SSDP.setManufacturerURL("http://www.rodic.si");
  SSDP.setDeviceType("upnp:rootdevice");
  SSDP.begin();

  DBG_PORT.print(F("DONE\n"));
  digitalWrite(LED_PIN, OFF);
}

void loop() {
  currentMillis = millis();

  if (currentMillis > nextTime) {
    if((WiFiMulti.run() == WL_CONNECTED)) {
      digitalWrite(LED_PIN, ON);
      DBG_PORT.println("IP address: ");
      DBG_PORT.println(WiFi.localIP());
      DBG_PORT.print("[HTTP] begin...\n");
      http.begin(TargetHTTP);
      //DBG_PORT.print("[HTTP] GET...\n");
      int httpCode = http.GET(); // httpCode will be negative on error
      if (httpCode > 0) {
        if (currentMillis > (0xFFFFFFFF - CheckPeriod)) nextTime = 0; else nextTime = currentMillis + CheckPeriod; //To avoid overflow
        if (currentMillis > (0xFFFFFFFF - ResetPeriod)) LastConnect = 0; else LastConnect = currentMillis; //To avoid overflow
        //DBG_PORT.printf("[HTTP] GET... code: %d, got: ", httpCode);
        if (httpCode == HTTP_CODE_OK) {
          String payload = http.getString();
          //DBG_PORT.println(payload);
        }
        DBG_PORT.printf("[NEXT] Scan scheduled in %d seconds.", (CheckPeriod/1000));
      } else {
        DBG_PORT.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
        if (currentMillis > (0xFFFFFFFF - CheckPeriod)) nextTime = 0; else nextTime = currentMillis + (CheckPeriod / 100);
      }
      http.end();
      digitalWrite(LED_PIN, OFF);
    }
  }

  debouncer.update();

  // Trigger restart
  if ((currentMillis > (LastConnect + ResetPeriod)) || (debouncer.fell())) {
    digitalWrite(RELAY_PIN, ROFF);
    delay(100);
    DBG_PORT.println(F("RESETTING!!!"));
    //WiFi.forceSleepBegin();
    //WiFiOff();
    unsigned int cnt = 0;
    unsigned long qwe = millis() + TimeToWaitForRouterReset;
    while (millis() < qwe) {
      //DBG_PORT.println(F("."));
      ESP.wdtFeed();
      yield();
      delay(500);
      cnt++;
      if ((cnt % 2) == 0) digitalWrite(LED_PIN, OFF); else digitalWrite(LED_PIN, ON);
      if (cnt > 10) digitalWrite(RELAY_PIN, RON);
    }
    ESP.restart();
  }
  
  ArduinoOTA.handle();  
  httpServer.handleClient();
  ESP.wdtFeed();
  yield();
}
