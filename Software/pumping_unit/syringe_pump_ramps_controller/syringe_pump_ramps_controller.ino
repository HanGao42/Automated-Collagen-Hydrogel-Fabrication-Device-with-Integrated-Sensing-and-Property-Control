#include <AccelStepper.h>

// RAMPS 1.4 / 1.6 pins on Arduino Mega.
#define X_STEP_PIN    54
#define X_DIR_PIN     55
#define X_ENABLE_PIN  38

#define Y_STEP_PIN    60
#define Y_DIR_PIN     61
#define Y_ENABLE_PIN  56

#define Z_STEP_PIN    46
#define Z_DIR_PIN     48
#define Z_ENABLE_PIN  62

// E1 driver socket on RAMPS. E0 is not used here.
#define E1_STEP_PIN    36
#define E1_DIR_PIN     34
#define E1_ENABLE_PIN  30

AccelStepper xMotor(AccelStepper::DRIVER, X_STEP_PIN, X_DIR_PIN);
AccelStepper yMotor(AccelStepper::DRIVER, Y_STEP_PIN, Y_DIR_PIN);
AccelStepper zMotor(AccelStepper::DRIVER, Z_STEP_PIN, Z_DIR_PIN);
AccelStepper e1Motor(AccelStepper::DRIVER, E1_STEP_PIN, E1_DIR_PIN);

// ===== Mechanical settings =====
// Common 1.8 degree stepper motor: 200 full steps per revolution.
const float MOTOR_STEPS_PER_REV = 200.0;

// You said only MS2 jumper is installed.
// For TMC2208 modules in standalone mode, actual microstep behavior can vary
// by module wiring/configuration. Start with 16 and calibrate if distance is off.
const float MICROSTEPS = 16.0;

// Leadscrew travel per revolution, in mm.
// Example: T8 lead screw is often 8 mm/rev; M8 threaded rod is often about 1.25 mm/rev.
const float LEADSCREW_MM_PER_REV = 8.0;

const float STEPS_PER_MM =
  (MOTOR_STEPS_PER_REV * MICROSTEPS) / LEADSCREW_MM_PER_REV;

// Motion settings.
const float DEFAULT_MAX_SPEED_MM_S = 5.0;
const float DEFAULT_ACCEL_MM_S2 = 20.0;

// ===== Syringe calibration =====
// Small syringe: 43 mm travel pushes about 2 ml.
// Large syringe: 89 mm travel pushes about 10 ml.
const float SMALL_MM_PER_ML = 43.0 / 2.0;
const float LARGE_MM_PER_ML = 89.0 / 10.0;

struct MotorPort {
  const char *name;
  const char *commandPrefix;
  byte enablePin;
  AccelStepper *stepper;
};

MotorPort motors[] = {
  {"X", "X", X_ENABLE_PIN, &xMotor},
  {"Y", "Y", Y_ENABLE_PIN, &yMotor},
  {"Z", "Z", Z_ENABLE_PIN, &zMotor},
  {"E1", "E1", E1_ENABLE_PIN, &e1Motor},
};

const int MOTOR_COUNT = sizeof(motors) / sizeof(motors[0]);

void enableMotor(MotorPort *motor) {
  digitalWrite(motor->enablePin, LOW);   // RAMPS drivers are usually enabled by LOW.
}

void disableMotor(MotorPort *motor) {
  digitalWrite(motor->enablePin, HIGH);
}

void enableAllMotors() {
  for (int i = 0; i < MOTOR_COUNT; i++) {
    enableMotor(&motors[i]);
  }
}

void configureMotor(AccelStepper &motor) {
  motor.setMaxSpeed(DEFAULT_MAX_SPEED_MM_S * STEPS_PER_MM);
  motor.setAcceleration(DEFAULT_ACCEL_MM_S2 * STEPS_PER_MM);
  motor.setCurrentPosition(0);
}

MotorPort *findMotorByCommand(String &cmd, int &prefixLength) {
  if (cmd.startsWith("E1")) {
    prefixLength = 2;
    return &motors[3];
  }

  char motorCode = cmd.charAt(0);
  for (int i = 0; i < MOTOR_COUNT; i++) {
    if (motors[i].commandPrefix[0] == motorCode && motors[i].commandPrefix[1] == '\0') {
      prefixLength = 1;
      return &motors[i];
    }
  }

  prefixLength = 0;
  return NULL;
}

void moveMM(MotorPort *motor, float mm) {
  long steps = lround(mm * STEPS_PER_MM);
  motor->stepper->move(steps);
}

void moveML(MotorPort *motor, float ml, float mmPerML) {
  moveMM(motor, ml * mmPerML);
}

void printMoveML(MotorPort *motor, const char *syringeName, float ml, float mmPerML) {
  Serial.print(motor->name);
  Serial.print(" ");
  Serial.print(syringeName);
  Serial.print(" syringe move ");
  Serial.print(ml);
  Serial.print(" ml, travel ");
  Serial.print(ml * mmPerML);
  Serial.println(" mm");
}

void printHelp() {
  Serial.println();
  Serial.println("Syringe pump controller on RAMPS X/Y/Z/E1");
  Serial.println("Commands:");
  Serial.println("  XS2       X motor, small syringe, forward 2 ml");
  Serial.println("  YS-0.5    Y motor, small syringe, backward 0.5 ml");
  Serial.println("  ZL10      Z motor, large syringe, forward 10 ml");
  Serial.println("  E1L-1     E1 motor, large syringe, backward 1 ml");
  Serial.println("  Xm10      X motor calibration/debug: move forward 10 mm");
  Serial.println("  E1m-5     E1 motor calibration/debug: move backward 5 mm");
  Serial.println("  s2.5      set max speed for all motors to 2.5 mm/s");
  Serial.println("  a10       set acceleration for all motors to 10 mm/s^2");
  Serial.println("  enableX   enable one motor: X, Y, Z, or E1");
  Serial.println("  disableX  disable one motor: X, Y, Z, or E1");
  Serial.println("  zeroX     set one motor current position as zero");
  Serial.println("  ?         show this help");
  Serial.println();
}

void setAllMaxSpeed(float speedMMPerS) {
  for (int i = 0; i < MOTOR_COUNT; i++) {
    motors[i].stepper->setMaxSpeed(speedMMPerS * STEPS_PER_MM);
  }
}

void setAllAcceleration(float accelMMPerS2) {
  for (int i = 0; i < MOTOR_COUNT; i++) {
    motors[i].stepper->setAcceleration(accelMMPerS2 * STEPS_PER_MM);
  }
}

bool handleMotorUtilityCommand(String cmd, const char *prefix) {
  if (!cmd.startsWith(prefix)) {
    return false;
  }

  String motorText = cmd.substring(strlen(prefix));
  int prefixLength = 0;
  MotorPort *motor = findMotorByCommand(motorText, prefixLength);
  if (motor == NULL || prefixLength != motorText.length()) {
    Serial.println("Unknown motor. Use X, Y, Z, or E1.");
    return true;
  }

  if (strcmp(prefix, "enable") == 0) {
    enableMotor(motor);
    Serial.print(motor->name);
    Serial.println(" enabled.");
  } else if (strcmp(prefix, "disable") == 0) {
    disableMotor(motor);
    Serial.print(motor->name);
    Serial.println(" disabled.");
  } else if (strcmp(prefix, "zero") == 0) {
    motor->stepper->setCurrentPosition(0);
    Serial.print(motor->name);
    Serial.println(" zeroed.");
  }

  return true;
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < MOTOR_COUNT; i++) {
    pinMode(motors[i].enablePin, OUTPUT);
    configureMotor(*motors[i].stepper);
  }

  enableAllMotors();
  printHelp();
}

void loop() {
  for (int i = 0; i < MOTOR_COUNT; i++) {
    motors[i].stepper->run();
  }

  if (!Serial.available()) {
    return;
  }

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.length() == 0) {
    return;
  }

  if (handleMotorUtilityCommand(cmd, "enable")) {
    return;
  }
  if (handleMotorUtilityCommand(cmd, "disable")) {
    return;
  }
  if (handleMotorUtilityCommand(cmd, "zero")) {
    return;
  }

  char command = cmd.charAt(0);
  float value = cmd.substring(1).toFloat();

  if (command == 's') {
    if (value > 0) {
      setAllMaxSpeed(value);
      Serial.print("Max speed set to ");
      Serial.print(value);
      Serial.println(" mm/s for all motors.");
    } else {
      Serial.println("Speed must be greater than 0.");
    }
    return;
  }

  if (command == 'a') {
    if (value > 0) {
      setAllAcceleration(value);
      Serial.print("Acceleration set to ");
      Serial.print(value);
      Serial.println(" mm/s^2 for all motors.");
    } else {
      Serial.println("Acceleration must be greater than 0.");
    }
    return;
  }

  if (command == '?') {
    printHelp();
    return;
  }

  int motorPrefixLength = 0;
  MotorPort *motor = findMotorByCommand(cmd, motorPrefixLength);
  if (motor == NULL) {
    Serial.println("Unknown command. Use X, Y, Z, or E1 before syringe command.");
    printHelp();
    return;
  }

  String payload = cmd.substring(motorPrefixLength);
  char syringeOrMode = payload.charAt(0);
  float mlOrMM = payload.substring(1).toFloat();

  enableMotor(motor);

  if (syringeOrMode == 'S') {
    moveML(motor, mlOrMM, SMALL_MM_PER_ML);
    printMoveML(motor, "Small", mlOrMM, SMALL_MM_PER_ML);
  } else if (syringeOrMode == 'L') {
    moveML(motor, mlOrMM, LARGE_MM_PER_ML);
    printMoveML(motor, "Large", mlOrMM, LARGE_MM_PER_ML);
  } else if (syringeOrMode == 'm') {
    moveMM(motor, mlOrMM);
    Serial.print(motor->name);
    Serial.print(" move ");
    Serial.print(mlOrMM);
    Serial.println(" mm");
  } else {
    Serial.println("Unknown syringe/mode. Use S, L, or m after motor name.");
  }
}
