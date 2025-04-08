#include <Arduino.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* ssid = "Rachit's iPhone";
const char* password = "hariseldon";
const char* post_url = "http://localhost:4242/post";
const char* get_entry_url_2  = "http://localhost:4242/get_entry_2";
const char* get_entry_url_4  = "http://localhost:4242/get_entry_4";
const char* get_exit_pin_url = "http://localhost:4242/get_exit_pin";
const char* get_exit_approved_url = "http://localhost:4242/get_exit_approved";

#define NUM_SLOTS 4
#define speedOfSound 0.0343
#define parking_threshold 4.00
#define parking_time_entry 10000
#define parking_time_exit 10000
#define sensor_timeout 20000

#define ENTRY_IR_SENSOR 34
#define EXIT_IR_SENSOR 35
#define DETECTION_THRESHOLD 1000

#define ENTRY_SERVO_PIN 12
#define EXIT_SERVO_PIN 13

#define ENTRY_LED_PIN 15
#define EXIT_LED_PIN 2

#define GATE_WAIT_TIME 5000
#define GATE_CLOSE_WAIT 3000

class ParkingSlot {
public:
    int trigPin, echoPin, ledPin;
    unsigned long carArrivedTime = 0;
    unsigned long carLeftTime = 0;
    bool isCarParked = false;
    bool isCarPresent = false;
    bool isSensorFaulty = false;
    float lastValidDistance = -1;

    ParkingSlot(int t, int e, int l) : trigPin(t), echoPin(e), ledPin(l) {}

    void setupPins() {
        pinMode(trigPin, OUTPUT);
        pinMode(echoPin, INPUT);
        pinMode(ledPin, OUTPUT);
        digitalWrite(ledPin, LOW);
    }
};

ParkingSlot slots[NUM_SLOTS] = {
    ParkingSlot(14, 27, 4),
    ParkingSlot(26, 25, 16),
    ParkingSlot(33, 32, 17),
    ParkingSlot(22, 23, 18)
};

Servo entryGateServo;
Servo exitGateServo;
bool entryGateOpen = false;
bool exitGateOpen = false;

unsigned long entryStartTime = 0;
unsigned long entryNoDetectionStartTime = 0;

unsigned long exitStartTime = 0;
unsigned long exitNoDetectionStartTime = 0;
bool waitingForExitSlot = false;

// Get distance from HC-SR04 sensor; returns -1 on sensor error.
float getDistance(int trigPin, int echoPin);

// Check each parking slot: update sensor status, handle timing for parked/car left.
void checkParkingSlots(unsigned long currentTime);

// Entry gate: check IR sensor detection, assign slot, get passkey, and control servo.
void checkEntryGate();

// Exit gate: check IR sensor, get exit pin and approval, and control exit servo.
void checkExitGate();

// Print sensor readings in a single line with entry, exit, and slot statuses.
void printSensorReadings();

void setup() {
    Serial.begin(9600);

    for (int i = 0; i < NUM_SLOTS; i++) {
        slots[i].setupPins();
    }

    pinMode(ENTRY_IR_SENSOR, INPUT);
    pinMode(EXIT_IR_SENSOR, INPUT);
    pinMode(ENTRY_LED_PIN, OUTPUT);
    digitalWrite(ENTRY_LED_PIN, LOW);
    pinMode(EXIT_LED_PIN, OUTPUT);
    digitalWrite(EXIT_LED_PIN, LOW);

    entryGateServo.attach(ENTRY_SERVO_PIN);
    exitGateServo.attach(EXIT_SERVO_PIN);
    entryGateServo.write(0);
    exitGateServo.write(0);

    randomSeed(analogRead(0));

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting...");
    }
    Serial.println("\nConnected to WiFi!");

    HTTPClient http;
    http.begin(post_url);
    http.addHeader("Content-Type", "application/json");
    int response = http.POST("{}");
    if (response > 0) Serial.println("Http post connection works at the start!\n");
    else Serial.println("Initial HTTP POST failed.");
    http.end();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        unsigned long currentTime = millis();
        checkEntryGate();
        checkExitGate();
        checkParkingSlots(currentTime);
        printSensorReadings();
    } else {
        Serial.println("WiFi Disconnected\nRetrying...");
        delay(4000);
        WiFi.begin(ssid, password);
        delay(5500);
    }
    delay(500);
}

float getDistance(int trigPin, int echoPin) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH, sensor_timeout);
    if (duration == 0) return -1;
    return (duration * speedOfSound) / 2;
}

void checkParkingSlots(unsigned long currentTime) {
    HTTPClient http_post;
    http_post.begin(post_url);
    http_post.addHeader("Content-Type", "application/json");

    for (int i = 0; i < NUM_SLOTS; i++) {
        float distanceCm = getDistance(slots[i].trigPin, slots[i].echoPin);

        if (distanceCm == -1) slots[i].isSensorFaulty = true;
        else {
            slots[i].isSensorFaulty = false;
            slots[i].lastValidDistance = distanceCm;
        }

        if (!slots[i].isSensorFaulty) {
            if (distanceCm < parking_threshold) {
                digitalWrite(slots[i].ledPin, HIGH);
                if (!slots[i].isCarPresent) {
                    slots[i].isCarPresent = true;
                    slots[i].carArrivedTime = currentTime;
                }
            } else {
                digitalWrite(slots[i].ledPin, LOW);
                if (slots[i].isCarPresent) {
                    slots[i].isCarPresent = false;
                    slots[i].carLeftTime = currentTime;
                }
            }

            if (slots[i].isCarPresent && !slots[i].isCarParked && (currentTime - slots[i].carArrivedTime >= parking_time_entry)) {
                String payload = "{\"type\": \"car-arrived\", \"slot\": " + String(i + 1) + "}";
                int response = http_post.POST(payload);
                Serial.print("CAR - PARKED SENT TO SERVER : "); Serial.println(response);
                slots[i].isCarParked = true;
            }

            if (!slots[i].isCarPresent && slots[i].isCarParked && (currentTime - slots[i].carLeftTime >= parking_time_exit)) {
                String payload = "{\"type\": \"car-left\", \"slot\": " + String(i + 1) + "}";
                int response = http_post.POST(payload);
                Serial.print("CAR - LEFT SENT TO SERVER : "); Serial.println(response);
                slots[i].isCarParked = false;
            }
        }
    }
    http_post.end();
}

void printSensorReadings() {
    String output = "";
    output += String(analogRead(ENTRY_IR_SENSOR)) + "\t";
    output += String(analogRead(EXIT_IR_SENSOR)) + "\t";

    for (int i = 0; i < NUM_SLOTS; i++) {
        float d = slots[i].lastValidDistance;
        if (!slots[i].isSensorFaulty) {
            if (d < parking_threshold) output += "YES\t";
            else output += String(d, 1) + "cm\t";
        } else {
            output += "ERR\t";
        }
    }
    Serial.println(output);
}

void checkEntryGate() {
    int entrySensorValue = analogRead(ENTRY_IR_SENSOR);

    if (entrySensorValue < DETECTION_THRESHOLD) {
        if (entryStartTime == 0) entryStartTime = millis();
        if ((millis() - entryStartTime >= GATE_WAIT_TIME) && !entryGateOpen) {

            int wheels = 4;
            int slot = -1, passkey = -1, httpResponseCode;
            String payload;

            // Create the JSON payload
            StaticJsonDocument<100> jsonDoc;
            jsonDoc["wheels"] = wheels;
            String requestBody;
            serializeJson(jsonDoc, requestBody);

            HTTPClient http;
            if (wheels == 2) {
                http.begin(get_entry_url_2);
            } else {
                http.begin(get_entry_url_4);
            }
            http.addHeader("Content-Type", "application/json");
            httpResponseCode = http.POST(requestBody);
            payload = http.getString();
            http.end();

            if (httpResponseCode == 200) {
                StaticJsonDocument<200> doc;
                DeserializationError error = deserializeJson(doc, payload);
                if (!error) {
                    slot = doc["slot"];
                    passkey = doc["passkey"];
                    Serial.println("Slot: " + String(slot));
                    Serial.println("Passkey: " + String(passkey));
                } else {
                    Serial.print("JSON parse failed: ");
                    Serial.println(error.c_str());
                }
            } else {
                Serial.print("POST failed: ");
                Serial.println(httpResponseCode);
            }

            Serial.print("Assigned Parking Slot: ");
            Serial.println(slot);
            digitalWrite(ENTRY_LED_PIN, HIGH);
            entryGateServo.write(90);
            entryGateOpen = true;
            entryNoDetectionStartTime = 0;
        }
    } else {
        entryStartTime = 0;
        if (entryGateOpen) {
            if (entryNoDetectionStartTime == 0) entryNoDetectionStartTime = millis();
            if (millis() - entryNoDetectionStartTime >= GATE_CLOSE_WAIT) {
                entryGateServo.write(0);
                digitalWrite(ENTRY_LED_PIN, LOW);
                entryGateOpen = false;
                entryNoDetectionStartTime = 0;
            }
        }
    }
}

void checkExitGate() {
    int exitSensorValue = analogRead(EXIT_IR_SENSOR);

    if (exitSensorValue < DETECTION_THRESHOLD) {
        if (exitStartTime == 0) exitStartTime = millis();
        if ((millis() - exitStartTime >= GATE_WAIT_TIME) && !exitGateOpen && !waitingForExitSlot) {
            HTTPClient http;
            http.begin(get_exit_pin_url);
            http.addHeader("Content-Type", "application/json");

            // Dummy body if needed (since POST usually has one)
            String body = "{}";
            int response = http.POST(body);

            if (response > 0) {
                String payload = http.getString();
                Serial.print("Exit code pin : ");
                Serial.println(payload);
            } else {
                Serial.print("POST failed with code: ");
                Serial.println(response);
            }

            http.end();
            waitingForExitSlot = true;
        }
    } else {
        exitStartTime = 0;
        if (exitGateOpen) {
            if (exitNoDetectionStartTime == 0) exitNoDetectionStartTime = millis();
            if (millis() - exitNoDetectionStartTime >= GATE_CLOSE_WAIT) {
                exitGateServo.write(0);
                digitalWrite(EXIT_LED_PIN, LOW);
                exitGateOpen = false;
                exitNoDetectionStartTime = 0;
            }
        }
    }

    if (waitingForExitSlot) {
        HTTPClient http;
        http.begin(get_exit_approved_url);
        http.addHeader("Content-Type", "application/json");

        String body = "{}";
        int response = http.POST(body);
        String approval = http.getString();
        http.end();

        if (approval.toInt() == 1) {
            Serial.println("Confirmed. Opening exit gate...");
            digitalWrite(EXIT_LED_PIN, HIGH);
            exitGateServo.write(90);
            exitGateOpen = true;
            waitingForExitSlot = false;
            exitNoDetectionStartTime = 0;
        } else {
            Serial.println("Approval not yet granted.");
        }
    }
}





void checkEntryGateGet() {
    int entrySensorValue = analogRead(ENTRY_IR_SENSOR);

    if (entrySensorValue < DETECTION_THRESHOLD) {
        if (entryStartTime == 0) entryStartTime = millis();
        if ((millis() - entryStartTime >= GATE_WAIT_TIME) && !entryGateOpen) {

            int wheels = 4;
            int slot = -1, passkey = -1, httpResponseCode;
            String payload;

            if (wheels == 2) {
                HTTPClient http;
                http.begin(get_entry_url_2);
                http.addHeader("Content-Type", "application/json");
                httpResponseCode = http.GET();
                payload = http.getString();
                http.end();
            } else {
                HTTPClient http;
                http.begin(get_entry_url_4);
                http.addHeader("Content-Type", "application/json");
                httpResponseCode = http.GET();
                payload = http.getString();
                http.end();
            }

            if (httpResponseCode == 200) {
                StaticJsonDocument<200> doc;
                DeserializationError error = deserializeJson(doc, payload);
                if (!error) {

                    slot = doc["slot"];
                    passkey = doc["passkey"];
                    Serial.println("Slot: " + String(slot));
                    Serial.println("Passkey: " + String(passkey));
                } else {
                    Serial.print("JSON parse failed: ");
                    Serial.println(error.c_str());
                }
            } else {
                Serial.print("GET failed: ");
                Serial.println(httpResponseCode);
            }

            Serial.print("Assigned Parking Slot: ");
            Serial.println(slot);
            digitalWrite(ENTRY_LED_PIN, HIGH);
            entryGateServo.write(90);
            entryGateOpen = true;
            entryNoDetectionStartTime = 0;
        }
    } else {
        entryStartTime = 0;
        if (entryGateOpen) {
            if (entryNoDetectionStartTime == 0) entryNoDetectionStartTime = millis();
            if (millis() - entryNoDetectionStartTime >= GATE_CLOSE_WAIT) {
                entryGateServo.write(0);
                digitalWrite(ENTRY_LED_PIN, LOW);
                entryGateOpen = false;
                entryNoDetectionStartTime = 0;
            }
        }
    }
}

void checkExitGateGet() {
    int exitSensorValue = analogRead(EXIT_IR_SENSOR);

    if (exitSensorValue < DETECTION_THRESHOLD) {
        if (exitStartTime == 0) exitStartTime = millis();
        if ((millis() - exitStartTime >= GATE_WAIT_TIME) && !exitGateOpen && !waitingForExitSlot) {
            HTTPClient http;
            http.begin(get_exit_pin_url);
            http.addHeader("Content-Type", "application/json");
            int response = http.GET();
            if (response > 0) {
                Serial.print("Exit code pin : ");
                Serial.println(http.getString());
            }
            http.end();
            waitingForExitSlot = true;
        }
    } else {
        exitStartTime = 0;
        if (exitGateOpen) {
            if (exitNoDetectionStartTime == 0) exitNoDetectionStartTime = millis();
            if (millis() - exitNoDetectionStartTime >= GATE_CLOSE_WAIT) {
                exitGateServo.write(0);
                digitalWrite(EXIT_LED_PIN, LOW);
                exitGateOpen = false;
                exitNoDetectionStartTime = 0;
            }
        }
    }

    if (waitingForExitSlot) {
        HTTPClient http;
        http.begin(get_exit_approved_url);
        http.addHeader("Content-Type", "application/json");
        int response = http.GET();
        String approval = http.getString();
        http.end();

        if (approval.toInt() == 1) {
            Serial.println("Confirmed. Opening exit gate...");
            digitalWrite(EXIT_LED_PIN, HIGH);
            exitGateServo.write(90);
            exitGateOpen = true;
            waitingForExitSlot = false;
            exitNoDetectionStartTime = 0;
        }
    }
}

