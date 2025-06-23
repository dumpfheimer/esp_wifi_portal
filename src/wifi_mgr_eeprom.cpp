// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#include "wifi_mgr_eeprom.h"

//  EEPROM SETUP
// HEADER HEADER VERSION VERSION NR_ENTRIES
// FOR EACH ENTRY:
// LENGTH_OF_NAME LENGTH_OF VALUE NAME NAME NAME NAME VALUE VALUE VALUE

#define WIFI_MGR_MAX_CONFIG_ENTRIES 32

#define WIFI_MGR_EEPROM_HEADER_1 0x43
#define WIFI_MGR_EEPROM_HEADER_2 0x96
#define WIFI_MGR_EEPROM_VERSION_1 0x00
#define WIFI_MGR_EEPROM_VERSION_2 0x01

bool initialized = false;
int eepromStartAddress = 512;
int eepromSize = 1024;

struct CacheEntry {
    char* name = nullptr;
    size_t nameLen = 0;
    char* value = nullptr;
    size_t valueLen = 0;
};
CacheEntry* cache = new CacheEntry[WIFI_MGR_MAX_CONFIG_ENTRIES];

CacheEntry* nextEmptyCacheEntry() {
    for (int i = 0; i < WIFI_MGR_MAX_CONFIG_ENTRIES; i++) {
        if (cache[i].name == nullptr) return &cache[i];
    }
    return nullptr;
}

void wifiMgrConfigureEEPROM(int startAddress, int size) {
    if (initialized) return;
    eepromStartAddress = startAddress;
    eepromSize = size;
}

bool wifiMgrSetupEEPROM() {
    if (initialized) return true;
    EEPROM.begin(eepromSize);

    //EEPROM.r
    uint8_t header1 = EEPROM.read(eepromStartAddress + 0);
    uint8_t header2 = EEPROM.read(eepromStartAddress + 1);
    uint8_t version1 = EEPROM.read(eepromStartAddress + 2);
    uint8_t version2 = EEPROM.read(eepromStartAddress + 3);
    uint8_t numberOfEntries = EEPROM.read(eepromStartAddress + 4);

    if (header1 != 0x43 || header2 != 0x96 || version1 != 0x00 || version2 != 0x01) {
        // not initialized
        EEPROM.write(eepromStartAddress + 0, WIFI_MGR_EEPROM_HEADER_1);
        EEPROM.write(eepromStartAddress + 1, WIFI_MGR_EEPROM_HEADER_2);
        EEPROM.write(eepromStartAddress + 2, WIFI_MGR_EEPROM_VERSION_1);
        EEPROM.write(eepromStartAddress + 3, WIFI_MGR_EEPROM_VERSION_2);
        EEPROM.write(eepromStartAddress + 4, 0x00);
        numberOfEntries = 0;
    }

    uint16_t eepromPtr = eepromStartAddress + 5;
    for (int i = 0; i < numberOfEntries; i++) {
        uint8_t entryNameLength = EEPROM.read(eepromPtr);
        uint8_t entryValueLength = EEPROM.read(eepromPtr + 1 + entryNameLength);

        if (entryNameLength != 0 && entryValueLength != 0) {
            auto *newEntry = nextEmptyCacheEntry();
            if (newEntry == nullptr) {
                return false;  // No more space in cache
            }
            newEntry->name = new char[entryNameLength + 1];
            newEntry->name[entryNameLength] = 0;
            newEntry->nameLen = entryNameLength;
            newEntry->value = new char[entryValueLength + 1];
            newEntry->value[entryValueLength] = 0;
            newEntry->valueLen = entryValueLength;

            for (int ri = 0; ri < entryNameLength && ri < 256; ri++) {
                newEntry->name[ri] = (char) EEPROM.read(eepromPtr + 1 + ri);
            }
            for (int ri = 0; ri < entryValueLength && ri < 256; ri++) {
                newEntry->value[ri] = (char) EEPROM.read(eepromPtr + 2 + entryNameLength + ri);
            }
        }
        eepromPtr += 2 + entryNameLength + entryValueLength;
        if (eepromPtr >= eepromStartAddress + eepromSize) {
            return false;  // EEPROM space exceeded
        }
    }
    initialized = true;
    return true;
}
CacheEntry* getCacheEntryByName(const char* name) {
    if(!wifiMgrSetupEEPROM()) return nullptr;
    for (int i = 0; i < WIFI_MGR_MAX_CONFIG_ENTRIES; i++) {
        if (cache[i].name != nullptr && strcmp(name, cache[i].name) == 0) {
            return &cache[i];
        }
    }
    return nullptr;
}
bool wifiMgrCommitEEPROM() {
    wifiMgrSetupEEPROM();
    EEPROM.write(eepromStartAddress + 0, WIFI_MGR_EEPROM_HEADER_1);
    EEPROM.write(eepromStartAddress + 1, WIFI_MGR_EEPROM_HEADER_2);
    EEPROM.write(eepromStartAddress + 2, WIFI_MGR_EEPROM_VERSION_1);
    EEPROM.write(eepromStartAddress + 3, WIFI_MGR_EEPROM_VERSION_2);
    uint16_t eepromPtr = eepromStartAddress + 5;
    uint8_t count = 0;
    for (int i = 0; i < WIFI_MGR_MAX_CONFIG_ENTRIES; i++) {
        CacheEntry cacheEntry = cache[i];
        if (cacheEntry.value != nullptr && cacheEntry.name != nullptr) {
            EEPROM.write(eepromPtr++, cacheEntry.nameLen);
            for (unsigned int wi = 0; wi < cacheEntry.nameLen; wi++) {
                EEPROM.write(eepromPtr++, cacheEntry.name[wi]);
            }
            EEPROM.write(eepromPtr++, cacheEntry.valueLen);
            for (unsigned int wi = 0; wi < cacheEntry.valueLen; wi++) {
                EEPROM.write(eepromPtr++, cacheEntry.value[wi]);
            }
            count++;
        }
    }
    EEPROM.write(eepromStartAddress + 4, count);
    return EEPROM.commit();
}
void wifiMgrClearEEPROM() {
    if(!wifiMgrSetupEEPROM()) return;
    for (int i = 0; i < WIFI_MGR_MAX_CONFIG_ENTRIES; i++) {
        if (cache[i].name != nullptr) {
            delete[] cache[i].name;
            cache[i].name = nullptr;
        }
        cache[i].nameLen = 0;
        if (cache[i].value != nullptr) {
            delete[] cache[i].value;
            cache[i].value = nullptr;
        }
        cache[i].valueLen = 0;
    }
    initialized = false;
}
const char* wifiMgrGetConfig(const char* name) {
    if(!wifiMgrSetupEEPROM()) return nullptr;
    CacheEntry* cacheEntry = getCacheEntryByName(name);
    if (cacheEntry == nullptr) return nullptr;
    else return cacheEntry->value;
}
bool wifiMgrSetConfig(const char* name, const char* value) {
    if(!wifiMgrSetupEEPROM()) return false;
    CacheEntry* cacheEntry = getCacheEntryByName(name);

    if (cacheEntry == nullptr) {
        cacheEntry = nextEmptyCacheEntry();
        if (cacheEntry == nullptr) return false;

        size_t len = strlen(name);
        cacheEntry->name = new char[len + 1];
        if (cacheEntry->name == nullptr) return false;
        strncpy(cacheEntry->name, name, len);
        cacheEntry->name[len] = '\0';  // Ensure null termination
        cacheEntry->nameLen = len;
    }

    if (cacheEntry->value != nullptr) delete[] cacheEntry->value;
    size_t len = strlen(value);
    cacheEntry->value = new char[len + 1];
    if (cacheEntry->value == nullptr) return false;
    strncpy(cacheEntry->value, value, len);
    cacheEntry->value[len] = '\0';  // Ensure null termination
    cacheEntry->valueLen = len;
    return true;
}
bool wifiMgrSetConfig(const char* name, const char* value, uint8_t len) {
    if(!wifiMgrSetupEEPROM()) return false;
    CacheEntry* cacheEntry = getCacheEntryByName(name);

    if (cacheEntry == nullptr) {
        cacheEntry = nextEmptyCacheEntry();
        if (cacheEntry == nullptr) return false;

        size_t nameLen = strlen(name);
        cacheEntry->name = new char[nameLen + 1];
        if (cacheEntry->name == nullptr) return false;
        strncpy(cacheEntry->name, name, nameLen);
        cacheEntry->name[nameLen] = '\0';  // Ensure null termination
        cacheEntry->nameLen = nameLen;
    }

    if (cacheEntry->value != nullptr) delete[] cacheEntry->value;
    cacheEntry->value = new char[len + 1];
    if (cacheEntry->value == nullptr) return false;
    for (uint16_t wi = 0; wi < len; wi++) {
        cacheEntry->value[wi] = (char) value[wi];
    }
    cacheEntry->value[len] = '\0';  // Ensure null termination
    cacheEntry->valueLen = len;
    return true;
}
long wifiMgrGetLongConfig(const char* name, long def) {
    long load;
    CacheEntry* cacheEntry = getCacheEntryByName(name);
    if (cacheEntry == nullptr || cacheEntry->valueLen != 4) {
        return def;
    }
    load = 0;
    load |= cacheEntry->value[0] << 24;
    load |= cacheEntry->value[1] << 16;
    load |= cacheEntry->value[2] << 8;
    load |= cacheEntry->value[3];
    return load;
}
bool wifiMgrSetLongConfig(const char* name, long val) {
    char tmp[] = {0,0,0,0};
    tmp[0] = val >> 24 & 0xFF;
    tmp[1] = val >> 16 & 0xFF;
    tmp[2] = val >> 8 & 0xFF;
    tmp[3] = val & 0xFF;
    return wifiMgrSetConfig(name, tmp, 4);
}
unsigned long wifiMgrGetUlongConfig(const char* name, unsigned long def) {
    unsigned long load;
    CacheEntry* cacheEntry = getCacheEntryByName(name);
    if (cacheEntry == nullptr || cacheEntry->valueLen != 4) {
        return def;
    }
    load = 0;
    load |= cacheEntry->value[0] << 24;
    load |= cacheEntry->value[1] << 16;
    load |= cacheEntry->value[2] << 8;
    load |= cacheEntry->value[3];
    return load;
}
bool wifiMgrSetUlongConfig(const char* name, unsigned long val) {
    char tmp[] = {0,0,0,0};
    tmp[0] = val >> 24 & 0xFF;
    tmp[1] = val >> 16 & 0xFF;
    tmp[2] = val >> 8 & 0xFF;
    tmp[3] = val & 0xFF;
    return wifiMgrSetConfig(name, tmp, 4);
}
bool wifiMgrGetBoolConfig(const char* name, bool def) {
    CacheEntry* cacheEntry = getCacheEntryByName(name);
    if (cacheEntry == nullptr || cacheEntry->valueLen != 1) {
        return def;
    }
    return cacheEntry->value[0] != 0;
}
bool wifiMgrSetBoolConfig(const char* name, bool val) {
    char tmp[1] = {0};
    if (val) tmp[0] = 1;
    return wifiMgrSetConfig(name, tmp, 1);
}