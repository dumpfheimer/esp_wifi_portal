// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#include "wifi_mgr.h"

#if defined(ESP8266)
MDNSResponder wifiMgrMdns;
#elif defined(ESP32)
#endif

unsigned long wifiMgrLastNonShitRSS = 0;
unsigned long wifiMgrlastConnected = 0;
unsigned long wifiMgrInvalidRSSISince = 0;
unsigned long wifiMgrInvalidRSSITimeout = 30 * 1000;
unsigned long wifiMgrNotifyNoWifiTimeout = 600 * 1000; // 10m
unsigned long wifiMgrTolerateBadRSSms = 300 * 1000; // 5m
unsigned long wifiMgrRescanInterval = 3600 * 1000; // 1h
unsigned long wifiMgrLastScan = 0;
unsigned long wifiMgrWaitForConnectMs = 30000; // 30s
unsigned long wifiMgrWaitForScanMs = 30000; // 30s
unsigned long wifiMgrScanCount = 0;
unsigned long wifiMgrConnectCount = 0;
uint8_t wifiMgrRebootAfterUnsuccessfullTries = 0;
uint8_t wifiMgrUnsuccessfullTries = 0;

int8_t badRSS = -70;
const char* wifiMgrSSID = nullptr;
const char* wifiMgrPW = nullptr;
const char* wifiMgrHN = nullptr;

void (*loopFunctionPointer)(void) = nullptr;
void (*wifiMgrNotifyNoWifiCallback)(void) = nullptr;

XWebServer *wifiMgrServer = nullptr;

boolean waitForWifi(unsigned long timeout) {
    unsigned long waitForConnectStart = millis();
    while (!WiFi.isConnected() && (millis() - waitForConnectStart) < timeout) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        yield();
    }
    return WiFi.isConnected();
}

void delayAndLoop(unsigned long delayMS) {
    unsigned long start = millis();
    while (millis() - start < delayMS) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        yield();
    }
}

void waitForDisconnect(unsigned long timeout) {
    unsigned long waitForConnectStart = millis();
    while (WiFi.status() == WL_CONNECTED && (millis() - waitForConnectStart) < timeout) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        yield();
    }
}

void connectToWifi() {
    //if (wifiMgrServer != nullptr) wifiMgrServer->stop();
    //if (wifiMgrServer != nullptr) wifiMgrServer->close();
#if defined(ESP8266)
    if (wifiMgrMdns.isRunning()) wifiMgrMdns.end();
#elif defined(ESP32)
    mdns_free();
#endif

    WiFi.disconnect(true);
    waitForDisconnect(3000);
    WiFi.mode(WIFI_STA);

    wifiMgrScanCount++;

    int n = WiFi.scanNetworks(true, false, 0);

    unsigned long waitForScanStart = millis();

    while (WiFi.scanComplete() < 0 && (millis() - waitForScanStart) < wifiMgrWaitForScanMs) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        yield();
    }
    n = WiFi.scanComplete();
    WiFi.scanDelete();

    if (n > 0) {
        String ssid;
        uint8_t encryptionType;
        int32_t RSSI;
        uint8_t *BSSID;
        int32_t channel;
        bool isHidden;

        uint8_t bestBSSID[6];
        int32_t bestRSSI = -999;
        int32_t bestChannel = 0;

        for (int i = 0; i < n; i++) {
#if defined(ESP8266)
            WiFi.getNetworkInfo(i, ssid, encryptionType, RSSI, BSSID, channel, isHidden);
#elif defined(ESP32)
            WiFi.getNetworkInfo(i, ssid, encryptionType, RSSI, BSSID, channel);
	    isHidden = false;
#endif

            if (!isHidden && ssid.equals(wifiMgrSSID) && RSSI > bestRSSI) {
                bestRSSI = RSSI;
                memcpy(bestBSSID, BSSID, 6);
                bestChannel = channel;
            }
        }

        wifiMgrLastScan = millis();

        if (bestRSSI != -999) {
            WiFi.begin(wifiMgrSSID, wifiMgrPW, bestChannel, bestBSSID);
            wifiMgrConnectCount++;
            if (!waitForWifi(wifiMgrWaitForConnectMs)) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                waitForDisconnect(3000);
                wifiMgrUnsuccessfullTries += 1;
                if (wifiMgrUnsuccessfullTries >= wifiMgrRebootAfterUnsuccessfullTries) {
                    ESP.restart();
                }
            } else {
                wifiMgrUnsuccessfullTries = 0;
                if (wifiMgrHN != nullptr && strlen(wifiMgrHN) > 0) {
#if defined(ESP8266)
                    if (wifiMgrMdns.isRunning()) wifiMgrMdns.end();
                    wifiMgrMdns.begin(wifiMgrHN, WiFi.localIP());
#elif defined(ESP32)
                    mdns_init();
                    mdns_hostname_set(wifiMgrHN);
#endif
                }
                // status 0 means the server is closed - so not running (I think)
                if (wifiMgrServer != nullptr && wifiMgrServer->getServer().status() == 0) wifiMgrServer->begin();
                wifiMgrLastNonShitRSS = millis();
                wifiMgrInvalidRSSISince = 0;
            }
        }
    }
    WiFi.scanDelete();
}

void setupWifi(const char* SSID, const char* password) {
    setupWifi(SSID, password, nullptr);
}

void setupWifi(const char* SSID, const char* password, const char* hostname) {
    setupWifi(SSID, password, hostname, wifiMgrTolerateBadRSSms, wifiMgrWaitForConnectMs);
}

void setupWifi(const char* SSID, const char* password, const char* hostname, unsigned long tolerateBadRSSms, unsigned long waitForConnectMs) {
    setupWifi(SSID, password, hostname, tolerateBadRSSms, waitForConnectMs, wifiMgrWaitForScanMs, wifiMgrRescanInterval);
}

void onOTAEnd(bool success) {
    if (success) {
        ESP.restart();
    }
}

void setupWifi(const char* SSID, const char* password, const char* hostname, unsigned long tolerateBadRSSms, unsigned long waitForConnectMs, unsigned long waitForScanMs, unsigned long rescanInterval) {
    WiFi.mode(WIFI_STA);
    if (hostname != nullptr) WiFi.hostname(hostname);
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);

#if defined(ESP8266)
    ESP8266WiFiClass::persistent(false);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
#elif defined(ESP32)
    WiFi.persistent(false);
    WiFi.setSleep(false);
#endif


    // Free previous values if they exist to prevent memory leaks
    if (wifiMgrSSID != nullptr) free((void*)wifiMgrSSID);
    if (wifiMgrPW != nullptr) free((void*)wifiMgrPW);
    if (wifiMgrHN != nullptr) free((void*)wifiMgrHN);

    wifiMgrSSID = SSID ? strdup(SSID) : nullptr;
    wifiMgrPW = password ? strdup(password) : nullptr;
    wifiMgrHN = hostname ? strdup(hostname) : nullptr;
    wifiMgrTolerateBadRSSms = tolerateBadRSSms;
    wifiMgrWaitForConnectMs = waitForConnectMs;
    wifiMgrWaitForScanMs = waitForScanMs;
    wifiMgrRescanInterval = rescanInterval;

    connectToWifi();
}

void loopWifi() {
    if (!WiFi.isConnected()) {
        connectToWifi();
        if (!WiFi.isConnected() && wifiMgrNotifyNoWifiCallback != nullptr && millis() - wifiMgrlastConnected > wifiMgrNotifyNoWifiTimeout) {
            wifiMgrNotifyNoWifiCallback();
        }
    }
    if (WiFi.isConnected()) {
        if (millis() - wifiMgrlastConnected > 1000) {
            wifiMgrlastConnected = millis();

            int8_t rss = WiFi.RSSI();

            if (rss < badRSS) {
                wifiMgrInvalidRSSISince = 0;
                if ((millis() - wifiMgrLastNonShitRSS) > wifiMgrTolerateBadRSSms) {
                    connectToWifi();
                }
            } /*else if (rss > 0) {
                if (wifiMgrInvalidRSSISince == 0) {
                    wifiMgrInvalidRSSISince = millis();
                } else {
                    if (millis() - wifiMgrInvalidRSSISince > wifiMgrInvalidRSSITimeout) {
                        connectToWifi();
                    }
                }
            } */else {
                wifiMgrInvalidRSSISince = 0;
                wifiMgrLastNonShitRSS = millis();
            }

            if (wifiMgrRescanInterval > 0 && (millis() - wifiMgrLastScan) > wifiMgrRescanInterval) {
                connectToWifi();
            }
        }
    }
    yield();
}

void sendRSSI() {
    wifiMgrServer->send(200, "text/plain", String(WiFi.RSSI()));
}

void isConnected() {
    wifiMgrServer->send(200, "text/plain", String(WiFi.isConnected()));
}

void ssid() {
    wifiMgrServer->send(200, "text/plain", WiFi.SSID());
}

void bssid() {
    wifiMgrServer->send(200, "text/plain", WiFi.BSSIDstr());
}

void status() {
    String s = "ssid: " + WiFi.SSID() +
            "\nconnected: " + String(WiFi.isConnected()) +
            "\nbssid: " + WiFi.BSSIDstr() +
            "\nrssi: " + String(WiFi.RSSI()) +
            "\nuptime: " + String(millis()/1000) +
            "s\nlast scan: " + String((millis() - wifiMgrLastScan)/1000) +
            "s\nscanned: " + String(wifiMgrScanCount) +
            " times\nconnected: " + String(wifiMgrConnectCount) +
            " times\n" +
            "\nfree heap: " + ESP.getFreeHeap()
#if defined(ESP8266)
            + "\nheap fragmentation: " + ESP.getHeapFragmentation()
#endif
;
    wifiMgrServer->send(200, "text/plain", s);
}

void restart() {
    wifiMgrServer->send(200, "text/plain", "restarting");
    unsigned long start = millis();
    while (millis() - start < 500) yield();
    ESP.restart();
}

void reconnect() {
    wifiMgrServer->send(200, "text/plain", "reconnecting");
    unsigned long start = millis();
    while (millis() - start < 500) yield();
    connectToWifi();
}

void wifiMgrExpose(XWebServer *wifiMgrServer_) {
    wifiMgrServer = wifiMgrServer_;
    if (wifiMgrServer != nullptr) {
        wifiMgrServer->on("/wifiMgr/rssi", sendRSSI);
        wifiMgrServer->on("/wifiMgr/isConnected", isConnected);
        wifiMgrServer->on("/wifiMgr/ssid", ssid);
        wifiMgrServer->on("/wifiMgr/bssid", bssid);
        wifiMgrServer->on("/wifiMgr/status", status);
        wifiMgrServer->on("/wifiMgr/restart", restart);
        wifiMgrServer->on("/wifiMgr/reconnect", reconnect);

        ElegantOTA.begin(wifiMgrServer);
    }
}

XWebServer* wifiMgrGetWebServer() {
    return wifiMgrServer;
}

void wifiMgrSetBadRSSI(int8_t rssi) {
    badRSS = rssi;
}

void wifiMgrSetRebootAfterUnsuccessfullTries(uint8_t _wifiMgrRebootAfterUnsuccessfullTries) {
    wifiMgrRebootAfterUnsuccessfullTries = _wifiMgrRebootAfterUnsuccessfullTries;
}

void wifiMgrNotifyNoWifi(void (*wifiMgrNotifyNoWifiCallbackArg)(void), unsigned long timeout) {
    wifiMgrNotifyNoWifiCallback = wifiMgrNotifyNoWifiCallbackArg;
    wifiMgrNotifyNoWifiTimeout = timeout;
}

void setLoopFunction(void (*loopFunctionPointerArg)(void)) {
    loopFunctionPointer = loopFunctionPointerArg;
}
