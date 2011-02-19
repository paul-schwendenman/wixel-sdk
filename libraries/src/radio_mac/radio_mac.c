// TODO: see if the CC2511 radio module ALWAYS stays in RX mode when it receives a
// packet with a bad CRC.

// Things to try:
// TODO: look at radio settings from smartrf_cc2511.h (swrc088c) and try them

/*  NOTE: Calibration of the frequency synthesizer and other RF hardware takes about 800 us and
 *  must be done regularly.  There are several options for when to do the calibration and not.
 *  We configured the radio to automatically calibrate whenever going from the IDLE state to TX
 *  or RX (MCSM0.FS_AUTOCAL = 01).  The radio will go in to the idle state whenever the is an RX
 *  timeout.  However, to enable a quick turnaround between TX and RX, we configured the radio to
 *  automatically go in to the FSTXON mode after it is done with RX or TX mode.  FSTXON means that
 *  the frequency synthesizer is on and the radio is ready to go in to RX or TX mode quickly
 *  (but it goes to TX mode faster).
 *
 *  So basically we are depending on the RX timeout feature to schedule our calibrations for us,
 *  instead of doing it manually.  This should work, but it will probaby hinder our ability to
 *  quickly recover from lost packets (the RX timeout event is what happens when a packet is lost).
 *  An easy alternative would be to only calibrate after every 10th RX timeout or something like
 *  that.
 */

/*  The definition of the maximum packet size (and the code that sets the PKTLEN register) is not
 *  in this layer.  That is up to the higher-level code (radio_link.c) to decide.   When this
 *  layer needs to know the packet size (for setting up the DMA), it reads it from PKTLEN.  This
 *  makes the code a little less efficient because it takes longer to access an XDATA register
 *  than a hardcoded constant, but makes this MAC layer much more reusable.
 */

#include "radio_mac.h"
#include <cc2511_map.h>
#include <string.h>
#include <dma.h>
#include <radio_registers.h>
void delayMicroseconds(uint8 ms);  // for tmphax

#include "random.h"

// The RFST register is how we tell the radio to do something, and these are the
// command strobes we can write to it:
#define SFSTXON 0
#define SCAL    1
#define SRX     2
#define STX     3
#define SIDLE   4

void radioPhyInit();
static void radioMacEvent(uint8 event);

// Bits for sending commands to the MAC in an interrupt safe way.
static volatile BIT strobe = 0;

// Error reporting
volatile BIT radioRxOverflowOccurred = 0;
volatile BIT radioTxUnderflowOccurred = 0;
volatile uint8 DATA radioBadMarcState = 0xFF;

volatile uint8 DATA radioMacState = RADIO_MAC_STATE_OFF;

ISR(RF, 1)
{
    S1CON = 0; // Clear the general RFIF interrupt registers

	if (RFIF & 0x10) // Check IRQ_DONE
	{
	    if (radioMacState == RADIO_MAC_STATE_TX)
	    {
			// We just sent a packet.
	    	radioMacEvent(RADIO_MAC_EVENT_TX);
	    }
	    else if (radioMacState == RADIO_MAC_STATE_RX)
	    {
			// We just received a packet, but it might have an invalid CRC or be irrelevant
	    	// for other reasons.
	    	radioMacEvent(RADIO_MAC_EVENT_RX);
		}
	}

	if (RFIF & 0x20)  // Check IRQ_TIMEOUT
	{
		// We were listening for packets but we didn't receive anything
		// and the timeout period expired.
		radioMacEvent(RADIO_MAC_EVENT_RX_TIMEOUT);
	}

    if (strobe)
    {
    	// Some other code has set the strobe bit, which means he wants the radioMacEventHandler to
    	// run soon, typically because new data is available for it to send.
    	if (radioMacState == RADIO_MAC_STATE_TX)
    	{
    		// We are currently transmitting, so we will wait for the end of that packet.
    		// Then, we will issue a RADIO_MAC_EVENT_TX.
    		return;
    	}

    	if (PKTSTATUS & (1<<3))  // Check SFD bit (Start of Frame Delimiter)
    	{
    		// We are currently receiving a packet, so we will wait for the end of that
    		// packet and then issue a RADIO_MAC_EVENT_RX.
    		// ASSUMPTION: There is no automatic address filtering, and packets with
    		// bad CRCs still result in a RAIDO_MAC_EVENT_RX.
    		return;
    	}

    	/*  The code below is necessary because we found that if the radio is in the
           process of calibrating itself to go in to RX mode, it won't respond
           correctly to an STX strobe (it goes in to RX mode instead of TX).
           We only need to worry about that here, and not in the other events,
           because those other events only happen at times when the radio should not
           be in the middle of calibrating itself. */
    	if (MARCSTATE != 0x0D)
    	{
    		RFST = SIDLE;
    	}

    	// We are currently listening for packets and nothing is being received at the
    	// moment, so we should stop and issue a RADIO_MAC_EVENT_STROBE now.
    	radioMacEvent(RADIO_MAC_EVENT_STROBE);
    }

	if (RFIF & 0x80)   // Check IRQ_TXUNF
	{
		// We were not sending data to the radio fast enough, so there was a
		// TX underflow.  This should not happen because we use DMA to send
		// the data.  Report it as an error.
		radioTxUnderflowOccurred = 1;
		RFIF = ~0x80;
	}

	if (RFIF & 0x40)   // Check IRQ_RXOVF
	{

		// We were not reading data from the radio fast enough, so there was
		// a RX overflow.  This should not happen.  Report it as an error.
		radioRxOverflowOccurred = 1;
		RFIF = ~0x40;

		// The radio module is probably now in the RX_OVERFLOW state where it can not
		// receive packets.  The way to get out of this state is:
		//RFST = SIDLE;  (and you probably should put NOPs here, though in my tests it was not necessary)
		//RFST = SRX;
		// We never expect an RX overflow to happen though, so we won't do that.
	}
}

void radioMacEvent(uint8 event)
{
	/** Turn off the radio. ****************************************************/
	/* This is necessary because David has observed that sometimes (maybe every
	 * time?) when a packet with a bad CRC is received, the radio stays in RX
	 * instead of going to FSTXON mode the way it should (RXOFF_MODE=01).
	 * If we allow the radio to stay in RX mode then an RX overflow error could
	 * happen later after we disarm the DMA channel. */
	if (MARCSTATE != 0x12 && MARCSTATE != 0x01 && MARCSTATE != 0x00 && MARCSTATE != 0x15)
	{
		// Report the bad state to the main loop for debugging purposes.
		if (radioBadMarcState == 0xFF && MARCSTATE != 0x0D)
		{
			radioBadMarcState = MARCSTATE;
		}

		// Fix the bad state by telling the radio to go to the SFSTXON state.
		RFST = SFSTXON;
	}

	/** Disarm the DMA channel. ************************************************/
	DMAARM = 0x80 | (1<<DMA_CHANNEL_RADIO); // Abort any ongoing radio DMA transfer.
	DMAIRQ &= ~(1<<DMA_CHANNEL_RADIO);      // Clear any pending radio DMA interrupt

	/** Report the event to the higher-level code so it can decide what to do. **/
	radioMacState = RADIO_MAC_STATE_RX;    // Default next state: RX
	MCSM2 = 0x07;                          // Default next timeout: infinite.
	radioMacEventHandler(event);

	/** Clear the some flags from the radio ***********************************/
	// We want to do it before restarting the radio (to avoid accidentally missing
	// an event) but we want to do it as long as possible AFTER turning off the
	// radio.
	RFIF = ~0x30;  // Clear IRQ_DONE and IRQ_TIMEOUT if they are set.

    /** Start up the radio in the new state which was decided above. **/
	delayMicroseconds(2);
	switch(radioMacState)
	{
	case RADIO_MAC_STATE_RX:
		DMAARM |= (1<<DMA_CHANNEL_RADIO);   // Arm DMA channel.
		RFST = SRX;            	            // Switch radio to RX.
		break;
	case RADIO_MAC_STATE_TX:
	    DMAARM |= (1<<DMA_CHANNEL_RADIO);   // Arm DMA channel.
	    RFST = STX;                         // Switch radio to TX.
	    break;
	}

	// Clear the strobe bit because we just ran the radioMacEventHandler.
    strobe = 0;
}

void radioMacStrobe()
{
	strobe = 1;
	S1CON |= 3;
}

void radioMacInit()
{
	radioRegistersInit();

	CHANNR = 0;

    // MCSM.FS_AUTOCAL = 1: Calibrate freq when going from IDLE to RX or TX (or FSTXON).
	MCSM0 = 0x14;    // Main Radio Control State Machine Configuration
    MCSM1 = 0x05;    // Disable CCA.  After RX, go to FSTXON.  After TX, go to FSTXON.
    MCSM2 = 0x07;    // NOTE: MCSM2 also gets set every time we go in to RX mode.

    IEN2 |= 0x01;    // Enable RF general interrupt
    RFIM = 0xF0;     // Enable these interrupts: DONE, RXOVF, TXUNF, TIMEOUT

	EA = 1;  	     // Enable interrupts in general

	dmaConfig.radio.DC6 = 19; // WORDSIZE = 0, TMODE = 0, TRIG = 19
}

// Called by the user during RADIO_MAC_STATE_IDLE to tell the Mac that it should
// start trying to receive a packet.
void radioMacRx(uint8 XDATA * packet, uint16 timeout)
{
	if (timeout)
	{
	    MCSM2 = 0x01;   // RX_TIME = 1.  Helps determine the units of the RX timeout period.
	    WORCTRL = 3;    // WOR_RES = 3.  Helps determine the units of the RX timeout period.
		WOREVT1 = timeout >> 8;
		WOREVT0 = timeout & 0xFF;
	}
	else
	{
		MCSM2 = 0x07;  // RX_TIME = 7: No timeout.
	}

	dmaConfig.radio.SRCADDRH = XDATA_SFR_ADDRESS(RFD) >> 8;
	dmaConfig.radio.SRCADDRL = XDATA_SFR_ADDRESS(RFD);
	dmaConfig.radio.DESTADDRH = (unsigned int)packet >> 8;
	dmaConfig.radio.DESTADDRL = (unsigned int)packet;
	dmaConfig.radio.LENL = 1 + PKTLEN + 2;
	dmaConfig.radio.VLEN_LENH = 0b10000000; // Transfer length is FirstByte+3
	// Assumption: DC6 is set correctly
	dmaConfig.radio.DC7 = 0x10; // SRCINC = 0, DESTINC = 1, IRQMASK = 0, M8 = 0, PRIORITY = 0

	radioMacState = RADIO_MAC_STATE_RX;
}

// Called by the user during RADIO_MAC_STATE_IDLE or RADIO_MAC_STATE_RX to tell the Mac
// that it should start trying to send a packet.
void radioMacTx(uint8 XDATA * packet)
{
	dmaConfig.radio.SRCADDRH = (unsigned int)packet >> 8;
	dmaConfig.radio.SRCADDRL = (unsigned int)packet;
	dmaConfig.radio.DESTADDRH = XDATA_SFR_ADDRESS(RFD) >> 8;
	dmaConfig.radio.DESTADDRL = XDATA_SFR_ADDRESS(RFD);
	dmaConfig.radio.LENL = 1 + PKTLEN;
	dmaConfig.radio.VLEN_LENH = 0b00100000; // Transfer length is FirstByte+1
	// Assumption: DC6 is set correctly
	dmaConfig.radio.DC7 = 0x40; // SRCINC = 1, DESTINC = 0, IRQMASK = 0, M8 = 0, PRIORITY = 0

    radioMacState = RADIO_MAC_STATE_TX;
}

// Local Variables: **
// mode: C **
// c-basic-offset: 4 **
// tab-width: 4 **
// indent-tabs-mode: nil **
// end: **
