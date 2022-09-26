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

#define RADIO_GATEWAY_ID 1  // 1 for gateway, other integers for nodes
#define RADIO_NETWORK_ID 76  // The same on all nodes that talk to each other
#define RADIO_FREQUENCY RF69_868MHZ
#define RADIO_ENCRYPT_KEY "3m0I0kubJa88BMjR"  // Exactly the same 16 characters on all nodes

#define BAUD_RATE 9600
#define MAX_NUMBER_OF_LAMPS 10
#define MESSAGE_SIZE_IN_BYTES 5


// Types
// -------------------------------------
typedef enum {
  STATUS_REQUEST,   // GATEWAY LampID
  SET_MODE,         // GATEWAY LampID, value_1 = LampMode
  SET_TIME,         // GATEWAY LampID, value_1 = hours, value_2 = minutes, value_3 = seconds
  MODE_STATUS,      // NODE    LampID, value_1 = LampMode
  LIGHT_STATUS,     // NODE    LampID, value_1 = LightStatus
  MOTION_STATUS,    // NODE    LampID, value_1 = MotionStatus
  SUN_STATUS,       // NODE    LampID, value_1 = uint8_t
  NUMBER_OF_MESSAGE_TYPES,
} MessageType;

typedef enum {
  MODE_ON,
  MODE_AUTO,
  MODE_OFF,
  NUMBER_OF_MESSAGE_MODES,
} LampMode;

typedef enum {
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


// Global variables
// -------------------------------------

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
    broadcast_message(STATUS_REQUEST, 0, 0, 0, 0);
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
    for (uint8_t id = 0; id < MAX_NUMBER_OF_LAMPS; id++) {
      if (lamp_database[id].is_registered) {
        send_message(SET_MODE, id, (uint8_t)lamp_mode, 0, 0);
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

  send_message(SET_MODE, lamp_id, (uint8_t)lamp_mode, 0, 0);
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

  for (int id = 0; id < MAX_NUMBER_OF_LAMPS; id++) {
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
void print_message(MessageType type, uint8_t id, uint8_t value_1, uint8_t value_2, uint8_t value_3) {
  String type_str;
  switch (type) {
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
    default:
      {
        type_str = "UNKNOWN";
        break;
      }
  }
  Serial.println(type_str + ", ID: " + String((int)id) + ", Value: " + String((int)value_1) + " " + String((int)value_2) + " " + String((int)value_3));
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
void update_database(MessageType type, uint8_t id, uint8_t value_1, uint8_t value_2, uint8_t value_3) {
  Serial.print("[INFO] Message received: ");
  print_message(type, id, value_1, value_2, value_3);

  if (id >= MAX_NUMBER_OF_LAMPS) {
    Serial.println("[ERROR] Tried to access lamp with invalid ID: " + String((int)id));
    return;
  }

  // Message from unregistered lamp ID erases the database entry and enables the lamp
  if (lamp_database[id].is_registered == false) {
    reset_database_entry(id);
    lamp_database[id].is_registered = true;
  }

  if (type == MODE_STATUS) {
    lamp_database[id].lamp_mode = (LampMode)value_1;
  } else if (type == LIGHT_STATUS) {
    lamp_database[id].light_status = (LightStatus)value_1;
  } else if (type == MOTION_STATUS) {
    lamp_database[id].motion_status = (MotionStatus)value_1;
  } else if (type == SUN_STATUS) {
    lamp_database[id].sun_value = value_1;
  } else {
    Serial.println("[ERROR] Received unsupported message from a node: ");
    print_message(type, id, value_1, value_2, value_3);
  }
}


/*
 * This function constructs a message and sends it using the radio module.
 */
void send_message(MessageType type, uint8_t id, uint8_t value_1, uint8_t value_2, uint8_t value_3) {
  uint8_t data[MESSAGE_SIZE_IN_BYTES];

  // Adding data to the array
  data[0] = type;
  data[1] = id;
  data[2] = value_1;
  data[3] = value_2;
  data[4] = value_3;

  // Send message (radio ID has to be offset by 2, as 0 is for broadcast and 1 is for gateway)
  radio.send(id + 2, data, MESSAGE_SIZE_IN_BYTES);

  Serial.print("[INFO] Message Sent: ");
  print_message(type, id, value_1, value_2, value_3);
}


/*
 * This function constructs a message and sends it to all nodes (NODE_ID == 0).
 */
void broadcast_message(MessageType type, uint8_t id, uint8_t value_1, uint8_t value_2, uint8_t value_3) {
  uint8_t data[MESSAGE_SIZE_IN_BYTES];

  // Adding data to the array
  data[0] = type;
  data[1] = id;
  data[2] = value_1;
  data[3] = value_2;
  data[4] = value_3;

  // Send message
  radio.send(0, data, MESSAGE_SIZE_IN_BYTES);

  Serial.print("[INFO] Message Broadcasted: ");
  print_message(type, id, value_1, value_2, value_3);
}


/*
 * Receives radio message.
 * Returns nullptr if nothing was received or uint8_t list with received data.
 */
uint8_t* receive_message() {
  if (radio.receiveDone()) {
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
      return nullptr;
    }

    return data;
  }
  else {
    return nullptr;
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
  radio.encrypt(RADIO_ENCRYPT_KEY);
  delay(10);

  // Send status request message to all IDs to register existing lamps
  broadcast_message(STATUS_REQUEST, 0, 0, 0, 0);
}


void loop() {

  // NTP time update
  if (should_update_time) {
    should_update_time = false;
    timeClient.update();
    for (int id = 0; id < MAX_NUMBER_OF_LAMPS; id++) {
      if (lamp_database[id].is_registered) {
        send_message(SET_TIME, id, (uint8_t)timeClient.getHours(), (uint8_t)timeClient.getMinutes(), (uint8_t)timeClient.getSeconds());
      }
    }
  }

  // Receive Radio message
  uint8_t* message = receive_message();
  if (message != nullptr) {
    update_database((MessageType)message[0], message[1], message[2], message[3], message[4]);
  }

  // Handle HTTP requests
  server.handleClient();
}
