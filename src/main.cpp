#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <LiquidCrystal_I2C.h>

#define BUZZER_PIN 27

//LCD Setup
LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi credentials
const char* ssid = "Surya";
const char* password = "Aronsurya12";

// MQTT Broker settings
const char* mqtt_broker = "0d85364bea6c4d089979b536d2013b26.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_username = "hivemq.webclient.1761715579377";
const char* mqtt_password = "X8ex*bB3%Z6?YWuaA5n.";
const char* client_id = "ESP32_Scheduler";

// MQTT Topics
const char* topic_schedule_command = "scheduler/command";
const char* topic_schedule_status = "scheduler/status";
const char* topic_task_execute = "scheduler/execute";
const char* topic_heartbeat = "scheduler/heartbeat";

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 25200;
const int daylightOffset_sec = 0;

WiFiClientSecure espClient;
PubSubClient mqtt_client(espClient);

// Task Structure
struct Task {
  String id;
  String name;
  String action;
  int hour;
  int minute;
  String days;
  int priority;
  bool enabled;
  unsigned long lastExecuted;
  bool executed_today;
};

// Forward declarations
void sendStatusUpdate(const char* status, const char* message);
void sendTaskExecution(Task & task, const char* result);
void sendHeartbeat();
void showLCD(const String &line1, const String &line2 = "");
void buzz(int duration = 200);
void executeTask(Task &task);

// Task Queue
Task taskQueue[10];
int taskCount = 0;

// System Status
bool ntpSynced = false;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000;

// ==================== LCD & BUZZER ====================

void showLCD(const String &line1, const String &line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

void buzz(int duration) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(duration);
  digitalWrite(BUZZER_PIN, LOW);
}

// ==================== TIME FUNCTIONS ====================

void syncNTP() {
  Serial.println("[NTP] Syncing time with NTP server...");
  showLCD("Syncing Time", "Please wait...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    Serial.print(".");
    delay(1000);
    retry++;
  }
  
  if (retry < 10) {
    Serial.println("\n[SUCCESS] NTP time synced!");
    Serial.println(&timeinfo, "Current time: %Y-%m-%d %H:%M:%S");
    ntpSynced = true;
    showLCD("Time Synced", "Success!");
    delay(1000);
  } else {
    Serial.println("\n[ERROR] Failed to sync NTP time");
    ntpSynced = false;
    showLCD("Time Sync", "Failed!");
    buzz(1000);
    delay(1000);
  }
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Time not synced";
  }
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void getCurrentTimeComponents(int &hour, int &minute, int &dayOfWeek) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    dayOfWeek = timeinfo.tm_wday;
  }
}

// ==================== MQTT FUNCTIONS ====================

void reconnect() {
  int retryCount = 0;
  
  while (!mqtt_client.connected() && retryCount < 5) {
    Serial.println("\n==================================================");
    Serial.print("[MQTT] Connection Attempt #");
    Serial.println(retryCount + 1);
    Serial.println("==================================================");
    Serial.print("Broker: ");
    Serial.println(mqtt_broker);
    Serial.print("Port: ");
    Serial.println(mqtt_port);
    Serial.print("Client ID: ");
    Serial.println(client_id);
    Serial.print("Username: ");
    Serial.println(mqtt_username);
    
    showLCD("MQTT Connecting", "Attempt " + String(retryCount + 1));
    
    if (mqtt_client.connect(client_id, mqtt_username, mqtt_password)) {
      Serial.println("\n[SUCCESS] MQTT CONNECTION SUCCESS!");
      Serial.println("==================================================");
      Serial.println("[INFO] Subscribed Topics:");
      Serial.println("   - " + String(topic_schedule_command) + " (receive commands)");
      Serial.println("\n[INFO] Publishing Topics:");
      Serial.println("   - " + String(topic_schedule_status) + " (status updates)");
      Serial.println("   - " + String(topic_task_execute) + " (task execution reports)");
      Serial.println("   - " + String(topic_heartbeat) + " (heartbeat every 30s)");
      Serial.println("==================================================");
      
      showLCD("MQTT Connected", "HiveMQ Cloud");
      buzz(200);
      delay(500);
      buzz(200);
      
      mqtt_client.subscribe(topic_schedule_command);
      Serial.println("\n[SUCCESS] Subscribed to: " + String(topic_schedule_command));
      Serial.println("[INFO] Send JSON command to this topic to control ESP32\n");
      
      sendStatusUpdate("online", "Device connected and ready");
      return;
      
    } else {
      int errorCode = mqtt_client.state();
      Serial.println("\n[ERROR] MQTT CONNECTION FAILED!");
      Serial.print("Error Code: ");
      Serial.println(errorCode);
      
      // Detailed error explanation
      switch(errorCode) {
        case -4:
          Serial.println("[ERROR] MQTT_CONNECTION_TIMEOUT");
          Serial.println("  Network issue or broker not responding");
          showLCD("MQTT Error", "Timeout");
          break;
        case -3:
          Serial.println("[ERROR] MQTT_CONNECTION_LOST");
          Serial.println("  Connection dropped");
          showLCD("MQTT Error", "Conn Lost");
          break;
        case -2:
          Serial.println("[ERROR] MQTT_CONNECT_FAILED");
          Serial.println("  Network cannot connect to broker");
          showLCD("MQTT Error", "Connect Fail");
          break;
        case -1:
          Serial.println("[ERROR] MQTT_DISCONNECTED");
          showLCD("MQTT Error", "Disconnected");
          break;
        case 1:
          Serial.println("[ERROR] MQTT_CONNECT_BAD_PROTOCOL");
          Serial.println("  Protocol version mismatch");
          showLCD("MQTT Error", "Bad Protocol");
          break;
        case 2:
          Serial.println("[ERROR] MQTT_CONNECT_BAD_CLIENT_ID");
          Serial.println("  Client ID rejected");
          showLCD("MQTT Error", "Bad ClientID");
          break;
        case 3:
          Serial.println("[ERROR] MQTT_CONNECT_UNAVAILABLE");
          Serial.println("  Server unavailable");
          showLCD("MQTT Error", "Unavailable");
          break;
        case 4:
          Serial.println("[ERROR] MQTT_CONNECT_BAD_CREDENTIALS");
          Serial.println("  *** USERNAME/PASSWORD INCORRECT! ***");
          Serial.println("  Credentials may have expired!");
          Serial.println("  Create new credentials in HiveMQ Console");
          showLCD("MQTT Error", "Bad Credential");
          break;
        case 5:
          Serial.println("[ERROR] MQTT_CONNECT_UNAUTHORIZED");
          Serial.println("  Client does not have permission");
          showLCD("MQTT Error", "Unauthorized");
          break;
        default:
          Serial.println("[ERROR] UNKNOWN ERROR");
          showLCD("MQTT Error", "Unknown");
      }
      
      Serial.println("==================================================");
      buzz(1000);
      retryCount++;
      
      if (retryCount < 5) {
        Serial.println("Retrying in 5 seconds...\n");
        delay(5000);
      }
    }
  }
  
  if (!mqtt_client.connected()) {
    Serial.println("\n[ERROR] MQTT CONNECTION FAILED AFTER 5 ATTEMPTS!");
    Serial.println("[WARNING] Device will run without MQTT");
    Serial.println("[WARNING] Check credentials and try restarting device\n");
    showLCD("MQTT Failed", "No Connection");
    buzz(1000);
    delay(3000);
  }
}

void sendStatusUpdate(const char* status, const char* message) {
  if (!mqtt_client.connected()) return;
  
  StaticJsonDocument<300> doc;
  doc["device"] = client_id;
  doc["status"] = status;
  doc["message"] = message;
  doc["time"] = getCurrentTime();
  doc["tasks_loaded"] = taskCount;
  doc["ntp_synced"] = ntpSynced;
  
  char buffer[400];
  serializeJson(doc, buffer);
  
  if (mqtt_client.publish(topic_schedule_status, buffer)) {
    Serial.println("[PUBLISH] Status published to: " + String(topic_schedule_status));
  }
}

void sendTaskExecution(Task &task, const char* result) {
  if (!mqtt_client.connected()) return;
  
  StaticJsonDocument<400> doc;
  doc["device"] = client_id;
  doc["task_id"] = task.id;
  doc["task_name"] = task.name;
  doc["action"] = task.action;
  doc["result"] = result;
  doc["executed_at"] = getCurrentTime();
  doc["priority"] = task.priority;
  
  char buffer[500];
  serializeJson(doc, buffer);
  
  if (mqtt_client.publish(topic_task_execute, buffer)) {
    Serial.println("[PUBLISH] Task execution report published to: " + String(topic_task_execute));
  }
}

void sendHeartbeat() {
  if (!mqtt_client.connected()) return;
  
  StaticJsonDocument<200> doc;
  doc["device"] = client_id;
  doc["uptime"] = millis() / 1000;
  doc["time"] = getCurrentTime();
  doc["tasks"] = taskCount;
  doc["free_heap"] = ESP.getFreeHeap();
  
  char buffer[300];
  serializeJson(doc, buffer);
  
  if (mqtt_client.publish(topic_heartbeat, buffer)) {
    Serial.println("[HEARTBEAT] Published to: " + String(topic_heartbeat));
  }
}

// ==================== TASK MANAGEMENT ====================

bool shouldExecuteToday(Task &task, int dayOfWeek) {
  if (task.days == "daily") return true;
  if (task.days == "weekday" && dayOfWeek >= 1 && dayOfWeek <= 5) return true;
  if (task.days == "weekend" && (dayOfWeek == 0 || dayOfWeek == 6)) return true;
  
  String dayStr = String((dayOfWeek == 0) ? 7 : dayOfWeek);
  return task.days.indexOf(dayStr) >= 0;
}

void executeTask(Task &task) {
  Serial.println("\n==================================================");
  Serial.println("[EXECUTE] EXECUTING TASK");
  buzz(500);

  showLCD("Running Task:", task.name);
  Serial.println("Task ID: " + task.id);
  Serial.println("Task Name: " + task.name);
  Serial.println("Action: " + task.action);
  Serial.println("Priority: " + String(task.priority));
  Serial.println("Time: " + getCurrentTime());
  
  bool success = true;
  String result = "success";
  
  if (task.action == "led_blink") {
    Serial.println("[ACTION] LED Blink Pattern");
    for (int i = 0; i < 5; i++) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
      delay(200);
    }
  } 
  else if (task.action == "serial_message") {
    Serial.println("[ACTION] Serial Message Broadcast");
    Serial.println(">>> SCHEDULED MESSAGE: " + task.name + " <<<");
  }
  else if (task.action == "mqtt_broadcast") {
    Serial.println("[ACTION] MQTT Broadcast");
    StaticJsonDocument<200> broadcastDoc;
    broadcastDoc["type"] = "scheduled_broadcast";
    broadcastDoc["task"] = task.name;
    broadcastDoc["time"] = getCurrentTime();
    
    char broadcastBuffer[300];
    serializeJson(broadcastDoc, broadcastBuffer);
    if (mqtt_client.connected()) {
      mqtt_client.publish("scheduler/broadcast", broadcastBuffer);
    }
  }
  else if (task.action == "status_report") {
    Serial.println("[ACTION] Status Report");
    sendStatusUpdate("task_executed", ("Executed: " + task.name).c_str());
  }
  else {
    Serial.println("[ERROR] Unknown action: " + task.action);
    result = "unknown_action";
    success = false;
  }
  
  task.lastExecuted = millis();
  task.executed_today = true;
  
  Serial.println("Result: " + result);
  Serial.println("==================================================\n");
  showLCD("Done:", task.name);
  delay(2000);
  lcd.clear();

  sendTaskExecution(task, result.c_str());
}

void checkAndExecuteTasks() {
  if (!ntpSynced) return;
  
  int currentHour, currentMinute, dayOfWeek;
  getCurrentTimeComponents(currentHour, currentMinute, dayOfWeek);
  
  static int lastHour = -1;
  if (currentHour == 0 && lastHour == 23) {
    for (int i = 0; i < taskCount; i++) {
      taskQueue[i].executed_today = false;
    }
    Serial.println("[INFO] New day - reset execution flags");
  }
  lastHour = currentHour;
  
  for (int i = 0; i < taskCount; i++) {
    Task &task = taskQueue[i];
    
    if (!task.enabled) continue;
    if (task.executed_today) continue;
    if (!shouldExecuteToday(task, dayOfWeek)) continue;
    
    if (task.hour == currentHour && task.minute == currentMinute) {
      executeTask(task);
    }
  }
}

void addOrUpdateTask(JsonObject taskObj) {
  String taskId = taskObj["id"].as<String>();
  
  int index = -1;
  for (int i = 0; i < taskCount; i++) {
    if (taskQueue[i].id == taskId) {
      index = i;
      break;
    }
  }
  
  if (index == -1) {
    if (taskCount >= 10) {
      Serial.println("[WARNING] Task queue full!");
      sendStatusUpdate("error", "Task queue full");
      buzz(1000);
      return;
    }
    index = taskCount++;
  }
  
  Task &task = taskQueue[index];
  task.id = taskId;
  task.name = taskObj["name"].as<String>();
  task.action = taskObj["action"].as<String>();
  task.hour = taskObj["hour"];
  task.minute = taskObj["minute"];
  task.days = taskObj["days"].as<String>();
  task.priority = taskObj["priority"] | 2;
  task.enabled = taskObj["enabled"] | true;
  task.executed_today = false;
  
  Serial.println("[SUCCESS] Task added/updated: " + task.name);
  buzz(300);
  sendStatusUpdate("task_updated", ("Task: " + task.name).c_str());
}

void deleteTask(String taskId) {
  for (int i = 0; i < taskCount; i++) {
    if (taskQueue[i].id == taskId) {
      for (int j = i; j < taskCount - 1; j++) {
        taskQueue[j] = taskQueue[j + 1];
      }
      taskCount--;
      Serial.println("[DELETE] Task deleted: " + taskId);
      buzz(200);
      sendStatusUpdate("task_deleted", taskId.c_str());
      return;
    }
  }
}

void listTasks() {
  Serial.println("\n[TASKS] Current Tasks:");
  Serial.println("==================================================");
  for (int i = 0; i < taskCount; i++) {
    Task &task = taskQueue[i];
    Serial.printf("%d. [%s] %s - %02d:%02d (%s) - Priority:%d - %s\n",
                  i + 1,
                  task.enabled ? "ENABLED" : "DISABLED",
                  task.name.c_str(),
                  task.hour,
                  task.minute,
                  task.days.c_str(),
                  task.priority,
                  task.action.c_str());
  }
  Serial.println("==================================================\n");
}

// ==================== MQTT CALLBACK ====================

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println("\n==================================================");
  Serial.print("[MQTT] Message received on topic: ");
  Serial.println(topic);
  showLCD("MQTT Msg:", topic);

  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  
  Serial.print("Payload: ");
  Serial.println(message);
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.println("[ERROR] JSON parse error: " + String(error.c_str()));
    Serial.println("==================================================");
    buzz(1000);
    return;
  }
  
  String command = doc["command"].as<String>();
  Serial.println("Command: " + command);
  showLCD("Cmd:", command);
  buzz(200);

  if (command == "add_task") {
    Serial.println("[CMD] Processing: Add Task");
    JsonObject taskObj = doc["task"];
    addOrUpdateTask(taskObj);
  }
  else if (command == "delete_task") {
    String taskId = doc["task_id"].as<String>();
    Serial.println("[CMD] Processing: Delete Task " + taskId);
    deleteTask(taskId);
  }
  else if (command == "list_tasks") {
    Serial.println("[CMD] Processing: List Tasks");
    listTasks();
  }
  else if (command == "sync_time") {
    Serial.println("[CMD] Processing: Sync Time");
    syncNTP();
  }
  else if (command == "execute_now") {
    String taskId = doc["task_id"].as<String>();
    Serial.println("[CMD] Processing: Execute Task " + taskId);
    for (int i = 0; i < taskCount; i++) {
      if (taskQueue[i].id == taskId) {
        executeTask(taskQueue[i]);
        break;
      }
    }
  }
  else {
    Serial.println("[WARNING] Unknown command: " + command);
  }
  
  Serial.println("==================================================\n");
}

// ==================== SETUP & LOOP ====================

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  Serial.begin(115200);
  delay(1000);
  
  lcd.init();
  lcd.backlight();
  showLCD("Smart Scheduler", "ESP32 IoT");
  buzz(200);
  delay(2000);

  pinMode(LED_BUILTIN, OUTPUT);
  
  Serial.println("\n\n==================================================");
  Serial.println("[START] ESP32 Smart Scheduler IoT");
  Serial.println("        with MQTT & Enhanced Debugging");
  Serial.println("==================================================\n");
  
  // Connect WiFi
  Serial.println("[WIFI] Connecting to WiFi...");
  showLCD("Connecting WiFi", ssid);
  WiFi.begin(ssid, password);
  
  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 20) {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SUCCESS] WiFi connected");
    Serial.println("IP: " + WiFi.localIP().toString());
    showLCD("WiFi Connected", WiFi.localIP().toString());
    buzz(200);
    delay(2000);
  } else {
    Serial.println("\n[ERROR] WiFi connection failed!");
    showLCD("WiFi Failed", "Check SSID/Pass");
    buzz(1000);
    delay(5000);
    ESP.restart();
  }
  
  // Sync NTP
  syncNTP();
  
  // Configure MQTT
  espClient.setInsecure();
  mqtt_client.setServer(mqtt_broker, mqtt_port);
  mqtt_client.setCallback(callback);
  mqtt_client.setKeepAlive(90);
  mqtt_client.setBufferSize(2048);
  mqtt_client.setSocketTimeout(30);
  
  // Connect MQTT
  reconnect();
  
  Serial.println("\n[SUCCESS] Setup complete!");
  Serial.println("[INFO] Use MQTT Explorer to send commands");
  Serial.println("[INFO] Subscribe to 'scheduler/#' to monitor all topics");
  Serial.println("==================================================\n");
  showLCD("System Ready", getCurrentTime());
  buzz(300);
  delay(2000);
}

void loop() {
  // Maintain MQTT connection
  if (!mqtt_client.connected()) {
    Serial.println("\n[WARNING] MQTT disconnected, attempting reconnect...");
    reconnect();
  }
  mqtt_client.loop();
  
  // Check tasks every minute
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck >= 60000 || lastCheck == 0) {
    checkAndExecuteTasks();
    lastCheck = millis();
  }
  
  // Send heartbeat
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
  
  delay(100);
}