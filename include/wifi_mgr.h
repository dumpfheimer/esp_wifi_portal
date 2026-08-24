// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#if __has_include("my_config.h")
#include "my_config.h"
#endif

#if __has_include("configuration.h")
#include "configuration.h"
#endif

// mDNS is compiled in by default; define WIFI_MGR_DISABLE_MDNS (in my_config.h,
// configuration.h or build_flags) to compile it out entirely.
#if !defined(WIFI_MGR_DISABLE_MDNS)
#define WIFI_MGR_USE_MDNS 1
#else
#define WIFI_MGR_USE_MDNS 0
#endif

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#if WIFI_MGR_USE_MDNS
#include <ESP8266mDNS.h>
#endif
#define XWebServer ESP8266WebServer
#define XWiFiClass ESP8266WiFiClass
#elif defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#if WIFI_MGR_USE_MDNS
#include <ESPmDNS.h>
#endif
#define XWebServer WebServer
#define XWiFiClass WiFiClass
#include "Update.h"
#else
#error "This hardware is not supported"
#endif

// Define WIFI_MGR_DEBUG (in my_config.h, configuration.h or build_flags) to get
// the connection state machine logged to Serial. Serial.begin() is the app's job.
#if defined(WIFI_MGR_DEBUG)
#define WIFI_MGR_LOG(fmt, ...) Serial.printf("[wifiMgr] " fmt "\n", ##__VA_ARGS__)
#else
#define WIFI_MGR_LOG(fmt, ...) do {} while (0)
#endif

void setupWifi(const char* SSID, const char* password);
void setupWifi(const char* SSID, const char* password, const char* hostname);
void setupWifi(const char* SSID, const char* password, const char* hostname, unsigned long tolerateBadRSSms, unsigned long waitForConnectMs);
void setupWifi(const char* SSID, const char* password, const char* hostname, unsigned long tolerateBadRSSms, unsigned long waitForConnectMs, unsigned long wifiMgrWaitForScanMs, unsigned long rescanInterval);
void loopWifi();
void wifiMgrExpose(XWebServer *server_);
XWebServer* wifiMgrGetWebServer();
bool wifiMgrIsConnecting(); // true while connectToWifi() is in progress
void wifiMgrSetRebootAfterUnsuccessfullTries(uint8_t _wifiMgrRebootAfterUnsuccessfullTries);
void wifiMgrSetBadRSSI(int8_t rssi);
void wifiMgrNotifyNoWifi(void (*wifiMgrNotifyNoWifiCallbackArg)(void), unsigned long timeout);
void setLoopFunction(void (*loopFunctionPointerArg)(void));
void wifiMgrCleanup(); // Function to clean up resources before restart
void setRescanInterval(unsigned long rescanInterval);

#endif //WIFI_MGR_H
