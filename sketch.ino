/*
  Arduino UNO Q AI Agentic Robot
  Roni Bandini, July 2026, @ronibandini
  MIT License
  Note: these primitives functions are called through the bridge by the MPU (OpenClaw using robot.py)

  Servos:      DFRobot 2.5g 360°    D13 = left, D12 = right
  Line sensor: analog line detector A1   
  Ultrasonic:  analog output        A0
  Magnetometer: SDA, SCL

  Libraries (Sketch → Include Library → Manage Libraries):
    - Servo 1.3.0
    - Gyvermag 1.0.0

*/

#include <Servo.h>
#include <Arduino_RouterBridge.h>
#include <HMC5883L.h>
#include <HMC5983L.h>
#include <QMC5883L.h>

// GPIOs
const int LEFT_PIN  = 13;
const int RIGHT_PIN = 12;
const int SONAR_PIN = A0;
const int LINE_PIN  = A1;


#define SONAR_MAX_CM 520

// Line detector
const int LINE_MIN = 950;
const int LINE_MAX2 = 1023;

// Servos
const int LEFT_FWD   = 0;
const int LEFT_BACK  = 180;
const int RIGHT_FWD  = 180;
const int RIGHT_BACK = 0;
const int STOP_VAL   = 90;

// Flags
volatile bool  pendingMove       = false;
volatile float pendingSeconds    = 1.0f; 
char           pendingCmd[16]    = "";
volatile bool  pendingFwdUntil   = false;
volatile int   pendingFwdCm      = 30;
volatile bool  pendingBackLine    = false;
volatile int   pendingBackLineNum = 1;

Servo leftServo;
Servo rightServo;
QMC5883L mag;

// Compass related
float frontHeading       = 0.0f;
float leftTargetHeading  = 0.0f;
float rightTargetHeading = 0.0f;
bool  compassCalibrated  = false;
volatile bool calibrationConverged = false;
const unsigned long CALIBRATE_HARD_TIMEOUT_MS = 25000UL;
const float         HEADING_TOLERANCE_DEG = 3.0f;
const unsigned long TURN_PULSE_MS         = 50;   // turn pulse duration
const unsigned long TURN_SETTLE_MS        = 150;   // motors off before reading compass
const unsigned long TURN_TIMEOUT_MS       = 15000UL;


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



bool onCalibrate(const MagCalProgress& p) {
  Serial.print("  t=");
  Serial.print(p.elapsed);
  Serial.print(" ms  balance=");
  Serial.print(p.balance * 100.0f, 0);
  Serial.print("%  radius=");
  Serial.println(p.gaussR);

  bool goodFit     = p.elapsed > 15000 && p.balance > 0.75f && p.gaussR > 0.15f;
  bool hardTimeout = p.elapsed > CALIBRATE_HARD_TIMEOUT_MS;

  if (goodFit || hardTimeout) {
    calibrationConverged = goodFit;
    return true;
  }
  return false;
}


void calibrateCompass() {
  Serial.println();
  Serial.println("=== Compass calibration ===");
  Serial.println("Turn the robot in place for about 3 full slow spins now.");
  Serial.println("(runs for up to 25 seconds)");

  calibrationConverged = false;
  unsigned long calStart = millis();
  mag.calibrate(onCalibrate);
  unsigned long calElapsed = millis() - calStart;

  MagGauss g = mag.readGauss();
  float heading = mag.headingDeg();

  Serial.println();
  Serial.print("Calibration ");
  Serial.println(calibrationConverged
    ? "converged."
    : "hit the safety timeout (may still be a bit biased).");
  Serial.print("  elapsed: ");     Serial.print(calElapsed); Serial.println(" ms");
  Serial.print("  heading ahorita: "); Serial.print(heading);
  Serial.print("  (X=");           Serial.print(g[0]);
  Serial.print(" Y=");             Serial.print(g[1]);
  Serial.print(" Z=");             Serial.print(g[2]);
  Serial.println(")");
  Serial.println("============================");
  Serial.println();
}

// Reads a fresh sample and returns hd in degrees.
float readHeading() {
  mag.readGauss();
  return mag.headingDeg();
}

// Smallest angular distance from current to target, in [-180, 180].
float headingDiff(float targetHeading, float currentHeading) {
  float diff = targetHeading - currentHeading;
  if (diff < -180.0f) diff += 360.0f;
  if (diff > 180.0f)  diff -= 360.0f;
  return diff;
}


bool doSetCalibration(float front, float left, float right) {
  frontHeading       = front;
  leftTargetHeading  = left;
  rightTargetHeading = right;
  compassCalibrated  = true;

  Serial.print("[setCalibration] front="); Serial.print(frontHeading);
  Serial.print("  left=");  Serial.print(leftTargetHeading);
  Serial.print("  right="); Serial.println(rightTargetHeading);
  return true;
}


String turnToHeading(float targetHeading) {
  unsigned long start = millis();
  float startHeading = readHeading();
  float current = startHeading;
  int pulses = 0;

  float initialDiff = headingDiff(targetHeading, current);
  int   initialSign  = (initialDiff >= 0) ? 1 : -1;
  bool  turnRight    = (initialDiff >= 0);   // shorter arc: positive diff = go right

  while (millis() - start < TURN_TIMEOUT_MS) {
    current = readHeading();
    float diff       = headingDiff(targetHeading, current);
    int   currentSign = (diff >= 0) ? 1 : -1;

    Serial.print("  pulse=");   Serial.print(pulses);
    Serial.print("  current="); Serial.print(current);
    Serial.print("  target=");  Serial.print(targetHeading);
    Serial.print("  diff=");    Serial.println(diff);

    if (abs(diff) <= HEADING_TOLERANCE_DEG || currentSign != initialSign) {
      stopMotors();
      Serial.println("  reached/crossed target heading");
      return "1," + String(pulses) + "," + String(startHeading, 1) + "," +
             String(current, 1) + "," + String(targetHeading, 1);
    }

    if (turnRight) {
      leftServo.write(LEFT_BACK);
      rightServo.write(RIGHT_FWD);
    } else {
      leftServo.write(LEFT_FWD);
      rightServo.write(RIGHT_BACK);
    }
    delay(TURN_PULSE_MS);
    stopMotors();
    delay(TURN_SETTLE_MS);
    pulses++;
  }
  stopMotors();
  Serial.println("  !!! timeout reaching target heading");
  return "0," + String(pulses) + "," + String(startHeading, 1) + "," +
         String(current, 1) + "," + String(targetHeading, 1);
}

String doTurnLeft90() {
  if (!compassCalibrated) {
    Serial.println("[turnLeft90] ERROR: run setCalibration first");
    return "notCalibrated";
  }
  Serial.println("[turnLeft90] adjusting to the 'left' heading (shortest path)");
  return turnToHeading(leftTargetHeading);
}

String doTurnRight90() {
  if (!compassCalibrated) {
    Serial.println("[turnRight90] ERROR: run setCalibration first");
    return "notCalibrated";
  }
  Serial.println("[turnRight90] adjusting to the 'right' heading (shortest path)");
  return turnToHeading(rightTargetHeading);
}

// Adjusts back to the stored front heading, via the shorter arc.
String doHeadingFront() {
  if (!compassCalibrated) {
    Serial.println("[headingFront] ERROR: run setCalibration first");
    return "notCalibrated";
  }
  Serial.println("[headingFront] adjusting to the 'front' heading (shortest path)");
  return turnToHeading(frontHeading);
}


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
  float heading = readHeading();
  return String(dist) + "," + String(line) + "," + String(heading, 1);
}

bool handleSetCalibration(float front, float left, float right) {
  return doSetCalibration(front, left, right);
}

String handleTurnLeft90() {
  return doTurnLeft90();
}

String handleTurnRight90() {
  return doTurnRight90();
}

String handleHeadingFront() {
  return doHeadingFront();
}

// Setup ******************************************************************

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(10);

  leftServo.attach(LEFT_PIN);
  rightServo.attach(RIGHT_PIN);
  stopMotors();

  pinMode(LINE_PIN, INPUT);

  Wire.begin();
  mag.begin();   

  if (!Bridge.begin()) {
    Serial.println("Bridge start fail");
  }

  Bridge.provide_safe("move",             handleMove);
  Bridge.provide_safe("forwardUntil",     handleForwardUntil);
  Bridge.provide_safe("forwardUntilLine", handleForwardUntilLine);
  Bridge.provide_safe("backUntilLine",    handleBackUntilLine);
  Bridge.provide_safe("readSensors",      handleReadSensors);
  Bridge.provide_safe("setCalibration",   handleSetCalibration);
  Bridge.provide_safe("turnLeft90",       handleTurnLeft90);
  Bridge.provide_safe("turnRight90",      handleTurnRight90);
  Bridge.provide_safe("headingFront",     handleHeadingFront);
  Serial.println("Bridge ready.");

  calibrateCompass();

  Serial.println("All good.");
}



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

  MagGauss g = mag.readGauss();
  float heading = mag.headingDeg();

  Serial.print("Distance: ");
  if (dist < 0) Serial.print("no signal");
  else { Serial.print(dist); Serial.print(" cm");
  }
  Serial.print("  |  Line: ");
  Serial.print(line);
  Serial.print(isOnLine(line) ? " (ON LINE)" : "");
  Serial.print("  |  Heading: ");
  Serial.print(heading);
  Serial.print("  (X=");
  Serial.print(g[0]);
  Serial.print(" Y=");
  Serial.print(g[1]);
  Serial.print(" Z=");
  Serial.print(g[2]);
  Serial.print(")");
  Serial.println();

  delay(500);
}
