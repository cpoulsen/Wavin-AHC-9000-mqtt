#include <FS.h>  
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include "WavinController.h"
#include "PrivateConfig.h"
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
WiFiManager wifiManager;

#define HOST "WavinGW-%s" // Change this to whatever you like. 
// MQTT defines
// Esp8266 MAC will be added to the device name, to ensure unique topics
// Default is topics like 'heat/floorXXXXXXXXXXXX/3/target', where 3 is the output id and XXXXXXXXXXXX is the mac
const String   MQTT_PREFIX              = "heat/";       // include tailing '/' in prefix
const String   MQTT_DEVICE_NAME         = "floor";       // only alfanumeric and no '/'
const String   MQTT_ONLINE              = "/online";      
const String   MQTT_SUFFIX_CURRENT      = "/current";    // include heading '/' in all suffixes
const String   MQTT_SUFFIX_SETPOINT_GET = "/target";
const String   MQTT_SUFFIX_SETPOINT_SET = "/target_set";
const String   MQTT_SUFFIX_MODE_GET     = "/mode";
const String   MQTT_SUFFIX_MODE_SET     = "/mode_set";
const String   MQTT_SUFFIX_BATTERY      = "/battery";
const String   MQTT_SUFFIX_OUTPUT       = "/output";

const String   MQTT_VALUE_MODE_STANDBY  = "off";
const String   MQTT_VALUE_MODE_MANUAL   = "heat";

const String   MQTT_CLIENT = "Wavin-AHC-9000-mqtt";       // mqtt client_id prefix. Will be suffixed with Esp8266 mac to make it unique

String mqttDeviceNameWithMac;
String mqttClientWithMac;

// Operating mode is controlled by the MQTT_SUFFIX_MODE_ topic.
// When mode is set to MQTT_VALUE_MODE_MANUAL, temperature is set to the value of MQTT_SUFFIX_SETPOINT_
// When mode is set to MQTT_VALUE_MODE_STANDBY, the following temperature will be used
const float STANDBY_TEMPERATURE_DEG = 5.0;

const uint16_t RECIEVE_TIMEOUT_MS = 1000;
WavinController wavinController(RECIEVE_TIMEOUT_MS);
char chipid[12];
char mqttserver[40]=MQTTSERVER;
char mqttusername[20]=MQTTUSERNAME;
char mqttpassword[20]=MQTTPASSWORD;

WiFiServer server(80);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastUpdateTime = 0;

const uint16_t POLL_TIME_MS = 5000;

struct lastKnownValue_t {
  uint16_t temperature;
  uint16_t setpoint;
  uint16_t battery;
  uint16_t status;
  uint16_t mode;
} lastSentValues[WavinController::NUMBER_OF_CHANNELS];

const uint16_t LAST_VALUE_UNKNOWN = 0xFFFF;

bool configurationPublished[WavinController::NUMBER_OF_CHANNELS];


// Read a float value from a non zero terminated array of bytes and
// return 10 times the value as an integer
uint16_t temperatureFromString(String payload)
{
  float targetf = payload.toFloat();
  return (unsigned short)(targetf * 10);
}


// Returns temperature in degrees with one decimal
String temperatureAsFloatString(uint16_t temperature)
{
  float temperatureAsFloat = ((float)temperature) / 10;
  return String(temperatureAsFloat, 1);
}


uint8_t getIdFromTopic(char* topic)
{
  unsigned int startIndex = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/").length();
  int i = 0;
  uint8_t result = 0;

  while(topic[startIndex+i] != '/' && i<3)
  {
    result = result * 10 + (topic[startIndex+i]-'0');
    i++;
  }

  return result;
}


void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  String topicString = String(topic);
  
  char terminatedPayload[length+1];
  for(unsigned int i=0; i<length; i++)
  {
    terminatedPayload[i] = payload[i];
  }
  terminatedPayload[length] = 0;
  String payloadString = String(terminatedPayload);

  uint8_t id = getIdFromTopic(topic);

  if(topicString.endsWith(MQTT_SUFFIX_SETPOINT_SET))
  {
    uint16_t target = temperatureFromString(payloadString);
    wavinController.writeRegister(WavinController::CATEGORY_PACKED_DATA, id, WavinController::PACKED_DATA_MANUAL_TEMPERATURE, target);
  }
  else if(topicString.endsWith(MQTT_SUFFIX_MODE_SET))
  {
    if(payloadString == MQTT_VALUE_MODE_MANUAL) 
    {
      wavinController.writeMaskedRegister(
        WavinController::CATEGORY_PACKED_DATA,
        id,
        WavinController::PACKED_DATA_CONFIGURATION,
        WavinController::PACKED_DATA_CONFIGURATION_MODE_MANUAL,
        ~WavinController::PACKED_DATA_CONFIGURATION_MODE_MASK);
    }
    else if (payloadString == MQTT_VALUE_MODE_STANDBY)
    {
      wavinController.writeMaskedRegister(
        WavinController::CATEGORY_PACKED_DATA, 
        id, 
        WavinController::PACKED_DATA_CONFIGURATION, 
        WavinController::PACKED_DATA_CONFIGURATION_MODE_STANDBY, 
        ~WavinController::PACKED_DATA_CONFIGURATION_MODE_MASK);
    }
  }

  // Force re-read of registers from controller now
  lastUpdateTime = 0;

}


void resetLastSentValues()
{
  for(int8_t i=0; i<WavinController::NUMBER_OF_CHANNELS; i++)
  {
    lastSentValues[i].temperature = LAST_VALUE_UNKNOWN;
    lastSentValues[i].setpoint = LAST_VALUE_UNKNOWN;
    lastSentValues[i].battery = LAST_VALUE_UNKNOWN;
    lastSentValues[i].status = LAST_VALUE_UNKNOWN;
    lastSentValues[i].mode = LAST_VALUE_UNKNOWN;

    configurationPublished[i] = false;
  }
}


void publishIfNewValue(String topic, String payload, uint16_t newValue, uint16_t *lastSentValue)
{
  if (newValue != *lastSentValue)
  {
    if (mqttClient.publish(topic.c_str(), payload.c_str(), true))
    {
        *lastSentValue = newValue;
    }
    else
    {
      *lastSentValue = LAST_VALUE_UNKNOWN;
    }
  }
}


// Publish discovery messages for HomeAssistant
// See https://www.home-assistant.io/docs/mqtt/discovery/
void publishConfiguration(uint8_t channel)
{
  String climateTopic = String("homeassistant/climate/" + mqttDeviceNameWithMac + "/" + channel + "/config");
  String climateMessage = String(
    "{\"name\": \"" +mqttDeviceNameWithMac + "_" + channel +  "_climate\", "
    "\"current_temperature_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_CURRENT + "\", " 
    "\"temperature_command_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_SETPOINT_SET + "\", " 
    "\"temperature_state_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_SETPOINT_GET + "\", " 
    "\"mode_command_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_MODE_SET + "\", " 
    "\"mode_state_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_MODE_GET + "\", " 
    "\"modes\": [\"" + MQTT_VALUE_MODE_MANUAL + "\", \"" + MQTT_VALUE_MODE_STANDBY + "\"], " 
    "\"availability_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + MQTT_ONLINE +"\", "
    "\"payload_available\": \"True\", "
    "\"payload_not_available\": \"False\", "
    "\"qos\": \"0\"}"
  );
  
  String batteryTopic = String("homeassistant/sensor/" + mqttDeviceNameWithMac + "/" + channel + "/config");
  String batteryMessage = String(
    "{\"name\": \"" +mqttDeviceNameWithMac + "_" + channel +  "_battery\", "
    "\"state_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + "/battery\", " 
    "\"availability_topic\": \"" + MQTT_PREFIX + mqttDeviceNameWithMac + MQTT_ONLINE +"\", "
    "\"payload_available\": \"True\", "
    "\"payload_not_available\": \"False\", "
    "\"device_class\": \"battery\", "
    "\"unit_of_measurement\": \"%\", "
    "\"qos\": \"0\"}"
  );

  mqttClient.publish(climateTopic.c_str(), climateMessage.c_str(), true);  
  mqttClient.publish(batteryTopic.c_str(), batteryMessage.c_str(), true);
  
  configurationPublished[channel] = true;
}

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  shouldSaveConfig = true;
}

void setup()
{
WiFi.mode(WIFI_STA);
  //read configuration from FS json
  if (SPIFFS.begin()) {
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
#ifdef ARDUINOJSON_VERSION_MAJOR >= 6
        DynamicJsonDocument json(1024);
        auto deserializeError = deserializeJson(json, buf.get());
        serializeJson(json, Serial);
        if ( ! deserializeError ) {
#else
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
#endif
          strcpy(mqttserver, json["mqtt_server"]);
          strcpy(mqttusername, json["mqtt_user"]);
          strcpy(mqttpassword, json["mqtt_pass"]);
        }
      }
    }
  } 
  //end read
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqttserver, 40);
  WiFiManagerParameter custom_mqtt_user("user", "mqtt username", mqttusername, 20);
  WiFiManagerParameter custom_mqtt_pass("password", "mqtt password", mqttpassword, 20);

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
    //fetches ssid and pass and tries to connect
    //if it does not connect it starts an access point with the specified name
    //and goes into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect("MODBUS-MODULE")) {
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.restart();
      delay(5000);
    }

    //read updated parameters
    strcpy(mqttserver, custom_mqtt_server.getValue());
    strcpy(mqttusername, custom_mqtt_user.getValue());
    strcpy(mqttpassword, custom_mqtt_pass.getValue());
    
    //save the custom parameters to FS
    if (shouldSaveConfig) {
    #ifdef ARDUINOJSON_VERSION_MAJOR >= 6
      DynamicJsonDocument json(1024);
    #else
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.createObject();
    #endif
      json["mqtt_server"] = mqttserver;
      json["mqtt_user"] = mqttusername;
      json["mqtt_pass"] = mqttpassword;
    
      File configFile = SPIFFS.open("/config.json", "w");
    
    #ifdef ARDUINOJSON_VERSION_MAJOR >= 6
      serializeJson(json, Serial);
      serializeJson(json, configFile);
    #else
      json.printTo(Serial);
      json.printTo(configFile);
    #endif
      configFile.close();
      //end save
    }
  uint8_t mac[6];
  WiFi.macAddress(mac);

  char macStr[13] = {0};
  sprintf(macStr, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  mqttDeviceNameWithMac = String(MQTT_DEVICE_NAME + macStr);
  mqttClientWithMac = String(MQTT_CLIENT + macStr);

  char host[64];
  uint32_t chipID = ESP.getChipId();
  sprintf(chipid, "%08X", chipID);
  sprintf(host, HOST, chipid);
  delay(500);
  WiFi.hostname(host);
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
  });
  ArduinoOTA.onEnd([]() {
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  });
  ArduinoOTA.onError([](ota_error_t error) {
  });
  ArduinoOTA.begin();
  server.begin();
  
  mqttClient.setServer(mqttserver, 1883);
  mqttClient.setCallback(mqttCallback);

}


void loop()
{
  ArduinoOTA.handle();
  WiFiClient wifiClient = server.available();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!mqttClient.connected())
    {
      String will = String(MQTT_PREFIX + mqttDeviceNameWithMac + MQTT_ONLINE);
      if (mqttClient.connect(mqttClientWithMac.c_str(), mqttusername, mqttpassword, will.c_str(), 1, true, "False") )
      {
          String setpointSetTopic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/+" + MQTT_SUFFIX_SETPOINT_SET);
          mqttClient.subscribe(setpointSetTopic.c_str(), 1);
          
          String modeSetTopic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/+" + MQTT_SUFFIX_MODE_SET);
          mqttClient.subscribe(modeSetTopic.c_str(), 1);
          
          mqttClient.publish(will.c_str(), (const uint8_t *)"True", 4, true);

          // Forces resending of all parameters to server
          resetLastSentValues();
      }
      else
      {
          return;
      }
    }
  
    // Process incomming messages and maintain connection to the server
    if(!mqttClient.loop())
    {
        return;
    }

    if (lastUpdateTime + POLL_TIME_MS < millis())
    {
      lastUpdateTime = millis();

      uint16_t registers[11];

      for(uint8_t channel = 0; channel < WavinController::NUMBER_OF_CHANNELS; channel++)
      {
        if (wavinController.readRegisters(WavinController::CATEGORY_CHANNELS, channel, WavinController::CHANNELS_PRIMARY_ELEMENT, 1, registers))
        {
          uint16_t primaryElement = registers[0] & WavinController::CHANNELS_PRIMARY_ELEMENT_ELEMENT_MASK;
          bool allThermostatsLost = registers[0] & WavinController::CHANNELS_PRIMARY_ELEMENT_ALL_TP_LOST_MASK;

          if(primaryElement==0)
          {
              // Channel not used
              continue;
          }

          if(!configurationPublished[channel])
          {
            uint16_t standbyTemperature = STANDBY_TEMPERATURE_DEG * 10;
            wavinController.writeRegister(WavinController::CATEGORY_PACKED_DATA, channel, WavinController::PACKED_DATA_STANDBY_TEMPERATURE, standbyTemperature);
            publishConfiguration(channel);
          }

          // Read the current setpoint programmed for channel
          if (wavinController.readRegisters(WavinController::CATEGORY_PACKED_DATA, channel, WavinController::PACKED_DATA_MANUAL_TEMPERATURE, 1, registers))
          {
            uint16_t setpoint = registers[0];

            String topic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_SETPOINT_GET);
            String payload = temperatureAsFloatString(setpoint);

            publishIfNewValue(topic, payload, setpoint, &(lastSentValues[channel].setpoint));
          }

          // Read the current mode for the channel
          if (wavinController.readRegisters(WavinController::CATEGORY_PACKED_DATA, channel, WavinController::PACKED_DATA_CONFIGURATION, 1, registers))
          {
            uint16_t mode = registers[0] & WavinController::PACKED_DATA_CONFIGURATION_MODE_MASK; 

            String topic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_MODE_GET);
            if(mode == WavinController::PACKED_DATA_CONFIGURATION_MODE_STANDBY)
            {
              publishIfNewValue(topic, MQTT_VALUE_MODE_STANDBY, mode, &(lastSentValues[channel].mode));
            }
            else if(mode == WavinController::PACKED_DATA_CONFIGURATION_MODE_MANUAL)
            {
              publishIfNewValue(topic, MQTT_VALUE_MODE_MANUAL, mode, &(lastSentValues[channel].mode));
            }            
          }

          // Read the current status of the output for channel
          if (wavinController.readRegisters(WavinController::CATEGORY_CHANNELS, channel, WavinController::CHANNELS_TIMER_EVENT, 1, registers))
          {
            uint16_t status = registers[0] & WavinController::CHANNELS_TIMER_EVENT_OUTP_ON_MASK;

            String topic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_OUTPUT);
            String payload;
            if (status & WavinController::CHANNELS_TIMER_EVENT_OUTP_ON_MASK)
              payload = "on";
            else
              payload = "off";

            publishIfNewValue(topic, payload, status, &(lastSentValues[channel].status));
          }

          // If a thermostat for the channel is connected to the controller
          if(!allThermostatsLost)
          {
            // Read values from the primary thermostat connected to this channel 
            // Primary element from controller is returned as index+1, so 1 i subtracted here to read the correct element
            if (wavinController.readRegisters(WavinController::CATEGORY_ELEMENTS, primaryElement-1, 0, 11, registers))
            {
              uint16_t temperature = registers[WavinController::ELEMENTS_AIR_TEMPERATURE];
              uint16_t battery = registers[WavinController::ELEMENTS_BATTERY_STATUS]; // In 10% steps

              String topic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_CURRENT);
              String payload = temperatureAsFloatString(temperature);

              publishIfNewValue(topic, payload, temperature, &(lastSentValues[channel].temperature));

              topic = String(MQTT_PREFIX + mqttDeviceNameWithMac + "/" + channel + MQTT_SUFFIX_BATTERY);
              payload = String(battery*10);

              publishIfNewValue(topic, payload, battery, &(lastSentValues[channel].battery));
            }
          }         
        }

        // Process incomming messages and maintain connection to the server
        if(!mqttClient.loop())
        {
            return;
        }
      }
    }
  }
}
