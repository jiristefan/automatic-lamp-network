#include <ESP8266WiFi.h>
#include <RFM69.h>
#include <SPI.h>

#define NODE_ID       1                  // 1 for gateway, other integers for nodes
#define NETWORK_ID    76                 // the same on all nodes that talk to each other
#define FREQUENCY     RF69_868MHZ
#define ENCRYPT_KEY   "3m0I0kubJa88BMjR" // exactly the same 16 characters/bytes on all nodes!
#define SERIAL_BAUD   9600

RFM69 radio(D8, D1, false, nullptr);

void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  delay(10);

  WiFi.mode(WIFI_OFF);
  delay(10);
  
  radio.initialize(FREQUENCY, NODE_ID, NETWORK_ID);
  radio.encrypt(ENCRYPT_KEY);
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
  // Check if something was received (could be an interrupt from the radio)
  if(radio.receiveDone())
  {
    //print message received to serial
    Serial.print('[');
    Serial.print(radio.SENDERID);
    Serial.print("] ");
    Serial.print("[RX_RSSI: ");
    Serial.print(radio.RSSI);
    Serial.print("] ");
    Serial.println((char*) radio.DATA);
    
    //check if sender wanted an ACK
    if(radio.ACKRequested())
    {
      radio.sendACK();
      Serial.println("ACK sent");
    }
  }
  Serial.flush();
  delay(100);
}
