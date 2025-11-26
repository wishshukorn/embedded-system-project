#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <cstdlib>

// WiFi Configuration
const char* ssid = "hh00";
const char* password = "helpingthe";

// API Configuration
const char* apiEndpoint = "https://nnn-llm-service.onrender.com/llm-service";

// I2S Configuration
#define I2S_WS 25
#define I2S_SD 33
#define I2S_SCK 32
#define I2S_PORT I2S_NUM_0

// Recording Configuration
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define RECORD_TIME 15
#define BUFFER_SIZE 1024
#define WAKE_THRESHOLD 1500

struct WAVHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t fileSize;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = CHANNELS;
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize;
};

WebServer server(80);
int16_t buffer[BUFFER_SIZE];
bool isRecording = false;
int fileCounter = 0;

const char *mqttServer = "mqtt.netpie.io";
const int mqtt_port = 1883;
const char *mqtt_Client = "e5c92504-9b52-4e28-8474-38c03f01ef49";
const char *mqtt_username = "3QpN9SEAV7f9abvurNDYmmUMFiPRqE9C";
const char *mqtt_password = "U8Y5qGd8dKbrfytqhsN3gFRYLrdhFoyb";

WiFiClient espClient;
PubSubClient client(espClient);
char msg[100];

void reconnect()
{
  while (!client.connected())
  {
    Serial.print("Attempting MQTT connection…");
    if (client.connect(mqtt_Client, mqtt_username, mqtt_password))
    {
      Serial.println("connected");
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println("try again in 5 seconds");
      delay(5000);
    }
  }
}

void setupI2S() {
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
  
  Serial.println("✓ I2S initialized");
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
    Serial.println("❌ Failed to open file for writing");
    return;
  }

  Serial.println("🔴 Recording started...");
  
  writeWAVHeader(audioFile, 0);
  
  uint32_t totalBytesWritten = 0;
  uint32_t recordingDuration = RECORD_TIME * 1000;
  uint32_t startTime = millis();
  
  while (millis() - startTime < recordingDuration) {
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, buffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);
    
    audioFile.write((uint8_t*)buffer, bytesRead);
    totalBytesWritten += bytesRead;
    
    if ((millis() - startTime) % 1000 < 50) {
      Serial.print(".");
    }
  }
  
  Serial.println("\n✓ Recording complete!");
  
  audioFile.seek(0);
  writeWAVHeader(audioFile, totalBytesWritten);
  audioFile.close();
  
  Serial.printf("💾 Saved: %s (%d KB)\n", filename, (totalBytesWritten + sizeof(WAVHeader)) / 1024);
  Serial.printf("📊 SPIFFS: %d / %d bytes used\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
}

// ฟังก์ชันส่งไฟล์ไปยัง API และรับไฟล์กลับมา
bool sendToAPI(const char* inputFile) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected!");
    return false;
  }

  File file = SPIFFS.open(inputFile, "r");
  if (!file) {
    Serial.println("❌ Failed to open file for upload");
    return false;
  }

  int fileSize = file.size();
  Serial.println("\n📤 Uploading to API...");
  Serial.printf("📁 File: %s (%d bytes)\n", inputFile, fileSize);

  // แยก URL เป็น host และ path
  String url = String(apiEndpoint);
  url.replace("https://", "");
  url.replace("http://", "");
  
  int slashIndex = url.indexOf('/');
  String host = url.substring(0, slashIndex);
  String path = url.substring(slashIndex);
  
  Serial.printf("🌐 Connecting to: %s\n", host.c_str());

  WiFiClientSecure client;
  client.setInsecure(); // ข้าม SSL verification (ใช้เฉพาะ development)
  
  if (!client.connect(host.c_str(), 443)) {
    Serial.println("❌ Connection failed!");
    file.close();
    return false;
  }
  
  Serial.println("✓ Connected!");
  
  // สร้าง boundary
  String boundary = "----ESP32Boundary" + String(random(1000, 9999));
  
  // สร้าง header และ footer
  String header = "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"audio_file\"; filename=\"";
  header += String(inputFile).substring(1);
  header += "\"\r\n";
  header += "Content-Type: audio/wav\r\n\r\n";
  
  String footer = "\r\n--" + boundary + "--\r\n";
  
  int contentLength = header.length() + fileSize + footer.length();
  
  // ส่ง HTTP headers
  client.print("POST " + path + " HTTP/1.1\r\n");
  client.print("Host: " + host + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n\r\n");
  
  // ส่ง multipart header
  client.print(header);
  
  // ส่งไฟล์ทีละ chunk
  Serial.println("📤 Uploading file...");
  uint8_t buff[512];
  size_t totalSent = 0;
  
  while (file.available()) {
    size_t len = file.read(buff, sizeof(buff));
    client.write(buff, len);
    totalSent += len;
    
    if (totalSent % 20480 == 0) { // แสดงทุก 20KB
      Serial.printf("📤 Sent: %d / %d bytes (%.1f%%)\r", totalSent, fileSize, (totalSent * 100.0 / fileSize));
    }
  }
  Serial.printf("📤 Sent: %d / %d bytes (100.0%%)  \n", totalSent, fileSize);
  
  file.close();
  
  // ส่ง footer
  client.print(footer);
  client.flush();
  
  Serial.println("⏳ Waiting for response...");
  
  // รอ response header (เพิ่มเวลารอเป็น 60 วินาที เพราะ API อาจจะประมวลผลนาน)
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 60000) {
      Serial.println("❌ Response timeout!");
      client.stop();
      return false;
    }
    delay(100);
  }
  
  // อ่าน HTTP response headers
  String statusLine = "";
  timeout = millis();
  while (client.available() && (millis() - timeout < 5000)) {
    char c = client.read();
    statusLine += c;
    if (c == '\n') break;
  }
  Serial.println("📡 Response: " + statusLine);
  
  int httpCode = 0;
  if (statusLine.indexOf("200") > 0) {
    httpCode = 200;
  } else if (statusLine.indexOf("201") > 0) {
    httpCode = 201;
  }
  
  // อ่านและข้าม headers
  int contentLen = 0;
  String currentLine = "";
  timeout = millis();
  
  while (client.available() && (millis() - timeout < 10000)) {
    char c = client.read();
    
    if (c == '\n') {
      currentLine.trim();
      
      // Debug: แสดง header ทุกบรรทัด
      if (currentLine.length() > 0) {
        Serial.println("Header: " + currentLine);
      }
      
      if (currentLine.startsWith("Content-Length:") || currentLine.startsWith("content-length:")) {
        contentLen = currentLine.substring(15).toInt();
        Serial.printf("📦 Response size: %d bytes\n", contentLen);
      }
      
      if (currentLine.length() == 0) {
        // Headers จบแล้ว (เจอบรรทัดว่าง)
        Serial.println("✓ Headers complete, starting download...");
        break;
      }
      
      currentLine = "";
      timeout = millis(); // reset timeout
    } else if (c != '\r') {
      currentLine += c;
    }
  }
  
  if (httpCode != 200 && httpCode != 201) {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
    // อ่านและแสดง error message
    String errorMsg = "";
    while (client.available() && errorMsg.length() < 500) {
      errorMsg += (char)client.read();
    }
    Serial.println("Error: " + errorMsg);
    client.stop();
    return false;
  }
  
  Serial.println("✓ Success! Downloading response...");
  
  // สร้างไฟล์สำหรับบันทึก response
  String responseFile = String(inputFile);
  responseFile.replace(".wav", "_response.wav");
  
  File outFile = SPIFFS.open(responseFile.c_str(), FILE_WRITE);
  if (!outFile) {
    Serial.println("❌ Failed to create response file");
    client.stop();
    return false;
  }
  
  // ดาวน์โหลดและบันทึก response
  size_t totalReceived = 0;
  timeout = millis();
  uint8_t downloadBuff[1024]; // เพิ่มเป็น 1KB buffer
  
  Serial.println("📥 Downloading...");
  
  while (client.connected() || client.available()) {
    size_t available = client.available();
    
    if (available) {
      // อ่านเป็น chunk แทนทีละ byte
      size_t toRead = min(available, sizeof(downloadBuff));
      size_t bytesRead = client.read(downloadBuff, toRead);
      
      if (bytesRead > 0) {
        outFile.write(downloadBuff, bytesRead);
        totalReceived += bytesRead;
        
        if (totalReceived % 20480 == 0) { // แสดงทุก 20KB
          if (contentLen > 0) {
            Serial.printf("📥 Downloaded: %d / %d bytes (%.1f%%)\r", totalReceived, contentLen, (totalReceived * 100.0 / contentLen));
          } else {
            Serial.printf("📥 Downloaded: %d bytes\r", totalReceived);
          }
        }
        
        timeout = millis(); // reset timeout
      }
    } else {
      // ไม่มีข้อมูล รอนิดหน่อย
      delay(10);
    }
    
    // Timeout ถ้าไม่มีข้อมูล 10 วินาที
    if (millis() - timeout > 10000) {
      Serial.println("\n⚠️ Download timeout");
      break;
    }
    
    // ถ้ารู้ขนาดและได้ครบแล้ว ออกจาก loop
    if (contentLen > 0 && totalReceived >= contentLen) {
      break;
    }
  }
  
  Serial.printf("\n✅ Downloaded: %d bytes\n", totalReceived);
  outFile.close();
  client.stop();
  
  if (totalReceived > 0) {
    Serial.printf("💾 Response saved: %s\n", responseFile.c_str());
    return true;
  } else {
    Serial.println("⚠️ No data received");
    SPIFFS.remove(responseFile.c_str());
    return false;
  }
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
    Serial.printf("🎤 Volume: %d (Threshold: %d)\n", avgVolume, WAKE_THRESHOLD);
    lastPrint = millis();
  }
  
  if (avgVolume > WAKE_THRESHOLD) {
    Serial.printf("🎯 Wake word detected! Volume: %d\n", avgVolume);
    return true;
  }
  
  return false;
}

void deleteOldestFile() {
  if (SPIFFS.usedBytes() > SPIFFS.totalBytes() * 0.8) {
    Serial.println("⚠️ Storage almost full, deleting oldest...");
    
    char oldestFile[32];
    sprintf(oldestFile, "/rec_000.wav");
    
    if (SPIFFS.exists(oldestFile)) {
      SPIFFS.remove(oldestFile);
      Serial.printf("🗑️ Deleted: %s\n", oldestFile);
      
      // ลบ response file ที่เกี่ยวข้องด้วย
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
        
        // rename response files ด้วย
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
  html += "<h1>🎤 ESP32 Voice Recorder</h1>";
  
  html += "<div class='status'>";
  html += "<strong>📊 Storage:</strong> " + String(SPIFFS.usedBytes()) + " / " + String(SPIFFS.totalBytes()) + " bytes<br>";
  html += "<strong>📁 Files:</strong> " + String(fileCounter) + " recordings<br>";
  html += "<strong>🌐 IP:</strong> " + WiFi.localIP().toString();
  html += "</div>";
  
  html += "<h2>📂 Recordings</h2>";
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
      String fileLabel = isResponse ? "🔊 " : "🎙️ ";
      
      html += "<div class='" + itemClass + "'>";
      html += "<div><strong>" + fileLabel + filename + "</strong><br>";
      html += "<span class='info'>" + String(fileSize / 1024) + " KB</span></div>";
      html += "<div>";
      html += "<a href='/download?file=" + filename + "' class='btn btn-download'>⬇️ Download</a>";
      html += "<a href='/play?file=" + filename + "' class='btn btn-play' target='_blank'>▶️ Play</a>";
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
  html += "<a href='/deleteall' class='btn btn-delete' onclick='return confirm(\"ลบไฟล์ทั้งหมด?\")'>🗑️ Delete All</a>";
  html += "<a href='/' class='btn btn-play'>🔄 Refresh</a>";
  html += "</div>";
  
  html += "<p style='text-align:center; color:#999; margin-top:30px;'>💡 พูดว่า 'Jarvis' ดังๆ เพื่อเริ่มบันทึก " + String(RECORD_TIME) + " วินาที<br>";
  html += "🌟 ไฟล์จะถูกส่งไปยัง API อัตโนมัติและได้รับไฟล์ตอบกลับ</p>";
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
    Serial.println("❌ File not found: " + filename);
    server.send(404, "text/plain", "File not found: " + filename);
    return;
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("❌ Failed to open file");
    server.send(500, "text/plain", "Failed to open file");
    return;
  }
  
  Serial.println("✓ Sending file: " + filename + " (" + String(file.size()) + " bytes)");
  
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
    Serial.println("❌ File not found: " + filename);
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
    Serial.println("🗑️ Deleted: " + filename);
    
    // ลบ response file ที่เกี่ยวข้องด้วย (ถ้ามี)
    String relatedFile;
    if (filename.indexOf("_response") >= 0) {
      // ถ้าลบ response file ให้ลบไฟล์ต้นฉบับด้วย
      relatedFile = filename;
      relatedFile.replace("_response.wav", ".wav");
    } else {
      // ถ้าลบไฟล์ต้นฉบับ ให้ลบ response file ด้วย
      relatedFile = filename;
      relatedFile.replace(".wav", "_response.wav");
    }
    
    if (SPIFFS.exists(relatedFile.c_str())) {
      SPIFFS.remove(relatedFile.c_str());
      Serial.println("🗑️ Also deleted: " + relatedFile);
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
  Serial.printf("🗑️ Deleted %d files\n", deleteCount);
  
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
  Serial.println("✓ Web server started");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔═══════════════════════════════════╗");
  Serial.println("║  ESP32 I2S Voice Recorder v3.0    ║");
  Serial.println("║  with API Upload Feature          ║");
  Serial.println("╚═══════════════════════════════════╝\n");
  
  setupI2S();
  
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS failed!");
    while (1) delay(1000);
  }
  
  Serial.println("✓ SPIFFS initialized");
  Serial.printf("📊 Storage: %d / %d bytes\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
  
  // นับไฟล์ที่มีอยู่
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String filename = String(file.name());
    if (filename.endsWith(".wav") && filename.indexOf("_response") < 0) {
      fileCounter++;
    }
    file = root.openNextFile();
  }
  Serial.printf("📁 Found %d existing recordings\n", fileCounter);
  
  Serial.printf("\n📡 Connecting to WiFi: %s", ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi Connected!");
    Serial.print("🌐 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("👉 เปิด IP นี้ในเบราว์เซอร์เพื่อดูไฟล์บันทึก");
    Serial.println("🌟 API Upload: ENABLED\n");
    
    setupWebServer();
  } else {
    Serial.println("\n❌ WiFi failed! Running offline...");
    Serial.println("⚠️ API Upload will be disabled\n");
  }
  
  Serial.println("🎤 Listening for wake word 'Jarvis'...");
  Serial.printf("📊 Current threshold: %d\n", WAKE_THRESHOLD);
  Serial.println("💡 ดูค่า Volume ที่แสดงเพื่อปรับ WAKE_THRESHOLD\n");
  client.setServer(mqttServer, mqtt_port);
}

void loop() {
  server.handleClient();
  
  if (!isRecording) {
    if (detectWakeWord()) {
      isRecording = true;
      
      deleteOldestFile();
      
      char filename[32];
      sprintf(filename, "/rec_%03d.wav", fileCounter++);
      
      recordAudio(filename);
      
      // ส่งไฟล์ไปยัง API
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n🚀 Starting API upload...");
        bool success = sendToAPI(filename);
        
        if (success) {
          Serial.println("✅ API upload completed successfully!");
        } else {
          Serial.println("⚠️ API upload failed, but file is saved locally");
        }
      } else {
        Serial.println("⚠️ WiFi not connected, skipping API upload");
      }
      
      isRecording = false;
      Serial.println("🎤 Listening again...\n");
    }
  }
  if (!client.connected())
  {
    reconnect();
  }
  client.loop();
  double humidity = rand() % 101;
  double temperature = rand() % 101;
  double lumen = rand() % 101;
  double pm25 = rand() % 101;
  String data = "{\"data\": {"
              "\"humidity\":" + String(humidity) + ","
              "\"temperature\":" + String(temperature) + ","
              "\"lumen\":" + String(lumen) + ","
              "\"pm2.5\":" + String(pm25) +
              "}}";
  Serial.println(data);
  data.toCharArray(msg, (data.length() + 1));
  client.publish("@shadow/data/update", msg);
  
  delay(1000);
}