// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#ifndef WIFI_MGR_PORTAL_H
#define WIFI_MGR_PORTAL_H

#include <wifi_mgr.h>
#include <wifi_mgr_eeprom.h>

enum PortalConfigEntryType {
    STRING = 0,
    NUMBER = 1,
    BOOL = 2
};
struct PortalConfigEntry {
    PortalConfigEntryType type;
    const char* name;
    const char* eepromKey;
    bool isPassword;
    bool restartOnChange;
    PortalConfigEntry* next;
};

// Define the callback function type for on-change listeners
typedef void (*WifiMgrPortalOnChangeCallback)(int numChanges);

void wifiMgrPortalSetup(bool redirectIndex, const char* ssidPrefix, const char* password);
bool wifiMgrPortalLoop();
void wifiMgrPortalAddConfigEntry(const char* name, const char* eepromKey, PortalConfigEntryType type, bool isPassword, bool restartOnChange);
void wifiMgrPortalCleanup(); // Add cleanup function declaration

// On-change listener functions
void wifiMgrPortalAddOnChangeListener(WifiMgrPortalOnChangeCallback callback);
void wifiMgrPortalRemoveOnChangeListener(WifiMgrPortalOnChangeCallback callback);

#endif //WIFI_MGR_PORTAL_H
