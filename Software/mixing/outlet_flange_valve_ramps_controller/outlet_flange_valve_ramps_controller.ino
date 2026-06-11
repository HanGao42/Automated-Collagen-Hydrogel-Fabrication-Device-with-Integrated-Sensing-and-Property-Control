/*
  Arduino Mega + RAMPS 1.4 Z flange controller

  Hardware:
    - Arduino Mega 2560
    - RAMPS 1.4
    - Z driver socket with TMC2208 in standalone STEP/DIR mode

  Serial commands, 115200 baud:
    UP          move one calibrated height upward
    DOWN        move one calibrated height downward
    MIX_X_CW90  rotate the X-area mixing valve motor one quarter turn clockwise
    MIX_X_CCW90 rotate the X-area mixing valve motor one quarter turn counter-clockwise
    MIX_Y_CW180 rotate the Y-area mixing valve motor one half turn clockwise
    MIX_Y_CCW180 rotate the Y-area mixing valve motor one half turn counter-clockwise
    STOP        immediately stop stepping and disable Z driver power
    ENABLE      enable Z driver power without moving
    ON          same as ENABLE
    UNLOCK      clear stop state and enable Z driver power
    OFF         disable Z driver power without changing stop state
    STATUS      print motor power and stop state
    SETTINGS    print motion settings
    HELP        print commands

  This firmware deliberately uses direct stepping instead of AccelStepper so
  STOP can be detected during a blocking move, matching the gantry controller.
*/

const byte Z_STEP_PIN = 46;    // RAMPS Z_STEP
const byte Z_DIR_PIN = 48;     // RAMPS Z_DIR
const byte Z_ENABLE_PIN = 62;  // RAMPS Z_ENABLE, active LOW

const byte X_STEP_PIN = 54;    // RAMPS X_STEP
const byte X_DIR_PIN = 55;     // RAMPS X_DIR
const byte X_ENABLE_PIN = 38;  // RAMPS X_ENABLE, active LOW

const byte Y_STEP_PIN = 60;    // RAMPS Y_STEP
const byte Y_DIR_PIN = 61;     // RAMPS Y_DIR
const byte Y_ENABLE_PIN = 56;  // RAMPS Y_ENABLE, active LOW

// Calibrate these for your screw, motor, and TMC2208 microstep setting.
const long HEIGHT_TRAVEL_STEPS = 6400L;
const long MIX_VALVE_QUARTER_TURN_STEPS = 400L;
const long MIX_VALVE_HALF_TURN_STEPS = MIX_VALVE_QUARTER_TURN_STEPS * 2L;
const unsigned int STEP_HIGH_US = 5;
const unsigned int MIN_STEP_INTERVAL_US = 500;
const unsigned int MAX_STEP_INTERVAL_US = 8000;
const float DEFAULT_MAX_SPEED_STEPS_S = 1200.0;
const float DEFAULT_ACCEL_STEPS_S2 = 800.0;
const float START_SPEED_STEPS_S = 150.0;
const float MIX_VALVE_MAX_SPEED_STEPS_S = 700.0;
const float MIX_VALVE_ACCEL_STEPS_S2 = 500.0;

// Flip if UP and DOWN are reversed.
const bool INVERT_Z_DIR = false;
const bool INVERT_X_VALVE_DIR = true;
const bool INVERT_Y_VALVE_DIR = false;

String inputLine;
bool emergencyStop = false;
bool motorEnabled = false;
bool xValveEnabled = false;
bool yValveEnabled = false;
float maxSpeedStepsS = DEFAULT_MAX_SPEED_STEPS_S;
float accelStepsS2 = DEFAULT_ACCEL_STEPS_S2;

void setup() {
  pinMode(Z_STEP_PIN, OUTPUT);
  pinMode(Z_DIR_PIN, OUTPUT);
  pinMode(Z_ENABLE_PIN, OUTPUT);
  pinMode(X_STEP_PIN, OUTPUT);
  pinMode(X_DIR_PIN, OUTPUT);
  pinMode(X_ENABLE_PIN, OUTPUT);
  pinMode(Y_STEP_PIN, OUTPUT);
  pinMode(Y_DIR_PIN, OUTPUT);
  pinMode(Y_ENABLE_PIN, OUTPUT);

  digitalWrite(Z_STEP_PIN, LOW);
  digitalWrite(Z_DIR_PIN, LOW);
  digitalWrite(X_STEP_PIN, LOW);
  digitalWrite(X_DIR_PIN, LOW);
  digitalWrite(Y_STEP_PIN, LOW);
  digitalWrite(Y_DIR_PIN, LOW);
  disableMotor();
  disableXValveMotor();
  disableYValveMotor();

  Serial.begin(115200);
  delay(800);

  Serial.println(F("RAMPS Z flange ready."));
  printHelp();
}

void loop() {
  readSerialCommands();
}

void printHelp() {
  Serial.println(F("Commands: UP | DOWN | MIX_X_CW90 | MIX_X_CCW90 | MIX_Y_CW180 | MIX_Y_CCW180 | STOP | ENABLE | ON | UNLOCK | OFF | STATUS | SETTINGS | HELP"));
}

void enableMotor() {
  digitalWrite(Z_ENABLE_PIN, LOW);
  motorEnabled = true;
}

void disableMotor() {
  digitalWrite(Z_ENABLE_PIN, HIGH);
  motorEnabled = false;
}

void enableXValveMotor() {
  digitalWrite(X_ENABLE_PIN, LOW);
  xValveEnabled = true;
}

void disableXValveMotor() {
  digitalWrite(X_ENABLE_PIN, HIGH);
  xValveEnabled = false;
}

void enableYValveMotor() {
  digitalWrite(Y_ENABLE_PIN, LOW);
  yValveEnabled = true;
}

void disableYValveMotor() {
  digitalWrite(Y_ENABLE_PIN, HIGH);
  yValveEnabled = false;
}

void disableAllMotors() {
  disableMotor();
  disableXValveMotor();
  disableYValveMotor();
}

void setDirection(bool up) {
  bool level = up;
  if (INVERT_Z_DIR) {
    level = !level;
  }
  digitalWrite(Z_DIR_PIN, level ? HIGH : LOW);
}

void pulseStep() {
  digitalWrite(Z_STEP_PIN, HIGH);
  delayMicroseconds(STEP_HIGH_US);
  digitalWrite(Z_STEP_PIN, LOW);
}

void pulseStepPin(byte stepPin) {
  digitalWrite(stepPin, HIGH);
  delayMicroseconds(STEP_HIGH_US);
  digitalWrite(stepPin, LOW);
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

bool checkStopRequest() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      inputLine.trim();
      inputLine.toUpperCase();
      if (inputLine == "STOP") {
        emergencyStop = true;
        disableAllMotors();
        Serial.println(F("EMERGENCY STOP. Motor power off. Send ENABLE or UNLOCK to continue."));
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

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "STOP") {
    emergencyStop = true;
    disableAllMotors();
    Serial.println(F("EMERGENCY STOP. Motor power off. Send ENABLE or UNLOCK to continue."));
    return;
  }

  if (cmd == "ENABLE" || cmd == "ON" || cmd == "UNLOCK") {
    emergencyStop = false;
    enableMotor();
    Serial.println(F("Motor power on."));
    Serial.println(F("OK"));
    return;
  }

  if (cmd == "OFF") {
    disableAllMotors();
    Serial.println(F("Motor power off."));
    Serial.println(F("OK"));
    return;
  }

  if (cmd == "HELP") {
    printHelp();
    return;
  }

  if (cmd == "STATUS") {
    printStatus();
    Serial.println(F("OK"));
    return;
  }

  if (cmd == "SETTINGS") {
    printSettings();
    Serial.println(F("OK"));
    return;
  }

  if (emergencyStop) {
    Serial.println(F("Stopped. Send ENABLE or UNLOCK to continue."));
    return;
  }

  if (cmd == "UP") {
    moveHeight(true);
    return;
  }

  if (cmd == "DOWN") {
    moveHeight(false);
    return;
  }

  if (cmd == "MIX_X_CW90") {
    moveMixValve('X', true, MIX_VALVE_QUARTER_TURN_STEPS);
    return;
  }

  if (cmd == "MIX_X_CCW90") {
    moveMixValve('X', false, MIX_VALVE_QUARTER_TURN_STEPS);
    return;
  }

  if (cmd == "MIX_Y_CW180") {
    moveMixValve('Y', true, MIX_VALVE_HALF_TURN_STEPS);
    return;
  }

  if (cmd == "MIX_Y_CCW180") {
    moveMixValve('Y', false, MIX_VALVE_HALF_TURN_STEPS);
    return;
  }

  Serial.println(F("Unknown command. Send HELP."));
}

void printStatus() {
  Serial.print(F("STATUS motor="));
  Serial.print(motorEnabled ? F("on") : F("off"));
  Serial.print(F(" x_valve="));
  Serial.print(xValveEnabled ? F("on") : F("off"));
  Serial.print(F(" y_valve="));
  Serial.print(yValveEnabled ? F("on") : F("off"));
  Serial.print(F(" stopped="));
  Serial.println(emergencyStop ? F("yes") : F("no"));
}

void printSettings() {
  Serial.print(F("SETTINGS travel_steps="));
  Serial.print(HEIGHT_TRAVEL_STEPS);
  Serial.print(F(" max_speed_steps_s="));
  Serial.print(maxSpeedStepsS, 2);
  Serial.print(F(" accel_steps_s2="));
  Serial.print(accelStepsS2, 2);
  Serial.print(F(" mix_quarter_turn_steps="));
  Serial.print(MIX_VALVE_QUARTER_TURN_STEPS);
  Serial.print(F(" mix_speed_steps_s="));
  Serial.print(MIX_VALVE_MAX_SPEED_STEPS_S, 2);
  Serial.print(F(" mix_accel_steps_s2="));
  Serial.println(MIX_VALVE_ACCEL_STEPS_S2, 2);
}

unsigned int speedToIntervalUs(float speedStepsS) {
  if (speedStepsS < START_SPEED_STEPS_S) {
    speedStepsS = START_SPEED_STEPS_S;
  }

  float interval = 1000000.0 / speedStepsS;
  if (interval < MIN_STEP_INTERVAL_US) {
    interval = MIN_STEP_INTERVAL_US;
  }
  if (interval > MAX_STEP_INTERVAL_US) {
    interval = MAX_STEP_INTERVAL_US;
  }
  return (unsigned int)interval;
}

float trapezoidSpeed(long doneSteps, long totalSteps) {
  return trapezoidSpeedFor(doneSteps, totalSteps, maxSpeedStepsS, accelStepsS2);
}

float trapezoidSpeedFor(long doneSteps, long totalSteps, float maxSpeed, float accel) {
  float remainingSteps = totalSteps - doneSteps;
  float accelSpeed = sqrt(START_SPEED_STEPS_S * START_SPEED_STEPS_S + 2.0 * accel * doneSteps);
  float decelSpeed = sqrt(START_SPEED_STEPS_S * START_SPEED_STEPS_S + 2.0 * accel * remainingSteps);
  float speed = accelSpeed;

  if (decelSpeed < speed) {
    speed = decelSpeed;
  }
  if (maxSpeed < speed) {
    speed = maxSpeed;
  }
  if (speed < START_SPEED_STEPS_S) {
    speed = START_SPEED_STEPS_S;
  }
  return speed;
}

void moveHeight(bool up) {
  if (HEIGHT_TRAVEL_STEPS <= 0) {
    Serial.println(F("Move rejected: HEIGHT_TRAVEL_STEPS must be positive."));
    return;
  }

  enableMotor();
  setDirection(up);
  delayMicroseconds(10);

  Serial.println(up ? F("MOVING UP") : F("MOVING DOWN"));

  for (long i = 0; i < HEIGHT_TRAVEL_STEPS; i++) {
    if (checkStopRequest()) {
      return;
    }

    pulseStep();
    unsigned int intervalUs = speedToIntervalUs(trapezoidSpeed(i + 1, HEIGHT_TRAVEL_STEPS));
    delayMicroseconds(intervalUs);
  }

  Serial.println(F("OK"));
}

void moveMixValve(char axis, bool clockwise, long steps) {
  if (steps <= 0) {
    Serial.println(F("Move rejected: mixing valve steps must be positive."));
    return;
  }

  byte stepPin = X_STEP_PIN;
  byte dirPin = X_DIR_PIN;
  bool inverted = INVERT_X_VALVE_DIR;

  if (axis == 'Y') {
    stepPin = Y_STEP_PIN;
    dirPin = Y_DIR_PIN;
    inverted = INVERT_Y_VALVE_DIR;
    enableYValveMotor();
  } else {
    enableXValveMotor();
  }

  bool level = clockwise;
  if (inverted) {
    level = !level;
  }
  digitalWrite(dirPin, level ? HIGH : LOW);
  delayMicroseconds(10);

  Serial.print(F("MOVING MIX "));
  Serial.print(axis);
  Serial.println(clockwise ? F(" CW") : F(" CCW"));

  for (long i = 0; i < steps; i++) {
    if (checkStopRequest()) {
      return;
    }

    pulseStepPin(stepPin);
    unsigned int intervalUs = speedToIntervalUs(
      trapezoidSpeedFor(i + 1, steps, MIX_VALVE_MAX_SPEED_STEPS_S, MIX_VALVE_ACCEL_STEPS_S2)
    );
    delayMicroseconds(intervalUs);
  }

  if (axis == 'Y') {
    disableYValveMotor();
  } else {
    disableXValveMotor();
  }

  Serial.println(F("OK"));
}
