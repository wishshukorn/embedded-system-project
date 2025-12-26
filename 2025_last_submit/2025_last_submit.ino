#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <cstdlib>
#include <esp_now.h>
#include <Arduino.h>
#include <driver/dac.h>

// WiFi Configuration
const char* ssid = "hh00";
const char* password = "helpingthe";

// API Configuration
const char* apiEndpoint = "https://nnn-llm-service.onrender.com/llm-service";

// Switch
#define SWITCH 14

//LED
#define LED 12

// I2S Configuration
#define I2S_WS 25
#define I2S_SD 33
#define I2S_SCK 32
#define I2S_PORT I2S_NUM_0

// Recording Configuration
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define RECORD_TIME 10
#define BUFFER_SIZE 1024
#define WAKE_THRESHOLD 500
// #define WAKE_THRESHOLD 200
#define MIC_GAIN 4
#define NOISE_GATE_THRESHOLD 500

#define AUDIO_OUT_PIN 26

#define VOLUME_BOOST 32
#define SLOW_FACTOR 8

// #define I2S_SPEAKER I2S_NUM_1

struct WAVHeaderSpeaker {
  char riff[4];
  uint32_t fileSize;
  char wave[4];
  char fmt[4];
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4];
  uint32_t dataSize;
};

struct WAVHeader {
  char riff[4] = { 'R', 'I', 'F', 'F' };
  uint32_t fileSize;
  char wave[4] = { 'W', 'A', 'V', 'E' };
  char fmt[4] = { 'f', 'm', 't', ' ' };
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = CHANNELS;
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  char data[4] = { 'd', 'a', 't', 'a' };
  uint32_t dataSize;
};

WebServer server(80);
int16_t buffer[BUFFER_SIZE];
bool isRecording = false;
int fileCounter = 0;

const char* mqttServer = "mqtt.netpie.io";
const int mqtt_port = 1883;
const char* mqtt_Client = "e5c92504-9b52-4e28-8474-38c03f01ef49";
const char* mqtt_username = "3QpN9SEAV7f9abvurNDYmmUMFiPRqE9C";
const char* mqtt_password = "U8Y5qGd8dKbrfytqhsN3gFRYLrdhFoyb";
volatile bool taskAlertReceived = false;

WiFiClient espClient;
PubSubClient client(espClient);
char msg[100];

typedef struct struct_message {
  float temp;
  float light;
  float humidity;
  float pm25;
}__attribute__((packed)) struct_message;

struct_message myData;


void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  

  if (len != sizeof(myData)) {
    Serial.println("Error: Data size mismatch");
    return;
  }

  memcpy(&myData, incomingData, sizeof(myData));
  

  Serial.print("Bytes received: ");
  Serial.print(len);
  Serial.print(" temp: ");
  Serial.print(myData.temp);
  Serial.print(" light: ");
  Serial.print(myData.light);
  Serial.print(" humidity: ");
  Serial.print(myData.humidity);
  Serial.print(" pm2.5: ");
  Serial.print(myData.pm25);
  Serial.println("");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  
  if (String(topic) == "@msg/task") {
    taskAlertReceived = true;
    Serial.println("Task alert received!");
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection…");
    if (client.connect(mqtt_Client, mqtt_username, mqtt_password)) {
      Serial.println("connected");
      client.subscribe("@msg/task", 1);
      Serial.println("Subscribed to @msg/task");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println("try again in 5 seconds");
      delay(5000);
    }
  }
}

void setupI2SSpeaker(uint32_t sampleRate) {
  i2s_driver_uninstall(I2S_PORT);
  delay(50);

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = sampleRate,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 128,
    .use_apll = false
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
}

void setupI2S() {
  i2s_driver_uninstall(I2S_PORT);
  delay(50);
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);

  Serial.println("I2S initialized");
}

void writeWAVHeader(File file, uint32_t dataSize) {
  WAVHeader header;
  header.dataSize = dataSize;
  header.fileSize = dataSize + sizeof(WAVHeader) - 8;

  file.write((uint8_t*)&header, sizeof(WAVHeader));
}

void recordAudio(const char* filename) {
  File audioFile = SPIFFS.open(filename, FILE_WRITE);
  if (!audioFile) {
    Serial.println("Failed to open file for writing");
    return;
  }

  Serial.println("Recording started...");
  digitalWrite(LED, HIGH);

  writeWAVHeader(audioFile, 0);

  uint32_t totalBytesWritten = 0;
  uint32_t recordingDuration = RECORD_TIME * 1000;
  uint32_t startTime = millis();

  while (millis() - startTime < recordingDuration) {
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, buffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);

    int samples = bytesRead / 2;
    for (int i = 0; i < samples; i++) {
      int16_t original = buffer[i];

       if (abs(original) < NOISE_GATE_THRESHOLD) {
          buffer[i] = 0;
          continue;
       }
        int32_t amplified = (int32_t)original * MIC_GAIN;

      if (amplified > 16000) amplified = 16000 + (amplified - 16000) / 4;
      if (amplified < -16000) amplified = -16000 + (amplified + 16000) / 4; 

      if (amplified > 32767) amplified = 32767;
      if (amplified < -32768) amplified = -32768;
      
      buffer[i] = (int16_t)amplified;
    }

    audioFile.write((uint8_t*)buffer, bytesRead);
    totalBytesWritten += bytesRead;

    if ((millis() - startTime) % 1000 < 50) {
      Serial.print(".");
    }
  }

  Serial.println("\n✓ Recording complete!");
  digitalWrite(LED, LOW);
  audioFile.seek(0);
  writeWAVHeader(audioFile, totalBytesWritten);
  audioFile.close();

  Serial.printf("💾 Saved: %s (%d KB)\n", filename, (totalBytesWritten + sizeof(WAVHeader)) / 1024);
  Serial.printf("📊 SPIFFS: %d / %d bytes used\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
}

bool sendToAPI(const char* inputFile) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return false;
  }

  File file = SPIFFS.open(inputFile, "r");
  if (!file) {
    Serial.println("Failed to open file for upload");
    return false;
  }

  int fileSize = file.size();
  Serial.println("\nUploading to API...");
  Serial.printf("File: %s (%d bytes)\n", inputFile, fileSize);

  String url = String(apiEndpoint);
  url.replace("https://", "");
  url.replace("http://", "");

  int slashIndex = url.indexOf('/');
  String host = url.substring(0, slashIndex);
  String path = url.substring(slashIndex);

  Serial.printf("Connecting to: %s\n", host.c_str());

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(host.c_str(), 443)) {
    Serial.println("Connection failed!");
    file.close();
    return false;
  }

  Serial.println("Connected!");

  String boundary = "----ESP32Boundary" + String(random(1000, 9999));

  String header = "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"audio_file\"; filename=\"";
  header += String(inputFile).substring(1);
  header += "\"\r\n";
  header += "Content-Type: audio/wav\r\n\r\n";

  String footer = "\r\n--" + boundary + "--\r\n";

  int contentLength = header.length() + fileSize + footer.length();

  client.print("POST " + path + " HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(header);

  Serial.println("Uploading file...");
  uint8_t buff[512];
  size_t totalSent = 0;

  while (file.available()) {
    size_t len = file.read(buff, sizeof(buff));
    client.write(buff, len);
    totalSent += len;

    if (totalSent % 20480 == 0) {
      Serial.printf("Sent: %d / %d bytes (%.1f%%)\r", totalSent, fileSize, (totalSent * 100.0 / fileSize));
    }
  }
  Serial.printf("Sent: %d / %d bytes (100.0%%)  \n", totalSent, fileSize);

  file.close();

  client.print(footer);
  client.flush();
  if (SPIFFS.exists(inputFile)) {
      SPIFFS.remove(inputFile);
      Serial.printf("Deleted original file: %s\n", inputFile);
      fileCounter--;
  }
  Serial.println("Waiting for response...");

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 120000) {
      Serial.println("Response timeout!");
      client.stop();
      return false;
    }
    delay(100);
  }

  String statusLine = "";
  timeout = millis();
  while (client.available() && (millis() - timeout < 5000)) {
    char c = client.read();
    statusLine += c;
    if (c == '\n') break;
  }
  Serial.println("Response: " + statusLine);

  int httpCode = 0;
  if (statusLine.indexOf("200") > 0) {
    httpCode = 200;
  } else if (statusLine.indexOf("201") > 0) {
    httpCode = 201;
  }

  int contentLen = 0;
  String currentLine = "";
  timeout = millis();

  while (client.available() && (millis() - timeout < 10000)) {
    char c = client.read();

    if (c == '\n') {
      currentLine.trim();

      if (currentLine.length() > 0) {
        Serial.println("Header: " + currentLine);
      }

      if (currentLine.startsWith("Content-Length:") || currentLine.startsWith("content-length:")) {
        contentLen = currentLine.substring(15).toInt();
        Serial.printf("Response size: %d bytes\n", contentLen);
      }

      if (currentLine.length() == 0) {
        Serial.println("Headers complete, starting download...");
        break;
      }

      currentLine = "";
      timeout = millis();
    } else if (c != '\r') {
      currentLine += c;
    }
  }

  if (httpCode != 200 && httpCode != 201) {
    Serial.printf("HTTP Error: %d\n", httpCode);
    String errorMsg = "";
    while (client.available() && errorMsg.length() < 500) {
      errorMsg += (char)client.read();
    }
    Serial.println("Error: " + errorMsg);
    for (int i=0;i<5;i++){
      digitalWrite(LED, HIGH);
      delay(500);
      digitalWrite(LED, LOW);
      delay(500);
    }
    client.stop();
    return false;
  }

  Serial.println("Success! Downloading response...");

  String responseFile = String(inputFile);
  responseFile.replace(".wav", "_response.wav");

  File outFile = SPIFFS.open(responseFile.c_str(), FILE_WRITE);
  if (!outFile) {
    Serial.println("Failed to create response file");
    client.stop();
    return false;
  }

  size_t totalReceived = 0;
  timeout = millis();
  uint8_t downloadBuff[1024];

  Serial.println("Downloading...");

  while (client.connected() || client.available()) {
    size_t available = client.available();

    if (available) {
      size_t toRead = min(available, sizeof(downloadBuff));
      size_t bytesRead = client.read(downloadBuff, toRead);

      if (bytesRead > 0) {
        outFile.write(downloadBuff, bytesRead);
        totalReceived += bytesRead;

        if (totalReceived % 20480 == 0) {
          if (contentLen > 0) {
            Serial.printf("Downloaded: %d / %d bytes (%.1f%%)\r", totalReceived, contentLen, (totalReceived * 100.0 / contentLen));
          } else {
            Serial.printf("Downloaded: %d bytes\r", totalReceived);
          }
        }

        timeout = millis();
      }
    } else {
      delay(10);
    }

    if (millis() - timeout > 10000) {
      Serial.println("\nDownload timeout");
      break;
    }

    if (contentLen > 0 && totalReceived >= contentLen) {
      break;
    }
  }

  Serial.printf("\nDownloaded: %d bytes\n", totalReceived);
  outFile.close();
  client.stop();

  if (totalReceived > 0) {
    Serial.printf("Response saved: %s\n", responseFile.c_str());
    playWav(responseFile.c_str());
    if (SPIFFS.exists(inputFile)) {
      SPIFFS.remove(inputFile);
      Serial.printf("Deleted original file: %s\n", inputFile);
      fileCounter--;
    }
    return true;
  } else {
    Serial.println("No data received");
    SPIFFS.remove(responseFile.c_str());
    return false;
  }
}

void fetchAndPlayAlert() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected!");
    return;
  }
  
  HTTPClient http;
  http.begin("https://task-alert-service.onrender.com/next_alert");
  http.setTimeout(15000);
  
  Serial.println("Fetching alert from server...");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    int len = http.getSize();
    Serial.printf("Alert received (%d bytes)\n", len);
    
    String alertFile = "/alert.wav";
    File file = SPIFFS.open(alertFile, FILE_WRITE);
    
    if (file) {
      WiFiClient* stream = http.getStreamPtr();
      uint8_t buff[512];
      int totalRead = 0;
      
      while (http.connected() && (len > 0 || len == -1)) {
        size_t available = stream->available();
        if (available) {
          int c = stream->readBytes(buff, min(available, sizeof(buff)));
          file.write(buff, c);
          totalRead += c;
          if (len > 0) len -= c;
        }
        delay(1);
      }
      
      file.close();
      Serial.printf("Alert saved: %d bytes\n", totalRead);
      
      playWav(alertFile.c_str());
      
      SPIFFS.remove(alertFile.c_str());
      
    } else {
      Serial.println("Failed to create alert file");
    }
    
  } else {
    Serial.printf("HTTP Error: %d\n", httpCode);
    if (httpCode > 0) {
      Serial.println(http.getString());
    }
  }
  
  http.end();
}

void playWav(const char* filename) {
  File file = SPIFFS.open(filename);
  if (!file) {
    Serial.println("ไม่พบไฟล์เสียง");
    return;
  }

  WAVHeader header;
  file.read((uint8_t*)&header, sizeof(WAVHeader));

  Serial.printf("เล่น: %s (Slow Factor: %d)\n", filename, SLOW_FACTOR);

  Serial.printf("SampleRate Header: %d\n", header.sampleRate);
  setupI2SSpeaker(header.sampleRate);

  int16_t buffer[64];
  int16_t outBuffer[64 * SLOW_FACTOR];

  unsigned long lastYield = millis();

  while (file.available()) {
    int bytesRead = file.read((uint8_t*)buffer, sizeof(buffer));
    int samples = bytesRead / 2;

    int outIndex = 0;
    for (int i = 0; i < samples; i++) {
      int32_t val = buffer[i];

      val = val * VOLUME_BOOST;
      if (val > 32767) val = 32767;
      if (val < -32768) val = -32768;

      uint16_t dacVal = (uint16_t)(val + 32768);

      for (int k = 0; k < SLOW_FACTOR; k++) {
        outBuffer[outIndex++] = dacVal;
      }
    }

    size_t written;
    i2s_write(I2S_PORT, outBuffer, outIndex * 2, &written, portMAX_DELAY);

    if (millis() - lastYield > 100) {
      yield();
      delay(1);
      lastYield = millis();
    }

    if (Serial.available()) break;
  }

  i2s_zero_dma_buffer(I2S_PORT);
  file.close();
  Serial.println("จบเพลง");

  String responseFile = String(filename);
  String originalFile = responseFile;

  // ลบ response
  if (SPIFFS.exists(responseFile.c_str())) {
    SPIFFS.remove(responseFile.c_str());
    Serial.printf("Deleted: %s\n", responseFile.c_str());
  }

  delay(100);
  setupI2S();
  Serial.println("Switched back to microphone mode");
}

bool detectWakeWord() {
  size_t bytesRead = 0;
  i2s_read(I2S_PORT, buffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);

  int32_t sum = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    sum += abs(buffer[i]);
  }
  int32_t avgVolume = sum / BUFFER_SIZE;

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 2000) {
    Serial.printf("Volume: %d (Threshold: %d)\n", avgVolume, WAKE_THRESHOLD);
    lastPrint = millis();
  }

  if (avgVolume > WAKE_THRESHOLD) {
    Serial.printf("Wake word detected! Volume: %d\n", avgVolume);
    return true;
  }

  return false;
}

void deleteOldestFile() {
  if (SPIFFS.usedBytes() > SPIFFS.totalBytes() * 0.8) {
    Serial.println("Storage almost full, deleting oldest...");

    char oldestFile[32];
    sprintf(oldestFile, "/rec_000.wav");

    if (SPIFFS.exists(oldestFile)) {
      SPIFFS.remove(oldestFile);
      Serial.printf("Deleted: %s\n", oldestFile);

      String responseFile = String(oldestFile);
      responseFile.replace(".wav", "_response.wav");
      if (SPIFFS.exists(responseFile.c_str())) {
        SPIFFS.remove(responseFile.c_str());
      }

      for (int i = 1; i < fileCounter; i++) {
        char oldName[32], newName[32];
        sprintf(oldName, "/rec_%03d.wav", i);
        sprintf(newName, "/rec_%03d.wav", i - 1);

        if (SPIFFS.exists(oldName)) {
          SPIFFS.rename(oldName, newName);
        }

        String oldResp = String(oldName);
        oldResp.replace(".wav", "_response.wav");
        String newResp = String(newName);
        newResp.replace(".wav", "_response.wav");

        if (SPIFFS.exists(oldResp.c_str())) {
          SPIFFS.rename(oldResp.c_str(), newResp.c_str());
        }
      }
      fileCounter--;
    }
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP32 Voice Recorder</title>";
  html += "<style>";
  html += "body { font-family: Arial; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".status { background: #e3f2fd; padding: 15px; border-radius: 5px; margin: 20px 0; }";
  html += ".file-list { margin: 20px 0; }";
  html += ".file-item { background: #f5f5f5; padding: 12px; margin: 8px 0; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; }";
  html += ".response-file { background: #e8f5e9; }";
  html += ".btn { padding: 8px 16px; margin: 5px; border: none; border-radius: 5px; cursor: pointer; text-decoration: none; display: inline-block; color: white; }";
  html += ".btn-download { background: #4CAF50; }";
  html += ".btn-delete { background: #f44336; }";
  html += ".btn-play { background: #2196F3; }";
  html += ".btn:hover { opacity: 0.8; }";
  html += ".controls { text-align: center; margin: 20px 0; }";
  html += ".info { color: #666; font-size: 14px; }";
  html += "@media (max-width: 600px) { .file-item { flex-direction: column; align-items: flex-start; } }";
  html += "</style></head><body>";

  html += "<div class='container'>";
  html += "<h1>ESP32 Voice Recorder</h1>";

  html += "<div class='status'>";
  html += "<strong>Storage:</strong> " + String(SPIFFS.usedBytes()) + " / " + String(SPIFFS.totalBytes()) + " bytes<br>";
  html += "<strong>Files:</strong> " + String(fileCounter) + " recordings<br>";
  html += "<strong>IP:</strong> " + WiFi.localIP().toString();
  html += "</div>";

  html += "<h2>Recordings</h2>";
  html += "<div class='file-list'>";

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  bool hasFiles = false;

  while (file) {
    String filename = String(file.name());
    if (filename.endsWith(".wav")) {
      hasFiles = true;
      int fileSize = file.size();
      bool isResponse = filename.indexOf("_response") >= 0;

      String itemClass = isResponse ? "file-item response-file" : "file-item";
      String fileLabel = isResponse ? "[Sound] " : "[Mic]";

      html += "<div class='" + itemClass + "'>";
      html += "<div><strong>" + fileLabel + filename + "</strong><br>";
      html += "<span class='info'>" + String(fileSize / 1024) + " KB</span></div>";
      html += "<div>";
      html += "<a href='/download?file=" + filename + "' class='btn btn-download'>⬇Download</a>";
      html += "<a href='/play?file=" + filename + "' class='btn btn-play' target='_blank'>▶Play</a>";
      html += "<a href='/delete?file=" + filename + "' class='btn btn-delete' onclick='return confirm(\"Delete this file?\")'>🗑️ Delete</a>";
      html += "</div></div>";
    }
    file = root.openNextFile();
  }

  if (!hasFiles) {
    html += "<p style='text-align:center; color:#999; padding: 30px;'>ยังไม่มีไฟล์บันทึก<br>พูดว่า 'Jarvis' เพื่อเริ่มบันทึกเสียง!</p>";
  }

  html += "</div>";

  html += "<div class='controls'>";
  html += "<a href='/deleteall' class='btn btn-delete' onclick='return confirm(\"ลบไฟล์ทั้งหมด?\")'>Delete All</a>";
  html += "<a href='/' class='btn btn-play'>Refresh</a>";
  html += "</div>";

  html += "ไฟล์จะถูกส่งไปยัง API อัตโนมัติและได้รับไฟล์ตอบกลับ</p>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleDownload() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }

  String filename = server.arg("file");

  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }

  Serial.println("Download request: " + filename);

  if (!SPIFFS.exists(filename)) {
    Serial.println("File not found: " + filename);
    server.send(404, "text/plain", "File not found: " + filename);
    return;
  }

  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("Failed to open file");
    server.send(500, "text/plain", "Failed to open file");
    return;
  }

  Serial.println("Sending file: " + filename + " (" + String(file.size()) + " bytes)");

  server.sendHeader("Content-Disposition", "attachment; filename=" + filename.substring(1));
  server.streamFile(file, "audio/wav");
  file.close();
}

void handlePlay() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }

  String filename = server.arg("file");

  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }

  Serial.println("Play request: " + filename);

  if (!SPIFFS.exists(filename)) {
    Serial.println("File not found: " + filename);
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = SPIFFS.open(filename, "r");
  if (!file) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }

  server.streamFile(file, "audio/wav");
  file.close();
}

void handleDelete() {
  if (!server.hasArg("file")) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  String filename = server.arg("file");

  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }

  if (SPIFFS.exists(filename)) {
    SPIFFS.remove(filename);
    Serial.println("Deleted: " + filename);

    // ลบ response
    String relatedFile;
    if (filename.indexOf("_response") >= 0) {
      relatedFile = filename;
      relatedFile.replace("_response.wav", ".wav");
    } else {
      relatedFile = filename;
      relatedFile.replace(".wav", "_response.wav");
    }

    if (SPIFFS.exists(relatedFile.c_str())) {
      SPIFFS.remove(relatedFile.c_str());
      Serial.println("Also deleted: " + relatedFile);
    }
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDeleteAll() {
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  int deleteCount = 0;

  while (file) {
    String filename = String(file.name());
    file.close();

    if (filename.endsWith(".wav")) {
      SPIFFS.remove(filename);
      deleteCount++;
    }

    file = root.openNextFile();
  }

  fileCounter = 0;
  Serial.printf("Deleted %d files\n", deleteCount);

  server.sendHeader("Location", "/");
  server.send(303);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/play", handlePlay);
  server.on("/delete", handleDelete);
  server.on("/deleteall", handleDeleteAll);

  server.begin();
  Serial.println("Web server started");
}

void setup() {
  pinMode(SWITCH, INPUT);
  pinMode(LED, OUTPUT);
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  Serial.println("\n╔═══════════════════════════════════╗");
  Serial.println("║  ESP32 I2S Voice Recorder v3.0    ║");
  Serial.println("║  with API Upload Feature          ║");
  Serial.println("╚═══════════════════════════════════╝\n");

  setupI2S();

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS failed!");
    while (1) delay(1000);
  }

  Serial.println("SPIFFS initialized");
  Serial.printf("Storage: %d / %d bytes\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String filename = String(file.name());
    if (filename.endsWith(".wav") && filename.indexOf("_response") < 0) {
      fileCounter++;
    }
    file = root.openNextFile();
  }
  Serial.printf("Found %d existing recordings\n", fileCounter);

  Serial.printf("\nConnecting to WiFi: %s", ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("เปิด IP นี้ในเบราว์เซอร์เพื่อดูไฟล์บันทึก");
    Serial.println("API Upload: ENABLED\n");

    setupWebServer();
  } else {
    Serial.println("\n WiFi failed! Running offline...");
    Serial.println("API Upload will be disabled\n");
  }
  Serial.println(WiFi.localIP());
    if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  esp_now_register_recv_cb(OnDataRecv);
  Serial.printf("Current threshold: %d\n", WAKE_THRESHOLD);
  Serial.println("ดูค่า Volume ที่แสดงเพื่อปรับ WAKE_THRESHOLD\n");
  client.setServer(mqttServer, mqtt_port);
  client.setCallback(mqttCallback);
  client.subscribe("@msg/task", 1);
}

int switch_status = 0;
int last_switch_status = 0;
int last_last_switch_status = 0;

void loop() {
  server.handleClient();
  int temp_switch_status = digitalRead(SWITCH);
  switch_status = temp_switch_status | last_switch_status | last_last_switch_status;
  Serial.printf("Switch: %d\n", switch_status);
  last_last_switch_status = last_switch_status;
  last_switch_status = temp_switch_status;
  if (!isRecording && switch_status) {
    if (detectWakeWord()) {
      isRecording = true;

      deleteOldestFile();

      char filename[32];
      sprintf(filename, "/rec_%03d.wav", fileCounter++);

      recordAudio(filename);

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n Starting API upload...");
        bool success = sendToAPI(filename);

        if (success) {
          Serial.println("API upload completed successfully!");
        } else {
          Serial.println("API upload failed, but file is saved locally");
        }
      } else {
        Serial.println("WiFi not connected, skipping API upload");
      }

      isRecording = false;
      Serial.println("Listening again...\n");
    }
  }
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  if (taskAlertReceived) {
    taskAlertReceived = false;
    Serial.println(" Processing task alert...");
    fetchAndPlayAlert();
  }
  double humidity = myData.humidity;
  double temperature = myData.temp;
  double lumen = myData.light;
  double pm25 = myData.pm25;
  String data = "{\"data\": {"
                "\"humidity\":"
                + String(humidity) + ","
                                     "\"temperature\":"
                + String(temperature) + ","
                                        "\"lumen\":"
                + String(lumen) + ","
                                  "\"pm2.5\":"
                + String(pm25) + "}}";
  // Serial.println(data);
  if (!(pm25 == 0 || humidity == 0 || temperature == 0 || lumen ==0)) {
    data.toCharArray(msg, (data.length() + 1));
    client.publish("@shadow/data/update", msg);
  }

  delay(1000);
}