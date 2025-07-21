// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#include "wifi_mgr.h"

#if defined(ESP8266)
MDNSResponder wifiMgrMdns;
ESP8266HTTPUpdateServer updateServer;
#elif defined(ESP32)
#endif

unsigned long wifiMgrLastNonShitRSS = 0;
unsigned long wifiMgrlastConnected = 0;
unsigned long wifiMgrInvalidRSSISince = 0;
unsigned long wifiMgrInvalidRSSITimeout = 30 * 1000;
unsigned long wifiMgrInvalidIPSince = 0;
unsigned long wifiMgrInvalidIPTimeout = 10 * 1000;
unsigned long wifiMgrNotifyNoWifiTimeout = 600 * 1000; // 10m
unsigned long wifiMgrTolerateBadRSSms = 300 * 1000; // 5m
unsigned long wifiMgrRescanInterval = 3600 * 1000; // 1h
unsigned long wifiMgrLastScan = 0;
unsigned long wifiMgrWaitForConnectMs = 30000; // 30s
unsigned long wifiMgrWaitForScanMs = 30000; // 30s
unsigned long wifiMgrScanCount = 0;
unsigned long wifiMgrConnectCount = 0;
unsigned long wifiMgrInvalidRSSICount = 0;
unsigned long wifiMgrInvalidIPCount = 0;
unsigned long wifiMgrPostStartedServerCount = 0;
uint8_t wifiMgrRebootAfterUnsuccessfullTries = 0;
uint8_t wifiMgrUnsuccessfullTries = 0;

#if defined(ESP32)
static bool mdnsInitialized = false;
#endif

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
    if (mdnsInitialized) {
        mdns_free();
        mdnsInitialized = false;
    }
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

    if (n > 0) {
        String ssid;
        uint8_t encryptionType;
        int32_t RSSI;
        uint8_t *BSSID;
        int32_t channel;
        bool isHidden = false;

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
        WiFi.scanDelete();

        wifiMgrLastScan = millis();

        if (bestRSSI != -999) {
            WiFi.begin(wifiMgrSSID, wifiMgrPW, bestChannel, bestBSSID);
            uint8_t status = WiFi.waitForConnectResult();
            wifiMgrConnectCount++;
            if (status != WL_CONNECTED) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                waitForDisconnect(3000);
                wifiMgrUnsuccessfullTries += 1;
                if (wifiMgrUnsuccessfullTries >= wifiMgrRebootAfterUnsuccessfullTries) {
                    wifiMgrCleanup(); // Clean up resources before restart
                    ESP.restart();
                }
            } else {
                wifiMgrUnsuccessfullTries = 0;
                if (wifiMgrHN != nullptr && strlen(wifiMgrHN) > 0) {
#if defined(ESP8266)
                    if (wifiMgrMdns.isRunning()) wifiMgrMdns.end();
                    wifiMgrMdns.begin(wifiMgrHN, WiFi.localIP());
#elif defined(ESP32)
                    esp_err_t err = mdns_init();
                    if (err == ESP_OK) {
                        mdns_hostname_set(wifiMgrHN);
                        mdnsInitialized = true;
                    }
#endif
                }

#if defined(ESP8266)
                // status 0 means the server is closed - so not running (I think)
                if (wifiMgrServer != nullptr && wifiMgrServer->getServer().status() == 0) wifiMgrServer->begin();
#elif defined(ESP32)
                wifiMgrServer->begin();
#endif
                wifiMgrLastNonShitRSS = millis();
                wifiMgrInvalidRSSISince = 0;
                wifiMgrInvalidIPSince = 0;
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

void setRescanInterval(unsigned long rescanInterval) {
    wifiMgrRescanInterval = rescanInterval;
}

void onOTAEnd(bool success) {
    if (success) {
        wifiMgrCleanup(); // Clean up resources before restart
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
    if (wifiMgrSSID != nullptr) {
        free((void*)wifiMgrSSID);
        wifiMgrSSID = nullptr;
    }
    if (wifiMgrPW != nullptr) {
        free((void*)wifiMgrPW);
        wifiMgrPW = nullptr;
    }
    if (wifiMgrHN != nullptr) {
        free((void*)wifiMgrHN);
        wifiMgrHN = nullptr;
    }

    wifiMgrSSID = (SSID != nullptr && strlen(SSID) > 0) ? strdup(SSID) : nullptr;
    wifiMgrPW = (password != nullptr && strlen(password) > 0) ? strdup(password) : nullptr;
    wifiMgrHN = (hostname != nullptr && strlen(hostname) > 0) ? strdup(hostname) : nullptr;
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
            } else if (rss > 0) {
                if (wifiMgrInvalidRSSISince == 0) {
                    wifiMgrInvalidRSSISince = millis();
                } else {
                    if (millis() - wifiMgrInvalidRSSISince > wifiMgrInvalidRSSITimeout) {
                        wifiMgrInvalidRSSICount++;
                        connectToWifi();
                    }
                }
            } else {
                wifiMgrInvalidRSSISince = 0;
                wifiMgrLastNonShitRSS = millis();
            }
            if (WiFi.localIP().toString() == "0.0.0.0") {
                if (millis() - wifiMgrInvalidIPSince > wifiMgrInvalidIPTimeout) {
                    wifiMgrInvalidIPCount++;
                    connectToWifi();
                }
            } else {
                wifiMgrInvalidIPSince = 0;
            }

#if defined(ESP8266)
            if (wifiMgrServer != nullptr && wifiMgrServer->getServer().status() == 0) {
                wifiMgrPostStartedServerCount++;
                wifiMgrServer->begin();
            }
#endif

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
    char buffer[500];
    int len = 0;

    len += snprintf(buffer + len, sizeof(buffer) - len, "ssid: %s\n", WiFi.SSID().c_str());
    len += snprintf(buffer + len, sizeof(buffer) - len, "connected: %d\n", WiFi.isConnected());
    len += snprintf(buffer + len, sizeof(buffer) - len, "bssid: %s\n", WiFi.BSSIDstr().c_str());
    len += snprintf(buffer + len, sizeof(buffer) - len, "rssi: %d\n", WiFi.RSSI());
    len += snprintf(buffer + len, sizeof(buffer) - len, "uptime: %lus\n", millis()/1000);
    len += snprintf(buffer + len, sizeof(buffer) - len, "last scan: %lus\n", (millis() - wifiMgrLastScan)/1000);
    len += snprintf(buffer + len, sizeof(buffer) - len, "scanned: %lu times\n", wifiMgrScanCount);
    len += snprintf(buffer + len, sizeof(buffer) - len, "connected: %lu times\n\n", wifiMgrConnectCount);
    len += snprintf(buffer + len, sizeof(buffer) - len, "free heap: %du\n", ESP.getFreeHeap());
    len += snprintf(buffer + len, sizeof(buffer) - len, "reconnects invalid IP: %lu\n", wifiMgrInvalidIPCount);
    len += snprintf(buffer + len, sizeof(buffer) - len, "reconnects invalid RSSI: %lu\n", wifiMgrInvalidRSSICount);
    len += snprintf(buffer + len, sizeof(buffer) - len, "server restarts (post): %lu\n", wifiMgrPostStartedServerCount);
#if defined(ESP8266)
    len += snprintf(buffer + len, sizeof(buffer) - len, "heap fragmentation: %d", ESP.getHeapFragmentation());
#endif
;
    wifiMgrServer->send(200, "text/plain", buffer);
}

void restart() {
    wifiMgrServer->send(200, "text/plain", "restarting");
    unsigned long start = millis();
    while (millis() - start < 500) yield();
    wifiMgrCleanup(); // Clean up resources before restart
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

#if defined(ESP8266)
        updateServer.setup(wifiMgrServer, "/update");
#elif defined(ESP32)
        static bool authenticate = false;
        static char *_username = nullptr;
        static char *_password = nullptr;
        // this portion was copied from ElegantOTA 2 and was provided with the MIT license it does not seem to be the original source, though
        // MIT License
        //
        //Copyright (c) 2019 Ayush Sharma
        //
        //Permission is hereby granted, free of charge, to any person obtaining a copy
        //of this software and associated documentation files (the "Software"), to deal
        //in the Software without restriction, including without limitation the rights
        //to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
        //copies of the Software, and to permit persons to whom the Software is
        //furnished to do so, subject to the following conditions:
        //
        //The above copyright notice and this permission notice shall be included in all
        //copies or substantial portions of the Software.
        //
        //THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
        //IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
        //FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
        //AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
        //LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
        //OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
        //SOFTWARE.
        // start of licensed code
        wifiMgrServer->on("/update", HTTP_POST, [&](){
            if (authenticate && !wifiMgrServer->authenticate(_username, _password)) {
                return;
            }
            wifiMgrServer->sendHeader("Connection", "close");
            wifiMgrServer->send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            #if defined(ESP32)
                // Needs some time for Core 0 to send response
                delay(100);
                yield();
                delay(100);
            #endif
            wifiMgrCleanup(); // Clean up resources before restart
            ESP.restart();
        }, [&](){
            // Actual OTA Download
            if (authenticate && !wifiMgrServer->authenticate(_username, _password)) {
                return;
            }

            HTTPUpload& upload = wifiMgrServer->upload();
            if (upload.status == UPLOAD_FILE_START) {
                Serial.setDebugOutput(true);
                Serial.printf("Update Received: %s\n", upload.filename.c_str());
                if (upload.name == "filesystem") {
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) { //start with max available size
                        Update.printError(Serial);
                    }
                } else {
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { //start with max available size
                        Update.printError(Serial);
                    }
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) { //true to set the size to the current progress
                    Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
                } else {
                    Update.printError(Serial);
                }
                Serial.setDebugOutput(false);
            } else {
                Serial.printf("Update Failed Unexpectedly (likely broken connection): status=%d\n", upload.status);
            }
        });
        // end of licensed code
#endif
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

// Function to clean up resources before restart
void wifiMgrCleanup() {
    // Free allocated strings
    if (wifiMgrSSID != nullptr) {
        free((void*)wifiMgrSSID);
        wifiMgrSSID = nullptr;
    }
    if (wifiMgrPW != nullptr) {
        free((void*)wifiMgrPW);
        wifiMgrPW = nullptr;
    }
    if (wifiMgrHN != nullptr) {
        free((void*)wifiMgrHN);
        wifiMgrHN = nullptr;
    }
    
    // Disconnect WiFi
    WiFi.disconnect(true);
    
    // Free MDNS resources
#if defined(ESP8266)
    if (wifiMgrMdns.isRunning()) wifiMgrMdns.end();
#elif defined(ESP32)
    if (mdnsInitialized) {
        mdns_free();
        mdnsInitialized = false;
    }
#endif
}
