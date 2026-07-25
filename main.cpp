/* 
    Bed Weight Scale Alarm Node - System v1.1.0
    - Utilizes ParamServer v1.1.0 non-blocking distributed status matrix.
    - Integrated ArduinoOTA service for wireless flashing.
    - Non-blocking HX711_ADC sampling cycle.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HX711_ADC.h>
#include "ParamServer.h"

// --- Hardware Pins ---
const int LOADCELL_DOUT_PIN = 4;
const int LOADCELL_SCK_PIN = 5;
const int LOCAL_ALARM_PIN = 2; 

// --- Configuration Constants for OTA ---
const char* otaHostname = "BedScaleAlarm";
const char* otaPassword = "admin";

// --- Instantiate Core Objects ---
HX711_ADC scale(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
ParamServer server(8888);

// --- Persistent Parameters (Linked to UDP Server / LittleFS) ---
float cal_factor = 20814.0;     
float min_weight = 3.00;        
int alarm_duration = 1000;      
int tare_offset = 0;            
int device_number = 0;          // Default 0 = Unconfigured Device
String device_name = "Bed Scale Alarm";
String version = "Bed Weight Scale Alarm Node v1.1.0";
int time_zone = 0;             // Default to UTC
float dst = 0;                 // Default no daylight saving time

// --- Runtime Server Control Parameters ---
float current_weight = 0.00;

// --- Private Machine Variables ---
bool patient_present = false;
unsigned long alarm_start_time = 0;
bool local_alarm_active = false;
unsigned long last_measurement_time = 0;

// ========== ArduinoOTA Initialization ==========
void setupOTA() {
    ArduinoOTA.setHostname(otaHostname);
    if (strlen(otaPassword) > 0) {
        ArduinoOTA.setPassword(otaPassword);
    }
    
    ArduinoOTA.setTimeout(30000);
    ArduinoOTA.setPort(3232);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        Serial.println("\n[*] OTA Update initiating: " + type);
    });
    
    ArduinoOTA.onEnd([]() {
        Serial.println("\n[+] OTA Update finalized. Restarting...");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("    Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[-] OTA Error Code [%u]\n", error);
    });

    ArduinoOTA.begin();
    Serial.println("[+] OTA Network Daemon Active.");
}

void setup() {
    Serial.begin(115200);
    pinMode(LOCAL_ALARM_PIN, OUTPUT);
    digitalWrite(LOCAL_ALARM_PIN, LOW);

    // 1. Register parameters for UDP configuration and LittleFS persistence
    server.registerParam("version", &version, true, true);
    server.registerParam("cal", &cal_factor);
    server.registerParam("min_weight", &min_weight);
    server.registerParam("alarm_dur", &alarm_duration);
    server.registerParam("tare_offset", &tare_offset);
    server.registerParam("device_number", &device_number);
    server.registerParam("device_name", &device_name);
    server.registerParam("current_weight", &current_weight, false, true); // Transient, Read-Only
    server.registerParam("time_zone", &time_zone);
    server.registerParam("dst", &dst);    

    // 2. Initialize ParamServer network engine and load saved supervisors
    if (!server.begin("Patient_Scale_AP")) {
        Serial.println("[-] Core ParamServer Init Failure!");
        while(1);
    }

    // Pass the saved or default device number to the network engine
    server.setDeviceNumber(device_number);

    // 3. Initialize Wireless OTA Engine
    setupOTA();

    // 4. Initialize HX711_ADC Load Cell Interface
    scale.begin();
    scale.setCalFactor(cal_factor);
    scale.setSamplesInUse(5); 

    Serial.println("[*] Stabilizing scale configuration...");
    unsigned long stabilizing_timeout = millis();
    while (!scale.update()) {
        if (millis() - stabilizing_timeout > 3000) {
            Serial.println("[-] Scale Hardware Timeout!");
            break;
        }
        // Yield to RTOS to prevent watchdog resets during tight loops
        vTaskDelay(1); 
    }

    // Process raw electrical hardware baseline zero reference
    if (tare_offset == 0) {
        Serial.println("[*] No tare_offset found. Assuming empty bed and capturing zero baseline...");
        scale.tare(); 
        scale.refreshDataSet(); 
        tare_offset = scale.getTareOffset(); 
        server.forceSaveParam("tare_offset");
        Serial.printf("[+] Saved baseline tare offset: %d\n", tare_offset);
    } else {
        Serial.printf("[+] Restored tare offset from Flash: %d\n", tare_offset);
        scale.setTareOffset(tare_offset); 
        scale.refreshDataSet(); 
    }

    if (device_number == 0) {
        Serial.println("[!] WARNING: device_number is currently 0 (Unconfigured). Assign ID over UDP.");
    }
}

void loop() {
    // 1. Service background network stacks
    server.update();
    ArduinoOTA.handle();

    // 2. Run non-blocking HX711 sample engine at maximum CPU velocity
    if (scale.dataWaitingAsync()) {
        scale.updateAsync();
    }    

    unsigned long current_time = millis();

    // 3. Automatic Wi-Fi connection health check (Every 10 seconds)
    static unsigned long last_wifi_check_time = 0;
    if (current_time - last_wifi_check_time > 10000) {
        last_wifi_check_time = current_time;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[!] Wi-Fi disconnected. Attempting background reconnect...");
            // OPTIMIZATION: Use reconnect() to leverage the ESP32's internally cached 
            // credentials rather than blindly calling disconnect/begin.
            WiFi.reconnect(); 
        }
    }

    // 4. Process scale readings and occupancy state machine (Every 250ms)
    if (current_time - last_measurement_time > 250) {
        last_measurement_time = current_time;

        // Keep ParamServer engine and scale hardware synced with on-the-fly UDP parameter writes
        static float last_known_cal = 0.0;
        static int last_known_offset = -1;
        static int last_known_dev_num = -1;

        if (cal_factor != last_known_cal) { scale.setCalFactor(cal_factor); last_known_cal = cal_factor; }
        if (tare_offset != last_known_offset) { scale.setTareOffset(tare_offset); last_known_offset = tare_offset; }
        if (device_number != last_known_dev_num) { 
            server.setDeviceNumber(device_number); 
            last_known_dev_num = device_number; 
            Serial.printf("[+] Device ID updated to: %02d\n", device_number);
        }

        current_weight = scale.getData(); 
        if (current_weight < 0.0) current_weight = 0.0; 

        // --- PHYSICAL OCCUPANCY STATE ENGINE ---

        // Event: Patient enters bed
        if (!patient_present && current_weight >= min_weight) {
            patient_present = true;
            
            // Set network flags to occupied (1) and dispatch plain text soft alert
            server.setDistributedStatus(1);
            server.dispatchSoftAlert("Patient in bed");
            
            Serial.println("[!] Patient presence detected in bed.");
        } 
        
        // Event: Patient exits bed (Critical Alarm Trigger)
        if (patient_present && current_weight < min_weight) {
            patient_present = false;
            
            Serial.println("[CRITICAL] PATIENT_OUT_OF_BED_ALERT triggered!");
            
            // 1. Dispatch "msg dd01" to all supervisors and lock flags to Critical (2)
            server.dispatchCritical(1);

            // 2. Trigger local physical notification buzzer
            local_alarm_active = true;
            alarm_start_time = current_time;
            digitalWrite(LOCAL_ALARM_PIN, HIGH);
        }
    }

    // 5. Manage physical alarm buzzer timing envelope
    if (local_alarm_active && (current_time - alarm_start_time >= (unsigned long)alarm_duration)) {
        local_alarm_active = false;
        digitalWrite(LOCAL_ALARM_PIN, LOW);
    }

    // OPTIMIZATION: yield() is an ESP8266 holdover. On ESP32, vTaskDelay(1) 
    // cleanly hands 1 millisecond of CPU time back to the FreeRTOS idle task, 
    // keeping the Wi-Fi stack extremely stable and preventing watchdog resets.
    vTaskDelay(1);
}