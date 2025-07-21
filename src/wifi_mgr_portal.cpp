// PUBLISHED UNDER CC BY-NC 4.0 https://creativecommons.org/licenses/by-nc/4.0/

#include "wifi_mgr_portal.h"

bool wifiMgrPortalIsSetup = false;
bool wifiMgrPortalStarted = false;
bool wifiMgrPortalConnectFailed = false;
bool wifiMgrPortalCommitFailed = false;
bool wifiMgrPortalRedirectIndex = false;
bool wifiMgrPortalIsOwnServer = false;
XWebServer *wifiMgrPortalWebServer = nullptr;
const char *ssidPrefix = nullptr;
const char *password = nullptr;

PortalConfigEntry *firstEntry = nullptr;

PortalConfigEntry* getLastEntry() {
    PortalConfigEntry* tmp = firstEntry;
    if (tmp == nullptr) return nullptr;
    while (tmp->next != nullptr) tmp = tmp->next;
    return tmp;
}

void wifiMgrPortalSendConfigure() {
    PortalConfigEntry *tmp = firstEntry;
    int changes = 0;
    bool needRestart = false;
    bool isWifi = false;
    if (wifiMgrPortalWebServer->method() == HTTP_POST) {
        while (tmp != nullptr) {
            if (wifiMgrPortalWebServer->hasArg(tmp->eepromKey) && !wifiMgrPortalWebServer->arg(tmp->eepromKey).isEmpty()) {
                String val = wifiMgrPortalWebServer->arg(tmp->eepromKey);
                const char *currentVal = wifiMgrGetConfig(tmp->eepromKey);
                // config item is in post
                if ((currentVal == nullptr || strcmp(val.c_str(), currentVal)) && (!tmp->isPassword || !wifiMgrPortalWebServer->arg(tmp->eepromKey).isEmpty())) {
                    // value changed
                    if (strcmp(tmp->eepromKey, "SSID") == 0 || strcmp(tmp->eepromKey, "WIFI_PW") == 0 || strcmp(tmp->eepromKey, "HOST") == 0) {
                        isWifi = true;
                    }
                    if (tmp->restartOnChange) needRestart = true;
                    if (tmp->type == STRING) {
                        wifiMgrSetConfig(tmp->eepromKey, wifiMgrPortalWebServer->arg(tmp->eepromKey).c_str());
                    } else if (tmp->type == NUMBER) {
                        wifiMgrSetLongConfig(tmp->eepromKey, wifiMgrPortalWebServer->arg(tmp->eepromKey).toInt());
                    } else if (tmp->type == BOOL) {
                        wifiMgrSetBoolConfig(tmp->eepromKey, wifiMgrPortalWebServer->arg(tmp->eepromKey) == "1");
                    }
                    changes++;
                }
            }
            tmp = tmp->next;
        }
    }
    
    // Start building the HTML response with improved structure
    String ret = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
    ret += "  <meta charset=\"UTF-8\">\n";
    ret += "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
    ret += "  <title>WiFi Manager</title>\n";
    ret += "  <link rel=\"stylesheet\" href=\"/wifiMgr/style.css\">\n";
    ret += "</head>\n<body>\n";
    ret += "  <div class=\"container\">\n";
    ret += "    <h1>WiFi Manager</h1>\n";
    
    // Add status messages if needed
    if (changes > 0 || needRestart || wifiMgrPortalConnectFailed || wifiMgrPortalCommitFailed) {
        if (changes > 0) {
            ret += "    <div class=\"message success\">" + String(changes) + " changes made successfully.</div>\n";
        }
        if (needRestart) {
            ret += "    <div class=\"message info\">Device will restart now.</div>\n";
        }
        if (wifiMgrPortalConnectFailed) {
            ret += "    <div class=\"message error\">Failed to connect to WiFi. Please check your credentials.</div>\n";
        }
        if (wifiMgrPortalCommitFailed) {
            ret += "    <div class=\"message error\">Failed to save settings to EEPROM.</div>\n";
        }
    }
    
    // Start the form
    ret += "    <form action=\"#\" method=\"POST\" onsubmit=\"return validateForm(this)\">\n";
    
    // Add form fields
    tmp = firstEntry;
    while (tmp != nullptr) {
        ret += "      <div class=\"form-group\">\n";
        ret += "        <h2>" + String(tmp->name) + "</h2>\n";
        
        if (tmp->type == STRING) {
            ret += "        <input type=\"" + String(tmp->isPassword ? "password" : "text") + "\" ";
            ret += "name=\"" + String(tmp->eepromKey) + "\" ";
            if (!tmp->isPassword) {
                ret += "value=\"" + String(wifiMgrGetConfig(tmp->eepromKey)) + "\" ";
            }
            if (strcmp(tmp->eepromKey, "SSID") == 0) {
                ret += "required ";
            }
            ret += ">\n";
        } else if (tmp->type == NUMBER) {
            ret += "        <input type=\"number\" name=\"" + String(tmp->eepromKey) + "\" ";
            if (!tmp->isPassword) {
                ret += "value=\"" + String(wifiMgrGetConfig(tmp->eepromKey)) + "\" ";
            }
            ret += ">\n";
        } else if (tmp->type == BOOL) {
            ret += "        <select name=\"" + String(tmp->eepromKey) + "\">\n";
            ret += "          <option value=\"1\"";
            if (wifiMgrGetBoolConfig(tmp->eepromKey, false)) ret += " selected";
            ret += ">Yes / On</option>\n";
            ret += "          <option value=\"0\"";
            if (!wifiMgrGetBoolConfig(tmp->eepromKey, true)) ret += " selected";
            ret += ">No / Off</option>\n";
            ret += "        </select>\n";
        }
        
        ret += "      </div>\n";
        tmp = tmp->next;
    }
    
    // Add submit button
    ret += "      <input type=\"submit\" value=\"Save Settings\">\n";
    ret += "    </form>\n";
    ret += "  </div>\n";
    
    // Add footer
    ret += "  <footer>WiFi Manager Portal - ESP WiFi Configuration</footer>\n";
    
    // Add JavaScript
    ret += "  <script src=\"/wifiMgr/script.js\"></script>\n";
    ret += "</body>\n</html>";

    if (wifiMgrPortalWebServer->method() == HTTP_POST) {
        if (isWifi) {
            wifiMgrPortalWebServer->send(200, "text/html", ret);
            unsigned long start = millis();
            while (millis() - start < 500) yield();
            setupWifi(wifiMgrGetConfig("SSID"), wifiMgrGetConfig("WIFI_PW"));
            if (WiFi.isConnected()) {
                if (!wifiMgrCommitEEPROM()) {
                    wifiMgrPortalCommitFailed = false;
                }
                wifiMgrPortalConnectFailed = false;
            } else {
                wifiMgrPortalIsSetup = false;
                wifiMgrPortalStarted = false;
                wifiMgrPortalConnectFailed = true;
                wifiMgrPortalLoop();
                return;
            }
        } else {
            if (!wifiMgrCommitEEPROM()) {
                wifiMgrPortalCommitFailed = true;
                wifiMgrPortalWebServer->send(200, "text/html", ret);
            } else {
                wifiMgrPortalWebServer->send(200, "text/html", ret);
            }
        }
        if (needRestart) {
            unsigned long start = millis();
            while (millis() - start < 1000) yield();
            wifiMgrPortalCleanup(); // Clean up resources before restart
            ESP.restart();
        }
    } else {
        wifiMgrPortalWebServer->send(200, "text/html", ret);
    }
}

// CSS content handler
void handleCSS() {
    String css = R"(
/* WiFi Manager Portal Styles */
:root {
  --primary-color: #2196F3;
  --primary-dark: #1976D2;
  --secondary-color: #FF9800;
  --text-color: #333333;
  --light-bg: #f5f5f5;
  --border-color: #dddddd;
  --success-color: #4CAF50;
  --error-color: #F44336;
}

* {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

body {
  font-family: 'Arial', sans-serif;
  line-height: 1.6;
  color: var(--text-color);
  background-color: var(--light-bg);
  padding: 20px;
  max-width: 800px;
  margin: 0 auto;
}

.container {
  background-color: white;
  border-radius: 8px;
  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
  padding: 20px;
  margin-bottom: 20px;
}

h1 {
  color: var(--primary-color);
  margin-bottom: 20px;
  text-align: center;
}

h2 {
  color: var(--primary-dark);
  margin: 15px 0 10px 0;
  font-size: 1.2rem;
  border-bottom: 1px solid var(--border-color);
  padding-bottom: 5px;
}

.form-group {
  margin-bottom: 15px;
}

label {
  display: block;
  margin-bottom: 5px;
  font-weight: bold;
}

input[type="text"],
input[type="password"],
input[type="number"],
select {
  width: 100%;
  padding: 10px;
  border: 1px solid var(--border-color);
  border-radius: 4px;
  font-size: 16px;
}

input[type="text"]:focus,
input[type="password"]:focus,
input[type="number"]:focus,
select:focus {
  outline: none;
  border-color: var(--primary-color);
  box-shadow: 0 0 0 2px rgba(33, 150, 243, 0.2);
}

button, 
input[type="submit"] {
  background-color: var(--primary-color);
  color: white;
  border: none;
  padding: 10px 15px;
  border-radius: 4px;
  cursor: pointer;
  font-size: 16px;
  display: block;
  width: 100%;
  margin-top: 20px;
  transition: background-color 0.3s;
}

button:hover,
input[type="submit"]:hover {
  background-color: var(--primary-dark);
}

.message {
  padding: 10px;
  margin: 15px 0;
  border-radius: 4px;
}

.success {
  background-color: rgba(76, 175, 80, 0.1);
  border: 1px solid var(--success-color);
  color: var(--success-color);
}

.error {
  background-color: rgba(244, 67, 54, 0.1);
  border: 1px solid var(--error-color);
  color: var(--error-color);
}

.info {
  background-color: rgba(33, 150, 243, 0.1);
  border: 1px solid var(--primary-color);
  color: var(--primary-color);
}

footer {
  text-align: center;
  margin-top: 20px;
  font-size: 0.8rem;
  color: #666;
}

@media (max-width: 600px) {
  body {
    padding: 10px;
  }
  
  .container {
    padding: 15px;
  }
  
  h1 {
    font-size: 1.5rem;
  }
  
  h2 {
    font-size: 1.1rem;
  }
}
)";
    wifiMgrPortalWebServer->send(200, "text/css", css);
}

// JavaScript content handler
void handleJS() {
    String js = R"(
// WiFi Manager Portal JavaScript
document.addEventListener('DOMContentLoaded', function() {
  // Add form submission handling
  const form = document.querySelector('form');
  if (form) {
    form.addEventListener('submit', function(e) {
      // Show loading message
      const submitBtn = form.querySelector('input[type="submit"]');
      if (submitBtn) {
        const originalText = submitBtn.value || 'Submit';
        submitBtn.value = 'Saving...';
        submitBtn.disabled = true;
        
        // Re-enable after submission (for cases where page doesn't reload)
        setTimeout(() => {
          submitBtn.value = originalText;
          submitBtn.disabled = false;
        }, 5000);
      }
    });
  }

  // Add password visibility toggle
  const passwordInputs = document.querySelectorAll('input[type="password"]');
  passwordInputs.forEach(input => {
    // Create toggle button
    const toggleBtn = document.createElement('button');
    toggleBtn.type = 'button';
    toggleBtn.className = 'password-toggle';
    toggleBtn.textContent = 'Show';
    toggleBtn.style.position = 'absolute';
    toggleBtn.style.right = '10px';
    toggleBtn.style.top = '50%';
    toggleBtn.style.transform = 'translateY(-50%)';
    toggleBtn.style.background = 'none';
    toggleBtn.style.border = 'none';
    toggleBtn.style.color = '#2196F3';
    toggleBtn.style.cursor = 'pointer';
    toggleBtn.style.fontSize = '14px';
    
    // Create wrapper for positioning
    const wrapper = document.createElement('div');
    wrapper.style.position = 'relative';
    
    // Insert wrapper and move input inside
    input.parentNode.insertBefore(wrapper, input);
    wrapper.appendChild(input);
    wrapper.appendChild(toggleBtn);
    
    // Add toggle functionality
    toggleBtn.addEventListener('click', function() {
      if (input.type === 'password') {
        input.type = 'text';
        toggleBtn.textContent = 'Hide';
      } else {
        input.type = 'password';
        toggleBtn.textContent = 'Show';
      }
    });
  });

  // Add validation for form fields
  const inputs = document.querySelectorAll('input[type="text"], input[type="password"], input[type="number"]');
  inputs.forEach(input => {
    input.addEventListener('blur', function() {
      validateInput(input);
    });
  });

  // Add message auto-hide
  const messages = document.querySelectorAll('.message');
  if (messages.length > 0) {
    setTimeout(() => {
      messages.forEach(msg => {
        msg.style.opacity = '0';
        msg.style.transition = 'opacity 0.5s ease';
        setTimeout(() => {
          msg.style.display = 'none';
        }, 500);
      });
    }, 5000);
  }
});

// Input validation function
function validateInput(input) {
  // Clear previous validation
  const existingError = input.parentNode.querySelector('.validation-error');
  if (existingError) {
    existingError.remove();
  }
  
  // Skip validation for empty optional fields
  if (!input.value && !input.hasAttribute('required')) {
    return true;
  }
  
  let isValid = true;
  let errorMessage = '';
  
  // Validate based on input type or name
  if (input.name === 'SSID' && input.value.length < 1) {
    isValid = false;
    errorMessage = 'SSID is required';
  } else if (input.name === 'HOST' && input.value.length > 0) {
    // Hostname validation (letters, numbers, hyphens, no spaces)
    const hostnameRegex = /^[a-zA-Z0-9-]+$/;
    if (!hostnameRegex.test(input.value)) {
      isValid = false;
      errorMessage = 'Hostname can only contain letters, numbers, and hyphens';
    }
  } else if (input.type === 'number') {
    const min = input.getAttribute('min');
    const max = input.getAttribute('max');
    
    if (min !== null && parseInt(input.value) < parseInt(min)) {
      isValid = false;
      errorMessage = `Value must be at least ${min}`;
    } else if (max !== null && parseInt(input.value) > parseInt(max)) {
      isValid = false;
      errorMessage = `Value must be at most ${max}`;
    }
  }
  
  // Display error if validation failed
  if (!isValid) {
    const errorElement = document.createElement('div');
    errorElement.className = 'validation-error';
    errorElement.style.color = '#F44336';
    errorElement.style.fontSize = '12px';
    errorElement.style.marginTop = '5px';
    errorElement.textContent = errorMessage;
    
    input.parentNode.appendChild(errorElement);
    input.style.borderColor = '#F44336';
  } else {
    input.style.borderColor = '';
  }
  
  return isValid;
}

// Form validation before submission
function validateForm(form) {
  const inputs = form.querySelectorAll('input[type="text"], input[type="password"], input[type="number"]');
  let isValid = true;
  
  inputs.forEach(input => {
    if (!validateInput(input)) {
      isValid = false;
    }
  });
  
  return isValid;
}
)";
    wifiMgrPortalWebServer->send(200, "application/javascript", js);
}

void wifiMgrPortalSetup(bool redirectIndex, const char* ssidPrefix_, const char* password_) {
    // Free previous values if they exist to prevent memory leaks
    if (ssidPrefix != nullptr) {
        free((void*)ssidPrefix);
        ssidPrefix = nullptr;
    }
    if (password != nullptr) {
        free((void*)password);
        password = nullptr;
    }
    
    // Make copies of the strings to prevent dangling pointers
    ssidPrefix = (ssidPrefix_ != nullptr && strlen(ssidPrefix_) > 0) ? strdup(ssidPrefix_) : nullptr;
    password = (password_ != nullptr && strlen(password_) > 0) ? strdup(password_) : nullptr;
    wifiMgrPortalRedirectIndex = redirectIndex;
    const char* ssid = wifiMgrGetConfig("SSID");
    wifiMgrPortalAddConfigEntry("SSID", "SSID", STRING, false, true);
    const char* pw = wifiMgrGetConfig("WIFI_PW");
    wifiMgrPortalAddConfigEntry("WiFi Password", "WIFI_PW", STRING, true, true);
    wifiMgrPortalAddConfigEntry("Hostname", "HOST", STRING, false, true);
    if (ssid != nullptr && pw != nullptr) {
        // configured
        const char* host = wifiMgrGetConfig("HOST");
        if (host == nullptr || strlen(host) == 0) {
            String macAddress = WiFi.macAddress();
            macAddress.replace(":", "");
            macAddress = macAddress.substring(6, macAddress.length());

            setupWifi(ssid, pw, (String(ssidPrefix) + macAddress).c_str());
        }
        else setupWifi(ssid, pw, host);

#if defined(ESP8266)
        if (wifiMgrPortalWebServer != nullptr && wifiMgrPortalWebServer->getServer().status() == 0) wifiMgrPortalWebServer->begin();
#endif
        wifiMgrPortalIsSetup = true;
    }
    wifiMgrPortalWebServer = wifiMgrGetWebServer();
    if (wifiMgrPortalWebServer == nullptr) {
        wifiMgrPortalWebServer = new XWebServer(80);
        wifiMgrPortalIsOwnServer = true;
    }
    
    // Add routes for CSS and JS files
    wifiMgrPortalWebServer->on("/wifiMgr/style.css", HTTP_GET, handleCSS);
    wifiMgrPortalWebServer->on("/wifiMgr/script.js", HTTP_GET, handleJS);
    
    // Add routes for configuration
    wifiMgrPortalWebServer->on("/wifiMgr/configure", HTTP_POST, wifiMgrPortalSendConfigure);
    wifiMgrPortalWebServer->on("/wifiMgr/configure", HTTP_GET, wifiMgrPortalSendConfigure);
    if (wifiMgrPortalRedirectIndex) {
        // TODO: actually do a redirect
        wifiMgrPortalWebServer->on("/", HTTP_POST, wifiMgrPortalSendConfigure);
        wifiMgrPortalWebServer->on("/", HTTP_GET, wifiMgrPortalSendConfigure);
    }
}

void wifiMgrPortalAddConfigEntry(const char* name, const char* eepromKey, PortalConfigEntryType type, bool isPassword, bool restartOnChange) {
    auto *newEntry = new (std::nothrow) PortalConfigEntry();
    if (!newEntry) {
        return;
    }

    newEntry->name = name;
    newEntry->eepromKey = eepromKey;
    newEntry->type = type;
    newEntry->isPassword = isPassword;
    newEntry->restartOnChange = restartOnChange;

    PortalConfigEntry* last = getLastEntry();
    if (last == nullptr) firstEntry = newEntry;
    else last->next = newEntry;
}

bool wifiMgrPortalLoop() {
    if (wifiMgrPortalIsSetup) {
        loopWifi();
        if (wifiMgrPortalWebServer != nullptr) wifiMgrPortalWebServer->handleClient();
        return true;
    } else if (!wifiMgrPortalStarted) {
        String macAddress = WiFi.macAddress();
        macAddress.replace(":", "");
        macAddress = macAddress.substring(6, macAddress.length());
        WiFi.softAP((String(ssidPrefix) + macAddress).c_str(), password);

#if defined(ESP8266)
        if (wifiMgrPortalWebServer != nullptr && wifiMgrPortalWebServer->getServer().status() == 0) wifiMgrPortalWebServer->begin();
#endif

        wifiMgrPortalStarted = true;
    } else {
        if (wifiMgrPortalWebServer != nullptr) wifiMgrPortalWebServer->handleClient();
    }
    return false;
}

// Cleanup function to free memory used by PortalConfigEntry objects
void wifiMgrPortalCleanup() {
    PortalConfigEntry* current = firstEntry;
    PortalConfigEntry* next = nullptr;
    
    while (current != nullptr) {
        next = current->next;
        delete current;
        current = next;
    }
    
    firstEntry = nullptr;
    
    // If we created our own server, delete it
    if (wifiMgrPortalIsOwnServer && wifiMgrPortalWebServer != nullptr) {
        delete wifiMgrPortalWebServer;
        wifiMgrPortalWebServer = nullptr;
        wifiMgrPortalIsOwnServer = false;
    }
}
