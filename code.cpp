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

// For ultrasonic sensors (parking slots)
#define NUM_SLOTS 4
#define speedOfSound 0.0343         // cm per microsecond
#define parking_threshold 4.00      // Car present if distance < 4 cm
#define parking_time_entry 10000    // 3 minutes in ms (for parked/left confirmation)
#define parking_time_exit 10000 
#define sensor_timeout 20000        // Timeout for pulseIn in microseconds

// For IR sensors & servo gates
#define ENTRY_IR_SENSOR 34
#define EXIT_IR_SENSOR 35
#define DETECTION_THRESHOLD 1000    // For analog read detection (car present if reading < threshold)

#define ENTRY_SERVO_PIN 12
#define EXIT_SERVO_PIN 13

#define ENTRY_LED_PIN 15          // LED for entry gate
#define EXIT_LED_PIN 2            // LED for exit gate

#define GATE_WAIT_TIME 5000       // 5 seconds wait time for car detection at gate
#define GATE_CLOSE_WAIT 3000      // 3 seconds continuous no detection to close gate

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
    ParkingSlot(14, 27, 4),   // Slot 1: trig 14, echo 27, LED on pin 4
    ParkingSlot(26, 25, 16),  // Slot 2: trig 26, echo 25, LED on pin 16
    ParkingSlot(33, 32, 17),  // Slot 3: trig 33, echo 32, LED on pin 17
    ParkingSlot(22, 23, 18)   // Slot 4: trig 22, echo 23, LED on pin 18
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

// Check each parking slot: update sensor status, handle timing for parked/car left, and print messages.
void checkParkingSlots(HTTPClient &http, unsigned long currentTime);

// Entry gate: if IR sensor detects a car (analog reading < threshold) continuously for 5 seconds,
// assign a random parking slot, light LED, and open servo gate. Then, when sensor no longer detects a car
// for 3 seconds, close the gate.
void checkEntryGate();

// Exit gate: if IR sensor detects a car continuously for 5 seconds,
// prompt for slot number (via Serial input). Once a valid number is entered,
// light LED and open servo gate until the car leaves (detected by sensor for 3 seconds).
void checkExitGate();

// Print sensor readings in a single line with 6 columns:
// Column1: Entry IR sensor reading
// Column2: Exit IR sensor reading
// Columns3-6: For each parking slot, print "YES" if distance < 4 cm, otherwise the distance value.
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
    Serial.println("Http post connection works at the start!\n");
    http.end();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {

        unsigned long currentTime = millis();
        
        checkEntryGate();
        checkExitGate();
        checkParkingSlots( currentTime);
        printSensorReadings(); 
    }
    else {
        Serial.println("WiFi Disconnected");
        Serial.println("Waiting 10 secs...");
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
    if (duration == 0) {
        return -1;
    }
    return (duration * speedOfSound) / 2;
}

void checkParkingSlots(unsigned long currentTime) {

    HTTPClient http_post;
    http_post.begin(post_url);
    http_post.addHeader("Content-Type", "application/json");

    for (int i = 0; i < NUM_SLOTS; i++) {
        float distanceCm = getDistance(slots[i].trigPin, slots[i].echoPin);
        
        if (distanceCm == -1) {
            slots[i].isSensorFaulty = true;
        } else {
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
                if(slots[i].isCarParked != true) {
                    String payload = "{\"type\": \"car-arrived\", \"slot\": " + String(i+1) +"}";
                    int response = http.POST(payload);
                    Serial.print("CAR - PARKED SENT TO SERVER : "); Serial.print(String(response)); Serial.println("\n");
                    slots[i].isCarParked = true;
                }
                else {
                    slots[i].isCarParked = true;
                    Serial.print("\nCar parked at slot "); Serial.print(i + 1);
                    Serial.print(" at: ");
                    Serial.println(slots[i].carArrivedTime / 1000);
                }   
            }
            
            if (!slots[i].isCarPresent && slots[i].isCarParked && (currentTime - slots[i].carLeftTime >= parking_time_exit )) {
                if(slots[i].isCarParked == true) {
                    String payload = "{\"type\": \"car-left\", \"slot\": " + String(i+1) +"}";
                    int response = http.POST(payload);
                    Serial.print("CAR - LEFT SENT TO SERVER : "); Serial.print(String(response)); Serial.println("\n");
                    slots[i].isCarParked = false;
                }
                else {
                    slots[i].isCarParked = false;
                    Serial.print("\nCar left from slot "); Serial.print(i + 1);
                    Serial.print(" at: ");
                    Serial.println(slots[i].carLeftTime / 1000);
                }
            }
        }
    }
    http_post.end();
}

void checkEntryGate() {
    int entrySensorValue = analogRead(ENTRY_IR_SENSOR);
    
    if (entrySensorValue < DETECTION_THRESHOLD) {
        if (entryStartTime == 0) {
            entryStartTime = millis();
        }
        if ((millis() - entryStartTime >= GATE_WAIT_TIME) && !entryGateOpen) {

            int wheels = 4; // TODO : take this as input

            // String payload = "{\"type\": \"entry-gate\", \"wheels\": " + to_string(wheels) +"}";
            // int response = http.POST(payload);
            // Serial.print("CAR ENTERED GATE WITH "); Serial.print(wheels); Serial.print("  wheels --- response:"); Serial.print(String(response)); Serial.println("\n");
            int slot;
            int passkey;    
            int httpResponseCode;
            String payload ;

            if (wheels  == 2){

                HTTPClient http_get_entry_2;
                http_get_entry_2.begin(get_entry_url_2);
                http_get_entry_2.addHeader("Content-Type", "application/json");

                httpResponseCode = http_get_entry_2.GET();
                payload = http_get_entry_2.getString();
            

                http_get_entry_2.end();

            }
            else if (wheels  == 4){
                HTTPClient http_get_entry_4;
                http_get_entry_4.begin(get_entry_url_4);
                http_get_entry_4.addHeader("Content-Type", "application/json");

                httpResponseCode = http_get_entry_4.GET();
                payload = http_get_entry_4.getString();

                http_get_entry_4.end();
            }
            

            if (httpResponseCode == 200) {
                Serial.println("Raw JSON: " + payload);

                StaticJsonDocument<200> doc;

                DeserializationError error = deserializeJson(doc, payload);
                if (error) {
                    Serial.print("JSON parse failed: ");
                    Serial.println(error.c_str());
                } else {
                    slot = doc["slot"];
                    passkey = doc["passkey"];

                    Serial.println("Slot: " + String(slot));
                    Serial.println("Passkey: " + String(passkey));
                    // TODO : put to LCD 
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
            if (entryNoDetectionStartTime == 0) {
                entryNoDetectionStartTime = millis();
            }
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
        if (exitStartTime == 0) {
            exitStartTime = millis();
        }
        if ((millis() - exitStartTime >= GATE_WAIT_TIME) && !exitGateOpen && !waitingForExitSlot) {
            // Serial.println("Enter your parking slot number:");   -- done at website now

            HTTPClient http_get_exit_pin;
            http_get_exit_pin.begin(get_exit_pin_url);
            http_get_exit_pin.addHeader("Content-Type", "application/json");

            int httpResponseCode = http_get_exit_pin.GET();
            String exit_pin = http_get_exit_pin.getString();
            Serial.print("Exit code pin : ");Serial.println(exit_pin);
            // TODO : DISPLAY IT ON LCD SCREEN FOR 15 SECS
            http_get_exit_pin.end();
            waitingForExitSlot = true;
        }
    } 
    else {
        exitStartTime = 0;
        if (exitGateOpen) {
            if (exitNoDetectionStartTime == 0) {
                exitNoDetectionStartTime = millis();
            }
            if (millis() - exitNoDetectionStartTime >= GATE_CLOSE_WAIT) {
                exitGateServo.write(0);
                digitalWrite(EXIT_LED_PIN, LOW);
                exitGateOpen = false;
                exitNoDetectionStartTime = 0;
            }
        }
    }


    
    if (waitingForExitSlot) {
        HTTPClient http_get_exit_approved;
        http_get_exit_approved.begin(get_exit_approved_url);
        http_get_exit_approved.addHeader("Content-Type", "application/json");

        int httpResponseCode = http_get_exit_approved.GET();
        String approval = http_get_exit_approved.getString();
        // TODO : DISPLAY IT ON LCD SCREEN FOR 8 SECS

        http_get_exit_approved.end();

        if ( approval.toInt() == 1 ) {
            Serial.println("Confirmed. Opening exit gate...");
            digitalWrite(EXIT_LED_PIN, HIGH);
            exitGateServo.write(90);
            exitGateOpen = true;
            waitingForExitSlot = false;
            exitNoDetectionStartTime = 0;
        }
        //  else {
        //     Serial.println("Invalid slot number. Try again.");
        // }
    }
}

void printSensorReadings() {
    String output = "";
    
    output += String(analogRead(ENTRY_IR_SENSOR)) + "\t";
    output += String(analogRead(EXIT_IR_SENSOR)) + "\t";
    
    for (int i = 0; i < NUM_SLOTS; i++) {
        float d = slots[i].lastValidDistance;
        if (!slots[i].isSensorFaulty) {
            if (d < parking_threshold) {
                output += "YES\t";
            } else {
                output += String(d, 1) + "cm\t";
            }
        } else {
            output += "ERR\t";
        }
    }
    
    Serial.println(output);
}