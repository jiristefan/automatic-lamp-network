// Radio libraries
#include <RFM69.h>
#include <SPI.h>

#include <Ticker.h>

#define RADIO_NODE_ID 2  // Node ID used for node identification over the radio (0 is for broadcast, 1 is for gateway, 2+ are for nodes)
#define RADIO_GATEWAY_ID 1  // 1 for gateway, other integers for nodes
#define RADIO_NETWORK_ID 0  // The same on all nodes that talk to each other
#define RADIO_FREQUENCY RF69_868MHZ
#define RADIO_ENCRYPT_KEY "3m0I0kubJa88BMjR"  // Exactly the same 16 characters on all nodes

#define NODE_ID (RADIO_NODE_ID - 2) // Actual node ID in the gateway database (0 is for broadcast, 1 is for gateway, 2+ are for nodes)
#define BAUD_RATE 9600
#define MAX_NUMBER_OF_LAMPS 10
#define MESSAGE_SIZE_IN_BYTES 8
#define MESSAGE_PAYLOAD_SIZE_IN_BYTES 5
#define LAMP_PIN BUILTIN_LED
#define RFM_SEND_NUMBER_OF_RETRIES 10
#define RFM_SEND_WAIT_RETRY_WAIT_TIME 10
#define MAX_NUMBER_OF_EVENTS 10
#define CHECK_TIMER_INTERVAL_MS 10000
#define MSG_QUEUE_SIZE 100
#define STARTING_LAMP_ID 2
#define TIMER_UPDATE_INTERVAL 1

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
}  MessageType;

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
unsigned long previousCounter=0;
Time current_node_time;

Message msg_queue[MSG_QUEUE_SIZE];
uint8_t msg_queue_index = 0;

Event time_plan[MAX_NUMBER_OF_EVENTS];
uint8_t index_of_last_added_event = 0;
uint32_t print_counter = 0;

// Radio
RFM69 radio(D8, D1, false, nullptr);

// Lamp variables
LampMode lamp_mode = MODE_AUTO;
LightStatus light_status = LIGHT_OFF;
MotionStatus motion_status = MOTION_NOT_DETECTED;
uint8_t sun_value = 0;
Ticker timer_update;

// Functions
// -------------------------------------

uint8_t get_index_of_event(Event time_plan[MAX_NUMBER_OF_EVENTS], Time current_time)
{
  if(time_plan[0].lamp_state == LIGHT_NOT_INITIALIZED)
  {
    Serial.println("No initialized events detected");
    Message msg={.type=REQUEST_FOR_EVENTS, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={0, 0, 0, 0, 0}};
    add_message_to_queue(msg);

    return -1;
  }
  
  uint8_t top_index = 0;
  for(uint8_t i = 0; i < MAX_NUMBER_OF_EVENTS; i++)
  {
    if(time_plan[i].lamp_state == LIGHT_NOT_INITIALIZED)
    {
      top_index = i-1;
      Serial.println("Top index is at: "+ String(i));
      break;
    }
  }
  uint8_t index = 0;
  
  uint32_t node_time_in_secs = current_time.hours * 3600 + current_time.minutes * 60 + current_time.seconds;
  uint32_t event_time_in_secs = time_plan[0].time.hours * 3600 + time_plan[0].time.minutes * 60 + time_plan[0].time.seconds;
  Serial.println("Node time is "+String(node_time_in_secs) + " Event time at index 0 is "+String(event_time_in_secs));
  if(node_time_in_secs < event_time_in_secs)
  {
    Serial.println("Current node time is lower than the first event in the time plan, lamp should be set to the highest event");
    return top_index;
  }

  for(uint8_t i = 0; i <= top_index; i++)
  {
    uint32_t event_time_in_secs = time_plan[i].time.hours * 3600 + time_plan[i].time.minutes * 60 + time_plan[i].time.seconds;
    Serial.println("Event time at index "+String(i)+ " is " + String(event_time_in_secs));
    if(node_time_in_secs > event_time_in_secs)
    {
      Serial.println("Current node time is higher than the event at the index "+String(i) + ", checking next event in the queue");
      index = i;
    }
  }

  return index;
}

uint8_t handle_events()
{
  print_time_plan();
  Serial.println("Checking events");
  uint8_t index = get_index_of_event(time_plan, current_node_time);
  Serial.println("Got index " + String(index));
  if(index>MAX_NUMBER_OF_EVENTS)
  {
    Serial.println("Current node time is not set");
    return -1; 
  }

  Serial.println("Lamp at " + String(index) + " is in state " + String(time_plan[index].lamp_state));
  light_status = time_plan[index].lamp_state;
  switch_lamp(light_status);

  return 0;
}

bool is_light()
{
  /*
    jestli sviti slunce, vratit true
  */
  //return true;
  return false;
}

uint8_t switch_lamp(LightStatus state)
{
  if(state == LIGHT_ON)
  {
      digitalWrite(LAMP_PIN, 0); // 0 is ON, 1 is OFF
  }
  else if(state == LIGHT_OFF)
  {
      digitalWrite(LAMP_PIN, 1); // 0 is ON, 1 is OFF
  }
  return 0;
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

void handle_message(Message message) {
  if (message.source_id != RADIO_GATEWAY_ID && message.target_id != RADIO_NODE_ID && message.type != STATUS_REQUEST) {
    Serial.print("[ERROR] Message with wrong ID received: ");
    print_message(message);
    return;
  }

  if(message.type == MSG_NOT_INITIALIZED)
  {
    Serial.print("[ERROR] Message is not initialized: ");
    print_message(message);
    return;
  }

  if (message.type == STATUS_REQUEST) {
    Message msg={.type=MODE_STATUS, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) lamp_mode, 0, 0, 0, 0}};
    add_message_to_queue(msg);

    msg={.type=LIGHT_STATUS, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) light_status, 0, 0, 0, 0}};
    add_message_to_queue(msg);

    msg={.type=MOTION_STATUS, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) motion_status, 0, 0, 0, 0}};
    add_message_to_queue(msg);

    msg={.type=SUN_STATUS, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) sun_value, 0, 0, 0, 0}};
    add_message_to_queue(msg);

  } else if (message.type == SET_MODE) {
    lamp_mode = (LampMode) message.payload[0];
    Message msg={.type=MODE_STATUS, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) lamp_mode, 0, 0, 0, 0}};
    add_message_to_queue(msg);
  } else if (message.type == SET_TIME) {
    current_node_time.hours = message.payload[0];
    current_node_time.minutes = message.payload[1];
    current_node_time.seconds = message.payload[2];
  } else if (message.type == SET_EVENT) {
    
    uint8_t replace_index = message.payload[4];    
    time_plan[replace_index].time.hours = message.payload[0];
    time_plan[replace_index].time.minutes = message.payload[1];
    time_plan[replace_index].time.seconds = message.payload[2];
    time_plan[replace_index].lamp_state = (LightStatus)message.payload[3];
    //print_time_plan(time_plan, MAX_NUMBER_OF_EVENTS);
  } else {
    Serial.print("[ERROR] Received unsupported message from a node: ");
    print_message(message);
  }
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

void update_time()
{
  if(current_node_time.seconds > 60 || current_node_time.minutes > 60 || current_node_time.hours > 24)
  {
    Serial.println("Invalid time, not updating it, sending a message to get noticed by gateway");
    Message msg={.type=REQUEST_FOR_TIME_UPDATE, .source_id=RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={0, 0, 0, 0, 0}};
    add_message_to_queue(msg);
    return;
  }

  current_node_time.seconds += 1;
  if(current_node_time.seconds >= 60)
  {
    current_node_time.seconds = 0;
    current_node_time.minutes += 1;
    if(current_node_time.minutes >= 60)
    {
      current_node_time.minutes = 0;
      current_node_time.hours += 1;
      if(current_node_time.hours >= 24)
      {
        current_node_time.hours = 0;
      }
    }
  }
}

void setup() {
  Serial.begin(BAUD_RATE);
  Serial.println();
  Serial.println("Starting a node with ID: " + String(RADIO_NODE_ID));
  timer_update.attach(TIMER_UPDATE_INTERVAL, update_time);
  // Initialize the LED_BUILTIN pin as an output
  pinMode(LED_BUILTIN, OUTPUT);
  switch_lamp(light_status);

  // Init Radio
  if(radio.initialize(RADIO_FREQUENCY, RADIO_NODE_ID, RADIO_NETWORK_ID) == true)
  {
    Serial.println("initialize is true");
  }
  else
  {
    Serial.println("initialize is false");
  }
  //radio.encrypt(RADIO_ENCRYPT_KEY);
  delay(10);

}

void loop() {
  // put your main code here, to run repeatedly:
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
    Serial.println("current time: "+String(current_node_time.hours) +":"+ String(current_node_time.minutes) +":"+ String(current_node_time.seconds));
  }
  
  Message message = receive_message();
  if (message.type != MSG_NOT_INITIALIZED) {
    handle_message(message);
  }
  
  if (lamp_mode == MODE_ON && light_status == LIGHT_OFF) {
    Serial.println("Switching lamp on");
    light_status = LIGHT_ON;
    switch_lamp(light_status); // turn on the lamp
    
    Message msg={.type=LIGHT_STATUS, .source_id = RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) light_status, 0, 0, 0, 0}};
    add_message_to_queue(msg);
  }
  else if (lamp_mode == MODE_OFF && light_status == LIGHT_ON) {
    light_status = LIGHT_OFF;
    Serial.println("Switching lamp off");
    switch_lamp(light_status); // turn off the lamp
    Message msg={.type=LIGHT_STATUS, .source_id = RADIO_NODE_ID, .target_id=RADIO_GATEWAY_ID, .payload={(uint8_t) light_status, 0, 0, 0, 0}};
    add_message_to_queue(msg);
  }
  else if (lamp_mode == MODE_AUTO) {
    unsigned long currentCounter = millis();

    if ((currentCounter - previousCounter) >= CHECK_TIMER_INTERVAL_MS) 
    { // check for rollover
      handle_events();
      previousCounter = currentCounter;
    }
  }
}
