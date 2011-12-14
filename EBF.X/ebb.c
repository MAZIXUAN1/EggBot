// Versions:
// 1.8 - 
// 1.8.1 5/19/10 - Only change is to recompile with Microchip USB Stack v2.7
// 1.8.2 5/31/10 - Only change is to change name in USB enumeration string to Ei Bot Board - using new PID for SchmalzHaus
// 1.9   6/11/10 - Added two commands:
//					SQ - Solenoid Query - returns 0 or 1 for down and up
//					ST - Solenoid Toggle - toggles state of the servo/solenoid
// 1.9.2 6/15/10 - Added commands:
//					SC,11 sets pen up speed
//					SC,12 sets pen down speed
//					SL - sets the current layer
//					QL - queries the current layer
//					SN - sets move (node) count
//					QN - Query node count
//					QB - Query Button command
// 1.9.3 6/16/10 - Replaced SN with CL (Clear Node) command
// 1.9.4 6/22/10 - Node Count now incremented on pauses (SM with zero step size) as well
// 1.9.5 7/2/10 - Node count no longer incrimented at all except for NI command
//					NI - Node count Incriment
//					ND - Node count Decriment
//					SN - Set Node count (with 8 byte variable)
//					BL - With latest bootloader, will jumpt to Boot Load mode
// 1.9.6 7/3/10 - Removed extra vectors below 0x1000 for easier merging of HEX files 
//					- use c018i_HID_BL.o now
// 2.0.0 9/9/10 - Add in
//					QC - Query Current - reads voltage of current adjustment pot
//						NOTE: This is NOT done the 'right way'. Instead, we set up the pin for 
//						analog input at boot, then when the QC comes in, we activate the ADC and
//						take one reading and then shut it down. Eventually, we should re-write the
//						'UBW' ADC routines to work with the much more flexible ADC in the 46J50 part
//						and then just use that generic code for reading the value of the pot.
//					SC,13,{0,1} - enables/disables RB0 as another PRG button for pause detection
// 2.0.1 9/13/10 - Bug fix - on v1.1 EBB hardware, need to disable RB0 alt pause button.
//					switched it to RB2 on v1.1 hardware
// 2.0.2 10/3/10 - Bug fix - QC command not returning proper results - added cast and now works OK
// 2.1.0 10/21/10- Added in
//					SE - Set Engraver - turns engraver (on RB3) on or off, or set to PWM power level
// 				   Added code in init to pre-charge RC7 (USB_SENSE_IO) high before running rest of code
//					to get around wrong resistor value on hardware.
// 2.1.1 11/21/10- Removed Microchip USB stack v2.7, replaced it with v2.8 from MAL 2010_10_19,
//					Also using generic Microchip folder now rather than re-named one (simpler to update)
//				   Updated code in main.c (and others) to match updates from latest MAL CDC example
// 2.1.1cTest1 01/17/11 - Added third paramter to SP command to use any PortB pin for servo output
//                 For this version only - used PortB2 as standard servo output
// 2.1.1d 02/11/11 - Reverted back to RB1 for servo output
//                 - Updated check_and_send_TX_data() to allow unlimited data to go out without overrunning
//                    the output buffer, same as UBW 1.4.7
// 2.1.2 11/04/11 - Fixed PI command to return just a 0 or a 1
//                - Updated to USB stack 2.9a
//                - Created MPLAB X project for this firmware
//                - Added SC,14,<state> to enable/disable solenoid output on RB4
//                - Fixed bug with S2 command and solenoid command interaction - we now turn off solenoid
//                      output on RB4 if user uses S2 command to use RB4 for RC servo output
//                - Fixed bug with S2 command where a duration of 0 would not shut off the PWM channel
//                - Fixed bug in S2 command where <rate> variable was not being used correctly
//                - Switched default number of S2 channels to 8 (from 7 before)
// 2.1.3 12/12/11 - RB3 now defaults to digital I/O on boot, can still use SE command to do PWM later if you want
//                - Compiled with latest UBW stack - 2.9b from MAL 2011-10-18
// 2.1.4 12/14/11 - RB3 now defaults to OFF, rather than ON, at boot.

#include <p18cxxx.h>
#include <usart.h>
#include <stdio.h>
#include <ctype.h>
#include <delays.h>
#include "Usb\usb.h"
#include "Usb\usb_function_cdc.h"
#include "usb_config.h"
#include "HardwareProfile.h"
#include "ubw.h"
#include "ebb.h"
#include "delays.h"
#include "ebb_demo.h"
/// TODO: Fix this based upon type of CPU
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
	#include "RCServo2.h"
#endif

// Reload value for TIMER1
// We need a 25KHz ISR to fire, so we take Fosc (48Mhz), devide by 4
// (normal CPU instruction rate of Fosc/4), then use the TIMER1 prescaler
// to divide by 4 again. Then we use a reload value of 120 to give us
// a rate of 48MHz/4/4/120 = 25KHz.
#define TIMER1_L_RELOAD (255 - 113)
#define TIMER1_H_RELOAD (255)

// This is the value that gets multiplied by Steps/Duration to compute
// the StepAdd values.
#define OVERFLOW_MUL	(0x8000 / 25)

#define MAX_RC_DURATION 11890

#if defined(BOARD_EBB_V10)
	#define DIR1_BIT	(0x80)
	#define STEP1_BIT	(0x40)
	#define DIR2_BIT	(0x20)
	#define STEP2_BIT	(0x10)
	#define DIR3_BIT	(0x08)
	#define STEP3_BIT	(0x04)
	#define DIR4_BIT	(0x02)
	#define STEP4_BIT	(0x01)
#elif defined(BOARD_EBB_V11)
	#define STEP1_BIT	(0x01)
	#define DIR1_BIT	(0x02)
	#define STEP2_BIT	(0x04)
	#define DIR2_BIT	(0x08)
#elif defined(BOARD_EBB_V12)
	#define STEP1_BIT	(0x01)
	#define DIR1_BIT	(0x02)
	#define STEP2_BIT	(0x04)
	#define DIR2_BIT	(0x08)
#elif defined(BOARD_EBB_V13_AND_ABOVE) 
/// TODO: Edit these
	#define STEP1_BIT	(0x01)
	#define DIR1_BIT	(0x02)
	#define STEP2_BIT	(0x04)
	#define DIR2_BIT	(0x08)
#elif defined(BOARD_UBW)
	#define DIR1_BIT	(0x02)
	#define STEP1_BIT	(0x01)
	#define DIR2_BIT	(0x08)
	#define STEP2_BIT	(0x04)
	#define DIR3_BIT	(0x20)
	#define STEP3_BIT	(0x10)
	#define DIR4_BIT	(0x80)
	#define STEP4_BIT	(0x40)
#endif

typedef enum
{
	COMMAND_NONE = 0,
	COMMAND_MOVE,
	COMMAND_DELAY,
	COMMAND_PEN_UP,
	COMMAND_PEN_DOWN
} CommandType;

typedef enum
{
	SOLENOID_OFF = 0,
	SOLENOID_ON,
	SOLENOID_PWM
} SolenoidStateType;

// LOCAL FUNCTIONS
static void process_SM(
	unsigned int Duration, 
	signed int A1Stp, 
	signed int A2Stp, 
	signed int A3Stp, 
	signed int A4Stp
);

#pragma udata access fast_vars
// Working registers
static near unsigned int StepAcc[4];
static near signed int StepAdd[4];
static near unsigned int StepsCounter[4];
static near unsigned char DirBits;
static near unsigned char OutByte;
static near unsigned char TookStep;
static near unsigned char AllDone;
static near unsigned char i;
near unsigned char NextReady;
static near CommandType Command;

#pragma udata
// ToLoad registers
static signed int ToLoadStepAdd[4];
static unsigned int ToLoadStepsCounter[4];
static unsigned char ToLoadDirBits;
static CommandType ToLoadCommand;
static unsigned short ToLoadDelayCounter;
static unsigned short DelayCounter;
unsigned int DemoModeActive;
unsigned int comd_counter;
static SolenoidStateType SolenoidState;
static unsigned int SolenoidDelay;
static unsigned char UseBuiltInDrivers;
static unsigned char UseServoForUpDown;
static unsigned int g_servo_max;
static unsigned int g_servo_min;
static PenStateType PenState;
static unsigned long NodeCount;
static char Layer;
static BOOL ButtonPushed;
static BOOL UseAltPause;
unsigned char QC_ms_timer;
static UINT StoredEngraverPower;
BOOL gUseSolenoid;

// ISR
#pragma interrupt high_ISR
void high_ISR(void)
{
	//Check which interrupt flag caused the interrupt.
	//Service the interrupt
	//Clear the interrupt flag
	//Etc.
	#if defined(USB_INTERRUPT)
		USBDeviceTasks();
	#endif

	if (PIR1bits.TMR1IF)
	{
		// Clear the interrupt 
		PIR1bits.TMR1IF = 0;
		TMR1L = TIMER1_L_RELOAD;	// Set to 120 for 25KHz ISR fire
		TMR1H = TIMER1_H_RELOAD;	//

		OutByte = DirBits;
		TookStep = FALSE;
		AllDone = TRUE;

		if (Command == COMMAND_DELAY)
		{
			if (DelayCounter)
			{
				DelayCounter--;
			}
			if (DelayCounter)
			{
				AllDone = FALSE;
			}
		}
		else if (Command == COMMAND_MOVE)
		{
			// Only output DIR bits if we are actually doing something
			if (StepsCounter[0] || StepsCounter[1] || StepsCounter[2] || StepsCounter[3])
			{
				// Always output direction bits early so they're ready when we step
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
				if (UseBuiltInDrivers)
				{
					PORTB = DirBits;
				}
				else
				{
					PORTC = DirBits;
				}
#elif defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
				if (UseBuiltInDrivers)
				{
					if (DirBits & DIR1_BIT)
					{
						Dir1IO = 1;
					}
					else
					{
						Dir1IO = 0;
					}	
					if (DirBits & DIR2_BIT)
					{
						Dir2IO = 1;
					}
					else
					{
						Dir2IO = 0;
					}	
				}
				else
				{
					if (DirBits & DIR1_BIT)
					{
						Dir1AltIO = 1;
					}
					else
					{
						Dir1AltIO = 0;
					}	
					if (DirBits & DIR2_BIT)
					{
						Dir2AltIO = 1;
					}
					else
					{
						Dir2AltIO = 0;
					}	
				}
#endif

				// Only do this if there are steps left to take
				if (StepsCounter[0])
				{
					StepAcc[0] = StepAcc[0] + StepAdd[0];
					if (StepAcc[0] > 0x8000)
					{
						StepAcc[0] = StepAcc[0] - 0x8000;
						OutByte = OutByte | STEP1_BIT;
						TookStep = TRUE;
						StepsCounter[0]--;
					}
					AllDone = FALSE;
				}
				if (StepsCounter[1])
				{
					StepAcc[1] = StepAcc[1] + StepAdd[1];
					if (StepAcc[1] > 0x8000)
					{
						StepAcc[1] = StepAcc[1] - 0x8000;
						OutByte = OutByte | STEP2_BIT;
						TookStep = TRUE;
						StepsCounter[1]--;
					}
					AllDone = FALSE;
				}
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
				if (StepsCounter[2])
				{
					StepAcc[2] = StepAcc[2] + StepAdd[2];
					if (StepAcc[2] > 0x8000)
					{
						StepAcc[2] = StepAcc[2] - 0x8000;
						OutByte = OutByte | STEP3_BIT;
						TookStep = TRUE;
						StepsCounter[2]--;
					}
					AllDone = FALSE;
				}
				if (StepsCounter[3])
				{
					StepAcc[3] = StepAcc[3] + StepAdd[3];
					if (StepAcc[3] > 0x8000)
					{
						StepAcc[3] = StepAcc[3] - 0x8000;
						OutByte = OutByte | STEP4_BIT;
						TookStep = TRUE;
						StepsCounter[3]--;
					}
					AllDone = FALSE;
				}
#endif	

				if (TookStep)
				{
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
					if (UseBuiltInDrivers)
					{
						PORTB = OutByte;
					}
					else
					{
						PORTC = OutByte;
					}
#elif defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
					if (UseBuiltInDrivers)
					{
						if (OutByte & STEP1_BIT)
						{
							Step1IO = 1;
						}
						if (OutByte & STEP2_BIT)
						{
							Step2IO = 1;
						}
					}
					else
					{
						if (OutByte & STEP1_BIT)
						{
							Step1AltIO = 1;
						}
						if (OutByte & STEP2_BIT)
						{
							Step2AltIO = 1;
						}
					}
#endif
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
					Delay1TCY();
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
					if (UseBuiltInDrivers)
					{
						PORTB = DirBits;
					}
					else
					{
						PORTC = DirBits;
					}
#elif defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
					if (UseBuiltInDrivers)
					{
						Step1IO = 0;
						Step2IO = 0;
					}
					else
					{
						Step1AltIO = 0;
						Step2AltIO = 0;
					}
#endif
				}
			}
		}
		// Check to see if we should change the state of the pen
		else if (Command == COMMAND_PEN_UP)
		{
			if (gUseRCServo1)
			{
				g_RC_value[9] = g_servo_min;
			}
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
			if (gUseRCServo2)
			{
				// This code below is the meat of the Process_S2() function
				// We have to manually write it in here rather than calling
				// the function because a real function inside the ISR 
				// causes the compiler to generate enormous amounts of setup/teardown
				// code and things run way too slowly.
				// Process_S2(1, g_servo2_min, 4, g_servo2_rate_up);
				gRC2Rate[0] = g_servo2_rate_up;
				gRC2Target[0] = g_servo2_min;
				gRC2Pin[0] = g_servo2_RPpin;
				if (gRC2Value[0] == 0)
				{
					gRC2Value[0] = g_servo2_min;
				}
			}
#endif
            if (gUseSolenoid)
			{
				SolenoidState = SOLENOID_OFF;
    			PenUpDownIO = 0;
			}		

			if (DelayCounter)
			{
				DelayCounter--;
			}
			if (DelayCounter)
			{
				AllDone = FALSE;
			}
			PenState = PEN_UP;
		}
		else if (Command == COMMAND_PEN_DOWN)
		{
			if (gUseRCServo1)
			{
				g_RC_value[9] = g_servo_max;
			}
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
			if (gUseRCServo2)
			{
				// This code below is the meat of the Process_S2() function
				// We have to manually write it in here rather than calling
				// the function because a real function inside the ISR 
				// causes the compiler to generate enormous amounts of setup/teardown
				// code and things run way too slowly.
				// Process_S2(1, g_servo2_max, 4, g_servo2_rate_down);
				gRC2Rate[0] = g_servo2_rate_down;
				gRC2Target[0] = g_servo2_max;
				gRC2Pin[0] = g_servo2_RPpin;
				if (gRC2Value[0] == 0)
				{
					gRC2Value[0] = g_servo2_max;
				}
                gUseRCServo2 = TRUE;
			}
#endif
			if (gUseSolenoid)
			{
				SolenoidState = SOLENOID_ON;
    			PenUpDownIO = 1;
			}

			if (DelayCounter)
			{
				DelayCounter--;
			}
			if (DelayCounter)
			{
				AllDone = FALSE;
			}
			PenState = PEN_DOWN;
		}
		else
		{
			
		}
	
		// Load the next move set in
		if (AllDone)
		{
			Command = COMMAND_NONE;
			if (NextReady)
			{
				for (i=0; i<4; i++)
				{
					StepAdd[i] = ToLoadStepAdd[i];
					StepsCounter[i] = ToLoadStepsCounter[i];
				}
				DirBits = ToLoadDirBits;
				Command = ToLoadCommand;
				DelayCounter = ToLoadDelayCounter;
				NextReady = FALSE;
			}
		}
		

		// Check for button being pushed
		if (
			(!swProgram)
			||
			(
				UseAltPause
				&&
#if defined(BOARD_EBB_V11)
				!PORTBbits.RB2		// For v1.1 hardware, use RB2 rather than RB0 for alt pause
#else
				!PORTBbits.RB0
#endif
			)
		)
		{
			ButtonPushed = TRUE;
		}
	}
}

// Init code
void EBB_Init(void)
{
	StepAdd[0] = 1;
	StepAdd[1] = 1;
	StepAdd[2] = 1;
	StepAdd[3] = 1;
	StepsCounter[0] = 0;
	StepsCounter[1] = 0;
	StepsCounter[2] = 0;
	StepsCounter[3] = 0;
	NextReady = FALSE;

#if defined(BOARD_EBB_V10)
	// Allow access to our bits in T1CON
	WDTCONbits.ADSHR = 0;
#endif

#if defined(BOARD_EBB_V10) || defined(BOARD_UBW)
	// Set up TMR1 for our 25KHz High ISR for stepping
	T1CONbits.RD16 = 0; 	// Set 8 bit mode
	T1CONbits.T1RUN = 0; 	// System clocked from other than T1
	T1CONbits.T1CKPS1 = 1; 	// Use 1:4 Prescale value
	T1CONbits.T1CKPS0 = 0;
	T1CONbits.T1OSCEN = 0; 	// Don't use external osc
	T1CONbits.T1SYNC = 0;
	T1CONbits.TMR1CS = 0; 	// Use Fosc/4 to clock timer
#elif defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
	// Set up TMR1 for our 25KHz High ISR for stepping
	T1CONbits.RD16 = 0; 	// Set 8 bit mode
	T1CONbits.TMR1CS1 = 0; 	// System clocked from Fosc/4
	T1CONbits.TMR1CS0 = 0;
	T1CONbits.T1CKPS1 = 1; 	// Use 1:4 Prescale value
	T1CONbits.T1CKPS0 = 0;
	T1CONbits.T1OSCEN = 0; 	// Don't use external osc
	T1CONbits.T1SYNC = 0;
#endif
	TMR1L = TIMER1_L_RELOAD;	// Set to 120 for 25KHz ISR fire
	TMR1H = TIMER1_H_RELOAD;	// 

	T1CONbits.TMR1ON = 1; // Turn the timer on

	IPR1bits.TMR1IP = 1;	// Use high priority interrupt
	PIR1bits.TMR1IF = 0;	// Clear the interrupt
	PIE1bits.TMR1IE = 1;	// Turn on the interrupt

	// For debugging
#if defined(BOARD_EBB_V10)
	PORTA = 0;
	TRISA = 0;
	PORTB = 0;
	TRISB = 0;
	PORTC = 0;		// Start out low
	TRISC = 0;		// Make portC
	PORTD = 0;
	TRISD = 0;
	PORTE = 0x16;
	TRISE = 0;
	PORTF = 0xA4;
	TRISF = 0x40;	// RF6 needs to be an input
	PORTG = 0;
	TRISG = 0;	
	PORTH = 0;
	TRISH = 0;
	PORTJ = 0;
	TRISJ = 0;

	Enable1IO = ENABLE_MOTOR;
	Enable1IO_TRIS = OUTPUT_PIN;
	Enable2IO = ENABLE_MOTOR;
	Enable2IO_TRIS = OUTPUT_PIN;
	Enable3IO = ENABLE_MOTOR;
	Enable3IO_TRIS = OUTPUT_PIN;
	Enable4IO = ENABLE_MOTOR;
	Enable4IO_TRIS = OUTPUT_PIN;

	Sleep1IO = 1;
	Sleep2IO = 1;
	Sleep3IO = 1;
	Sleep4IO = 1;

	MS1_1IO = 1;
	MS2_1IO = 1;
	MS1_2IO = 1;
	MS2_2IO = 1;
	MS1_3IO = 1;
	MS2_3IO = 1;
	MS1_4IO = 1;
	MS2_4IO = 1;

#elif defined(BOARD_EBB_V11)
	PORTA = 0;
	TRISA = 0x81;	// Bit0 and Bit7 needs to be an input (RA0 is REF analog input)
	PORTB = 0;
	TRISB = 2;		// Bit1 is our StartDemo switch
	INTCON2bits.RBPU = 0;	// Turn on weak-pull ups for port B
	PORTC = 0;		// Start out low
	TRISC = 0x80;	// Make portC output execpt for PortC bit 7, USB bus sense
	PORTD = 0;
	TRISD = 0;
	PORTE = 0;
	TRISE = 0;	
	ANCON0 = 0xFE;	// Let AN0 (RA0) be an analog input
	ANCON1 = 0x1F;	// Set all the rest to digital I/O

	Enable1IO = ENABLE_MOTOR;
	Enable2IO = ENABLE_MOTOR;
	MS1_1IO = 1;
	MS2_1IO = 1;
	MS1_2IO	= 1;
	MS2_2IO	= 1;
	Sleep1IO = 1;	
	Sleep2IO = 1;
	Step1IO	= 0;
	Dir1IO = 0;
	Step2IO	= 0;	
	Dir2IO = 0;	

#elif defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
//	PORTA = 0;
	RefRA0_IO_TRIS = INPUT_PIN;
//	PORTB = 0;
//	INTCON2bits.RBPU = 0;	// Turn on weak-pull ups for port B
//	PORTC = 0;		// Start out low
//	TRISC = 0x80;	// Make portC output execpt for PortC bit 7, USB bus sense
//	PORTD = 0;
//	TRISD = 0;
//	PORTE = 0;
//	TRISE = 0;	
	ANCON0 = 0xFE;	// Let AN0 (RA0) be an analog input
	ANCON1 = 0x17;	// Let AN11 (V+) also be an analog input

	MS1_IO = 1;
	MS1_IO_TRIS = OUTPUT_PIN;
	MS2_IO = 1;
	MS2_IO_TRIS = OUTPUT_PIN;
	MS3_IO	= 1;
	MS3_IO_TRIS = OUTPUT_PIN;

	Enable1IO = 1;	
	Enable1IO_TRIS = OUTPUT_PIN;	
	Enable2IO = 1;
	Enable2IO_TRIS = OUTPUT_PIN;

	Step1IO	= 0;
	Step1IO_TRIS = OUTPUT_PIN;
	Dir1IO = 0;
	Dir1IO_TRIS = OUTPUT_PIN;
	Step2IO	= 0;	
	Step2IO_TRIS = OUTPUT_PIN;	
	Dir2IO = 0;	
	Dir2IO_TRIS = OUTPUT_PIN;

#elif defined(BOARD_UBW)
	PORTA = 0;
	TRISA = 0;
	PORTB = 0;
	TRISB = 0;
	PORTC = 0;		// Start out low
	TRISC = 0;		// Make portC outputs
#endif

	// For bug in VUSB divider resistor, set RC7 as output and set high
	// Wait a little while to charge up
	// Then set back as an input
	// The idea here is to get the schmidt trigger input RC7 high before
	// we make it an input, thus getting it above the 2.65V ST threshold
	// And allowing VUSB to keep the logic level on the pin high at 2.5V
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
    #if defined(USE_USB_BUS_SENSE_IO)
	    tris_usb_bus_sense = OUTPUT_PIN; // See HardwareProfile.h
    	USB_BUS_SENSE = 1;
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		Delay1TCY();
		tris_usb_bus_sense = INPUT_PIN;
		USB_BUS_SENSE = 0;
	#endif
#endif
    gUseSolenoid = TRUE;

    // Set up pen up/down direction as output
    /// TODO: This should be different based upon the board type, right?
	PenUpDownIO = 0;
	PenUpDownIO_TRIS = OUTPUT_PIN;

	SolenoidState = SOLENOID_ON;
	UseBuiltInDrivers = TRUE;
	gUseRCServo1 = FALSE;
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
	gUseRCServo2 = TRUE;
#endif
	PenState = PEN_UP;
	Layer = 0;
	NodeCount = 0;
	ButtonPushed = FALSE;
	// Default RB0 to be an input, with the pull-up enabled, for use as alternate
	// PAUSE button (just like PRG)
	// Except for v1.1 hardware, use RB2
#if defined(BOARD_EBB_V11)
	TRISBbits.TRISB2 = 1;
#else
	TRISBbits.TRISB0 = 1;
#endif
	INTCON2bits.RBPU = 0;	// Turn on all of PortB pull-ups
	UseAltPause = TRUE;

	TRISBbits.TRISB3 = 0;		// Make RB3 an output
	PORTBbits.RB3 = 0;          // And turn it on
}

// Stepper (mode) Configure command
// SC,1,0<CR> will use just solenoid output for pen up/down
// SC,1,1<CR> will use servo on RB1 for pen up/down
// SC,1,2<CR> will use servo on RB1 for pen up/down, but with ECCP2 (PWM) in hardware (default)
// SC,2,0<CR> will use built-in stepper driver chips (default)
// SC,2,1<CR> will use the following pins for stepper driver outputs (EBB_V11)
//		ENABLE1 = RA5
//		ENABLE2 = RB5
//		STEP1 = RD1
//		DIR1 = RD0
//		STEP2 = RC2
//		DIR2 = RC0
// SC,4,<servo2_min><CR> will set <servo2_min> as the minimum value for the servo (1 to 65535)
// SC,5,<servo2_max><CR> will set <servo2_max> as the maximum value for the servo (1 to 65535)
// SC,6,<servo_min><CR> will set <servo_min> as the minimum value for the servo (1 to 11890)
// SC,7,<servo_max><CR> will set <servo_max> as the maximum value for the servo (1 to 11890)
// SC,8,<servo2_slots><CR> sets the number of slots for the servo2 system (1 to 24)
// SC,9,<servo2_slotMS><CR> sets the number of ms in duration for each slot (1 to 6)
// SC,10,<servo2_rate><CR> sets the rate of change for the servo (both up and down)
// SC,11,<servo2_rate><CR> sets the pen up speed
// SC,12,<servo2_rate><CR> sets the pen down speed
// SC,13,1<CR> enables RB3 as parallel input to PRG button for pause detection
// SC,13,0<CR> disables RB3 as parallel input to PRG button for pause detection
// SC,14,1<CR> enables solenoid output on RB4
// SC,14,0<CR> disables solenoid output on RB4
void parse_SC_packet (void)
{
	unsigned char Para1 = 0;
	unsigned int Para2 = 0;

	// Extract each of the values.
	extract_number (kUCHAR, &Para1, kREQUIRED);
	extract_number (kUINT, &Para2, kREQUIRED);

	// Bail if we got a conversion error
	if (error_byte)
	{
		return;
	}

	// Check for command to use servo rather than solenoid (we'll leave
	// the solenoid on too)
	if (Para1 == 1)
	{
		if (Para2 == 0)
		{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
			gUseRCServo2 = FALSE;
#endif
			gUseRCServo1 = FALSE;
            gUseSolenoid = TRUE;
			// Turn off RC Servo pulses on RB1
			g_RC_value[9] = 0;
		}
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)					// NOTE: Only VBB V1.1 and above have this RC Servo option
		else if (Para2 == 1)
		{
			gUseRCServo1 = TRUE;
			gUseRCServo2 = FALSE;
			TRISBbits.TRISB1 = 0; 	// RB1 needs to be an output
			
			// We're going to do the work here of an 'RC' command, and set the RC servo to one
			// of it limits.
			// Store the new RC time value
//			g_RC_value[9] = (65535 - (g_servo_min + 45));			
			// Only set this state if we are off - if we are already running on 
			// this pin, then the new value will be picked up next time around (19ms)
			if (kOFF == g_RC_state[9])
			{
				g_RC_state[9] = kWAITING;
			}
		}
		else
		{
			gUseRCServo1 = FALSE;
			TRISBbits.TRISB1 = 0; 	// RB1 needs to be an output
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
			Process_S2(1, g_servo2_min, 4, g_servo2_rate_up);
#endif
			process_SP(PEN_UP, 0);			// Start servo up 
		}
#endif
	}
	// Check for command to switch between built-in drivers and external drivers
	else if (Para1 == 2)
	{
		if (Para2 == 0)
		{
			UseBuiltInDrivers = TRUE;
			// Initalize the alternate driver I/O ports
			Dir1AltIO_TRIS = 0;
			Dir2AltIO_TRIS = 0;
			Step1AltIO_TRIS = 0;
			Step2AltIO_TRIS = 0;
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
			Enable1AltIO_TRIS = 0;
			Enable2AltIO_TRIS = 0;
#endif
		}
		else
		{
			UseBuiltInDrivers = FALSE;
		}
	}
	// Set <min_servo> for Servo2 method
	else if (Para1 == 4)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		g_servo2_min = Para2;
#endif
	}
	// Set <max_servo> for Servo2
	else if (Para1 == 5)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		g_servo2_max = Para2;
#endif
	}
	// Set <min_servo>
	else if (Para1 == 6)
	{
		if (Para2 > MAX_RC_DURATION)
		{
			Para2 = MAX_RC_DURATION;
		}
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		g_servo_min = Para2;
#endif
	}
	// Set <max_servo>
	else if (Para1 == 7)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		if (Para2 > MAX_RC_DURATION)
		{
			Para2 = MAX_RC_DURATION;
		}
		g_servo_max = Para2;
#endif
	}
	// Set <gRC2Slots>
	else if (Para1 == 8)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		if (Para2 > MAX_RC2_SERVOS)
		{
			Para2 = MAX_RC2_SERVOS;
		}
		gRC2Slots = Para2;
#endif
	}
	else if (Para1 == 9)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		if (Para2 > 6)
		{
			Para2 = 6;
		}
		gRC2SlotMS = Para2;
#endif
	}
	else if (Para1 == 10)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		g_servo2_rate_up = Para2;
		g_servo2_rate_down = Para2;
#endif
	}
	else if (Para1 == 11)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		g_servo2_rate_up = Para2;
#endif
	}
	else if (Para1 == 12)
	{
#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		g_servo2_rate_down = Para2;
#endif
	}
	if (Para1 == 13)
	{
		if (Para2)
		{
			UseAltPause = TRUE;
		}
		else
		{
			UseAltPause = FALSE;
		}			
	}
    if (Para1 == 14)
    {
        if (Para2)
        {
            gUseSolenoid = TRUE;
        }
        else
        {
            gUseSolenoid = FALSE;
        }

        // Now set the state of the output pin to match pen state
        if (PenState == PEN_UP)
        {
            SolenoidState = SOLENOID_OFF;
            PenUpDownIO = 0;
        }
        else
        {
            SolenoidState = SOLENOID_ON;
            PenUpDownIO = 1;
        }
    }
	print_ack();
}

// The Stepper Motor command
// Usage: SM,<move_duration>,<axis1_steps>,<axis2_steps>,<axis3_steps>,<axis4_steps><CR>
// <move_duration> is a number from 1 to 65535, indiciating the number of milliseconds this move should take
// <axisX_steps> is a signed 16 bit number indicating how many steps (and what direction) the axis should take
// NOTE1: <axis2_steps>, <axis3_steps> and <axis4_steps> are optional and can be left off
// If the EBB can not make the move in the speicified time, it will take as long as it needs to at max speed
// i.e. SM,1,1000 will not produce 1000steps in 1ms. Instead, it will take 40ms (25KHz max step rate)
// NOTE2: If you specify zero steps for the axies, then you effectively create a delay. Use for small
// pauses before raising or lowering the pen, for example.
void parse_SM_packet (void)
{
	unsigned int Duration;
	signed int A1Steps = 0, A2Steps = 0, A3Steps = 0, A4Steps = 0;

	// Extract each of the values.
	extract_number (kUINT, &Duration, kREQUIRED);
	extract_number (kINT, &A1Steps, kREQUIRED);
	extract_number (kINT, &A2Steps, kOPTIONAL);
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
	extract_number (kINT, &A3Steps, kOPTIONAL);
	extract_number (kINT, &A4Steps, kOPTIONAL);
#endif
	// Bail if we got a conversion error
	if (error_byte)
	{
		return;
	}
	process_SM(Duration, A1Steps, A2Steps, A3Steps, A4Steps);

	print_ack();
}

static void process_SM(
	unsigned int Duration, 
	signed int A1Stp, 
	signed int A2Stp, 
	signed int A3Stp, 
	signed int A4Stp
)
{
	// Trial: Spin here until there's space in the fifo
	while(NextReady)
	;

	// Check for delay
	if (A1Stp == 0 && A2Stp == 0 && A3Stp == 0 && A4Stp == 0)
	{
		ToLoadCommand = COMMAND_DELAY;
		ToLoadDelayCounter = 25 * Duration;
	}
	else
	{
		ToLoadDelayCounter = 1;
		ToLoadDirBits = 0;
		
		// Always enable both motors when we want to move them
#if defined(BOARD_EBB_V10) || defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
		Enable1IO = ENABLE_MOTOR;
		Enable2IO = ENABLE_MOTOR;
#if defined(BOARD_EBB_V10)
		Enable3IO = ENABLE_MOTOR;
		Enable4IO = ENABLE_MOTOR;
#endif
#endif

		// First, set the direction bits
		if (A1Stp < 0)
		{
			ToLoadDirBits = ToLoadDirBits | DIR1_BIT;
			A1Stp = -A1Stp;
		}
		if (A2Stp < 0)
		{
			ToLoadDirBits = ToLoadDirBits | DIR2_BIT;
			A2Stp = -A2Stp;
		}
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
		if (A3Stp < 0)
		{
			ToLoadDirBits = ToLoadDirBits | DIR3_BIT;
			A3Stp = -A3Stp;
		}
		if (A4Stp < 0)
		{
			ToLoadDirBits = ToLoadDirBits | DIR4_BIT;
			A4Stp = -A4Stp;
		}
#endif	
		// Range check Steps/Duration
//		if (
//			(A1Steps / Duration < 25)
//			||
//			(A2Steps / Duration < 25)
//			||
//			(A3Steps / Duration < 25)
//			||
//			(A4Steps / Duration < 25)
//		)
//		{
//				bitset (error_byte, kERROR_BYTE_STEPS_TO_FAST);
//				return;			
//		}	
//		else
//		{
			// To compute StepAdd values from Duration,
			ToLoadStepAdd[0] = (unsigned int)
									(
										((unsigned long)0x8000 * (unsigned long)A1Stp)
										/
										((unsigned long)25 * (unsigned long)Duration)
									) + 1;
			ToLoadStepsCounter[0] = A1Stp;
			ToLoadStepAdd[1] = (unsigned int)
									(
										((unsigned long)0x8000 * (unsigned long)A2Stp)
										/
										((unsigned long)25 * (unsigned long)Duration)
									) + 1;
			ToLoadStepsCounter[1] = A2Stp;
#if defined(BOARD_UBW) || defined(BOARD_EBB_V10)
			ToLoadStepAdd[2] = (unsigned int)
									(
										((unsigned long)0x8000 * (unsigned long)A3Stp)
										/
										((unsigned long)25 * (unsigned long)Duration)
									) + 1;
			ToLoadStepsCounter[2] = A3Stp;
			ToLoadStepAdd[3] = (unsigned int)
									(
										((unsigned long)0x8000 * (unsigned long)A4Stp)
										/
										((unsigned long)25 * (unsigned long)Duration)
									) + 1;
			ToLoadStepsCounter[3] = A4Stp;
#endif
			ToLoadCommand = COMMAND_MOVE;

//printf("SA:%5d S:%4d SA:%5d S:%4d\n\r", ToLoadStepAdd[0], ToLoadStepsCounter[0], ToLoadStepAdd[1], ToLoadStepsCounter[1]);

//		}
	}
		
	NextReady = TRUE;

}

// Query Pen
// Usage: QP<CR>
// Returns: 0 for down, 1 for up, then OK<CR>
void parse_QP_packet(void)
{
	printf((far rom char *)"%d\n\r", PenState);

	print_ack();
}

// Toggle Pen
// Usage: TP<CR>
// Returns: OK<CR>
// Just toggles state of pen arm
void parse_TP_packet(void)
{
	unsigned short CommandDuration = 500;

	// Extract each of the values.
	extract_number (kUINT, &CommandDuration, kOPTIONAL);

	// Bail if we got a conversion error
	if (error_byte)
	{
		return;
	}

	if (PenState == PEN_UP)
	{
		process_SP(PEN_DOWN, CommandDuration);
	}
	else
	{
		process_SP(PEN_UP, CommandDuration);
	}

	print_ack();
}

// Set Pen
// Usage: SP,<1,0>,<Duration>,<PortB_Pin><CR>
// <PortB_Pin> is 0 to 7
void parse_SP_packet(void)
{
	unsigned char State = 0;
	unsigned short CommandDuration = 0;
	unsigned char Pin = DEFAULT_EBB_SERVO_PORTB_PIN;
    ExtractReturnType Ret;

	// Extract each of the values.
	extract_number (kUCHAR, &State, kREQUIRED);
	extract_number (kUINT, &CommandDuration, kOPTIONAL);
	Ret = extract_number (kUCHAR, &Pin, kOPTIONAL);

	// Bail if we got a conversion error
	if (error_byte)
	{
		return;
	}

	if (Pin > 7)
	{
		Pin = DEFAULT_EBB_SERVO_PORTB_PIN;
	}
    // Make sure that the selected pin we're going to use is an output
    // (This code only works for PortB - maybe expand it in the future for all ports.)
    TRISB = TRISB & ~(1 << Pin);
    // Add 3 to get from PORTB pin number to RPn number
    g_servo2_RPpin = Pin + 3;

	process_SP(State, CommandDuration);

	print_ack();
}

void process_SP(SolenoidStateType NewState, unsigned short CommandDuration)
{	
	// Trial: Spin here until there's space in the fifo
	while(NextReady)
	;

#if defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
    gUseRCServo2 = TRUE;
#endif

	if (NewState == PEN_UP)
	{
		ToLoadCommand = COMMAND_PEN_UP;
	}
	else
	{
		ToLoadCommand = COMMAND_PEN_DOWN;
	}
	ToLoadDelayCounter = CommandDuration * 25;

	NextReady = TRUE;	
}

// Enable Motor
// Usage: EM,<EnableAxis1>,<EnableAxis2>,<EnableAxis3>,<EnableAxis4><CR>
// Everything afer EnableAxis1 is optional
// Each parameter can have a value of
//		0 to disable that motor driver
// FOR OLD DRIVER CHIP
//		1 to enable the driver in 1/8th step mode
//		2 to enable the driver in 1/4 step mode
//		3 to enable the driver in 1/2 step mode
//		4 to enable the driver in full step mode
// FOR NEW DRIVER CHIP (only first parameter applies, and it applies to both drivers)
//		1 to enable the driver in 1/16th step mode
//		2 to enable the driver in 1/8 step mode
//		3 to enable the driver in 1/4 step mode
//		4 to enable the driver in 1/2 step mode
//		5 to enable the driver in full step mode
// If you disable a motor, it goes 'limp' (we clear the ENABLE pin on that motor's
// driver chip)
void parse_EM_packet(void)
{
#if defined(BOARD_EBB_V10) || defined(BOARD_EBB_V11) || defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
	unsigned char EA1, EA2, EA3, EA4;
	ExtractReturnType RetVal;

	// Extract each of the values.
	RetVal = extract_number (kUCHAR, &EA1, kREQUIRED);
	if (kEXTRACT_OK == RetVal)
	{
		// Bail if we got a conversion error
		if (error_byte)
		{
			return;
		}
		if (UseBuiltInDrivers)
		{
			if (EA1 > 0)
			{
				Enable1IO = ENABLE_MOTOR;
#if defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE)
				if (EA1 == 1)
				{
					MS1_IO = 1;
					MS2_IO = 1;
					MS3_IO = 1;
				}
				if (EA1 == 2)
				{
					MS1_IO = 1;
					MS2_IO = 1;
					MS3_IO = 0;
				}
				if (EA1 == 3)
				{
					MS1_IO = 0;
					MS2_IO = 1;
					MS3_IO = 0;
				}
				if (EA1 == 4)
				{
					MS1_IO = 1;
					MS2_IO = 0;
					MS3_IO = 0;
				}				
				if (EA1 == 5)
				{
					MS1_IO = 0;
					MS2_IO = 0;
					MS3_IO = 0;
				}				
#else
				if (EA1 == 1)
				{
					MS1_1IO = 1;
					MS2_1IO = 1;
				}
				if (EA1 == 2)
				{
					MS1_1IO = 0;
					MS2_1IO = 1;
				}
				if (EA1 == 3)
				{
					MS1_1IO = 1;
					MS2_1IO = 0;
				}
				if (EA1 == 4)
				{
					MS1_1IO = 0;
					MS2_1IO = 0;
				}
#endif
			}
			else
			{
				Enable1IO = DISABLE_MOTOR;
			}
		}
		else
		{
			if (EA1 > 0)
			{
				Enable1AltIO = ENABLE_MOTOR;
			}
			else
			{
				Enable1AltIO = DISABLE_MOTOR;
			}
		}
	}

	RetVal = extract_number (kUCHAR, &EA2, kOPTIONAL);
	if (kEXTRACT_OK == RetVal)
	{
		// Bail if we got a conversion error
		if (error_byte)
		{
			return;
		}
		if (UseBuiltInDrivers)
		{
			if (EA2 > 0)
			{
				Enable2IO = ENABLE_MOTOR;
#if !(defined(BOARD_EBB_V12) || defined(BOARD_EBB_V13_AND_ABOVE))
/// TODO: fix this based upon type of driver chip
				if (EA2 == 1)
				{
					MS1_2IO = 1;
					MS2_2IO = 1;
				}
				if (EA2 == 2)
				{
					MS1_2IO = 0;
					MS2_2IO = 1;
				}
				if (EA2 == 3)
				{
					MS1_2IO = 1;
					MS2_2IO = 0;
				}
				if (EA2 == 4)
				{
					MS1_2IO = 0;
					MS2_2IO = 0;
				}				
#endif
			}
			else
			{
				Enable2IO = DISABLE_MOTOR;
			}
		}
		else
		{
			if (EA2 > 0)
			{
				Enable2AltIO = ENABLE_MOTOR;
			}
			else
			{
				Enable2AltIO = DISABLE_MOTOR;
			}
		}
	}
#if defined(BOARD_EBB_V10)
	RetVal = extract_number (kUCHAR, &EA3, kOPTIONAL);
	if (kEXTRACT_OK == RetVal)
	{
		// Bail if we got a conversion error
		if (error_byte)
		{
			return;
		}
		if (EA3 > 0)
		{
			Enable3IO = ENABLE_MOTOR;
		}
		else
		{
			Enable3IO = DISABLE_MOTOR;
		}
	}
	RetVal = extract_number (kUCHAR, &EA4, kOPTIONAL);
	if (kEXTRACT_OK == RetVal)
	{
		// Bail if we got a conversion error
		if (error_byte)
		{
			return;
		}
		if (EA4 > 0)
		{
			Enable4IO = ENABLE_MOTOR;
		}
		else
		{
			Enable4IO = DISABLE_MOTOR;
		}
	}
#endif
#endif
	print_ack();
}

// Node counter Incriment
// Usage: NI<CR>
void parse_NI_packet(void)
{
	if (NodeCount < 0xFFFFFFFEL)
	{
		NodeCount++;
	}
	print_ack();
}

// Node counter Deccriment
// Usage: ND<CR>
void parse_ND_packet(void)
{
	if (NodeCount)
	{
		NodeCount--;
	}
	print_ack();
}

// Set Node counter
// Usage: SN,<value><CR>
// <value> is a 4 byte unsigned value
void parse_SN_packet(void)
{
	unsigned long Temp;
	ExtractReturnType RetVal;
	
	RetVal = extract_number (kULONG, &Temp, kREQUIRED);
	if (kEXTRACT_OK == RetVal)
	{
		NodeCount = Temp;
	}
	print_ack();
}

// Query Node counter
// Usage: QN<CR>
// Returns: <NodeCount><CR>
// OK<CR>
void parse_QN_packet(void)
{
	printf ((far rom char*)"%010lu\r\n", NodeCount);

	print_ack();
}

// Set Layer
// Usage: SL,<NewLayer><CR>
void parse_SL_packet(void)
{
	// Extract each of the values.
	extract_number (kUCHAR, &Layer, kREQUIRED);

	// Bail if we got a conversion error
	if (error_byte)
	{
		return;
	}

	print_ack();
}

// Query Layer
// Usage: QL<CR>
// Returns: <Layer><CR>
// OK<CR>
void parse_QL_packet(void)
{
	printf ((far rom char*)"%03i\r\n", Layer);

	print_ack();
}

// Query Button
// Usage: QB<CR>
// Returns: <HasButtonBeenPushedSinceLastQB><CR> (0 or 1)
// OK<CR>
void parse_QB_packet(void)
{
	printf ((far rom char*)"%1i\r\n", ButtonPushed);
	ButtonPushed = FALSE;

	print_ack();
}

// Query Current
// Usage: QC<CR>
// Returns: <voltage_on_REF_RA0_net>,<voltage_on_v+_net><CR>
// Both values have a range of 0 to 1023 (10-bit ADC)
// The ADC is set up with 0V = 0 and 3.3V = 1023
// For the REF_RA0 (current adjustment pot) a value of 0V-ADC (0 counts) = 46mA
// and a value of 2.58V-ADC (800 counts) = 1.35A
// For the V+ net a value of 0V-ADC (0 counts) = 0V on V+
// and a value of 3.36V-ADC (1023 counts) = 37V on V+
// REF_RA0 comes in on AN0 (RA0)
// V+ comes in on AN11 (RC2)
void parse_QC_packet(void)
{
	unsigned int  AN0 = 0;
	unsigned int  AN11 = 0;

	// Set the channel to zero to start off with (AN0)
	ADCON0 = 0;
	// Set up right justified, 8Tad tim, Fosc/4
	ADCON1 = 0b10100100;
	
	// Clear the interrupt
	PIR1bits.ADIF = 0;

	// And make sure to always use low priority.
	IPR1bits.ADIP = 0;

	// Make sure it's on!
	ADCON0bits.ADON = 1;

	// Wait for 10ms
	QC_ms_timer = 10;
	while (QC_ms_timer);

	// And tell the A/D to GO!
	ADCON0bits.GO_DONE = 1;
	
	// Now sit and wait until the conversion is done
	while (ADCON0bits.GO_DONE)
	;

	// Now grab our result
	AN0 = ((UINT)ADRESH << 8) + ADRESL;
	
	// Set the channel to AN11
	ADCON0 = 0b00101101;
	
	// Wait for 10ms
	QC_ms_timer = 10;
	while (QC_ms_timer);

	// And tell the A/D to GO!
	ADCON0bits.GO_DONE = 1;
	
	// Now sit and wait until the conversion is done
	while (ADCON0bits.GO_DONE)
	;
	
	// Now grab our result
	AN11 = ((UINT)ADRESH << 8) + ADRESL;
	
	// Print out our results
	printf ((far rom char*)"%04i,%04i\r\n", AN0, AN11);

	print_ack();	
}	

// Set Engraver
// Usage: SE,<state>,<power><CR>
// <state> is 0 for off and 1 for on (required)
// <power> is 10 bit PWM power level (optional)
// We boot up with <power> at 1023 (full on)
// The engraver motor is always assumed to be on RB3
// So our init routine will map ECCP1
//
// Timer0 is RC command
// Timer1 is stepper
// Timer2 and ECCP1 is engraver PWM
// Timer3 and ECCP2 is RC servo2 output
// Timer4 is 1ms ISR

void parse_SE_packet(void)
{
	BYTE State = 0;
	UINT Power = 1024;
	
	// Extract each of the values.
	extract_number (kUCHAR, &State, kREQUIRED);
	extract_number (kUINT, &Power, kOPTIONAL);

	// Bail if we got a conversion error
	if (error_byte)
	{
		return;
	}

	// Limit check
	if (Power <= 1023)
	{
		StoredEngraverPower = Power;
        
        // Set up PWM for Engraver control
        // We will use ECCP1 and Timer2 for the engraver PWM output on RB3
        // Our PWM will operate at about 40Khz.

        // Set our reload value
        PR2 = 0xFF;

        // Set to %50 power on boot
        StoredEngraverPower = 512;

        // Initalize Timer2

        // The prescaler will be at 1
        T2CONbits.T2CKPS = 0b00;

        // Do not generate an interrupt
        PIE1bits.TMR2IE = 0;

        TCLKCONbits.T3CCP1 = 1;		// ECCP1 uses Timer1/2 and ECCP2 uses Timer3/4
        TCLKCONbits.T3CCP2 = 0;		// ECCP1 uses Timer1/2 and ECCP2 uses Timer3/4

        CCP1CONbits.CCP1M = 0b1100;	// Set EECP1 as PWM mode
        CCP1CONbits.P1M = 0b00;		// Enhanged PWM mode: single ouptut

        // Set up output routing to go to RB3 (RP6)
        RPOR6 = 14;	// 14 is CCP1/P1A - ECCP1 PWM Output Channel A

    	T2CONbits.TMR2ON = 1;		// Turn it on
    }

	// Now act on the State
	if (State)
	{
		// Set RB3 to StoredEngraverPower
		CCPR1L = StoredEngraverPower >> 2;
		CCP1CON = (CCP1CON & 0b11001111) | ((StoredEngraverPower << 4) & 0b00110000);
	}
	else
	{
		// Set RB3 to low by setting PWM duty cycle to zero
		CCPR1L = 0;
		CCP1CON = (CCP1CON & 0b11001111);
	}		

	print_ack();
}

// RM command
// For Run Motor - allows completely independant running of the two stepper motors
void parse_RM_packet(void)
{
	
	
}

// QM command
// For Query Motor - returns the curent status of each motor
// QM takes no parameters, so useage is just QM<CR>
// QM returns:
// QM,<SyncFIFOSatus>,<Motor1Satus>,<Motor2Status><CR>
// where <SyncFIFOStatus> is 0 (no command executing) or 1 (command executing)
// and <MotorXStatus> is 0 (motor not executing a command) or 1 (motor executing a command)
void parse_QM_packet(void)
{
	printf((far ROM char *)"QM,%i,%i,%i\n\r", NextReady, 0, 0);
}
