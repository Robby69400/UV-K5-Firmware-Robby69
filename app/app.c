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

#include "app/action.h"
#ifdef ENABLE_AIRCOPY
	#include "app/aircopy.h"
#endif
#include "app/app.h"
#include "app/chFrScanner.h"
#include "app/dtmf.h"
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "app/generic.h"
#include "app/main.h"
#include "app/menu.h"
#include "app/scanner.h"
#include "app/uart.h"
#include "ARMCM0.h"
#include "audio.h"
#include "board.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#ifdef ENABLE_FMRADIO
	#include "driver/bk1080.h"
#endif
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "dtmf.h"
#include "external/printf/printf.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "spectrum.h"
#if defined(ENABLE_OVERLAY)
	#include "sram-overlay.h"
#endif
#include "ui/battery.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/status.h"
#include "ui/ui.h"
#include "driver/systick.h"
#ifdef ENABLE_MESSENGER
	#include "app/messenger.h"
#endif
#ifdef ENABLE_ENCRYPTION
	#include "helper/crypto.h"
#endif

#include "driver/eeprom.h"   // EEPROM_ReadBuffer()

#ifdef ENABLE_MESSENGER_NOTIFICATION
	bool gPlayMSGRing = false;
	uint8_t gPlayMSGRingCount = 0;
#endif

bool gCurrentTxState = false;

static void ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
static void FlashlightTimeSlice();

static void UpdateRSSI(const int vfo)
{
	int16_t rssi = BK4819_GetRSSI();

	if (gCurrentRSSI[vfo] == rssi)
		return;     // no change

	gCurrentRSSI[vfo] = rssi;

	UI_UpdateRSSI(rssi, vfo);
}

static void CheckForIncoming(void)
{
	if (!g_SquelchLost)
		return;          // squelch is closed

	// squelch is open

	if (gScanStateDir == SCAN_OFF)
	{	// not RF scanning
		if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF)
		{	// dual watch is disabled

			#ifdef ENABLE_NOAA
				if (gIsNoaaMode)
				{
					gNOAA_Countdown_10ms = NOAA_countdown_3_10ms;
					gScheduleNOAA        = false;
				}
			#endif

			if (gCurrentFunction != FUNCTION_INCOMING)
			{
				FUNCTION_Select(FUNCTION_INCOMING);
				//gUpdateDisplay = true;

				UpdateRSSI(gEeprom.RX_VFO);
				gUpdateRSSI = true;
			}

			return;
		}

		// dual watch is enabled and we're RX'ing a signal

		if (gRxReceptionMode != RX_MODE_NONE)
		{
			if (gCurrentFunction != FUNCTION_INCOMING)
			{
				FUNCTION_Select(FUNCTION_INCOMING);
				//gUpdateDisplay = true;

				UpdateRSSI(gEeprom.RX_VFO);
				gUpdateRSSI = true;
			}
			return;
		}

		gDualWatchCountdown_10ms = dual_watch_count_after_rx_10ms;
		gScheduleDualWatch       = false;

		// let the user see DW is not active
		gDualWatchActive = false;
		gUpdateStatus    = true;
	}
	else
	{	// RF scanning
		if (gRxReceptionMode != RX_MODE_NONE)
		{
			if (gCurrentFunction != FUNCTION_INCOMING)
			{
				FUNCTION_Select(FUNCTION_INCOMING);
				//gUpdateDisplay = true;

				UpdateRSSI(gEeprom.RX_VFO);
				gUpdateRSSI = true;
			}
			return;
		}

		gScanPauseDelayIn_10ms = scan_pause_delay_in_3_10ms;
		gScheduleScanListen    = false;
	}

	gRxReceptionMode = RX_MODE_DETECTED;

	if (gCurrentFunction != FUNCTION_INCOMING)
	{
		FUNCTION_Select(FUNCTION_INCOMING);
		//gUpdateDisplay = true;

		UpdateRSSI(gEeprom.RX_VFO);
		gUpdateRSSI = true;
	}
}

static void HandleIncoming(void)
{
	bool bFlag;

	if (!g_SquelchLost) {	// squelch is closed
#ifdef ENABLE_DTMF
		if (gDTMF_RX_index > 0)
			DTMF_clear_RX();
#endif
		if (gCurrentFunction != FUNCTION_FOREGROUND) {
			FUNCTION_Select(FUNCTION_FOREGROUND);
			gUpdateDisplay = true;
		}
		return;
	}

	bFlag = (gScanStateDir == SCAN_OFF && gCurrentCodeType == CODE_TYPE_OFF);

#ifdef ENABLE_NOAA
	if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gNOAACountdown_10ms > 0) {
		gNOAACountdown_10ms = 0;
		bFlag               = true;
	}
#endif

	if (g_CTCSS_Lost && gCurrentCodeType == CODE_TYPE_CONTINUOUS_TONE) {
		bFlag       = true;
		gFoundCTCSS = false;
	}

	if (g_CDCSS_Lost && gCDCSSCodeType == CDCSS_POSITIVE_CODE && (gCurrentCodeType == CODE_TYPE_DIGITAL || gCurrentCodeType == CODE_TYPE_REVERSE_DIGITAL)) {
		gFoundCDCSS = false;
	}
	else if (!bFlag)
		return;

#ifdef ENABLE_DTMF
	if (gScanStateDir == SCAN_OFF) { // not scanning
		if (gRxVfo->DTMF_DECODING_ENABLE || gSetting_KILLED) { // DTMF DCD is enabled

			DTMF_HandleRequest();

			if (gDTMF_CallState == DTMF_CALL_STATE_NONE) {
				if (gRxReceptionMode == RX_MODE_DETECTED) {
					gDualWatchCountdown_10ms = dual_watch_count_after_1_10ms;
					gScheduleDualWatch       = false;

					gRxReceptionMode = RX_MODE_LISTENING;

					// let the user see DW is not active
					gDualWatchActive = false;
					gUpdateStatus    = true;

					gUpdateDisplay = true;
				}
				return;
			}
		}
	}
#endif

	APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
}

static void HandleReceive(void)
{
	#define END_OF_RX_MODE_SKIP 0
	#define END_OF_RX_MODE_END  1
	#define END_OF_RX_MODE_TTE  2

	uint8_t Mode = END_OF_RX_MODE_SKIP;

	if (gFlagTailNoteEliminationComplete)
	{
		Mode = END_OF_RX_MODE_END;
		goto Skip;
	}

	if (gScanStateDir != SCAN_OFF && IS_FREQ_CHANNEL(gNextMrChannel))
	{ // we are scanning in the frequency mode
		if (g_SquelchLost)
			return;

		Mode = END_OF_RX_MODE_END;
		goto Skip;
	}

	if (gCurrentCodeType != CODE_TYPE_OFF
		&& ((gFoundCTCSS && gFoundCTCSSCountdown_10ms == 0)
			|| (gFoundCDCSS && gFoundCDCSSCountdown_10ms == 0))
	){
		gFoundCTCSS = false;
		gFoundCDCSS = false;
		Mode        = END_OF_RX_MODE_END;
		goto Skip;
	}

	if (g_SquelchLost)
	{
		if (!gEndOfRxDetectedMaybe
#ifdef ENABLE_NOAA
			&& !IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE)
#endif
		){
			switch (gCurrentCodeType)
			{
				case CODE_TYPE_OFF:
					if (gEeprom.SQUELCH_LEVEL)
					{
						if (g_CxCSS_TAIL_Found)
						{
							Mode               = END_OF_RX_MODE_TTE;
							g_CxCSS_TAIL_Found = false;
						}
					}
					break;

				case CODE_TYPE_CONTINUOUS_TONE:
					if (g_CTCSS_Lost)
					{
						gFoundCTCSS = false;
					}
					else
					if (!gFoundCTCSS)
					{
						gFoundCTCSS               = true;
						gFoundCTCSSCountdown_10ms = 100;   // 1 sec
					}

					if (g_CxCSS_TAIL_Found)
					{
						Mode               = END_OF_RX_MODE_TTE;
						g_CxCSS_TAIL_Found = false;
					}
					break;

				case CODE_TYPE_DIGITAL:
				case CODE_TYPE_REVERSE_DIGITAL:
					if (g_CDCSS_Lost && gCDCSSCodeType == CDCSS_POSITIVE_CODE)
					{
						gFoundCDCSS = false;
					}
					else
					if (!gFoundCDCSS)
					{
						gFoundCDCSS               = true;
						gFoundCDCSSCountdown_10ms = 100;   // 1 sec
					}

					if (g_CxCSS_TAIL_Found)
					{
						if (BK4819_GetCTCType() == 1)
							Mode = END_OF_RX_MODE_TTE;

						g_CxCSS_TAIL_Found = false;
					}

					break;
			}
		}
	}
	else
		Mode = END_OF_RX_MODE_END;

	if (!gEndOfRxDetectedMaybe         &&
	     Mode == END_OF_RX_MODE_SKIP   &&
	     gNextTimeslice40ms            &&
	    (gCurrentCodeType == CODE_TYPE_DIGITAL || gCurrentCodeType == CODE_TYPE_REVERSE_DIGITAL) &&
	     BK4819_GetCTCType() == 1)
		Mode = END_OF_RX_MODE_TTE;
	else
		gNextTimeslice40ms = false;

Skip:
	switch (Mode)
	{
		case END_OF_RX_MODE_SKIP:
			break;

		case END_OF_RX_MODE_END:
			RADIO_SetupRegisters(true);

			#ifdef ENABLE_NOAA
				if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE))
					gNOAACountdown_10ms = 300;         // 3 sec
			#endif

			gUpdateDisplay = true;
#ifdef ENABLE_SCANNER1
			if (gScanStateDir != SCAN_OFF)
			{
				switch (gEeprom.SCAN_RESUME_MODE)
				{
					case SCAN_RESUME_TO:
						break;

					case SCAN_RESUME_CO:
						gScanPauseDelayIn_10ms = scan_pause_delay_in_7_10ms;
						gScheduleScanListen    = false;
						break;

					case SCAN_RESUME_SE:
						CHFRSCANNER_Stop();
						break;
				}
			}
#endif
			break;

		case END_OF_RX_MODE_TTE:
			AUDIO_AudioPathOff();

			gTailNoteEliminationCountdown_10ms = 20;
			gFlagTailNoteEliminationComplete   = false;
			gEndOfRxDetectedMaybe = true;
			gEnableSpeaker        = false;
			break;
	}
}

static void HandleFunction(void)
{
	switch (gCurrentFunction)
	{
		case FUNCTION_FOREGROUND:
			CheckForIncoming();
			break;

		case FUNCTION_TRANSMIT:
			break;

		case FUNCTION_MONITOR:
			break;

		case FUNCTION_INCOMING:
			HandleIncoming();
			break;

		case FUNCTION_RECEIVE:
			HandleReceive();
			break;

		case FUNCTION_POWER_SAVE:
			if (!gRxIdleMode)
				CheckForIncoming();
			break;

		case FUNCTION_BAND_SCOPE:
			break;
	}
}

void APP_StartListening(FUNCTION_Type_t Function)
{
	const unsigned int chan = gEeprom.RX_VFO;
//	const unsigned int chan = gRxVfo->CHANNEL_SAVE;

#ifdef ENABLE_DTMF
	if (gSetting_KILLED)
		return;
#endif

#ifdef ENABLE_FMRADIO
	if (gFmRadioMode)
		BK1080_Init(0, false);
#endif

	// clear the other vfo's rssi level (to hide the antenna symbol)
	gVFO_RSSI_bar_level[(chan + 1) & 1u] = 0;

	AUDIO_AudioPathOn();
	gEnableSpeaker = true;

	if (gSetting_backlight_on_tx_rx >= BACKLIGHT_ON_TR_RX)
		BACKLIGHT_TurnOn();
#ifdef ENABLE_SCANNER1
	if (gScanStateDir != SCAN_OFF)
		CHFRSCANNER_Found();
#endif
#ifdef ENABLE_NOAA
	if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gIsNoaaMode) {
		gRxVfo->CHANNEL_SAVE        = gNoaaChannel + NOAA_CHANNEL_FIRST;
		gRxVfo->pRX->Frequency      = NoaaFrequencyTable[gNoaaChannel];
		gRxVfo->pTX->Frequency      = NoaaFrequencyTable[gNoaaChannel];
		gEeprom.ScreenChannel[chan] = gRxVfo->CHANNEL_SAVE;

		gNOAA_Countdown_10ms        = 500;   // 5 sec
		gScheduleNOAA               = false;
	}
#endif

	if (gScanStateDir == SCAN_OFF &&
	    gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
	{	// not scanning, dual watch is enabled

		gDualWatchCountdown_10ms = dual_watch_count_after_2_10ms;
		gScheduleDualWatch       = false;

		// when crossband is active only the main VFO should be used for TX
		if(gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF)
			gRxVfoIsActive = true;

		// let the user see DW is not active
		gDualWatchActive = false;
		gUpdateStatus    = true;
	}

	// AF gain - original QS values
	// if (gRxVfo->Modulation != MODULATION_FM){
	// 	BK4819_WriteRegister(BK4819_REG_48, 0xB3A8);
	// }
	// else 
	{
	BK4819_WriteRegister(BK4819_REG_48,
		(11u << 12)                |     // ??? .. 0 to 15, doesn't seem to make any difference
		( 0u << 10)                |     // AF Rx Gain-1
		(gEeprom.VOLUME_GAIN << 4) |     // AF Rx Gain-2
		(gEeprom.DAC_GAIN    << 0));     // AF DAC Gain (after Gain-1 and Gain-2)
	}

#ifdef ENABLE_VOICE
	if (gVoiceWriteIndex == 0)       // AM/FM RX mode will be set when the voice has finished
#endif
		RADIO_SetModulation(gRxVfo->Modulation);  // no need, set it now

	FUNCTION_Select(Function);

#ifdef ENABLE_FMRADIO
	if (Function == FUNCTION_MONITOR || gFmRadioMode)
#else
	if (Function == FUNCTION_MONITOR)
#endif
	{	// squelch is disabled
		if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
			GUI_SelectNextDisplay(DISPLAY_MAIN);
	}
	else
		gUpdateDisplay = true;

	gUpdateStatus = true;
}

uint32_t APP_SetFreqByStepAndLimits(VFO_Info_t *pInfo, int8_t direction, uint32_t lower, uint32_t upper)
{
	uint32_t Frequency = FREQUENCY_RoundToStep(pInfo->freq_config_RX.Frequency + (direction * pInfo->StepFrequency), pInfo->StepFrequency);

	// Fixes frequency step down when frequency == 0
	if (Frequency == 0 && direction < 0) {
		return upper;
	}

	if (Frequency > upper)
		Frequency =  lower;
	else if (Frequency < lower)
		Frequency =  upper;

	return Frequency;
}

uint32_t APP_SetFrequencyByStep(VFO_Info_t *pInfo, int8_t direction)
{
	return APP_SetFreqByStepAndLimits(pInfo, direction, Band_freq_min(pInfo->Band), frequencyBandTable[pInfo->Band].upper);
}

#ifdef ENABLE_NOAA
	static void NOAA_IncreaseChannel(void)
	{
		if (++gNoaaChannel > 9)
			gNoaaChannel = 0;
	}
#endif

static void DualwatchAlternate(void)
{
	#ifdef ENABLE_NOAA
		if (gIsNoaaMode)
		{
			if (!IS_NOAA_CHANNEL(gEeprom.ScreenChannel[0]) || !IS_NOAA_CHANNEL(gEeprom.ScreenChannel[1]))
				gEeprom.RX_VFO = (gEeprom.RX_VFO + 1) & 1;
			else
				gEeprom.RX_VFO = 0;

			gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];

			if (gEeprom.VfoInfo[0].CHANNEL_SAVE >= NOAA_CHANNEL_FIRST)
				NOAA_IncreaseChannel();
		}
		else
	#endif
	{	// toggle between VFO's
		gEeprom.RX_VFO = !gEeprom.RX_VFO;
		gRxVfo         = &gEeprom.VfoInfo[gEeprom.RX_VFO];

		if (!gDualWatchActive)
		{	// let the user see DW is active
			gDualWatchActive = true;
			gUpdateStatus    = true;
		}
	}

	RADIO_SetupRegisters(false);

	#ifdef ENABLE_NOAA
		gDualWatchCountdown_10ms = gIsNoaaMode ? dual_watch_count_noaa_10ms : dual_watch_count_toggle_10ms;
	#else
		gDualWatchCountdown_10ms = dual_watch_count_toggle_10ms;
	#endif
}

static void CheckRadioInterrupts(void)
{
	if (SCANNER_IsScanning())
		return;

	while (BK4819_ReadRegister(BK4819_REG_0C) & 1u)
	{	// BK chip interrupt request

		uint16_t interrupt_status_bits;

		// reset the interrupt ?
		BK4819_WriteRegister(BK4819_REG_02, 0);

		// fetch the interrupt status bits
		interrupt_status_bits = BK4819_ReadRegister(BK4819_REG_02);

		// 0 = no phase shift
		// 1 = 120deg phase shift
		// 2 = 180deg phase shift
		// 3 = 240deg phase shift
		const uint8_t ctcss_shift = BK4819_GetCTCShift();
		if (ctcss_shift > 0)
			g_CTCSS_Lost = true;

		if (interrupt_status_bits & BK4819_REG_02_DTMF_5TONE_FOUND)
		{	// save the RX'ed DTMF character
			const char c = DTMF_GetCharacter(BK4819_GetDTMF_5TONE_Code());
			if (c != 0xff)
			{
				if (gCurrentFunction != FUNCTION_TRANSMIT)
				{
#ifdef ENABLE_DTMF
					if (gSetting_live_DTMF_decoder)
					{
						size_t len = strlen(gDTMF_RX_live);
						if (len >= (sizeof(gDTMF_RX_live) - 1))
						{	// make room
							memmove(&gDTMF_RX_live[0], &gDTMF_RX_live[1], sizeof(gDTMF_RX_live) - 1);
							len--;
						}
						gDTMF_RX_live[len++]  = c;
						gDTMF_RX_live[len]    = 0;
						gDTMF_RX_live_timeout = DTMF_RX_live_timeout_500ms;  // time till we delete it
						gUpdateDisplay        = true;
					}
#endif

#ifdef ENABLE_DTMF
					if (gRxVfo->DTMF_DECODING_ENABLE || gSetting_KILLED)
					{
						if (gDTMF_RX_index >= (sizeof(gDTMF_RX) - 1))
						{	// make room
							memmove(&gDTMF_RX[0], &gDTMF_RX[1], sizeof(gDTMF_RX) - 1);
							gDTMF_RX_index--;
						}
						gDTMF_RX[gDTMF_RX_index++] = c;
						gDTMF_RX[gDTMF_RX_index]   = 0;
						gDTMF_RX_timeout           = DTMF_RX_timeout_500ms;  // time till we delete it
						gDTMF_RX_pending           = true;

						DTMF_HandleRequest();
					}
#endif
				}
			}
		}

		if (interrupt_status_bits & BK4819_REG_02_CxCSS_TAIL)
			g_CxCSS_TAIL_Found = true;

		if (interrupt_status_bits & BK4819_REG_02_CDCSS_LOST)
		{
			g_CDCSS_Lost = true;
			gCDCSSCodeType = BK4819_GetCDCSSCodeType();
		}

		if (interrupt_status_bits & BK4819_REG_02_CDCSS_FOUND)
			g_CDCSS_Lost = false;

		if (interrupt_status_bits & BK4819_REG_02_CTCSS_LOST)
			g_CTCSS_Lost = true;

		if (interrupt_status_bits & BK4819_REG_02_CTCSS_FOUND)
			g_CTCSS_Lost = false;

		#ifdef ENABLE_VOX
			if (interrupt_status_bits & BK4819_REG_02_VOX_LOST)
			{
				g_VOX_Lost         = true;
				gVoxPauseCountdown = 10;
	
				if (gEeprom.VOX_SWITCH)
				{
					if (gCurrentFunction == FUNCTION_POWER_SAVE && !gRxIdleMode)
					{
						gPowerSave_10ms            = power_save2_10ms;
						gPowerSaveCountdownExpired = 0;
					}
	
					if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF && (gScheduleDualWatch || gDualWatchCountdown_10ms < dual_watch_count_after_vox_10ms))
					{
						gDualWatchCountdown_10ms = dual_watch_count_after_vox_10ms;
						gScheduleDualWatch = false;
	
						// let the user see DW is not active
						gDualWatchActive = false;
						gUpdateStatus    = true;
					}
				}
			}

			if (interrupt_status_bits & BK4819_REG_02_VOX_FOUND)
			{
				g_VOX_Lost         = false;
				gVoxPauseCountdown = 0;
			}
		#endif

		if (interrupt_status_bits & BK4819_REG_02_SQUELCH_LOST)
		{
			g_SquelchLost = true;
			BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
		}

		if (interrupt_status_bits & BK4819_REG_02_SQUELCH_FOUND)
		{
			g_SquelchLost = false;
			BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
		}

		#ifdef ENABLE_AIRCOPY
			if (interrupt_status_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL &&
			    gScreenToDisplay == DISPLAY_AIRCOPY &&
			    gAircopyState == AIRCOPY_TRANSFER &&
			    gAirCopyIsSendMode == 0)
			{
				unsigned int i;
				for (i = 0; i < 4; i++)
					g_FSK_Buffer[gFSKWriteIndex++] = BK4819_ReadRegister(BK4819_REG_5F);
				AIRCOPY_StorePacket();
			}
		#endif

		#ifdef ENABLE_MESSENGER
			MSG_StorePacket(interrupt_status_bits);
		#endif
	}
}

void APP_EndTransmission(bool playRoger)
{	// back to RX mode
	RADIO_SendEndOfTransmission(playRoger);
	// send the CTCSS/DCS tail tone - allows the receivers to mute the usual FM squelch tail/crash
	RADIO_EnableCxCSS();
	RADIO_SetupRegisters(false);

	if (gMonitor)
		gFlagReconfigureVfos = true; //turn the monitor back on
}

#ifdef ENABLE_VOX
	static void HandleVox(void)
	{
#ifdef ENABLE_DTMF
		if (gSetting_KILLED)
			return;
#endif

		if (gVoxResumeCountdown == 0)
		{
			if (gVoxPauseCountdown)
				return;
		}
		else
		{
			g_VOX_Lost         = false;
			gVoxPauseCountdown = 0;
		}
	
		#ifdef ENABLE_FMRADIO
			if (gFmRadioMode)
				return;
		#endif
	
		if (gCurrentFunction == FUNCTION_RECEIVE || gCurrentFunction == FUNCTION_MONITOR)
			return;
	
		if (gScanStateDir != SCAN_OFF)
			return;
	
		if (gVOX_NoiseDetected)
		{
			if (g_VOX_Lost)
				gVoxStopCountdown_10ms = vox_stop_count_down_10ms;
			else
			if (gVoxStopCountdown_10ms == 0)
				gVOX_NoiseDetected = false;
	
			if (gCurrentFunction == FUNCTION_TRANSMIT && !gPttIsPressed && !gVOX_NoiseDetected)
			{
				if (gFlagEndTransmission)
				{
					//if (gCurrentFunction != FUNCTION_FOREGROUND)
						FUNCTION_Select(FUNCTION_FOREGROUND);
				}
				else
				{
					APP_EndTransmission(true);
					FUNCTION_Select(FUNCTION_FOREGROUND);
				}
	
				gUpdateStatus        = true;
				gUpdateDisplay       = true;
				gFlagEndTransmission = false;
			}
			return;
		}
	
		if (g_VOX_Lost)
		{
			gVOX_NoiseDetected = true;
	
			if (gCurrentFunction == FUNCTION_POWER_SAVE)
				FUNCTION_Select(FUNCTION_FOREGROUND);
	
			if (gCurrentFunction != FUNCTION_TRANSMIT && gSerialConfigCountDown_500ms == 0)
			{
#ifdef ENABLE_DTMF
				gDTMF_ReplyState = DTMF_REPLY_NONE;
#endif
				RADIO_PrepareTX();
				gUpdateDisplay = true;
			}
		}
	}
#endif

void APP_Update(void)
{


#ifdef ENABLE_VOICE
	if (gFlagPlayQueuedVoice) {
			AUDIO_PlayQueuedVoice();
			gFlagPlayQueuedVoice = false;
	}
#endif

	if (gCurrentFunction == FUNCTION_TRANSMIT && (gTxTimeoutReached || gSerialConfigCountDown_500ms > 0))
	{	// transmitter timed out or must de-key
		gTxTimeoutReached = false;

		gFlagEndTransmission = true;
		APP_EndTransmission(true);

		AUDIO_PlayBeep(BEEP_880HZ_60MS_TRIPLE_BEEP);

		RADIO_SetVfoState(VFO_STATE_TIMEOUT);

		GUI_DisplayScreen();
	}

	if (gReducedService)
		return;

	if (gCurrentFunction != FUNCTION_TRANSMIT)
		HandleFunction();

#ifdef ENABLE_FMRADIO
//	if (gFmRadioCountdown_500ms > 0)
	if (gFmRadioMode && gFmRadioCountdown_500ms > 0)    // 1of11
		return;
#endif

//Robby69 auto start spectrum 
	uint8_t Spectrum_state = 0; //Spectrum Not Active
  	EEPROM_ReadBuffer(0x1D00, &Spectrum_state, 1);
	if (Spectrum_state >0 && Spectrum_state <10)
		APP_RunSpectrum(Spectrum_state);
	
#ifdef ENABLE_VOICE
	if (!SCANNER_IsScanning() && gScanStateDir != SCAN_OFF && gScheduleScanListen && !gPttIsPressed && gVoiceWriteIndex == 0)
#else
	if (!SCANNER_IsScanning() && gScanStateDir != SCAN_OFF && gScheduleScanListen && !gPttIsPressed)
#endif
#ifdef ENABLE_SCANNER1
	{	// scanning
		CHFRSCANNER_ContinueScanning();
	}
#endif
#ifdef ENABLE_NOAA
#ifdef ENABLE_VOICE
		if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF && gIsNoaaMode && gScheduleNOAA && gVoiceWriteIndex == 0)
#else
		if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF && gIsNoaaMode && gScheduleNOAA)
#endif
		{
			NOAA_IncreaseChannel();
			RADIO_SetupRegisters(false);

			gNOAA_Countdown_10ms = 7;      // 70ms
			gScheduleNOAA        = false;
		}
#endif

	// toggle between the VFO's if dual watch is enabled
	if (!SCANNER_IsScanning() && gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
	{
#ifdef ENABLE_VOICE
		if (gScheduleDualWatch && gVoiceWriteIndex == 0)
#else
		if (gScheduleDualWatch)
#endif
		{
			if (gScanStateDir == SCAN_OFF)
			{
				if (!gPttIsPressed &&
#ifdef ENABLE_FMRADIO
					!gFmRadioMode &&
#endif
#ifdef ENABLE_DTMF
				    gDTMF_CallState == DTMF_CALL_STATE_NONE &&
#endif
				    gCurrentFunction != FUNCTION_POWER_SAVE)
				{
					DualwatchAlternate();    // toggle between the two VFO's

					if (gRxVfoIsActive && gScreenToDisplay == DISPLAY_MAIN)
						GUI_SelectNextDisplay(DISPLAY_MAIN);

					gRxVfoIsActive     = false;
					gScanPauseMode     = false;
					gRxReceptionMode   = RX_MODE_NONE;
					gScheduleDualWatch = false;
				}
			}
		}
	}


#ifdef ENABLE_VOX
	if (gEeprom.VOX_SWITCH)
		HandleVox();
#endif

	if (gSchedulePowerSave)
	{
		if (
#ifdef ENABLE_FMRADIO
			gFmRadioMode                  ||
#endif
			gPttIsPressed                     ||
		    gKeyBeingHeld                     ||
			gEeprom.BATTERY_SAVE == 0         ||
		    gScanStateDir != SCAN_OFF         ||
		    gCssBackgroundScan                      ||
		    gScreenToDisplay != DISPLAY_MAIN  
#ifdef ENABLE_DTMF			
			|| gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
			)
		{
			gBatterySaveCountdown_10ms   = battery_save_count_10ms;
		}
		else 
#ifdef ENABLE_NOAA
		if ((!IS_NOAA_CHANNEL(gEeprom.ScreenChannel[0]) && !IS_NOAA_CHANNEL(gEeprom.ScreenChannel[1])) || !gIsNoaaMode)
#endif
		{
			//if (gCurrentFunction != FUNCTION_POWER_SAVE)
				FUNCTION_Select(FUNCTION_POWER_SAVE);
		}
#ifdef ENABLE_NOAA
		else
		{
			gBatterySaveCountdown_10ms = battery_save_count_10ms;
		}
#else
		gSchedulePowerSave = false;
#endif
	}

#ifdef ENABLE_VOICE
	if (gPowerSaveCountdownExpired && gCurrentFunction == FUNCTION_POWER_SAVE && gVoiceWriteIndex == 0)
#else
	if (gPowerSaveCountdownExpired && gCurrentFunction == FUNCTION_POWER_SAVE)
#endif
	{	// wake up, enable RX then go back to sleep

		if (gRxIdleMode)
		{
			BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();

#ifdef ENABLE_VOX
			if (gEeprom.VOX_SWITCH)
				BK4819_EnableVox(gEeprom.VOX1_THRESHOLD, gEeprom.VOX0_THRESHOLD, gEeprom.VOX_DELAY);
#endif

			if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF &&
			    gScanStateDir == SCAN_OFF &&
			    !gCssBackgroundScan)
			{	// dual watch mode, toggle between the two VFO's
				DualwatchAlternate();

				gUpdateRSSI = false;
			}

			FUNCTION_Init();

			gPowerSave_10ms = power_save1_10ms; // come back here in a bit
			gRxIdleMode     = false;            // RX is awake
		}
		else
		if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF || gScanStateDir != SCAN_OFF || gCssBackgroundScan || gUpdateRSSI)
		{	// dual watch mode off or scanning or rssi update request

			UpdateRSSI(gEeprom.RX_VFO);

			// go back to sleep

			gPowerSave_10ms = gEeprom.BATTERY_SAVE * 10;
			gRxIdleMode     = true;

			BK4819_DisableVox();
			BK4819_Sleep();
			BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

			// Authentic device checked removed

		}
		else
		{
			// toggle between the two VFO's
			DualwatchAlternate();

			gUpdateRSSI       = true;
			gPowerSave_10ms   = power_save1_10ms;
		}

		gPowerSaveCountdownExpired = false;

	}
}

static void gobacktospectrum(void){
	uint8_t Spectrum_state = 0; //Spectrum Not Active
	EEPROM_ReadBuffer(0x1D00, &Spectrum_state, 1);
	if (Spectrum_state >10) //WAS SPECTRUM
		APP_RunSpectrum(Spectrum_state-10);
}

// called every 10ms
static void CheckKeys(void)
{

	if (0 
#ifdef ENABLE_DTMF
	|| gSetting_KILLED 
#endif	
#ifdef ENABLE_AIRCOPY	
	|| (gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState != AIRCOPY_READY)
#endif	
	)
		return;


// -------------------- Toggle PTT ------------------------
//ROBBY69 TOGGLE PTT
// First check if we need to stop transmission due to timeout
if (gCurrentTxState && gTxTimerCountdown_500ms == 0) {
    ProcessKey(KEY_PTT, false, false);  // Turn off TX
    gCurrentTxState = false;
    gPttIsPressed = false;  // Reset PTT state as well
	
}

// Then handle PTT button logic
if (!GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) && gSerialConfigCountDown_500ms == 0)
{   // PTT button is pressed
    if (!gPttIsPressed)
    {   // Only act on the initial press, not while holding
        if (++gPttDebounceCounter >= 3)    // 30ms debounce
        {   
            gPttIsPressed = true;
            gPttDebounceCounter = 0;
            boot_counter_10ms = 0;
		if (Ptt_Toggle_Mode) {
                // Toggle between KEY_PTT on/off
                if (gCurrentTxState) {
                    ProcessKey(KEY_PTT, false, false);  // Turn off TX
                    gCurrentTxState = false;
					
					
                } else {
                    ProcessKey(KEY_PTT, true, false);   // Turn on TX
                    gCurrentTxState = true;
                }
            } else {
                // Standard PTT behavior - transmit while pressed
                if (!gCurrentTxState) {
                    ProcessKey(KEY_PTT, true, false);   // Turn on TX
                    gCurrentTxState = true;
                }
            }
        }
    }
    else
    {
        gPttDebounceCounter = 0;  // Reset if already pressed
    }
}
else
{   // PTT button is released
    if (gPttIsPressed) {
        if (!Ptt_Toggle_Mode && gCurrentTxState) {
            // Only turn off TX if in normal PTT mode and we're transmitting
            ProcessKey(KEY_PTT, false, false);
            gCurrentTxState = false;
			gobacktospectrum();
        }
    }
    gPttIsPressed = false;
    gPttDebounceCounter = 0;
}

// --------------------- OTHER KEYS ----------------------------

	// scan the hardware keys
	KEY_Code_t Key = KEYBOARD_Poll();

	if (Key != KEY_INVALID) // any key pressed
		boot_counter_10ms = 0;   // cancel boot screen/beeps if any key pressed

	if (gKeyReading0 != Key) // new key pressed
	{	

		if (gKeyReading0 != KEY_INVALID && Key != KEY_INVALID)
			ProcessKey(gKeyReading1, false, gKeyBeingHeld);  // key pressed without releasing previous key

		gKeyReading0     = Key;
		gDebounceCounter = 0;
		return;
	}

	gDebounceCounter++;

	if (gDebounceCounter == key_debounce_10ms) // debounced new key pressed
	{	
		if (Key == KEY_INVALID) //all non PTT keys released
		{
			if (gKeyReading1 != KEY_INVALID) // some button was pressed before
			{
				ProcessKey(gKeyReading1, false, gKeyBeingHeld); // process last button released event
				gKeyReading1 = KEY_INVALID;
			}
		}
		else // process new key pressed
		{
			gKeyReading1 = Key;
			ProcessKey(Key, true, false);
		}

		gKeyBeingHeld = false;
		return;
	}

	if (gDebounceCounter < key_repeat_delay_10ms || Key == KEY_INVALID) // the button is not held long enough for repeat yet, or not really pressed
		return;

	if (gDebounceCounter == key_repeat_delay_10ms) //initial key repeat with longer delay
	{	
		if (Key != KEY_PTT)
		{
			gKeyBeingHeld = true;
			ProcessKey(Key, true, true); // key held event
		}
	}
	else //subsequent fast key repeats
	{	
		if (Key == KEY_UP || Key == KEY_DOWN) // fast key repeats for up/down buttons
		{
			gKeyBeingHeld = true;
			if ((gDebounceCounter % key_repeat_10ms) == 0)
				ProcessKey(Key, true, true); // key held event
		}

		if (gDebounceCounter < 0xFFFF)
			return;

		gDebounceCounter = key_repeat_delay_10ms+1;
	}
}

void APP_TimeSlice10ms(void)
{
	gFlashLightBlinkCounter++;

	#ifdef ENABLE_MESSENGER
		keyTickCounter++;
	#endif

	if (UART_IsCommandAvailable())
	{
		__disable_irq();
		UART_HandleCommand();
		__enable_irq();
	}

	if (gReducedService)
		return;

	if (gCurrentFunction != FUNCTION_POWER_SAVE || !gRxIdleMode)
		CheckRadioInterrupts();

	if (gCurrentFunction == FUNCTION_TRANSMIT)
	{	// transmitting
		#ifdef ENABLE_AUDIO_BAR
			if ((gFlashLightBlinkCounter % (150 / 10)) == 0) // once every 150ms
				UI_DisplayAudioBar();
		#endif
	}

	if (gUpdateDisplay)
	{
		gUpdateDisplay = false;
		GUI_DisplayScreen();
	}

	if (gUpdateStatus)
		UI_DisplayStatus();

	// Skipping authentic device checks

	#ifdef ENABLE_FMRADIO
		if (gFmRadioMode && gFmRadioCountdown_500ms > 0)   // 1of11
			return;
	#endif

	FlashlightTimeSlice();

	#ifdef ENABLE_VOX
		if (gVoxResumeCountdown > 0)
			gVoxResumeCountdown--;

		if (gVoxPauseCountdown > 0)
			gVoxPauseCountdown--;
	#endif

	if (gCurrentFunction == FUNCTION_TRANSMIT)
	{
		#ifdef ENABLE_ALARM
			if (gAlarmState == ALARM_STATE_TXALARM || gAlarmState == ALARM_STATE_ALARM)
			{
				uint16_t Tone;

				gAlarmRunningCounter++;
				gAlarmToneCounter++;

				Tone = 500 + (gAlarmToneCounter * 25);
				if (Tone > 1500)
				{
					Tone              = 500;
					gAlarmToneCounter = 0;
				}

				BK4819_SetScrambleFrequencyControlWord(Tone);

				if (gEeprom.ALARM_MODE == ALARM_MODE_TONE && gAlarmRunningCounter == 512)
				{
					gAlarmRunningCounter = 0;

					if (gAlarmState == ALARM_STATE_TXALARM)
					{
						gAlarmState = ALARM_STATE_ALARM;

						RADIO_EnableCxCSS();
						BK4819_SetupPowerAmplifier(0, 0);
						BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
						BK4819_Enable_AfDac_DiscMode_TxDsp();
						BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);

						GUI_DisplayScreen();
					}
					else
					{
						gAlarmState = ALARM_STATE_TXALARM;

						GUI_DisplayScreen();

						BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
						RADIO_SetTxParameters();
						BK4819_TransmitTone(true, 500);
						SYSTEM_DelayMs(2);
						AUDIO_AudioPathOn();

						gEnableSpeaker    = true;
						gAlarmToneCounter = 0;
					}
				}
			}
		#endif
	}

#ifdef ENABLE_FMRADIO
	if (gFmRadioMode && gFM_RestoreCountdown_10ms > 0)
	{
		if (--gFM_RestoreCountdown_10ms == 0)
		{	// switch back to FM radio mode
			FM_Start();
			GUI_SelectNextDisplay(DISPLAY_FM);
		}
	}
#endif

	
	SCANNER_TimeSlice10ms();
	
#ifdef ENABLE_AIRCOPY
	if (gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState == AIRCOPY_TRANSFER && gAirCopyIsSendMode == 1)
	{
		if (gAircopySendCountdown > 0)
		{
			if (--gAircopySendCountdown == 0)
			{
				AIRCOPY_SendMessage();
				GUI_DisplayScreen();
			}
		}
	}
#endif

	CheckKeys();
}

void cancelUserInputModes(void)
{
	if (gDTMF_InputMode || gDTMF_InputBox_Index > 0)
	{
		DTMF_clear_input_box();
		gBeepToPlay           = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		gRequestDisplayScreen = DISPLAY_MAIN;
		gUpdateDisplay        = true;
	}

	if (gWasFKeyPressed || gKeyInputCountdown > 0 || gInputBoxIndex > 0)
	{
		gWasFKeyPressed     = false;
		gInputBoxIndex      = 0;
		gKeyInputCountdown  = 0;
		gBeepToPlay         = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
		gUpdateStatus       = true;
		gUpdateDisplay      = true;
	}
}

// this is called once every 500ms
void APP_TimeSlice500ms(void)
{
	bool exit_menu = false;

	#ifdef ENABLE_MESSENGER_NOTIFICATION
		if (gPlayMSGRing) {
			gPlayMSGRingCount = 5;
			gPlayMSGRing = false;
		}
		if (gPlayMSGRingCount > 0) {
			AUDIO_PlayBeep(BEEP_880HZ_200MS);
			gPlayMSGRingCount--;
		}
	#endif

	#ifdef ENABLE_MESSENGER
		if (hasNewMessage > 0) {
			if (hasNewMessage == 1) {
				hasNewMessage = 2;
			} else if (hasNewMessage == 2) {
				hasNewMessage = 1;
			}
		}	
	#endif

	#ifdef ENABLE_ENCRYPTION
		if(gRecalculateEncKey){
			CRYPTO_Generate256BitKey(gEeprom.ENC_KEY, gEncryptionKey, sizeof(gEeprom.ENC_KEY));
			gRecalculateEncKey = false;
		}
	#endif

	// Skipped authentic device check

	if (gKeypadLocked > 0)
		if (--gKeypadLocked == 0)
			gUpdateDisplay = true;

	if (gKeyInputCountdown > 0)
	{
		if (--gKeyInputCountdown == 0)
		{
			cancelUserInputModes();

			if (gBeepToPlay != BEEP_NONE)
			{
				AUDIO_PlayBeep(gBeepToPlay);
				gBeepToPlay = BEEP_NONE;
			}
		}
	}

	if (gDTMF_RX_live_timeout > 0)
	{
		#ifdef ENABLE_RSSI_BAR
			if (center_line == CENTER_LINE_DTMF_DEC ||
				center_line == CENTER_LINE_NONE)  // wait till the center line is free for us to use before timing out
		#endif
		{
			if (--gDTMF_RX_live_timeout == 0)
			{
				if (gDTMF_RX_live[0] != 0)
				{
					memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));
					gUpdateDisplay   = true;
				}
			}
		}
	}

	if (gMenuCountdown > 0)
		if (--gMenuCountdown == 0)
			exit_menu = (gScreenToDisplay == DISPLAY_MENU);	// exit menu mode

#ifdef ENABLE_DTMF
	if (gDTMF_RX_timeout > 0)
		if (--gDTMF_RX_timeout == 0)
			DTMF_clear_RX();
#endif

	// Skipped authentic device check

	#ifdef ENABLE_FMRADIO
		if (gFmRadioCountdown_500ms > 0)
		{
			gFmRadioCountdown_500ms--;
			if (gFmRadioMode)           // 1of11
				return;
		}
	#endif

	if (gBacklightCountdown > 0 && 
		!gAskToSave && 
		!gCssBackgroundScan &&
		// don't turn off backlight if user is in backlight menu option
		!(gScreenToDisplay == DISPLAY_MENU && (UI_MENU_GetCurrentMenuId() == MENU_ABR || UI_MENU_GetCurrentMenuId() == MENU_ABR_MAX)) 
		) 
	{	if (--gBacklightCountdown == 0)
				if (gEeprom.BACKLIGHT_TIME < (ARRAY_SIZE(gSubMenu_BACKLIGHT) - 1)) // backlight is not set to be always on
					BACKLIGHT_TurnOff();   // turn backlight off
	}

	if (gSerialConfigCountDown_500ms > 0)
	{
	}

	if (gReducedService)
	{
		BOARD_ADC_GetBatteryInfo(&gBatteryCurrentVoltage);

		if (gBatteryCalibration[3] < gBatteryCurrentVoltage)
		{
			#ifdef ENABLE_OVERLAY
				overlay_FLASH_RebootToBootloader();
			#else
				NVIC_SystemReset();
			#endif
		}

		return;
	}

	gBatteryCheckCounter++;

	// Skipped authentic device check

	if (gCurrentFunction != FUNCTION_TRANSMIT)
	{

		if ((gBatteryCheckCounter & 1) == 0)
		{
			BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[gBatteryVoltageIndex++]);
			if (gBatteryVoltageIndex > 3)
				gBatteryVoltageIndex = 0;
			BATTERY_GetReadings(true);
		}
	}
	
	// regular display updates (once every 2 sec) - if need be
	if ((gBatteryCheckCounter & 3) == 0)
	{
		if (gSetting_battery_text > 0)
			gUpdateStatus = true;
	}

	#ifdef ENABLE_FMRADIO
		if (gAskToSave && !gCssBackgroundScan)
	#else
		if (!gCssBackgroundScan)
	#endif
	{
		#ifdef ENABLE_AIRCOPY
			if (gScanStateDir == SCAN_OFF && gScreenToDisplay != DISPLAY_AIRCOPY && !SCANNER_IsScanning())
		#else
			if (gScanStateDir == SCAN_OFF && !SCANNER_IsScanning())
		#endif
		{
			if (gEeprom.AUTO_KEYPAD_LOCK && gKeyLockCountdown > 0 && !gDTMF_InputMode && gScreenToDisplay != DISPLAY_MENU)
			{
				if (--gKeyLockCountdown == 0)
					gEeprom.KEY_LOCK = true;     // lock the keyboard
				gUpdateStatus = true;            // lock symbol needs showing
			}

			if (exit_menu)
			{
				gMenuCountdown = 0;

				if (gEeprom.BACKLIGHT_TIME == 0) // backlight always off
				{
					BACKLIGHT_TurnOff();	// turn the backlight OFF
				}

				if (gInputBoxIndex > 0 || gDTMF_InputMode)
					AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
/*
				if (SCANNER_IsScanning())
				{
					BK4819_StopScan();

					RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD);
					RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD);

					RADIO_SetupRegisters(true);
				}
*/
				DTMF_clear_input_box();

				gWasFKeyPressed  = false;
				gInputBoxIndex   = 0;

				gAskToSave       = false;
				gAskToDelete     = false;

				gUpdateStatus    = true;
				gUpdateDisplay   = true;

				{
					GUI_DisplayType_t disp = DISPLAY_INVALID;

					#ifdef ENABLE_FMRADIO
						if (gFmRadioMode &&
							gCurrentFunction != FUNCTION_RECEIVE &&
							gCurrentFunction != FUNCTION_MONITOR &&
							gCurrentFunction != FUNCTION_TRANSMIT)
						{
							disp = DISPLAY_FM;
						}
					#endif

					if (disp == DISPLAY_INVALID)
					{
#ifndef ENABLE_NO_CODE_SCAN_TIMEOUT
						if (!SCANNER_IsScanning())
#endif
							disp = DISPLAY_MAIN;
					}

					if (disp != DISPLAY_INVALID)
						GUI_SelectNextDisplay(disp);
				}
			}
		}
	}

	if (gCurrentFunction != FUNCTION_POWER_SAVE && gCurrentFunction != FUNCTION_TRANSMIT)
		UpdateRSSI(gEeprom.RX_VFO);

	if (!gPttIsPressed && gVFOStateResumeCountdown_500ms > 0)
	{
		if (--gVFOStateResumeCountdown_500ms == 0)
		{
			RADIO_SetVfoState(VFO_STATE_NORMAL);
#ifdef ENABLE_FMRADIO
			if (gCurrentFunction != FUNCTION_RECEIVE  &&
			    gCurrentFunction != FUNCTION_TRANSMIT &&
			    gCurrentFunction != FUNCTION_MONITOR  &&
				gFmRadioMode)
			{	// switch back to FM radio mode
				FM_Start();
				GUI_SelectNextDisplay(DISPLAY_FM);
			}
#endif				
		}
	}

	BATTERY_TimeSlice500ms();
	SCANNER_TimeSlice500ms();

#ifdef ENABLE_DTMF
	if (gCurrentFunction != FUNCTION_TRANSMIT)
	{
		if (gDTMF_DecodeRingCountdown_500ms > 0)
		{	// make "ring-ring" sound
			gDTMF_DecodeRingCountdown_500ms--;
			AUDIO_PlayBeep(BEEP_880HZ_200MS);
		}
	}
	else
		gDTMF_DecodeRingCountdown_500ms = 0;

	if (gDTMF_CallState  != DTMF_CALL_STATE_NONE &&
	    gCurrentFunction != FUNCTION_TRANSMIT &&
	    gCurrentFunction != FUNCTION_RECEIVE)
	{
		if (gDTMF_auto_reset_time_500ms > 0)
		{
			if (--gDTMF_auto_reset_time_500ms == 0)
			{
				if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED && gEeprom.DTMF_auto_reset_time >= DTMF_HOLD_MAX)
					gDTMF_CallState = DTMF_CALL_STATE_RECEIVED_STAY;     // keep message on-screen till a key is pressed
				else
					gDTMF_CallState = DTMF_CALL_STATE_NONE;
				gUpdateDisplay  = true;
			}
		}
	}

	if (gDTMF_IsTx && gDTMF_TxStopCountdown_500ms > 0)
	{
		if (--gDTMF_TxStopCountdown_500ms == 0)
		{
			gDTMF_IsTx     = false;
			gUpdateDisplay = true;
		}
	}
#endif
}

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
	static void ALARM_Off(void)
	{
		gAlarmState = ALARM_STATE_OFF;

		AUDIO_AudioPathOff();
		gEnableSpeaker = false;

		if (gEeprom.ALARM_MODE == ALARM_MODE_TONE)
		{
			RADIO_SendEndOfTransmission(true);
			RADIO_EnableCxCSS();
		}

		#ifdef ENABLE_VOX
			gVoxResumeCountdown = 80;
		#endif

		SYSTEM_DelayMs(5);

		RADIO_SetupRegisters(true);

		if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
			gRequestDisplayScreen = DISPLAY_MAIN;
	}
#endif



static void ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
	if (Key == KEY_EXIT && !BACKLIGHT_IsOn() && gEeprom.BACKLIGHT_TIME > 0)
	{	// just turn the light on for now so the user can see what's what
		BACKLIGHT_TurnOn();
		gBeepToPlay = BEEP_NONE;
		return;
	}

	if (gCurrentFunction == FUNCTION_POWER_SAVE)
		FUNCTION_Select(FUNCTION_FOREGROUND);

	gBatterySaveCountdown_10ms = battery_save_count_10ms;

	if (gEeprom.AUTO_KEYPAD_LOCK)
		gKeyLockCountdown = 30;     // 15 seconds

	if (!bKeyPressed) // key released
	{
		if (gFlagSaveVfo)
		{
			SETTINGS_SaveVfoIndices();
			gFlagSaveVfo = false;
		}

		if (gFlagSaveSettings)
		{
			SETTINGS_SaveSettings();
			gFlagSaveSettings = false;
		}

		if (gFlagSaveChannel)
		{
			SETTINGS_SaveChannel(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, gTxVfo, gFlagSaveChannel);
			gFlagSaveChannel = false;

			if (!SCANNER_IsScanning() && gVfoConfigureMode == VFO_CONFIGURE_NONE)
				// gVfoConfigureMode is so as we don't wipe out previously setting this variable elsewhere
				gVfoConfigureMode = VFO_CONFIGURE;
		}
	}
	else // key pressed or held
	{
		const uint8_t s = gSetting_backlight_on_tx_rx;
		const int m = UI_MENU_GetCurrentMenuId();
		if 	(	//not when PTT and the backlight shouldn't turn on on TX
				!(Key == KEY_PTT && s != BACKLIGHT_ON_TR_TX && s != BACKLIGHT_ON_TR_TXRX) 
				// not in the backlight menu
				&& !(gScreenToDisplay == DISPLAY_MENU && ( m == MENU_ABR || m == MENU_ABR_MAX || m == MENU_ABR_MIN))
			) 
		{
			BACKLIGHT_TurnOn();
		}

		if (Key == KEY_EXIT && bKeyHeld)
		{	// exit key held pressed

			// clear the live DTMF decoder
			if (gDTMF_RX_live[0] != 0)
			{
				memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));
				gDTMF_RX_live_timeout = 0;
				gUpdateDisplay        = true;
			}

			// cancel user input
			cancelUserInputModes();
			
			if (gMonitor)
				ACTION_Monitor(); //turn off the monitor
#ifdef ENABLE_SCAN_RANGES
			gScanRangeStart = 0;
#endif
		}

		if (gScreenToDisplay == DISPLAY_MENU)       // 1of11
			gMenuCountdown = menu_timeout_500ms;

#ifdef ENABLE_DTMF
		if (gDTMF_DecodeRingCountdown_500ms > 0)
		{	// cancel the ringing
			gDTMF_DecodeRingCountdown_500ms = 0;

			AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);

			if (Key != KEY_PTT)
			{
				gPttWasReleased = true;
				return;
			}
		}
#endif
	}

	bool lowBatPopup = gLowBattery && !gLowBatteryConfirmed &&  gScreenToDisplay == DISPLAY_MAIN;

	if ((gEeprom.KEY_LOCK || lowBatPopup) && gCurrentFunction != FUNCTION_TRANSMIT && Key != KEY_PTT)
	{	// keyboard is locked or low battery popup

		// close low battery popup
		if(Key == KEY_EXIT && bKeyPressed && lowBatPopup) {
			gLowBatteryConfirmed = true;
			gUpdateDisplay = true;
			AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);
			return;
		}		

		if (Key == KEY_F)
		{	// function/key-lock key

			if (!bKeyPressed)
				return;

			if (!bKeyHeld)
			{	// keypad is locked, tell the user
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
				gKeypadLocked  = 4;      // 2 seconds
				gUpdateDisplay = true;
				return;
			}
		}
		// KEY_MENU has a special treatment here, because we want to pass hold event to ACTION_Handle
		// but we don't want it to complain when initial press happens
		// we want to react on realese instead
		else if (Key != KEY_SIDE1 && Key != KEY_SIDE2 &&        // pass side buttons
			     !(Key == KEY_MENU && bKeyHeld)) // pass KEY_MENU held
		{
			if ((!bKeyPressed || bKeyHeld || (Key == KEY_MENU && bKeyPressed)) && // prevent released or held, prevent KEY_MENU pressed
				!(Key == KEY_MENU && !bKeyPressed))  // pass KEY_MENU released
				return;

			// keypad is locked, tell the user
			AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
			gKeypadLocked  = 4;          // 2 seconds
			gUpdateDisplay = true;
			return;
		}
	}

	if (Key <= KEY_9 || Key == KEY_F)
	{
		if (gScanStateDir != SCAN_OFF || gCssBackgroundScan)
		{	// FREQ/CTCSS/DCS scanning
			if (bKeyPressed && !bKeyHeld)
				AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
			return;
		}
	}

	bool bFlag = false;
	if (Key == KEY_PTT)
	{
		if (gPttWasPressed)
		{
			bFlag = bKeyHeld;
			if (!bKeyPressed)
			{
				bFlag          = true;
				gPttWasPressed = false;
			}
		}
	}
	else if (gPttWasReleased)
	{
		if (bKeyHeld)
			bFlag = true;
		if (!bKeyPressed)
		{
			bFlag           = true;
			gPttWasReleased = false;
		}
	}

	if (gWasFKeyPressed && (Key == KEY_PTT || Key == KEY_EXIT || Key == KEY_SIDE1 || Key == KEY_SIDE2))
	{	// cancel the F-key
		gWasFKeyPressed = false;
		gUpdateStatus   = true;
	}

	if (!bFlag)
	{
		if (gCurrentFunction == FUNCTION_TRANSMIT)
		{	// transmitting

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
			if (gAlarmState == ALARM_STATE_OFF)
#endif
			{
				char Code;

				if (Key == KEY_PTT)
				{
					GENERIC_Key_PTT(bKeyPressed);
					goto Skip;
				}

				if (Key == KEY_SIDE2)
				{	// transmit 1750Hz tone
					Code = 0xFE;
				}
				else
				{
					Code = DTMF_GetCharacter(Key - KEY_0);
					if (Code == 0xFF)
						goto Skip;

					// transmit DTMF keys
				}

				if (!bKeyPressed || bKeyHeld)
				{
					if (!bKeyPressed)
					{
						AUDIO_AudioPathOff();

						gEnableSpeaker = false;

						BK4819_ExitDTMF_TX(false);

						if (gCurrentVfo->SCRAMBLING_TYPE == 0 || !gSetting_ScrambleEnable)
							BK4819_DisableScramble();
						else
							BK4819_EnableScramble(gCurrentVfo->SCRAMBLING_TYPE - 1);
					}
				}
				else
				{
					if (gEeprom.DTMF_SIDE_TONE)
					{	// user will here the DTMF tones in speaker
						AUDIO_AudioPathOn();
						gEnableSpeaker = true;
					}

					BK4819_DisableScramble();

					if (Code == 0xFE)
						BK4819_TransmitTone(gEeprom.DTMF_SIDE_TONE, 1750);
					else
						BK4819_PlayDTMFEx(gEeprom.DTMF_SIDE_TONE, Code);
				}
			}
#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
				else
				if ((!bKeyHeld && bKeyPressed) || (gAlarmState == ALARM_STATE_TX1750 && bKeyHeld && !bKeyPressed))
				{
					ALARM_Off();

					FUNCTION_Select(FUNCTION_FOREGROUND);

					if (Key == KEY_PTT)
						gPttWasPressed  = true;
					else
					if (!bKeyHeld)
						gPttWasReleased = true;
				}
#endif
		}
		else
		{
			switch (gScreenToDisplay)
			{
				case DISPLAY_MAIN:
					if ((Key == KEY_SIDE1 || Key == KEY_SIDE2) && !SCANNER_IsScanning())
						{
							ACTION_Handle(Key, bKeyPressed, bKeyHeld);
						}
					else
						MAIN_ProcessKeys(Key, bKeyPressed, bKeyHeld);

					break;
				#ifdef ENABLE_FMRADIO
					case DISPLAY_FM:
						FM_ProcessKeys(Key, bKeyPressed, bKeyHeld);
						break;
				#endif
				case DISPLAY_MENU:
					MENU_ProcessKeys(Key, bKeyPressed, bKeyHeld);
					break;
				
				#ifdef ENABLE_MESSENGER
					case DISPLAY_MSG:
						MSG_ProcessKeys(Key, bKeyPressed, bKeyHeld);
						break;
				#endif

				case DISPLAY_SCANNER:
					SCANNER_ProcessKeys(Key, bKeyPressed, bKeyHeld);
					break;

				#ifdef ENABLE_AIRCOPY
					case DISPLAY_AIRCOPY:
						AIRCOPY_ProcessKeys(Key, bKeyPressed, bKeyHeld);
						break;
				#endif
				case DISPLAY_INVALID:
				default:
					gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
					break;
			}
		}
	}

Skip:
	if (gBeepToPlay != BEEP_NONE)
	{
		AUDIO_PlayBeep(gBeepToPlay);
		gBeepToPlay = BEEP_NONE;
	}

	if (gFlagAcceptSetting)
	{
		gMenuCountdown = menu_timeout_500ms;

		MENU_AcceptSetting();

		gFlagRefreshSetting = true;
		gFlagAcceptSetting  = false;
	}

	if (gRequestSaveSettings)
	{
		if (!bKeyHeld)
			SETTINGS_SaveSettings();
		else
			gFlagSaveSettings = 1;

		gRequestSaveSettings = false;
		gUpdateStatus        = true;
	}

	if (gRequestSaveVFO)
	{
		if (!bKeyHeld)
			SETTINGS_SaveVfoIndices();
		else
			gFlagSaveVfo = true;
		gRequestSaveVFO = false;
	}

	if (gRequestSaveChannel > 0)
	{
		if (!bKeyHeld)
		{
			SETTINGS_SaveChannel(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, gTxVfo, gRequestSaveChannel);

			if (!SCANNER_IsScanning())
				if (gVfoConfigureMode == VFO_CONFIGURE_NONE)  // 'if' is so as we don't wipe out previously setting this variable elsewhere
					gVfoConfigureMode = VFO_CONFIGURE;
		}
		else
		{
			gFlagSaveChannel = gRequestSaveChannel;

			if (gRequestDisplayScreen == DISPLAY_INVALID)
				gRequestDisplayScreen = DISPLAY_MAIN;
		}

		gRequestSaveChannel = 0;
	}

	if (gVfoConfigureMode != VFO_CONFIGURE_NONE)
	{
		if (gFlagResetVfos)
		{
			RADIO_ConfigureChannel(0, gVfoConfigureMode);
			RADIO_ConfigureChannel(1, gVfoConfigureMode);
		}
		else
			RADIO_ConfigureChannel(gEeprom.TX_VFO, gVfoConfigureMode);

		if (gRequestDisplayScreen == DISPLAY_INVALID)
			gRequestDisplayScreen = DISPLAY_MAIN;

		gFlagReconfigureVfos = true;
		gVfoConfigureMode    = VFO_CONFIGURE_NONE;
		gFlagResetVfos       = false;
	}

	if (gFlagReconfigureVfos)
	{
		RADIO_SelectVfos();

#ifdef ENABLE_NOAA
		RADIO_ConfigureNOAA();
#endif

		RADIO_SetupRegisters(true);

#ifdef ENABLE_DTMF
		gDTMF_auto_reset_time_500ms = 0;
		gDTMF_CallState             = DTMF_CALL_STATE_NONE;
		gDTMF_TxStopCountdown_500ms = 0;
		gDTMF_IsTx                  = false;
#endif

		gVFO_RSSI_bar_level[0]      = 0;
		gVFO_RSSI_bar_level[1]      = 0;

		gFlagReconfigureVfos        = false;

		if (gMonitor)
			ACTION_Monitor();   // 1of11
	}

	if (gFlagRefreshSetting)
	{
		gFlagRefreshSetting = false;
		gMenuCountdown      = menu_timeout_500ms;

		MENU_ShowCurrentSetting();
	}

	if (gFlagPrepareTX)
	{
		RADIO_PrepareTX();
		gFlagPrepareTX = false;
	}

	#ifdef ENABLE_VOICE
		if (gAnotherVoiceID != VOICE_ID_INVALID)
		{
			if (gAnotherVoiceID < 76)
				AUDIO_SetVoiceID(0, gAnotherVoiceID);
			AUDIO_PlaySingleVoice(false);
			gAnotherVoiceID = VOICE_ID_INVALID;
		}
	#endif

	GUI_SelectNextDisplay(gRequestDisplayScreen);
	gRequestDisplayScreen = DISPLAY_INVALID;

	gUpdateDisplay = true;
}

static void FlashlightTimeSlice()
{
		if (gFlashLightState == FLASHLIGHT_BLINK && (gFlashLightBlinkCounter & 15u) == 0)
		GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
	else if(gFlashLightState == FLASHLIGHT_SOS) {
		const uint16_t u = 15;
		static uint8_t c;
		static uint16_t next;

		if(gFlashLightBlinkCounter - next > 7*u) {
			c = 0;
			next = gFlashLightBlinkCounter + 1;
		}
		else if(gFlashLightBlinkCounter == next) {
			if(c==0) {
				GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
			}
			else
				GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);

			if(c >= 18) {
				next = gFlashLightBlinkCounter + 7*u;
				c = 0;
			}
			else if(c==7 || c==9 || c==11)
			 	next = gFlashLightBlinkCounter + 3*u;
			else
				next = gFlashLightBlinkCounter + u;

			c++;
		}
	}
}