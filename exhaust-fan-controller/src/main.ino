#include <Arduino.h>
#include <TM1637Display.h>

// Constants

#define PIN_BUTTON_VIEW_DUTY    0    // physical pin 13
#define PIN_POTENTIOMETER_SPEED 1 // physical pin 12

#define PIN_DISPLAY_CLK         4 // physical pin 9
#define PIN_DISPLAY_DIO         5 // physical pin 8

#define PIN_FAN_PWM             PA6 // physical pin 7
#define PIN_FAN_FG_SIGNAL       PB2 // physical pin 5

#define MAX_FAN_RPM             4000 //Alveo3D BLHP-H24 Fan

#define ERROR_UNKNOWN           0
#define ERROR_SURPASSED_MAX_RPM 1

TM1637Display display(PIN_DISPLAY_CLK, PIN_DISPLAY_DIO);


// Variables

// Determines whether an erroneous state has been reached to shut down the system
bool errorState = false;
// Determines the error
int errorCode = 0;

// Current duty cycle for PWM signal
float dutyCycle = 0.0;
// Amount of potentiometer measurements that shall be averaged
float dutyMeasurementsPerAverage = 4;
// The minimum value that the new duty cycle has to differ from the previous one.
float dutyThreshold = 0.0045;

// RPM of fan
uint32_t RPM = 0;
// RPM averaging to reach 10RPM accuracy while still updating RPM every 1s
uint32_t avgRPMArr[] = {0, 0, 0, 0, 0, 0};
// Element of RPM averaging array to be updated next
uint8_t nextRPMAvgSpot = 0;
// Frequency of FG signal
uint32_t FGFreq = 0;
// Timer variable to be able to determin FG signal frequency
uint32_t FGTimer = 0;

// Boolean determining if RPM or duty cycle shall be shown on the display
bool showRPM = false;
// Array for displaying duty cycle without flicker and a "P" for percent at the end
uint8_t dutyCycleDisplayText[] = {0x00, 0x00, 0x00, 0b01110011};




// PWM control signal

// Initializes timer 1 on pin PA6 (physical pin 7) for 25kHz fast PWM mode
void setupPWM(uint16_t top, float initial_duty) {
  float duty = initial_duty;

  pinMode(PIN_FAN_PWM, OUTPUT);
  TCCR1A = _BV(COM1A1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);

  ICR1 = top;
  if (initial_duty < 0) duty = 0;
  if (initial_duty > 1) duty = 1;
  OCR1A = (uint16_t) (ICR1 + 1)*duty;
}

// Sets PWM duty cycle on pin PA6
void setPWMDuty(float duty) {
  float d = duty;
  if (duty < 0) d = 0;
  if (duty > 1) d = 1;
  OCR1A = (uint16_t) (ICR1 + 1)*d;
}

// Utilities

// Can detect a rising edge on any of the pins of the ATTiny44A
uint8_t prevStates[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
bool detectRisingEdge(uint8_t pin) {
  uint8_t current = digitalRead(pin);
  bool ret = prevStates[pin] != current && current == HIGH;
  prevStates[pin] = current;
  return ret;
}


// Setup for rising edge detection
void setupRisingEdgeDetection() {
  for (int i = 0; i < 12; i++) {
    prevStates[i] = digitalRead(i);
  }
}

// Get duty cycle (0-1) from Potentiometer
float getDutyCycleFromPotentiometer(uint8_t analogPin) {
  return ((float)analogRead(analogPin))/1015; // Dividing by 1015 instead of 1023 to ensure that 100% can actually be reached.
}

// FG signal and RPM evaluation

void measureRPM() {
  avgRPMArr[nextRPMAvgSpot] = 30*FGFreq;
  if (avgRPMArr[nextRPMAvgSpot] > MAX_FAN_RPM) {
    errorState = true;
    errorCode = ERROR_SURPASSED_MAX_RPM;
  }
  nextRPMAvgSpot = (nextRPMAvgSpot+1)%(sizeof(avgRPMArr)/sizeof(avgRPMArr[0]));
}

uint32_t calcAvgRPM() {
  uint32_t avg = 0;
    for (uint8_t i = 0; i < sizeof(avgRPMArr)/sizeof(avgRPMArr[0]); i++) {
      avg += avgRPMArr[i];
    }
    return avg/(sizeof(avgRPMArr)/sizeof(avgRPMArr[0]));
}

// Interrupts

// Count FG signal rising edges for determining signal frequency and thus RPM
void handleFGRising() {
  FGFreq++;
}


void setup() {
  // Setup pins and timer
  pinMode(PIN_BUTTON_VIEW_DUTY, INPUT);
  pinMode(PIN_POTENTIOMETER_SPEED, INPUT);
  pinMode(PIN_FAN_FG_SIGNAL, INPUT);
  setupPWM(319, 0);

  pinMode(11, INPUT_PULLUP); // Set reset pin to pullup to allow for long press to reset the microcontroller

  // Setup interrupt for determining frequency of FG signal
  pinMode(PIN_FAN_FG_SIGNAL, INPUT_PULLUP);
  attachInterrupt(0, handleFGRising, RISING);

  // Setup display
  display.setBrightness(5);
  display.clear();
}



void loop() {
  // Shutdown if erroneous state has been reached
  if (errorState) {
    setPWMDuty(0);
    uint8_t data[] = {0b01111001, 0b01110111, 0b01110111, display.encodeDigit(errorCode)};
    display.setSegments(data);
    return;
  }

  // Duty cycle

  // Amount of duty cycle measurements completed for the purpose of averaging
  static float dutyCount = 0;
  // Variable to calculate duty count average
  static float d = 0;


  // Set duty cycle

  if (dutyCount < dutyMeasurementsPerAverage) {
    d += getDutyCycleFromPotentiometer(PIN_POTENTIOMETER_SPEED);
    dutyCount++;
  } else {   // Desired amount of measurements reached
    // New average value of duty cycle
    float newDuty = d/dutyMeasurementsPerAverage;
    // Ensures that the duty cycle is only altered when it differs significantly (more than dutyKeepBelow) from the previous value to decrease physical load on the fan and maximize its lifetime.
    if (!(newDuty - dutyThreshold <= dutyCycle && dutyCycle <= newDuty + dutyThreshold)) {
      dutyCycle = d/dutyMeasurementsPerAverage;
      setPWMDuty(dutyCycle);
    }
    // Reset temporary variables for averaging
    dutyCount = 0;
    d = 0;
  }

  // RPM

  // Read RPM from FG signal
  if (FGTimer == 0) {
    FGTimer = millis();
  }
  // Resetting RPM and FGTimer in the unlikely case that millis() overflows and starts at 0 again (after 49.71 days)
  if (FGTimer > millis()) {
    RPM = 0;
    FGTimer = 0;
    FGFreq = 0;
  }
  // Calculating RPM each second after evaluating FGFreq
  if ((millis() - FGTimer) >= 1000) {
    // Calculation of RPM using frequency FGFreq of FG signal which is a frequency modulated rectangular signal
    // Now the revolution period TS equals twice the period of the FG signal (TS = 2 P(FG) = 2/FGFreq = 60/N = 60/RPM)
    // Thus the RPM can be calculated by multiplying the signal frequency FGFreq by 30 (RPM = 30 * FGFreq)
    // As RPM values are evaluated periodically each second, we can only reach 30RPM accuracy. To allow for 10RPM accuracy, we conduct a running average calculation using the avgRPMArr array.
    measureRPM();
    // Reset frequency and timer
    FGFreq = 0;
    FGTimer = millis();
  }


  // Display

  // Toggle between showing RPM / duty cycle when button has been pressed.
  if (detectRisingEdge(PIN_BUTTON_VIEW_DUTY)) {
    showRPM = !showRPM;
    display.clear();
  }

  // Display information on a 7 segment 4 digit display using TM1637
  if (showRPM) {
    display.showNumberDec(calcAvgRPM());
  } else {
    int duty = dutyCycle*100;
    dutyCycleDisplayText[0] = display.encodeDigit((duty/100)%10);
    dutyCycleDisplayText[1] = display.encodeDigit((duty/10)%10);
    dutyCycleDisplayText[2] = display.encodeDigit((duty)%10);
    display.setSegments(dutyCycleDisplayText);
  }
}