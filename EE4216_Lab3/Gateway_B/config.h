#pragma once
// LOCAL CONFIGURATION — placeholder values only; DO NOT commit real keys.
const char* const GATEWAY_AP_SSID = "Matric_Number";
const char* const GATEWAY_AP_PASSWORD = "12345678";
// Gateway STA joins this separate Internet-enabled 2.4 GHz Wi-Fi network.
const char* const UPSTREAM_WIFI_SSID = "Wifi_SSID";
const char* const UPSTREAM_WIFI_PASSWORD = "Wifi_pasword";

// Obtain these from ThingSpeak's Channels > API Keys tabs.
const char* const SENSOR_WRITE_API_KEY = "";
const char* const LED_CONTROL_READ_API_KEY = "";
const char* const LED_CONTROL_CHANNEL_ID = "";

const unsigned long CLOUD_UPLOAD_INTERVAL_MS = 20000UL;
const unsigned long CLOUD_POLL_INTERVAL_MS = 5000UL;
