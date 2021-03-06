#include <Ethernet3.h>
#include <EthernetUdp3.h> 

#include <avr/io.h>
#include <avr/interrupt.h>

/* Original sketch available at:
 * https://hackaday.io/project/18126-dav5-v301-raman-spectrometer/log/53099-using-an-arduino-r3-to-power-the-tcd1304ap-ccd-chip
 * 
 * TCD1304      - Teensy 3.6
 * ------------------------
 * pin 3 (ICG)  - pin 6 (D4)
 * pin 4 (MCLK) - pin 7 (D2)
 * pin 5 (SH)   - pin 8 (D3)
 */
#include <ADC.h>
#include <ADC_util.h>
#include <SPI.h>       

#define PIXELS 3648 // total number of data samples including dummy outputs
#define N 50 // number of subdivisions of framerate to expose for (electronic shutter)
#define CLOCK GPIO9_DR  // use output of GPIOB on Teensy 3.6

#define UDP_TX_PACKET_MAX_SIZE 3*PIXELS/2 // redefine max packet size

#define ICG (1<<4)  // 4 (TCD1304 pin 3, teensy pin 2)
#define MCLK (1<<5) // 5 (TCD1304 pin 4, teensy pin 3)
#define SH (1<<6)   // 6 (TCD1304 pin 5, teensy pin 4)  

#define F 4// clock rate in MHz, 0.5 1 2 or 4 should work..

IntervalTimer frameSampler;
IntervalTimer CCDsampler;
ADC *adc = new ADC(); // adc object

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(169, 254, 1, 1);
IPAddress subnet(255, 255, 0, 0);
EthernetUDP Udp;
unsigned int localPort = 8888;
char packetBuffer[UDP_TX_PACKET_MAX_SIZE]; //buffer to hold incoming packet,

void setup(){
  float uspf = (float) (6 + (32+PIXELS+14)* 4.0/((float) F));
  Serial.print("us per frame: ");
  Serial.println(uspf);
  
    // Enable the serial port.
  Serial.begin(921600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
  
  pinMode(9, OUTPUT); // ethernet board reset pin
  digitalWrite(9, HIGH);
  delayMicroseconds(1000);
  digitalWrite(9, LOW);
  delayMicroseconds(1000);
  digitalWrite(9, HIGH);

  pinMode(10, OUTPUT);
  Ethernet.setCsPin(10); 
  Ethernet.init(1); // only initialize with 1 socket with 16k memory
  Ethernet.begin(mac, ip, subnet);
  //Ethernet.setSubnetMask(subnet); // teensy and rpi should have same subnet mask
  Udp.begin(localPort);

/*
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println("Ethernet shield was not found.");
  }
  else if (Ethernet.hardwareStatus() == EthernetW5100) {
    Serial.println("W5100 Ethernet controller detected.");
  }
  else if (Ethernet.hardwareStatus() == EthernetW5200) {
    Serial.println("W5200 Ethernet controller detected.");
  }
  else if (Ethernet.hardwareStatus() == EthernetW5500) {
    Serial.println("W5500 Ethernet controller detected.");
  }
*/
  Serial.print("max packet size [bytes]: ");
  Serial.println(UDP_TX_PACKET_MAX_SIZE);

  int packetSize = 0;
  while (packetSize == 0)
  {
    //Serial.println("checking for packet!");
    packetSize = Udp.parsePacket(); // wait
  }
  if (packetSize) 
  {
    Serial.print("Received packet of size ");
    Serial.println(packetSize);
    Serial.print("From ");
    for (int i =0; i < 4; i++)
    {
      Serial.print(Udp.remoteIP()[i], DEC);
      if (i < 3)
      {
        Serial.print(".");
      }
    }
    Serial.print(", port ");
    Serial.println(Udp.remotePort());

    // read the packet into packetBufffer
    Udp.read(packetBuffer,UDP_TX_PACKET_MAX_SIZE);
    Serial.println("Contents:");
    Serial.println(packetBuffer);
  }

  //Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
  //Udp.write("hello");
  //Udp.endPacket();
  
  // set clock pins to output
  pinMode(2, OUTPUT);
  pinMode(3, OUTPUT);
  pinMode(4, OUTPUT);
  CLOCK |= ICG; // Set the integration clear gate high.

  // generate clock frequency of 1MHz on teensy pin 7
  analogWriteFrequency(3, F*1000000);
  analogWrite(3, 124);

  // make ADC very fast
  adc->adc0->setAveraging(1);  
  adc->adc0->setResolution(12);
  adc->adc0->setConversionSpeed(ADC_CONVERSION_SPEED::VERY_HIGH_SPEED);
  adc->adc0->setSamplingSpeed(ADC_SAMPLING_SPEED::VERY_HIGH_SPEED);
  adc->adc0->startContinuous(A0);   

  // to get longer exposure times, set N = 1 and add some extra time 
  CCDsampler.priority(0); // needs high priority to execute more determinisitically?
  frameSampler.priority(1); 

  attachInterrupt(digitalPinToInterrupt(1), beginCCDsampler, RISING);
  
  frameSampler.begin(triggerCCD, uspf / (float) N + 0.0);
  Serial.println("exiting setup");
}

int c = 0;

void beginCCDsampler(){
  CCDsampler.begin(sampleCCD, 4 / F);
}

void triggerCCD(){
  // this routine sends the message to the CCD to start sending data. CCD will begin when ICG goes high
  bool t = (c == 0);
  if (t) CLOCK &= ~ICG; // set ICG low
  delayMicroseconds(1); // timing requirement (1000 ns max)
  CLOCK |= SH;   // set SH high 
  delayMicroseconds(1); // timing requirement (1000 ns max)
  CLOCK &= ~SH;  // set SH low
  if (t) {
    // it appears to take about 5us before the first ADC sample happens, but this appears to be related to the 5us delay..
    delayMicroseconds(4); // timing requirement (min 1000ns, typ 5000 ns); making this the same as (or a multiple of?) the ADC sample period seems to best align the first ADC sample with the rising edge of ICD; adding the delay seems to block the timer from executing
    CLOCK |= ICG;  // set ICG high
    //CCDsampler.begin(sampleCCD, 4 / F); // with higher priority, this happens at right time..?
  }
  c++;
  if (c == N) c = 0; 
}

byte vals[2][3*PIXELS/2];
//uint16_t vals[PIXELS][2];
int i = 0;
int j = 0;
int f = 0;
int ii = 0;

uint16_t sample = 0;
uint16_t sample_prev = 0;

void sampleCCD(){
  // wait for conversion and sample ccd
  sample_prev = sample;
  while (!adc->adc0->isComplete()); // wait for conversion
  sample = (uint16_t)adc->adc0->analogReadContinuous();
  if ((i > 32) && (i%2 == 1)) { // only odd indices

    // bit shift and stuff to fit 2 samples into 3 bytes, MSB first
    vals[j][ii] = highByte(sample_prev) << 4 | lowByte(sample_prev) >> 4;
    vals[j][ii+1] = lowByte(sample_prev) << 4 | highByte(sample);
    vals[j][ii+2] = lowByte(sample);
    ii += 3;
    if (ii >= (3*PIXELS/2)){ // stop sampling before dummy outputs
      CCDsampler.end();
      i = 0;
      ii = 0;
      // probably trigger a switch to the other array to prevent overwriting, and trigger a send or something
      j = !j;  
      f = 1; 
    }
  }
  i += 1;
}

byte (*packet)[3*PIXELS/2];

void loop(){
  if (f == 1){
    Serial.println("sending frame!");
    f = 0;
    int ts = micros();
    packet = &vals[!j];
    Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
    Udp.write(*packet, 3*PIXELS/2);
    Udp.endPacket();
    int tf = micros();
    Serial.print("elapsed time: ");
    Serial.println(tf - ts);
    
  }
}
