/*
 * Tầng 1 trên một ESP8266 NodeMCU: gộp cua_chinh + nha_xe_moi_nhat + phần DHT11/LCD của bao_trom_va_do_am.
 *
 *   - Cửa chính: bàn phím 4x4 (I2C), LCD 16x2 thứ nhất, servo, còi bíp
 *   - Cửa garage: motor qua kênh A của L298N, mở/đóng bằng phím B hoặc từ web/app
 *   - Quạt tầng 1: kênh B của L298N
 *   - Đèn trong nhà tầng 1: 2 dây LED qua 2 module MOSFET
 *   - Nhiệt độ, độ ẩm: DHT11, hiện trên LCD 16x2 thứ hai
 *
 * Sơ đồ chân (không đổi chỗ D3, D4, D8: lúc cấp nguồn D3, D4 phải ở mức cao, D8 ở mức thấp,
 * sai thì mạch không khởi động hoặc không nạp được code):
 *   D1 (GPIO5)  SCL chung: LCD cửa (0x27), LCD nhiệt độ (0x26, hàn cầu A0), bàn phím PCF8574 (0x20)
 *   D2 (GPIO4)  SDA chung
 *   D5 (GPIO14) L298N IN1   motor garage (băm xung để chạy chậm)
 *   D6 (GPIO12) L298N IN2   motor garage
 *   D7 (GPIO13) L298N IN3   quạt. IN4 nối GND. Giữ jumper ENA và ENB trên L298N
 *   D0 (GPIO16) MOSFET dây LED 1 (chân này chỉ bật/tắt)
 *   D8 (GPIO15) MOSFET dây LED 2
 *   D3 (GPIO0)  DHT11
 *   D4 (GPIO2)  servo cửa chính
 *   RX (GPIO3)  còi bíp. Serial chỉ còn chiều in ra (TX)
 * Nguồn: motor, quạt và dây LED lấy nguồn 12V riêng; servo lấy 5V riêng, không lấy từ chân 3V3 của
 * NodeMCU; mọi nguồn nối chung GND. MOSFET phải là loại logic-level (IRLZ44N, AO3400...).
 *
 * Firebase, tính từ /home/floor_1 (một stream cho cả nhánh, ESP8266 không đủ RAM cho nhiều stream):
 *   light_room_1     đèn tầng 1, bật/tắt cả 2 dây LED. Đảo logic như firmware cũ: false = sáng
 *   fan_room_1       quạt: true = chạy
 *   garage/command   lệnh "open" / "close" từ web, app. garage/status mạch ghi "open" / "closed"
 *   door_opened_1    true trong 3 giây cửa chính mở
 *   temperature_1, humidity_1  nhiệt độ, độ ẩm
 *
 * Bàn phím: số, # và * để nhập mật khẩu; A mở cửa chính; B mở cửa garage (cần mật khẩu) hoặc đóng
 * (không cần); C xóa; D đổi mật khẩu. Mật khẩu đổi chỉ giữ tới lần khởi động lại, như cua_chinh.
 */

// ESP8266WiFi, WiFiShare và Firebase phải đứng trước Keypad: Keypad.h có "#define CLOSED HIGH", trùng
// tên với hằng CLOSED trong thư viện mạng lwip và làm lỗi biên dịch nếu include sau.
#include <ESP8266WiFi.h>
#include <WiFiShare.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <Wire.h>
#include <Keypad_I2C.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <DHT.h>

// ==== Cấu hình Wi-Fi + Firebase (project Smart House: smart-house-fcd4f) ====
// Wi-Fi nhà do WiFiShare lo (firmware/libraries/WiFiShare). Mọi mạch dùng cùng SETUP_SSID và
// SETUP_PASSWORD. Không commit mật khẩu thật lên GitHub.
#define DEVICE_NAME "tang_1"
#define SETUP_SSID "SmartHouse-CaiDat"
#define SETUP_PASSWORD "DIEN_MAT_KHAU_CAI_DAT"
#define API_KEY "AIzaSyBom2FOJvMisw2NcE89Zkr-rP_dgOSqbMY"
#define DATABASE_URL "https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app/"
// Tài khoản của các mạch (UID có trong Rules). Không dùng chế độ test: Rules chặn truy cập không đăng nhập.
#define USER_EMAIL "DIEN_EMAIL_TAI_KHOAN_MACH"
#define USER_PASSWORD "DIEN_MAT_KHAU_TAI_KHOAN_MACH"

#define FLOOR_PATH "/home/floor_1"

// ==== Chân ====
#define GARAGE_IN1 D5
#define GARAGE_IN2 D6
#define FAN_PIN D7
#define LED1_PIN D0
#define LED2_PIN D8
#define DHT_PIN D3
#define SERVO_PIN D4
#define BUZZER_PIN 3  // RX

#define KEYPAD_ADDR 0x20
#define LCD_DOOR_ADDR 0x27
#define LCD_CLIMATE_ADDR 0x26

// ==== Thông số ====
#define SERVO_CLOSED 100
#define SERVO_OPEN 20
#define DOOR_OPEN_MS 3000
#define GARAGE_RUN_MS 300       // motor garage chạy 300 ms mỗi lần, như nha_xe_moi_nhat
#define GARAGE_SPEED_OPEN 90    // độ rộng xung 0..255. ENA cắm jumper nên tốc độ hơi khác bản cũ, chỉnh lại nếu cần
#define GARAGE_SPEED_CLOSE 85

LiquidCrystal_I2C lcdDoor(LCD_DOOR_ADDR, 16, 2);
LiquidCrystal_I2C lcdClimate(LCD_CLIMATE_ADDR, 16, 2);
Servo doorServo;
DHT dht(DHT_PIN, DHT11);

const byte ROWS = 4;
const byte COLS = 4;
char hexaKeys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {0, 1, 2, 3};
byte colPins[COLS] = {4, 5, 6, 7};
Keypad_I2C kpad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS, KEYPAD_ADDR, 1, &Wire);

WiFiShare wifiShare;
FirebaseData fbdo;    // ghi trạng thái
FirebaseData stream;  // nhận lệnh từ web, app
FirebaseAuth auth;
FirebaseConfig config;
bool firebaseStarted = false;
bool streamStarted = false;
unsigned long lastStreamTry = 0;

// ==== Trạng thái ====
String password = "111#";
String input = "";
bool changingPass = false;
bool confirmedOld = false;
char lastKey = 0;

bool doorOpen = false;
unsigned long doorCloseAt = 0;
unsigned long lcdDoorRestoreAt = 0;  // 0: LCD cửa đang hiện lời nhắc, không có thông báo chờ xóa

bool fanOn = false;
bool lightOn = false;  // 2 dây LED bật/tắt cùng nhau

bool garageRunning = false;
unsigned long garageStart = 0;
String lastCommand = "";     // lần chạy gần nhất: "open" / "close". Rỗng: chưa biết cửa đang ở đâu
String pendingCommand = "";  // lệnh từ web đến lúc motor đang chạy, xử lý khi motor dừng
bool garageFromKeypad = false;
bool statusDirty = false;        // trạng thái cửa garage chưa ghi được lên Firebase
bool statusHasCommand = false;   // ghi cả lệnh, vì lần chạy đó do bàn phím
unsigned long lastStatusTry = 0;
int statusFails = 0;

unsigned long lastSensorRead = 0;
unsigned long lastUpload = 0;
float lastTemp = -1000, lastHumi = -1000;
float sentTemp = -1000, sentHumi = -1000;

uint8_t beepsLeft = 0;
unsigned int beepMs = 0;
unsigned long nextBeepAt = 0;

// ==== Còi: bíp không chặn loop(), để motor, servo và Firebase vẫn chạy ====
void beep(unsigned int ms = 100, uint8_t times = 1) {
  beepMs = ms;
  beepsLeft = times;
  nextBeepAt = millis();
}

void runBeeper() {
  if (beepsLeft == 0 || (long)(millis() - nextBeepAt) < 0) return;
  tone(BUZZER_PIN, 1000, beepMs);
  beepsLeft--;
  nextBeepAt = millis() + beepMs + 200;
}

// ==== LCD cửa ====
void showPrompt() {
  lcdDoor.clear();
  lcdDoor.print(changingPass ? (confirmedOld ? "MK moi:" : "MK cu:") : "Nhap mat khau:");
  lcdDoor.setCursor(0, 1);
  if (input.length() > 0) {
    lcdDoor.print("Nhap: ");
    for (unsigned int i = 0; i < input.length(); i++) lcdDoor.print('*');
  } else if (!changingPass) {
    lcdDoor.print("A:Cua  B:Garage");
  }
  lcdDoorRestoreAt = 0;
}

// Hiện thông báo một lúc rồi quay lại lời nhắc
void showMessage(const char *line1, const char *line2, unsigned long ms) {
  lcdDoor.clear();
  lcdDoor.print(line1);
  lcdDoor.setCursor(0, 1);
  lcdDoor.print(line2);
  lcdDoorRestoreAt = millis() + ms;
}

// ==== Firebase ====
void initFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  // false: thư viện Firebase không tự nối lại Wi-Fi, việc đó làm hỏng Wi-Fi cài đặt của WiFiShare.
  // Phải gọi trước Firebase.begin().
  Firebase.reconnectNetwork(false);
  Firebase.begin(&config, &auth);
  stream.setBSSLBufferSize(2048, 512);
  firebaseStarted = true;
}

// Chỉ dùng Firebase khi đã vào mạng nhà. Lần đầu vào được mạng thì khởi động Firebase.
// Mất mạng thì bàn phím, cửa, garage và LCD vẫn chạy, chỉ không ghi lên Firebase.
bool firebaseReady() {
  if (!wifiShare.online()) return false;
  if (!firebaseStarted) initFirebase();
  return Firebase.ready();
}

// Ghi nhiều giá trị dưới /home/floor_1 trong một request
void writeFloor(FirebaseJson &json) {
  if (!firebaseReady()) return;
  if (!Firebase.RTDB.updateNode(&fbdo, FLOOR_PATH, &json)) {
    Serial.print(F("Loi ghi Firebase: "));
    Serial.println(fbdo.errorReason());
  }
}

// ==== Quạt, đèn ====
void setFan(bool on) {
  if (on == fanOn) return;
  fanOn = on;
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  Serial.println(on ? F("Quat: BAT") : F("Quat: TAT"));
}

void setLights(bool on) {
  if (on == lightOn) return;
  lightOn = on;
  digitalWrite(LED1_PIN, on ? HIGH : LOW);
  digitalWrite(LED2_PIN, on ? HIGH : LOW);
  Serial.println(on ? F("Den tang 1: BAT") : F("Den tang 1: TAT"));
}

// ==== Nhận lệnh từ stream ====
String unquote(const String &s) {
  if (s.length() >= 2 && s[0] == '"' && s[s.length() - 1] == '"') return s.substring(1, s.length() - 1);
  return s;
}

// Một giá trị dưới /home/floor_1. path không có "/" đầu, vd. "garage/command"; value đã bỏ dấu ngoặc kép.
void applyValue(const String &path, const String &value) {
  bool isBool = value == "true" || value == "false";
  if (path == "light_room_1" && isBool) {
    setLights(value == "false");  // đảo logic: false = đèn sáng
  } else if (path == "fan_room_1" && isBool) {
    setFan(value == "true");
  } else if (path == "garage/command" && (value == "open" || value == "close")) {
    pendingCommand = value;
  } else if (path == "garage/status" && lastCommand.length() == 0 && !garageRunning) {
    // Lần đầu nhận dữ liệu: coi trạng thái đã lưu là vị trí hiện tại của cửa, để lệnh cũ trên web
    // không chạy lại motor khi mạch vừa khởi động
    if (value == "open") lastCommand = "open";
    else if (value == "closed") lastCommand = "close";
  }
}

// Các giá trị mạch cần nghe, tính từ /home/floor_1
const char *const WATCHED[] = {"light_room_1", "fan_room_1", "garage/command", "garage/status"};

// Sự kiện dạng JSON: ảnh cả nhánh lúc mở stream (base rỗng), hoặc nhiều giá trị đổi cùng lúc tại base.
// Lấy từng giá trị cần nghe bằng đường dẫn con. Không dựng lại đường dẫn bằng iterator của FirebaseJson:
// nó không giảm độ sâu khi ra khỏi một nhánh con, nên các phần tử sau đó mang độ sâu sai.
void applyJson(const String &base, FirebaseJson &json) {
  FirebaseJsonData r;
  for (const char *w : WATCHED) {
    String target(w), sub;
    if (base.length() == 0) sub = target;
    else if (target.startsWith(base + "/")) sub = target.substring(base.length() + 1);
    else continue;
    if (json.get(r, sub) && (r.typeNum == FirebaseJson::JSON_BOOL || r.typeNum == FirebaseJson::JSON_STRING)) {
      applyValue(target, r.stringValue);  // bool: "true"/"false"; chuỗi: đã bỏ dấu ngoặc kép
    }
  }

  // Lệnh ghi nhiều đường dẫn một lúc có thể đến dưới dạng khóa chứa "/", vd. {"garage/command": "open"}.
  // json.get() hiểu "/" là đi vào nhánh con nên không thấy; khóa kiểu này chỉ nằm ở tầng đầu.
  size_t count = json.iteratorBegin();
  for (size_t i = 0; i < count; i++) {
    FirebaseJson::IteratorValue it = json.valueAt(i);
    if (it.key.indexOf('/') < 0) continue;
    applyValue(base.length() > 0 ? base + "/" + it.key : it.key, unquote(it.value));
  }
  json.iteratorEnd();
}

// Stream giữ một kết nối mở và chỉ nhận dữ liệu khi giá trị đổi, không đọc Firebase ở mỗi vòng loop().
void readStream() {
  if (!firebaseReady()) {
    if (!wifiShare.online()) streamStarted = false;  // vào lại mạng thì mở stream lại
    return;
  }

  if (!streamStarted) {
    // Mở stream thì Firebase gửi lệnh garage đang lưu. Ghi trạng thái cửa trước, để lệnh cũ không
    // chạy lại cửa vừa mở bằng bàn phím lúc mất mạng. Ghi lỗi 3 lần thì vẫn mở stream.
    if (statusDirty && statusFails < 3) return;
    if (lastStreamTry != 0 && millis() - lastStreamTry < 5000) return;
    lastStreamTry = millis();
    streamStarted = Firebase.RTDB.beginStream(&stream, FLOOR_PATH);
    if (!streamStarted) {
      Serial.print(F("Loi mo stream: "));
      Serial.println(stream.errorReason());
    }
    return;
  }

  if (!Firebase.RTDB.readStream(&stream)) {
    Serial.print(F("Loi doc stream: "));
    Serial.println(stream.errorReason());
    return;
  }
  if (!stream.streamAvailable()) return;

  String base = stream.dataPath();
  if (base.startsWith("/")) base.remove(0, 1);
  String type = stream.dataType();
  if (type == "json") applyJson(base, stream.jsonObject());
  else if (type == "boolean") applyValue(base, stream.boolData() ? "true" : "false");
  else if (type == "string") applyValue(base, stream.stringData());
}

// ==== Cửa chính ====
void openDoor() {
  doorServo.write(SERVO_OPEN);
  doorOpen = true;
  doorCloseAt = millis() + DOOR_OPEN_MS;
  showMessage("Mo cua...", "", DOOR_OPEN_MS);
  beep(300);

  // Vào nhà thì bật đèn tầng 1, như cua_chinh bật đèn khi mở cửa
  setLights(true);
  FirebaseJson json;
  json.set("door_opened_1", true);
  json.set("light_room_1", false);  // false = đèn sáng
  writeFloor(json);
}

void closeDoorIfDue() {
  if (!doorOpen || (long)(millis() - doorCloseAt) < 0) return;
  doorServo.write(SERVO_CLOSED);
  doorOpen = false;
  FirebaseJson json;
  json.set("door_opened_1", false);
  writeFloor(json);
}

// ==== Cửa garage ====
void stopGarage() {
  digitalWrite(GARAGE_IN1, LOW);
  digitalWrite(GARAGE_IN2, LOW);
}

void startGarage(const String &cmd, bool fromKeypad) {
  if (cmd == "open") {
    digitalWrite(GARAGE_IN2, LOW);
    analogWrite(GARAGE_IN1, GARAGE_SPEED_OPEN);
  } else {
    digitalWrite(GARAGE_IN1, LOW);
    analogWrite(GARAGE_IN2, GARAGE_SPEED_CLOSE);
  }
  garageStart = millis();
  garageRunning = true;
  lastCommand = cmd;
  garageFromKeypad = fromKeypad;
}

// Cập nhật trạng thái cửa garage trong một request. Lần chạy do bàn phím thì ghi cả lệnh, để web hiện
// đúng và lệnh kế tiếp từ web vẫn là một thay đổi. Ghi không được thì thử lại 5 giây một lần.
void syncGarageStatus() {
  if (!statusDirty || !firebaseReady()) return;
  if (lastStatusTry != 0 && millis() - lastStatusTry < 5000) return;
  lastStatusTry = millis();

  FirebaseJson json;
  json.set("status", lastCommand == "open" ? "open" : "closed");
  if (statusHasCommand) json.set("command", lastCommand);
  if (Firebase.RTDB.updateNode(&fbdo, FLOOR_PATH "/garage", &json)) {
    statusDirty = false;
    statusHasCommand = false;
    statusFails = 0;
  } else {
    statusFails++;
    Serial.print(F("Loi ghi trang thai garage: "));
    Serial.println(fbdo.errorReason());
  }
}

void runGarage() {
  if (!garageRunning && pendingCommand.length() > 0) {
    String cmd = pendingCommand;
    pendingCommand = "";
    if (cmd != lastCommand) startGarage(cmd, false);
  }

  if (garageRunning && millis() - garageStart >= GARAGE_RUN_MS) {
    stopGarage();
    garageRunning = false;
    // Ghi trạng thái sau khi motor đã dừng
    statusDirty = true;
    if (garageFromKeypad) statusHasCommand = true;
    lastStatusTry = 0;
    statusFails = 0;
  }
  syncGarageStatus();
}

// ==== Bàn phím ====
void handleKey(char key) {
  beep();
  Serial.println(key);

  if (key == 'A') {
    if (!changingPass) {
      if (input == password) openDoor();
      else {
        showMessage("Sai! Thu lai", "", 1500);
        beep(200, 3);
      }
      input = "";
    } else if (!confirmedOld) {
      bool ok = input == password;
      input = "";
      if (ok) {
        confirmedOld = true;
        showPrompt();
      } else {
        changingPass = false;
        showMessage("Sai MK cu!", "", 2000);
      }
    } else {
      password = input;
      changingPass = false;
      confirmedOld = false;
      input = "";
      showMessage("Da doi MK", "", 2000);
    }
  } else if (key == 'B') {
    // Đóng garage không cần mật khẩu, mở thì cần
    if (garageRunning) return;
    if (lastCommand == "open") {
      startGarage("close", true);
      showMessage("Dong garage", "", 1500);
    } else if (input == password) {
      startGarage("open", true);
      showMessage("Mo garage", "", 1500);
    } else {
      showMessage("Sai! Thu lai", "", 1500);
      beep(200, 3);
    }
    input = "";
  } else if (key == 'C') {
    input = "";
    showPrompt();
  } else if (key == 'D') {
    changingPass = true;
    confirmedOld = false;
    input = "";
    showPrompt();
  } else {
    input += key;
    if (input.length() > 12) input.remove(0, 1);  // vừa một dòng LCD
    showPrompt();
  }
}

void readKeypad() {
  char key = kpad.getKey();
  if (key && key != lastKey) {
    lastKey = key;
    handleKey(key);
  } else if (!key) {
    lastKey = 0;
  }
}

// ==== DHT11 + LCD nhiệt độ ====
// Đọc mỗi 2 giây. Gửi lên Firebase khi đổi đủ nhiều, cách nhau ít nhất 10 giây, như bao_trom_va_do_am.
void readClimate() {
  unsigned long now = millis();
  if (now - lastSensorRead < 2000) return;
  lastSensorRead = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    lcdClimate.setCursor(0, 0);
    lcdClimate.print("Loi cam bien DHT");
    lcdClimate.setCursor(0, 1);
    lcdClimate.print("                ");
    lastTemp = lastHumi = -1000;
    return;
  }

  if (abs(t - lastTemp) >= 0.1 || abs(h - lastHumi) >= 0.1) {
    lcdClimate.setCursor(0, 0);
    lcdClimate.print("Nhiet do: ");
    lcdClimate.print(t, 1);
    lcdClimate.write(223);
    lcdClimate.print("C  ");
    lcdClimate.setCursor(0, 1);
    lcdClimate.print("Do am  : ");
    lcdClimate.print(h, 1);
    lcdClimate.print("%   ");
    lastTemp = t;
    lastHumi = h;
  }

  bool changed = abs(t - sentTemp) >= 0.5 || abs(h - sentHumi) >= 1;
  if (changed && (lastUpload == 0 || now - lastUpload >= 10000) && firebaseReady()) {
    lastUpload = now;
    FirebaseJson json;
    json.set("temperature_1", t);
    json.set("humidity_1", h);
    if (Firebase.RTDB.updateNode(&fbdo, FLOOR_PATH, &json)) {
      sentTemp = t;
      sentHumi = h;
    }
  }
}

// ==== Setup, loop ====
void setup() {
  // RX dùng cho còi nên Serial chỉ in ra
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(GARAGE_IN1, OUTPUT);
  pinMode(GARAGE_IN2, OUTPUT);
  stopGarage();
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);

  Wire.begin(D2, D1);
  kpad.begin();
  lcdDoor.init();
  lcdDoor.backlight();
  lcdClimate.init();
  lcdClimate.backlight();
  lcdClimate.print("Doc cam bien...");
  showPrompt();

  doorServo.attach(SERVO_PIN);
  doorServo.write(SERVO_CLOSED);
  dht.begin();

  wifiShare.begin(DEVICE_NAME, SETUP_SSID, SETUP_PASSWORD);
}

void loop() {
  wifiShare.loop();
  readStream();
  readKeypad();
  runBeeper();
  closeDoorIfDue();
  runGarage();
  readClimate();

  if (lcdDoorRestoreAt != 0 && !doorOpen && (long)(millis() - lcdDoorRestoreAt) >= 0) showPrompt();
}
