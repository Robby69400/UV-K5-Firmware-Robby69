/* Original work Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Modified work Copyright 2024 kamilsss655
 * https://github.com/kamilsss655
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#include "app/app.h"
#include "app/chFrScanner.h"
#include "app/common.h"

#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif

#include "app/generic.h"
#include "app/menu.h"
#include "app/scanner.h"
#include "audio.h"
#include "driver/keyboard.h"
#include "dtmf.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"
#include "driver/bk4819.h"

void GENERIC_Key_F(bool bKeyPressed, bool bKeyHeld)
{
	if (gInputBoxIndex > 0)
	{
		if (!bKeyHeld && bKeyPressed) // short pressed
			gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		return;
	}

	if (bKeyHeld || !bKeyPressed) // held or released
	{
		if (bKeyHeld || bKeyPressed) // held or pressed (cannot be held and not pressed I guess, so it checks only if HELD?)
		{
			if (!bKeyHeld) // won't ever pass
				return;

			if (!bKeyPressed) // won't ever pass
				return;

			COMMON_KeypadLockToggle();
		}
		else // released
		{

			if (gScreenToDisplay != DISPLAY_MAIN)
				return;

			gWasFKeyPressed = !gWasFKeyPressed; // toggle F function

			if (gWasFKeyPressed)
				gKeyInputCountdown = key_input_timeout_500ms;

			#ifdef ENABLE_VOICE
				if (!gWasFKeyPressed)
					gAnotherVoiceID = VOICE_ID_CANCEL;
			#endif

			gUpdateStatus = true;
		}
	}
	else // short pressed
	{
		if (gScreenToDisplay != DISPLAY_FM)
		{
			gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
			return;
		}

		gBeepToPlay     = BEEP_440HZ_500MS;

		gPttWasReleased = true;
	}
}

void GENERIC_Key_PTT(bool bKeyPressed)
{
	gInputBoxIndex = 0;

	if (!bKeyPressed || gSerialConfigCountDown_500ms > 0)
	{	// PTT released
		if (gCurrentFunction == FUNCTION_TRANSMIT)
		{	// we are transmitting .. stop

			if (gFlagEndTransmission)
			{
				FUNCTION_Select(FUNCTION_FOREGROUND);
			}
			else
			{
				APP_EndTransmission(true);
				FUNCTION_Select(FUNCTION_FOREGROUND);
			}

			gFlagEndTransmission = false;

			#ifdef ENABLE_VOX
				gVOX_NoiseDetected = false;
			#endif

			RADIO_SetVfoState(VFO_STATE_NORMAL);

			if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
				gRequestDisplayScreen = DISPLAY_MAIN;
		}

		return;
	}

	// PTT pressed

#ifdef ENABLE_SCANNER1
	if (SCANNER_IsScanning())
	{	// CTCSS/CDCSS scanning .. stop
		SCANNER_Stop();
		goto cancel_tx;
	}

	if (gScanStateDir != SCAN_OFF)
	{	// frequency/channel scanning . .stop
		CHFRSCANNER_Stop();
		goto cancel_tx;
	}
#endif
	// set maximum squelch to protect the screen from glitching
	BK4819_SetupSquelch(255, 255, 127, 127, 255, 255);

#ifdef ENABLE_FMRADIO
	if (gFM_ScanState != FM_SCAN_OFF)
	{	// FM radio is scanning .. stop
		FM_Start();
		#ifdef ENABLE_VOICE
			gAnotherVoiceID = VOICE_ID_SCANNING_STOP;
		#endif
		gRequestDisplayScreen = DISPLAY_FM;
		goto cancel_tx;
	}
#endif

#ifdef ENABLE_FMRADIO
	if (gScreenToDisplay == DISPLAY_FM)
		goto start_tx;	// listening to the FM radio .. start TX'ing
#endif

	if (gCurrentFunction == FUNCTION_TRANSMIT)
	{	// already transmitting
		gInputBoxIndex = 0;
		return;
	}

	if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
		gRequestDisplayScreen = DISPLAY_MAIN;


	if (!gDTMF_InputMode && gDTMF_InputBox_Index == 0)
		goto start_tx;	// wasn't entering a DTMF code .. start TX'ing (maybe)

	// was entering a DTMF string

	if (gDTMF_InputBox_Index > 0 || gDTMF_PreviousIndex > 0)
	{	// going to transmit a DTMF string

		if (gDTMF_InputBox_Index == 0 && gDTMF_PreviousIndex > 0)
			gDTMF_InputBox_Index = gDTMF_PreviousIndex;           // use the previous DTMF string

		if (gDTMF_InputBox_Index < sizeof(gDTMF_InputBox))
			gDTMF_InputBox[gDTMF_InputBox_Index] = 0;             // NULL term the string

#ifdef ENABLE_DTMF
		// append our DTMF ID to the inputted DTMF code -
		//  IF the user inputted code is exactly 3 digits long and D-DCD is enabled
		if (gDTMF_InputBox_Index == 3 && gTxVfo->DTMF_DECODING_ENABLE > 0)
			gDTMF_CallMode = DTMF_CheckGroupCall(gDTMF_InputBox, 3);
		else
			gDTMF_CallMode = DTMF_CALL_MODE_DTMF;

		gDTMF_State      = DTMF_STATE_0;
#endif
		// remember the DTMF string
		gDTMF_PreviousIndex = gDTMF_InputBox_Index;
		strcpy(gDTMF_String, gDTMF_InputBox);
		gDTMF_ReplyState = DTMF_REPLY_ANI;
	}

	DTMF_clear_input_box();

start_tx:
	// request start TX
	gFlagPrepareTX = true;
	goto done;
	
cancel_tx:
	if (gPttIsPressed)
	{
		gPttWasPressed = true;
	}

done:	
	gPttDebounceCounter = 0;
	if (gScreenToDisplay != DISPLAY_MENU && gRequestDisplayScreen != DISPLAY_FM)     // 1of11 .. don't close the menu
		gRequestDisplayScreen = DISPLAY_MAIN;
	gUpdateStatus  = true;
	gUpdateDisplay = true;

	//done tx - restore squelch here
	RADIO_ConfigureSquelchAndOutputPower(gCurrentVfo);
}
