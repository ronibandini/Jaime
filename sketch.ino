/*
  Arduino UNO Q Robot
  Roni Bandini July 2026
  MIT License

  Servos:      DFRobot 2.5g 360°    D13 = left, D12 = right
  Line sensor: analog line detector A1  (reads ~55-65 on yellow/white tape)
  Ultrasonic:  analog output        A0

  Libraries (Sketch → Include Library → Manage Libraries):
    - Arduino_RouterBridge   (included with UNO Q board support)

  Bridge functions (called from Python via bridge.py):
    move        (String cmd, float seconds)   forward/back/right/left/stop
    forwardUntil    (int targetCm)               drive forward until distance <= targetCm
    forwardUntilLine(int lineNumber)              drive forward until the Nth line is crossed;
                                                   aborts early if a wall is detected under 30cm.
                                                   Returns the number of lines actually crossed
                                                   (equals lineNumber on a normal finish, less if
                                                   it stopped early for a wall).
    backUntilLine   (int lineNumber)              drive backward until the Nth line is crossed
    readSensors     ()                            returns "distance,line"

  Serial debug:
    Runs at 115200 baud (raised from 9600) to keep print overhead from slowing
    the line-detection polling loop.
    Prints distance + raw line sensor value every 500ms in the main loop.
    During forwardUntilLine/backUntilLine, prints only on line-state changes
    (not every iteration) so the polling loop stays fast enough to catch
    quick line crossings.
*/

#include <Servo.h>
#include <Arduino_RouterBridge.h>

// ─── Pins ─────────────────────────────────────────────────────────────────────
const int LEFT_PIN  = 13;
const int RIGHT_PIN = 12;
const int SONAR_PIN = A0;
const int LINE_PIN  = A1;

// ─── Ultrasonic ───────────────────────────────────────────────────────────────
// UNO Q ADC ref = 3.3V, sensor VCC = 5V, 10-bit (0-1023)
// Saturates at ~341 cm (sensor Vout exceeds 3.3V beyond that)
#define SONAR_MAX_CM 520

// ─── Line detector tuning ─────────────────────────────────────────────────────
// Black is 1023. Yellow and white tape reads 55-65 on this sensor.
// Widened slightly for margin.
const int LINE_MIN = 950;
const int LINE_MAX2 = 1023;

// ─── Servo constants ──────────────────────────────────────────────────────────
const int LEFT_FWD   = 0;
const int LEFT_BACK  = 180;
const int RIGHT_FWD  = 180;
const int RIGHT_BACK = 0;
const int STOP_VAL   = 90;

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
  rightServo.write(RIGHT_FWD);
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
  leftServo.write(LEFT_BACK);
  rightServo.write(RIGHT_FWD);
  delay((unsigned long)(seconds * 1000.0f));
  stopMotors();
}

void doLeft(float seconds) {
  leftServo.write(LEFT_FWD);
  rightServo.write(RIGHT_BACK);
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

// Drives forward toward the Nth line, but aborts early if the ultrasonic
// sensor reports a wall closer than 30cm. Returns the number of lines
// actually crossed: equal to targetLineNumber on a normal finish, or a
// smaller number if the wall stop cut it short.
int doForwardUntilLine(int targetLineNumber) {
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
    if (dist > 0 && dist < 30) {
      Serial.print("  !!! wall at ");
      Serial.print(dist);
      Serial.println(" cm, stopping early");
      break;
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
  Serial.print("[forwardUntilLine] done, linesCrossed=");
  Serial.println(linesCrossed);
  return linesCrossed;
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
//  BRIDGE HANDLERS  (provide_safe → executes in loop() context)
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

int handleForwardUntilLine(int lineNumber) {
  return doForwardUntilLine(lineNumber);
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

  // ── Execute pending commands ──────────────────────────────────────────────
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

  // ── Periodic sensor Serial print ─────────────────────────────────────────
  int dist = readUltrasonic();
  int line = readLineSensor();

  Serial.print("Distance: ");
  if (dist < 0) Serial.print("no signal");
  else { Serial.print(dist); Serial.print(" cm");
  }
  Serial.print("  |  Line: ");
  Serial.print(line);
  Serial.print(isOnLine(line) ? " (ON LINE)" : "");
  Serial.println();

  delay(500);
}