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
#include <stdlib.h>  // abs()

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#include "bitmaps.h"
#include "board.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/ui.h"
center_line_t center_line = CENTER_LINE_NONE;

// ***************************************************************************

static void DrawSmallAntennaAndBars(uint8_t *p, unsigned int level)
{
	if(level>6)
		level = 6;

	//memcpy(p, BITMAP_Antenna, ARRAY_SIZE(BITMAP_Antenna));

	for(uint8_t i = 1; i <= level; i++) {
		char bar = (0xff << (6-i)) & 0x7F;
		memset(p + 2 + i*3, bar, 2);
	}
}
#if defined ENABLE_AUDIO_BAR || defined ENABLE_RSSI_BAR

static void DrawLevelBar(uint8_t xpos, uint8_t line, uint8_t level)
{
	const char hollowBar[] = {
		0b01111111,
		0b01000001,
		0b01000001,
		0b01111111
	};

	uint8_t *p_line = gFrameBuffer[line];
	level = MIN(level, 13);

	for(uint8_t i = 0; i < level; i++) {
		if(i < 9) {
			for(uint8_t j = 0; j < 4; j++)
				p_line[xpos + i * 5 + j] = (~(0x7F >> (i+1))) & 0x7F;
		}
		else {
			memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
		}
	}
}
#endif

#ifdef ENABLE_AUDIO_BAR

unsigned int sqrt16(unsigned int value)
{	// return square root of 'value'
	unsigned int shift = 16;         // number of bits supplied in 'value' .. 2 ~ 32
	unsigned int bit   = 1u << --shift;
	unsigned int sqrti = 0;
	while (bit)
	{
		const unsigned int temp = ((sqrti << 1) | bit) << shift--;
		if (value >= temp) {
			value -= temp;
			sqrti |= bit;
		}
		bit >>= 1;
	}
	return sqrti;
}

void UI_DisplayAudioBar(void)
{
	if(gLowBattery && !gLowBatteryConfirmed)
		return;

	const unsigned int line      = 3;

	if (gCurrentFunction != FUNCTION_TRANSMIT ||
		gScreenToDisplay != DISPLAY_MAIN
	#ifdef ENABLE_DTMF
		|| gDTMF_CallState != DTMF_CALL_STATE_NONE
	#endif
		)
	{
		return;  // screen is in use
	}
			
	#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
		if (gAlarmState != ALARM_STATE_OFF)
			return;
	#endif
	const unsigned int voice_amp  = BK4819_GetVoiceAmplitudeOut();  // 15:0

	// make non-linear to make more sensitive at low values
	const unsigned int level      = MIN(voice_amp * 8, 65535u);
	const unsigned int sqrt_level = MIN(sqrt16(level), 124u);
	uint8_t bars = 13 * sqrt_level / 124;

	uint8_t *p_line = gFrameBuffer[line];
	memset(p_line, 0, LCD_WIDTH);

	DrawLevelBar(62, line, bars);

	if (gCurrentFunction == FUNCTION_TRANSMIT)
		ST7565_BlitFullScreen();

}
#endif


static void DisplayRSSIBar(const int16_t rssi, const bool now)
{
#if defined(ENABLE_RSSI_BAR)

	if (center_line == CENTER_LINE_RSSI) {
		const unsigned int txt_width    = 7 * 8;                 // 8 text chars
		const unsigned int bar_x        = 2 + txt_width + 4;     // X coord of bar graph

		const unsigned int line         = 3;
		uint8_t           *p_line        = gFrameBuffer[line];
		char               str[16];

		const char plus[] = {
			0b00011000,
			0b00011000,
			0b01111110,
			0b01111110,
			0b01111110,
			0b00011000,
			0b00011000,
		};

		if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
			return;     // display is in use

		if (gCurrentFunction == FUNCTION_TRANSMIT ||
			gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF
			|| gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
			)
			return;     // display is in use

		if (now)
			memset(p_line, 0, LCD_WIDTH);

		sLevelAttributes sLevelAtt;
		
		sLevelAtt = GetSLevelAttributes(rssi, gRxVfo->freq_config_RX.Frequency);
		
		uint8_t overS9Bars = MIN(sLevelAtt.over/10, 4);
		
		if(overS9Bars == 0) {
			sprintf(str, "% 4d S%d", sLevelAtt.dBmRssi, sLevelAtt.sLevel); 
		}
		else {
			sprintf(str, "% 4d  %2d", sLevelAtt.dBmRssi, sLevelAtt.over);
			memcpy(p_line + 2 + 7*5, &plus, ARRAY_SIZE(plus));
		}

		UI_PrintStringSmall(str, 2, 0, line);

		DrawLevelBar(bar_x, line, sLevelAtt.sLevel + overS9Bars);
	}
#else

	uint8_t Level;

	if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][3]) {
		Level = 6;
	} else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][2]) {
		Level = 4;
	} else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][1]) {
		Level = 2;
	} else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][0]) {
		Level = 1;
	} else {
		Level = 0;
	}

	uint8_t *pLine = (gEeprom.RX_VFO == 0)? gFrameBuffer[2] : gFrameBuffer[6];
	if (now)
		memset(pLine, 0, 23);
	DrawSmallAntennaAndBars(pLine, Level);
#endif

	if (now)
		ST7565_BlitFullScreen();
}


void UI_UpdateRSSI(const int16_t rssi, const int vfo)
{
	(void)vfo;  // unused
	
	// optional larger RSSI dBm, S-point and bar level

	if (gCurrentFunction == FUNCTION_RECEIVE ||
		gCurrentFunction == FUNCTION_MONITOR ||
		gCurrentFunction == FUNCTION_INCOMING)
	{
		
		DisplayRSSIBar(rssi, true);
	}

}

// ***************************************************************************

void UI_DisplayMain(void)
{
	const unsigned int line0 = 0;  // text screen line
	const unsigned int line1 = 4;
	char               String[22];
	unsigned int       vfo_num;

	center_line = CENTER_LINE_NONE;

	// clear the screen
	memset(gFrameBuffer, 0, sizeof(gFrameBuffer));

	if(gLowBattery && !gLowBatteryConfirmed) {
		UI_DisplayPopup("LOW BATTERY");
		ST7565_BlitFullScreen();
		return;
	}

	if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
	{	// tell user how to unlock the keyboard
		UI_PrintString("Long press #", 0, LCD_WIDTH, 1, 8);
		UI_PrintString("to unlock",    0, LCD_WIDTH, 3, 8);
		ST7565_BlitFullScreen();
		return;
	}
							
	unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;

	for (vfo_num = 0; vfo_num < 2; vfo_num++)
	{
		const unsigned int line       = (vfo_num == 0) ? line0 : line1;
		const bool         isMainVFO   = (vfo_num == gEeprom.TX_VFO);
		uint8_t           *p_line0    = gFrameBuffer[line + 0];
		uint8_t           *p_line1    = gFrameBuffer[line + 1];
		unsigned int       mode       = 0;

		if (activeTxVFO != vfo_num) // this is not active TX VFO
		{
#ifdef ENABLE_SCAN_RANGES
			if(gScanRangeStart) {
					UI_PrintString("ScnRng", 5, 0, line, 8);
					sprintf(String, "%3u.%05u", gScanRangeStart / 100000, gScanRangeStart % 100000);
					UI_PrintStringSmall(String, 56, 0, line);
					sprintf(String, "%3u.%05u", gScanRangeStop / 100000, gScanRangeStop % 100000);
					UI_PrintStringSmall(String, 56, 0, line + 1);
					
				continue;
			}
#endif


			if (
#ifdef ENABLE_DTMF
				gDTMF_CallState != DTMF_CALL_STATE_NONE || gDTMF_IsTx || 
#endif				
				gDTMF_InputMode)
			{	// show DTMF stuff
#ifdef ENABLE_DTMF
				char Contact[16];

				if (!gDTMF_InputMode)
				{

					memset(Contact, 0, sizeof(Contact));
					if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT)
						strcpy(String, (gDTMF_State == DTMF_STATE_CALL_OUT_RSP) ? "CALL OUT(RSP)" : "CALL OUT");
					else
					if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY)
						sprintf(String, "CALL FRM:%s", (DTMF_FindContact(gDTMF_Caller, Contact)) ? Contact : gDTMF_Caller);
					else
					if (gDTMF_IsTx)
						strcpy(String, (gDTMF_State == DTMF_STATE_TX_SUCC) ? "DTMF TX(SUCC)" : "DTMF TX");
				}
				else
#endif				
				{
					sprintf(String, ">%s", gDTMF_InputBox);
				}
				UI_PrintString(String, 2, 0, 0 + (vfo_num * 3), 8);
#ifdef ENABLE_DTMF
				memset(String,  0, sizeof(String));
				if (!gDTMF_InputMode) {
					memset(Contact, 0, sizeof(Contact));
					if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT)
						sprintf(String, ">%s", (DTMF_FindContact(gDTMF_String, Contact)) ? Contact : gDTMF_String);
					else
					if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY)
						sprintf(String, ">%s", (DTMF_FindContact(gDTMF_Callee, Contact)) ? Contact : gDTMF_Callee);
					else
					if (gDTMF_IsTx)
						sprintf(String, ">%s", gDTMF_String);
				}

				UI_PrintString(String, 2, 0, 2 + (vfo_num * 3), 8);
#endif
				center_line = CENTER_LINE_IN_USE;
				continue;
			}

			// highlight the selected/used VFO with a marker
			if (isMainVFO)
				memmove(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
		}
		else // active TX VFO
		{	// highlight the selected/used VFO with a marker
			if (isMainVFO)
				memmove(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
			else
				memmove(p_line0 + 0, BITMAP_VFO_NotDefault, sizeof(BITMAP_VFO_NotDefault));
		}

		if (gCurrentFunction == FUNCTION_TRANSMIT)
		{	// transmitting

#ifdef ENABLE_ALARM
			if (gAlarmState == ALARM_STATE_ALARM)
				mode = 2;
			else
#endif
			{
				if (activeTxVFO == vfo_num)
				{	// show the TX symbol
					mode = 1;
#ifdef ENABLE_SMALL_BOLD
					UI_PrintStringSmallBold("TX", 14, 0, line);
#else
					UI_PrintStringSmall("TX", 14, 0, line);
#endif
				}
			}
		}
		else
		{	// receiving .. show the RX symbol
			mode = 2;
			if ((gCurrentFunction == FUNCTION_RECEIVE ||
			     gCurrentFunction == FUNCTION_MONITOR ||
			     gCurrentFunction == FUNCTION_INCOMING) &&
			     gEeprom.RX_VFO == vfo_num)
			{
#ifdef ENABLE_SMALL_BOLD
				UI_PrintStringSmallBold("RX", 14, 0, line);
#else
				UI_PrintStringSmall("RX", 14, 0, line);
#endif
			}
		}

		if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
		{	// channel mode
			const unsigned int x = 2;
			const bool inputting = (gInputBoxIndex == 0 || gEeprom.TX_VFO != vfo_num) ? false : true;
			if (!inputting)
				sprintf(String, "M%u", gEeprom.ScreenChannel[vfo_num] + 1);
			else
				sprintf(String, "M%.3s", INPUTBOX_GetAscii());  // show the input text
			UI_PrintStringSmall(String, x, 0, line + 1);
		}
		//Robby69 removed
		/*else if (IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
		{	// frequency mode
			// show the frequency band number //Robby69 removed
			const unsigned int x = 2;
			char * buf = gEeprom.VfoInfo[vfo_num].pRX->Frequency < 100000000 ? "" : "+";
			sprintf(String, "F%u%s", 1 + gEeprom.ScreenChannel[vfo_num] - FREQ_CHANNEL_FIRST, buf);
			UI_PrintStringSmall(String, x, 0, line + 1);
		}*/
#ifdef ENABLE_NOAA
		else
		{
			if (gInputBoxIndex == 0 || gEeprom.TX_VFO != vfo_num)
			{	// channel number
				sprintf(String, "N%u", 1 + gEeprom.ScreenChannel[vfo_num] - NOAA_CHANNEL_FIRST);
			}
			else
			{	// user entering channel number
				sprintf(String, "N%u%u", '0' + gInputBox[0], '0' + gInputBox[1]);
			}
			UI_PrintStringSmall(String, 7, 0, line + 1);
		}
#endif

		// ************

		unsigned int state = VfoState[vfo_num];

#ifdef ENABLE_ALARM
		if (gCurrentFunction == FUNCTION_TRANSMIT && gAlarmState == ALARM_STATE_ALARM) {
			if (activeTxVFO == vfo_num)
				state = VFO_STATE_ALARM;
		}
#endif

		uint32_t frequency = gEeprom.VfoInfo[vfo_num].pRX->Frequency;

		if (state != VFO_STATE_NORMAL)
		{
			const char *state_list[] = {"", "BUSY", "BAT LOW", "TX DISABLE", "TIMEOUT", "ALARM", "VOLT HIGH"};
			if (state < ARRAY_SIZE(state_list))
				UI_PrintString(state_list[state], 31, 0, line, 8);
		}
		else if (gInputBoxIndex > 0 && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]) && gEeprom.TX_VFO == vfo_num)
		{	// user entering a frequency
			const char * ascii = INPUTBOX_GetAscii();
			bool isGigaF = frequency>=100000000;
			sprintf(String, "%.*s.%.3s", 3 + isGigaF, ascii, ascii + 3 + isGigaF);
#ifdef ENABLE_BIG_FREQ
			if(!isGigaF) {
				// show the remaining 2 small frequency digits
				UI_PrintStringSmall(String + 7, 113, 0, line + 1);
				String[7] = 0;
				// show the main large frequency digits
				UI_DisplayFrequency(String, 32, line, false);
			}
			else
#endif
			{
				// show the frequency in the main font
				UI_PrintString(String, 32, 0, line, 8);
			}

			continue;
		}
		else
		{
			if (gCurrentFunction == FUNCTION_TRANSMIT)
			{	// transmitting
				if (activeTxVFO == vfo_num)
					frequency = gEeprom.VfoInfo[vfo_num].pTX->Frequency;
			}

			if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
			{	// it's a channel

				// show the scan list assigment symbols
				const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];
				if (att.scanlist > 0) {
					sprintf(String, "%d", att.scanlist);
					UI_PrintStringSmall(String, 113, 0, line + 1);
				}

				// compander symbol
#ifndef ENABLE_BIG_FREQ
				if (att.compander)
					memmove(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#else
				// TODO:  // find somewhere else to put the symbol
#endif

				switch (gEeprom.CHANNEL_DISPLAY_MODE)
				{
					case MDF_FREQUENCY:	// show the channel frequency
						sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
#ifdef ENABLE_BIG_FREQ
						if(frequency < 100000000) {
							// show the remaining 2 small frequency digits
							UI_PrintStringSmall(String + 7, 113, 0, line + 1);
							String[7] = 0;
							// show the main large frequency digits
							UI_DisplayFrequency(String, 32, line, false);
						}
						else
#endif
						{
							// show the frequency in the main font
							UI_PrintString(String, 32, 0, line, 8);
						}

						break;

					case MDF_CHANNEL:	// show the channel number
						sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
						UI_PrintString(String, 32, 0, line, 8);
						break;

					case MDF_NAME:		// show the channel name
					case MDF_NAME_FREQ:	// show the channel name and frequency

						SETTINGS_FetchChannelName(String, gEeprom.ScreenChannel[vfo_num]);
						if (String[0] == 0)
						{	// no channel name, show the channel number instead
							sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
						}

						if (gEeprom.CHANNEL_DISPLAY_MODE == MDF_NAME) {
							UI_PrintString(String, 32, 0, line, 8);
						}
						else {
#ifdef ENABLE_SMALL_BOLD
							UI_PrintStringSmallBold(String, 32 + 4, 0, line);
#else
							UI_PrintStringSmall(String, 32 + 4, 0, line);
#endif
							// show the channel frequency below the channel number/name
							sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
							UI_PrintStringSmall(String, 32 + 4, 0, line + 1);
						}

						break;
				}
			}
			else
			{	// frequency mode
				sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);

#ifdef ENABLE_BIG_FREQ
				if(frequency < 100000000) {
					// show the remaining 2 small frequency digits
					UI_PrintStringSmall(String + 7, 113, 0, line + 1);
					String[7] = 0;
					// show the main large frequency digits
					UI_DisplayFrequency(String, 32, line, false);
				}
				else
#endif
				{
					// show the frequency in the main font
					UI_PrintString(String, 32, 0, line, 8);
				}
			}
		}

		// ************

		{	// show the TX/RX level
			uint8_t Level = 0;

			if (mode == 1)
			{	// TX power level
	
				switch (gRxVfo->OUTPUT_POWER)
				{
					case OUTPUT_POWER_LOW:  Level = 2; break;
					case OUTPUT_POWER_MID:  Level = 4; break;
					case OUTPUT_POWER_HIGH: Level = 6; break;
	  
				}
			}
			else 
			if (mode == 2)
			{	// RX signal level
				#ifndef ENABLE_RSSI_BAR
					// bar graph
					if (gVFO_RSSI_bar_level[vfo_num] > 0)
						Level = gVFO_RSSI_bar_level[vfo_num];
				#endif
			}
			if(Level)
				DrawSmallAntennaAndBars(p_line1 + LCD_WIDTH, Level);
		}
		// ************

		String[0] = '\0';

		// show the modulation symbol
		const char * s = "";
		const ModulationMode_t mod = gEeprom.VfoInfo[vfo_num].Modulation;
		switch (mod){
			case MODULATION_FM: {
				const FREQ_Config_t *pConfig = (mode == 1) ? gEeprom.VfoInfo[vfo_num].pTX : gEeprom.VfoInfo[vfo_num].pRX;
				const unsigned int code_type = pConfig->CodeType;
				const char *code_list[] = {"FM", "CT", "DCS", "DCR"};
				if (code_type < ARRAY_SIZE(code_list))
					s = code_list[code_type];
				break;
			}
			default:
				s = gModulationStr[mod];
			break;
		}		
		UI_PrintStringSmall(s, LCD_WIDTH + 25, 0, line + 1);

		// show the Ptt_Toggle_Mode
		
		if (Ptt_Toggle_Mode) 	UI_PrintStringSmall("T", LCD_WIDTH + 0, 0, line + 1);

		if (state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM)
		{	// show the TX power
			const char pwr_list[] = "LMH";
			const unsigned int i = gEeprom.VfoInfo[vfo_num].OUTPUT_POWER;
			String[0] = (i < ARRAY_SIZE(pwr_list)) ? pwr_list[i] : '\0';
			String[1] = '\0';
			UI_PrintStringSmall(String, LCD_WIDTH + 50, 0, line + 1);
		}

		if (gEeprom.VfoInfo[vfo_num].freq_config_RX.Frequency != gEeprom.VfoInfo[vfo_num].freq_config_TX.Frequency)
		{	// show the TX offset symbol
			const char dir_list[] = "\0+-";
			const unsigned int i = gEeprom.VfoInfo[vfo_num].TX_OFFSET_FREQUENCY_DIRECTION;
			String[0] = (i < sizeof(dir_list)) ? dir_list[i] : '?';
			String[1] = '\0';
			UI_PrintStringSmall(String, LCD_WIDTH + 60, 0, line + 1);
		}

		// show the TX/RX reverse symbol
		if (gEeprom.VfoInfo[vfo_num].FrequencyReverse)
			UI_PrintStringSmall("R", LCD_WIDTH + 62, 0, line + 1);

		{	// show the narrow band symbol
			UI_PrintStringSmall(bwNames[gEeprom.VfoInfo[vfo_num].CHANNEL_BANDWIDTH], LCD_WIDTH + 70, 0, line + 1);
		}

#ifdef ENABLE_DTMF
		// show the DTMF decoding symbol
		if (gEeprom.VfoInfo[vfo_num].DTMF_DECODING_ENABLE || gSetting_KILLED)
			UI_PrintStringSmall("DTMF", LCD_WIDTH + 78, 0, line + 1);
#endif

		// show the audio scramble symbol
		if (gEeprom.VfoInfo[vfo_num].SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
			UI_PrintStringSmall("SCR", LCD_WIDTH + 106, 0, line + 1);
	}

	if (center_line == CENTER_LINE_NONE)
	{	// we're free to use the middle line

		const bool rx = (gCurrentFunction == FUNCTION_RECEIVE ||
		                 gCurrentFunction == FUNCTION_MONITOR ||
		                 gCurrentFunction == FUNCTION_INCOMING);

#ifdef ENABLE_AUDIO_BAR
		if (gCurrentFunction == FUNCTION_TRANSMIT) {
			center_line = CENTER_LINE_AUDIO_BAR;
			UI_DisplayAudioBar();
		}
		//else
#endif

#ifdef ENABLE_RSSI_BAR
		if (rx) {
			center_line = CENTER_LINE_RSSI;
			DisplayRSSIBar(gCurrentRSSI[gEeprom.RX_VFO], false);
		}
		//else
#endif
#ifdef ENABLE_DTMF
		if (rx || gCurrentFunction == FUNCTION_FOREGROUND || gCurrentFunction == FUNCTION_POWER_SAVE)
		{

				if (gSetting_live_DTMF_decoder && gDTMF_RX_live[0] != 0)
				{	// show live DTMF decode
					const unsigned int len = strlen(gDTMF_RX_live);
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars


					if (gScreenToDisplay != DISPLAY_MAIN
						|| gDTMF_CallState != DTMF_CALL_STATE_NONE
						)
						return;
						
					center_line = CENTER_LINE_DTMF_DEC;
					
					strcpy(String, "DTMF ");
					strcat(String, gDTMF_RX_live + idx);
					UI_PrintStringSmall(String, 2, 0, 3);
				}
				if (gSetting_live_DTMF_decoder && gDTMF_RX_index > 0)
				{	// show live DTMF decode
					const unsigned int len = gDTMF_RX_index;
					const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

					if (gScreenToDisplay != DISPLAY_MAIN ||
						gDTMF_CallState != DTMF_CALL_STATE_NONE)
						return;

					center_line = CENTER_LINE_DTMF_DEC;
					
					strcpy(String, "DTMF ");
					strcat(String, gDTMF_RX + idx);
					UI_PrintStringSmall(String, 2, 0, 3);
				}

		}
#endif
	}

	ST7565_BlitFullScreen();
}

// ***************************************************************************
