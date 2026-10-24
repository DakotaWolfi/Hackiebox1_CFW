/* Toniebox Brain Test v0.7: ESP32-S3 + CC3200 motherboard.
   Codec register sequence adapted from Oaradla/Hackiebox1_CFW src/codec.c,
   GPL-3.0-or-later. Arduino ESP32 core 3.x required. */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "TonieboxTypes.h"

#define SDA_PIN 5
#define SCL_PIN 6
#define POWER_PIN 15
#define RESET_PIN 16
#define RED_PIN 17
#define GREEN_PIN 18
#define BLUE_PIN 40
#define AUDIO_DATA 10
#define AUDIO_BCLK 11
#define AUDIO_WS 12
#define DAC 0x18
#define NFC_SCK 39
#define NFC_MISO 38
#define NFC_MOSI 2
#define NFC_CS 1
#define NFC_IRQ 13
#define STORAGE_EN 21 // active LOW: original CC3200 pin 58
#define SD_CLK_PIN 41  // old CC3200 pin 1
#define SD_CMD_PIN 42  // old CC3200 pin 2
#define SD_D0_PIN 47   // old CC3200 pin 64
#define EAR_BIG_PIN 7    // old CC3200 pin 57, active LOW
#define EAR_SMALL_PIN 8  // old CC3200 pin 59, active LOW

int currentPage=-1;
i2s_chan_handle_t tx=nullptr;
bool audioReady=false;
bool nfcReady=false;
uint32_t nfcHz=2000000;
SPIClass nfcSPI(FSPI);

bool rawWrite(uint8_t reg,uint8_t value){
  Wire.beginTransmission(DAC); Wire.write(reg); Wire.write(value);
  int err=Wire.endTransmission();
  if(err)Serial.printf("I2C write 0x%02X error %d\n",reg,err);
  return err==0;
}
bool wr(int pg,int reg,int value){
  if(pg!=currentPage){if(!rawWrite(0,pg)){currentPage=-1;return false;}currentPage=pg;}
  return rawWrite(reg,value);
}
bool sequence(const Reg* data,int n){
  for(int i=0;i<n;i++){
    if(!wr(data[i].p,data[i].r,data[i].v)){
      Serial.printf("Codec configuration failed at page %u reg 0x%02X\n",data[i].p,data[i].r);
      return false;
    }
  }
  return true;
}
#define RUN(a) if(!sequence(a,sizeof(a)/sizeof(a[0])))return false
bool initCodec(){
  currentPage=-1;
  const Reg a[]={ {0,1,1},{0,4,7},{0,6,0x20},{0,7,0},{0,8,0},
    {0,5,0x96},{0,0x0B,0x84},{0,0x0C,0x86},{0,0x0D,1},{0,0x0E,0} };
  const Reg b[]={ {0,0x1B,0},{0,0x3C,0x19},{1,0x21,0x4E},
    {1,0x22,0x70},{1,0x23,0x44},{1,0x2E,0x0B},{1,0x2C,0xE0},
    {3,0x10,1},{0,0x43,0x8C},{0,0x30,0x80},{0,0x33,0x14} };
  const Reg off1[]={ {1,0x24,0x7F},{1,0x25,0x7F},{1,0x26,0x7F} };
  const Reg off2[]={ {1,0x28,0},{1,0x29,0},{1,0x2A,0} };
  const Reg off3[]={ {1,0x1F,0},{1,0x20,0} };
  const Reg speaker[]={ {1,0x2A,4},{1,0x20,0x86},{1,0x26,0x92} };
  RUN(a);delay(10);RUN(b);delay(200);
  RUN(off1);delay(50);RUN(off2);delay(50);RUN(off3);delay(50);
  RUN(speaker);delay(50);
  if(!wr(0,0x3F,0xFD))return false;
  // -25 dB DAC channel gain, remain muted until test tone.
  if(!wr(0,65,0xCE)||!wr(0,66,0xCE)||!wr(0,64,0x0C))return false;
  Serial.println("Codec initialization: PASS (speaker, muted, -25 dB)");
  return true;
}
bool initI2S(){
  i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
  channel.dma_desc_num=8;channel.dma_frame_num=256;
  esp_err_t err=i2s_new_channel(&channel,&tx,nullptr);
  if(err!=ESP_OK){Serial.printf("i2s_new_channel error %d\n",err);return false;}
  i2s_std_config_t c={
    .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
    .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=(gpio_num_t)AUDIO_BCLK,
      .ws=(gpio_num_t)AUDIO_WS,.dout=(gpio_num_t)AUDIO_DATA,
      .din=I2S_GPIO_UNUSED,.invert_flags={false,false,false}}
  };
  err=i2s_channel_init_std_mode(tx,&c);
  if(err==ESP_OK)err=i2s_channel_enable(tx);
  Serial.printf("I2S: %s (error %d)\n",err==ESP_OK?"PASS":"FAIL",err);
  return err==ESP_OK;
}
void playTone(int hz,int milliseconds){
  if(!audioReady)return;
  Serial.printf("Tone %d Hz, %d ms\n",hz,milliseconds);
  if(!wr(0,64,0x00))return;
  int16_t frames[256*2];
  int total=16000*milliseconds/1000;
  double phase=0,delta=2.0*PI*hz/16000.0;
  for(int base=0;base<total;base+=256){
    int n=min(256,total-base);
    for(int i=0;i<n;i++){
      int16_t sample=(int16_t)(sin(phase)*3500.0);
      frames[2*i]=frames[2*i+1]=sample;
      phase+=delta;if(phase>=2*PI)phase-=2*PI;
    }
    size_t written=0;
    esp_err_t e=i2s_channel_write(tx,frames,n*4,&written,1000);
    if(e!=ESP_OK||written!=(size_t)(n*4)){
      Serial.printf("I2S write error %d, %u bytes\n",e,(unsigned)written);break;
    }
  }
  int16_t zeros[256*2]={0};
  for(int i=0;i<5;i++){size_t done=0;i2s_channel_write(tx,zeros,sizeof(zeros),&done,1000);}
  delay(40);wr(0,64,0x0C);
  Serial.println("Muted");
}
void leds(bool r,bool g,bool b){digitalWrite(RED_PIN,r);digitalWrite(GREEN_PIN,g);digitalWrite(BLUE_PIN,b);}
// Debounced button state. Detection and release both logged; tone only on press.
struct ButtonState { int gpio; const char *name; int tone; bool stable; bool sampled; unsigned long changed; };
ButtonState ears[2]={
  {EAR_BIG_PIN,"BIG EAR",660,false,false,0},
  {EAR_SMALL_PIN,"SMALL EAR",330,false,false,0}
};
void pollButtons(){
  for(auto &b: ears){
    bool pressed=(digitalRead(b.gpio)==LOW);
    if(pressed!=b.sampled){b.sampled=pressed;b.changed=millis();}
    if(pressed!=b.stable && millis()-b.changed>=30){
      b.stable=pressed;
      Serial.printf("%s %s (GPIO%d)\n",b.name,pressed?"PRESSED":"released",b.gpio);
      if(pressed){
        if(b.gpio==EAR_BIG_PIN)leds(false,true,false);
        else leds(false,false,true);
        playTone(b.tone,120);
        leds(false,false,false);
      }
    }
  }
}

// TRF7970A SPI protocol adapted from Hackiebox1_CFW/src/nfc.c (GPL-3.0-or-later).
// Phase 0 for commands and writes; phase 1 for reading returned bytes.
void nfcFrameBegin(){digitalWrite(NFC_CS,LOW);}
void nfcFrameEnd(){digitalWrite(NFC_CS,HIGH);}
uint8_t nfcByte(uint8_t b,uint8_t mode=SPI_MODE0){
  nfcSPI.beginTransaction(SPISettings(nfcHz,MSBFIRST,mode));
  uint8_t r=nfcSPI.transfer(b);
  nfcSPI.endTransaction();return r;
}
void nfcWrite(uint8_t reg,uint8_t val){
  nfcFrameBegin();nfcByte(reg);nfcByte(val);nfcFrameEnd();
}
uint8_t nfcRead(uint8_t reg){
  nfcFrameBegin();nfcByte(0x40|reg);
  uint8_t v=nfcByte(0xFF,SPI_MODE1);nfcFrameEnd();return v;
}
void nfcCommand(uint8_t cmd){
  nfcFrameBegin();nfcByte(0x80|cmd);
  if(cmd!=0x10)nfcByte(0);nfcFrameEnd();
}
uint8_t nfcReadIRQ(){
  nfcFrameBegin();nfcByte(0x40|0x20|0x0C);
  uint8_t st=nfcByte(0xFF,SPI_MODE1);
  (void)nfcByte(0xFF,SPI_MODE1);nfcFrameEnd();return st;
}
uint16_t crcStep(uint16_t crc,uint8_t b){
  crc^=b;for(int k=0;k<8;k++)crc=(crc&1)?(crc>>1)^0x8408:(crc>>1);
  return crc;
}
// Non-destructive SPI diagnostics. Does NOT reset the motherboard, cycle SD power,
// format, mount or modify a card. Register 0x0D is restored to the original value.
bool nfcProbeOnce(const char *label){
  uint8_t before=nfcRead(0x0D);
  uint8_t st=nfcRead(0x00), iso=nfcRead(0x01);
  const uint8_t probe=0x3E;
  nfcWrite(0x0D,probe);
  delay(2);
  uint8_t after=nfcRead(0x0D);
  nfcWrite(0x0D,before);
  delay(2);
  uint8_t restored=nfcRead(0x0D);
  bool pass=after==probe && restored==before;
  Serial.printf("[%s] Hz=%lu CS=%d IRQ=%d STATUS=%02X ISO=%02X MASK %02X -> %02X -> %02X  %s\\n",
    label,(unsigned long)nfcHz,digitalRead(NFC_CS),digitalRead(NFC_IRQ),
    st,iso,before,after,restored,pass?"PASS":"FAIL");
  return pass;
}
bool nfcStart(){
  pinMode(STORAGE_EN,OUTPUT);digitalWrite(STORAGE_EN,LOW);
  pinMode(NFC_CS,OUTPUT);digitalWrite(NFC_CS,HIGH);
  pinMode(NFC_IRQ,INPUT);
  delay(500);
  nfcSPI.begin(NFC_SCK,NFC_MISO,NFC_MOSI,-1);
  Serial.println("\\n=== NFC DIAGNOSTICS v0.7 (no SD writes) ===");
  Serial.printf("GPIO: storage_EN=%d CS=%d IRQ=%d reset=%d peri_power=%d\\n",
    digitalRead(STORAGE_EN),digitalRead(NFC_CS),digitalRead(NFC_IRQ),
    digitalRead(RESET_PIN),digitalRead(POWER_PIN));
  bool pass=false;
  // Attempt baseline before changing NFC state
  nfcHz=2000000;
  pass=nfcProbeOnce("as found");
  // The same soft-init + idle sequence as the previously successful v0.4 test.
  if(!pass){
    nfcCommand(0x03);
    nfcCommand(0x00);
    delay(50);
    pass=nfcProbeOnce("soft init");
  }
  if(!pass){
    const uint32_t speeds[]={1000000,250000,100000};
    for(uint32_t hz:speeds){
      nfcHz=hz;
      delay(100);
      if(nfcProbeOnce("slower SPI")){pass=true;break;}
    }
  }
  if(!pass){
    Serial.println("NFC not responding. RF stays OFF. Send NFC to retry without resetting ESP.");
    return false;
  }
  Serial.println("NFC SPI access PASS. Enabling ISO15693 field.");
  nfcWrite(0x00,0x21);nfcWrite(0x01,0x82);nfcWrite(0x0D,0x3E);
  nfcWrite(0x09,0x21);nfcWrite(0x06,0x80);nfcWrite(0x07,0x14);
  nfcWrite(0x08,0x1F);nfcWrite(0x0A,0x00);
  nfcWrite(0x10,nfcRead(0x10)|0x10);
  delay(10);
  Serial.println("RF enabled. Present a tag.");
  return true;
}
// Raw ISO15693 INVENTORY, command 26 01 00 plus CRC. Return a UID when read.
bool nfcInventory(uint8_t uid[8]){
  const uint8_t req[3]={0x26,0x01,0x00};
  uint16_t crc=0xFFFF;
  for(uint8_t b:req)crc=crcStep(crc,b);
  crc=~crc;
  uint8_t frame[5]={req[0],req[1],req[2],(uint8_t)crc,(uint8_t)(crc>>8)};
  (void)nfcReadIRQ();
  nfcFrameBegin();
  nfcByte(0x8F); // reset FIFO
  nfcByte(0x90); // transmit without hardware CRC
  nfcByte(0x3D); // continuous write starting at TX length register
  nfcByte(0x00);nfcByte(0x50); // five bytes, full bytes
  for(uint8_t b:frame)nfcByte(b);
  nfcFrameEnd();
  uint8_t irq=0;
  bool rx=false;
  for(int i=0;i<15;i++){
    unsigned long start=millis();
    while(digitalRead(NFC_IRQ)==LOW && millis()-start<12)delay(1);
    if(digitalRead(NFC_IRQ)==LOW)break;
    irq=nfcReadIRQ();
    if(irq&0x1F){Serial.printf("NFC IRQ error: %02X\n",irq);return false;}
    if(irq&0x40){rx=true;break;}
  }
  if(!rx)return false;
  uint8_t resp[12];
  nfcFrameBegin();nfcByte(0x40|0x20|0x1F);
  for(uint8_t &b:resp)b=nfcByte(0xFF,SPI_MODE1);
  nfcFrameEnd();
  crc=0xFFFF;int end=-1;
  for(int i=0;i<12;i++){crc=crcStep(crc,resp[i]);if(i>=2&&crc==0xF0B8){end=i+1;break;}}
  if(end!=12 || (resp[0]&1)){Serial.printf("NFC response CRC/length unexpected (end=%d):",end);for(uint8_t b:resp)Serial.printf(" %02X",b);Serial.println();return false;}
  memcpy(uid,resp+2,8);return true;
}
void nfcPoll(){
  static uint32_t next=0;static uint8_t last[8]={0};static bool seen=false;
  if(!nfcReady||millis()<next)return;next=millis()+350;
  uint8_t uid[8];
  if(nfcInventory(uid)){
    if(!seen||memcmp(uid,last,8)!=0){
      Serial.print("ISO15693 UID (RF transmission order): ");
      for(int i=0;i<8;i++)Serial.printf("%02X%s",uid[i],i==7?"":" ");Serial.println();
      memcpy(last,uid,8);seen=true;leds(false,true,true);
      // Avoid an audio beep here, so NFC polling stays timely.
    }
  }else if(seen){seen=false;leds(false,false,false);Serial.println("Tag removed or inventory missed");}
}

void setup(){
  Serial.begin(115200);delay(1500);
  Serial.println("\n=== TONIEBOX BRAIN TRANSPLANT v0.7 - NFC SPI DIAGNOSTICS ===");
  pinMode(RED_PIN,OUTPUT);pinMode(GREEN_PIN,OUTPUT);pinMode(BLUE_PIN,OUTPUT);
  leds(false,false,false);
  pinMode(EAR_BIG_PIN,INPUT_PULLUP);
  pinMode(EAR_SMALL_PIN,INPUT_PULLUP);
  for(auto &b: ears){b.stable=b.sampled=(digitalRead(b.gpio)==LOW);b.changed=millis();
    Serial.printf("%s at startup: %s\n",b.name,b.stable?"PRESSED (LOW)":"released (HIGH)");}
  pinMode(RESET_PIN,OUTPUT);digitalWrite(RESET_PIN,LOW);
  pinMode(POWER_PIN,OUTPUT);digitalWrite(POWER_PIN,HIGH);
  delay(30);digitalWrite(RESET_PIN,HIGH);delay(50);
  Wire.begin(SDA_PIN,SCL_PIN,100000);Wire.setTimeOut(100);
  int found=0;
  Serial.println("I2C scan:");
  for(int a=8;a<120;a++){
    Wire.beginTransmission(a);
    if(Wire.endTransmission()==0){Serial.printf(" ACK 0x%02X\n",a);found++;}
  }
  Serial.printf("Found %d devices\n",found);
  if(!initCodec()){Serial.println("Codec FAILED - no tone");}
  else audioReady=initI2S();
  nfcReady=nfcStart();
  Serial.println("Commands: NFC (retry diagnostics), STATUS (GPIO levels). SD not accessed.");
}
void loop(){
  if(Serial.available()){
    String command=Serial.readStringUntil('\n');
    command.trim();
    if(command=="NFC")nfcReady=nfcStart();
    else if(command=="STATUS"){
      Serial.printf("GPIO status: storage=%d CS=%d IRQ=%d reset=%d power=%d ears=%d/%d\\n",
        digitalRead(STORAGE_EN),digitalRead(NFC_CS),digitalRead(NFC_IRQ),
        digitalRead(RESET_PIN),digitalRead(POWER_PIN),
        digitalRead(EAR_BIG_PIN),digitalRead(EAR_SMALL_PIN));
    }
    else if(command.length())Serial.println("Commands: NFC, STATUS");
  }
  pollButtons();
  nfcPoll();
  delay(5);
}
