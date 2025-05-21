#include "config.h"
#include "version.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Adafruit_NeoPixel.h>

#include <Arduino_MQTT_Client.h>
#include <Server_Side_RPC.h>
#include <ThingsBoard.h>

// Initialize underlying client, used to establish a connection
#if ENCRYPTED
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif

// Initalize the Mqtt client instance
Arduino_MQTT_Client mqttClient(espClient);

// Actuator status global variables
bool VMC_STATUS;	 // Status VMC
bool LIGHT_STATUS;	 // Status light
bool HEATER_STATUS;	 // Status heater
bool AC_STATUS;		 // Status A/C

/// @brief Initalizes WiFi connection,
// will endlessly delay until a connection has been successfully established
void InitWiFi();

/// @brief Reconnects the WiFi uses InitWiFi if the connection has been removed
/// @return Returns true as soon as a connection has been established again
bool reconnect();

/// @brief Update callback that will be called as soon as one of the provided shared attributes
/// changes value, if none are provided we subscribe to any shared attribute change instead
/// @param data Data containing the shared attributes that were changed and their current value
void processSharedAttributeUpdate(const JsonObjectConst& data);

/// @brief Process Light change RPC
void processSwitchLightChange(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process VMC change RPC
void processSwitchVmcChange(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process heater change RPC
void processSwitchHeaterChange(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process AC change RPC
void processSwitchACChange(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process light status inquiry RPC
void getSwitchLight(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process VMC status inquiry RPC
void getSwitchVmc(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process heater status inquiry RPC
void getSwitchHeater(const JsonVariantConst& data, JsonDocument& response);

/// @brief Process AC status inquiry RPC
void getSwitchAC(const JsonVariantConst& data, JsonDocument& response);

/// @brief Set light pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setLight(bool status);

/// @brief Set VMC pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setVMC(bool status);

/// @brief Set heater pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setHeater(bool status);

/// @brief Set AC pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setAC(bool status);

constexpr const char CONNECTING_MSG[]	= "Connecting to: (%s) with token (%s)\n";
constexpr const char LIGHT_RELAY_KEY[]	= "LIGHT_RELAY";
constexpr const char VMC_RELAY_KEY[]	= "VMC_RELAY";
constexpr const char HEATER_RELAY_KEY[] = "HEATER_RELAY";
constexpr const char AC_RELAY_KEY[]		= "AC_RELAY";
constexpr const char VERSION_KEY[]		= "VERSION";

constexpr const char RPC_JSON_METHOD[]			   = "example_json";
constexpr const char RPC_GET_LIGHT_SWITCH_METHOD[] = "get_light_switch";
constexpr const char RPC_SET_LIGHT_SWITCH_METHOD[] = "set_light_switch";
constexpr const char RPC_LIGHT_SWITCH_KEY[]		   = "LIGHT_RELAY";

constexpr const char RPC_GET_VMC_SWITCH_METHOD[] = "get_vmc_switch";
constexpr const char RPC_SET_VMC_SWITCH_METHOD[] = "set_vmc_switch";
constexpr const char RPC_VMC_SWITCH_KEY[]		 = "VMC_RELAY";

constexpr const char RPC_GET_HEATER_SWITCH_METHOD[] = "get_heater_switch";
constexpr const char RPC_SET_HEATER_SWITCH_METHOD[] = "set_heater_switch";
constexpr const char RPC_HEATER_SWITCH_KEY[]		= "HEATER_RELAY";

constexpr const char RPC_GET_AC_SWITCH_METHOD[] = "get_ac_switch";
constexpr const char RPC_SET_AC_SWITCH_METHOD[] = "set_ac_switch";
constexpr const char RPC_AC_SWITCH_KEY[]		= "AC_RELAY";

// Maximum size packets will ever be sent or received by the underlying MQTT client,
// if the size is to small messages might not be sent or received messages will be discarded
constexpr uint16_t MAX_MESSAGE_SEND_SIZE		= 128U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE		= 128U;
constexpr uint8_t  MAX_RPC_SUBSCRIPTIONS		= 8U;
constexpr uint8_t  MAX_RPC_RESPONSE				= 16U;
constexpr uint8_t  MAX_RPC_REQUEST				= 10U;
constexpr uint64_t REQUEST_TIMEOUT_MICROSECONDS = 5000U * 1000U;

// Maximum amount of attributs we can request or subscribe, has to be set both in the ThingsBoard
// template list and Attribute_Request_Callback template list and should be the same as the amount
// of variables in the passed array. If it is less not all variables will be requested or subscribed
constexpr size_t MAX_ATTRIBUTES = 3U;

// Initialize used apis
Server_Side_RPC<MAX_RPC_SUBSCRIPTIONS, MAX_RPC_RESPONSE> server_rpc;
const std::array<IAPI_Implementation*, 1U>				 apis
	= { &server_rpc /*, &client_rpc , &shared_update */ };

// Initialize ThingsBoard instance with the maximum needed buffer size
ThingsBoard tb(
	mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

// Statuses for subscribing to shared attributes
bool RPC_subscribed = false;

// Initial client attributes sent
bool init_att_published = false;

void setup()
{
// Initalize serial connection for debugging
#if SERIAL_DEBUG
	Serial.begin(SERIAL_DEBUG_BAUD);
	delay(200);
#endif

	// Set VMC, LIGHT, HEATER & AC pin mode as OUTPUT
	pinMode(VMC_PIN, OUTPUT);
	pinMode(LIGHT_PIN, OUTPUT);
	pinMode(HEATER_PIN, OUTPUT);
	pinMode(AC_PIN, OUTPUT);

	// Set initial value LOW for VMC, LIGHT, HEATER & AC pin
	digitalWrite(VMC_PIN, LOW);
	digitalWrite(LIGHT_PIN, LOW);
	digitalWrite(HEATER_PIN, LOW);
	digitalWrite(AC_PIN, LOW);

	// Set global variables actuator status as FALSE

	// Init Wifi connexion
	InitWiFi();
}

void loop()
{
	if (!reconnect())
	{
		return;
	}

	// Check Thingsboard connection
	if (!tb.connected())
	{
		// Reconnect to the ThingsBoard server,
		// if a connection was disrupted or has not yet been established
#if SERIAL_DEBUG
		Serial.printf(CONNECTING_MSG, THINGSBOARD_SERVER, TOKEN);
#endif
		if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT))
		{
#if SERIAL_DEBUG
			Serial.println("Failed to connect");
#endif
			return;
		}
	}

	// Send initial values
	if (!init_att_published)
	{
		Serial.println("Sending device type attribute...");
		tb.sendAttributeData(VERSION_KEY, VERSION);
		tb.sendAttributeData(LIGHT_RELAY_KEY, LIGHT_STATUS);
		// NEED TO COMPLETED
		init_att_published = true;
	}

	if (!RPC_subscribed)
	{
		Serial.println("Requesting RPC....");

		Serial.println("Subscribing for RPC...");
		const RPC_Callback callbacks[MAX_RPC_SUBSCRIPTIONS] = {
			{ RPC_SET_LIGHT_SWITCH_METHOD, processSwitchLightChange },
			{ RPC_SET_VMC_SWITCH_METHOD, processSwitchVmcChange }
			// NEED TO BE COMPLETED
		};

		if (!server_rpc.RPC_Subscribe(callbacks + 0U, callbacks + MAX_RPC_SUBSCRIPTIONS))
		{
			Serial.println("Failed to subscribe for RPC");
			return;
		}

		Serial.println("Subscribe done");
		RPC_subscribed = true;
	}

	tb.loop();
}

/// @brief Initalizes WiFi connection,
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

/// @brief Process VMC change RPC
void processSwitchVmcChange(const JsonVariantConst& data, JsonDocument& response)
{
	bool rcvSwitchStatus;

#if SERIAL_DEBUG
	Serial.println("Received the set vmc switch method");
#endif

	const int switch_state = data["enabled"];

	if (switch_state == 0)
	{
		rcvSwitchStatus = false;
	}
	if (switch_state == 1)
	{
		rcvSwitchStatus = true;
	}

#if SERIAL_DEBUG
	Serial.print("VMC switch received state: ");
	Serial.println(switch_state);
#endif

	response.set(rcvSwitchStatus);
	setVMC(rcvSwitchStatus);
}

/// @brief Process light status inquiry RPC
void getSwitchLight(const JsonVariantConst& data, JsonDocument& response)
{
#if SERIAL_DEBUG
	Serial.println("Received the json RPC method");
#endif

	// Size of the response document needs to be configured to the size of the innerDoc + 1.
	StaticJsonDocument<16>					doc;
	StaticJsonDocument<JSON_OBJECT_SIZE(1)> innerDoc;
	if (LIGHT_STATUS)
	{
		innerDoc = true;
	}
	else
	{
		innerDoc = false;
	}
	response[LIGHT_RELAY_KEY] = innerDoc;
}

/// @brief Set heater pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setHeater(bool status)
{
#if SERIAL_DEBUG
	Serial.printf("Changing heater status to : %s\n", status ? "true" : "false");
#endif
	digitalWrite(HEATER_PIN, status);
	HEATER_STATUS = status;
	if (!tb.connected())
	{
// Reconnect to the ThingsBoard server,
// if a connection was disrupted or has not yet been established
#if SERIAL_DEBUG
		Serial.printf(CONNECTING_MSG, THINGSBOARD_SERVER, TOKEN);
#endif
		if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT))
		{
#if SERIAL_DEBUG
			Serial.println("Failed to connect");
#endif
		}
	}
	tb.sendAttributeData(HEATER_RELAY_KEY, HEATER_STATUS);
	return status;
}

/// @brief Set VMC pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setVMC(bool status)
{
#if SERIAL_DEBUG
	Serial.printf("Changing VMC status to : %s\n", status ? "true" : "false");
#endif
	digitalWrite(VMC_PIN, status);
	VMC_STATUS = status;
	if (!tb.connected())
	{
		// Reconnect to the ThingsBoard server,
		// if a connection was disrupted or has not yet been established
#if SERIAL_DEBUG
		Serial.printf(CONNECTING_MSG, THINGSBOARD_SERVER, TOKEN);
#endif
		if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT))
		{
#if SERIAL_DEBUG
			Serial.println("Failed to connect");
#endif
		}
	}
	tb.sendAttributeData(VMC_RELAY_KEY, VMC_STATUS);
	return status;
}

/// @brief Process Light change RPC
void processSwitchLightChange(const JsonVariantConst& data, JsonDocument& response)
{
	bool rcvSwitchStatus;

#if SERIAL_DEBUG
	Serial.println("Received the set light switch method");
#endif

	const int switch_state = data["enabled"];

	if (switch_state == 0)
	{
		rcvSwitchStatus = false;
	}
	if (switch_state == 1)
	{
		rcvSwitchStatus = true;
	}

#if SERIAL_DEBUG
	Serial.print("Light switch received state: ");
	Serial.println(switch_state);
#endif

	response.set(rcvSwitchStatus);
	setLight(rcvSwitchStatus);
}

/// @brief Set light pin value and publish it to Thingsboard server
/// @return Returns true if pin is HIGH, false if LOW
bool setLight(bool status)
{
#if SERIAL_DEBUG
	Serial.printf("Changing light status to : %s\n", status ? "true" : "false");
#endif
	digitalWrite(LIGHT_PIN, status);
	LIGHT_STATUS = status;
	if (!tb.connected())
	{
		// Reconnect to the ThingsBoard server,
		// if a connection was disrupted or has not yet been established
#if SERIAL_DEBUG
		Serial.printf(CONNECTING_MSG, THINGSBOARD_SERVER, TOKEN);
#endif
		if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT))
		{
#if SERIAL_DEBUG
			Serial.println("Failed to connect");
#endif
		}
	}
	tb.sendAttributeData(LIGHT_RELAY_KEY, LIGHT_STATUS);
	return status;
}