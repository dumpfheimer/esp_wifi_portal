// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#include "wifi_mgr.h"

#if defined(ESP8266)
#if WIFI_MGR_USE_MDNS
MDNSResponder wifiMgrMdns;
#endif
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
#if defined(ESP32)
// pause between re-issued connect attempts in waitForWifi(), and how many
// failed associations to tolerate before giving the attempt up early - see
// the comment in waitForWifi()
#define WIFI_MGR_ESP32_RETRY_BACKOFF_MS 500
#define WIFI_MGR_ESP32_MAX_RETRIES 8
#endif
unsigned long wifiMgrWaitForScanMs = 30000; // 30s
unsigned long wifiMgrScanCount = 0;
unsigned long wifiMgrConnectCount = 0;
unsigned long wifiMgrInvalidRSSICount = 0;
unsigned long wifiMgrInvalidIPCount = 0;
unsigned long wifiMgrPostStartedServerCount = 0;
volatile bool wifiMgrConnecting = false;
uint8_t wifiMgrRebootAfterUnsuccessfullTries = 0;
uint8_t wifiMgrUnsuccessfullTries = 0;
// Set by the /wifiMgr/reconnect handler, executed by loopWifi():
// connectToWifi() blocks for up to ~90s and its wait loops re-enter
// server.handleClient() via the app's yield callbacks. Running it inside an
// HTTP handler makes the web server re-enter itself mid-request, which can
// crash the device.
volatile bool wifiMgrReconnectRequested = false;
// a connection only clears the failure counter after surviving 60s: a
// connect-then-collapse loop (e.g. heap-starved reassociation) used to reset
// the counter on every brief association, so the
// rebootAfterUnsuccessfullTries escape hatch never fired
unsigned long wifiMgrConnectedSince = 0;
bool wifiMgrConnectionWasStable = false;

#if defined(ESP32)
static bool wifiMgrServerStarted = false;
#if WIFI_MGR_USE_MDNS
static bool mdnsInitialized = false;
#endif

// Auto-reconnect is off, so the SDK never retries an association on its own:
// every STA_DISCONNECTED event means "the current attempt is dead until someone
// calls connect again". The event handler only records it; waitForWifi() reacts
// from the calling task. ASSOC_LEAVE is skipped - that is a deliberate
// WiFi.disconnect() by this lib (or the core), not a failure.
static volatile uint16_t wifiMgrStaDropCount = 0;
static volatile uint8_t wifiMgrStaDropReason = 0;

static void wifiMgrOnStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (reason == WIFI_REASON_ASSOC_LEAVE) return;
    wifiMgrStaDropReason = reason;
    wifiMgrStaDropCount = wifiMgrStaDropCount + 1;
}
#endif

int8_t badRSS = -70;
const char* wifiMgrSSID = nullptr;
const char* wifiMgrPW = nullptr;
const char* wifiMgrHN = nullptr;

void (*loopFunctionPointer)(void) = nullptr;
void (*wifiMgrNotifyNoWifiCallback)(void) = nullptr;

XWebServer *wifiMgrServer = nullptr;

boolean waitForWifi(unsigned long timeout) {
    unsigned long start = millis();
#if defined(ESP32)
    unsigned long lastRetryAt = 0;
    uint8_t retries = 0;
#endif
    while ((millis() - start) < timeout) {
        wl_status_t s = WiFi.status();
        if (s == WL_CONNECTED) return true;
#if defined(ESP8266)
        // WiFi.status() queries the live SDK station state and the SDK retries
        // internally, so these are reported only once it has really given up:
        // terminal failures, no point waiting out the timeout.
        if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL || s == WL_WRONG_PASSWORD) return false;
#elif defined(ESP32)
        // On ESP32 with auto-reconnect off, a single STA_DISCONNECTED - however
        // transient the reason (201 NO_AP_FOUND, 202 AUTH_FAIL, 203 ASSOC_FAIL,
        // routine noise right after a scan when connecting with an explicit
        // channel + BSSID) - ends the attempt for good: the SDK goes silent and
        // WiFi.status() keeps whatever it latched. So instead of watching the
        // status, watch the events and re-issue the connect ourselves, with a
        // small backoff. A network that is genuinely down or rejecting us fails
        // every association, so give up after a handful of retries rather than
        // burning the whole timeout.
        if (wifiMgrStaDropCount > 0 && (millis() - lastRetryAt) > WIFI_MGR_ESP32_RETRY_BACKOFF_MS) {
            wifiMgrStaDropCount = 0;
            lastRetryAt = millis();
            retries++;
            if (retries > WIFI_MGR_ESP32_MAX_RETRIES) {
                WIFI_MGR_LOG("giving up after %u failed associations (last reason %u)", retries - 1, wifiMgrStaDropReason);
                return false;
            }
            WIFI_MGR_LOG("association failed (reason %u), reconnect %u/%u", wifiMgrStaDropReason, retries, WIFI_MGR_ESP32_MAX_RETRIES);
            WiFi.reconnect();
        }
#endif
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        // delay() yields to the SDK on ESP8266 and feeds the idle/task WDT on
        // ESP32, where yield() is a bare vPortYield() that never lets the
        // priority-0 idle task run.
        delay(1);
    }
    return WiFi.isConnected();
}

void delayAndLoop(unsigned long delayMS) {
    unsigned long start = millis();
    while (millis() - start < delayMS) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        delay(1);
    }
}

void waitForDisconnect(unsigned long timeout) {
    unsigned long waitForConnectStart = millis();
    while (WiFi.status() == WL_CONNECTED && (millis() - waitForConnectStart) < timeout) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        delay(1);
    }
}

void wifiNotifyUnsuccessfullTry() {
    wifiMgrUnsuccessfullTries += 1;
    if (wifiMgrRebootAfterUnsuccessfullTries > 0 && wifiMgrUnsuccessfullTries >= wifiMgrRebootAfterUnsuccessfullTries) {
        wifiMgrCleanup(); // Clean up resources before restart
        ESP.restart();
    }
}

void connectToWifi() {
    if (wifiMgrConnecting) return;
    wifiMgrConnecting = true;
    // a previous connection that died before proving stable counts as a
    // failed try - see wifiMgrConnectedSince
    if (wifiMgrConnectedSince != 0 && !wifiMgrConnectionWasStable) {
        wifiNotifyUnsuccessfullTry();
    }
    wifiMgrConnectedSince = 0;
    WIFI_MGR_LOG("connectToWifi() start, free heap %lu", (unsigned long)ESP.getFreeHeap());
    //if (wifiMgrServer != nullptr) wifiMgrServer->stop();
    //if (wifiMgrServer != nullptr) wifiMgrServer->close();
#if WIFI_MGR_USE_MDNS
#if defined(ESP8266)
    if (wifiMgrMdns.isRunning()) wifiMgrMdns.end();
#elif defined(ESP32)
    if (mdnsInitialized) {
        mdns_free();
        mdnsInitialized = false;
    }
#endif
#endif

    WiFi.disconnect(true);
    waitForDisconnect(3000);
    WiFi.mode(WIFI_STA);

    wifiMgrScanCount++;

    int n = WiFi.scanNetworks(true, false, 0);

    unsigned long waitForScanStart = millis();

    while (WiFi.scanComplete() == -1 && (millis() - waitForScanStart) < wifiMgrWaitForScanMs) {
        if (loopFunctionPointer != nullptr) loopFunctionPointer();
        delay(1);
    }
    n = WiFi.scanComplete();
    WIFI_MGR_LOG("scan done: %d networks (looking for '%s')", n, wifiMgrSSID != nullptr ? wifiMgrSSID : "(null)");

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

        if (bestRSSI != -999) {
            WIFI_MGR_LOG("connecting to %02x:%02x:%02x:%02x:%02x:%02x ch %ld rssi %ld, timeout %lums",
                         bestBSSID[0], bestBSSID[1], bestBSSID[2], bestBSSID[3], bestBSSID[4], bestBSSID[5],
                         (long)bestChannel, (long)bestRSSI, wifiMgrWaitForConnectMs);
#if defined(ESP32)
            // forget disconnect events from before this attempt (e.g. the drop
            // that triggered this reconnect) - only failures of THIS association
            // may re-issue the connect in waitForWifi()
            wifiMgrStaDropCount = 0;
#endif
            WiFi.begin(wifiMgrSSID, wifiMgrPW, bestChannel, bestBSSID);
            bool connected = waitForWifi(wifiMgrWaitForConnectMs);
            wifiMgrConnectCount++;
            WIFI_MGR_LOG("connect result: %d, status %d, ip %s", connected ? 1 : 0,
                         (int)WiFi.status(), WiFi.localIP().toString().c_str());
            if (!connected) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                waitForDisconnect(3000);
                wifiNotifyUnsuccessfullTry();
            } else {
                wifiMgrConnectedSince = millis();
                wifiMgrConnectionWasStable = false;
#if WIFI_MGR_USE_MDNS
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
#endif

#if defined(ESP8266)
                // status 0 means the server is closed - so not running (I think)
                if (wifiMgrServer != nullptr && wifiMgrServer->getServer().status() == 0) wifiMgrServer->begin();
#elif defined(ESP32)
                // wifiMgrServer is null when the app never called wifiMgrExpose()
                // and the portal runs its own server - dereferencing it here
                // crashed the device the moment a connect succeeded.
                // WebServer::begin() also close()es and rebinds the listening
                // socket, so only do it once instead of on every reconnect.
                if (wifiMgrServer != nullptr && !wifiMgrServerStarted) {
                    wifiMgrServer->begin();
                    wifiMgrServerStarted = true;
                    WIFI_MGR_LOG("web server started");
                }
#endif
                wifiMgrLastNonShitRSS = millis();
                wifiMgrInvalidRSSISince = 0;
                wifiMgrInvalidIPSince = 0;
            }
        } else {
            WIFI_MGR_LOG("configured SSID not found in scan results");
            wifiNotifyUnsuccessfullTry();
        }
    } else {
        WIFI_MGR_LOG("scan returned no networks (%d)", n);
        wifiNotifyUnsuccessfullTry();
    }
    wifiMgrLastScan = millis();
    WiFi.scanDelete();

    wifiMgrConnecting = false;
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
    static bool staDropHandlerRegistered = false;
    if (!staDropHandlerRegistered) {
        WiFi.onEvent(wifiMgrOnStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        staDropHandlerRegistered = true;
    }
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
    if (wifiMgrReconnectRequested) {
        wifiMgrReconnectRequested = false;
        connectToWifi();
    }
    if (!WiFi.isConnected()) {
        if (wifiMgrLastScan == 0 || (millis() - wifiMgrLastScan) > 10000) {
            connectToWifi();
        }
        if (!WiFi.isConnected() && wifiMgrNotifyNoWifiCallback != nullptr && (wifiMgrlastConnected == 0 ? millis() : millis() - wifiMgrlastConnected) > wifiMgrNotifyNoWifiTimeout) {
            wifiMgrNotifyNoWifiCallback();
        }
    }
    if (WiFi.isConnected()) {
        if (millis() - wifiMgrlastConnected > 1000) {
            wifiMgrlastConnected = millis();

            if (!wifiMgrConnectionWasStable && wifiMgrConnectedSince != 0 && (millis() - wifiMgrConnectedSince) > 60000) {
                wifiMgrConnectionWasStable = true;
                wifiMgrUnsuccessfullTries = 0;
            }

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
                if (wifiMgrInvalidIPSince == 0) {
                    wifiMgrInvalidIPSince = millis();
                } else if (millis() - wifiMgrInvalidIPSince > wifiMgrInvalidIPTimeout) {
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
    // deferred to loopWifi() via the flag - see wifiMgrReconnectRequested
    wifiMgrServer->send(200, "text/plain", "reconnecting");
    wifiMgrReconnectRequested = true;
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

bool wifiMgrIsConnecting() {
    return wifiMgrConnecting;
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
#if WIFI_MGR_USE_MDNS
#if defined(ESP8266)
    if (wifiMgrMdns.isRunning()) wifiMgrMdns.end();
#elif defined(ESP32)
    if (mdnsInitialized) {
        mdns_free();
        mdnsInitialized = false;
    }
#endif
#endif
}
