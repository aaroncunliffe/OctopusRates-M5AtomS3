#include "time.h"
#include "secrets.h"
#include <M5GFX.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

M5GFX display;

// --------------------
// consts
// --------------------

// Wifi
// Set in secrets.h
const char WIFI_SSID[] = SECRET_WIFI_SSID;
const char WIFI_PASSWORD[] = SECRET_WIFI_PASSWORD;

// Time
const char* NTP_SERVER = "time.google.com";
const long GMT_OFFSET_SEC = 0;
const int DAYLIGHT_OFFSET_SEC = 0;

// Octopus
// Change character here for different DNO regions E-1R-AGILE-24-10-01-{{DNO_REGION}} - E is Midlands
const char* OCTOPUS_RATE_API_BASE_TEMPLATE = "https://api.octopus.energy/v1/products/AGILE-24-10-01/electricity-tariffs/E-1R-AGILE-24-10-01-E/standard-unit-rates/";
const unsigned long UPDATE_INTERVAL = 1000 * 60 * 10;  // 10 minutes

// < priceLow = green
// > priceLow && < priceHigh = orange
// > priceHigh = red
const float PRICE_LOW = 10.0;
const float PRICE_HIGH = 24.0;

// --------------------
// Globals
// --------------------
struct PriceSlot {
  time_t validFrom;
  time_t validTo;
  float price;
};

PriceSlot prices[48];  // 24 hours of 30-min slots
int priceCount = 0;
unsigned long lastUpdate = 0;
float currentPrice = 0.00;

void setup() {
  Serial.begin(115200);
  Serial.println("Octopus Agile Display Starting...");

  // Display setup
  display.begin();
  display.setRotation(3);
  display.setTextWrap(true, true);
  displayMessage("Setting up...");

  // Connect WIFI
  Serial.println("Wifi Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected");

  // Sync time
  Serial.println("Syncing time");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             NTP_SERVER);
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("Time Synced:");
  Serial.println(&timeinfo, " %A, %B %d %Y %H:%M:%S");

  // Update Prices
  fetchPrices();

  lastUpdate = millis();
}

void loop() {
  // Periodic update
  if (millis() - lastUpdate > UPDATE_INTERVAL) {
    fetchPrices();
    lastUpdate = millis();
  }

  // Find and display current price - Only update display on change
  int slot = findCurrentSlot();
  if (slot >= 0) {
    if (currentPrice != prices[slot].price) {
      Serial.printf("Price change from %f to %f, updating display...", currentPrice, prices[slot].price);
      displayPrice(prices[slot].price, "CURRENT");
      currentPrice = prices[slot].price;
    }
  }

  delay(1000 * 10); // Loop every 10 seconds
}

// --------------------
// Functions
// --------------------

bool fetchPrices() {
  // Get prices from now onwards
  time_t now;
  time(&now);

  time_t slotStart = now - (now % 1800); // Round down to previous half an hour
  time_t slotEnd = slotStart + (24 * 60 * 60);  // 24 hours later

  struct tm* fromTime = gmtime(&slotStart);
  char periodFrom[32];
  strftime(periodFrom, sizeof(periodFrom), "%Y-%m-%dT%H:%M:%SZ", fromTime);

  struct tm* toTime = gmtime(&slotEnd);
  char periodTo[32];
  strftime(periodTo, sizeof(periodTo), "%Y-%m-%dT%H:%M:%SZ", toTime);


  char fullUrl[512];
  snprintf(fullUrl, sizeof(fullUrl), "%s?period_from=%s&period_to=%s&page_size=48", OCTOPUS_RATE_API_BASE_TEMPLATE, periodFrom, periodTo);

  Serial.print("Full request URL: ");
  Serial.println(fullUrl);

  HTTPClient http;
  http.begin(fullUrl);
  http.setTimeout(10000);

  int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    Serial.printf("HTTP error: %d\n", httpCode);
    return false;
  }

  Serial.print("Http code: ");
  Serial.println(httpCode);

  String payload = http.getString();
  http.end();

  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.printf("JSON error: %s\n", error.c_str());
    return false;
  }

  serializeJson(doc, Serial);

  JsonArray results = doc["results"];
  priceCount = 0;

  for (JsonObject slot : results) {
    if (priceCount >= 48) break;

    const char* validFrom = slot["valid_from"];
    const char* validTo = slot["valid_to"];
    float price = slot["value_inc_vat"];

    prices[priceCount].validFrom = parseISO8601(validFrom);
    prices[priceCount].validTo = parseISO8601(validTo);
    prices[priceCount].price = price;
    priceCount++;
  }

  Serial.println("Parsed all prices");

  // Sort by time (API returns newest last)
  // Bubble sort - very inefficient
  for (int i = 0; i < priceCount - 1; i++) {
    for (int j = i + 1; j < priceCount; j++) {
      if (prices[i].validFrom > prices[j].validFrom) {
        PriceSlot temp = prices[i];
        prices[i] = prices[j];
        prices[j] = temp;
      }
    }
  }

  Serial.printf("Fetched %d price slots\n", priceCount);
  return priceCount > 0;
}

int findCurrentSlot() {
  time_t now;
  time(&now);

  for (int i = 0; i < priceCount; i++) {
    if (now >= prices[i].validFrom && now < prices[i].validTo) {
      return i;
    }
  }
  return -1;
}


time_t parseISO8601(const char* str) {
  struct tm tm = { 0 };
  int year, month, day, hour, minute, second;

  if (sscanf(str, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) == 6) {
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return mktime(&tm) - _timezone;
  }
  return 0;
}


uint16_t getPriceColour(float price) {
  if (price < 0) return TFT_BLUE;  // Plunge pricing
  if (price < PRICE_LOW) return TFT_GREEN;
  if (price < PRICE_HIGH) return TFT_YELLOW;
  return TFT_RED;
}

void displayMessage(const char* msg) {
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE);
  display.setTextSize(1.5);
  display.setTextDatum(MC_DATUM);
  display.drawString(msg, display.width() / 2, display.height() / 2);
  Serial.println(msg);
}

void displayPrice(float price, const char* label) {
  display.clear(TFT_BLACK);

  uint16_t colour = getPriceColour(price);
  uint16_t priceTextColour = colour;

  // Show extremes with full background colour change instead of just text colour
  if (colour != TFT_YELLOW) {
    display.clear(colour);
    priceTextColour = TFT_WHITE;
  }

  // Label at top
  display.setTextColor(TFT_WHITE);
  display.setTextSize(2);
  display.setTextDatum(TC_DATUM);
  display.drawString(label, display.width() / 2, 5);

  // Price in centre (large)
  display.setTextColor(priceTextColour);
  display.setTextSize(3);
  display.setTextDatum(MC_DATUM);

  char priceStr[16];
  snprintf(priceStr, sizeof(priceStr), "%.2fp", price);
  display.drawString(priceStr, display.width() / 2, display.height() / 2);
}