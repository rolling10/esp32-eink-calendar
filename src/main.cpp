#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// 时间配置
const char* ntpServer = "ntp.aliyun.com";
const long  gmtOffset_sec = 8 * 3600;  // 东八区
const int   daylightOffset_sec = 0;

// 状态指示灯配置
#define LED_PIN 22
enum DeviceState {
  STATE_INIT,
  STATE_WIFI_CONNECTING,
  STATE_MQTT_CONNECTING,
  STATE_NORMAL,
  STATE_AP_MODE,
  STATE_SLEEP_PREPARE
};
DeviceState currentState = STATE_INIT;
unsigned long lastLedUpdate = 0;
bool ledState = false;

// 墨水屏配置
int epd_cs = 5;
int epd_dc = 17;
int epd_rst = 16;
int epd_busy = 4;
GxEPD2_3C<GxEPD2_420c_GDEY042Z98, GxEPD2_420c_GDEY042Z98::HEIGHT>* display = nullptr;
U8G2_FOR_ADAFRUIT_GFX u8g2Font;

// 硬件按钮配置
const int BUTTON_PIN = 14;
volatile unsigned long lastButtonPress = 0;
volatile bool buttonPressed = false;
bool apModeEnabled = false;

// 网络配置存储
Preferences preferences;
WebServer server(80);

// 默认配置
char wifi_ssid[32] = "just2";
char wifi_password[32] = "xxxxxxxxx";
char mqtt_server[32] = "192.168.1.40";
int mqtt_port = 1883;
char mqtt_topic[32] = "jisilu/calendar";
char mqtt_user[32] = "";
char mqtt_pass[32] = "";

// 定时唤醒配置
#define MAX_WAKE_TIMES 5
char wake_times[MAX_WAKE_TIMES][6]; // 格式: "HH:MM"
int wake_times_count = 1;

WiFiClient espClient;
PubSubClient client(espClient);

// 显示缓存
String lastDate = "";
String weekDay = "";  // 添加星期变量
String datePrefix = ""; // 添加"今日"或"明日"标识
String lastEvents = "";
String messages[50];
int messageCount = 0;
volatile bool mqttWakeFlag = false;

// 中断服务函数
void IRAM_ATTR handleButtonPress() {
  buttonPressed = true;
}

void initDisplay() {
  // 如果已经存在display对象，先删除它
  if (display != nullptr) {
    delete display;
  }
  
  // 使用当前配置的引脚创建新的display对象
  display = new GxEPD2_3C<GxEPD2_420c_GDEY042Z98, GxEPD2_420c_GDEY042Z98::HEIGHT>(GxEPD2_420c_GDEY042Z98(epd_cs, epd_dc, epd_rst, epd_busy));
  
  display->init(115200);
  display->setRotation(0);
  u8g2Font.begin(*display);  // 注意这里使用解引用
  u8g2Font.setFontMode(1);
  u8g2Font.setForegroundColor(GxEPD_BLACK);
  u8g2Font.setBackgroundColor(GxEPD_WHITE);
  u8g2Font.setFont(u8g2_font_wqy16_t_gb2312);
  display->fillScreen(GxEPD_WHITE);
  display->display(true);
  
  Serial.printf("墨水屏初始化完成，使用引脚 CS:%d DC:%d RST:%d BUSY:%d\n", epd_cs, epd_dc, epd_rst, epd_busy);
}

void initButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
}

void syncTime() {
  if (WiFi.status() == WL_CONNECTED) {
    // 配置NTP服务器
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    // 等待时间同步
    Serial.println("等待NTP时间同步...");
    struct tm timeinfo;
    int retry = 0;
    while(!getLocalTime(&timeinfo) && retry < 10) {
      Serial.print(".");
      delay(500);
      retry++;
    }
    
    if (retry < 10) {
      Serial.println("时间已同步");
      char strftime_buf[64];
      strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.println(strftime_buf);
    } else {
      Serial.println("时间同步失败");
    }
  }
}

void updateLedState() {
  unsigned long now = millis();
  unsigned long interval = 0;
  bool shouldBlink = false;
  
  switch(currentState) {
    case STATE_INIT:
      interval = 200; // 快速闪烁
      shouldBlink = true;
      break;
    case STATE_WIFI_CONNECTING:
      interval = 500; // 慢速闪烁
      shouldBlink = true;
      break;
    case STATE_MQTT_CONNECTING:
      // 双闪模式
      if(now - lastLedUpdate >= 1000) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        lastLedUpdate = now;
      }
      return;
    case STATE_NORMAL:
      digitalWrite(LED_PIN, HIGH); // 常亮
      return;
    case STATE_AP_MODE:
      interval = 1000; // 慢速闪烁
      shouldBlink = true;
      break;
    case STATE_SLEEP_PREPARE:
      // 三次快闪后熄灭
      if(now - lastLedUpdate >= 3000) {
        for(int i=0; i<3; i++) {
          digitalWrite(LED_PIN, HIGH);
          delay(100);
          digitalWrite(LED_PIN, LOW);
          delay(100);
        }
        lastLedUpdate = now;
      }
      return;
  }
  
  if(shouldBlink && now - lastLedUpdate >= interval) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    lastLedUpdate = now;
  }
}

void loadConfig() {
  Serial.println("开始加载配置...");
  preferences.begin("config", true);
  
  // 检查是否有保存的配置
  bool hasConfig = preferences.isKey("wifi_ssid");
  
  if (hasConfig) {
    // 加载网络配置
    preferences.getString("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    preferences.getString("wifi_pass", wifi_password, sizeof(wifi_password));
    preferences.getString("mqtt_server", mqtt_server, sizeof(mqtt_server));
    mqtt_port = preferences.getInt("mqtt_port", 1883);
    preferences.getString("mqtt_topic", mqtt_topic, sizeof(mqtt_topic));
    preferences.getString("mqtt_user", mqtt_user, sizeof(mqtt_user));
    preferences.getString("mqtt_pass", mqtt_pass, sizeof(mqtt_pass));
    
    Serial.printf("已加载网络配置: SSID=%s, MQTT=%s:%d\n", 
                 wifi_ssid, mqtt_server, mqtt_port);
    
    // 加载墨水屏引脚配置
    epd_cs = preferences.getInt("epd_cs", 5);
    epd_dc = preferences.getInt("epd_dc", 17);
    epd_rst = preferences.getInt("epd_rst", 16);
    epd_busy = preferences.getInt("epd_busy", 4);
    
    Serial.printf("已加载墨水屏引脚配置: CS=%d, DC=%d, RST=%d, BUSY=%d\n", 
                 epd_cs, epd_dc, epd_rst, epd_busy);
    
    // 加载定时唤醒配置（使用更短的键名）
    wake_times_count = preferences.getInt("wt_cnt", 1);
    // 兼容旧版本配置
    if (wake_times_count == 1 && preferences.isKey("wake_times_count")) {
      wake_times_count = preferences.getInt("wake_times_count", 1);
    }
    Serial.printf("加载定时配置，存储的时间点数量: %d\n", wake_times_count);
  
    // 重置所有时间槽
    for(int i = 0; i < MAX_WAKE_TIMES; i++) {
      wake_times[i][0] = '\0';
    }
  
    // 加载保存的时间（使用更短的键名）
    int validCount = 0;
    for(int i = 0; i < wake_times_count && i < MAX_WAKE_TIMES; i++) {
      String key = "wt_" + String(i);
      String timeStr = preferences.getString(key.c_str(), "");
      
      // 尝试从旧格式加载
      if(timeStr.length() == 0) {
        String oldKey = "wake_time_" + String(i);
        timeStr = preferences.getString(oldKey.c_str(), "");
      }
      
      // 验证时间格式
      if(timeStr.length() > 0) {
        int hour, minute;
        if(sscanf(timeStr.c_str(), "%d:%d", &hour, &minute) == 2 && 
           hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
          // 格式化为标准格式 (HH:MM)
          snprintf(wake_times[validCount], sizeof(wake_times[validCount]), 
                  "%02d:%02d", hour, minute);
          Serial.printf("加载有效时间点 %d: %s\n", validCount, wake_times[validCount]);
          validCount++;
        } else {
          Serial.printf("忽略无效时间格式: %s\n", timeStr.c_str());
        }
      }
    }
  
    // 更新实际的有效时间点数量
    wake_times_count = validCount;
  
    // 确保至少有一个默认时间
    if(wake_times_count == 0) {
      strcpy(wake_times[0], "05:10");
      wake_times_count = 1;
      Serial.println("没有有效的时间点，使用默认值: 05:10");
    }
  
    Serial.printf("成功加载 %d 个有效时间点\n", wake_times_count);
  } else {
    Serial.println("未找到保存的配置，使用默认值");
    // 确保至少有一个默认时间
    strcpy(wake_times[0], "05:10");
    wake_times_count = 1;
  }
  
  preferences.end();
  Serial.println("配置加载完成");
}

void saveConfig() {
  Serial.println("开始保存配置...");
  preferences.begin("config", false);
  
  // 保存网络配置
  preferences.putString("wifi_ssid", wifi_ssid);
  preferences.putString("wifi_pass", wifi_password);
  preferences.putString("mqtt_server", mqtt_server);
  preferences.putInt("mqtt_port", mqtt_port);
  preferences.putString("mqtt_topic", mqtt_topic);
  preferences.putString("mqtt_user", mqtt_user);
  preferences.putString("mqtt_pass", mqtt_pass);
  
  // 保存墨水屏引脚配置
  preferences.putInt("epd_cs", epd_cs);
  preferences.putInt("epd_dc", epd_dc);
  preferences.putInt("epd_rst", epd_rst);
  preferences.putInt("epd_busy", epd_busy);
  
  // 简化保存逻辑
  Serial.printf("保存定时配置，共 %d 个时间点\n", wake_times_count);
  
  // 保存时间点数量（使用更短的键名）
  preferences.putInt("wt_cnt", wake_times_count);
  Serial.printf("保存时间点数量: %d\n", wake_times_count);
  
  // 保存每个时间点（使用更短的键名）
  for(int i = 0; i < wake_times_count; i++) {
    String key = "wt_" + String(i);
    preferences.putString(key.c_str(), wake_times[i]);
    Serial.printf("保存时间点 %d: %s\n", i, wake_times[i]);
  }
  
  // 立即提交更改
  preferences.end();
  preferences.begin("config", true); // 以只读方式重新打开进行验证
  
  // 验证保存结果
  int savedCount = preferences.getInt("wake_times_count", 0);
  Serial.printf("验证保存结果: 保存了 %d 个时间点\n", savedCount);
  
  bool verificationSuccess = true;
  for(int i = 0; i < savedCount; i++) {
    String key = "wake_time_" + String(i);
    String savedTime = preferences.getString(key.c_str(), "");
    if(savedTime != wake_times[i]) {
      Serial.printf("验证失败: 时间点 %d 不匹配 (保存=%s, 读取=%s)\n", 
                   i, wake_times[i], savedTime.c_str());
      verificationSuccess = false;
    } else {
      Serial.printf("验证成功: 时间点 %d = %s\n", i, savedTime.c_str());
    }
  }
  
  if(!verificationSuccess) {
    Serial.println("警告: 部分时间点可能未正确保存");
  }
  
  preferences.end();
  Serial.println("配置保存完成");
}

void startAPMode() {
  apModeEnabled = true;
  WiFi.softAP("ESP32-Config");
  
  server.on("/", HTTP_GET, [](){
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<title>ESP32配置</title></head>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 20px; }";
    html += "h1, h2 { color: #333; }";
    html += "form { margin-bottom: 20px; }";
    html += "label { display: inline-block; width: 150px; margin-bottom: 10px; }";
    html += "input { padding: 5px; margin-bottom: 10px; width: 200px; }";
    html += "input[type=submit] { background-color: #4CAF50; color: white; padding: 10px 15px; border: none; cursor: pointer; width: auto; }";
    html += ".wake-time { margin-bottom: 10px; }";
    html += ".add-btn { background-color: #008CBA; color: white; padding: 5px 10px; border: none; cursor: pointer; margin-bottom: 15px; }";
    html += ".remove-btn { background-color: #f44336; color: white; padding: 2px 8px; border: none; cursor: pointer; margin-left: 10px; }";
    html += ".reset-btn { background-color: #ff9800; color: white; padding: 10px 15px; border: none; cursor: pointer; margin-top: 20px; }";
    html += "</style>";
    html += "<script>";
    html += "function addWakeTime() {";
    html += "  var container = document.getElementById('wake-times');";
    html += "  var timeInputs = container.getElementsByClassName('wake-time');";
    html += "  if(timeInputs.length >= " + String(MAX_WAKE_TIMES) + ") {";
    html += "    alert('最多添加 " + String(MAX_WAKE_TIMES) + " 个定时时间');";
    html += "    return;";
    html += "  }";
    html += "  var div = document.createElement('div');";
    html += "  div.className = 'wake-time';";
    html += "  var index = timeInputs.length;";
    html += "  div.innerHTML = '<input type=\"time\" name=\"wake_time_' + index + '\" value=\"\">' +";
    html += "                 '<button type=\"button\" class=\"save-btn\" onclick=\"saveSingleTime(' + index + ')\">保存</button>' +";
    html += "                 '<button type=\"button\" class=\"remove-btn\" onclick=\"this.parentElement.remove()\">删除</button>';";
    html += "  // 确保新添加的时间点也能保存";
    html += "  container.appendChild(div);";
    html += "}";
    html += "";
    html += "function saveSingleTime(index) {";
    html += "  var timeValue = document.getElementsByName('wake_time_' + index)[0].value;";
    html += "  if(!timeValue) {";
    html += "    alert('请先选择时间');";
    html += "    return;";
    html += "  }";
    html += "  fetch('/save-single', {";
    html += "    method: 'POST',";
    html += "    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },";
    html += "    body: 'index=' + index + '&time=' + encodeURIComponent(timeValue)";
    html += "  }).then(response => {";
    html += "    if(response.ok) return response.text();";
    html += "    throw new Error('保存失败');";
    html += "  }).then(text => {";
    html += "    alert(text);";
    html += "  }).catch(err => {";
    html += "    alert(err.message);";
    html += "  });";
    html += "}";
    html += "</script>";
    html += "<body>";
    html += "<h1>ESP32配置</h1>";
    html += "<form action='/save' method='POST'>";
    
    html += "<h2>网络配置</h2>";
    html += "<div><label>WiFi SSID:</label><input type='text' name='ssid' value='"+String(wifi_ssid)+"'></div>";
    html += "<div><label>WiFi密码:</label><input type='password' name='pass' value='"+String(wifi_password)+"'></div>";
    
    html += "<h2>MQTT配置</h2>";
    html += "<div><label>MQTT服务器:</label><input type='text' name='mqtt' value='"+String(mqtt_server)+"'></div>";
    html += "<div><label>MQTT端口:</label><input type='number' name='port' value='"+String(mqtt_port)+"'></div>";
    html += "<div><label>MQTT主题:</label><input type='text' name='topic' value='"+String(mqtt_topic)+"'></div>";
    html += "<div><label>MQTT用户名:</label><input type='text' name='mqtt_user' value='"+String(mqtt_user)+"'></div>";
    html += "<div><label>MQTT密码:</label><input type='password' name='mqtt_pass' value='"+String(mqtt_pass)+"'></div>";
    
    html += "<h2>墨水屏引脚配置</h2>";
    html += "<div><label>CS引脚:</label><input type='number' name='epd_cs' value='"+String(epd_cs)+"'></div>";
    html += "<div><label>DC引脚:</label><input type='number' name='epd_dc' value='"+String(epd_dc)+"'></div>";
    html += "<div><label>RST引脚:</label><input type='number' name='epd_rst' value='"+String(epd_rst)+"'></div>";
    html += "<div><label>BUSY引脚:</label><input type='number' name='epd_busy' value='"+String(epd_busy)+"'></div>";
    
    html += "<h2>定时唤醒配置</h2>";
    html += "<div id='wake-times'>";
    
    // 显示已有的定时时间
    Serial.printf("在Web界面显示 %d 个定时时间\n", wake_times_count);
    for(int i = 0; i < MAX_WAKE_TIMES; i++) {
      if(i < wake_times_count) {
        // 显示已保存的时间点
        Serial.printf("显示时间点 %d: %s\n", i, wake_times[i]);
        html += "<div class='wake-time'>";
        html += "<input type='time' name='wake_time_" + String(i) + "' value='" + String(wake_times[i]) + "'>";
        html += "<button type='button' class='save-btn' onclick='saveSingleTime(" + String(i) + ")'>保存</button>";
        html += "<button type='button' class='remove-btn' onclick='this.parentElement.remove()'>删除</button>";
        html += "</div>";
      } else {
        // 显示空的时间输入框
        html += "<div class='wake-time'>";
        html += "<input type='time' name='wake_time_" + String(i) + "' value=''>";
        html += "<button type='button' class='save-btn' onclick='saveSingleTime(" + String(i) + ")'>保存</button>";
        if(i > 0) {
          html += "<button type='button' class='remove-btn' onclick='this.parentElement.remove()'>删除</button>";
        }
        html += "</div>";
      }
    }
    
    html += "</div>";
    html += "<button type='button' class='add-btn' onclick='addWakeTime()'>添加定时时间</button>";
    
    html += "<div><input type='submit' value='保存配置'></div>";
    html += "</form>";
    
    // 添加重置配置按钮
    html += "<button type='button' class='reset-btn' onclick='resetConfig()'>重置所有配置</button>";
    
    html += "<script>";
    html += "function resetConfig() {";
    html += "  if (confirm('确定要重置所有配置吗？这将清除所有设置并重启设备。')) {";
    html += "    fetch('/reset', { method: 'POST' })";
    html += "      .then(response => response.text())";
    html += "      .then(text => { alert(text); window.location.reload(); })";
    html += "      .catch(error => alert('重置失败: ' + error));";
    html += "  }";
    html += "}";
    html += "</script>";
    
    html += "</body></html>";
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/save", HTTP_POST, [](){
    Serial.println("接收到配置保存请求");
    
    // 保存网络配置
    if (server.hasArg("ssid")) strcpy(wifi_ssid, server.arg("ssid").c_str());
    if (server.hasArg("pass")) strcpy(wifi_password, server.arg("pass").c_str());
    if (server.hasArg("mqtt")) strcpy(mqtt_server, server.arg("mqtt").c_str());
    if (server.hasArg("port")) mqtt_port = server.arg("port").toInt();
    if (server.hasArg("topic")) strcpy(mqtt_topic, server.arg("topic").c_str());
    if (server.hasArg("mqtt_user")) strcpy(mqtt_user, server.arg("mqtt_user").c_str());
    if (server.hasArg("mqtt_pass")) strcpy(mqtt_pass, server.arg("mqtt_pass").c_str());
    
    // 保存墨水屏引脚配置
    if (server.hasArg("epd_cs")) epd_cs = server.arg("epd_cs").toInt();
    if (server.hasArg("epd_dc")) epd_dc = server.arg("epd_dc").toInt();
    if (server.hasArg("epd_rst")) epd_rst = server.arg("epd_rst").toInt();
    if (server.hasArg("epd_busy")) epd_busy = server.arg("epd_busy").toInt();
    
    // 保存定时唤醒配置
    wake_times_count = 0;
    Serial.println("开始收集定时时间点...");
    
    // 首先收集所有提交的时间点
    for(int i = 0; i < MAX_WAKE_TIMES; i++) {
      String argName = "wake_time_" + String(i);
      if (server.hasArg(argName)) {
        String timeValue = server.arg(argName);
        if(timeValue.length() > 0) {
          // 验证时间格式
          int hour, minute;
          if(sscanf(timeValue.c_str(), "%d:%d", &hour, &minute) == 2 && 
             hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
            // 格式化为标准格式 (HH:MM)
            snprintf(wake_times[wake_times_count], sizeof(wake_times[wake_times_count]), 
                    "%02d:%02d", hour, minute);
            Serial.printf("接收到有效定时时间 %d: %s\n", wake_times_count, wake_times[wake_times_count]);
            wake_times_count++;
          } else {
            Serial.printf("忽略无效时间格式: %s\n", timeValue.c_str());
          }
        }
      }
    }
    
    Serial.printf("共收集到 %d 个有效时间点\n", wake_times_count);
    
    // 确保至少有一个定时时间
    if(wake_times_count == 0) {
      strcpy(wake_times[0], "05:10");
      wake_times_count = 1;
      Serial.println("没有定时时间，使用默认值: 05:10");
    }
    
    // 打印所有收集到的时间点用于调试
    Serial.println("收集到的时间点列表:");
    for(int i = 0; i < wake_times_count; i++) {
      Serial.printf("时间点 %d: %s\n", i, wake_times[i]);
    }
    
    // 直接保存到Preferences
    preferences.begin("config", false);
    
    // 保存网络配置
    preferences.putString("wifi_ssid", wifi_ssid);
    preferences.putString("wifi_pass", wifi_password);
    preferences.putString("mqtt_server", mqtt_server);
    preferences.putInt("mqtt_port", mqtt_port);
    preferences.putString("mqtt_topic", mqtt_topic);
    preferences.putString("mqtt_user", mqtt_user);
    preferences.putString("mqtt_pass", mqtt_pass);
    
    // 保存墨水屏引脚配置
    preferences.putInt("epd_cs", epd_cs);
    preferences.putInt("epd_dc", epd_dc);
    preferences.putInt("epd_rst", epd_rst);
    preferences.putInt("epd_busy", epd_busy);
    
    // 保存时间点数量（使用更短的键名）
    preferences.putInt("wt_cnt", wake_times_count);
    
    // 保存每个时间点（使用更短的键名）
    for(int i = 0; i < wake_times_count; i++) {
      String key = "wt_" + String(i);
      preferences.putString(key.c_str(), wake_times[i]);
      Serial.printf("保存时间点 %d: %s\n", i, wake_times[i]);
    }
    
    // 清除不再使用的时间点
    for(int i = wake_times_count; i < MAX_WAKE_TIMES; i++) {
      String key = "wake_time_" + String(i);
      if(preferences.isKey(key.c_str())) {
        preferences.remove(key.c_str());
        Serial.printf("移除未使用的时间点 %d\n", i);
      }
    }
    
    preferences.end();
    Serial.println("配置保存完成");
    
    String msg = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    msg += "<title>保存成功</title></head><body>";
    msg += "<h1>配置已保存，设备将重启...</h1>";
    msg += "</body></html>";
    server.send(200, "text/html; charset=utf-8", msg);
    delay(1000);
    ESP.restart();
  });
  
  // 添加重置配置的路由
  server.on("/reset", HTTP_POST, [](){
    // 清除NVS分区中的所有配置
    preferences.begin("config", false);
    preferences.clear();
    preferences.end();
    
    String msg = "配置已重置，设备将重启";
    server.send(200, "text/plain; charset=utf-8", msg);
    delay(1000);
    ESP.restart();
  });

  // 添加删除时间点的路由
  server.on("/delete-time", HTTP_POST, [](){
    if (server.hasArg("index")) {
      int index = server.arg("index").toInt();
      if(index >= 0 && index < wake_times_count) {
        // 从内存中移除时间点
        for(int i = index; i < wake_times_count - 1; i++) {
          strcpy(wake_times[i], wake_times[i + 1]);
        }
        wake_times_count--;
        
        // 保存更新后的配置
        preferences.begin("config", false);
        preferences.putInt("wake_times_count", wake_times_count);
        
        // 更新所有时间点
        for(int i = 0; i < wake_times_count; i++) {
          String key = "wake_time_" + String(i);
          preferences.putString(key.c_str(), wake_times[i]);
        }
        
        // 删除多余的时间点
        String key = "wake_time_" + String(wake_times_count);
        if(preferences.isKey(key.c_str())) {
          preferences.remove(key.c_str());
        }
        
        preferences.end();
        server.send(200, "text/plain", "时间点已删除");
      } else {
        server.send(400, "text/plain", "无效的时间点索引");
      }
    } else {
      server.send(400, "text/plain", "缺少参数");
    }
  });

  // 添加单个时间点保存的路由
  server.on("/save-single", HTTP_POST, [](){
    if (server.hasArg("index") && server.hasArg("time")) {
      int index = server.arg("index").toInt();
      String timeValue = server.arg("time");
      
      if(index < 0 || index >= MAX_WAKE_TIMES) {
        server.send(400, "text/plain", "无效的时间点索引");
        return;
      }
      
      // 验证时间格式
      int hour, minute;
      if(sscanf(timeValue.c_str(), "%d:%d", &hour, &minute) == 2 && 
         hour >= 0 && hour < 24 && minute >= 0 && minute < 60) {
        
        preferences.begin("config", false);
        
        // 读取当前的wake_times_count
        wake_times_count = preferences.getInt("wake_times_count", 1);
        
        // 格式化为标准格式 (HH:MM)
        snprintf(wake_times[index], sizeof(wake_times[index]), 
                "%02d:%02d", hour, minute);
        
        // 保存时间点
        String key = "wake_time_" + String(index);
        preferences.putString(key.c_str(), wake_times[index]);
        
        // 如果是新增的时间点，更新计数器
        if(index >= wake_times_count) {
          wake_times_count = index + 1;
          preferences.putInt("wake_times_count", wake_times_count);
        }
        
        preferences.end();
        
        Serial.printf("已保存时间点 %d: %s (总数: %d)\n", 
                     index, wake_times[index], wake_times_count);
        
        // 重新加载所有配置以确保同步
        loadConfig();
        
        server.send(200, "text/plain", "保存成功");
      } else {
        Serial.printf("无效的时间格式: %s\n", timeValue.c_str());
        server.send(400, "text/plain", "无效的时间格式");
      }
    } else {
      server.send(400, "text/plain", "缺少参数");
    }
  });

  server.begin();
  
  // 显示AP信息
  display->fillScreen(GxEPD_WHITE);
  u8g2Font.setCursor(10, 30);
  u8g2Font.print("配置模式已启动");
  u8g2Font.setCursor(10, 60);
  u8g2Font.print("SSID: ESP32-Config");
  u8g2Font.setCursor(10, 90);
  u8g2Font.print("IP: 192.168.4.1");
  u8g2Font.setCursor(10, 120);
  u8g2Font.print("短按按键可退出配置模式");
  display->display(true);
  
  Serial.println("AP模式已启动，短按按键可退出配置模式");
}

void drawPage() {
  if(apModeEnabled) return;
  
  display->fillScreen(GxEPD_WHITE);
  
  // 顶部日期显示
  display->setFont(&FreeMonoBold18pt7b);
  display->setTextColor(GxEPD_RED);
  int16_t x1, y1;
  uint16_t dateWidth, dateHeight;
  display->getTextBounds(lastDate, 0, 0, &x1, &y1, &dateWidth, &dateHeight);
  
  // 计算日期前缀的宽度
  int prefixWidth = 0;
  if(datePrefix.length() > 0) {
    // 使用与MQTT消息相同的字体计算前缀宽度
    prefixWidth = u8g2Font.getUTF8Width(datePrefix.c_str()) + 5; // 添加5像素间距
  }
  
  // 显示日期，保持居中位置
  display->setCursor((display->width() - dateWidth)/2, dateHeight + 10);
  display->print(lastDate);
  
  // 添加"今日"或"明日"标识，使用MQTT消息相同的字体
  if(datePrefix.length() > 0) {
    u8g2Font.setForegroundColor(GxEPD_RED);  // 设置前缀为红色
    u8g2Font.setCursor((display->width() - dateWidth)/2 - prefixWidth, dateHeight + 10);
    u8g2Font.print(datePrefix);
    u8g2Font.setForegroundColor(GxEPD_BLACK);  // 恢复默认颜色
  }

  // 添加星期显示，使用MQTT消息相同的字体
  if(weekDay.length() > 0) {
    u8g2Font.setForegroundColor(GxEPD_RED);  // 设置星期为红色
    u8g2Font.setCursor(display->width()/2 + dateWidth/2 + 10, dateHeight + 10);
    u8g2Font.print(weekDay);
    u8g2Font.setForegroundColor(GxEPD_BLACK);  // 恢复默认颜色
  }

  // 计算显示区域
  int columnWidth = display->width() / 2;
  int startY = dateHeight + 35;
  int lineHeight = 30;
  int maxLinesPerPage = (display->height() - startY - 20) / lineHeight;

  // 显示所有消息
  int leftLines = 0;
  int rightLines = 0;
  
  for (int i = 0; i < messageCount; i++) {
    bool useLeftColumn = (leftLines <= rightLines);
    int column = useLeftColumn ? 10 : (columnWidth + 10);
    int y = startY + (useLeftColumn ? leftLines : rightLines) * lineHeight;
    
    u8g2Font.setCursor(column, y);
    u8g2Font.print("• " + messages[i]);
    
    if (useLeftColumn) leftLines++;
    else rightLines++;
  }
  
  display->display(true);
}

void processMessages() {
  messageCount = 0;
  
  for (int i = 0; i < lastEvents.length() && messageCount < 50; ) {
    int nlPos = lastEvents.indexOf('\n', i);
    if (nlPos == -1) nlPos = lastEvents.length();
    
    String msg = lastEvents.substring(i, nlPos);
    msg.trim();
    
    if (msg.length() > 0) {  // 修复这里，添加了括号()
      messages[messageCount++] = msg;
    }
    i = nlPos + 1;
  }

  // 不再需要分页计算
}

void callback(char* topic, byte* payload, unsigned int length) {
  if(apModeEnabled) return;
  
  mqttWakeFlag = true;
  String json;
  for(unsigned int i = 0; i < length; i++) {
    json += (char)payload[i];
  }

  JsonDocument doc;
  deserializeJson(doc, json);

  lastDate = doc["日期"].as<String>();
  
  // 从日期字符串计算星期
  int year, month, day;
  if(sscanf(lastDate.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
    struct tm timeinfo = {0};
    timeinfo.tm_year = year - 1900;  // 年份从1900年开始
    timeinfo.tm_mon = month - 1;     // 月份从0开始
    timeinfo.tm_mday = day;
    mktime(&timeinfo);  // 这会自动计算星期
    
    // 转换星期为中文
    const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    weekDay = String("周") + weekdays[timeinfo.tm_wday];
    
    // 比较NTP时间和MQTT日期，确定是"今日"还是"明日"
    time_t now;
    struct tm timeinfo_now;
    time(&now);
    localtime_r(&now, &timeinfo_now);
    
    // 创建MQTT日期的time_t
    struct tm mqtt_tm = {0};
    mqtt_tm.tm_year = year - 1900;
    mqtt_tm.tm_mon = month - 1;
    mqtt_tm.tm_mday = day;
    mqtt_tm.tm_hour = 0;
    mqtt_tm.tm_min = 0;
    mqtt_tm.tm_sec = 0;
    time_t mqtt_time = mktime(&mqtt_tm);
    
    // 创建今天0点的time_t
    struct tm today_tm = timeinfo_now;
    today_tm.tm_hour = 0;
    today_tm.tm_min = 0;
    today_tm.tm_sec = 0;
    time_t today_time = mktime(&today_tm);
    
    // 创建明天0点的time_t
    struct tm tomorrow_tm = today_tm;
    tomorrow_tm.tm_mday += 1;
    time_t tomorrow_time = mktime(&tomorrow_tm);
    
    // 判断MQTT日期是今天还是明天
    if(mqtt_time >= today_time && mqtt_time < tomorrow_time) {
      datePrefix = "今日";
    } else if(mqtt_time >= tomorrow_time && mqtt_time < tomorrow_time + 24*3600) {
      datePrefix = "明日";
    } else {
      datePrefix = ""; // 如果不是今天或明天，不显示前缀
    }
  } else {
    weekDay = "";  // 如果日期格式不正确，不显示星期
    datePrefix = ""; // 同样不显示前缀
  }
  
  lastEvents = "";
  
  for(JsonVariant item : doc["data"].as<JsonArray>()) {
    lastEvents += item.as<String>() + "\n";
  }

  processMessages();
  drawPage();
}

void reconnect() {
  if(apModeEnabled) return;
  
  // 检查WiFi连接状态
  if (WiFi.status() != WL_CONNECTED) {
    currentState = STATE_WIFI_CONNECTING;
    Serial.println("尝试连接WiFi...");
    WiFi.begin(wifi_ssid, wifi_password);
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi连接成功");
      Serial.println("IP地址: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("WiFi连接失败");
      return;
    }
  }

  // 连接MQTT
  currentState = STATE_MQTT_CONNECTING;
  
  // 添加MQTT连接重试次数限制
  int mqtt_retries = 0;
  const int MAX_MQTT_RETRIES = 3;  // 最多尝试3次
  
  while (!client.connected() && mqtt_retries < MAX_MQTT_RETRIES) {
    Serial.println("尝试连接MQTT...");
    Serial.printf("服务器: %s, 端口: %d, 用户: %s\n", mqtt_server, mqtt_port, mqtt_user);
    
    String clientId = "ESP32Calendar-" + String(random(0xffff), HEX);
    bool success = false;
    
    if (strlen(mqtt_user) > 0) {
      success = client.connect(clientId.c_str(), mqtt_user, mqtt_pass);
    } else {
      success = client.connect(clientId.c_str());
    }
    
    if (success) {
      client.subscribe(mqtt_topic);
      currentState = STATE_NORMAL;
      Serial.println("MQTT连接成功");
      break;  // 连接成功，跳出循环
    } else {
      mqtt_retries++;
      Serial.printf("MQTT连接失败，错误码: %d, 重试 %d/%d\n", 
                   client.state(), mqtt_retries, MAX_MQTT_RETRIES);
      delay(5000);
    }
  }
  
  // 如果MQTT连接失败，但WiFi连接正常，仍然可以继续运行
  if (!client.connected()) {
    Serial.println("MQTT连接失败，但将继续运行");
    // 可以在这里添加错误处理，例如显示错误信息
  }
}

void setup() {
  Serial.begin(115200);
  
  // 初始化LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  currentState = STATE_INIT;

  // 检查唤醒原因
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if(wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("唤醒原因: 按键按下");
    
    // 先加载配置，再初始化显示屏
    loadConfig();
    initDisplay();
    
    // 如果是按键唤醒，立即检测长按
    unsigned long pressStartTime = millis();
    bool longPressDetected = false;
    
    // 显示提示信息
    display->fillScreen(GxEPD_WHITE);
    u8g2Font.setCursor(10, 30);
    u8g2Font.print("检测按键...");
    u8g2Font.setCursor(10, 60);
    u8g2Font.print("长按进入配置模式");
    u8g2Font.setCursor(10, 90);
    u8g2Font.print("短按刷新数据");
    display->display(false); // 快速显示
    
    // 等待并检测长按
    while(digitalRead(BUTTON_PIN) == LOW) { // 按键仍然按下
      if(millis() - pressStartTime >= 2000) { // 长按超过2秒
        longPressDetected = true;
        break;
      }
      delay(10);
    }
    
    if(longPressDetected) {
      Serial.println("长按检测到，进入AP模式");
      startAPMode();
      initButton(); // 初始化按钮中断
      return; // 直接返回到loop
    }
    
    Serial.println("短按检测到，继续正常启动");
  } else if(wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)) {
      char timeStr[10];
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
      Serial.printf("唤醒原因: 定时器 (%s)\n", timeStr);
    } else {
      Serial.println("唤醒原因: 定时器 (时间未同步)");
    }
    
    // 先加载配置，再初始化显示屏
    loadConfig();
    initDisplay();
  } else {
    Serial.println("唤醒原因: 上电或复位");
    
    // 先加载配置，再初始化显示屏
    loadConfig();
    initDisplay();
  }
  
  initButton();

  // 检查是否有配置信息
  bool hasConfig = strlen(wifi_ssid) > 0;
  
  // 尝试连接WiFi
  if (hasConfig) {
    Serial.println("尝试连接WiFi: " + String(wifi_ssid));
    WiFi.begin(wifi_ssid, wifi_password);
    
    // 增加连接重试次数，给予更多时间连接
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    
    Serial.println("");
  }

  // 如果没有配置或连接失败，且是通过按键唤醒，则进入AP模式
  if (WiFi.status() != WL_CONNECTED) {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      // 按键唤醒且WiFi连接失败，进入AP模式
      startAPMode();
    } else {
      // 其他唤醒原因（定时器或上电），显示错误信息
      display->fillScreen(GxEPD_WHITE);
      u8g2Font.setCursor(10, 30);
      u8g2Font.print("WiFi连接失败");
      u8g2Font.setCursor(10, 60);
      u8g2Font.print("请长按按钮进入配置模式");
      display->display(true);
      
      // 设置按键唤醒并进入睡眠
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, LOW);
      delay(5000);
      esp_deep_sleep_start();
    }
  } else {
    // 同步时间
    syncTime();
    
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
    client.setBufferSize(4096);
    reconnect();

    lastDate = "等待数据...";
    lastEvents = "【示例】消息1\n【示例】消息2\n【示例】消息3\n【示例】消息4\n";
    processMessages();
    drawPage();
  }
}

// 退出AP模式的函数
void exitAPMode() {
  apModeEnabled = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  
  // 显示退出AP模式的提示
  display->fillScreen(GxEPD_WHITE);
  u8g2Font.setCursor(10, 30);
  u8g2Font.print("退出配置模式");
  u8g2Font.setCursor(10, 60);
  u8g2Font.print("正在重新连接WiFi...");
  display->display(false);
  
  Serial.println("退出AP模式");
  
  // 重新连接WiFi
  WiFi.begin(wifi_ssid, wifi_password);
  
  // 等待WiFi连接
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi连接成功");
    
    // 同步时间
    syncTime();
    
    // 重新连接MQTT
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(callback);
    client.setBufferSize(4096);
    
    // 尝试连接MQTT
    if (!client.connected()) {
      reconnect();
    }
    
    // 显示连接成功
    display->fillScreen(GxEPD_WHITE);
    u8g2Font.setCursor(10, 30);
    u8g2Font.print("WiFi连接成功");
    u8g2Font.setCursor(10, 60);
    u8g2Font.print("正在获取数据...");
    display->display(false);
    
    // 请求刷新数据
    if (client.connected()) {
      client.publish(mqtt_topic, "refresh");
    }
  } else {
    Serial.println("\nWiFi连接失败");
    display->fillScreen(GxEPD_WHITE);
    u8g2Font.setCursor(10, 30);
    u8g2Font.print("WiFi连接失败");
    u8g2Font.setCursor(10, 60);
    u8g2Font.print("请检查网络设置");
    display->display(false);
    delay(3000);
    ESP.restart();  // 如果连接失败，重启设备
  }
}

void loop() {
  static unsigned long pressStartTime = 0;
  static bool longPressHandled = false;
  
  if(apModeEnabled) {
    server.handleClient();
    
    // 在AP模式下检测按键
    bool currentButtonState = (digitalRead(BUTTON_PIN) == LOW);
    
    // 如果按键被按下且之前没有记录时间
    if (currentButtonState && pressStartTime == 0) {
      pressStartTime = millis();
      longPressHandled = false;
    }
    
    // 如果按键释放
    if (!currentButtonState && pressStartTime > 0) {
      unsigned long pressDuration = millis() - pressStartTime;
      pressStartTime = 0;  // 重置计时器
      
      // 短按退出AP模式
      if (!longPressHandled && pressDuration < 2000) {
        exitAPMode();
      }
    }
    
    return;
  }
  
  // 检测按键状态
  bool currentButtonState = (digitalRead(BUTTON_PIN) == LOW);  // 低电平表示按下
  
  // 如果按键被按下且之前没有记录时间
  if (currentButtonState && pressStartTime == 0) {
    pressStartTime = millis();
    longPressHandled = false;
  }
  
  // 如果按键正在被按下，检查是否达到长按时间
  if (currentButtonState && pressStartTime > 0 && !longPressHandled) {
    unsigned long pressDuration = millis() - pressStartTime;
    if (pressDuration >= 2000) {  // 长按超过2秒
      longPressHandled = true;
      startAPMode();
      Serial.println("长按检测到，进入AP模式");
    }
  }
  
  // 如果按键释放
  if (!currentButtonState && pressStartTime > 0) {
    unsigned long pressDuration = millis() - pressStartTime;
    pressStartTime = 0;  // 重置计时器
    
    if (!longPressHandled && pressDuration < 2000) {  // 短按
      // 短按刷新数据
      if (client.connected()) {
        client.publish(mqtt_topic, "refresh");
        Serial.println("短按检测到，刷新数据");
      }
    }
  }

  // 更新LED状态
  updateLedState();

  // 处理网络请求
  if(apModeEnabled) {
    currentState = STATE_AP_MODE;
    server.handleClient();
  } else {
    if (!client.connected()) {
      currentState = STATE_MQTT_CONNECTING;
      reconnect();
    } else {
      currentState = STATE_NORMAL;
    }
    client.loop();
    
    // 进入深度睡眠模式，等待按键唤醒或定时唤醒
    if(!apModeEnabled && messageCount > 0 && lastDate != "等待数据...") {
      currentState = STATE_SLEEP_PREPARE;
      
      // 设置按键唤醒
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, LOW);
      
      // 设置定时唤醒
      struct tm timeinfo;
      if(getLocalTime(&timeinfo)) {
        time_t now;
        time(&now);
        
        // 找出最近的唤醒时间
        time_t next_wake = now + 24*3600; // 默认24小时后
        
        for(int i = 0; i < wake_times_count; i++) {
          int hour, minute;
          if(sscanf(wake_times[i], "%d:%d", &hour, &minute) == 2) {
            struct tm wake_tm;
            localtime_r(&now, &wake_tm);
            wake_tm.tm_hour = hour;
            wake_tm.tm_min = minute;
            wake_tm.tm_sec = 0;
            
            // 如果当前时间已经过了这个时间点，设置为明天
            if (hour < timeinfo.tm_hour || 
                (hour == timeinfo.tm_hour && minute <= timeinfo.tm_min)) {
              wake_tm.tm_mday += 1;
            }
            
            time_t wake_time_t = mktime(&wake_tm);
            
            // 更新为最近的唤醒时间
            if(wake_time_t > now && wake_time_t < next_wake) {
              next_wake = wake_time_t;
            }
          }
        }
        
        // 计算睡眠时间（秒）
        uint64_t sleep_time = next_wake - now;
        
        // 转换为微秒
        uint64_t sleep_time_us = sleep_time * 1000000ULL;
        
        // 设置定时唤醒
        esp_sleep_enable_timer_wakeup(sleep_time_us);
        
        // 转换为可读时间
        struct tm *next_tm = localtime(&next_wake);
        Serial.printf("设置定时唤醒: %02d:%02d:%02d, 睡眠时间: %lld 秒\n", 
                      next_tm->tm_hour, next_tm->tm_min, next_tm->tm_sec, sleep_time);
      } else {
        Serial.println("获取时间失败，只设置按键唤醒");
      }
      
      Serial.println("进入深度睡眠，等待按键或定时唤醒");
      esp_deep_sleep_start();
    }
  }
}