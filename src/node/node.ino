// Radio libraries
#include <RFM69.h>
#include <SPI.h>

#define RADIO_NODE_ID 2  // Node ID used for node identification over the radio (0 is for broadcast, 1 is for gateway, 2+ are for nodes)
#define RADIO_GATEWAY_ID 1  // 1 for gateway, other integers for nodes
#define RADIO_NETWORK_ID 76  // The same on all nodes that talk to each other
#define RADIO_FREQUENCY RF69_868MHZ
#define RADIO_ENCRYPT_KEY "3m0I0kubJa88BMjR"  // Exactly the same 16 characters on all nodes

#define NODE_ID (RADIO_NODE_ID - 2) // Actual node ID in the gateway database (0 is for broadcast, 1 is for gateway, 2+ are for nodes)
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

typedef struct time {
  uint8_t hours = 0;
  uint8_t minutes = 0;
  uint8_t seconds = 0;
} Time;

// Global variables
// -------------------------------------

// Radio
RFM69 radio(D8, D1, false, nullptr);

// Lamp variables
LampMode lamp_mode = MODE_AUTO;
LightStatus light_status = LIGHT_OFF;
MotionStatus motion_status = MOTION_NOT_DETECTED;
uint8_t sun_value = 0;
Time node_time;

// Functions
// -------------------------------------

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

  // Send message to gateway
  radio.sendWithRetry(RADIO_GATEWAY_ID, data, MESSAGE_SIZE_IN_BYTES, 10, 10);

  Serial.print("[INFO] Message Sent: ");
  print_message(type, id, value_1, value_2, value_3);
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
      }print_message(type, id, value_1, value_2, value_3);
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


void handle_message(MessageType type, uint8_t id, uint8_t value_1, uint8_t value_2, uint8_t value_3) {
  Serial.print("[INFO] Message received: ");
  print_message(type, id, value_1, value_2, value_3);

  if (id != NODE_ID && type != STATUS_REQUEST) {
    Serial.print("[ERROR] Message with wrong ID received: ");
    print_message(type, id, value_1, value_2, value_3);
    return;
  }

  if (type == STATUS_REQUEST) {
    send_message(MODE_STATUS, NODE_ID, (uint8_t) lamp_mode, 0, 0);
    send_message(LIGHT_STATUS, NODE_ID, (uint8_t) light_status, 0, 0);
    send_message(MOTION_STATUS, NODE_ID, (uint8_t) motion_status, 0, 0);
    send_message(SUN_STATUS, NODE_ID, sun_value, 0, 0);
  } else if (type == SET_MODE) {
    lamp_mode = (LampMode) value_1;
    send_message(MODE_STATUS, NODE_ID, (uint8_t)lamp_mode, 0, 0);
  } else if (type == SET_TIME) {
    node_time.hours = value_1;
    node_time.minutes = value_2;
    node_time.seconds = value_3;
  } else {
    Serial.print("[ERROR] Received unsupported message from a node: ");
    print_message(type, id, value_1, value_2, value_3);
  }
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

  // Initialize the LED_BUILTIN pin as an output
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LED_BUILTIN, LIGHT_OFF);

  // Init Radio
  radio.initialize(RADIO_FREQUENCY, RADIO_NODE_ID, RADIO_NETWORK_ID);
  radio.encrypt(RADIO_ENCRYPT_KEY);
  delay(10);

}

void loop() {
  // put your main code here, to run repeatedly:
  uint8_t* message = receive_message();
  if (message != nullptr) {
    handle_message((MessageType)message[0], message[1], message[2], message[3], message[4]);
  }
  
  if (lamp_mode == MODE_ON && light_status == LIGHT_OFF) {
    light_status = LIGHT_ON;
    send_message(LIGHT_STATUS, NODE_ID, (uint8_t)light_status, 0, 0);
  }
  else if (lamp_mode == MODE_OFF && light_status == LIGHT_ON) {
    light_status = LIGHT_OFF;
    send_message(LIGHT_STATUS, NODE_ID, (uint8_t)light_status, 0, 0);
  }
  else if (lamp_mode == MODE_AUTO) {
    // Just turn the LED on and off every minute
    if (node_time.minutes % 2 == 0 && light_status == LIGHT_OFF) {
      light_status = LIGHT_ON;
      send_message(LIGHT_STATUS, NODE_ID, (uint8_t)light_status, 0, 0);
    }
    else if (node_time.minutes % 2 != 0 && light_status == LIGHT_ON) {
      light_status = LIGHT_OFF;
      send_message(LIGHT_STATUS, NODE_ID, (uint8_t)light_status, 0, 0);
    }
  }

  digitalWrite(LED_BUILTIN, light_status); // 0 is ON, 1 is OFF
}
