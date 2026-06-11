const int relayPin = 8;
const bool RELAY_ACTIVE_LOW = true;

bool uvRunning = false;
unsigned long uvOffAtMs = 0;
String inputLine = "";

void setUv(bool on) {
  bool level = on;
  if (RELAY_ACTIVE_LOW) {
    level = !level;
  }
  digitalWrite(relayPin, level ? HIGH : LOW);
  uvRunning = on;
}

void printHelp() {
  Serial.println("UV relay controller ready.");
  Serial.println("Commands:");
  Serial.println("  start 10   turn UV on for 10 seconds");
  Serial.println("  start10    same as start 10");
  Serial.println("  stop       turn UV off immediately");
  Serial.println("  status     show current state");
}

float parseStartSeconds(String command) {
  command.trim();
  command.toLowerCase();

  if (!command.startsWith("start")) {
    return -1.0;
  }

  String valueText = command.substring(5);
  valueText.trim();
  if (valueText.length() == 0) {
    return -1.0;
  }

  return valueText.toFloat();
}

void printStatus() {
  Serial.print("UV ");
  Serial.println(uvRunning ? "ON" : "OFF");
}

void handleCommand(String command) {
  command.trim();
  if (command.length() == 0) {
    return;
  }

  String lower = command;
  lower.toLowerCase();

  if (lower == "stop" || lower == "off") {
    setUv(false);
    uvOffAtMs = 0;
    Serial.println("OK: UV OFF");
    return;
  }

  if (lower == "status") {
    printStatus();
    return;
  }

  if (lower == "help" || lower == "?") {
    printHelp();
    return;
  }

  float seconds = parseStartSeconds(command);
  if (seconds > 0.0) {
    unsigned long durationMs = (unsigned long)(seconds * 1000.0);
    setUv(true);
    uvOffAtMs = millis() + durationMs;
    Serial.print("OK: UV ON for ");
    Serial.print(seconds, 3);
    Serial.println(" seconds");
    return;
  }

  Serial.println("ERROR: Use start <seconds>, stop, status, or help.");
}

void pollSerial() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      handleCommand(inputLine);
      inputLine = "";
      continue;
    }

    if (inputLine.length() < 40) {
      inputLine += ch;
    } else {
      inputLine = "";
      Serial.println("ERROR: Command too long.");
    }
  }
}

void setup() {
  pinMode(relayPin, OUTPUT);
  setUv(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("UV default OFF.");
  printHelp();
}

void loop() {
  pollSerial();

  if (uvRunning && uvOffAtMs != 0 && (long)(millis() - uvOffAtMs) >= 0) {
    setUv(false);
    uvOffAtMs = 0;
    Serial.println("OK: UV timed OFF");
  }
}
