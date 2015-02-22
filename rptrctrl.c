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
#include <string.h>
#include <getopt.h>
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

#define COR_POS_LOGIC 1
#define COR_NEG_LOGIC 0

//#define COR_POS_LOGIC
//#ifdef COR_POS_LOGIC
//  #define COR_ON   HIGH    // DIO Pin state when COR is active
//  #define COR_OFF  LOW     // DIO Pin state when COR is not active
//#else
//  #define COR_ON   LOW     // DIO Pin state when COR is active
//  #define COR_OFF  HIGH    // DIO Pin state when COR is not active
//#endif

// These values define what the DIO output pin state is when this
// output is active or not. These two cases are inverses of each
// other, comment out the following define to make PTT follow
// NEGATIVE logic

#define PTT_POS_LOGIC 1
#define PTT_NEG_LOGIC 0

//#define PTT_POS_LOGIC
//#ifdef PTT_POS_LOGIC
//  #define PTT_ON   HIGH    // DIO Pin state when PTT is active
//  #define PTT_OFF  LOW     // DIO Pin state when PTT is not active
//#else
//  #define PTT_ON   LOW     // DIO Pin state when PTT is active
//  #define PTT_OFF  HIGH    // DIO Pin state when PTT is not active
//#endif

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

// 17.21.22
// This is where we define what DIO PINs map to what functions
int PTT_PIN = 17;		// DIO Pin number for the PTT out - 17
int COR_PIN = 18;		// DIO Pin number for the COR in - 18
int COR_LED = 22;		// DIO Pin number for the undebounced COR indicator LED - 22
int ID_PIN = 21;		// DIO Pin for the ID Audio output tone
int PWM_PIN = 18;		// PWM Pin for the ID Audio output tone

// This is where the callsign is mapped in dah/dit/spaces
// e.g. N0S would be 3,1,0,3,3,3,3,3,0,3,3,3,0
// Put your call here, then count the number of elements and set
// NumElements below
int Elements[200];
//int Elements[] = {
//  3,1,3,0,3,1,1,1,0,1,1,1,1,3,0,3,3,3,0,1,1,0,3,1,1,0,3,3,1,1,3,3,0,1,3,1
//};

char Callsign[30];

#define DEFAULT_CALLSIGN "KB4OID"

// Here's where we define some of the CW ID characteristics
int NumElements = 0;     // This is the number of elements in the ID
int ID_tone = 1200;       // Audio frequency of CW ID
int BEEP_type = CBEEP_SINGLE;    // Courtesy Beep Type
int BEEP_tone1 = 1000;    // Audio frequency of Courtesy Beep 1
int BEEP_tone2 = 800;     // Audio frequency of Courtesy Beep 2
int BeepDuration = 2;     // Courtesy Tone length (in CWID increments)
int CW_TIMEBASE = 50;     // CW ID Speed (This is a delay in mS)
// (50 is about 20wpm)

//#define CBEEP_TYPE  CBEEP_SINGLE

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
int COR_Value;  // current COR value
int pCOR_Value; // previous COR value
int PTT_Value;  // current PTT state

// COR and PTT Logic sense
int COR_SENSE = COR_NEG_LOGIC;
int PTT_SENSE = PTT_POS_LOGIC;

// COR and PTT Logic sense states
int COR_ON;
int COR_OFF;
int PTT_ON;
int PTT_OFF;

int Need_ID;   // Whether on not we need to ID (was bool)

/* Flag set by ‘--verbose’. */
static int verbose_flag;

//~~~~~~ abstraction of arduino dio commands
#define OUTPUT 1
#define INPUT 0
//#define HIGH 1
//#define LOW 0

// This functions returns the current time in seconds from start
// of UNIX epoch
time_t now(void) {

	time_t timer;

	timer = time(NULL);

	return(timer);
}

// This function emulates the arduino pinMode function,
// setting the specified pin to the provided mode using
// the bcm2835 library
void pinMode(int pin,int value) {
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

// This function emulates the arduino digitalWrite
// function, setting the specified pin to the 
// provided value using the bcm2835 library
void digitalWrite(int pin,int value) {
	if (DEBUG)
		printf("DW: 0x%02x: 0x%02x\n",pin,value);

	bcm2835_gpio_write(pin, value);
}

// This function emulates the arduino digitalRead
// function, returning the value of the specified 
// pin using the bcm2835 library
int digitalRead(int pin) {
	int value = 0;
	value = bcm2835_gpio_lev(pin);
	if (DEBUG)
		printf("DR: 0x%02x: 0x%02x\n",pin,value);
	return(value);
}

// This function emulates the arduino analogWrite
// function, setting the specified PWM pin to the 
// provided value using the bcm2835 library
void analogWrite(int pin,int value) {
	// to be written
	
	if (DEBUG)
		printf("AW: 0x%02x: 0x%02x\n",pin,value);
}

/* This function will turn on the CW ID key
 * pin and start the PWM timer to enable tone
 * generation. 
 * Note: This is NOT a *Blocking call*
 */
void tone(int pin, int freq, int duration)	 {
	// Turn on ID Key
	digitalWrite(pin, ON);
	analogWrite(PWM_PIN,1023);
	if (DEBUG_TONE)
		printf("tone: %d, %d, %d\n",pin, freq, duration);
}

/* This function will turn off the CW ID key
 * pin and stops the PWM timer to disable tone
 * generation.
 * Note: This is NOT a *Blocking call*
 */
void noTone(int pin) {
	digitalWrite(pin, OFF);
	analogWrite(PWM_PIN,OFF);
	if (DEBUG_TONE)
		printf("noTone: %d\n",pin);
}

/* This function will reset the ID Timer by adding the
 * timer interval value to the current elapsed time
 */
void reset_id_timer() {
	IDTimer = ticks + IDTimerValue;
}


/* This function will generate a beep of the 
 * specified duration and frequency using PWM
 * (if enabled) and turn on the CW ID key
 * pin for the duration of the tone to enable 
 * and external tone generator.
 * Note: This is a *Blocking call*
 */
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

/* This function will play the courtesy beep. 
 * Note: This is a *Blocking call*
 */
void do_cbeep(int btype) {

	// wait 200 mS
	delay(ID_PTT_DELAY);

	// Calculate the Courtesy Tone duration
	int BeepDelay = BeepDuration * CW_TIMEBASE;

	switch(btype)
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

/* this function will play the CW ID, 
 * Note: This is a *BLOCKING CALL* 
 */
void do_ID() {
	int Element = 0;

	// exit if we do not need to ID yet
	if (!Need_ID)
		return;

	// We turn on the PTT output
	PTT_Value = PTT_ON;
	digitalWrite(PTT_PIN, PTT_Value);

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
	do_cbeep(BEEP_type);

	// we give a little PTT hang time
	delay(ID_PTT_HANG);

	// Turn off the PTT
	PTT_Value = PTT_OFF;
	digitalWrite(PTT_PIN, PTT_Value);

	// reset the ID timer
	reset_id_timer();

	// turn off need id
	Need_ID = LOW;
}

/* This function will print current repeater operating states
 * to the serial port. For debuggin purposes only.
 */
void show_state_info() {
	printf ("t: %d:state:%d,%d,%d:C:%d,%d:P:%d\n",now(),prevState, rptrState,nextState,COR_Value,pCOR_Value,PTT_Value);
}

/* Startup info */
void Show_Start_Info(void)
{
	int i;
	printf("Start Time: %d S\n",now());
	printf("ID_Tone: %d Hz\n",ID_tone);
	printf("Beep_Tone1: %d Hz\n",BEEP_tone1);
	printf("Beep_Tone2: %d Hz\n",BEEP_tone2);
	printf("CW ID Speed: %d mS\n",CW_TIMEBASE);
	printf("BeepDuration: %d mS\n",BeepDuration);
	printf("CallSign: '%s'\n",Callsign);
	printf("NumElements: %d\n",NumElements);
	printf("Elements: ");
	for (i=0;i<NumElements;i++) {
		printf("%d,",Elements[i]);
	}
	printf("\n");}

void setCOR_Sense(int Sense) {
	
	if (COR_SENSE == COR_POS_LOGIC) {
		COR_ON = HIGH;
		COR_OFF = LOW;
	} else {
		COR_ON = LOW;
		COR_OFF = HIGH;
	}
}	

void setPTT_Sense(int Sense) {
	if (PTT_SENSE == PTT_POS_LOGIC) {
		PTT_ON = HIGH;
		PTT_OFF = LOW;
	} else {
		PTT_ON = LOW;
		PTT_OFF = HIGH;
	}
}	

char* cvt2morse(char c) {
	switch(c)
	{
		case 'A':
		case 'a':
			return("130");
			break;
		case 'B':
		case 'b':
			return("31110");
			break;
		case 'C':
		case 'c':
			return("31310");
			break;
		case 'D':
		case 'd':
			return("3110");
			break;
		case 'E':
		case 'e':
			return("10");
			break;
		case 'F':
		case 'f':
			return("11310");
			break;
		case 'G':
		case 'g':
			return("3310");
			break;
		case 'H':
		case 'h':
			return("11110");
			break;
		case 'I':
		case 'i':
			return("110");
			break;
		case 'J':
		case 'j':
			return("13330");
			break;
		case 'K':
		case 'k':
			return("3130");
			break;
		case 'L':
		case 'l':
			return("13110");
			break;
		case 'M':
		case 'm':
			return("330");
			break;
		case 'N':
		case 'n':
			return("310");
			break;
		case 'O':
		case 'o':
			return("3330");
			break;
		case 'P':
		case 'p':
			return("13310");
			break;
		case 'Q':
		case 'q':
			return("33130");
			break;
		case 'R':
		case 'r':
			return("1310");
			break;
		case 'S':
		case 's':
			return("1110");
			break;
		case 'T':
		case 't':
			return("30");
			break;
		case 'U':
		case 'u':
			return("1130");
			break;
		case 'V':
		case 'v':
			return("11130");
			break;
		case 'W':
		case 'w':
			return("1330");
			break;
		case 'X':
		case 'x':
			return("31130");
			break;
		case 'Y':
		case 'y':
			return("31330");
			break;
		case 'Z':
		case 'z':
			return("33110");
			break;
		case '0':
			return("333330");
			break;
		case '1':
			return("133330");
			break;
		case '2':
			return("113330");
			break;
		case '3':
			return("111330");
			break;
		case '4':
			return("111130");
			break;
		case '5':
			return("111110");
			break;
		case '6':
			return("311110");
			break;
		case '7':
			return("331110");
			break;
		case '8':
			return("333110");
			break;
		case '9':
			return("333310");
			break;
		default:
			return("0");
			break;
	}
}

int ConvertCall(char * call) {
	int i;
	char l[200];
	memset(l,0x00,sizeof(l));
	
//	printf("call: '%s'\n",call);
	for (i=0;i<strlen(call);i++) {
		strcat(l,cvt2morse(call[i]));
	}
//	printf("l: '%s'\n",l);
//	printf("sizeof(Elements): %d\n",sizeof(Elements));
	for (i=0;i<sizeof(Elements)/SIZE_OF_INT;i++) {
		Elements[i] = 0;
	}
	for (i=0;i<strlen(l);i++) {
		Elements[i] = l[i]-0x30;
	}

	return(i);
}

/* One time startup init loop */
void setup() {
	
	setCOR_Sense(COR_SENSE);
	setPTT_Sense(PTT_SENSE);
	
	// Determine the size of the Elements array
//	NumElements = sizeof(Elements)/SIZE_OF_INT;
//	printf("NumElements: %d\n",NumElements);

//	printf("Callsign: '%s'\n",Callsign);
	
	NumElements = ConvertCall(Callsign);
//	printf("NumElements: %d\n",NumElements);

	// Get a current tick timer value
	ticks = now();

	// initialize the timers
	SQTimerValue = DEFAULT_SQ_TIMER;
	IDTimerValue = DEFAULT_ID_TIMER;

	// incase any setup code needs to know what state we are in
	rptrState = CS_START;

	// setup the DIO pins for the right modes
	pinMode(PTT_PIN, OUTPUT);
	pinMode(COR_PIN, INPUT);
	pinMode(COR_LED, OUTPUT);

	// make sure we start with PTT off
	digitalWrite(PTT_PIN, PTT_OFF);

	// Get current values for COR
	COR_Value = digitalRead(COR_PIN);
	pCOR_Value = COR_Value;

	// Here is the first state we jump to
	rptrState = CS_IDLE;

	Show_Start_Info();

	// make sure we ID at startup.
	Need_ID = HIGH;
}

void get_cor() {
	
	// Read the COR input and store it in a global
	COR_Value = digitalRead(COR_PIN);
	
	// lite the external COR indicator LED
	if (COR_Value == COR_ON)
		digitalWrite(COR_LED,HIGH);
	else
		digitalWrite(COR_LED,LOW);
}

void show_msg(char * buf) {

	printf ("[%d] %s\n",now(),buf);
}

void loop1() {

	// grab the current elapsed time
	ticks = now();

	// grab the current COR value
	get_cor();

	printf("COR_Value[%d]: %d\n",ticks,COR_Value);

//  if (COR_Value == COR_ON) {
//    printf("COR ON\n");
//  else
//    printf("COR OFF\n");

}

void loop() {

	// grab the current elapsed time
	ticks = now();

	// grab the current COR value
	get_cor();

	// execute the state machine
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
			if (COR_Value == COR_ON) {
				pCOR_Value = COR_Value;
				rptrState = CS_DEBOUNCE_COR_ON;
			}

			// look for ID timer expiry
			if ((ticks > IDTimer) && Need_ID)
				rptrState = CS_ID;

			break;

		case CS_DEBOUNCE_COR_ON:
			prevState = rptrState;
			// ideally we will delay here a little while and test
			// the current value (after the delay) with the pCOR_Value
			// to prove its not a flake
			delay(COR_DEBOUNCE_DELAY);
			if ( pCOR_Value != digitalRead(COR_PIN)) {
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
			PTT_Value = PTT_ON;
			digitalWrite(PTT_PIN, PTT_Value);
			// jump to the desired next state (set by the previous state)
			rptrState = nextState;
			show_msg("PTT ON");
			break;

		case CS_PTT:
			// we stay in this state and wait for COR to DROP (de-activate),
			// then jump to debounce
			prevState = rptrState;
			if (COR_Value != COR_ON)
				rptrState = CS_DEBOUNCE_COR_OFF;
			break;

		case CS_DEBOUNCE_COR_OFF:
			// ideally we will delay here a little while and test
			// the result with the pCOR_Value to prove its not a flake
			prevState = rptrState;
			delay(COR_DEBOUNCE_DELAY);
			if ( COR_Value != digitalRead(COR_PIN))
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
			do_cbeep(BEEP_type);
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
			if (COR_Value == COR_ON) {
				pCOR_Value = COR_Value;
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
			PTT_Value = PTT_OFF;
			digitalWrite(PTT_PIN, PTT_Value);
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
	pCOR_Value = COR_Value;

}

void LoadConfig(char * cfile) {
	
	printf("cfgFile: '%s'\n",cfile);
}

int ParseArgs(int argc, char **argv) {

	int c;

	while (1)
    {
		static struct option long_options[] =
        {
			/* These options set a flag. */
			{"verbose", no_argument,       &verbose_flag, 1},
			{"brief",   no_argument,       &verbose_flag, 0},
			/* These options don’t set a flag.
               We distinguish them by their indices. */
			{"add",     no_argument,       0, 'a'},
			{"append",  no_argument,       0, 'b'},
			{"delete",  required_argument, 0, 'd'},
			{"create",  required_argument, 0, 'c'},
			{"file",    required_argument, 0, 'f'},
			{0, 0, 0, 0}
		};
		/* getopt_long stores the option index here. */
		int option_index = 0;

		c = getopt_long (argc, argv, "abc:d:f:",
                       long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;

		switch (c)
        {
			case 0:
				/* If this option set a flag, do nothing else now. */
				if (long_options[option_index].flag != 0)
					break;
				printf ("option %s", long_options[option_index].name);
				if (optarg)
					printf (" with arg %s", optarg);
				printf ("\n");
				break;
			case 'a':
				puts ("option -a\n");
				break;

			case 'b':
				puts ("option -b\n");
				break;

			case 'c':
				printf ("option -c with value `%s'\n", optarg);
				break;

			case 'd':
				printf ("option -d with value `%s'\n", optarg);
				break;

			case 'f':
				printf ("option -f with value `%s'\n", optarg);
				break;

			case '?':
				/* getopt_long already printed an error message. */
				break;

			default:
				abort ();
		}
    }

	/* Instead of reporting ‘--verbose’
       and ‘--brief’ as they are encountered,
	   we report the final status resulting from them. */
	if (verbose_flag)
		puts ("verbose flag is set");

	/* Print any remaining command line arguments (not options). */
	if (optind < argc)
	{
		printf ("non-option ARGV-elements: ");
		while (optind < argc)
			printf ("%s ", argv[optind++]);
		putchar ('\n');
    }
}

int main(int argc, char **argv)
{
	char * cfgFile;
	
	strcpy(Callsign,DEFAULT_CALLSIGN);
	
	// Set starting points for the GPIO pins.
	COR_Value = COR_OFF;
	pCOR_Value = COR_Value;
	PTT_Value = PTT_OFF;
	ID_PIN = OFF;

	ParseArgs(argc,argv);
	LoadConfig(cfgFile);
	
	// If you call this, it will not actually access the GPIO
	// Use for testing
//	bcm2835_set_debug(1);

	
	// Initialize the bcm2835 library, if this fails,
	// then bail (exit).
	if (!bcm2835_init())
		return 1;

	// This is normally called on startup by the Arduino bootloader,
	// so we have to do it here.
	setup();	

	// This is the normal operating mode of an Arduino, again we 
	// have to provide this functionality. Note, this runs forever
	// we might add a stop feature at some time to allow the controller
	// to exit and restart.
	while(1)
	{
		loop();
	}
}
