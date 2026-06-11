/*
  Arduino Mega + RAMPS 1.4 gantry + syringe pump controller

  GANTRY module motion model:
    - Both motors same direction: X axis
    - Motors opposite directions: Y axis

  Serial commands, 115200 baud:
    GANTRY SETZERO
    GANTRY SETPOS X30 Y20
    GANTRY GOTO X20 Y30
    GANTRY CIRCLE X30 Y30 R8 N120 L1
    GANTRY SPEED S20
    GANTRY ACCEL S100

    PUMP ZS0.1        Z small syringe, dispense 0.1 ml
    PUMP E1S-0.1      E1 small syringe, aspirate 0.1 ml
    PUMP PAIR ZS0.1 E1S-0.1
    PUMP ZM5          Z calibration move 5 mm
    PUMP E1M-5        E1 calibration move -5 mm
    PUMP SPEED S5     set pump speed to 5 mm/s

    GANTRY DEPOSIT X30 Y30 R8 V0.5 N120 L1

    STOP              emergency stop, disables all motors
    UNLOCK            re-enable after STOP
    HELP              print commands

  Old unprefixed gantry commands still work for compatibility.
*/

// RAMPS 1.4 pinout for X and Y stepper sockets.
const byte MOTOR_A_STEP_PIN = 54;  // RAMPS X_STEP
const byte MOTOR_A_DIR_PIN = 55;   // RAMPS X_DIR
const byte MOTOR_A_EN_PIN = 38;    // RAMPS X_ENABLE, active LOW

const byte MOTOR_B_STEP_PIN = 60;  // RAMPS Y_STEP
const byte MOTOR_B_DIR_PIN = 61;   // RAMPS Y_DIR
const byte MOTOR_B_EN_PIN = 56;    // RAMPS Y_ENABLE, active LOW

const byte Z_PUMP_STEP_PIN = 46;   // RAMPS Z_STEP
const byte Z_PUMP_DIR_PIN = 48;    // RAMPS Z_DIR
const byte Z_PUMP_EN_PIN = 62;     // RAMPS Z_ENABLE, active LOW

const byte E1_PUMP_STEP_PIN = 36;  // RAMPS E1_STEP
const byte E1_PUMP_DIR_PIN = 34;   // RAMPS E1_DIR
const byte E1_PUMP_EN_PIN = 30;    // RAMPS E1_ENABLE, active LOW

// Adjust these first. Start low to avoid over-travel.
const float STEPS_PER_MM_X = 14.5;
const float STEPS_PER_MM_Y = 14.5;
const float MAX_X_MM = 165.0;
const float MAX_Y_MM = 165.0;

// Flip these if an axis moves the wrong way.
const bool INVERT_MOTOR_A_DIR = true;
const bool INVERT_MOTOR_B_DIR = true;
const bool INVERT_X_AXIS = false;
const bool INVERT_Y_AXIS = true;
const bool INVERT_Z_PUMP_DIR = false;
const bool INVERT_E1_PUMP_DIR = false;

// Conservative motion settings inspired by Marlin-style trapezoid planning.
const unsigned int STEP_HIGH_US = 5;
const unsigned int MIN_STEP_INTERVAL_US = 250;
const unsigned int MAX_STEP_INTERVAL_US = 12000;
const float DEFAULT_MAX_FEEDRATE_MM_S = 20.0;
const float DEFAULT_ACCELERATION_MM_S2 = 100.0;
const float START_FEEDRATE_MM_S = 2.0;
const int DEFAULT_CIRCLE_SEGMENTS = 120;
const int MIN_CIRCLE_SEGMENTS = 24;
const int MAX_CIRCLE_SEGMENTS = 360;
const int MAX_CIRCLE_LOOPS = 10;

// Pump settings from the syringe pump sketch.
const float PUMP_MOTOR_STEPS_PER_REV = 200.0;
const float PUMP_MICROSTEPS = 16.0;
const float PUMP_LEADSCREW_MM_PER_REV = 8.0;
const float PUMP_STEPS_PER_MM = (PUMP_MOTOR_STEPS_PER_REV * PUMP_MICROSTEPS) / PUMP_LEADSCREW_MM_PER_REV;
const float SMALL_SYRINGE_MM_PER_ML = 43.0 / 2.0;
const float LARGE_SYRINGE_MM_PER_ML = 89.0 / 10.0;
const float DEFAULT_PUMP_SPEED_MM_S = 5.0;
const float MIN_PUMP_SPEED_MM_S = 0.1;
const float MAX_PUMP_SPEED_MM_S = 20.0;

float currentX = 0.0;
float currentY = 0.0;
float maxFeedrateMmS = DEFAULT_MAX_FEEDRATE_MM_S;
float accelerationMmS2 = DEFAULT_ACCELERATION_MM_S2;
float pumpSpeedMmS = DEFAULT_PUMP_SPEED_MM_S;
bool homed = true;
bool emergencyStop = false;
String inputLine;

struct PumpMotor {
  const char *name;
  byte stepPin;
  byte dirPin;
  byte enPin;
  bool invertDir;
};

PumpMotor zPump = {"Z", Z_PUMP_STEP_PIN, Z_PUMP_DIR_PIN, Z_PUMP_EN_PIN, INVERT_Z_PUMP_DIR};
PumpMotor e1Pump = {"E1", E1_PUMP_STEP_PIN, E1_PUMP_DIR_PIN, E1_PUMP_EN_PIN, INVERT_E1_PUMP_DIR};

void setup() {
  pinMode(MOTOR_A_STEP_PIN, OUTPUT);
  pinMode(MOTOR_A_DIR_PIN, OUTPUT);
  pinMode(MOTOR_A_EN_PIN, OUTPUT);
  pinMode(MOTOR_B_STEP_PIN, OUTPUT);
  pinMode(MOTOR_B_DIR_PIN, OUTPUT);
  pinMode(MOTOR_B_EN_PIN, OUTPUT);
  pinMode(Z_PUMP_STEP_PIN, OUTPUT);
  pinMode(Z_PUMP_DIR_PIN, OUTPUT);
  pinMode(Z_PUMP_EN_PIN, OUTPUT);
  pinMode(E1_PUMP_STEP_PIN, OUTPUT);
  pinMode(E1_PUMP_DIR_PIN, OUTPUT);
  pinMode(E1_PUMP_EN_PIN, OUTPUT);

  disableGantryMotors();
  disablePumpMotor(&zPump);
  disablePumpMotor(&e1Pump);
  Serial.begin(115200);
  delay(800);

  Serial.println(F("RAMPS gantry + pump controller ready."));
  Serial.println(F("Manual zero mode: put gantry at left-bottom before power-up or send SETZERO there."));
  printHelp();
  setZero();
}

void loop() {
  readSerialCommands();
}

void printHelp() {
  Serial.println(F("Modules: GANTRY ... | PUMP ..."));
  Serial.println(F("GANTRY: SETZERO | SETPOS X30 Y20 | GOTO X20 Y30 | X10 Y5 | CIRCLE X30 Y30 R8 N120 L1 | POS | SPEED S20 | ACCEL S100"));
  Serial.println(F("GANTRY: DEPOSIT X30 Y30 R8 V0.5 N120 L1"));
  Serial.println(F("PUMP: ZS0.1 | E1S-0.1 | PAIR ZS0.1 E1S-0.1 | ZL0.1 | E1M5 | SPEED S5 | SETTINGS | ENABLE Z | DISABLE E1"));
  Serial.println(F("Global: STOP | UNLOCK | HELP"));
}

void enableGantryMotors() {
  digitalWrite(MOTOR_A_EN_PIN, LOW);
  digitalWrite(MOTOR_B_EN_PIN, LOW);
}

void disableGantryMotors() {
  digitalWrite(MOTOR_A_EN_PIN, HIGH);
  digitalWrite(MOTOR_B_EN_PIN, HIGH);
}

void enablePumpMotor(PumpMotor *pump) {
  digitalWrite(pump->enPin, LOW);
}

void disablePumpMotor(PumpMotor *pump) {
  digitalWrite(pump->enPin, HIGH);
}

void disableAllMotors() {
  disableGantryMotors();
  disablePumpMotor(&zPump);
  disablePumpMotor(&e1Pump);
}

void setMotorDir(byte dirPin, int dirSign, bool invert) {
  bool level = dirSign >= 0;
  if (invert) {
    level = !level;
  }
  digitalWrite(dirPin, level ? HIGH : LOW);
}

void pulseMotor(byte stepPin) {
  digitalWrite(stepPin, HIGH);
  delayMicroseconds(STEP_HIGH_US);
  digitalWrite(stepPin, LOW);
}

void pulseBoth(bool stepA, bool stepB) {
  if (stepA) {
    digitalWrite(MOTOR_A_STEP_PIN, HIGH);
  }
  if (stepB) {
    digitalWrite(MOTOR_B_STEP_PIN, HIGH);
  }
  delayMicroseconds(STEP_HIGH_US);
  if (stepA) {
    digitalWrite(MOTOR_A_STEP_PIN, LOW);
  }
  if (stepB) {
    digitalWrite(MOTOR_B_STEP_PIN, LOW);
  }
}

bool checkStopRequest() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      inputLine.trim();
      inputLine.toUpperCase();
      if (inputLine == "STOP") {
        emergencyStop = true;
        disableAllMotors();
        Serial.println(F("EMERGENCY STOP. Send UNLOCK to continue."));
      }
      inputLine = "";
    } else {
      inputLine += c;
      if (inputLine.length() > 80) {
        inputLine = "";
      }
    }
  }
  return emergencyStop;
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      inputLine.trim();
      if (inputLine.length() > 0) {
        handleCommand(inputLine);
      }
      inputLine = "";
    } else {
      inputLine += c;
      if (inputLine.length() > 80) {
        inputLine = "";
      }
    }
  }
}

bool extractValue(String cmd, char key, float &value) {
  int i = cmd.lastIndexOf(key);
  if (i < 0) {
    return false;
  }

  int start = i + 1;
  while (start < cmd.length() && cmd[start] == ' ') {
    start++;
  }

  int end = start;
  while (end < cmd.length()) {
    char c = cmd[end];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
      end++;
    } else {
      break;
    }
  }

  if (end == start) {
    return false;
  }

  value = cmd.substring(start, end).toFloat();
  return true;
}

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "STOP") {
    emergencyStop = true;
    disableAllMotors();
    Serial.println(F("EMERGENCY STOP. Send UNLOCK to continue."));
    return;
  }

  if (cmd == "UNLOCK") {
    emergencyStop = false;
    Serial.println(F("Unlocked."));
    Serial.println(F("OK"));
    return;
  }

  if (cmd == "HELP") {
    printHelp();
    return;
  }

  if (cmd.startsWith("GANTRY ")) {
    handleGantryCommand(cmd.substring(7));
    return;
  }

  if (cmd.startsWith("G ")) {
    handleGantryCommand(cmd.substring(2));
    return;
  }

  if (cmd.startsWith("PUMP ")) {
    handlePumpCommand(cmd.substring(5));
    return;
  }

  if (cmd.startsWith("P ")) {
    handlePumpCommand(cmd.substring(2));
    return;
  }

  if (cmd.startsWith("Z") || cmd.startsWith("E1")) {
    handlePumpCommand(cmd);
    return;
  }

  handleGantryCommand(cmd);
}

void handleGantryCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "POS") {
    printPosition();
    return;
  }

  if (cmd == "SETTINGS") {
    printSettings();
    Serial.println(F("OK"));
    return;
  }

  if (emergencyStop) {
    Serial.println(F("Stopped. Send UNLOCK to continue."));
    return;
  }

  if (cmd == "HOME" || cmd == "SETZERO") {
    setZero();
    Serial.println(F("OK"));
    return;
  }

  if (cmd.startsWith("SETPOS")) {
    handleSetPosition(cmd);
    return;
  }

  if (cmd.startsWith("SPEED")) {
    float value = 0.0;
    if (extractValue(cmd, 'S', value) && value >= 1.0 && value <= 80.0) {
      maxFeedrateMmS = value;
      printSettings();
      Serial.println(F("OK"));
    } else {
      Serial.println(F("Use SPEED S1..S80"));
    }
    return;
  }

  if (cmd.startsWith("ACCEL")) {
    float value = 0.0;
    if (extractValue(cmd, 'S', value) && value >= 10.0 && value <= 1000.0) {
      accelerationMmS2 = value;
      printSettings();
      Serial.println(F("OK"));
    } else {
      Serial.println(F("Use ACCEL S10..S1000"));
    }
    return;
  }

  if (cmd.startsWith("CIRCLE")) {
    handleCircleCommand(cmd);
    return;
  }

  if (cmd.startsWith("DEPOSIT")) {
    handleDepositCommand(cmd);
    return;
  }

  float xValue = 0.0;
  float yValue = 0.0;
  bool hasX = extractValue(cmd, 'X', xValue);
  bool hasY = extractValue(cmd, 'Y', yValue);

  if (!hasX && !hasY) {
    Serial.println(F("Unknown command. Send HELP."));
    return;
  }

  if (!homed) {
    Serial.println(F("Position is not set. Move to left-bottom and send SETZERO."));
    return;
  }

  float targetX = currentX;
  float targetY = currentY;

  if (cmd.startsWith("GOTO")) {
    if (hasX) {
      targetX = xValue;
    }
    if (hasY) {
      targetY = yValue;
    }
  } else {
    if (hasX) {
      targetX += xValue;
    }
    if (hasY) {
      targetY += yValue;
    }
  }

  moveTo(targetX, targetY, true);
}

void printPosition() {
  Serial.print(F("POS X"));
  Serial.print(currentX, 3);
  Serial.print(F(" Y"));
  Serial.print(currentY, 3);
  Serial.print(F(" homed="));
  Serial.println(homed ? F("yes") : F("no"));
}

void handleSetPosition(String cmd) {
  float xValue = currentX;
  float yValue = currentY;
  bool hasX = extractValue(cmd, 'X', xValue);
  bool hasY = extractValue(cmd, 'Y', yValue);

  if (!hasX && !hasY) {
    Serial.println(F("Use SETPOS Xvalue Yvalue"));
    return;
  }
  if (!withinLimits(xValue, yValue)) {
    Serial.println(F("SETPOS rejected: outside soft limits."));
    return;
  }

  currentX = xValue;
  currentY = yValue;
  homed = true;
  Serial.println(F("Logical position updated without movement."));
  printPosition();
  Serial.println(F("OK"));
}

void printSettings() {
  Serial.print(F("SETTINGS speed="));
  Serial.print(maxFeedrateMmS, 2);
  Serial.print(F("mm/s accel="));
  Serial.print(accelerationMmS2, 2);
  Serial.println(F("mm/s^2"));
}

void printPumpSettings() {
  Serial.print(F("PUMP SETTINGS speed="));
  Serial.print(pumpSpeedMmS, 3);
  Serial.print(F("mm/s stepsPerMm="));
  Serial.print(PUMP_STEPS_PER_MM, 3);
  Serial.print(F(" smallMmPerMl="));
  Serial.print(SMALL_SYRINGE_MM_PER_ML, 3);
  Serial.println();
  Serial.println(F("OK"));
}

PumpMotor *findPumpMotor(String cmd, int &prefixLength) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.startsWith("E1")) {
    prefixLength = 2;
    return &e1Pump;
  }

  if (cmd.startsWith("Z")) {
    prefixLength = 1;
    return &zPump;
  }

  prefixLength = 0;
  return NULL;
}

void setPumpDir(PumpMotor *pump, int dirSign) {
  bool level = dirSign >= 0;
  if (pump->invertDir) {
    level = !level;
  }
  digitalWrite(pump->dirPin, level ? HIGH : LOW);
}

unsigned int pumpStepIntervalUs() {
  float stepsPerSecond = pumpSpeedMmS * PUMP_STEPS_PER_MM;
  if (stepsPerSecond < 1.0) {
    stepsPerSecond = 1.0;
  }

  float interval = 1000000.0 / stepsPerSecond;
  if (interval < 250.0) {
    interval = 250.0;
  }
  if (interval > 30000.0) {
    interval = 30000.0;
  }
  return (unsigned int)interval;
}

long pumpMmToSteps(float mm) {
  float steps = mm * PUMP_STEPS_PER_MM;
  return steps >= 0.0 ? (long)(steps + 0.5) : (long)(steps - 0.5);
}

float pumpModeValueToMm(char mode, float value) {
  if (mode == 'S') {
    return value * SMALL_SYRINGE_MM_PER_ML;
  }
  if (mode == 'L') {
    return value * LARGE_SYRINGE_MM_PER_ML;
  }
  if (mode == 'M') {
    return value;
  }
  return 0.0;
}

void runPumpMove(PumpMotor *pump, float mm) {
  long steps = pumpMmToSteps(mm);
  long absSteps = labs(steps);

  if (absSteps == 0) {
    Serial.println(F("Pump move rounded to 0 steps."));
    Serial.println(F("OK"));
    return;
  }

  enablePumpMotor(pump);
  setPumpDir(pump, steps >= 0 ? 1 : -1);

  unsigned int intervalUs = pumpStepIntervalUs();
  for (long i = 0; i < absSteps; i++) {
    if (checkStopRequest()) {
      return;
    }
    digitalWrite(pump->stepPin, HIGH);
    delayMicroseconds(STEP_HIGH_US);
    digitalWrite(pump->stepPin, LOW);
    delayMicroseconds(intervalUs);
  }

  Serial.print(F("PUMP "));
  Serial.print(pump->name);
  Serial.print(F(" move "));
  Serial.print(mm, 4);
  Serial.print(F(" mm, steps "));
  Serial.println(absSteps);
  Serial.println(F("OK"));
}

void pulsePumpPair(bool stepA, byte stepPinA, bool stepB, byte stepPinB) {
  if (stepA) {
    digitalWrite(stepPinA, HIGH);
  }
  if (stepB) {
    digitalWrite(stepPinB, HIGH);
  }
  delayMicroseconds(STEP_HIGH_US);
  if (stepA) {
    digitalWrite(stepPinA, LOW);
  }
  if (stepB) {
    digitalWrite(stepPinB, LOW);
  }
}

void runPumpPairMove(PumpMotor *pumpA, float mmA, PumpMotor *pumpB, float mmB) {
  if (pumpA == pumpB) {
    Serial.println(F("PAIR rejected: use two different pump motors."));
    return;
  }

  long stepsA = pumpMmToSteps(mmA);
  long stepsB = pumpMmToSteps(mmB);
  long absA = labs(stepsA);
  long absB = labs(stepsB);
  long total = max(absA, absB);

  if (total == 0) {
    Serial.println(F("PAIR move rounded to 0 steps."));
    Serial.println(F("OK"));
    return;
  }

  enablePumpMotor(pumpA);
  enablePumpMotor(pumpB);
  setPumpDir(pumpA, stepsA >= 0 ? 1 : -1);
  setPumpDir(pumpB, stepsB >= 0 ? 1 : -1);

  unsigned int intervalUs = pumpStepIntervalUs();
  long errorA = 0;
  long errorB = 0;

  for (long i = 0; i < total; i++) {
    if (checkStopRequest()) {
      return;
    }

    errorA += absA;
    errorB += absB;

    bool stepA = false;
    bool stepB = false;

    if (errorA >= total) {
      errorA -= total;
      stepA = true;
    }
    if (errorB >= total) {
      errorB -= total;
      stepB = true;
    }

    pulsePumpPair(stepA, pumpA->stepPin, stepB, pumpB->stepPin);
    delayMicroseconds(intervalUs);
  }

  Serial.print(F("PUMP PAIR "));
  Serial.print(pumpA->name);
  Serial.print(F(" "));
  Serial.print(mmA, 4);
  Serial.print(F(" mm, "));
  Serial.print(pumpB->name);
  Serial.print(F(" "));
  Serial.print(mmB, 4);
  Serial.println(F(" mm"));
  Serial.println(F("OK"));
}

bool parsePumpMoveSpec(String spec, PumpMotor *&pump, float &mm) {
  spec.trim();
  spec.toUpperCase();

  int prefixLength = 0;
  pump = findPumpMotor(spec, prefixLength);
  if (pump == NULL) {
    return false;
  }

  String payload = spec.substring(prefixLength);
  payload.trim();
  if (payload.length() < 2) {
    return false;
  }

  char mode = payload.charAt(0);
  float value = payload.substring(1).toFloat();
  if (mode != 'S' && mode != 'L' && mode != 'M') {
    return false;
  }

  mm = pumpModeValueToMm(mode, value);
  return true;
}

void handlePumpPairCommand(String cmd) {
  String payload = cmd;
  payload.trim();

  int spaceIndex = payload.indexOf(' ');
  if (spaceIndex < 0) {
    Serial.println(F("Use PUMP PAIR ZS0.1 E1S-0.1"));
    return;
  }

  String first = payload.substring(0, spaceIndex);
  String second = payload.substring(spaceIndex + 1);
  first.trim();
  second.trim();

  PumpMotor *pumpA = NULL;
  PumpMotor *pumpB = NULL;
  float mmA = 0.0;
  float mmB = 0.0;

  if (!parsePumpMoveSpec(first, pumpA, mmA) || !parsePumpMoveSpec(second, pumpB, mmB)) {
    Serial.println(F("Use PUMP PAIR ZS0.1 E1S-0.1"));
    return;
  }

  runPumpPairMove(pumpA, mmA, pumpB, mmB);
}

void handlePumpCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (emergencyStop) {
    Serial.println(F("Stopped. Send UNLOCK to continue."));
    return;
  }

  if (cmd == "SETTINGS") {
    printPumpSettings();
    return;
  }

  if (cmd.startsWith("PAIR ")) {
    handlePumpPairCommand(cmd.substring(5));
    return;
  }

  if (cmd.startsWith("SPEED")) {
    float value = 0.0;
    if (extractValue(cmd, 'S', value) && value >= MIN_PUMP_SPEED_MM_S && value <= MAX_PUMP_SPEED_MM_S) {
      pumpSpeedMmS = value;
      printPumpSettings();
    } else {
      Serial.println(F("Use PUMP SPEED S0.1..S20"));
    }
    return;
  }

  if (cmd.startsWith("ENABLE ") || cmd.startsWith("DISABLE ")) {
    bool shouldEnable = cmd.startsWith("ENABLE ");
    String motorText = cmd.substring(shouldEnable ? 7 : 8);
    motorText.trim();

    int utilityPrefixLength = 0;
    PumpMotor *utilityPump = findPumpMotor(motorText, utilityPrefixLength);
    if (utilityPump == NULL || utilityPrefixLength != motorText.length()) {
      Serial.println(F("Unknown pump motor. Use Z or E1."));
      return;
    }

    if (shouldEnable) {
      enablePumpMotor(utilityPump);
      Serial.print(F("PUMP "));
      Serial.print(utilityPump->name);
      Serial.println(F(" enabled."));
    } else {
      disablePumpMotor(utilityPump);
      Serial.print(F("PUMP "));
      Serial.print(utilityPump->name);
      Serial.println(F(" disabled."));
    }
    Serial.println(F("OK"));
    return;
  }

  int prefixLength = 0;
  PumpMotor *pump = findPumpMotor(cmd, prefixLength);
  if (pump == NULL) {
    Serial.println(F("Unknown pump motor. Use Z or E1."));
    return;
  }

  String payload = cmd.substring(prefixLength);
  payload.trim();

  if (payload == "ENABLE") {
    enablePumpMotor(pump);
    Serial.print(F("PUMP "));
    Serial.print(pump->name);
    Serial.println(F(" enabled."));
    Serial.println(F("OK"));
    return;
  }

  if (payload == "DISABLE") {
    disablePumpMotor(pump);
    Serial.print(F("PUMP "));
    Serial.print(pump->name);
    Serial.println(F(" disabled."));
    Serial.println(F("OK"));
    return;
  }

  if (payload.length() < 2) {
    Serial.println(F("Use PUMP ZS0.1, PUMP E1S-0.1, PUMP ZM5, or PUMP SPEED S5."));
    return;
  }

  char mode = payload.charAt(0);
  float value = payload.substring(1).toFloat();

  if (mode == 'S') {
    runPumpMove(pump, pumpModeValueToMm(mode, value));
    return;
  }

  if (mode == 'L') {
    runPumpMove(pump, pumpModeValueToMm(mode, value));
    return;
  }

  if (mode == 'M') {
    runPumpMove(pump, pumpModeValueToMm(mode, value));
    return;
  }

  Serial.println(F("Unknown pump mode. Use S for small ml, L for large ml, or M for mm."));
}

bool withinLimits(float x, float y) {
  return x >= 0.0 && x <= MAX_X_MM && y >= 0.0 && y <= MAX_Y_MM;
}

long mmToSteps(float mm, float stepsPerMm) {
  float steps = mm * stepsPerMm;
  return steps >= 0.0 ? (long)(steps + 0.5) : (long)(steps - 0.5);
}

void moveTo(float targetX, float targetY, bool reportPosition) {
  if (!withinLimits(targetX, targetY)) {
    Serial.println(F("Move rejected: outside soft limits."));
    return;
  }

  float dxMm = targetX - currentX;
  float dyMm = targetY - currentY;
  if (INVERT_X_AXIS) {
    dxMm = -dxMm;
  }
  if (INVERT_Y_AXIS) {
    dyMm = -dyMm;
  }

  float pathMm = sqrt(dxMm * dxMm + dyMm * dyMm);
  long xSteps = mmToSteps(dxMm, STEPS_PER_MM_X);
  long ySteps = mmToSteps(dyMm, STEPS_PER_MM_Y);

  // Motor transform: A = X + Y, B = X - Y.
  long aSteps = xSteps + ySteps;
  long bSteps = xSteps - ySteps;

  runMotorSteps(aSteps, bSteps, pathMm);

  if (!emergencyStop) {
    currentX = targetX;
    currentY = targetY;
    if (reportPosition) {
      printPosition();
      Serial.println(F("OK"));
    }
  }
}

void handleCircleCommand(String cmd) {
  float centerX = currentX;
  float centerY = currentY;
  float radius = 0.0;
  float segmentsValue = DEFAULT_CIRCLE_SEGMENTS;
  float loopsValue = 1.0;

  bool hasX = extractValue(cmd, 'X', centerX);
  bool hasY = extractValue(cmd, 'Y', centerY);
  bool hasR = extractValue(cmd, 'R', radius);
  extractValue(cmd, 'N', segmentsValue);
  extractValue(cmd, 'L', loopsValue);

  int segments = (int)(segmentsValue + 0.5);
  int loops = (int)(loopsValue + 0.5);

  if (!hasX || !hasY || !hasR || radius <= 0.0) {
    Serial.println(F("Use CIRCLE Xcenter Ycenter Rradius Nsegments Lloops"));
    return;
  }
  if (segments < MIN_CIRCLE_SEGMENTS || segments > MAX_CIRCLE_SEGMENTS) {
    Serial.println(F("Circle rejected: N must be 24..360."));
    return;
  }
  if (loops < 1 || loops > MAX_CIRCLE_LOOPS) {
    Serial.println(F("Circle rejected: L must be 1..10."));
    return;
  }
  if (!withinLimits(centerX - radius, centerY - radius) || !withinLimits(centerX + radius, centerY + radius)) {
    Serial.println(F("Circle rejected: outside soft limits."));
    return;
  }

  float startX = centerX + radius;
  float startY = centerY;
  moveTo(startX, startY, false);
  if (emergencyStop) {
    return;
  }

  const float FULL_CIRCLE_RAD = 6.28318530718;
  for (int loopIndex = 0; loopIndex < loops; loopIndex++) {
    for (int i = 1; i <= segments; i++) {
      if (checkStopRequest()) {
        return;
      }

      float angle = FULL_CIRCLE_RAD * ((float)i / (float)segments);
      float targetX = centerX + radius * cos(angle);
      float targetY = centerY + radius * sin(angle);
      moveTo(targetX, targetY, false);

      if (emergencyStop) {
        return;
      }
    }
  }

  Serial.println(F("CIRCLE DONE"));
  printPosition();
  Serial.println(F("OK"));
}

void handleDepositCommand(String cmd) {
  float centerX = currentX;
  float centerY = currentY;
  float radius = 0.0;
  float volumeMl = 0.0;
  float segmentsValue = DEFAULT_CIRCLE_SEGMENTS;
  float loopsValue = 1.0;

  bool hasX = extractValue(cmd, 'X', centerX);
  bool hasY = extractValue(cmd, 'Y', centerY);
  bool hasR = extractValue(cmd, 'R', radius);
  bool hasV = extractValue(cmd, 'V', volumeMl);
  extractValue(cmd, 'N', segmentsValue);
  extractValue(cmd, 'L', loopsValue);

  int segments = (int)(segmentsValue + 0.5);
  int loops = (int)(loopsValue + 0.5);

  if (!hasX || !hasY || !hasR || !hasV || radius <= 0.0 || volumeMl <= 0.0) {
    Serial.println(F("Use DEPOSIT Xcenter Ycenter Rradius Vml Nsegments Lloops"));
    return;
  }
  if (segments < MIN_CIRCLE_SEGMENTS || segments > MAX_CIRCLE_SEGMENTS) {
    Serial.println(F("DEPOSIT rejected: N must be 24..360."));
    return;
  }
  if (loops < 1 || loops > MAX_CIRCLE_LOOPS) {
    Serial.println(F("DEPOSIT rejected: L must be 1..10."));
    return;
  }
  if (!withinLimits(centerX - radius, centerY - radius) || !withinLimits(centerX + radius, centerY + radius)) {
    Serial.println(F("DEPOSIT rejected: circle outside soft limits."));
    return;
  }
  if (!homed) {
    Serial.println(F("Position is not set. Move to left-bottom and send SETZERO."));
    return;
  }

  float startX = centerX + radius;
  float startY = centerY;
  moveTo(startX, startY, false);
  if (emergencyStop) {
    return;
  }

  long totalPumpSteps = pumpMmToSteps(volumeMl * SMALL_SYRINGE_MM_PER_ML);
  long totalSegments = (long)segments * (long)loops;
  long pumpAbsDone = 0;
  long pumpAbsTotal = labs(totalPumpSteps);
  int pumpSign = totalPumpSteps >= 0 ? 1 : -1;

  const float FULL_CIRCLE_RAD = 6.28318530718;
  long segmentIndex = 0;
  for (int loopIndex = 0; loopIndex < loops; loopIndex++) {
    for (int i = 1; i <= segments; i++) {
      if (checkStopRequest()) {
        return;
      }

      segmentIndex++;
      long targetPumpAbs = (pumpAbsTotal * segmentIndex + totalSegments / 2) / totalSegments;
      long segmentPumpSteps = (targetPumpAbs - pumpAbsDone) * pumpSign;
      pumpAbsDone = targetPumpAbs;

      float angle = FULL_CIRCLE_RAD * ((float)i / (float)segments);
      float targetX = centerX + radius * cos(angle);
      float targetY = centerY + radius * sin(angle);
      moveToWithPumpSteps(targetX, targetY, &e1Pump, segmentPumpSteps, false);

      if (emergencyStop) {
        return;
      }
    }
  }

  Serial.println(F("DEPOSIT DONE"));
  printPosition();
  Serial.println(F("OK"));
}

unsigned int speedToIntervalUs(float speedMmS, float mmPerMasterStep) {
  if (speedMmS < START_FEEDRATE_MM_S) {
    speedMmS = START_FEEDRATE_MM_S;
  }

  float interval = (mmPerMasterStep / speedMmS) * 1000000.0;
  if (interval < MIN_STEP_INTERVAL_US) {
    interval = MIN_STEP_INTERVAL_US;
  }
  if (interval > MAX_STEP_INTERVAL_US) {
    interval = MAX_STEP_INTERVAL_US;
  }
  return (unsigned int)interval;
}

float trapezoidSpeed(float doneMm, float remainingMm) {
  float accelSpeed = sqrt(START_FEEDRATE_MM_S * START_FEEDRATE_MM_S + 2.0 * accelerationMmS2 * doneMm);
  float decelSpeed = sqrt(START_FEEDRATE_MM_S * START_FEEDRATE_MM_S + 2.0 * accelerationMmS2 * remainingMm);
  float speed = accelSpeed;

  if (decelSpeed < speed) {
    speed = decelSpeed;
  }
  if (maxFeedrateMmS < speed) {
    speed = maxFeedrateMmS;
  }
  if (speed < START_FEEDRATE_MM_S) {
    speed = START_FEEDRATE_MM_S;
  }

  return speed;
}

void runMotorSteps(long aSteps, long bSteps, float pathMm) {
  long absA = labs(aSteps);
  long absB = labs(bSteps);
  long total = max(absA, absB);

  if (total == 0) {
    return;
  }

  float mmPerMasterStep = pathMm / total;
  if (mmPerMasterStep <= 0.0) {
    mmPerMasterStep = 1.0 / max(STEPS_PER_MM_X, STEPS_PER_MM_Y);
  }

  enableGantryMotors();
  setMotorDir(MOTOR_A_DIR_PIN, aSteps >= 0 ? 1 : -1, INVERT_MOTOR_A_DIR);
  setMotorDir(MOTOR_B_DIR_PIN, bSteps >= 0 ? 1 : -1, INVERT_MOTOR_B_DIR);

  long errorA = 0;
  long errorB = 0;

  for (long i = 0; i < total; i++) {
    if (checkStopRequest()) {
      return;
    }

    errorA += absA;
    errorB += absB;

    bool stepA = false;
    bool stepB = false;

    if (errorA >= total) {
      errorA -= total;
      stepA = true;
    }
    if (errorB >= total) {
      errorB -= total;
      stepB = true;
    }

    pulseBoth(stepA, stepB);

    float doneMm = (i + 1) * mmPerMasterStep;
    float remainingMm = pathMm - doneMm;
    if (remainingMm < 0.0) {
      remainingMm = 0.0;
    }
    unsigned int intervalUs = speedToIntervalUs(trapezoidSpeed(doneMm, remainingMm), mmPerMasterStep);
    delayMicroseconds(intervalUs);
  }
}

void runMotorAndPumpSteps(long aSteps, long bSteps, float pathMm, PumpMotor *pump, long pumpSteps) {
  long absA = labs(aSteps);
  long absB = labs(bSteps);
  long absPump = labs(pumpSteps);
  long total = max(max(absA, absB), absPump);

  if (total == 0) {
    return;
  }

  float mmPerMasterStep = pathMm / total;
  if (mmPerMasterStep <= 0.0) {
    mmPerMasterStep = 1.0 / max(STEPS_PER_MM_X, STEPS_PER_MM_Y);
  }

  enableGantryMotors();
  enablePumpMotor(pump);
  setMotorDir(MOTOR_A_DIR_PIN, aSteps >= 0 ? 1 : -1, INVERT_MOTOR_A_DIR);
  setMotorDir(MOTOR_B_DIR_PIN, bSteps >= 0 ? 1 : -1, INVERT_MOTOR_B_DIR);
  setPumpDir(pump, pumpSteps >= 0 ? 1 : -1);

  long errorA = 0;
  long errorB = 0;
  long errorPump = 0;

  for (long i = 0; i < total; i++) {
    if (checkStopRequest()) {
      return;
    }

    errorA += absA;
    errorB += absB;
    errorPump += absPump;

    bool stepA = false;
    bool stepB = false;
    bool stepPump = false;

    if (errorA >= total) {
      errorA -= total;
      stepA = true;
    }
    if (errorB >= total) {
      errorB -= total;
      stepB = true;
    }
    if (errorPump >= total) {
      errorPump -= total;
      stepPump = true;
    }

    if (stepA) {
      digitalWrite(MOTOR_A_STEP_PIN, HIGH);
    }
    if (stepB) {
      digitalWrite(MOTOR_B_STEP_PIN, HIGH);
    }
    if (stepPump) {
      digitalWrite(pump->stepPin, HIGH);
    }
    delayMicroseconds(STEP_HIGH_US);
    if (stepA) {
      digitalWrite(MOTOR_A_STEP_PIN, LOW);
    }
    if (stepB) {
      digitalWrite(MOTOR_B_STEP_PIN, LOW);
    }
    if (stepPump) {
      digitalWrite(pump->stepPin, LOW);
    }

    float doneMm = (i + 1) * mmPerMasterStep;
    float remainingMm = pathMm - doneMm;
    if (remainingMm < 0.0) {
      remainingMm = 0.0;
    }
    unsigned int intervalUs = speedToIntervalUs(trapezoidSpeed(doneMm, remainingMm), mmPerMasterStep);
    delayMicroseconds(intervalUs);
  }
}

void moveToWithPumpSteps(float targetX, float targetY, PumpMotor *pump, long pumpSteps, bool reportPosition) {
  if (!withinLimits(targetX, targetY)) {
    Serial.println(F("Move rejected: outside soft limits."));
    return;
  }

  float dxMm = targetX - currentX;
  float dyMm = targetY - currentY;
  if (INVERT_X_AXIS) {
    dxMm = -dxMm;
  }
  if (INVERT_Y_AXIS) {
    dyMm = -dyMm;
  }

  float pathMm = sqrt(dxMm * dxMm + dyMm * dyMm);
  long xSteps = mmToSteps(dxMm, STEPS_PER_MM_X);
  long ySteps = mmToSteps(dyMm, STEPS_PER_MM_Y);

  long aSteps = xSteps + ySteps;
  long bSteps = xSteps - ySteps;

  runMotorAndPumpSteps(aSteps, bSteps, pathMm, pump, pumpSteps);

  if (!emergencyStop) {
    currentX = targetX;
    currentY = targetY;
    if (reportPosition) {
      printPosition();
      Serial.println(F("OK"));
    }
  }
}

void setZero() {
  currentX = 0.0;
  currentY = 0.0;
  homed = true;
  Serial.println(F("Current position set to left-bottom X0 Y0."));
  printPosition();
}
