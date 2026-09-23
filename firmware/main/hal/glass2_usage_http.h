#pragma once

// Starts/stops the LAN-only Grok usage ingest endpoint. The board network
// callback invokes these only while the Wi-Fi station is connected.
bool glass2_usage_http_start();
void glass2_usage_http_stop();
