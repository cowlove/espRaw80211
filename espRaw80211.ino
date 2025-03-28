#include <Adafruit_GFX.h>               // Core graphics library
#include <Adafruit_ST7735.h>            // Hardware-specific library

#define TFT_CS 16
#define TFT_RST 9  
#define TFT_DC 17
#define TFT_SCLK 5   
#define TFT_MOSI 23  
#define ST7735

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

void setup() {
    pinMode(27,OUTPUT); 		//Backlight:27  TODO:JIM 27 appears to be an external pin 
    digitalWrite(27,HIGH);		//New version added to backlight control
    printf("%d\n", __LINE__); 
    tft.initR(INITR_18GREENTAB);                             // 1.44 v2.1
    printf("%d\n", __LINE__); 
    tft.fillScreen(ST7735_BLACK);                            
    tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);          
    tft.setRotation(1);
    tft.setTextSize(2); 
}

void loop() {
    delay(100);
    tft.setCursor(0, 0);
    tft.print("HELLO");
    printf("loop()\n");
}
