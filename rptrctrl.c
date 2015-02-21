/* rptrctrl.c - A program to do a 'minimalist' repeater controller
 * using a Raspberry PI and simple interfacing circuitry on the
 * GPIO Port.
 *
 * This code was converted from an Arduino sketch and is only
 * intended for testing. Standard Arduino sketch functions
 * (digitalWrite, digitalRead, etc.) were replaced with
 * functions abstracting BCM2835 GPIO Library calls. These
 * are pretty close to 1:1 mapped. (Yes, I realize there is
 * a wiringPI lib).
 *
 * This implementation does not afford a separate input for a
 * tone decoder output, hence the term, 'minimalist'. To provide
 * tone access control, the receiver must have tone decoding
 * built in and the COR output must AND with this.
 *
 * The COR input and PTT output pins on the Rasberry PI GPIO port
 * are specified by defines. These should be changed to match your
 * hardware configuration/implementation.
 *
 * Currently ID audio is intended to be generated by pulsing a
 * GPIO pin which should control an off board tone generator.
 * Onboard tone generation and WAV based VOICE ID in the next version!
 *
 * Default values for the ID timer (600 Seconds - 10 minutes) and
 * the squelch tail timer (1 second) are specified by defines.
 * The runtime values of these parameters are stored in variables
 * and could be changed programatically, if desired (e.g. via the
 * serial port). Of course, you'd have to write that code.
 *
 * The ID Time out timer is implemented using the C time library and
 * is based on the elapsed time counter (unsigned long int seconds)
 * so timeout values are restricted to integer values greater than
 * one second. The squelch tail timer is implemented the same way,
 * so it has the same restrictions.
 *
 * (C) 2013 KB4OID Labs - A division of Kodetroll Heavy Industries
 *
 * All rights reserved, but otherwise free to use for personal use.
 * No warranty expressed or implied.
 * This code is for educational or personal use only.
 *
 * File: rptrctrl.c
 * Date: April 22, 2013
 * Author: KB4OID/Kodetroll
 * Version: 0.85
 *
 * Build using: 'gcc -o rptrctrl rptrctrl.c  -l bcm2835 -lrt'
 *  - or -
 * just type 'make'
 *
 * NOTE: This application must be run as root to have permissions to
 * modify the GPIO pins.
 *
 * The bcm2835 gpio library from Mike McCauley is required to build this
 * application. It can be found here:
 * http://www.airspayce.com/mikem/bcm2835/bcm2835-1.38.tar.gz
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <bcm2835.h>

#define DEBUG 0
#define DEBUG_BEEP 0
#define DEBUG_TONE 0

#define SIZE_OF_INT 4

// Blinks on RPi pin GPIO 11
#define PIN RPI_GPIO_P1_11

//#include "pitches.h"

// Here we define the starting values of the ID and Squelch Tail
// Timers
#define DEFAULT_ID_TIMER 600       // In Seconds
#define DEFAULT_SQ_TIMER 1         // In Seconds

// other misc timer values
#define ID_PTT_DELAY  200       // in mS
#define ID_PTT_HANG   500       // in mS
#define CW_MIN_DELAY  30        // in mS
#define COR_DEBOUNCE_DELAY  50  // in mS

#define OFF LOW
#define ON HIGH

// These values define what the DIO input pin state is when this
// input is active or not. These two cases are inverses of each
// other, comment out the following define to make COR follow
// NEGATIVE logic

//#define COR_POSITIVE
#ifdef COR_POSITIVE
  #define COR_ON   HIGH    // DIO Pin state when COR is active
  #define COR_OFF  LOW     // DIO Pin state when COR is not active
#else
  #define COR_ON   LOW     // DIO Pin state when COR is active
  #define COR_OFF  HIGH    // DIO Pin state when COR is not active
#endif

// These values define what the DIO output pin state is when this
// output is active or not. These two cases are inverses of each
// other, comment out the following define to make PTT follow
// NEGATIVE logic

#define PTT_POSITIVE
#ifdef PTT_POSITIVE
  #define PTT_ON   HIGH    // DIO Pin state when PTT is active
  #define PTT_OFF  LOW     // DIO Pin state when PTT is not active
#else
  #define PTT_ON   LOW     // DIO Pin state when PTT is active
  #define PTT_OFF  HIGH    // DIO Pin state when PTT is not active
#endif

// Master enum of state machine states
enum CtrlStates {
  CS_START,
  CS_IDLE,
  CS_DEBOUNCE_COR_ON,
  CS_PTT_ON,
  CS_PTT,
  CS_DEBOUNCE_COR_OFF,
  CS_SQT_ON,
  CS_SQT_BEEP,
  CS_SQT,
  CS_SQT_OFF,
  CS_PTT_OFF,
  CS_ID
};

enum BeepTypes {
  CBEEP_NONE,
  CBEEP_SINGLE,
  CBEEP_DEDOOP,
  CBEEP_DODEEP,
  CBEEP_DEDEEP
};

// This is where we define what DIO PINs map to what functions
int PTT = 17;		// DIO Pin number for the PTT out - 9
int COR = 18;		// DIO Pin number for the COR in - 12
int CORLED = 22;	// DIO Pin number for the COR indicator LED - 11
int ID_PIN = 21;	// DIO Pin for the ID Audio output tone

// 17.21.22
// This is where the callsign is mapped in dah/dit/spaces
// e.g. N0S would be 3,1,0,3,3,3,3,3,0,3,3,3,0
// Put your call here, then count the number of elements and set
// NumElements below
int Elements[] = {
  3,1,3,0,3,1,1,1,0,1,1,1,1,3,0,3,3,3,0,1,1,0,3,1,1,0,3,3,1,1,3,3,0,1,3,1
};

// Here's where we define some of the CW ID characteristics
int NumElements = 36;     // This is the number of elements in the ID
int ID_tone = 1200;       // Audio frequency of CW ID
int BEEP_tone1 = 1000;    // Audio frequency of Courtesy Beep 1
int BEEP_tone2 = 800;     // Audio frequency of Courtesy Beep 2
int BeepDuration = 2;     // Courtesy Tone length (in CWID increments)
int CW_TIMEBASE = 50;     // CW ID Speed (This is a delay in mS)
// (50 is about 20wpm)

#define CBEEP_TYPE  CBEEP_SINGLE

// Timer definitions
time_t ticks;            // Current elapsed time in seconds
time_t IDTimer;          // next expire time for ID timer
time_t SQTimer;          // next expire time for Squelch Tail timer

// timer reset value definitions
int SQTimerValue;        // Squelch Tail interval time - in Seconds
int IDTimerValue;        // ID Timer interval time - in Seconds

// rptr run states
int nextState = 0;  // next state
int rptrState = 0;  // current state
int prevState = 0;  // previous state

// various DIO pin states
int CORValue;  // current COR value (was bool)
int pCORValue; // previous COR value (was bool)
int PTTValue;  // current PTT state (was bool)

int Need_ID;   // Whether on not we need to ID (was bool)

//~~~~~~ abstraction of arduino dio commands
#define OUTPUT 1
#define INPUT 0
//#define HIGH 1
//#define LOW 0

time_t now(void)
{

  time_t timer;

  timer = time(NULL);

  return(timer);
}

void pinMode(int pin,int value)
{
  // Set the pin to be an output
  if (value == OUTPUT)
  {
    bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_OUTP);
    bcm2835_gpio_set_pud(pin, BCM2835_GPIO_PUD_OFF);
  }
  else
  {
    bcm2835_gpio_fsel(pin, BCM2835_GPIO_FSEL_INPT);
    bcm2835_gpio_set_pud(pin, BCM2835_GPIO_PUD_UP);
  }

  if (DEBUG)
  {
    if (value == OUTPUT)
      printf("PM: 0x%02x: 0x%02x [OUTPUT]\n",pin,value);
    else
      printf("PM: 0x%02x: 0x%02x [INPUT]\n",pin,value);
  }
}

void digitalWrite(int pin,int value)
{
  if (DEBUG)
    printf("DW: 0x%02x: 0x%02x\n",pin,value);

  bcm2835_gpio_write(pin, value);
}

int digitalRead(int pin)
{
  int value = 0;
  value = bcm2835_gpio_lev(pin);
  if (DEBUG)
    printf("DR: 0x%02x: 0x%02x\n",pin,value);
  return(value);
}

void tone(int pin, int freq, int duration)
{
  digitalWrite(pin, ON);
  if (DEBUG_TONE)
    printf("tone: %d, %d, %d\n",pin, freq, duration);
}

void noTone(int pin)
{
  digitalWrite(pin, OFF);
  if (DEBUG_TONE)
    printf("nTone: %d\n",pin);
}

/* This function will reset the ID Timer by adding the
 * timer interval value to the current elapsed time
 */
void reset_id_timer() {
  IDTimer = ticks + IDTimerValue;
}

void beep(int freq, int duration) {

  if (DEBUG_BEEP)
    printf("Beep: %d, %d\n",freq,duration);
  // Start Playing the beep
  tone(ID_PIN,freq,duration);

  // Wait for the note to end
  delay(duration);

  // stop playing the beep
  noTone(ID_PIN);
  if (DEBUG_BEEP)
    printf("Beep Done!\n");
}

/* This function will play the courtesy beep. Blocking call */
void do_cbeep(void) {

  // wait 200 mS
  delay(ID_PTT_DELAY);

  // Calculate the Courtesy Tone duration
  int BeepDelay = BeepDuration * CW_TIMEBASE;

  switch(CBEEP_TYPE)
  {
    case CBEEP_NONE:
      break;

    case CBEEP_DEDOOP:
      beep(BEEP_tone1,BeepDelay*2);
      delay(BeepDelay);
      beep(BEEP_tone2,BeepDelay);
      break;

    case CBEEP_DODEEP:
      beep(BEEP_tone2,BeepDelay*2);
      delay(BeepDelay);
      beep(BEEP_tone1,BeepDelay);
      break;

    case CBEEP_DEDEEP:
      beep(BEEP_tone1,BeepDelay);
      delay(BeepDelay);
      beep(BEEP_tone1,BeepDelay);
      break;

    case CBEEP_SINGLE:
    default:
      beep(BEEP_tone1,BeepDelay);
      break;
  }

  // A little delay never hurts
  delay(CW_MIN_DELAY);

}

/* this function will play the CW ID *BLOCKING CALL* */
void do_ID() {
  int Element = 0;

  // exit if we do not need to ID yet
  if (!Need_ID)
    return;

  // We turn on the PTT output
  PTTValue = PTT_ON;
  digitalWrite(PTT, PTTValue);

  // wait 200 mS
  delay(ID_PTT_DELAY);

  // calculate the length of time to wait for the ID tone
  // to quit playing.
  int InterElementDelay = CW_TIMEBASE * 1.3;

  if (DEBUG)
    printf("NumElements: %d\n",NumElements);

  // We Play the ID elements
  for (Element = 0; Element < NumElements; Element++) {
    if (DEBUG)
      printf("Element: %d, Elements[%d]: %d\n",Element,Element,Elements[Element]);
    if (Elements[Element] != 0) {
      tone(ID_PIN,ID_tone,Elements[Element] * CW_TIMEBASE);
      delay(Elements[Element] * InterElementDelay);
      noTone(ID_PIN);
    }
    else
      delay(InterElementDelay);

    // add a little extra inter element delay
    delay(CW_MIN_DELAY);
  }

  // wait 200 mS
  delay(ID_PTT_DELAY);

  // do courtesy beep
  do_cbeep();

  // we give a little PTT hang time
  delay(ID_PTT_HANG);

  // Turn off the PTT
  PTTValue = PTT_OFF;
  digitalWrite(PTT, PTTValue);

  // reset the ID timer
  reset_id_timer();
  // turn off need id
  Need_ID = LOW;
}

/* This function will print current repeater operating states
 * to the serial port. For debuggin purposes only.
 */
void show_state_info() {
  printf ("t: %d:state:%d,%d,%d:C:%d,%d:P:%d\n",now(),prevState, rptrState,nextState,CORValue,pCORValue,PTTValue);
}

/* Startup info */
void Show_Start_Info(void)
{
  printf("Start Time:%d\n",now());
  printf("ID_Tone:%d\n",ID_tone);
  printf("Beep_Tone1:%d\n",BEEP_tone1);
  printf("Beep_Tone2:%d\n",BEEP_tone2);
  printf("NumElements:%d\n",NumElements);
}

/* One time startup init loop */
void setup() {

  // Determine the size of the Elements array
  NumElements = sizeof(Elements)/SIZE_OF_INT;

  // Get a current tick timer value
  ticks = now();

  // initialize the timers
  SQTimerValue = DEFAULT_SQ_TIMER;
  IDTimerValue = DEFAULT_ID_TIMER;

  // incase any setup code needs to know what state we are in
  rptrState = CS_START;

  // setup the DIO pins for the right modes
  pinMode(PTT, OUTPUT);
  pinMode(COR, INPUT);
  pinMode(CORLED, OUTPUT);

  // make sure we start with PTT off
  digitalWrite(PTT, PTT_OFF);

  // Get current values for COR
  CORValue = digitalRead(COR);
  pCORValue = CORValue;

  // Here is the first state we jump to
  rptrState = CS_IDLE;

  Show_Start_Info();

  // make sure we ID at startup.
  Need_ID = HIGH;
}

void get_cor() {
  // Read the COR input and store it in a global
  CORValue = digitalRead(COR);
  // lite the external COR indicator LED
  if (CORValue == COR_ON)
    digitalWrite(CORLED,HIGH);
  else
    digitalWrite(CORLED,LOW);
}

void show_msg(char * buf) {

  printf ("[%d] %s\n",now(),buf);
}

void loop1() {

  // grab the current elapsed time
  ticks = now();

  // grab the current COR value
  get_cor();

  printf("CORValue[%d]: %d\n",ticks,CORValue);

//  if (CORValue == COR_ON) {
//    printf("COR ON\n");
//  else
//    printf("COR OFF\n");

}

void loop() {

  // grab the current elapsed time
  ticks = now();

  // grab the current COR value
  get_cor();

  // execute the state machibe
  switch(rptrState)
  {
    case CS_START:
      // do nothing
      show_msg("START");
      rptrState = CS_IDLE;

      break;

    case CS_IDLE:
      // wait for COR to activate, then jump to debounce
      if (rptrState != prevState)
        show_msg("IDLE");
	
      prevState = rptrState;
      if (CORValue == COR_ON) {
        pCORValue = CORValue;
        rptrState = CS_DEBOUNCE_COR_ON;
      }

      // look for ID timer expiry
      if (ticks > IDTimer && Need_ID)
        rptrState = CS_ID;

      break;

    case CS_DEBOUNCE_COR_ON:
      prevState = rptrState;
      // ideally we will delay here a little while and test
      // the current value (after the delay) with the pCORValue
      // to prove its not a flake
      delay(COR_DEBOUNCE_DELAY);
      if ( pCORValue != digitalRead(COR)) {
        rptrState = CS_IDLE;  // FLAKE - bail back to IDLE
      } else {
        nextState = CS_PTT;    // where we will go after PTT_ON
        rptrState = CS_PTT_ON;  // good COR - PTT ON
        show_msg("COR ON");
      }
      break;

    case CS_PTT_ON:
      prevState = rptrState;
      // turn on PTT
      PTTValue = PTT_ON;
      digitalWrite(PTT, PTTValue);
      // jump to the desired next state (set by the previous state)
      rptrState = nextState;
      show_msg("PTT ON");
      break;

    case CS_PTT:
      // we stay in this state and wait for COR to DROP (de-activate),
      // then jump to debounce
      prevState = rptrState;
      if (CORValue != COR_ON)
        rptrState = CS_DEBOUNCE_COR_OFF;
      break;

    case CS_DEBOUNCE_COR_OFF:
      // ideally we will delay here a little while and test
      // the result with the pCORValue to prove its not a flake
      prevState = rptrState;
      delay(COR_DEBOUNCE_DELAY);
      if ( CORValue != digitalRead(COR))
        rptrState = CS_PTT;  // FLAKE - ignore
      else
        rptrState = CS_SQT_ON;  // COR dropped, go to sqt
        show_msg("COR OFF");
      break;

    case CS_SQT_ON:
      // set SQTimer active
      SQTimer = ticks + SQTimerValue;
      // jump to next state
      prevState = rptrState;
      rptrState = CS_SQT_BEEP;
      show_msg("SQT ON");
      break;

    case CS_SQT_BEEP:
      // Do the courtesy beep
      do_cbeep();
      // jump to CS_SQT to wait for SQT timer
      prevState = rptrState;
      rptrState = CS_SQT;
      show_msg("BEEP");
      break;

    case CS_SQT:
      // We stay in this state until SQTimer expires, then
      // we jump to to CS_SQT_OFF
      prevState = rptrState;
      if (ticks > SQTimer)
        rptrState = CS_SQT_OFF;
      if (CORValue == COR_ON) {
        pCORValue = CORValue;
        rptrState = CS_DEBOUNCE_COR_ON;
      }
      break;

    case CS_SQT_OFF:
      // set SQTail not active
      prevState = rptrState;
      nextState = CS_IDLE;
      rptrState = CS_PTT_OFF;
      // We just got done transmitting, so we need
      // to ID next time the ID timer expires
      Need_ID = HIGH;
      show_msg("SQT OFF");

      break;

    case CS_PTT_OFF:
      // Turn the PTT off
      PTTValue = PTT_OFF;
      digitalWrite(PTT, PTTValue);
      // jump to the desired next state (set by the previous state)
      prevState = rptrState;
      rptrState = nextState;
      show_msg("PTT OFF");

      break;

    case CS_ID:
      show_msg("ID");

      // Go do the ID (this is a *blocking* call)
      do_ID();
      // back to the IDLE state when
      prevState = rptrState;
      rptrState = CS_IDLE;
      // we have satisfied our need to ID, so NO
      Need_ID = LOW;
      show_msg("ID DONE");

      break;

    default:
      // do nothing
      break;
  }

  // Comment this out to stop reporting this info
  //show_state_info();

  // capture the current machine state and COR value and
  // save as 'previous' for the next loop.
  pCORValue = CORValue;

}


int main(int argc, char **argv)
{
  // If you call this, it will not actually access the GPIO
  // Use for testing
//    bcm2835_set_debug(1);

  if (!bcm2835_init())
      return 1;

  // This is normally called on startup by the Arduino bootloader,
  // so we have to do it here.
  setup();	

  // This is the normal operating mode of an Arduino, again we 
  // have to provide this functionality
  while(1)
  {
    loop();
  }
}

