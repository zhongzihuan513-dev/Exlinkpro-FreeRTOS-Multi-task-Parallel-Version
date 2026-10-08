#include "CST816T.h"
#include "Arduino.h"
#include <stdio.h>

#define I2C_SDA 21
#define I2C_SCL 22
#define RST_N_PIN 32
#define INT_N_PIN 33

CST816T cst816t(I2C_SDA, I2C_SCL, RST_N_PIN, INT_N_PIN); 

void setup() {
    Serial.begin(115200); 
    cst816t.begin(); 
}

TouchInfos tp;
int count = 0;
void loop() {
    count++;
    tp = cst816t.GetTouchInfo();

    // char tempString[128] = ""; 
    // sprintf(tempString, "CST816T (%d, %4d, %4d)\r", tp.touching, tp.x, tp.y);
    // Serial.printf(tempString);

    if(tp.touching)
        Serial.printf("CST816T %d (%d, %4d, %4d)\n", count, tp.touching, tp.x, tp.y);
    else
        Serial.printf("CST816T (%d)\n", count);
    delay(5);
}
