#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <AM2302-Sensor.h>
#include <time.h>
#include <FS.h>
#include <LittleFS.h>

ESP8266WebServer server(80);

const char* ssid = "RzeppaHome";
const char* password = "83218489161830208579";
const char* dns_name = "temperatur";

AM2302::AM2302_Sensor sensor(5);

// RAM History
const int HISTORY_SIZE = 200;
float tempHistory[HISTORY_SIZE];
float humHistory[HISTORY_SIZE];
String timeHistory[HISTORY_SIZE];
int historyIndex = 0;
bool historyFilled = false;

float lastTemp = NAN;
float lastHum = NAN;
int sensorErrorCount = 0;

unsigned long bootTime;

// DST (Sommer-/Winterzeit) EU
bool isDST(int day, int month, int dow) {
  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;

  int lastSunday = day - dow;

  if (month == 3) return lastSunday >= 25;
  if (month == 10) return lastSunday < 25;

  return false;
}

// Uhrzeit HH:MM:SS
String getTimeString() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (!t) return "00:00:00";

  int hour = t->tm_hour;
  int day = t->tm_mday;
  int month = t->tm_mon + 1;
  int dow = t->tm_wday;

  if (isDST(day, month, dow)) hour = (hour + 1) % 24;

  char buf[16];
  sprintf(buf, "%02d:%02d:%02d", hour, t->tm_min, t->tm_sec);
  return String(buf);
}

// Datum YYYY-MM-DD
String getDateString() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (!t) return "1970-01-01";

  char buf[16];
  sprintf(buf, "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
}

// Login
bool isAuthenticated() {
  if (server.hasHeader("Cookie")) {
    String c = server.header("Cookie");
    if (c.indexOf("ESPSESSIONID=1") != -1) return true;
  }
  return false;
}

void redirectToLogin() {
  server.sendHeader("Location", "/login");
  server.send(301);
}

// CSV speichern
void saveCSV(String date, String time, float temp, float hum) {
  String path = "/logs/" + date + ".csv";
  File f = LittleFS.open(path, "a");
  if (!f) return;
  f.printf("%s;%0.2f;%0.2f\n", time.c_str(), temp, hum);
  f.close();
}

// API: Liste aller Log-Tage
void handleApiLogs() {
  Dir dir = LittleFS.openDir("/logs");
  String json = "[";
  bool first = true;

  while (dir.next()) {
    String name = dir.fileName();
    name.replace("/logs/", "");
    name.replace(".csv", "");

    if (!first) json += ",";
    json += "\"" + name + "\"";
    first = false;
  }

  json += "]";
  server.send(200, "application/json", json);
}

// API: Tages-CSV lesen
void handleApiDay() {
  if (!server.hasArg("date")) {
    server.send(400, "text/plain", "Missing date");
    return;
  }

  String date = server.arg("date");
  String path = "/logs/" + date + ".csv";

  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "No data");
    return;
  }

  File f = LittleFS.open(path, "r");
  String json = "{ \"data\": [";
  bool first = true;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int p1 = line.indexOf(';');
    int p2 = line.indexOf(';', p1 + 1);

    String time = line.substring(0, p1);
    String temp = line.substring(p1 + 1, p2);
    String hum = line.substring(p2 + 1);

    if (!first) json += ",";
    json += "{ \"time\":\"" + time + "\", \"temp\":" + temp + ", \"hum\":" + hum + " }";
    first = false;
  }

  json += "] }";
  f.close();

  server.send(200, "application/json", json);
}

// API: System Info
void handleApiSystem() {
  long rssi = WiFi.RSSI();
  unsigned long uptimeSec = (millis() - bootTime) / 1000;

  String json = "{";
  json += "\"uptime\":\"" + String(uptimeSec) + " sec\",";
  json += "\"rssi\":" + String(rssi);
  json += "}";

  server.send(200, "application/json", json);
}

// API: Live Temp + CSV + RAM History
void handleApiTemp() {
  int status = sensor.read();

  if (status != 0) {
    sensorErrorCount++;
    if (sensorErrorCount >= 5) {
      server.send(200, "application/json", "{\"error\":\"sensor_failure\"}");
      return;
    }
    if (!isnan(lastTemp)) {
      server.send(200, "application/json",
        "{\"temp\":" + String(lastTemp) + ",\"hum\":" + String(lastHum) + "}");
      return;
    }
    server.send(200, "application/json", "{\"error\":\"sensor_failure\"}");
    return;
  }

  sensorErrorCount = 0;

  float temp = sensor.get_Temperature();
  float hum = sensor.get_Humidity();

  lastTemp = temp;
  lastHum = hum;

  String timeStr = getTimeString();
  String dateStr = getDateString();

  tempHistory[historyIndex] = temp;
  humHistory[historyIndex] = hum;
  timeHistory[historyIndex] = timeStr;

  historyIndex++;
  if (historyIndex >= HISTORY_SIZE) {
    historyIndex = 0;
    historyFilled = true;
  }

  saveCSV(dateStr, timeStr, temp, hum);

  server.send(200, "application/json",
    "{\"temp\":" + String(temp) + ",\"hum\":" + String(hum) + "}");
}

// API: RAM History
void handleApiHistory() {
  String json = "{ \"history\": [";
  int count = historyFilled ? HISTORY_SIZE : historyIndex;

  for (int i = 0; i < count; i++) {
    int idx = (historyFilled ? (historyIndex + i) % HISTORY_SIZE : i);

    json += "{";
    json += "\"t\":" + String(tempHistory[idx]) + ",";
    json += "\"h\":" + String(humHistory[idx]) + ",";
    json += "\"time\":\"" + timeHistory[idx] + "\"";
    json += "}";

    if (i < count - 1) json += ",";
  }

  json += "] }";
  server.send(200, "application/json", json);
}

// Login Page
void handleLogin() {
  String msg;

  if (server.hasArg("DISCONNECT")) {
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0; Path=/;");
    redirectToLogin();
    Serial.println("User has disconnected");
    return;
  }

  if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
    if (server.arg("USERNAME") == "user" && server.arg("PASSWORD") == "user") {
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1; Path=/;");
      server.sendHeader("Location", "/");
      server.send(301);
      Serial.println("User is loggt in");
      return;
    }
    msg = "Incorrect username or password.";
    Serial.println("User guest wrong password!!!");
  }

  server.send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<style>"
    "body{background:#111;color:#eee;font-family:Arial;padding:20px;}"
    "input{width:100%;max-width:300px;padding:10px;margin:8px 0;background:#222;color:#eee;"
    "border:1px solid #555;border-radius:6px;}"
    "a{color:#4aa3ff;}"
    "</style></head><body>"
    "<h2>Login</h2>"
    "<form method='POST'>"
    "Username:<br><input name='USERNAME'><br>"
    "Password:<br><input type='password' name='PASSWORD'><br>"
    "<input type='submit' value='Login'>"
    "</form>"
    "<p style='color:red;'>" + msg + "</p>"
    "</body></html>");
}

// Dashboard
void handleRoot() {
  if (!isAuthenticated()) { redirectToLogin(); return; }

  server.send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<style>"
    "body{background:#111;color:#eee;font-family:Arial;padding:20px;}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:20px;}"
    ".tile{background:#1c1c1c;padding:20px;border-radius:12px;text-align:center;}"
    ".value{font-size:2em;font-weight:bold;}"
    ".button{padding:11px 18px;background:#4aa3ff;color:#111;border-radius:6px;"
    "text-decoration:none;font-weight:bold;}"
    ".logout{padding:11px 18px;background:#ff0000;color:#111;border-radius:6px;"
    "text-decoration:none;font-weight:bold;}"
    "</style></head><body>"

    "<h2>Dashboard <a class='logout' href='/login?DISCONNECT=1' style='float:right;'>Logout</a></h2>"
    "<div class='grid'>"

    "<div class='tile'><h3>Temperature</h3><div id='temp' class='value'>--</div></div>"
    "<div class='tile'><h3>Humidity</h3><div id='hum' class='value'>--</div></div>"
    "<div class='tile'><h3>Sensor</h3><div id='sensor' class='value'>--</div></div>"

    "<div class='tile'><h3>System</h3>"
    "<div id='uptime'>--</div>"
    "<div id='wifi'>--</div></div>"

    "<div class='tile'><h3>Graph</h3><a class='button' href='/temperature'>Open</a></div>"
    "<div class='tile'><h3>Calendar</h3><a class='button' href='/calendar'>Open</a></div>"

    "</div>"

    "<script>"
    "function upd(){"
      "fetch('/api/temp').then(r=>r.json()).then(d=>{"
        "if(d.error){sensor.innerHTML='Error';return;}"
        "temp.innerHTML=d.temp+' °C';"
        "hum.innerHTML=d.hum+' %';"
        "sensor.innerHTML='OK';"
      "});"
      "fetch('/api/system').then(r=>r.json()).then(s=>{"
        "uptime.innerHTML='Uptime: '+s.uptime;"
        "wifi.innerHTML='WiFi: '+s.rssi+' dBm';"
      "});"
    "}"
    "setInterval(upd,2000);upd();"
    "</script>"

    "</body></html>");
}

// Live Graph Page
void handleTemperature() {
  if (!isAuthenticated()) { redirectToLogin(); return; }

  server.send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
    "<style>"
    "body{background:#111;color:#eee;font-family:Arial;padding:20px;}"
    "#chartContainer{width:100%;max-width:1200px;margin:auto;}"
    "canvas{width:100%!important;height:500px!important;background:#222;border-radius:8px;}"
    "a{color:#4aa3ff;}"
    "</style></head><body>"

    "<h2>Live Graph</h2>"
    "<div id='chartContainer'><canvas id='c'></canvas></div>"
    "<br><a href='/'>Back</a>"

    "<script>"
    "var x=[],t=[],h=[];"
    "var c=new Chart(document.getElementById('c'),{"
      "type:'line',"
      "data:{labels:x,datasets:["
        "{label:'Temp',data:t,borderColor:'red',fill:false},"
        "{label:'Hum',data:h,borderColor:'cyan',fill:false}"
      "]},"
      "options:{"
        "responsive:true,"
        "maintainAspectRatio:false,"
        "scales:{"
          "x:{ticks:{color:'#eee'}},"
          "y:{ticks:{color:'#eee'}}"
        "}"
      "}"
    "});"

    "function load(){fetch('/api/history').then(r=>r.json()).then(d=>{"
      "x.length=t.length=h.length=0;"
      "d.history.forEach(p=>{x.push(p.time);t.push(p.t);h.push(p.h);});"
      "c.update();"
    "});}"

    "function live(){fetch('/api/temp').then(r=>r.json()).then(d=>{"
      "if(d.error)return;"
      "var now=new Date().toLocaleTimeString();"
      "x.push(now);t.push(d.temp);h.push(d.hum);"
      "if(x.length>200){x.shift();t.shift();h.shift();}"
      "c.update();"
    "});}"

    "load();setInterval(live,2000);"
    "</script>"

    "</body></html>");
}

// Calendar Page
void handleCalendar() {
  if (!isAuthenticated()) { redirectToLogin(); return; }

  String html =
  "<html><head><meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
  "<style>"
  "body{background:#111;color:#eee;font-family:Arial;padding:20px;}"
  ".cal{display:grid;grid-template-columns:repeat(7,1fr);gap:8px;text-align:center;}"
  ".dayname{font-weight:bold;padding:10px 0;}"
  ".day{padding:15px;background:#1c1c1c;border-radius:8px;cursor:pointer;transition:0.2s;}"
  ".day:hover{background:#333;}"
  ".has{background:#4aa3ff;color:#111;font-weight:bold;}"
  "a{color:#4aa3ff;}"
  "</style></head><body>"

  "<h2>Kalender</h2>"

  "<div class='cal'>"
  "<div class='dayname'>Mo</div>"
  "<div class='dayname'>Di</div>"
  "<div class='dayname'>Mi</div>"
  "<div class='dayname'>Do</div>"
  "<div class='dayname'>Fr</div>"
  "<div class='dayname'>Sa</div>"
  "<div class='dayname'>So</div>"
  "</div>"

  "<div class='cal' id='days'></div>"

  "<br><a href='/'>Zurück</a>"

  "<script>"
  "function loadCalendar(){"
    "const now=new Date();"
    "const year=now.getFullYear();"
    "const month=now.getMonth();"
    "const first=new Date(year,month,1);"
    "const offset=(first.getDay()+6)%7;"
    "const dim=new Date(year,month+1,0).getDate();"

    "fetch('/api/logs').then(r=>r.json()).then(logs=>{"
      "let html='';"
      "for(let i=0;i<offset;i++) html+='<div></div>';"

      "for(let d=1;d<=dim;d++){"
        "let ds=year+'-'+String(month+1).padStart(2,'0')+'-'+String(d).padStart(2,'0');"
        "let cls = logs.includes(ds) ? 'day has' : 'day';"
        "html+=`<div class='${cls}' onclick=\"location='/day?date=${ds}'\">${d}</div>`;"
      "}"

      "document.getElementById('days').innerHTML=html;"
    "});"
  "}"
  "loadCalendar();"
  "</script>"

  "</body></html>";

  server.send(200, "text/html", html);
}

// Day Graph Page
void handleDay() {
  if (!isAuthenticated()) { redirectToLogin(); return; }
  if (!server.hasArg("date")) { server.send(400, "text/plain", "Missing date"); return; }

  String date = server.arg("date");

  String html =
  "<html><head><meta charset='UTF-8'>"
  "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
  "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
  "<style>"
  "body{background:#111;color:#eee;font-family:Arial;padding:20px;}"
  "#chartContainer{width:100%;max-width:1200px;margin:auto;}"
  "canvas{width:100%!important;height:500px!important;background:#222;padding:10px;border-radius:8px;}"
  "a{color:#4aa3ff;}"
  "</style></head><body>"

  "<h2>Graph für " + date + "</h2>"
  "<div id='chartContainer'><canvas id='chart'></canvas></div>"
  "<br><a href='/calendar'>Zurück</a>"

  "<script>"
  "var ctx=document.getElementById('chart').getContext('2d');"
  "var labels=[]; var tempData=[]; var humData=[];"

  "var chart=new Chart(ctx,{"
    "type:'line',"
    "data:{"
      "labels:labels,"
      "datasets:["
        "{label:'Temp (°C)',data:tempData,borderColor:'red',fill:false},"
        "{label:'Hum (%)',data:humData,borderColor:'cyan',fill:false}"
      "]"
    "},"
    "options:{"
      "responsive:true,"
      "maintainAspectRatio:false,"
      "scales:{"
        "x:{ticks:{color:'#eee'}},"
        "y:{ticks:{color:'#eee'}}"
      "}"
    "}"
  "});"

  "fetch('/api/day?date=" + date + "').then(r=>r.json()).then(data=>{"
    "data.data.forEach(p=>{"
      "labels.push(p.time);"
      "tempData.push(p.temp);"
      "humData.push(p.hum);"
    "});"
    "chart.update();"
  "});"

  "</script></body></html>";

  server.send(200, "text/html", html);
}

// 404 handler
void handleNotFound() {
  server.send(404, "text/plain", "404 - Not Found");
}

// Setup
void setup() {
  Serial.begin(9600);

  Serial.print("Connecting to WiFi ...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("");
  Serial.println("Connection successfull!!!");
  Serial.println("");
    Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  bootTime = millis();

  // NTP aktivieren (UTC+1, DST wird manuell korrigiert)
  Serial.println("Configuring Time");
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");

  // LittleFS starten
  Serial.println("Starting Logs");
  LittleFS.begin();
  if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");

  Serial.println("Configuring DNS");
  MDNS.begin(dns_name);

  // Routes
  Serial.println("Starting Routes");
  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/temperature", handleTemperature);
  server.on("/calendar", handleCalendar);
  server.on("/day", handleDay);

  server.on("/api/temp", handleApiTemp);
  server.on("/api/history", handleApiHistory);
  server.on("/api/system", handleApiSystem);
  server.on("/api/logs", handleApiLogs);
  server.on("/api/day", handleApiDay);

  server.onNotFound(handleNotFound);

  server.collectHeaders("Cookie");
  server.begin();
  Serial.println("");
  Serial.println("Server started Successfully!!!");
}

// Loop
void loop() {
  server.handleClient();
  MDNS.update();
}