/*
MIT License

Copyright (c) 2024 G.Products

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

//
// Should be set to Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS).
//

/*
 * Connect the SD card to the following pins:
 *
 * SD Card | ESP32
 *    D2       -
 *    D3       SS
 *    CMD      MOSI
 *    VSS      GND
 *    VDD      3.3V
 *    CLK      SCK
 *    VSS      GND
 *    D0       MISO
 *    D1       -
 */

#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <ArduinoOTA.h>
#include <WiFi.h>

// #define ADC_LOG // Log ON // Log ON: may cause panic reset.
// #define ADC_LOG_DETAIL // Log ON: may cause panic reset.
 
#define ADC_GPIO 34  // Analog ADC1_CH6
#define ADC_AVERAGE 10
#define ADC_INTERVAL_TIME 5  //ms
#define ADC_EXT_ATT 5.3875979262     //(22 / (22 + 100)) Devided by 22k, 100kohm, Trim -3%

#define DATA_SAMPLE_TIME 100  //ms
#define DATA_SIZE 20          // number of recording data to SD card at once

#define SD_MOUNT_RETRY 5  // number of retry

#define FILENAME_SIZE 10
#define FILENAME_HEADER "/log"
#define FILENAME_FOOTER ".txt"
#define FILE_NUM_MAX 100

// Port
#define PIN_LED_STATUS (GPIO_NUM_13)
#define PIN_LED_VFREAD (GPIO_NUM_14)

TaskHandle_t thp[1];
volatile uint8_t fileReady = 0;
volatile uint8_t dataReady = 0;
volatile int adcData[2][DATA_SIZE + 1];
volatile int adcDataRaw;
volatile uint8_t pData = 0;
volatile uint32_t isrCounter = 0;
char filename[FILENAME_SIZE + 1];

hw_timer_t *timer = NULL;
volatile SemaphoreHandle_t timerSemaphore;

// WiFi Setting for OTA
bool en_ota = false;
const char *ssid = "OTA_LOG";
const char *pass = "password";
const IPAddress ip(192, 168, 20, 1);
const IPAddress subnet(255, 255, 255, 0);

void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listDir(fs, file.path(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

void createDir(fs::FS &fs, const char *path) {
  Serial.printf("Creating Dir: %s\n", path);
  if (fs.mkdir(path)) {
    Serial.println("Dir created");
  } else {
    Serial.println("mkdir failed");
  }
}

void removeDir(fs::FS &fs, const char *path) {
  Serial.printf("Removing Dir: %s\n", path);
  if (fs.rmdir(path)) {
    Serial.println("Dir removed");
  } else {
    Serial.println("rmdir failed");
  }
}

bool readFile(fs::FS &fs, const char *path) {
  Serial.printf("Reading file: %s\n", path);

  File file = fs.open(path);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return true;
  }

  Serial.print("Read from file: ");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
  return false;
}

bool checkFile(fs::FS &fs, const char *path) {
  Serial.printf("Checking file: %s\n", path);

  File file = fs.open(path);
  if (!file) {
    Serial.println("Failed to open file for reading");
    return true;
  }
  file.close();
  return false;
}

void writeFile(fs::FS &fs, const char *path, const char *message) {
  Serial.printf("Writing file: %s\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }
  if (file.print(message)) {
    Serial.println("File written");
  } else {
    Serial.println("Write failed");
  }
  file.close();
}

bool appendFile(fs::FS &fs, const char *path, const char *message) {
  Serial.printf("Appending to file: %s\n", path);

  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for appending");
    return true;
  }
  if (file.print(message)) {
    Serial.println("Message appended");
    file.close();
    return false;
  } else {
    Serial.println("Append failed");
    file.close();
    return true;
  }
}

void renameFile(fs::FS &fs, const char *path1, const char *path2) {
  Serial.printf("Renaming file %s to %s\n", path1, path2);
  if (fs.rename(path1, path2)) {
    Serial.println("File renamed");
  } else {
    Serial.println("Rename failed");
  }
}

void deleteFile(fs::FS &fs, const char *path) {
  Serial.printf("Deleting file: %s\n", path);
  if (fs.remove(path)) {
    Serial.println("File deleted");
  } else {
    Serial.println("Delete failed");
  }
}

void testFileIO(fs::FS &fs, const char *path) {
  File file = fs.open(path);
  static uint8_t buf[512];
  size_t len = 0;
  uint32_t start = millis();
  uint32_t end = start;
  if (file) {
    len = file.size();
    size_t flen = len;
    start = millis();
    while (len) {
      size_t toRead = len;
      if (toRead > 512) {
        toRead = 512;
      }
      file.read(buf, toRead);
      len -= toRead;
    }
    end = millis() - start;
    Serial.printf("%u bytes read for %u ms\n", flen, end);
    file.close();
  } else {
    Serial.println("Failed to open file for reading");
  }


  file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }

  size_t i;
  start = millis();
  for (i = 0; i < 2048; i++) {
    file.write(buf, 512);
  }
  end = millis() - start;
  Serial.printf("%u bytes written for %u ms\n", 2048 * 512, end);
  file.close();
}

void Core0a(void *args) {  //CPU(Core0) for ADC averaging
  // ADC is connected to GPIO 34 (Analog ADC1_CH6)
  long analogVolts = 0;
  int count = 0;

  while (1) {
    analogVolts += analogReadMilliVolts(ADC_GPIO);

    if (++count > ADC_AVERAGE - 1) {
      analogVolts = analogVolts / ADC_AVERAGE;
      adcDataRaw = analogVolts * ADC_EXT_ATT;
      // adcDataRaw = analogVolts;

#ifdef ADC_LOG_DETAIL
      Serial.printf("ADC millivolts value ( ave: %d ) = %d\n", count, adcDataRaw);
#endif

      count = 0;
      analogVolts = 0;
    }
    delay(ADC_INTERVAL_TIME);
  }
}

void ARDUINO_ISR_ATTR onTimer() {
  // Performing every DATA SAMPLE_TIME
  xSemaphoreGiveFromISR(timerSemaphore, NULL);

  if (++isrCounter > DATA_SIZE - 1) {
    isrCounter = 0;
    if (pData == 0) {
      pData = 1;
      // digitalWrite(PIN_LED_STATUS, HIGH);
    } else {
      pData = 0;
      dataReady = 1;
      // digitalWrite(PIN_LED_STATUS, LOW);
    }
    Serial.printf("Switched adcData = %d\n", pData);
  }

#ifdef ADC_LOG
  Serial.printf("ADC millivolts value (Sampling) = %d\n", adcDataRaw);
#endif

  adcData[pData][isrCounter] = adcDataRaw;
}

void setup() {
  Serial.begin(115200);

  // GPIO initialize for LED
  pinMode(PIN_LED_VFREAD, INPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_LED_STATUS, LOW);

  // -----------------------------------------------
  // SD Card: Access SD card
  int i = 0;
  do {
    if (!SD.begin()) {
      Serial.printf("Card Mount Failed retry=%d\n", i);
      digitalWrite(PIN_LED_STATUS, HIGH);
      delay(1000);
      digitalWrite(PIN_LED_STATUS, LOW);
    }
  } while (++i < SD_MOUNT_RETRY);

  if (!SD.begin()) {
    Serial.println("Card Mount Failed\n");
    digitalWrite(PIN_LED_STATUS, HIGH);

    // OTA MODE
    // -------------------------------------------------
    // MUST KEEP OTA CODE
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid, pass);
    IPAddress address = WiFi.softAPIP();
    Serial.println(address);
    ArduinoOTA.onStart([]() {})
      .onEnd([]() {})
      .onProgress([](unsigned int progress, unsigned int total) {})
      .onError([](ota_error_t error) {});
    ArduinoOTA.begin();
    // -------------------------------------------------
    en_ota = true;
    return;
  }

  // -----------------------------------------------
  // Timer function
  // Create semaphore to inform us when the timer has fired
  timerSemaphore = xSemaphoreCreateBinary();

  // Use 1st timer of 4 (counted from zero).
  // Set 80 divider for prescaler (see ESP32 Technical Reference Manual for more
  // info).
  timer = timerBegin(0, 80, true);

  // Attach onTimer function to our timer.
  timerAttachInterrupt(timer, &onTimer, true);

  // Set alarm to call onTimer function every second (value in microseconds).
  // Repeat the alarm (third parameter)
  timerAlarmWrite(timer, (DATA_SAMPLE_TIME * 1000), true);

  // Start an alarm
  timerAlarmEnable(timer);

  // -----------------------------------------------
  // Thread for ADC
  xTaskCreatePinnedToCore(Core0a, "Core0a", 4096, NULL, 3, &thp[0], 0);


  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);

  listDir(SD, "/", 0);

  // -----------------------------------------------
  // SD Card: Decide filename
  uint8_t levels = 0;
  String str_h = FILENAME_HEADER;
  String str_f = FILENAME_FOOTER;
  int num = 0;
  String sbuf;

  do {
    sbuf = str_h + num + str_f;
    if (num++ > FILE_NUM_MAX) {
      Serial.println("FILE_NUM_MAX Failed\n");
      digitalWrite(PIN_LED_STATUS, HIGH);
      return;
    }
    for (int i = 0; i < FILENAME_SIZE; i++) {
      filename[i] = NULL;
    }
    sbuf.toCharArray(filename, (sbuf.length() + 1));
    Serial.printf("filename = %s\n", filename);
    delay(10);
  } while (checkFile(SD, filename) == false);

  num = DATA_SAMPLE_TIME;
  sbuf = num;
  str_h = "Data[mV]_(Tsample=";
  str_f = "ms)\n";
  sbuf = str_h + num + str_f;

  char cbuf[100 + 1];
  for (int i = 0; i < 100; i++) {
    cbuf[i] = NULL;
  }

  sbuf.toCharArray(cbuf, (sbuf.length() + 1));
  Serial.printf("title = %s\n", cbuf);

  writeFile(SD, filename, cbuf);

  fileReady = 1;
  Serial.printf("SD Card has been prepared\n");

  // createDir(SD, "/mydir");
  // listDir(SD, "/", 0);
  // removeDir(SD, "/mydir");
  // listDir(SD, "/", 2);
  // readFile(SD, "/hello.txt");
  // deleteFile(SD, "/foo.txt");
  // renameFile(SD, "/hello.txt", "/foo.txt");
  // readFile(SD, "/foo.txt");
  // testFileIO(SD, "/test.txt");
  // Serial.printf("Total space: %lluMB\n", SD.totalBytes() / (1024 * 1024));
  // Serial.printf("Used space: %lluMB\n", SD.usedBytes() / (1024 * 1024));
}

void loop() {
  uint8_t p, p_old;
  String sbuf;
  char cbuf[DATA_SIZE * 10 + 1];

  while (en_ota) {
    // -------------------------------------------------
    // MUST KEEP OTA CODE
    ArduinoOTA.handle();
    // -------------------------------------------------
    delay(1);
  }

  while (1) {

    if ((fileReady == 1) && (dataReady == 1)) {
      if (pData == 0) {
        p = 1;
      } else {
        p = 0;
      }

      if (p_old != p) {
        Serial.printf("Selected adcData = %d\n", p);
        digitalWrite(PIN_LED_STATUS, HIGH);

        sbuf = "";
        for (int i = 0; i < DATA_SIZE; i++) {
          sbuf = sbuf + adcData[p][i] + "\n";
        }

        for (int i = 0; i < DATA_SIZE * 10; i++) {
          cbuf[i] = NULL;
        }

        sbuf.toCharArray(cbuf, (sbuf.length() + 1));
        if (appendFile(SD, filename, cbuf)) {
          Serial.println("Failed to write");
          digitalWrite(PIN_LED_STATUS, HIGH);
          return;
        }

        p_old = p;
        Serial.printf("write data ------------- \n%s\n", cbuf);
        Serial.printf("write data end --------- \n");
        Serial.printf("write data num=: %d\n", sbuf.length());
        delay(50);
        digitalWrite(PIN_LED_STATUS, LOW);
      }
    }
    delay(100);
  }
}
