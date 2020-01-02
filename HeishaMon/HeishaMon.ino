#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <DNSServer.h>

#include <ArduinoJson.h>

#include "commands.h"
#include "webfunctions.h"


// maximum number of seconds between resets that
// counts as a double reset
#define DRD_TIMEOUT 0.1

// address to the block in the RTC user memory
// change it if it collides with another usage
// of the address block
#define DRD_ADDRESS 0x00

#define WAITTIME 5000
#define UPDATEALLTIME 300000 // how often all data is cleared and so resend to mqtt

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;
const char* update_path = "/firmware";
const char* update_username = "admin";
const char* update_password = "heisha";

// Default settings if config does not exists
char* wifi_hostname = "HeishaMon";
char* ota_password  = "panasonic";
char mqtt_server[40];
char mqtt_port[6] = "1883";
char mqtt_username[40];
char mqtt_password[40];

bool sending = false;
unsigned long nexttime = 0;
unsigned long nextalldatatime = 0;

byte inCheck = 0;


//useful for debugging, outputs info to a separate mqtt topic
const bool outputMqttLog = true;

//retain mqtt values for subscriber to receive on first connect
const bool MQTT_RETAIN_VALUES = true;


// instead of passing array pointers between functions we just define this in the global scope
#define MAXDATASIZE 256
char data[MAXDATASIZE];
int data_length = 0;

// store actual data in a json doc
DynamicJsonDocument actData(1024);

// log message to sprintf to
char log_msg[256];

// mqtt topic to sprintf and then publish to
char mqtt_topic[256];


//doule reset detection
DoubleResetDetect drd(DRD_TIMEOUT, DRD_ADDRESS);

// mqtt
WiFiClient mqtt_wifi_client;
PubSubClient mqtt_client(mqtt_wifi_client);


void mqtt_reconnect()
{
  Serial1.println("Reconnecting to mqtt server ...");
  if (mqtt_client.connect(wifi_hostname, mqtt_username, mqtt_password, mqtt_willtopic, 1, true, "Offline"))
  {
    mqtt_client.subscribe(mqtt_set_quiet_mode_topic);
    mqtt_client.subscribe(mqtt_set_shift_temperature_topic);
    mqtt_client.subscribe(mqtt_set_mode_topic);
    mqtt_client.subscribe(mqtt_set_force_DHW_topic);
    mqtt_client.subscribe(mqtt_set_force_defrost_topic);
    mqtt_client.subscribe(mqtt_set_force_sterilization_topic);
    mqtt_client.subscribe(mqtt_set_holiday_topic);
    mqtt_client.subscribe(mqtt_set_powerfull_topic);
    mqtt_client.subscribe(mqtt_set_tank_temp_topic);
    mqtt_client.subscribe(mqtt_set_cool_temp_topic);

    mqtt_client.publish(mqtt_willtopic, "Online");
  }
}
void log_message(char* string)
{
  Serial1.println(string);
  if (outputMqttLog)
  {
    mqtt_client.publish(mqtt_logtopic, string);
  }
}

void logHex(char *hex, int hex_len) {
  char buffer [48];
  buffer[47] = 0;
  for (int i = 0; i < hex_len; i += 16) {
    for (int j = 0; ((j < 16) && ((i + j) < hex_len)); j++) {
      sprintf(&buffer[3 * j], "%02X ", hex[i + j]);
    }
    sprintf(log_msg, "data: %s", buffer ); log_message(log_msg);
  }

}

byte calcChecksum(byte* command, int length) {
  byte chk = 0;
  for ( int i = 0; i < length; i++)  {
    chk += command[i];
  }
  chk = (chk ^ 0xFF) + 01;
  return chk;
}

bool readSerial()
{
  if (Serial.available() > 0) {
    data_length = Serial.readBytes(data, 203);
    while (Serial.available()) {
      delay(2);
      Serial.read();
    }
    
   sprintf(log_msg, "received size : %d", data_length); log_message(log_msg);
   logHex(data, data_length);

    byte chk = 0;
    for ( int i = 0; i < sizeof(data); i++)  {
      chk += data[i];
    }
    if ( chk == 0 ) {
      log_message("Checksum received ok!");
      return true;
    }
    else {
      log_message("Checksum received false!");
      return false;
    }
  }
  else data_length = 0;
}


bool send_command(byte* command, int length)
{
  if ( sending ) {
    log_message("Already sending data. Aborting this send request");
    return false;
  }
  sending = true;

  byte chk = calcChecksum(command, length);
  int bytesSent = Serial.write(command, length);
  bytesSent += Serial.write(chk);
  
  sprintf(log_msg, "sent bytes: %d with checksum: %d ", bytesSent, int(chk)); log_message(log_msg);
  logHex((char*)command, length);
  
  // wait until the serial buffer is filled with the replies
  delay(1000);

  // read the serial
  bool result = readSerial();
  sending = false;
  
  if ( result ) decode_heatpump_data();
  return result;
}

// Callback function that is called when a message has been pushed to one of your topics.
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  char msg[length + 1];
  for (int i = 0; i < length; i++) {
    msg[i] = (char)payload[i];
  }
  msg[length] = '\0';
  // set 0 for Off mode, set 1 for Quiet mode 1, set 2 for Quiet mode 2, set 3 for Quiet mode 3
  if (strcmp(topic, mqtt_set_quiet_mode_topic) == 0)
  {
    String set_quiet_mode_string(msg);
    int quiet_mode = (set_quiet_mode_string.toInt() + 1) * 8;

    sprintf(log_msg, "set Quiet mode to %d", quiet_mode / 8 - 1); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, quiet_mode, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }
  // set from -5 to 5 to get same temperature shift point
  if (strcmp(topic, mqtt_set_shift_temperature_topic) == 0)
  {
    String set_shift_temperature_string(msg);
    int shift_mode = set_shift_temperature_string.toInt() + 128;


    sprintf(log_msg, "set shift temperature to %d", shift_mode - 128 ); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, shift_mode, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set mode to force DHW by sending 1
  if (strcmp(topic, mqtt_set_force_DHW_topic) == 0)
  {
    String set_force_DHW_string(msg);
    int force_DHW_mode = 66; //hex 0x42
    if ( set_force_DHW_string.toInt() == 1 ) {
      force_DHW_mode = 130; //hex 0x82
    }
    sprintf(log_msg, "set force mode to %d", force_DHW_mode); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, force_DHW_mode, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set mode to force defrost  by sending 1
  if (strcmp(topic, mqtt_set_force_defrost_topic) == 0)
  {
    String set_force_defrost_string(msg);
    int force_defrost_mode = 0;
    if ( set_force_defrost_string.toInt() == 1 ) {
      force_defrost_mode = 2; //hex 0x02
    }
    sprintf(log_msg, "set force defrost mode to %d", force_defrost_mode); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, force_defrost_mode, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set mode to force sterilization by sending 1
  if (strcmp(topic, mqtt_set_force_sterilization_topic) == 0)
  {
    String set_force_sterilization_string(msg);
    int force_sterilization_mode = 0;
    if ( set_force_sterilization_string.toInt() == 1 ) {
      force_sterilization_mode = 4; //hex 0x04
    }
    sprintf(log_msg, "set force sterilization mode to %d", force_sterilization_mode); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, force_sterilization_mode, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }


  // set Holiday mode by sending 100, off will be 84
  if (strcmp(topic, mqtt_set_holiday_topic) == 0)
  {
    String set_holiday_string(msg);
    int set_holiday = set_holiday_string.toInt();

    sprintf(log_msg, "set holiday mode to %d", set_holiday); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, set_holiday, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set Powerfull mode by sending 0 = off, 30 for 30min, 60 for 60min, 90 for 90 min
  if (strcmp(topic, mqtt_set_powerfull_topic) == 0)
  {
    String set_powerfull_string(msg);
    int set_powerfull = (set_powerfull_string.toInt() / 30) + 73;


    sprintf(log_msg, "set powerfull mode to %d", (set_powerfull - 73) * 30); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, set_powerfull, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set Heat pump mode  33 = tank only, 82 = heat only, 83 = cool only, 88 = Auto, 98 = Heat+DHW, 99 = Cool+DHW, 104 = Auto + DHW
  if (strcmp(topic, mqtt_set_mode_topic) == 0)
  {
    String set_mode_string(msg);
    int set_mode = set_mode_string.toInt();


    sprintf(log_msg, "set heat pump mode to %d", set_mode); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x02, 0x00, set_mode, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set Tank temperature by sending desired temperature between 40C-75C
  if (strcmp(topic, mqtt_set_tank_temp_topic) == 0)
  {
    String set_tank_temp_string(msg);
    int set_tank_temp = set_tank_temp_string.toInt() + 128;

    sprintf(log_msg, "set Tank temperature to %d", set_tank_temp - 128); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, set_tank_temp, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  // set Cool temperature by sending desired temperature between 5C-20C (if selected Direct mode)
  if (strcmp(topic, mqtt_set_cool_temp_topic) == 0)
  {
    String set_cool_temp_string(msg);
    int set_cool_temp = set_cool_temp_string.toInt() + 128;


    sprintf(log_msg, "set Cool temperature to %d", set_cool_temp - 128); log_message(log_msg);
    byte command[] = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, set_cool_temp, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send_command(command, sizeof(command));
  }

  if (strcmp(topic, mqtt_topic_base) == 0)
  {
    log_message("Updating..");

    send_panasonic_query();
  }
}

void send_panasonic_query()
{
  log_message("Requesting new panasonic data...");
  if ( ! send_command(panasonicQuery, sizeof(panasonicQuery)) ) {
    log_message("Failed to get panasonic data!");
  }
}

void decode_heatpump_data() {
  if (millis() > nextalldatatime) {
    actData.clear(); // clearing all actual data so everything will be updated and sent to mqtt
    nextalldatatime = millis() + UPDATEALLTIME;
  }

  int Power_state = (int)(data[4]);
  char* Power_state_string;
  switch (Power_state & 0b11) { //probably only last two bits for Power dhw state
    case 0b01:
      Power_state_string = "off";
      break;
    case 0b10:
      Power_state_string = "on";
      break;
    default:
      Power_state_string = "Unknown";
      break;
  }

  if ( actData["Power_state"] != Power_state_string ) {
    actData["Power_state"] = Power_state_string;
    sprintf(log_msg, "received Power DHW state : %d (%s)", Power_state, Power_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Power"); mqtt_client.publish(mqtt_topic, Power_state_string, MQTT_RETAIN_VALUES);
  }

  int Mode_state = (int)(data[6]);
  char* Mode_state_string;
  switch (Mode_state) {
    case 98:
      Mode_state_string = "Heat+DHW";
      break;
    case 82:
      Mode_state_string = "Heat";
      break;
    case 97:
      Mode_state_string = "DHW";
      break;
    case 105:
      Mode_state_string = "Auto+DHW";
      break;
    case 99:
      Mode_state_string = "Cool+DHW";
      break;
    case 83:
      Mode_state_string = "Cool";
      break;
    case 89:
      Mode_state_string = "Auto";
      break;
    default:
      Mode_state_string = "Unknown";
      break;
  }

  if ( actData["Mode_state"] != Mode_state_string ) {
    actData["Mode_state"] = Mode_state_string;
    sprintf(log_msg, "received heat pump mode state : %d (%s)", Mode_state, Mode_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Mode_state"); mqtt_client.publish(mqtt_topic, Mode_state_string, MQTT_RETAIN_VALUES);
  }


  int quietpower_mode_state = (int)(data[7]);
  char* Powerfull_mode_state_string = "Unknown";
  char* Quiet_mode_state_string = "Unknown";
  switch (quietpower_mode_state & 0b11111000) { // only interested in left most 5 bits for quiet state
    case 0b10001000:
      Quiet_mode_state_string = "scheduled";
      break;
    case 0b01001000:
      Quiet_mode_state_string = "0ff";
      break;
    case 0b01010000:
      Quiet_mode_state_string = "1";
      break;
    case 0b01011000:
      Quiet_mode_state_string = "2";
      break;
    case 0b01100000:
      Quiet_mode_state_string = "3";
      break;
    default:
      break;
  }
  switch (quietpower_mode_state & 0b111) { // only interested in last 3 bits for powerfull state
    case 0b001:
      Powerfull_mode_state_string = "off";
      break;
    case 0b010:
      Powerfull_mode_state_string = "30m";
      break;
    case 0b011:
      Powerfull_mode_state_string = "60m";
      break;
    case 0b100:
      Powerfull_mode_state_string = "90m";
      break;
    default:
      break;
  }
  if ( actData["Quiet_mode_state"] != Quiet_mode_state_string ) {
    actData["Quiet_mode_state"] = Quiet_mode_state_string;
    sprintf(log_msg, "received quiet mode state : %d (%s)", quietpower_mode_state, Quiet_mode_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Quiet_mode_state"); mqtt_client.publish(mqtt_topic, Quiet_mode_state_string, MQTT_RETAIN_VALUES);
  }
  if ( actData["Powerfull_mode_state"] != Powerfull_mode_state_string ) {
    actData["Powerfull_mode_state"] = Powerfull_mode_state_string;
    sprintf(log_msg, "received powerfull mode state : %d (%s)", quietpower_mode_state, Powerfull_mode_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Powerfull_mode_state"); mqtt_client.publish(mqtt_topic, Powerfull_mode_state_string, MQTT_RETAIN_VALUES);
  }

  int valve_defrost_state = (int)(data[111]);
  char* Valve_state_string;
  switch (valve_defrost_state & 0b11) { //bitwise AND with 0b11 because we are only interested in last 2 bits of the byte.
    case 0b01:
      Valve_state_string = "Room";
      break;
    case 0b10:
      Valve_state_string = "Tank";
      break;
    default:
      Valve_state_string = "Unknown";
      break;
  }

  if ( actData["Valve_state"] != Valve_state_string ) {
    actData["Valve_state"] = Valve_state_string;
    sprintf(log_msg, "received 3-way valve state : %d (%s)", valve_defrost_state, Valve_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Valve_state"); mqtt_client.publish(mqtt_topic, Valve_state_string, MQTT_RETAIN_VALUES);
  }

  char* Defrosting_state_string;
  switch (valve_defrost_state & 0b1100) { //bitwise AND with 0b1100 because we are only interested in these two bits
    case 0b0100:
      Defrosting_state_string = "Not active";
      break;
    case 0b1000:
      Defrosting_state_string = "Active";
      break;
    default:
      Defrosting_state_string = "Unknown";
      break;
  }

  if ( actData["Defrosting_state"] != Defrosting_state_string ) {
    actData["Defrosting_state"] = Defrosting_state_string;
    sprintf(log_msg, "received defrosting state : %d (%s)", valve_defrost_state, Defrosting_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Defrosting_state"); mqtt_client.publish(mqtt_topic, Defrosting_state_string, MQTT_RETAIN_VALUES);
  }

  float WatOutTarTemp = (float)data[153] - 128;
  if ( actData["WatOutTarTemp"] != WatOutTarTemp ) {
    actData["WatOutTarTemp"] = WatOutTarTemp;
    sprintf(log_msg, "received temperature (WatOutTarTemp): %.2f", WatOutTarTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "WatOutTarTemp"); mqtt_client.publish(mqtt_topic, String(WatOutTarTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float ActWatOutTemp = (float)data[139] - 128;
  if ( actData["ActWatOutTemp"] != ActWatOutTemp ) {
    actData["ActWatOutTemp"] = ActWatOutTemp;
    sprintf(log_msg, "received temperature (ActWatOutTemp): %.2f", ActWatOutTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "ActWatOutTemp"); mqtt_client.publish(mqtt_topic, String(ActWatOutTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float InletTemp = (float)data[143] - 128;
  if ( actData["InletTemp"] != InletTemp ) {
    actData["InletTemp"] = InletTemp;
    sprintf(log_msg, "received temperature (InletTemp): %.2f", InletTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "InletTemp"); mqtt_client.publish(mqtt_topic, String(InletTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float TankSetTemp = (float)data[42] - 128;
  if ( actData["TankSetTemp"] != TankSetTemp ) {
    actData["TankSetTemp"] = TankSetTemp;
    sprintf(log_msg, "received temperature (TankSetTemp): %.2f", TankSetTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "TankSetTemp"); mqtt_client.publish(mqtt_topic, String(TankSetTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float ActTankTemp = (float)data[141] - 128;
  if ( actData["ActTankTemp"] != ActTankTemp ) {
    actData["ActTankTemp"] = ActTankTemp;
    sprintf(log_msg, "received temperature (ActTankTemp): %.2f", ActTankTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "ActTankTemp"); mqtt_client.publish(mqtt_topic, String(ActTankTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float ActOutTemp = (float)data[142] - 128;
  if ( actData["ActOutTemp"] != ActOutTemp ) {
    actData["ActOutTemp"] = ActOutTemp;
    sprintf(log_msg, "received temperature (ActOutTemp): %.2f", ActOutTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "ActOutTemp"); mqtt_client.publish(mqtt_topic, String(ActOutTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float RoomTherTemp = (float)data[156] - 128;
  if ( actData["RoomTherTemp"] != RoomTherTemp ) {
    actData["RoomTherTemp"] = RoomTherTemp;
    sprintf(log_msg, "received temperature (RoomTherTemp): %.2f", RoomTherTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "RoomTherTemp"); mqtt_client.publish(mqtt_topic, String(RoomTherTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float OutPipeTemp = (float)data[158] - 128;
  if ( actData["OutPipeTemp"] != OutPipeTemp ) {
    actData["OutPipeTemp"] = OutPipeTemp;
    sprintf(log_msg, "received temperature (OutPipeTemp): %.2f", OutPipeTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "OutPipeTemp"); mqtt_client.publish(mqtt_topic, String(OutPipeTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  int PumpFlow1 = (int)data[170];
  float PumpFlow2 = ((((float)data[169] - 1) / 5) * 2) / 100;
  float PumpFlow = PumpFlow1 + PumpFlow2;
  if ( actData["PumpFlow"] != PumpFlow ) {
    actData["PumpFlow"] = PumpFlow;
    sprintf(log_msg, "received pump flow (PumpFlow): %.2f", PumpFlow); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "PumpFlow"); mqtt_client.publish(mqtt_topic, String(PumpFlow).c_str(), MQTT_RETAIN_VALUES);
  }

  float CompFreq = (float)data[166] - 1;
  if ( actData["CompFreq"] != CompFreq ) {
    actData["CompFreq"] = CompFreq;
    sprintf(log_msg, "received compressor frequency (CompFreq): %.2f", CompFreq); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "CompFreq"); mqtt_client.publish(mqtt_topic, String(CompFreq).c_str(), MQTT_RETAIN_VALUES);
  }

  float HeatShiftTemp = (float)data[38] - 128;
  if ( actData["HeatShiftTemp"] != HeatShiftTemp ) {
    actData["HeatShiftTemp"] = HeatShiftTemp;
    sprintf(log_msg, "received temperature (HeatShiftTemp): %.2f", HeatShiftTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "HeatShiftTemp"); mqtt_client.publish(mqtt_topic, String(HeatShiftTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float CoolShiftTemp = (float)data[39] - 128;
  if ( actData["CoolShiftTemp"] != CoolShiftTemp ) {
    actData["CoolShiftTemp"] = CoolShiftTemp;
    sprintf(log_msg, "received temperature (CoolShiftTemp): %.2f", CoolShiftTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "CoolShiftTemp"); mqtt_client.publish(mqtt_topic, String(CoolShiftTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float HCurveOutHighTemp = (float)data[75] - 128;
  if ( actData["HCurveOutHighTemp"] != HCurveOutHighTemp ) {
    actData["HCurveOutHighTemp"] = HCurveOutHighTemp;
    sprintf(log_msg, "received temperature (HCurveOutHighTemp): %.2f", HCurveOutHighTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "HCurveOutHighTemp"); mqtt_client.publish(mqtt_topic, String(HCurveOutHighTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float HCurveOutLowTemp = (float)data[76] - 128;
  if ( actData["HCurveOutLowTemp"] != HCurveOutLowTemp ) {
    actData["HCurveOutLowTemp"] = HCurveOutLowTemp;
    sprintf(log_msg, "received temperature (HCurveOutLowTemp): %.2f", HCurveOutLowTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "HCurveOutLowTemp"); mqtt_client.publish(mqtt_topic, String(HCurveOutLowTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float HCurveOutsLowTemp = (float)data[77] - 128;
  if ( actData["HCurveOutsLowTemp"] != HCurveOutsLowTemp ) {
    actData["HCurveOutsLowTemp"] = HCurveOutsLowTemp;
    sprintf(log_msg, "received temperature (HCurveOutsLowTemp): %.2f", HCurveOutsLowTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "HCurveOutsLowTemp"); mqtt_client.publish(mqtt_topic, String(HCurveOutsLowTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  float HCurveOutsHighTemp = (float)data[78] - 128;
  if ( actData["HCurveOutsHighTemp"] != HCurveOutsHighTemp ) {
    actData["HCurveOutsHighTemp"] = HCurveOutsHighTemp;
    sprintf(log_msg, "received temperature (HCurveOutsHighTemp): %.2f", HCurveOutsHighTemp); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "HCurveOutsHighTemp"); mqtt_client.publish(mqtt_topic, String(HCurveOutsHighTemp).c_str(), MQTT_RETAIN_VALUES);
  }

  int ForceDHW_state = (int)(data[4]);
  char* ForceDHW_state_string;
  switch (ForceDHW_state & 0b11000000) { //probably only first two bits for force dhw state
    case 0b01000000:
      ForceDHW_state_string = "off";
      break;
    case 0b10000000:
      ForceDHW_state_string = "on";
      break;
    default:
      ForceDHW_state_string = "Unknown";
      break;
  }

  if ( actData["ForceDHW_state"] != ForceDHW_state_string ) {
    actData["ForceDHW_state"] = ForceDHW_state_string;
    sprintf(log_msg, "received force DHW state : %d (%s)", ForceDHW_state, ForceDHW_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "ForceDHW"); mqtt_client.publish(mqtt_topic, ForceDHW_state_string, MQTT_RETAIN_VALUES);
  }

  int Holiday_mode_state = (int)(data[5]);
  char* Holiday_mode_state_string;
  switch (Holiday_mode_state & 0b00110000) { //probably only these two bits determine holiday state
    case 0b00010000:
      Holiday_mode_state_string = "off";
      break;
    case 0b00100000:
      Holiday_mode_state_string = "on";
      break;
    default:
      Holiday_mode_state_string = "Unknown";
      break;
  }

  if ( actData["Holiday_mode_state"] != Holiday_mode_state_string ) {
    actData["Holiday_mode_state"] = Holiday_mode_state_string;
    sprintf(log_msg, "received Holiday state : %d (%s)", Holiday_mode_state, Holiday_mode_state_string); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Holiday"); mqtt_client.publish(mqtt_topic, Holiday_mode_state_string, MQTT_RETAIN_VALUES);
  }

  float FloorHeatDelta = (float)data[84] - 128;
  if ( actData["FloorHeatDelta"] != FloorHeatDelta ) {
    actData["FloorHeatDelta"] = FloorHeatDelta;
    sprintf(log_msg, "received temperature (FloorHeatDelta): %.2f", FloorHeatDelta); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "FloorHeatDelta"); mqtt_client.publish(mqtt_topic, String(FloorHeatDelta).c_str(), MQTT_RETAIN_VALUES);
  }

  float FloorCoolDelta = (float)data[94] - 128;
  if ( actData["FloorCoolDelta"] != FloorCoolDelta ) {
    actData["FloorCoolDelta"] = FloorCoolDelta;
    sprintf(log_msg, "received temperature (FloorCoolDelta): %.2f", FloorCoolDelta); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "FloorCoolDelta"); mqtt_client.publish(mqtt_topic, String(FloorCoolDelta).c_str(), MQTT_RETAIN_VALUES);
  }

  float TankHeatDelta = (float)data[99] - 128;
  if ( actData["TankHeatDelta"] != TankHeatDelta ) {
    actData["TankHeatDelta"] = TankHeatDelta;
    sprintf(log_msg, "received temperature (TankHeatDelta): %.2f", TankHeatDelta); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "TankHeatDelta"); mqtt_client.publish(mqtt_topic, String(TankHeatDelta).c_str(), MQTT_RETAIN_VALUES);
  }

  float Econsum = ((float)data[193] - 1.0) * 200;
  if ( actData["Econsum"] != Econsum ) {
    actData["Econsum"] = Econsum;
    sprintf(log_msg, "received Watt (Elec consume): %.2f", Econsum); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Econsum"); mqtt_client.publish(mqtt_topic, String(Econsum).c_str(), MQTT_RETAIN_VALUES);
  }

  float Eproduce = ((float)data[194] - 1.0) * 200;
  if ( actData["Eproduce"] != Eproduce ) {
    actData["Eproduce"] = Eproduce;
    sprintf(log_msg, "received Watt (Elec produced): %.2f", Eproduce); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "Eproduce"); mqtt_client.publish(mqtt_topic, String(Eproduce).c_str(), MQTT_RETAIN_VALUES);
  }

  int OperatingTime  = word(data[183], data[182]) - 1;
  if ( actData["OperatingTime"] != OperatingTime ) {
    actData["OperatingTime"] = OperatingTime;
    sprintf(log_msg, "received (OperatingTime): %.2f", OperatingTime); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "OperatingTime"); mqtt_client.publish(mqtt_topic, String(OperatingTime).c_str(), MQTT_RETAIN_VALUES);
  }

  int OperationsNumber  = word(data[180], data[179]) - 1;
  if ( actData["OperationsNumber"] != OperationsNumber ) {
    actData["OperationsNumber"] = OperationsNumber;
    sprintf(log_msg, "received (OperationsNumber): %.2f", OperationsNumber); log_message(log_msg);
    sprintf(mqtt_topic, "%s/%s", mqtt_topic_base, "OperationsNumber"); mqtt_client.publish(mqtt_topic, String(OperationsNumber).c_str(), MQTT_RETAIN_VALUES);
  }

}



void setupOTA() {
  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(wifi_hostname);

  // Set authentication
  ArduinoOTA.setPassword(ota_password);

  ArduinoOTA.onStart([]() {
  });
  ArduinoOTA.onEnd([]() {
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {

  });
  ArduinoOTA.onError([](ota_error_t error) {

  });
  ArduinoOTA.begin();
}

void setupHttp() {
  httpUpdater.setup(&httpServer, update_path, update_username, update_password);
  httpServer.on("/", [] {
    handleRoot(&httpServer, &actData);
  });
  httpServer.on("/factoryreset", [] {
    handleFactoryReset(&httpServer);
  });
  httpServer.on("/reboot", [] {
    handleReboot(&httpServer);
  });

  httpServer.begin();
}

void setupSerial() {
  //debug line on serial1 (D4, GPIO2)
  Serial1.begin(115200);

  //serial to cn-cnt
  Serial.begin(9600, SERIAL_8E1);
  Serial.flush();
  //swap to gpio13 (D7) and gpio15 (D8)
  Serial.swap();

  //enable gpio15 after boot using gpio5 (D1)
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);
}

void setupMqtt() {
  mqtt_client.setServer(mqtt_server, atoi(mqtt_port));
  mqtt_client.setCallback(mqtt_callback);
}

void setup() {
  setupSerial();
  setupWifi(drd, wifi_hostname, ota_password, mqtt_server, mqtt_port, mqtt_username, mqtt_password);
  setupOTA();
  setupMqtt();
  setupHttp();
  //switchSerial();
}

void loop() {
  // Handle OTA first.
  ArduinoOTA.handle();
  // then handle HTTP
  httpServer.handleClient();

  if (!mqtt_client.connected())
  {
    mqtt_reconnect();
  }
  mqtt_client.loop();

  // run the main program only each WAITTIME
  if (millis() > nexttime) {
    nexttime = millis() + WAITTIME;
    send_panasonic_query();
  }
  yield();
}