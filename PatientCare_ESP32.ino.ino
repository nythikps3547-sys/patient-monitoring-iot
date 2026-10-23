 
/*
  PatientCare IoT - Main ESP32 DevKit

  Features:
  - MAX30102 heart-rate and SpO2 estimates
  - Firebase Realtime Database uploads
  - Physical SOS button on GPIO27
  - Medicine reminders from /medicineReminders
  - Physical buzzer driver control on GPIO25
  - Heart-rate and SpO2 reference-limit alert flags
  - India Standard Time (UTC+05:30)
  - Wi-Fi reconnection and Firebase error reporting

  Required libraries:
  1. Firebase ESP Client by Mobizt
  2. SparkFun MAX3010x Sensor Library

  IMPORTANT:
  GPIO25 is a driver-control signal, not a buzzer power output.
  This is not a medical diagnostic device.
*/

#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

#include "MAX30105.h"
#include "spo2_algorithm.h"
#include "secrets.h"

// -----------------------------------------------------
// PIN CONFIGURATION
// -----------------------------------------------------

#define I2C_SDA_PIN       21
#define I2C_SCL_PIN       22
#define BUZZER_DRIVER_PIN 25
#define SOS_BUTTON_PIN    27

const int BUZZER_ON_LEVEL  = HIGH;
const int BUZZER_OFF_LEVEL = LOW;

// -----------------------------------------------------
// SENSOR AND ALERT SETTINGS
// -----------------------------------------------------

#define BUFFER_LENGTH 100

const uint32_t FINGER_IR_THRESHOLD = 50000UL;

const int HR_LOW_LIMIT = 50;
const int HR_HIGH_LIMIT = 120;
const int SPO2_LOW_LIMIT = 92;

// These are project reference thresholds, not
// clinical alarm settings or medical diagnoses.

// -----------------------------------------------------
// TIMING
// -----------------------------------------------------

const unsigned long UPLOAD_INTERVAL_MS = 5000UL;
const unsigned long REMINDER_CHECK_INTERVAL_MS = 10000UL;
const unsigned long BUTTON_DEBOUNCE_MS = 50UL;
const unsigned long BUZZER_DURATION_MS = 5000UL;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000UL;

// -----------------------------------------------------
// FIREBASE
// -----------------------------------------------------

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

bool firebaseStarted = false;
bool sensorFound = false;

unsigned long lastUploadAttempt = 0;
unsigned long lastReminderCheck = 0;
unsigned long lastWiFiRetry = 0;

// -----------------------------------------------------
// MAX30102
// -----------------------------------------------------

MAX30105 particleSensor;

uint32_t irBuffer[BUFFER_LENGTH];
uint32_t redBuffer[BUFFER_LENGTH];

int32_t calculatedSpO2 = 0;
int32_t calculatedHR = 0;

int8_t validSpO2 = 0;
int8_t validHR = 0;

// -----------------------------------------------------
// BUZZER AND SOS STATE
// -----------------------------------------------------

bool buzzerIsOn = false;
unsigned long buzzerStartedAt = 0;

bool previousButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastButtonChange = 0;

bool sosIsActive = false;

String lastFiredMinute = "";

// =====================================================
// BUZZER
// =====================================================

void startBuzzer() {
  digitalWrite(BUZZER_DRIVER_PIN, BUZZER_ON_LEVEL);
  buzzerStartedAt = millis();
  buzzerIsOn = true;
}

void updateBuzzer() {
  if (buzzerIsOn &&
      millis() - buzzerStartedAt >= BUZZER_DURATION_MS) {
    digitalWrite(BUZZER_DRIVER_PIN, BUZZER_OFF_LEVEL);
    buzzerIsOn = false;
  }
}

// =====================================================
// WIFI AND TIME
// =====================================================

void configureIndiaTime() {
  configTime(19800, 0, "pool.ntp.org", "time.google.com");
  Serial.println("India time synchronisation requested.");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi...");

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < 20000UL) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected.");
    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());

    configureIndiaTime();
  } else {
    Serial.println("\nWi-Fi connection failed.");
    Serial.println("The ESP32 will retry.");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWiFiRetry >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiRetry = millis();

    Serial.println("Wi-Fi disconnected; reconnecting...");
    WiFi.reconnect();
  }
}

// =====================================================
// FIREBASE
// =====================================================

void startFirebase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase startup skipped: no Wi-Fi.");
    return;
  }

  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;

  auth.user.email = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASSWORD;

  config.token_status_callback = tokenStatusCallback;

  Firebase.reconnectWiFi(true);
  Firebase.begin(&config, &auth);

  firebaseStarted = true;
  Serial.println("Firebase client started.");
}

bool firebaseReady() {
  return firebaseStarted &&
         WiFi.status() == WL_CONNECTED &&
         Firebase.ready();
}

void reportFirebaseError(const char *operation) {
  Serial.print(operation);
  Serial.print(" failed: ");
  Serial.println(fbdo.errorReason());
}

bool writeBool(const char *path, bool value) {
  if (!Firebase.RTDB.setBool(&fbdo, path, value)) {
    reportFirebaseError(path);
    return false;
  }

  return true;
}

// =====================================================
// SENSOR INITIALISATION
// =====================================================

bool initialiseSensor() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(
      "MAX30102 not found. Check SDA, SCL, power and GND."
    );
    return false;
  }

  particleSensor.setup(60, 4, 2, 100, 411, 4096);

  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);
  particleSensor.setPulseAmplitudeGreen(0);

  Serial.println("MAX30102 detected.");
  return true;
}

bool readOneSample(uint32_t &irValue, uint32_t &redValue) {
  unsigned long waitStart = millis();

  while (!particleSensor.available()) {
    particleSensor.check();

    if (millis() - waitStart >= 2000UL) {
      return false;
    }

    delay(1);
  }

  irValue = particleSensor.getIR();
  redValue = particleSensor.getRed();

  particleSensor.nextSample();
  return true;
}

// =====================================================
// READ AND CALCULATE SENSOR VALUES
// =====================================================

bool readSensorValues() {
  validHR = 0;
  validSpO2 = 0;

  if (!sensorFound) {
    return false;
  }

  for (int i = 0; i < BUFFER_LENGTH; i++) {
    if (!readOneSample(irBuffer[i], redBuffer[i])) {
      Serial.println("Sensor sample timeout.");
      return false;
    }
  }

  if (irBuffer[BUFFER_LENGTH - 1] < FINGER_IR_THRESHOLD) {
    Serial.println("No finger detected.");
    return false;
  }

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    BUFFER_LENGTH,
    redBuffer,
    &calculatedSpO2,
    &validSpO2,
    &calculatedHR,
    &validHR
  );

  if (validHR != 1 ||
      calculatedHR < 30 ||
      calculatedHR > 220) {
    validHR = 0;
  }

  if (validSpO2 != 1 ||
      calculatedSpO2 < 70 ||
      calculatedSpO2 > 100) {
    validSpO2 = 0;
  }

  if (validHR) {
    Serial.print("Heart rate estimate: ");
    Serial.print(calculatedHR);
    Serial.println(" BPM");
  } else {
    Serial.println("Heart-rate reading invalid.");
  }

  if (validSpO2) {
    Serial.print("SpO2 estimate: ");
    Serial.print(calculatedSpO2);
    Serial.println("%");
  } else {
    Serial.println("SpO2 reading invalid.");
  }

  return validHR || validSpO2;
}

// =====================================================
// UPLOAD SENSOR DATA AND ALERT FLAGS
// =====================================================

void uploadSensorData() {
  if (!firebaseReady()) {
    Serial.println("Firebase unavailable; upload skipped.");
    return;
  }

  if (validHR) {
    if (Firebase.RTDB.setInt(
          &fbdo, "/patient/heartRate", calculatedHR)) {

      bool outside =
        calculatedHR < HR_LOW_LIMIT ||
        calculatedHR > HR_HIGH_LIMIT;

      writeBool(
        "/patient/alerts/heartRateOutsideReference",
        outside
      );
    } else {
      reportFirebaseError("Upload heart rate");
    }
  }

  if (validSpO2) {
    if (Firebase.RTDB.setInt(
          &fbdo, "/patient/spo2", calculatedSpO2)) {

      bool below = calculatedSpO2 < SPO2_LOW_LIMIT;

      writeBool(
        "/patient/alerts/spo2BelowReference",
        below
      );
    } else {
      reportFirebaseError("Upload SpO2");
    }
  }

  bool bothValid = validHR && validSpO2;

  writeBool("/patient/sensorValid", bothValid);

  Serial.println("Firebase sensor upload cycle completed.");
}

// =====================================================
// PHYSICAL SOS BUTTON
// =====================================================

void handleSOSButton() {
  bool currentReading = digitalRead(SOS_BUTTON_PIN);

  if (currentReading != previousButtonReading) {
    lastButtonChange = millis();
    previousButtonReading = currentReading;
  }

  if (millis() - lastButtonChange < BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (currentReading == stableButtonState) {
    return;
  }

  stableButtonState = currentReading;

  if (stableButtonState != LOW) {
    return;
  }

  sosIsActive = !sosIsActive;

  Serial.print("Physical SOS: ");
  Serial.println(sosIsActive ? "ACTIVE" : "INACTIVE");

  startBuzzer();

  if (!firebaseReady()) {
    Serial.println(
      "SOS status not uploaded: Firebase unavailable."
    );
    return;
  }

  bool statusSaved = writeBool(
    "/patient/sos/active",
    sosIsActive
  );

  time_t now = time(nullptr);
  int64_t timestampMs = 0;

  if (now > 1700000000) {
    timestampMs = (int64_t)now * 1000LL;
  }

  if (!Firebase.RTDB.setDouble(
        &fbdo,
        "/patient/sos/timestamp",
        (double)timestampMs)) {
    reportFirebaseError("Update SOS timestamp");
  }

  if (statusSaved) {
    Serial.println("SOS status saved to Firebase.");
  }
}

// =====================================================
// MEDICINE REMINDERS
//
// Firebase structure:
// medicineReminders
//   reminderId
//     name: "Medicine"
//     time: "HH:MM"
//
// Times must use 24-hour HH:MM format.
// =====================================================

void checkMedicineReminders() {
  if (!firebaseReady()) {
    return;
  }

  struct tm localTimeInfo;

  if (!getLocalTime(&localTimeInfo, 100)) {
    Serial.println("Network time not ready; reminders skipped.");
    return;
  }

  char dateText[11];
  char timeText[6];

  strftime(
    dateText, sizeof(dateText),
    "%Y-%m-%d", &localTimeInfo
  );

  strftime(
    timeText, sizeof(timeText),
    "%H:%M", &localTimeInfo
  );

  String currentTime = String(timeText);
  String currentMinute = String(dateText) + " " + currentTime;

  if (!Firebase.RTDB.getJSON(&fbdo, "/medicineReminders")) {
    reportFirebaseError("Read medicine reminders");
    return;
  }

  FirebaseJson &reminders = fbdo.jsonObject();
  size_t count = reminders.iteratorBegin();

  bool reminderDue = false;

  for (size_t i = 0; i < count; i++) {
    FirebaseJson::IteratorValue item = reminders.valueAt(i);

    // Correct FirebaseJson type check.
    // Do not use item.is<JsonObject>() here.
   if (String(item.type) != "object") {
      continue;
    }

    FirebaseJson reminder;
    reminder.setJsonData(item.value);

    FirebaseJsonData timeData;

    if (reminder.get(timeData, "time") && timeData.success) {
      String scheduledTime = timeData.stringValue;
      scheduledTime.trim();

      bool validFormat =
        scheduledTime.length() == 5 &&
        scheduledTime.charAt(2) == ':' &&
        isDigit(scheduledTime.charAt(0)) &&
        isDigit(scheduledTime.charAt(1)) &&
        isDigit(scheduledTime.charAt(3)) &&
        isDigit(scheduledTime.charAt(4));

      if (validFormat) {
        int hour =
          (scheduledTime.charAt(0) - '0') * 10 +
          (scheduledTime.charAt(1) - '0');

        int minute =
          (scheduledTime.charAt(3) - '0') * 10 +
          (scheduledTime.charAt(4) - '0');

        if (hour <= 23 &&
            minute <= 59 &&
            scheduledTime == currentTime) {
          reminderDue = true;
          break;
        }
      }
    }
  }

  reminders.iteratorEnd();

  if (reminderDue && currentMinute != lastFiredMinute) {
    lastFiredMinute = currentMinute;

    Serial.println("Medicine reminder due: buzzer ON.");
    startBuzzer();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUZZER_DRIVER_PIN, OUTPUT);
  digitalWrite(BUZZER_DRIVER_PIN, BUZZER_OFF_LEVEL);

  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);

  previousButtonReading = digitalRead(SOS_BUTTON_PIN);
  stableButtonState = previousButtonReading;
  lastButtonChange = millis();

  Serial.println();
  Serial.println("=== PatientCare IoT starting ===");

  sensorFound = initialiseSensor();

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    startFirebase();
  }

  Serial.println("Setup complete.");
  Serial.println("Serial Monitor baud rate: 115200.");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  maintainWiFi();
  handleSOSButton();
  updateBuzzer();

  if (!firebaseStarted && WiFi.status() == WL_CONNECTED) {
    startFirebase();
  }

  if (millis() - lastReminderCheck >=
      REMINDER_CHECK_INTERVAL_MS) {
    lastReminderCheck = millis();
    checkMedicineReminders();
  }

  if (millis() - lastUploadAttempt >=
      UPLOAD_INTERVAL_MS) {
    lastUploadAttempt = millis();

    if (sensorFound) {
      readSensorValues();
    } else {
      validHR = 0;
      validSpO2 = 0;
    }

    uploadSensorData();
  }

  delay(5);
}
