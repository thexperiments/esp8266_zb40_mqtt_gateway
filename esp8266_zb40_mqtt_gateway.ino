#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#include <PubSubClient.h>

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40];
char mqtt_port[6] = "1883";
char mqtt_user[20] = "";
char mqtt_password[20] = "";
char mqtt_topic[50] = "/ZB40_GATEWAY";


const char* device_name = "ZB40_GATEWAY";

const char* config_ssid = "ZB40_Gateway";
const char* config_password = "roottoor";

//flag for saving data
bool shouldSaveConfig = false;

const int RESET_PIN = 14; //D5 on nodeMCU

//ZB40 config
// pinmapping HCS361 keeloq chip<->ESP node mcu (0,1,2,3)
const int HCS361_PINS[] = {16,5,4,15};
const int CMD_UP = 1;
const int CMD_DOWN = 2;
const int CMD_STOP = 3;
const int SHUTTER_ALL = 0;
const int SHUTTER_1 = 1;
const int SHUTTER_2 = 2;
const int SHUTTER_3 = 3;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  // put your setup code here, to run once:

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  //needs to be in the beginning for accessing settings reset on reset pin
  WiFiManager wifiManager;

  
  Serial.begin(115200);
  Serial.println();
  //setup gpios
  Serial.println("GPIO setup...");
  for (int i = 0; i < 4; i++){
    int current_pin = HCS361_PINS[i];
    digitalWrite(current_pin, LOW);
    pinMode(current_pin, OUTPUT);
  }

  pinMode(RESET_PIN, INPUT_PULLUP);

  //if reset is triggered
  if(digitalRead(RESET_PIN) == LOW){
    Serial.println("RESET triggered...");
    //we format the memory
    SPIFFS.format();
    //and reset the ESP settings
    wifiManager.resetSettings();
  }
  
  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_user, json["mqtt_user"]);
          strcpy(mqtt_password, json["mqtt_password"]);
          strcpy(mqtt_topic, json["mqtt_topic"]);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "something.com", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "1883", mqtt_port, 5);
  WiFiManagerParameter custom_mqtt_user("user", "user", mqtt_user, 20);
  WiFiManagerParameter custom_mqtt_password("password", "password", mqtt_password, 20);
  WiFiManagerParameter custom_mqtt_topic("topic", "/your/topic", mqtt_topic, 50);

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set static ip
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

  //add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_password);
  wifiManager.addParameter(&custom_mqtt_topic);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect(config_ssid,config_password)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }
  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_password, custom_mqtt_password.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_password"] = mqtt_password;
    json["mqtt_topic"] = mqtt_topic;
    
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  //make sure softap is off
  WiFi.softAPdisconnect(true);


  Serial.println("local ip");
  Serial.println(WiFi.localIP());

  //mqtt setup
  client.setServer(mqtt_server, atoi(mqtt_port));
  client.setCallback(mqtt_callback);
  
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  bool cmd_valid = true;
  String topic_string = String(topic);
  String message_string = "";
  
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
    //fill up the message string
    message_string.concat((char)payload[i]);
  }
  Serial.println();

  //we need to find out which command to send

  //find the topic slashes, ignore initial slash
  int shutter_slash = topic_string.indexOf("/",1);
  //cut the shutter string
  String shutter_string = topic_string.substring(shutter_slash + 1,topic_string.length() - 1);
  Serial.println(shutter_string.c_str());
  // map shutter strings
  int shutter_index = 0;
  if (shutter_string.equalsIgnoreCase("all")){
    shutter_index = SHUTTER_ALL;
  }
  else if(shutter_string.equalsIgnoreCase("1") || shutter_string.equalsIgnoreCase("2") || shutter_string.equalsIgnoreCase("3")){
    //bit simpler than more else ifs as SUTTER_1 == 1 ...
    shutter_index = shutter_string.toInt();
  }
  else {
    Serial.print("Received message on unhandled topic: ");
    Serial.println(topic);
    cmd_valid = false;
  }
  //map commands
  int shutter_cmd = 0;
  if (message_string.equalsIgnoreCase("up")){
    shutter_cmd = CMD_UP;
  }
  else if(message_string.equalsIgnoreCase("down")){
    shutter_cmd = CMD_DOWN;
  }
  else if(message_string.equalsIgnoreCase("stop")){
    shutter_cmd = CMD_STOP;
  }
  else {
    Serial.print("Received illegal command message: ");
    Serial.println(message_string.c_str());
    cmd_valid = false;
  }

  if (cmd_valid){
    send_ZB40_command(shutter_index, shutter_cmd);
  }
}

bool mqtt_connect() {
  bool mqtt_connected = false;
  if (mqtt_password == ""){
    //we dont have a password/user set so we connect without
    Serial.print("connecting without password...");
    mqtt_connected = client.connect(device_name);
  }
  else{
    //we have a user and password set
    Serial.print("connecting with username/password...");
    mqtt_connected = client.connect(device_name, mqtt_user, mqtt_password);
  }

  return(mqtt_connected);
}

void mqtt_reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (mqtt_connect()) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      client.publish(mqtt_topic, "ZB40_GATEWAY online");
      // ... and resubscribe
      char generated_topic[60];
      sprintf(generated_topic, "%s/#", mqtt_topic);
      Serial.print("subscribing to: ");
      Serial.println(generated_topic);
      client.subscribe(generated_topic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

//ZB40 functions
void send_ZB40_command(int shutter, int command){
  int HCS361_bits[4];
  //shutter is encoded in the bit 3 and 2
  HCS361_bits[3] = shutter >> 1 & 0x01;
  HCS361_bits[2] = shutter & 0x01;
  //command is encoded in bit 1 and 0
  HCS361_bits[1] = command >> 1 & 0x01;
  HCS361_bits[0] = command & 0x01;

  Serial.print("Sending... [");
  //set the outputs accordingly
  for (int i = 0; i < 4; i++){
    digitalWrite(HCS361_PINS[i], HCS361_bits[i]);
    Serial.print(HCS361_bits[i]);
  }
  Serial.println("]");
  //wait 500ms 
  delay(500);
  //and set them back to 0
  for (int i = 0; i < 4; i++){
    digitalWrite(HCS361_PINS[i], LOW);
  }
}

void loop() {
  if (!client.connected()) {
    mqtt_reconnect();
  }
  client.loop();

}
