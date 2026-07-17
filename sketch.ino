/*
  Jaime Arduino UNO Q Robot
  Roni Bandini July 2026
  MIT License

  Servos:      DFRobot 2.5g 360°    D13 = left, D12 = right
  Line sensor: analog line detector A1   
  Ultrasonic:  analog output        A0

  Bridge functions (called from Python via bridge.py):
    move        (String cmd, float seconds)   forward/back/right/left/stop
    forwardUntil    (int targetCm)               drive forward until distance <= targetCm
    forwardUntilLine(int lineNumber)              drive forward until the Nth line is crossed (returns string "true"/"false")
    backUntilLine   (int lineNumber)              drive backward until the Nth line is crossed
    readSensors     ()                            returns "distance,line"
*/

#include <Servo.h>
#include <Arduino_RouterBridge.h>

// ─── Pins ─────────────────────────────────────────────────────────────────────
const int LEFT_PIN  = 13;
const int RIGHT_PIN = 12;
const int SONAR_PIN = A0;
const int LINE_PIN  = A1;

// ─── Ultrasonic ───────────────────────────────────────────────────────────────
#define SONAR_MAX_CM 520

// ─── Line detector tuning ─────────────────────────────────────────────────────
const int LINE_MIN = 950;
const int LINE_MAX2 = 1023;

// ─── Servo constants ──────────────────────────────────────────────────────────
const int LEFT_FWD   = 0;
const int LEFT_BACK  = 180;
const int RIGHT_FWD  = 180;
const int RIGHT_BACK = 0;
const int STOP_VAL   = 90;

// Smooth now :)
const int LEFT_ROT_SLOW  = 105;  
const int LEFT_ROT_FWD   = 75;   

// ─── Pending command flags (set by Bridge, consumed in loop) ──────────────────
volatile bool  pendingMove       = false;
volatile float pendingSeconds    = 1.0f;
char           pendingCmd[16]    = "";
volatile bool  pendingFwdUntil   = false;
volatile int   pendingFwdCm      = 30;
volatile bool  pendingBackLine    = false;
volatile int   pendingBackLineNum = 1;

// ─── Objects ──────────────────────────────────────────────────────────────────
Servo leftServo;
Servo rightServo;

// ════════════════════════════════════════════════════════════════════════════
//  SENSOR FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

int readUltrasonic() {
  int raw = analogRead(SONAR_PIN);
  if (raw >= 1020) return -1;
  return (int)((float)raw * 5.0f / 3.3f * SONAR_MAX_CM / 1023.0f);
}

int readLineSensor() {
  return analogRead(LINE_PIN);
}

bool isOnLine(int value) {
  return value >= LINE_MIN && value <= LINE_MAX2;
}

// ════════════════════════════════════════════════════════════════════════════
//  MOTOR PRIMITIVES
// ════════════════════════════════════════════════════════════════════════════

void stopMotors() {
  leftServo.write(STOP_VAL);
  rightServo.write(STOP_VAL);
}

void driveForward() {
  leftServo.write(LEFT_BACK);
  rightServo.write(RIGHT_BACK);
}

void driveBack() {
  leftServo.write(LEFT_FWD);
  rightServo.write(RIGHT_FORWARD); 
}

// ════════════════════════════════════════════════════════════════════════════
//  TIMED MOVEMENT FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

void doForward(float seconds) {
  driveForward();
  delay((unsigned long)(seconds * 1000.0f));
  stopMotors();
}

void doBack(float seconds) {
  driveBack();
  delay((unsigned long)(seconds * 1000.0f));
  stopMotors();
}

void doRight(float seconds) {
  leftServo.write(LEFT_ROT_SLOW); 
  rightServo.write(STOP_VAL);
  delay((unsigned long)(seconds * 1000.0f));
  stopMotors();
}

void doLeft(float seconds) {
  leftServo.write(STOP_VAL);
  rightServo.write(LEFT_ROT_SLOW); 
  delay((unsigned long)(seconds * 1000.0f));
  stopMotors();
}

// ════════════════════════════════════════════════════════════════════════════
//  SENSOR-BASED MOVEMENT FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

void doForwardUntil(int targetCm) {
  Serial.print("[forwardUntil] target: ");
  Serial.print(targetCm);
  Serial.println(" cm");

  unsigned long start = millis();
  driveForward();

  while (millis() - start < 15000UL) {
    int dist = readUltrasonic();
    Serial.print("  dist: ");
    Serial.println(dist);
    if (dist > 0 && dist <= targetCm) break;
    delay(100);
  }
  stopMotors();
  Serial.println("[forwardUntil] done");
}

bool doForwardUntilLine(int targetLineNumber) {
  Serial.print("[forwardUntilLine] target line: ");
  Serial.println(targetLineNumber);

  unsigned long start = millis();
  int linesCrossed = 0;
  bool onLine = isOnLine(readLineSensor());
  Serial.print("  initial state onLine=");
  Serial.println(onLine);

  driveForward();

  while (millis() - start < 20000UL) {
    int dist = readUltrasonic();
    if (dist > 0 && dist <= 30) {
      Serial.println("[forwardUntilLine] Obstacle detected at <= 30cm! Stopping.");
      stopMotors();
      return false;
    }

    int value = readLineSensor();
    bool nowOnLine = isOnLine(value);

    if (nowOnLine != onLine) {
      Serial.print("  line value: ");
      Serial.print(value);
      Serial.print("  state -> ");
      Serial.println(nowOnLine ? "ON" : "OFF");
      if (nowOnLine) {
        linesCrossed++;
        Serial.print("  >>> line crossed, count = ");
        Serial.println(linesCrossed);
        if (linesCrossed >= targetLineNumber) {
          onLine = nowOnLine;
          break;
        }
      }
      onLine = nowOnLine;
    }

    delay(15);
  }
  stopMotors();
  Serial.println("[forwardUntilLine] done");
  return true;
}

void doBackUntilLine(int targetLineNumber) {
  Serial.print("[backUntilLine] target line: ");
  Serial.println(targetLineNumber);
  unsigned long start = millis();
  int linesCrossed = 0;

  bool onLine = isOnLine(readLineSensor());
  Serial.print("  initial state onLine=");
  Serial.println(onLine);

  driveBack();
  while (millis() - start < 20000UL) {
    int value = readLineSensor();
    bool nowOnLine = isOnLine(value);

    if (nowOnLine != onLine) {
      Serial.print("  line value: ");
      Serial.print(value);
      Serial.print("  state -> ");
      Serial.println(nowOnLine ? "ON" : "OFF");
      if (nowOnLine) {
        linesCrossed++;
        Serial.print("  >>> line crossed, count = ");
        Serial.println(linesCrossed);
        if (linesCrossed >= targetLineNumber) {
          onLine = nowOnLine;
          break;
        }
      }
      onLine = nowOnLine;
    }

    delay(15);
  }
  stopMotors();
  Serial.println("[backUntilLine] done");
}

// ════════════════════════════════════════════════════════════════════════════
//  BRIDGE HANDLERS
// ════════════════════════════════════════════════════════════════════════════

void handleMove(String cmd, float seconds) {
  strncpy(pendingCmd, cmd.c_str(), sizeof(pendingCmd) - 1);
  pendingSeconds = seconds;
  pendingMove    = true;
}

void handleForwardUntil(int targetCm) {
  pendingFwdCm    = targetCm;
  pendingFwdUntil = true;
}

String handleForwardUntilLine(int lineNumber) {
  bool success = doForwardUntilLine(lineNumber);
  return success ? "true" : "false";
}

void handleBackUntilLine(int lineNumber) {
  pendingBackLineNum = lineNumber;
  pendingBackLine    = true;
}

String handleReadSensors() {
  int dist = readUltrasonic();
  int line = readLineSensor();
  return String(dist) + "," + String(line);
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(10);

  leftServo.attach(LEFT_PIN);
  rightServo.attach(RIGHT_PIN);
  stopMotors();

  pinMode(LINE_PIN, INPUT);
  if (!Bridge.begin()) {
    Serial.println("Bridge init failed");
  }

  Bridge.provide_safe("move",             handleMove);
  Bridge.provide_safe("forwardUntil",     handleForwardUntil);
  Bridge.provide_safe("forwardUntilLine", handleForwardUntilLine);
  Bridge.provide_safe("backUntilLine",    handleBackUntilLine);
  Bridge.provide_safe("readSensors",      handleReadSensors);
  Serial.println("Ready.");
}

// ════════════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {

  if (pendingFwdUntil) {
    pendingFwdUntil = false;
    doForwardUntil(pendingFwdCm);
  }

  if (pendingBackLine) {
    pendingBackLine = false;
    doBackUntilLine(pendingBackLineNum);
  }

  if (pendingMove) {
    pendingMove = false;
    String cmd  = String(pendingCmd);
    if      (cmd == "forward") doForward(pendingSeconds);
    else if (cmd == "back")    doBack(pendingSeconds);
    else if (cmd == "right")   doRight(pendingSeconds);
    else if (cmd == "left")    doLeft(pendingSeconds);
    else if (cmd == "stop")    stopMotors();
  }

  int dist = readUltrasonic();
  int line = readLineSensor();

  Serial.print("Distance: ");
  if (dist < 0) Serial.print("no signal");
  else { Serial.print(dist); Serial.print(" cm"); }
  Serial.print("  |  Line: ");
  Serial.print(line);
  Serial.print(isOnLine(line) ? " (ON LINE)" : "");
  Serial.println();

  delay(500);
}