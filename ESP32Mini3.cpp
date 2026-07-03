/*
  =====================================================================
  РОЗУМНИЙ БУДИНОК НА ESP32-S2 mini (MQTT + RFID + SERVO + ALARMS)
  =====================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

// ==========================================
// НАЛАШТУВАННЯ МЕРЕЖІ ТА MQTT
// ==========================================
const char* ssid = "CSN_307";
const char* password = "LAN307LAN";
const char* mqtt_server = "192.168.10.117";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// РОЗПІНОВКА (ESP32-S2 Mini)
// ==========================================

// Світлодіоди
const int LED1_PIN = 17;   // Світлодіод кімнати 1
const int LED2_PIN = 21;   // Світлодіод кімнати 2
const int LED3_PIN = 4;    // Сигнальний світлодіод охорони/аварії

// Цифрові сенсори та пристрої
const int DHT_PIN = 6;
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

const int PIR_PIN = 40;
const int BUZZER_PIN = 16; 
const int SERVO_PIN = 10;
Servo doorServo;

// Аналогові сенсори
const int MQ135_PIN = 12;
const int SOIL_MOISTURE_PIN = 8;

// Піни для SPI та RFID RC522
const int SS_PIN = 33;     
const int MOSI_PIN = 35;   
const int SCK_PIN = 36;    
const int MISO_PIN = 37;   
const int RST_PIN = 38;    

MFRC522 rfid(SS_PIN, RST_PIN);

// ==========================================
// ГЛОБАЛЬНІ ЗМІННІ ТА СТАНИ
// ==========================================
bool isArmed = false;
bool isIntrusionAlarm = false;
bool isFireAlarm = false;

unsigned long previousMillisSensors = 0;
const long sensorInterval = 5000;

unsigned long previousMillisLedBlink = 0;
bool led3State = false;

const int GAS_THRESHOLD = 4400;
const String ALLOWED_CARD_UID = "DB0EEF0D";

// ==========================================
// ДОПОМІЖНІ ФУНКЦІЇ ЗВУКУ ТА ІНДИКАЦІЇ
// ==========================================

void blinkSignalLedOnce() {
  digitalWrite(LED3_PIN, HIGH);
  delay(200);
  digitalWrite(LED3_PIN, LOW);
}

void playSuccessMelody() {
  tone(BUZZER_PIN, 1000, 100);
  delay(120);
  tone(BUZZER_PIN, 1500, 100);
  delay(120);
  tone(BUZZER_PIN, 2000, 150);
  delay(150);
}

void playAccessDeniedTone() {
  tone(BUZZER_PIN, 250, 400); // Низький короткий звук відмови
}

void armSystem() {
  if (!isArmed) {
    isArmed = true;
    doorServo.write(0); // Закриваємо двері на замок
    blinkSignalLedOnce();
    Serial.println("Систему ВЗЯТО під охорону. Двері зачинено.");
    client.publish("home/security/mode", "ARM");
  }
}

void disarmSystem() {
  isArmed = false;
  isIntrusionAlarm = false;
  noTone(BUZZER_PIN);
  doorServo.write(90); // Відкриваємо двері
  digitalWrite(LED3_PIN, LOW);
  blinkSignalLedOnce();
  Serial.println("Систему ЗНЯТО з охорони. Двері відчинено.");
  client.publish("home/security/mode", "DISARM");
}

// ==========================================
// МЕРЕЖА ТА MQTT
// ==========================================

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Підключення до Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi підключено!");
  Serial.print("IP адреса: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.print("MQTT Команда ["); Serial.print(topic); Serial.print("]: "); Serial.println(msg);

  if (String(topic) == "home/light/room1") {
    if (msg == "ON") digitalWrite(LED1_PIN, HIGH);
    else if (msg == "OFF") digitalWrite(LED1_PIN, LOW);
  }
  else if (String(topic) == "home/light/room2") {
    if (msg == "ON") digitalWrite(LED2_PIN, HIGH);
    else if (msg == "OFF") digitalWrite(LED2_PIN, LOW);
  }
  else if (String(topic) == "home/security/mode") {
    if (msg == "ARM") armSystem();
    else if (msg == "DISARM") disarmSystem();
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Спроба підключення до MQTT...");
    String clientId = "ESP32Client-SmartHome";

    if (client.connect(clientId.c_str())) {
      Serial.println("Підключено до брокера!");
      client.subscribe("home/light/room1");
      client.subscribe("home/light/room2");
      client.subscribe("home/security/mode");
    } else {
      Serial.print("Помилка підключення, rc=");
      Serial.print(client.state());
      delay(5000);
    }
  }
}

// ==========================================
// ІНІЦІАЛІЗАЦІЯ (setup)
// ==========================================

void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(LED3_PIN, OUTPUT);

  noTone(BUZZER_PIN);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);
  digitalWrite(LED3_PIN, LOW);

  doorServo.attach(SERVO_PIN);
  doorServo.write(90); // На старті двері відчинені

  dht.begin();

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  Serial.println("--- RFID RC522 та Сервопривід ініціалізовано ---");

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// ==========================================
// ОСНОВНИЙ ЦИКЛ (loop)
// ==========================================

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long currentMillis = millis();

  // --- 1. КЕРУВАННЯ СИГНАЛЬНИМ СВІТЛОДІОДОМ LED3 ---
  if (isFireAlarm) {
    // Безперервне блимання при пожежі/газу
    if (currentMillis - previousMillisLedBlink >= 200) {
      previousMillisLedBlink = currentMillis;
      led3State = !led3State;
      digitalWrite(LED3_PIN, led3State);
    }
  } else if (isIntrusionAlarm) {
    digitalWrite(LED3_PIN, HIGH); // Постійно горить при тривозі проникнення
  } else {
    digitalWrite(LED3_PIN, LOW);  // У штатному режимі вимкнений
  }

  // --- 2. ЛОГІКА ЗЧИТУВАЧА RFID ---
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidString = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      uidString += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      uidString += String(rfid.uid.uidByte[i], HEX);
    }
    uidString.toUpperCase();
    Serial.print("Зчитано RFID UID: ");
    Serial.println(uidString);

    client.publish("home/security/rfid", uidString.c_str());

    if (uidString == ALLOWED_CARD_UID) {
      playSuccessMelody();
      if (isArmed) {
        disarmSystem(); // Вимикаємо охорону та відчиняємо двері
      }
    } else {
      playAccessDeniedTone();
      Serial.println("УВАГА! Невідома картка доступу.");
    }

    rfid.PICC_HaltA();
  }

  // --- 3. ЛОГІКА ДАТЧИКА РУХУ (PIR) ---
  static bool lastPirState = false;
  bool currentPirState = digitalRead(PIR_PIN);
  if (currentPirState != lastPirState) {
    lastPirState = currentPirState;
    if (currentPirState == HIGH) {
      client.publish("home/security/motion", "ON");
      if (isArmed) {
        isIntrusionAlarm = true;
        tone(BUZZER_PIN, 2000); // Сирена тривоги
        client.publish("home/security/alarm", "MOTION_DETECTED");
      }
    } else {
      client.publish("home/security/motion", "OFF");
      if (!isArmed && !isFireAlarm) noTone(BUZZER_PIN);
    }
  }

  // --- 4. ПЕРІОДИЧНЕ ОПИТУВАННЯ СЕНСОРІВ ---
  if (currentMillis - previousMillisSensors >= sensorInterval) {
    previousMillisSensors = currentMillis;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      client.publish("home/climate/temperature", String(t).c_str());
      client.publish("home/climate/humidity", String(h).c_str());
    }

    int gasLevel = analogRead(MQ135_PIN);
    client.publish("home/safety/gas", String(gasLevel).c_str());
    
    if (gasLevel > GAS_THRESHOLD) {
      isFireAlarm = true;
      client.publish("home/safety/alarm", "FIRE_DETECTED");
      tone(BUZZER_PIN, 2500); 

      // Аварійне вимкнення освітлення кімнат при небезпеці
      digitalWrite(LED1_PIN, LOW);
      digitalWrite(LED2_PIN, LOW);
    } else {
      if (isFireAlarm) {
        isFireAlarm = false;
        if (!isIntrusionAlarm) noTone(BUZZER_PIN);
      }
    }

    int soilMoisture = analogRead(SOIL_MOISTURE_PIN);
    client.publish("home/garden/soil", String(soilMoisture).c_str());
  }
}