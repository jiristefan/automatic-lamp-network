#include <ESP8266WiFi.h>
#include <RFM69.h>
#include <SPI.h>

#define NETWORKID     0   // Must be the same for all nodes
#define MYNODEID      2   // My node ID
#define TONODEID      1   // Destination node ID

#define FREQUENCY     RF69_868MHZ
#define ENCRYPT_KEY   "3m0I0kubJa88BMjR" // exactly the same 16 characters/bytes on all nodes!
#define SERIAL_BAUD   9600

//SPIClass *spi = new SPIClass();

//RFM69 radio(D8, D1, false, spi);
RFM69 radio(D8, D1, false, nullptr);

int counter = 0;
char buf[10];

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  delay(10);

  WiFi.mode(WIFI_OFF);
  delay(10);
  
  if(radio.initialize(FREQUENCY, MYNODEID, NETWORKID) == true)
  {
    Serial.println("initialize is true");
  }
  else
  {
    Serial.println("initialize is false");
  }
  
  //radio.encrypt(ENCRYPT_KEY);
  Serial.print("Frequency [Hz]: ");
  Serial.println(radio.getFrequency());
  Serial.print("Power [dB]: ");
  Serial.println(radio.getPowerLevel());
  Serial.print("Temperature [C]: ");
  Serial.println(radio.readTemperature());
  Serial.print("RSSI [dB]: ");
  Serial.println(radio.readRSSI());
  delay(10);
  
  Serial.println("Setup finished");
  Serial.flush();
}

void loop() {
  Serial.println("sending message");
  sprintf(buf, "%i", counter);
  counter += 1;
  radio.sendWithRetry(TONODEID, buf, 10); // Target node ID, message as string or byte array, message length
  Serial.println("Message sent");
  delay(5000);
}

// RFM69HCW Example Sketch
// Send serial input characters from one RFM69 node to another
// Based on RFM69 library sample code by Felix Rusu
// http://LowPowerLab.com/contact
// Modified for RFM69HCW by Mike Grusin, 4/16

// This sketch will show you the basics of using an
// RFM69HCW radio module. SparkFun's part numbers are:
// 915MHz: https://www.sparkfun.com/products/12775
// 434MHz: https://www.sparkfun.com/products/12823

// See the hook-up guide for wiring instructions:
// https://learn.sparkfun.com/tutorials/rfm69hcw-hookup-guide

// Uses the RFM69 library by Felix Rusu, LowPowerLab.com
// Original library: https://www.github.com/lowpowerlab/rfm69
// SparkFun repository: https://github.com/sparkfun/RFM69HCW_Breakout

// Include the RFM69 and SPI libraries:

/*#include <RFM69.h>
#include <SPI.h>

// Addresses for this node. CHANGE THESE FOR EACH NODE!

#define NETWORKID     0   // Must be the same for all nodes
#define MYNODEID      3   // My node ID
#define TONODEID      2   // Destination node ID

// RFM69 frequency, uncomment the frequency of your module:

//#define FREQUENCY   RF69_433MHZ
#define FREQUENCY     RF69_868MHZ

// AES encryption (or not):

#define ENCRYPT       true // Set to "true" to use encryption
#define ENCRYPTKEY    "TOPSECRETPASSWRD" // Use the same 16-byte key on all nodes

// Use ACKnowledge when sending messages (or not):

#define USEACK        true // Request ACKs or not

// Packet sent/received indicator LED (optional):

#define LED           D4 // LED positive pin
//#define GND           D4 // LED ground pin

// Create a library object for our RFM69HCW module:

RFM69 radio;
uint8_t counter = 0;

void setup()
{
  // Open a serial port so we can send keystrokes to the module:

  Serial.begin(9600);
  Serial.print("Node ");
  Serial.print(MYNODEID,DEC);
  Serial.println(" ready");  

  // Set up the indicator LED (optional):

  pinMode(LED,OUTPUT);
  digitalWrite(LED,LOW);
  pinMode(GND,OUTPUT);
  digitalWrite(GND,LOW);

  // Initialize the RFM69HCW:
  // radio.setCS(10);  //uncomment this if using Pro Micro
  if(radio.initialize(FREQUENCY, MYNODEID, NETWORKID) == true)
  {
    Serial.print("initialization is true");
  }
  else
  {
    Serial.print("initialization is false");
  }
  
  radio.setHighPower(); // Always use this for RFM69HCW
  // Turn on encryption if desired:

  if (ENCRYPT)
    radio.encrypt(ENCRYPTKEY);
}

void loop()
{
  // Set up a "buffer" for characters that we'll send:

  static char sendbuffer[62];
  for(int i=0;i<62;i++)
  {
    sendbuffer[i]=0;
  }
  static int sendlength = 0;

  // SENDING

  // In this section, we'll gather serial characters and
  // send them to the other node if we (1) get a carriage return,
  // or (2) the buffer is full (61 characters).

  // If there is any serial input, add it to the buffer:
  counter++;
  if(counter%2==0)
  //if (Serial.available() > 0)
  {
    //char input = Serial.read();
    char input = 'A';
    if (input != '\r') // not a carriage return
    {
      sendbuffer[sendlength] = counter;
      sendlength++;
    }

    // If the input is a carriage return, or the buffer is full:

    if ((input == '\r') || (sendlength == 61)) // CR or buffer full
    {
      // Send the packet!


      Serial.print("sending to node ");
      Serial.print(TONODEID, DEC);
      Serial.print(", message [");
      for (byte i = 0; i < sendlength; i++)
        Serial.print(sendbuffer[i]);
      Serial.println("]");

      // There are two ways to send packets. If you want
      // acknowledgements, use sendWithRetry():

      if (USEACK)
      {
        if (radio.sendWithRetry(TONODEID, sendbuffer, sendlength))
          Serial.println("ACK received!");
        else
          Serial.println("no ACK received");
      }

      // If you don't need acknowledgements, just use send():

      else // don't use ACK
      {
        Serial.println("before send");
        radio.send(TONODEID, sendbuffer, 62);
        Serial.println("sent without ack");
      }

      sendlength = 0; // reset the packet
      Blink(LED,10);
    }
  }

  // RECEIVING

  // In this section, we'll check with the RFM69HCW to see
  // if it has received any packets:

  if (radio.receiveDone()) // Got one!
  {
    // Print out the information:

    Serial.print("received from node ");
    Serial.print(radio.SENDERID, DEC);
    Serial.print(", message [");

    // The actual message is contained in the DATA array,
    // and is DATALEN bytes in size:

    for (byte i = 0; i < radio.DATALEN; i++)
      Serial.print((char)radio.DATA[i]);

    // RSSI is the "Receive Signal Strength Indicator",
    // smaller numbers mean higher power.

    Serial.print("], RSSI ");
    Serial.println(radio.RSSI);

    // Send an ACK if requested.
    // (You don't need this code if you're not using ACKs.)

    if (radio.ACKRequested())
    {
      radio.sendACK();
      Serial.println("ACK sent");
    }
    Blink(LED,10);
  }
}

void Blink(byte PIN, int DELAY_MS)
// Blink an LED for a given number of ms
{
  digitalWrite(PIN,HIGH);
  delay(DELAY_MS);
  digitalWrite(PIN,LOW);
}*/