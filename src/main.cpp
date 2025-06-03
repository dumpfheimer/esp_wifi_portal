// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#include "main.h"

XWebServer test(80);

void hihi() {
    test.send(200, "text/html", "hihi");
}
void setup() {
    Serial.begin(9600);
    wifiMgrExpose(&test);
    wifiMgrPortalSetup(true, "Test-", "p0rtal123");
    wifiMgrPortalAddConfigEntry("Chris Test", "test", PortalConfigEntryType::STRING, false, false);

    test.on("/hihi", hihi);
    test.begin();
}

void loop() {
    Serial.println("loop");
    if (wifiMgrPortalLoop()) {
        Serial.println("inner");
        test.handleClient();
    }
}
