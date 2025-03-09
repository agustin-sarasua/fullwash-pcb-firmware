// #include <Wire.h>
// #include <Arduino.h>

// void setup() {
//   Serial.begin(115200);
//   while (!Serial);
  
//   Wire.begin(19, 18); // SDA=19, SCL=18
  
//   Serial.println("\nI2C Scanner");
// }

// void loop() {
//   byte error, address;
//   int deviceCount = 0;
  
//   Serial.println("Scanning...");
  
//   for (address = 1; address < 127; address++) {
//     Wire.beginTransmission(address);
//     error = Wire.endTransmission();
    
//     if (error == 0) {
//       Serial.print("I2C device found at address 0x");
//       if (address < 16) {
//         Serial.print("0");
//       }
//       Serial.print(address, HEX);
//       Serial.println(" !");
      
//       deviceCount++;
//     } else if (error == 4) {
//       Serial.print("Unknown error at address 0x");
//       if (address < 16) {
//         Serial.print("0");
//       }
//       Serial.println(address, HEX);
//     }
//   }
  
//   if (deviceCount == 0) {
//     Serial.println("No I2C devices found");
//   } else {
//     Serial.println("Done.\n");
//   }
  
//   delay(5000); // Wait 5 seconds before scanning again
// }