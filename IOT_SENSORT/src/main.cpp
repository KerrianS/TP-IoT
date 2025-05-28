#include "config.h"
#include "version.h"

#include "driver/rtc_io.h"

#define USE_EXT0_WAKEUP 0

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Adafruit_NeoPixel.h>

#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>

// AHT20 Temperature & Humidity sensor
#if AHT20_ENABLE
#include <Adafruit_AHTX0.h>
Adafruit_AHTX0 aht;
#endif

// SGP40 Air Quality Sensor
#if SGP40_ENABLE
#include <Adafruit_SGP40.h>
Adafruit_SGP40 sgp;
#endif

// BH1750 Luxmeter
#if BH1750_ENABLE
#include <hp_BH1750.h>
hp_BH1750 bh1750;
#endif

// Battery pin
#define VBATPIN A13

// Initialize underlying client, used to establish a connection
#if ENCRYPTED
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif

// Initalize the Mqtt client instance
Arduino_MQTT_Client mqttClient(espClient);

// Initialize ThingsBoard instance
ThingsBoard tb(mqttClient);

// Statuses for subscribing to shared attributes
bool RPC_subscribed = false;

// Initial client attributes sent
bool init_att_published = false;

// Forward declarations
void InitWiFi();
bool reconnect();

// Définition des seuils d'alarme
#define TEMP_HIGH 20.0
#define TEMP_LOW 0.0
#define HUMIDITY_HIGH 70.0
#define HUMIDITY_LOW 20.0
#define VOC_HIGH 50
#define LUX_LOW 50.0
#define BATTERY_LOW 3.3

// Variables pour suivre l'état des alarmes
bool temp_alarm = false;
bool humidity_alarm = false;
bool voc_alarm = false;
bool lux_alarm = false;
bool battery_alarm = false;

// Variables pour stocker les dernières mesures
float last_temp = 0;
float last_humidity = 0;
uint16_t last_voc = 0;
float last_lux = 0;
float last_battery = 0;

void checkAndSendAlarms(float temp, float humidity, uint16_t voc, float lux, float battery) {
    // Alarme température haute
    if (temp > TEMP_HIGH) {
        if (!temp_alarm) {
            tb.sendTelemetryData("temp_alarm_high", true);
            temp_alarm = true;
            Serial.printf("ALARME: Température > %.1f°C : %.2f°C\n", TEMP_HIGH, temp);
        }
    } else if (temp_alarm && temp <= TEMP_HIGH) {
        tb.sendTelemetryData("temp_alarm_high", false);
        temp_alarm = false;
    }

    // Alarme température basse
    static bool temp_low_alarm = false;
    if (temp < TEMP_LOW) {
        if (!temp_low_alarm) {
            tb.sendTelemetryData("temp_alarm_low", true);
            temp_low_alarm = true;
            Serial.printf("ALARME: Température < %.1f°C : %.2f°C\n", TEMP_LOW, temp);
        }
    } else if (temp_low_alarm && temp >= TEMP_LOW) {
        tb.sendTelemetryData("temp_alarm_low", false);
        temp_low_alarm = false;
    }

    // Alarme VOC
    if (voc > VOC_HIGH) {
        if (!voc_alarm) {
            tb.sendTelemetryData("voc_alarm", true);
            voc_alarm = true;
            Serial.printf("ALARME: VOC > %d : %d\n", VOC_HIGH, voc);
        }
    } else if (voc_alarm && voc < VOC_HIGH) {
        tb.sendTelemetryData("voc_alarm", false);
        voc_alarm = false;
    }

    // Forcer l'alarme de luminosité pour test (à retirer si plus utile)
    // tb.sendTelemetryData("lux_alarm", true);
    // Serial.println("ALARME TEST: Luminosité faible (forcée pour test)");
    // lux_alarm = true;

    // Vérification batterie (inchangé)
    if (battery < BATTERY_LOW) {
        if (!battery_alarm) {
            tb.sendTelemetryData("battery_alarm", true);
            battery_alarm = true;
            Serial.printf("ALARME: Batterie faible: %.2fV\n", battery);
        }
    } else if (battery_alarm) {
        tb.sendTelemetryData("battery_alarm", false);
        battery_alarm = false;
    }
}

void setup()
{
    Serial.begin(9600);
    Serial.println("ESP32 démarré !");

    // Initialisation des capteurs
    Wire.begin();

    // Initialisation AHT20
    #if AHT20_ENABLE
    if (!aht.begin()) {
        Serial.println("Erreur: Impossible de trouver le capteur AHT20!");
    } else {
        Serial.println("AHT20 initialisé avec succès!");
    }
    #endif

    // Initialisation SGP40
    #if SGP40_ENABLE
    Wire.begin();
    delay(1000);  // Attendre que le capteur soit prêt
    
    // Tentative d'initialisation avec retry
    bool sgp_ok = false;
    for(int i = 0; i < 3 && !sgp_ok; i++) {
        if (sgp.begin()) {
            sgp_ok = true;
            Serial.println("SGP40 initialisé avec succès!");
            // Test de mesure initial
            uint16_t test_raw = sgp.measureRaw(25.0, 50.0);  // Test avec des valeurs standard
            Serial.printf("Test initial SGP40 - Signal brut: %d\n", test_raw);
        } else {
            Serial.printf("Tentative %d: Erreur d'initialisation SGP40\n", i+1);
            Wire.end();
            delay(1000);
            Wire.begin();
            delay(1000);
        }
    }
    
    if (!sgp_ok) {
        Serial.println("ERREUR: Impossible d'initialiser le SGP40 après 3 tentatives!");
    }
    #endif

    // Initialisation BH1750
    #if BH1750_ENABLE
    if (!bh1750.begin(BH1750_TO_GROUND)) {
        Serial.println("Erreur: Impossible de trouver le capteur BH1750!");
    } else {
        Serial.println("BH1750 initialisé avec succès!");
    }
    #endif

    // Init Wifi connexion
    InitWiFi();
}

void loop() {
    if (!reconnect()) {
        return;
    }

    // Check Thingsboard connection
    if (!tb.connected()) {
        Serial.printf("Connecting to: (%s) with token (%s)\n", THINGSBOARD_SERVER, TOKEN);
        if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
            Serial.println("Failed to connect");
            return;
        }
    }

    // Lecture des capteurs
    #if AHT20_ENABLE
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    last_temp = temp.temperature;
    last_humidity = humidity.relative_humidity;
    tb.sendTelemetryData("temperature", last_temp);
    tb.sendTelemetryData("humidity", last_humidity);
    #endif

    #if SGP40_ENABLE
    // Stabilisation toutes les 80 secondes
    static unsigned long last_stabilization = 0;
    if (millis() - last_stabilization >= 80000) {  // 80 secondes = 80000 ms
        // Mesure du signal brut d'abord
        uint16_t raw_signal = sgp.measureRaw(temp.temperature, humidity.relative_humidity);
        Serial.printf("Signal brut SGP40: %d\n", raw_signal);
        
        if (raw_signal > 0) {  // Ne mesurer le VOC que si le signal brut est valide
            // Puis mesure du VOC index
            last_voc = sgp.measureVocIndex(temp.temperature, humidity.relative_humidity);
            Serial.printf("Température: %.2f°C, Humidité: %.2f%%\n", temp.temperature, humidity.relative_humidity);
            Serial.printf("VOC Index mesuré: %d\n", last_voc);
            
            last_stabilization = millis();
            // Envoyer le VOC index seulement après la stabilisation
            tb.sendTelemetryData("voc", last_voc);
        } else {
            Serial.println("ERREUR: Signal brut SGP40 invalide!");
        }
    }
    #endif

    #if BH1750_ENABLE
    bh1750.start();  // Démarrer une nouvelle mesure
    last_lux = bh1750.getLux();  // Lire la valeur
    tb.sendTelemetryData("lux", last_lux);
    #endif

    // Lecture de la tension de la batterie
    float measuredvbat = analogRead(VBATPIN);
    measuredvbat *= 2;    // Diviseur de tension 1/2
    measuredvbat *= 3.3;  // Référence 3.3V
    measuredvbat /= 4095; // 12-bit ADC
    last_battery = measuredvbat;
    tb.sendTelemetryData("battery", last_battery);

    // Vérification et envoi des alarmes
    checkAndSendAlarms(last_temp, last_humidity, last_voc, last_lux, last_battery);

    tb.loop();
    delay(2000);  // Attendre 2 secondes entre chaque lecture
}

// @brief Initalizes WiFi connection,
// will endlessly delay until a connection has been successfully established
void InitWiFi()
{
#if SERIAL_DEBUG
    Serial.println("Connecting to AP ...");
#endif
    // Attempting to establish a connection to the given WiFi network
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED)
    {
        // Delay 500ms until a connection has been successfully established
        delay(500);
#if SERIAL_DEBUG
        Serial.print(".");
#endif
    }
#if SERIAL_DEBUG
    Serial.printf("\nConnected to AP : %s\n", WIFI_SSID);
#endif
#if ENCRYPTED
    espClient.setCACert(ROOT_CERT);
#endif
}

/// @brief Reconnects the WiFi uses InitWiFi if the connection has been removed
/// @return Returns true as soon as a connection has been established again
bool reconnect()
{
    // Check to ensure we aren't connected yet
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED)
    {
        return true;
    }

    // If we aren't establish a new connection to the given WiFi network
    InitWiFi();
    return true;
}

