#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#define sonoffRelayPin 12
#define sonoffGPIOPin 14
#define sonoffLEDPin 13
#define sonoffButtonPin 0

WiFiClient espClient;
PubSubClient client(espClient);

//////////////////////////////////////
//// DEVICE SPECIFIC CONFIG START ////
//////////////////////////////////////

// Send 0 or 1 to this topic to control the 
char* deviceControlTopic = "light/lobby";

// The Sonoff will publish its state (0 or 1) on this topic.
char* deviceStateTopic = "light/lobby/state"; 

// The Sonoff will publish a 1 on this topic when it connects to the MQTT server. A 0 will be published on
// this topic as a Last Will and Testament message which is sent by the MQTT server if the Sonoff is dis-
// connected for any reason.
char* deviceAvailabilityTopic = "light/lobby/availability";

// This message will be sent to the topic "automation" when the switch is toggled twice. 
char* deviceAutomationPayload = "lobby";

// The amount of time (in milliseconds) to wait for the switch to be toggled again. 
// 300 works well for me and is barely noticeable. Set to 0 if you don't intend to use this functionality.
unsigned long specialFunctionTimeout = 0;

// Determines whether the external switch GPIO is monitored or not
boolean switchEnabled = false;

// MQTT Server
char* mqttServerIP = "192.168.0.49";

// MQTT port
int mqttPort = 1883;

// MQTT User Name
char* mqttUser = "myUser";

// MQTT Password
char* mqttPass = "myPassword";

// WiFi SSID
char* SSID = "mySSID";

// WiFI Password
char* WiFiPassword = "myPassword";

////////////////////////////////////
//// DEVICE SPECIFIC CONFIG END ////
////////////////////////////////////


// Reconnect Variables
unsigned long reconnectStart = 0;
unsigned long lastReconnectMessage = 0;
unsigned long reconnectMessageInterval = 1000;
int currentReconnectStep = 0;
boolean offlineMode = true;

// Button Variables
boolean buttonState = 1; // Sonoff button is high when open
int buttonDebounceTime = 20;
int buttonLongPressTime = 2000; // 2000 = 2s
boolean buttonTiming = false;
unsigned long buttonTimingStart = 0;
int buttonAction = 0; // 0 = No action to perform, 1 = Perform short press action, 2 = Perform long press action

// Switch Variables
boolean switchState = 0;
boolean switchPreviousState = 0;
unsigned long switchIntervalStart, switchIntervalFinish, switchIntervalElapsed;
int switchCount = 0;
unsigned long switchLastStateChange = 0;
unsigned long switchDebounceDelay = 50;

// Misc
boolean relayState;
boolean recovered = false;

void sonoffRelaySwitch(boolean targetState) {
  // Off
  if (targetState == 0) {
    digitalWrite(sonoffRelayPin, LOW); // Turn the relay off
    client.publish(deviceStateTopic, "0", true); // Publish the state change
    relayState = 0; // Keep a local record of the current state
  }
  // On
  else if (targetState == 1) {
    digitalWrite(sonoffRelayPin, HIGH); // Turn the relay off
    client.publish(deviceStateTopic, "1", true); // Publish the state change
    relayState = 1; // Keep a local record of the current state
  }
}

void sonoffRelayToggle() {
  if (relayState == 0) {
    sonoffRelaySwitch(1);
  }
  else {
    sonoffRelaySwitch(0);
  }
}

void switchCheck() {
  // Lap the timer
  switchIntervalFinish = millis();

  // Calculate the elapsed time, set to 0 if it's the first press
  if (switchCount == 0) {
    switchIntervalElapsed = 0;
  }
  else {
    switchIntervalElapsed = switchIntervalFinish - switchIntervalStart;
  }

  // Check for a change in state, if changed loop for the deboucneDelay to check it's real
  switchState = digitalRead(sonoffGPIOPin);
  if (switchState != switchPreviousState) {
    unsigned long loopStart = millis();
    int supportive = 0;
    int unsupportive = 0;

    // Loop
    while ((millis() - loopStart) < switchDebounceDelay) {
      switchState = digitalRead(sonoffGPIOPin);
      if (switchState != switchPreviousState) {
        supportive++;
      }
      else {
        unsupportive++;
      }
    }

    // Calculate the findings and report
    if (supportive > unsupportive) {
      switchIntervalStart = millis();
      switchLastStateChange = millis();
      switchCount += 1;
      switchPreviousState = switchState;
    }
  }

  // If the elapsed time is greater that the timeout and the count is higher than 0, do stuff and reset the count
  if (switchIntervalElapsed > specialFunctionTimeout && switchCount > 0) {
    // If the count is odd, toggle the light 
    if ((switchCount % 2) != 0) {
      sonoffRelayToggle();
    }

    // If the count is 2, send the special command
    else if (switchCount == 2) {
      client.publish("automation", deviceAutomationPayload);
    }

    // If the count is 6, restart the ESP
    else if (switchCount == 6) {
      digitalWrite(sonoffRelayPin, LOW);
      delay(300);
      digitalWrite(sonoffRelayPin, HIGH);
      delay(300);
      digitalWrite(sonoffRelayPin, LOW);
      delay(300);
      digitalWrite(sonoffRelayPin, HIGH);
      delay(300);
      digitalWrite(sonoffRelayPin, LOW);
      ESP.restart();
    }

    // Reset the count
    switchCount = 0;
  }
}

void reconnect() {
  // IF statements used to complete process in single loop if possible

  // 0 - Turn the LED on and log the reconnect start time
  if (currentReconnectStep == 0) {
    digitalWrite(sonoffLEDPin, LOW);
    reconnectStart = millis();
    currentReconnectStep++;

    // If the ESP is reconnecting after a connection failure then wait a second before starting the reconnect routine
    if (offlineMode == false) {
      delay(1000);
    }
  }

  // If we've previously had a connection and have been trying to connect for more than 2 minutes then restart the ESP.
  // We don't do this if we've never had a connection as that means the issue isn't temporary and we don't want the relay
  // to turn off every 2 minutes.
  if (offlineMode == false && ((millis() - reconnectStart) > 120000)) {
    Serial.println("Restarting!");
    ESP.restart();
  }

  // 1 - Check WiFi Connection
  if (currentReconnectStep == 1) {
    if (WiFi.status() != WL_CONNECTED) {
      if ((millis() - lastReconnectMessage) > reconnectMessageInterval) {
        Serial.print("Awaiting WiFi Connection (");
        Serial.print((millis() - reconnectStart) / 1000);
        Serial.println("s)");
        lastReconnectMessage = millis();
      }
    }
    else {
      Serial.println("WiFi connected!");
      Serial.print("SSID: ");
      Serial.print(WiFi.SSID());
      Serial.println("");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      Serial.println("");

      lastReconnectMessage = 0;
      currentReconnectStep = 2;
    }
  }

  // 2 - Check MQTT Connection
  if (currentReconnectStep == 2) {
    if (!client.connected()) {      
      if ((millis() - lastReconnectMessage) > reconnectMessageInterval) {
        Serial.print("Awaiting MQTT Connection (");
        Serial.print((millis() - reconnectStart) / 1000);
        Serial.println("s)");
        lastReconnectMessage = millis();

        String clientId = "ESP8266Client-";
        clientId += String(random(0xffff), HEX);
        client.connect(clientId.c_str(), mqttUser, mqttPass, deviceAvailabilityTopic, 0, true, "0");
      }

      // Check the MQTT again and go forward if necessary
      if (client.connected()) {
        Serial.println("MQTT connected!");
        Serial.println("");

        lastReconnectMessage = 0;
        currentReconnectStep = 3;
      }

      // Check the WiFi again and go back if necessary
      else if (WiFi.status() != WL_CONNECTED) {
        currentReconnectStep = 1;
      }
    }
    else {
      Serial.println("MQTT connected!");
      Serial.println("");

      lastReconnectMessage = 0;
      currentReconnectStep = 3;
    }
  }

  // 3 - All connected, turn the LED back on, subscribe to the MQTT topics and then set offlineMode to false
  if (currentReconnectStep == 3) {
    digitalWrite(sonoffLEDPin, HIGH);  // Turn the LED off

    client.publish(deviceAvailabilityTopic, "1", true);
    
    client.subscribe(deviceControlTopic);
    client.subscribe(deviceStateTopic);

    if (offlineMode == true) {
      offlineMode = false;
      Serial.println("Offline Mode Deactivated");
      Serial.println("");
    }

    currentReconnectStep = 0; // Reset
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println("------------------- MQTT Recieved -------------------");

  if (((String(deviceControlTopic).equals(topic)) && (length == 1)) | (String(deviceStateTopic).equals(topic) && recovered == false)) {
    if ((char)payload[0] == '0') {
      sonoffRelaySwitch(0);
    }
    else if ((char)payload[0] == '1') {
      sonoffRelaySwitch(1);
    }
    recovered = true;
  }
}

void setup() {  
  // Serial
  Serial.begin(115200);
  Serial.println("");
  Serial.println("Serial Started");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, WiFiPassword);

  // LED
  pinMode(sonoffLEDPin, OUTPUT);     // Initialize the sonoffLEDPin pin as an output
  digitalWrite(sonoffLEDPin, LOW);   // Turn the LED on to indicate no connection

  // Relay Setup
  pinMode(sonoffRelayPin, OUTPUT);      // Initialize the sonoffRelayPin pin as an output
  digitalWrite(sonoffRelayPin, LOW);    // Turn device off as default

  // Switch Setup
  pinMode(sonoffGPIOPin, INPUT);                  // Initialize the sonoffGPIOPin as an input
  switchPreviousState = digitalRead(sonoffGPIOPin);   // Populate switchPreviousState with the startup state

  // MQTT
  client.setServer(mqttServerIP, mqttPort);
  client.setCallback(callback);
  
  // Sonoff Button Setup
  pinMode(sonoffButtonPin, INPUT);   // Initialize the sonoffButtonPin as an input
}

void buttonShortPressAction() {
  sonoffRelayToggle();
}

void buttonLongPressAction() {
  digitalWrite(sonoffRelayPin, LOW);
  delay(300);
  digitalWrite(sonoffRelayPin, HIGH);
  delay(300);
  digitalWrite(sonoffRelayPin, LOW);
  delay(300);
  digitalWrite(sonoffRelayPin, HIGH);
  delay(300);
  digitalWrite(sonoffRelayPin, LOW);
  ESP.restart();
}

void sonoffButtonCheck() {
  buttonState = digitalRead(sonoffButtonPin);

  if (buttonState == 0) { // The button is pressed
    if (buttonTiming == false) {
      buttonTiming = true;
      buttonTimingStart = millis();
      Serial.println("Button pressed, timing...");
    }
    else { // buttonTiming = true
      if (millis() >= (buttonTimingStart + buttonDebounceTime)) { // The button has been pressed longer than the debounce time, update the action to perform when the button is released
        buttonAction = 1;
      }
      
      if (millis() >= (buttonTimingStart + buttonLongPressTime)) { // The button has been pressed longer than the long press time, update the action to perform when the button is released
        buttonAction = 2;
      }
    }
  }
  else { // buttonState == 1, the button is released
    buttonTiming = false;

    if (buttonAction != 0) { // There is an action to perform
      if (buttonAction == 1) { // Perform the short press action
        buttonShortPressAction();
      }

      if (buttonAction == 2) { // Perform the short press action
        buttonLongPressAction();
      }

      buttonAction = 0; // Reset the buttonAction variable so that the action is only performed once
    }
  }
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  else {
    client.loop();
  }

  sonoffButtonCheck();
  
  if (switchEnabled == true) {
    switchCheck();
  }
}
