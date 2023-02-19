// WiFi libraries
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>

// NTP libraries
#include <Ticker.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

// Radio libraries
#include <RFM69.h>
#include <SPI.h>


#define WIFI_SSID "ssid"
#define WIFI_PASSWORD "password"

#define NTP_SERVER_ADDRESS "pool.ntp.org"
#define NTP_TIME_ZONE_OFFSET 3600         // Time zone is Central European +1 H
#define NTP_DAYLIGHT_SAVINGS_OFFSET 3600  // 3600 in summer, 0 in winter
#define NTP_SYNC_INTERVAL 60              // seconds

#define RADIO_NODE_ID 2  // Node ID used for node identification over the radio (0 is for broadcast, 1 is for gateway, 2+ are for nodes)
#define RADIO_GATEWAY_ID 1  // 1 for gateway, other integers for nodes
#define RADIO_NETWORK_ID 0  // The same on all nodes that talk to each other
#define RADIO_FREQUENCY RF69_868MHZ
#define RADIO_ENCRYPT_KEY "3m0I0kubJa88BMjR"  // Exactly the same 16 characters on all nodes

#define BAUD_RATE 9600
#define MAX_NUMBER_OF_LAMPS 10
#define MESSAGE_SIZE_IN_BYTES 8
#define MESSAGE_PAYLOAD_SIZE_IN_BYTES 5
#define LAMP_PIN LED_BUILTIN
#define RFM_SEND_NUMBER_OF_RETRIES 10
#define RFM_SEND_WAIT_RETRY_WAIT_TIME 10
#define MAX_NUMBER_OF_EVENTS 10
#define CHECK_TIMER_INTERVAL_MS 60000
#define MSG_QUEUE_SIZE 100
#define STARTING_LAMP_ID 2
// Types
// -------------------------------------
typedef enum {
  MSG_NOT_INITIALIZED,
  STATUS_REQUEST,   // GATEWAY LampID
  SET_MODE,         // GATEWAY LampID, value_1 = LampMode
  SET_TIME,         // GATEWAY LampID, value_1 = hours, value_2 = minutes, value_3 = seconds
  MODE_STATUS,      // NODE    LampID, value_1 = LampMode
  LIGHT_STATUS,     // NODE    LampID, value_1 = LightStatus
  MOTION_STATUS,    // NODE    LampID, value_1 = MotionStatus
  SUN_STATUS,       // NODE    LampID, value_1 = uint8_t
  SET_EVENT,        // value_1 = hours, value_2 = minutes, value_3 = seconds, value_4 = lamp on/off 
  REQUEST_FOR_TIME_UPDATE,
  REQUEST_FOR_EVENTS,
  NUMBER_OF_MESSAGE_TYPES,
} MessageType;

typedef enum {
  MODE_ON,
  MODE_AUTO,
  MODE_OFF,
  NUMBER_OF_MESSAGE_MODES,
} LampMode;

typedef enum {
  LIGHT_NOT_INITIALIZED,
  LIGHT_ON,
  LIGHT_OFF,
  NUMBER_LIGHT_STATUSES,
} LightStatus;

typedef enum {
  MOTION_DETECTED,
  MOTION_NOT_DETECTED,
  NUMBER_MOTION_STATUSES,
} MotionStatus;

typedef struct lamp {
  bool is_registered = false;
  LampMode lamp_mode = MODE_AUTO;
  LightStatus light_status = LIGHT_OFF;
  MotionStatus motion_status = MOTION_NOT_DETECTED;
  uint8_t sun_value = -1;
} Lamp;

typedef struct time {
  uint8_t hours = 100;
  uint8_t minutes = 100;
  uint8_t seconds = 100;
} Time;

typedef struct event
{
  Time time;
  LightStatus lamp_state = LIGHT_NOT_INITIALIZED;
} Event;

typedef struct message {
  MessageType type = MSG_NOT_INITIALIZED;
  uint8_t source_id = 0;
  uint8_t target_id = 0;
  uint8_t payload[MESSAGE_SIZE_IN_BYTES] = {0};
} Message;

// Global variables
// -------------------------------------
uint32_t print_counter = 0;
uint32_t update_time_counter = 0;

Message msg_queue[MSG_QUEUE_SIZE];
uint8_t msg_queue_index = 0;

// WiFi
ESP8266WebServer server(80);

// NTP
WiFiUDP ntp_UDP;
NTPClient timeClient(ntp_UDP, NTP_SERVER_ADDRESS);
volatile bool should_update_time = false;
Ticker ntp_time_update;

// Radio
RFM69 radio(D8, D1, false, nullptr);

Lamp lamp_database[MAX_NUMBER_OF_LAMPS];
Event time_plan[MAX_NUMBER_OF_EVENTS];

// Functions
// -------------------------------------

/*
 * Every HTTP request gets redirected to root.
 * Example: HTTP request to load url '/on3' which means setting lamp 3 to ON mode
 *          is redirected back to just '/' which is the main website.
 */
void redirect_to_root() {
  server.sendHeader("Location", String("/"), true);
  server.send(303, "text/plain", "");
}


/*
 * Checks if string contains only numbers.
 * Returns True or False.
 */
bool is_string_numeric(String str) {
  bool is_numeric = true;
  for (int i = 0; i < str.length(); i++) {
    if (!isDigit(str[i])) {
      is_numeric = false;
      break;
    }
  }
  return is_numeric;
}


void handle_status_request(String request) {
  redirect_to_root();
  if (request.indexOf("/status") >= 0) {
    // Send status request message to all IDs to register existing lamps
    Message msg={.type=STATUS_REQUEST, .source_id=RADIO_GATEWAY_ID, .target_id=0, .payload={0, 0, 0, 0, 0}};
    broadcast_message(msg);
  }
}


/*
 * Reads HTTP request from user and checks if it corresponds to command for changing lamp mode,
 * then sends the command to a lamp if it does.
 * Input parameters 'request' is a string containing the HTTP request, like '/on3' or '/auto5'.
 * Example: HTTP request to url '/auto5' is received, this function sends a message to lamp 5
 *          setting it into AUTO mode.
 */
void handle_mode_switch(String request) {
  LampMode lamp_mode = MODE_AUTO;
  String id_string;
  uint8_t lamp_id = 0;

  redirect_to_root();

  // Check which command was issued in the request header
  if (request.indexOf("/on") >= 0) {
    id_string = request.substring(3);
    lamp_mode = MODE_ON;
  } else if (request.indexOf("/auto") >= 0) {
    id_string = request.substring(5);
    lamp_mode = MODE_AUTO;
  } else if (request.indexOf("/off") >= 0) {
    id_string = request.substring(4);
    lamp_mode = MODE_OFF;
  } else {
    // If no valid mode was found, silently return,
    // different function is supposted to handle the request
    return;
  }

  // Check if command has ID attached to it
  if (id_string.length() <= 0) {
    Serial.println("[ERROR] Command without ID issued: " + request);
    return;
  }

  // If command to change mode of all lamps was given
  if (id_string[0] == 'A') {
    // Loop through every lamp in database and broadcast the new mode to it
    for (uint8_t id = STARTING_LAMP_ID; id < MAX_NUMBER_OF_LAMPS; id++) {
      if (lamp_database[id].is_registered) {
        Message msg={.type=SET_MODE, .source_id=RADIO_GATEWAY_ID, .target_id=id, .payload={(uint8_t)lamp_mode, 0, 0, 0, 0}};
        add_message_to_queue(msg);
      }
    }
    return;
  }

  // Check if ID string is numeric
  if (!is_string_numeric(id_string)) {
    Serial.println("[ERROR] ID is not A or numeric: " + request);
    return;
  }

  // Convert ID string to number
  lamp_id = id_string.toInt();

  // Check if lamp ID is in lamp database
  if (lamp_id < 0 || lamp_id >= MAX_NUMBER_OF_LAMPS) {
    Serial.println("[ERROR]  ID is outside of allowed range: " + request);
    return;
  }

  Message msg={.type=SET_MODE, .source_id=RADIO_GATEWAY_ID, .target_id=lamp_id, .payload={(uint8_t)lamp_mode, 0, 0, 0, 0}};
  add_message_to_queue(msg);
}


/*
 * Every time a new HTTP request is received,
 * this function is called.
 */
void server_handler() {
  String request = server.uri();
  handle_mode_switch(request);
  handle_status_request(request);
}


/*
 * This function is linked to a ticker and ensures
 * the microcontroller updates time from NTP server periodically.
 * This function is called every NTP_SYNC_INTERVAL seconds.
 */
void ICACHE_RAM_ATTR updateTime() {
  should_update_time = true;
}


/*
 * This function constructs HTML page and sends it to client
 * over HTTP.
 */
void ICACHE_RAM_ATTR sendPage() {
  String page = "<!DOCTYPE html>";
  page.concat("<html>"
              // Page Head START
              // ################
              "<head>"
              "<title>Lamp Control</title>"

              // Style START
              // -----------
              "<style type=\"text/css\">"

              ".lamp {"
              "color: Black;"
              "background-color: Silver;"
              "font-family: Arial, Helvetica, sans-serif;"
              "width: 220px;"
              "border: 3px solid LightGrey;"
              "padding: 10px;"
              "margin: 10px;"
              "border-radius: 25px;"
              "position: relative;"
              "}"

              ".header {"
              "color: White;"
              "background-color: DodgerBlue;"
              "padding: 20px;"
              "text-align: left;"
              "font-family: Arial, Helvetica, sans-serif;"
              "}"

              ".indicator {"
              "color: White;"
              "background-color: Black;"
              "border: 2px solid DimGrey;"
              "padding: 12px;"
              "border-radius: 50%;"
              "position: absolute;"
              "top: 10px;"
              "right: 10px;"
              "}"

              ".indicator.on {"
              "background-color: Chartreuse;"
              "}"

              ".indicator.off {"
              "background-color: Red;"
              "}"

              ".button {"
              "background-color: White;"
              "border: none;"
              "color: Black;"
              "font-family: Arial, Helvetica, sans-serif;"
              "padding: 10px 10px;"
              "text-align: center;"
              "text-decoration: none;"
              "display: inline-block;"
              "font-size: 16px;"
              "margin: 4px 2px;"
              "cursor: pointer;"
              "border-radius: 15px;"
              "}"

              ".button.on {"
              "background-color: MediumSeaGreen;"
              "padding: 10px 16px;"
              "}"

              ".button.auto {"
              "background-color: GoldenRod;"
              "padding: 10px 8px;"
              "}"

              ".button.off {"
              "background-color: IndianRed;"
              "padding: 10px 12px;"
              "}"

              ".button.status {"
              "background-color: DarkGray;"
              "padding: 10px 12px;"
              "}"

              "</style>"
              // ---------
              // Style END

              // Script START
              // ------------
              "<script type=\"text/javascript\">"
              "setInterval(function() {location.reload();}, 1000);"
              "</script>"
              // ----------
              // Script END

              "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"

              "</head>"
              // ################
              // Page Head END

              // Generate the body of the webpage dynamically
              "<body>"

              // Add header
              "<div class=\"header\">"
              "<b>Lamp Control</b>"
              "<table>"
              "<tr>"
              "<td><a href=\"/onA\" class=\"button on\">All ON</a></td>"
              "<td><a href=\"/autoA\" class=\"button auto\">All AUTO</a></td>"
              "<td><a href=\"/offA\" class=\"button off\">All OFF</a></td>"
              "<td><a href=\"/status\" class=\"button status\">Status Request</a></td>"
              "</tr>"
              "</table>"
              "</div><br>"

              "<center>");

  for (int id = STARTING_LAMP_ID; id < MAX_NUMBER_OF_LAMPS; id++) {
    if (lamp_database[id].is_registered) {
      page.concat("<div class=\"lamp\">");
      page.concat("<b>Lamp " + String(id + 1) + "</b>");

      if (lamp_database[id].light_status == LIGHT_ON) {
        page.concat("<div class=\"indicator on\"></div><br>");
      } else {
        page.concat("<div class=\"indicator off\"></div><br>");
      }

      page.concat("<table>");
      page.concat("<tr><th>Light status: </th> <td>" + String(lamp_database[id].light_status == LIGHT_ON ? "ON" : "OFF") + "</td></tr>");
      page.concat("<tr><th>Motion status: </th> <td>" + String(lamp_database[id].motion_status == MOTION_DETECTED ? "Detected" : "Not detected") + "</td></tr>");
      page.concat("<tr><th>Ambient light:</th> <td>" + ((lamp_database[id].sun_value == -1) ? "None" : (String(lamp_database[id].sun_value) + "%")) + "</td></tr>");

      // Lamp mode display
      page.concat("<tr>");
      page.concat("<th>Mode:</th>");
      page.concat("<td>");
      if (lamp_database[id].lamp_mode == MODE_ON) {
        page.concat("Always ON");
      } else if (lamp_database[id].lamp_mode == MODE_AUTO) {
        page.concat("AUTO");
      } else if (lamp_database[id].lamp_mode == MODE_OFF) {
        page.concat("Always OFF");
      } else {
        page.concat("ERROR");
      }
      page.concat("</td>");
      page.concat("</tr>");
      page.concat("</table>");

      // Lamp control buttons
      page.concat("<table>");
      page.concat("<tr>");
      page.concat("<td><a href=\"/on" + String(id) + "\" class=\"button on\">ON</a></td>");
      page.concat("<td><a href=\"/auto" + String(id) + "\" class=\"button auto\">AUTO</a></td>");
      page.concat("<td><a href=\"/off" + String(id) + "\" class=\"button off\">OFF</a></td>");
      page.concat("</tr>");
      page.concat("</table>");

      page.concat("</div><br>");
    }
  }
  page.concat(
    "</center>"
    "</body>"
    "</html>");

  server.send(200, "text/html", page);
}


/*
 * Prints formatted message to console.
 */
void print_message(Message message) {
  String type_str;
  switch (message.type) {
    case MSG_NOT_INITIALIZED:
      {
        type_str = "MSG_NOT_INITIALIZED";
        break;
      }
    case STATUS_REQUEST:
      {
        type_str = "STATUS_REQUEST";
        break;
      }
    case SET_MODE:
      {
        type_str = "SET_MODE";
        break;
      }
    case SET_TIME:
      {
        type_str = "SET_TIME";
        break;
      }
    case MODE_STATUS:
      {
        type_str = "MODE_STATUS";
        break;
      }
    case LIGHT_STATUS:
      {
        type_str = "LIGHT_STATUS";
        break;
      }
    case MOTION_STATUS:
      {
        type_str = "MOTION_STATUS";
        break;
      }
    case SUN_STATUS:
      {
        type_str = "SUN_STATUS";
        break;
      }
    case SET_EVENT:
      {
        type_str = "SET_EVENT";
        break;
      }
    case REQUEST_FOR_TIME_UPDATE:
      {
        type_str = "REQUEST_FOR_TIME_UPDATE";
        break;
      }
    case REQUEST_FOR_EVENTS:
      {
        type_str = "REQUEST_FOR_EVENTS";
        break;
      }
    default:
      {
        type_str = "UNKNOWN, value: "+String(message.type);
        break;
      }
  }
  Serial.print(type_str + ", Source ID: " + String((int)message.source_id) + ", Target ID: "+String((int)message.target_id) +", Payload: ");
  for (uint16_t i = 0;i<MESSAGE_PAYLOAD_SIZE_IN_BYTES;i++)
  {
    Serial.print(String((int)message.payload[i]) + ", ");
  }
  Serial.print("\n\r");
}

void print_time_plan()
{
  Serial.println("printing time plan:");
  for(uint8_t i = 0;i<MAX_NUMBER_OF_EVENTS;i++)
  {
    Serial.print("Index: "+String((int)i)+", hour: "+String((int)time_plan[i].time.hours)+", minute "+String((int)time_plan[i].time.minutes)+", second "+String((int)time_plan[i].time.seconds)+", lamp state: ");
    String lamp_str = "Unknown lamp state";
    switch(time_plan[i].lamp_state) 
    {
      case LIGHT_NOT_INITIALIZED:
      {
        lamp_str = "LIGHT_NOT_INITIALIZED";
        break;
      }
      case LIGHT_ON:
      {
        lamp_str = "LIGHT_ON";
        break;
      }
      case LIGHT_OFF:
      {
        lamp_str = "LIGHT_OFF";
        break;
      }
      case NUMBER_LIGHT_STATUSES:
      {
        lamp_str = "NUMBER_LIGHT_STATUSES";
        break;
      }
    }
    Serial.print(lamp_str+"\n\r");
  }
}

/*
 * Resets the specified entry of the global database of lamps.
 */
void reset_database_entry(uint8_t id) {
  lamp_database[id].is_registered = false;
  lamp_database[id].lamp_mode = MODE_AUTO;
  lamp_database[id].light_status = LIGHT_OFF;
  lamp_database[id].motion_status = MOTION_NOT_DETECTED;
  lamp_database[id].sun_value = -1;
}


/*
 * Updates the specified entry of the global database of lamps according to received radio message.
 */
void update_database(Message message) {
  Serial.println("updating database");
  if (message.source_id >= MAX_NUMBER_OF_LAMPS) {
    Serial.println("[ERROR] Tried to access lamp with invalid source ID: " + String((int)message.source_id));
    return;
  }

  // Message from unregistered lamp ID erases the database entry and enables the lamp
  if (lamp_database[message.source_id].is_registered == false) {
    Serial.println("id "+String(message.source_id) + " is newly registered, sending time plan");
    for(uint8_t i = 0;i<MAX_NUMBER_OF_EVENTS;i++)
    {
      if(time_plan[i].lamp_state != LIGHT_NOT_INITIALIZED)
      {
        Message msg={.type=SET_EVENT, .source_id = RADIO_GATEWAY_ID, .target_id=message.source_id, .payload={time_plan[i].time.hours, time_plan[i].time.minutes, time_plan[i].time.seconds, time_plan[i].lamp_state, i}};
        add_message_to_queue(msg);
      }
    }

    reset_database_entry(message.source_id);
    lamp_database[message.source_id].is_registered = true;
  }
  
  if (message.type == MODE_STATUS)
  {
    lamp_database[message.source_id].lamp_mode = (LampMode)message.payload[0];
  } 
  else if (message.type == LIGHT_STATUS) 
  {
    lamp_database[message.source_id].light_status = (LightStatus)message.payload[0];
  } 
  else if (message.type == MOTION_STATUS) 
  {
    lamp_database[message.source_id].motion_status = (MotionStatus)message.payload[0];
  } 
  else if (message.type == SUN_STATUS) 
  {
    lamp_database[message.source_id].sun_value = message.payload[0];
  }
  else if (message.type == REQUEST_FOR_EVENTS)
  {
    for(uint8_t i = 0;i<MAX_NUMBER_OF_EVENTS;i++)
    {
      if(time_plan[i].lamp_state != LIGHT_NOT_INITIALIZED)
      {      
        Message msg={.type=SET_EVENT, .source_id = RADIO_GATEWAY_ID, .target_id=message.source_id, .payload={time_plan[i].time.hours, time_plan[i].time.minutes, time_plan[i].time.seconds, time_plan[i].lamp_state, i}};
        add_message_to_queue(msg);
      }
    }
  }
  else if(message.type == REQUEST_FOR_TIME_UPDATE)
  {
    Message msg={.type=SET_TIME, .source_id=RADIO_GATEWAY_ID, .target_id=message.source_id, .payload={(uint8_t)timeClient.getHours(), (uint8_t)timeClient.getMinutes(), (uint8_t)timeClient.getSeconds(), 0}};
    add_message_to_queue(msg);
  }

  else 
  {
    Serial.println("[ERROR] Received unsupported message from a node: ");
    print_message(message);
  }
}

uint8_t add_message_to_queue(Message message)
{
  Serial.println("msg queue index: "+String(msg_queue_index));
  if(msg_queue_index >= MSG_QUEUE_SIZE)
  {
    return -1;
  }

  msg_queue[msg_queue_index] = message;
  msg_queue_index++;
  return 0;
}

/*
 * This function constructs a message and sends it using the radio module.
 */
void send_message(Message message) {

  // Send message to gateway
  uint8_t buf[MESSAGE_SIZE_IN_BYTES] = {0};
  buf[0] = message.type;
  buf[1] = message.source_id;
  buf[2] = message.target_id;
  uint8_t header_size = MESSAGE_SIZE_IN_BYTES - MESSAGE_PAYLOAD_SIZE_IN_BYTES;
  for(uint8_t i = 0;i<MESSAGE_PAYLOAD_SIZE_IN_BYTES;i++)
  {
    buf[i + header_size] = message.payload[i];
  }
  radio.sendWithRetry(message.target_id, buf, MESSAGE_SIZE_IN_BYTES);

  Serial.print("[INFO] Message Sent: ");
  print_message(message);
}

/*
 * This function constructs a message and sends it to all nodes (NODE_ID == 0).
 */
void broadcast_message(Message message) {
  Message message_copy = message;
  for(uint8_t i = STARTING_LAMP_ID;i<MAX_NUMBER_OF_LAMPS;i++)
  {
    message_copy.target_id=i;
    add_message_to_queue(message_copy);
    // Receive Radio message
    delay(10);

    for(uint8_t i = 0;i<5;i++)
    {
      Message message = receive_message();
      if (message.type != MSG_NOT_INITIALIZED) {
        update_database(message);
      }
    }
  }
}


/*
 * Receives radio message.
 * Returns nullptr if nothing was received or uint8_t list with received data.
 */
Message receive_message() {
  
  if (radio.receiveDone()) {
    Message msg;  
    uint8_t* data = radio.DATA;
    uint8_t message_length = radio.DATALEN;
    
    // Check if sender wanted an ACK
    if (radio.ACKRequested()) {
      radio.sendACK();
    }
    // If message does not have correct size
    if (message_length != MESSAGE_SIZE_IN_BYTES) {
      Serial.print("[ERROR] Received message with wrong size: ");
      Serial.print(message_length);
      Serial.print(", should be ");
      Serial.println(MESSAGE_SIZE_IN_BYTES);
      return msg;
    }

    msg.type = (MessageType) data[0];
    msg.source_id = data[1];
    msg.target_id = data[2];

    uint8_t header_size = MESSAGE_SIZE_IN_BYTES - MESSAGE_PAYLOAD_SIZE_IN_BYTES;

    for(uint8_t i = 0;i<MESSAGE_PAYLOAD_SIZE_IN_BYTES;i++)
    {
      msg.payload[i] = data[header_size+i];
    }

    Serial.print("[INFO] Message received: ");
    print_message(msg);

    return msg;
  }
  else {
    Message msg;  
    return msg;
  }
}


void setup() {
  Serial.begin(BAUD_RATE);
  Serial.println();

  // Connect to existing WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to " + String(WIFI_SSID) + "...");
    delay(500);
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  delay(10);

  // Init NTP server
  timeClient.begin();
  timeClient.setTimeOffset(NTP_TIME_ZONE_OFFSET + NTP_DAYLIGHT_SAVINGS_OFFSET);
  timeClient.update();
  ntp_time_update.attach(NTP_SYNC_INTERVAL, updateTime);
  delay(10);

  // Init HTTP server
  server.on("/", sendPage);
  server.onNotFound(server_handler);
  server.begin();
  Serial.println("HTTP server started");
  delay(10);

  // Init Radio
  radio.initialize(RADIO_FREQUENCY, RADIO_GATEWAY_ID, RADIO_NETWORK_ID);
  //radio.encrypt(RADIO_ENCRYPT_KEY);
  delay(10);

  time_plan[0].time.hours = 20;
  time_plan[0].time.minutes = 32;
  time_plan[0].time.seconds = 0;
  time_plan[0].lamp_state = (LightStatus) LIGHT_ON;

  time_plan[1].time.hours = 20;
  time_plan[1].time.minutes = 33;
  time_plan[1].time.seconds = 0;
  time_plan[1].lamp_state = (LightStatus) LIGHT_OFF;

  time_plan[2].time.hours = 20;
  time_plan[2].time.minutes = 34;
  time_plan[2].time.seconds = 0;
  time_plan[2].lamp_state = (LightStatus) LIGHT_ON;
  
  // Send status request message to all IDs to register existing lamps
  Message msg={.type=STATUS_REQUEST, .source_id = RADIO_GATEWAY_ID, .target_id=0, .payload={0, 0, 0, 0, 0}};
  broadcast_message(msg);
}

void loop() {

  if(msg_queue_index!=0)
  {
    send_message(msg_queue[0]);
    for(uint8_t i = 0;i<msg_queue_index;i++)
    {
      msg_queue[i] = msg_queue[i+1];
    }
    msg_queue_index--;
  }

  print_counter++;
  if(print_counter%100000==0)
  {
    /*update_time_counter++;
    if(update_time_counter%10 == 0)
    {
      should_update_time = true;
    }*/
    print_time_plan();

    Serial.println("current time: "+String(timeClient.getHours()) + ":"+ String(timeClient.getMinutes()) + ":"+ String(timeClient.getSeconds()));

  }

  // NTP time update
  if (should_update_time == true) {
    timeClient.update();
    Serial.println("Sending Time: "+String(timeClient.getHours()) + ":"+ String(timeClient.getMinutes()) + ":"+ String(timeClient.getSeconds()));
    should_update_time = false;
    for (int id = STARTING_LAMP_ID; id < MAX_NUMBER_OF_LAMPS; id++) {
      Serial.print("ID: "+String(id));
      if (lamp_database[id].is_registered) {
        Serial.print(" is registered, sending message");
        Message msg={.type=SET_TIME, .source_id=RADIO_GATEWAY_ID, .target_id=id, .payload={(uint8_t)timeClient.getHours(), (uint8_t)timeClient.getMinutes(), (uint8_t)timeClient.getSeconds(), 0}};
        add_message_to_queue(msg);
      }
      else
      {
        Serial.print(" is not registered\n\r");
      }
    }
  }
  
  // Receive Radio message
  Message message = receive_message();
  if (message.type != MSG_NOT_INITIALIZED) {
    update_database(message);
  }
  
  // Handle HTTP requests
  server.handleClient();
}
