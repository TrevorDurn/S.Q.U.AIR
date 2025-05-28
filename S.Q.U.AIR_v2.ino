// ** Welcome to S.Q.U.AIR - Sensor for the Quantitative Understanding of Air **
// Dev. by Trevor J Durning of the Westervelt Aerosol Lab, Lamont-Doherty Earth 
// Observatory @ Columbia University. Special thanks to Dr. Daniel M. Westervelt

#include <esp_task_wdt.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <SD.h>
#include <time.h>

// Disable the task watchdog
void disableTaskWDT() {
  esp_task_wdt_deinit();
}

// Wi-Fi credentials
const char* ssid     = "<YOUR_SSID>";
const char* password = "<YOUR_PASSWORD>";

// SD card chip-select pin
const int SD_CS_PIN = 14;

// HTTP server on port 80
WebServer server(80);

// PMS5003 on UART2 (GPIO16/17)
#define PMS_RX_PIN 16
#define PMS_TX_PIN 17
HardwareSerial pms(2);
const uint8_t CMD_PASSIVE[] = {0x42,0x4D,0xE1,0x00,0x00,0x01,0x71};
const uint8_t CMD_READ[]    = {0x42,0x4D,0xE2,0x00,0x00,0x01,0x72};

// BME280 on I2C (GPIO21/22)
#define BME_SDA_PIN 21
#define BME_SCL_PIN 22
Adafruit_BME280 bme;

// — Helper to serve PNGs from SD —
void servePng(const char* path) {
  File img = SD.open(path);
  if (!img) {
    server.send(404, "text/plain", String(path) + " not found");
    return;
  }
  server.sendHeader("Content-Type", "image/png");
  server.streamFile(img, "image/png");
  img.close();
}

// — Get current local datetime as string —
String getDateTime() {
  time_t now = time(nullptr);
  struct tm tmInfo;
  localtime_r(&now, &tmInfo);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
  return String(buf);
}

// — Initialize PMS5003 in passive mode —
void initPMS() {
  pms.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  pms.write(CMD_PASSIVE, sizeof(CMD_PASSIVE));
  delay(100);
  delay(10000);
}

// — Read PM2.5 from PMS5003 —
int readPM25() {
  while (pms.available()) pms.read();
  pms.write(CMD_READ, sizeof(CMD_READ));
  unsigned long t0 = millis();
  while (pms.available() < 2 && millis() - t0 < 2000);
  bool found = false;
  while (pms.available() >= 2 && !found) {
    if (pms.peek() == 0x42) {
      uint8_t h = pms.read(), l = pms.read();
      found = (l == 0x4D);
    } else {
      pms.read();
    }
  }
  if (!found) return -1;
  uint8_t buf[30];
  if (pms.readBytes(buf, 30) < 30) return -1;
  return (buf[12] << 8) | buf[13];
}

// — Web handler: root page with dashboard —
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <meta charset='utf-8'/>
  <title>S.Q.U.AIR Quality Dashboard</title>
  <style>
    body { text-align: center; font-family: sans-serif; }
    img, canvas { display: block; margin: 0 auto; max-width: 90vw; height: auto; }
    a { color: #0066cc; text-decoration: none; }
  </style>
  <script src='https://cdn.jsdelivr.net/npm/chart.js'></script>
</head>
<body>
  <!-- Photo 1 -->
  <img src='/photo1.png' alt='Photo 1'>

  <h1>S.Q.U.AIR - Sensor for the Quantitative Understanding of Air</h1>
  <h1>Add Header Here if You'd Like</h1>

  <p><strong>Current Time:</strong> <span id="time">--</span></p>
  <p>PM2.5: <span id="pm25">--</span> µg/m³</p>
  <p>Temp: <span id="temp">--</span> °F</p>
  <p>Humidity: <span id="hum">--</span> %</p>
  <p>Pressure: <span id="pres">--</span> hPa</p>
  <p>SD free: <span id="heap">--</span> bytes</p>
  <p>
    <a href='/download'>Download CSV</a> ||
    <a href='/reset'>Reset CSV</a>
  </p>

  <canvas id='myChart' width='800' height='400'></canvas>

  <!-- Photo 2 -->
  <img src='/photo2.png' alt='Photo 2'>

  <script>
    async function updateReadings() {
      const res = await fetch('/latest');
      const d   = await res.json();
      document.getElementById('time').textContent = d.time;
      document.getElementById('pm25').textContent = d.pm25;
      document.getElementById('temp').textContent = d.temp;
      document.getElementById('hum').textContent  = d.hum;
      document.getElementById('pres').textContent = d.pres;
      document.getElementById('heap').textContent = d.heap;
    }
    setInterval(updateReadings, 5000);
    updateReadings();

    const ctx   = document.getElementById('myChart').getContext('2d');
    const chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [{ label: 'PM2.5 (µg/m³)', yAxisID: 'y', data: [], fill: false }]
      },
      options: {
        scales: {
          y: { type: 'linear', position: 'left', title: { display: true, text: 'PM2.5' } }
        }
      }
    });

    async function updateChart() {
      const res = await fetch('/avgdata');
      const arr = await res.json();
      chart.data.labels           = arr.map(e => e.time);
      chart.data.datasets[0].data = arr.map(e => e.pm25);
      chart.update();
    }
    setInterval(updateChart, 60000);
    updateChart();
  </script>
</body>
</html>
  )rawliteral";

  server.send(200, "text/html; charset=utf-8", html);
}

// — Web handler: return latest readings as JSON —
void handleLatest() {
  String now     = getDateTime();
  int    pm25    = readPM25();
  float  tempC   = bme.readTemperature();                     // °C
  float  tempF   = tempC * 9.0 / 5.0 + 32.0;                  // convert to °F
  float  hum     = bme.readHumidity();
  float  pres    = bme.readPressure() / 100.0F;

  uint64_t total = SD.totalBytes();
  uint64_t used  = SD.usedBytes();
  uint64_t heap  = (total > used) ? (total - used) : 0;

  String j = String("{") +
    "\"time\":\"" + now    + "\"," +
    "\"pm25\":"   + pm25   + "," +
    "\"temp\":"   + String(tempF,1) + "," +   // Fahrenheit
    "\"hum\":"    + hum    + "," +
    "\"pres\":"   + pres   + "," +
    "\"heap\":"   + heap   +
  "}";
  server.send(200, "application/json", j);
}

// — Web handler: return averaged data over last 10 one-minute bins —
void handleAvgData() {
  if (!SD.exists("/data.csv")) {
    server.send(200, "application/json", "[]");
    return;
  }
  File f     = SD.open("/data.csv", FILE_READ);
  time_t now = time(nullptr);
  float sum[10]   = {0};
  int   count[10] = {0};

  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.startsWith("datetime")) continue;
    int c1 = line.indexOf(','), c2 = line.indexOf(',', c1 + 1);
    String dt = line.substring(0, c1);
    int    pm = line.substring(c1 + 1, c2).toInt();

    struct tm tmInfo = {0};
    int Y,M,D,h,m,s;
    if (sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d",
               &Y,&M,&D,&h,&m,&s) != 6) continue;
    tmInfo.tm_year = Y - 1900; tmInfo.tm_mon = M - 1;
    tmInfo.tm_mday = D;       tmInfo.tm_hour = h;
    tmInfo.tm_min  = m;       tmInfo.tm_sec  = s;
    time_t t = mktime(&tmInfo);

    long delta = now - t;
    if (delta >= 0 && delta < 600) {
      int idx = delta / 60;
      sum[idx]   += pm;
      count[idx] += 1;
    }
  }
  f.close();

  String json = "[";
  for (int i = 9; i >= 0; i--) {
    time_t bt = now - i * 60;
    bt -= bt % 60;
    struct tm btm;
    localtime_r(&bt, &btm);
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &btm);

    float avg = (count[i] > 0) ? (sum[i] / count[i]) : 0.0;
    if (i < 9) json += ",";
    json += String("{\"time\":\"") + buf +
            String("\",\"pm25\":") + String(avg,1) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// — Web handler: download full CSV —
void handleDownload() {
  File f = SD.open("/data.csv", FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "CSV not found");
    return;
  }
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=data.csv");
  server.streamFile(f, "text/csv");
  f.close();
}

// — Web handler: reset CSV file —
void handleReset() {
  SD.remove("/data.csv");
  File f = SD.open("/data.csv", FILE_WRITE);
  if (f) {
    f.println("datetime,pm25(ug/m3),temp(F),hum(%),pressure(hPa)");
    f.close();
    server.send(200, "text/plain", "CSV reset. <a href='/'>Back</a>");
  } else {
    server.send(500, "text/plain", "Failed to reset CSV.");
  }
}

// — Web handler: serve photo2.png from SD —
void handlePhotoTwo() {
  servePng("/photo2.png");
}
// — Web handler: serve photo1.png from SD —
void handlePhotoOne() {
  servePng("/photo1.png");
}

void setup() {
  disableTaskWDT();
  Serial.begin(115200);
  while (!Serial) delay(10);

  // Mount SD
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Card mount failed");
    return;
  }

  // Create CSV header if first run
  if (!SD.exists("/data.csv")) {
    File f = SD.open("/data.csv", FILE_WRITE);
    if (f) {
      f.println("datetime,pm25(ug/m3),temp(F),hum(%),pressure(hPa)");
      f.close();
    }
  }

  initPMS();
  Wire.begin(BME_SDA_PIN, BME_SCL_PIN);
  if (!bme.begin(0x76)) Serial.println("BME280 init failed");

  Serial.printf("Connecting to %s", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.printf("\nWi-Fi connected, IP = %s\n", WiFi.localIP().toString().c_str());

  // NTP
  configTime(-4*3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm ti;
  while (!getLocalTime(&ti) || ti.tm_year + 1900 < 2025) {
    delay(500);
  }

  // Register endpoints
  server.on("/",           HTTP_GET, handleRoot);
  server.on("/latest",     HTTP_GET, handleLatest);
  server.on("/avgdata",    HTTP_GET, handleAvgData);
  server.on("/download",   HTTP_GET, handleDownload);
  server.on("/reset",      HTTP_GET, handleReset);
  server.on("/photo1.png", HTTP_GET, handlePhotoOne);
  server.on("/photo2.png", HTTP_GET, handlePhotoTwo);

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  // Log readings to CSV
  String dt    = getDateTime();
  int    pm25  = readPM25();
  float  temp  = bme.readTemperature() * 9.0 / 5.0 + 32.0;  // °F
  float  hum   = bme.readHumidity();
  float  pres  = bme.readPressure() / 100.0F;

  File f = SD.open("/data.csv", FILE_APPEND);
  if (f) {
    f.print(dt);       f.print(",");
    f.print(pm25);     f.print(",");
    f.print(temp,2);   f.print(",");
    f.print(hum,2);    f.print(",");
    f.print(pres,2);
    f.println();
    f.close();
  }

  server.handleClient();
}
