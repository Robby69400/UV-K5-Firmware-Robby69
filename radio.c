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

#include "app/dtmf.h"
#ifdef ENABLE_FMRADIO
	#include "app/fm.h"
#endif
#include "audio.h"
#include "bsp/dp32g030/gpio.h"
#include "dcs.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/system.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/menu.h"
#include "board.h"
#ifdef ENABLE_MESSENGER
	#include "app/messenger.h"
#endif

VFO_Info_t    *gTxVfo;
VFO_Info_t    *gRxVfo;
VFO_Info_t    *gCurrentVfo;
DCS_CodeType_t gCurrentCodeType;
VfoState_t     VfoState[2];
bool           gMuteMic;

const char gModulationStr[][4] =
{
	"FM",
	"AM",
	"USB",
	
#ifdef ENABLE_BYP_RAW_DEMODULATORS
	"BYP",
	"RAW"
#endif
};

const char *bwNames[5] = {"25k", "12.5k", "8.33k", "6.25k", "5k"};

bool RADIO_CheckValidChannel(uint16_t Channel, bool bCheckScanList, uint8_t VFO)
{	// return true if the channel appears valid

	ChannelAttributes_t att;

	if (!IS_MR_CHANNEL(Channel))
		return false;

	att = gMR_ChannelAttributes[Channel];

	if (att.band > BAND7_470MHz)
		return false;

	if (bCheckScanList) {
		if (att.scanlist == VFO+1)
			return true;
		else
			return false;
	}

	return true;
}

uint8_t RADIO_FindNextChannel(uint8_t Channel, int8_t Direction, bool bCheckScanList, uint8_t VFO)
{
	unsigned int i;
		
	for (i = 0; IS_MR_CHANNEL(i); i++)
	{
		if (Channel == 0xFF)
			Channel = MR_CHANNEL_LAST;
		else
		if (!IS_MR_CHANNEL(Channel))
			Channel = MR_CHANNEL_FIRST;

		if (RADIO_CheckValidChannel(Channel, bCheckScanList, VFO))
			return Channel;

		Channel += Direction;
	}
	
	return 0xFF;
}

void RADIO_InitInfo(VFO_Info_t *pInfo, const uint8_t ChannelSave, const uint32_t Frequency)
{
	memset(pInfo, 0, sizeof(*pInfo));

	pInfo->Band                     = FREQUENCY_GetBand(Frequency);
	pInfo->SCANLIST  				= 0;
	pInfo->STEP_SETTING             = STEP_12_5kHz;
	pInfo->StepFrequency            = gStepFrequencyTable[pInfo->STEP_SETTING];
	pInfo->CHANNEL_SAVE             = ChannelSave;
	pInfo->FrequencyReverse         = false;
	pInfo->OUTPUT_POWER             = OUTPUT_POWER_LOW;
	pInfo->freq_config_RX.Frequency = Frequency;
	pInfo->freq_config_TX.Frequency = Frequency;
	pInfo->pRX                      = &pInfo->freq_config_RX;
	pInfo->pTX                      = &pInfo->freq_config_TX;
	pInfo->Compander                = 0;  // off

	if (ChannelSave == (FREQ_CHANNEL_FIRST + BAND2_108MHz))
		pInfo->Modulation = MODULATION_AM;
	else
		pInfo->Modulation = MODULATION_FM;
		
	RADIO_ConfigureSquelchAndOutputPower(pInfo);
}

void RADIO_ConfigureChannel(const unsigned int VFO, const unsigned int configure)
{
	VFO_Info_t *pVfo = &gEeprom.VfoInfo[VFO];
	uint8_t channel = gEeprom.ScreenChannel[VFO];

	if (IS_VALID_CHANNEL(channel)) {
#ifdef ENABLE_NOAA
		if (channel >= NOAA_CHANNEL_FIRST)
		{
			RADIO_InitInfo(pVfo, gEeprom.ScreenChannel[VFO], NoaaFrequencyTable[channel - NOAA_CHANNEL_FIRST]);

			if (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF)
				return;

			gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;

			gUpdateStatus = true;
			return;
		}
#endif

		if (IS_MR_CHANNEL(channel)) {
			channel = RADIO_FindNextChannel(channel, RADIO_CHANNEL_UP, false, VFO);
			if (channel == 0xFF) {
				channel                    = gEeprom.FreqChannel[VFO];
				gEeprom.ScreenChannel[VFO] = gEeprom.FreqChannel[VFO];
			}
			else {
				gEeprom.ScreenChannel[VFO] = channel;
				gEeprom.MrChannel[VFO]     = channel;
			}
		}
	}
	else
		channel = FREQ_CHANNEL_LAST - 1;

	ChannelAttributes_t att = gMR_ChannelAttributes[channel];
	if (att.__val == 0xFF) { // invalid/unused channel
		if (IS_MR_CHANNEL(channel)) {
			channel                    = gEeprom.FreqChannel[VFO];
			gEeprom.ScreenChannel[VFO] = channel;
		}

		uint8_t bandIdx = channel - FREQ_CHANNEL_FIRST;
		RADIO_InitInfo(pVfo, channel, frequencyBandTable[bandIdx].lower);
		return;
	}

	uint8_t band = att.band;
	if (band > BAND7_470MHz) {
		band = BAND6_400MHz;
	}

	if (!IS_MR_CHANNEL(channel)) {
		band = channel - FREQ_CHANNEL_FIRST;
	}

	pVfo->Band                    = band;
	pVfo->SCANLIST 				  = att.scanlist;
	pVfo->CHANNEL_SAVE            = channel;

	uint16_t base;
	if (IS_MR_CHANNEL(channel))
		base = channel * 16;
	else
		base = 0x0C80 + ((channel - FREQ_CHANNEL_FIRST) * 32) + (VFO * 16);

	if (configure == VFO_CONFIGURE_RELOAD || IS_FREQ_CHANNEL(channel))
	{
		uint8_t tmp;
		uint8_t data[8];

		// ***************

		EEPROM_ReadBuffer(base + 8, data, sizeof(data));

		tmp = data[3] & 0x0F;
		if (tmp > TX_OFFSET_FREQUENCY_DIRECTION_SUB)
			tmp = 0;
		pVfo->TX_OFFSET_FREQUENCY_DIRECTION = tmp;
		tmp = data[3] >> 4;
		if (tmp >= MODULATION_UKNOWN)
			tmp = MODULATION_FM;		
		pVfo->Modulation = tmp;

		tmp = data[6];
		if (tmp >= ARRAY_SIZE(gStepFrequencyTable))
			tmp = STEP_12_5kHz;
		pVfo->STEP_SETTING  = tmp;
		pVfo->StepFrequency = gStepFrequencyTable[tmp];

		tmp = data[7];
		if (tmp > (ARRAY_SIZE(gSubMenu_SCRAMBLER) - 1))
			tmp = 0;
		pVfo->SCRAMBLING_TYPE = tmp;

		pVfo->freq_config_RX.CodeType = (data[2] >> 0) & 0x0F;
		pVfo->freq_config_TX.CodeType = (data[2] >> 4) & 0x0F;

		tmp = data[0];
		switch (pVfo->freq_config_RX.CodeType)
		{
			default:
			case CODE_TYPE_OFF:
				pVfo->freq_config_RX.CodeType = CODE_TYPE_OFF;
				tmp = 0;
				break;

			case CODE_TYPE_CONTINUOUS_TONE:
				if (tmp > (ARRAY_SIZE(CTCSS_Options) - 1))
					tmp = 0;
				break;

			case CODE_TYPE_DIGITAL:
			case CODE_TYPE_REVERSE_DIGITAL:
				if (tmp > (ARRAY_SIZE(DCS_Options) - 1))
					tmp = 0;
				break;
		}
		pVfo->freq_config_RX.Code = tmp;

		tmp = data[1];
		switch (pVfo->freq_config_TX.CodeType)
		{
			default:
			case CODE_TYPE_OFF:
				pVfo->freq_config_TX.CodeType = CODE_TYPE_OFF;
				tmp = 0;
				break;

			case CODE_TYPE_CONTINUOUS_TONE:
				if (tmp > (ARRAY_SIZE(CTCSS_Options) - 1))
					tmp = 0;
				break;

			case CODE_TYPE_DIGITAL:
			case CODE_TYPE_REVERSE_DIGITAL:
				if (tmp > (ARRAY_SIZE(DCS_Options) - 1))
					tmp = 0;
				break;
		}
		pVfo->freq_config_TX.Code = tmp;

		if (data[4] == 0xFF)
		{
			pVfo->FrequencyReverse  = false;
			pVfo->CHANNEL_BANDWIDTH = BK4819_FILTER_BW_WIDE;
			pVfo->OUTPUT_POWER      = OUTPUT_POWER_LOW;
			pVfo->BUSY_CHANNEL_LOCK = false;
		}
		else
		{
			const uint8_t d4 = data[4];
			pVfo->FrequencyReverse  = !!((d4 >> 0) & 1u);
			pVfo->CHANNEL_BANDWIDTH = !!((d4 >> 1) & 1u);
			pVfo->OUTPUT_POWER      =   ((d4 >> 2) & 3u);
			pVfo->BUSY_CHANNEL_LOCK = !!((d4 >> 4) & 1u);
			if(pVfo->CHANNEL_BANDWIDTH != BK4819_FILTER_BW_WIDE)
				pVfo->CHANNEL_BANDWIDTH = ((d4 >> 5) & 3u) + 1;
		}	

		if (data[5] == 0xFF)
		{
#ifdef ENABLE_DTMF
			pVfo->DTMF_DECODING_ENABLE = false;
#endif
			pVfo->DTMF_PTT_ID_TX_MODE  = PTT_ID_OFF;
		}
		else
		{
#ifdef ENABLE_DTMF
			pVfo->DTMF_DECODING_ENABLE = ((data[5] >> 0) & 1u) ? true : false;
#endif
			pVfo->DTMF_PTT_ID_TX_MODE  = ((data[5] >> 1) & 7u);
		}

		// ***************

		struct {
			uint32_t Frequency;
			uint32_t Offset;
		} __attribute__((packed)) info;
		EEPROM_ReadBuffer(base, &info, sizeof(info));
		if(info.Frequency==0xFFFFFFFF)
			pVfo->freq_config_RX.Frequency = frequencyBandTable[band].lower;
		else
			pVfo->freq_config_RX.Frequency = info.Frequency;

		if (info.Offset >= 100000000)
			info.Offset = 1000000;
		pVfo->TX_OFFSET_FREQUENCY = info.Offset;

		// ***************
	}

	uint32_t frequency = pVfo->freq_config_RX.Frequency;

	// fix previously set incorrect band
	band = FREQUENCY_GetBand(frequency);

	if (frequency < Band_freq_min(band))
		frequency = Band_freq_min(band);
	else if (frequency > frequencyBandTable[band].upper)
		frequency = frequencyBandTable[band].upper;
	else if (channel >= FREQ_CHANNEL_FIRST)
		frequency = FREQUENCY_RoundToStep(frequency, pVfo->StepFrequency);

	pVfo->freq_config_RX.Frequency = frequency;

	if (frequency >= frequencyBandTable[BAND2_108MHz].upper && frequency < frequencyBandTable[BAND2_108MHz].upper)
		pVfo->TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
	else if (!IS_MR_CHANNEL(channel))
		pVfo->TX_OFFSET_FREQUENCY = FREQUENCY_RoundToStep(pVfo->TX_OFFSET_FREQUENCY, pVfo->StepFrequency);

	RADIO_ApplyTxOffset(pVfo);

	if (IS_MR_CHANNEL(channel))
	{	// 16 bytes allocated to the channel name but only 10 used, the rest are 0's
		SETTINGS_FetchChannelName(pVfo->Name, channel);
	}

	if (!pVfo->FrequencyReverse)
	{
		pVfo->pRX = &pVfo->freq_config_RX;
		pVfo->pTX = &pVfo->freq_config_TX;
	}
	else
	{
		pVfo->pRX = &pVfo->freq_config_TX;
		pVfo->pTX = &pVfo->freq_config_RX;
	}

	if (pVfo->Modulation != MODULATION_FM)
	{	// freq/chan is in AM mode
		pVfo->SCRAMBLING_TYPE         = 0;
//		pVfo->DTMF_DECODING_ENABLE    = false;  // no reason to disable DTMF decoding, aircraft use it on SSB
		pVfo->freq_config_RX.CodeType = CODE_TYPE_OFF;
		pVfo->freq_config_TX.CodeType = CODE_TYPE_OFF;
	}

	BK4819_InitAGC(gEeprom.RX_AGC, gTxVfo->Modulation);
	BK4819_SetAGC(1);
	//Robby69
	//BK4819_SetAGC(gEeprom.RX_AGC!=RX_AGC_OFF);

	RADIO_ConfigureSquelchAndOutputPower(pVfo);
}

void RADIO_ConfigureSquelchAndOutputPower(VFO_Info_t *pInfo)
{
	uint8_t          Txp[3];
	FREQUENCY_Band_t Band;

	// *******************************
	// squelch
	
	Band = FREQUENCY_GetBand(pInfo->pRX->Frequency);
	uint16_t Base = (Band < BAND4_174MHz) ? 0x1E60 : 0x1E00;

	if (gEeprom.SQUELCH_LEVEL == 0)
	{	// squelch == 0 (off)
		pInfo->SquelchOpenRSSIThresh    = 0;     // 0 ~ 255
		pInfo->SquelchOpenNoiseThresh   = 127;   // 127 ~ 0
		pInfo->SquelchCloseGlitchThresh = 255;   // 255 ~ 0

		pInfo->SquelchCloseRSSIThresh   = 0;     // 0 ~ 255
		pInfo->SquelchCloseNoiseThresh  = 127;   // 127 ~ 0
		pInfo->SquelchOpenGlitchThresh  = 255;   // 255 ~ 0
	}
	else
	{	// squelch >= 1
		Base += gEeprom.SQUELCH_LEVEL;                                        // my eeprom squelch-1
																			  // VHF   UHF
		EEPROM_ReadBuffer(Base + 0x00, &pInfo->SquelchOpenRSSIThresh,    1);  //  50    10
		EEPROM_ReadBuffer(Base + 0x10, &pInfo->SquelchCloseRSSIThresh,   1);  //  40     5

		EEPROM_ReadBuffer(Base + 0x20, &pInfo->SquelchOpenNoiseThresh,   1);  //  65    90
		EEPROM_ReadBuffer(Base + 0x30, &pInfo->SquelchCloseNoiseThresh,  1);  //  70   100

		EEPROM_ReadBuffer(Base + 0x40, &pInfo->SquelchCloseGlitchThresh, 1);  //  90    90
		EEPROM_ReadBuffer(Base + 0x50, &pInfo->SquelchOpenGlitchThresh,  1);  // 100   100

		uint16_t rssi_open    = pInfo->SquelchOpenRSSIThresh;
		uint16_t rssi_close   = pInfo->SquelchCloseRSSIThresh;
		uint16_t noise_open   = pInfo->SquelchOpenNoiseThresh;
		uint16_t noise_close  = pInfo->SquelchCloseNoiseThresh;
		uint16_t glitch_open  = pInfo->SquelchOpenGlitchThresh;
		uint16_t glitch_close = pInfo->SquelchCloseGlitchThresh;

		#if ENABLE_SQUELCH_MORE_SENSITIVE
			// make squelch a little more sensitive
			//
			// getting the best setting here is still experimental, bare with me
			//
			// note that 'noise' and 'glitch' values are inverted compared to 'rssi' values

			#if 0
				rssi_open   = (rssi_open   * 8) / 9;
				noise_open  = (noise_open  * 9) / 8;
				glitch_open = (glitch_open * 9) / 8;
			#else
				// even more sensitive .. use when RX bandwidths are fixed (no weak signal auto adjust)
				rssi_open   = (rssi_open   * 1) / 2;
				noise_open  = (noise_open  * 2) / 1;
				glitch_open = (glitch_open * 2) / 1;
			#endif

		#else
			// more sensitive .. use when RX bandwidths are fixed (no weak signal auto adjust)
			rssi_open   = (rssi_open   * 5) / 8;
			noise_open  = (noise_open  * 8) / 5;
			glitch_open = (glitch_open * 8) / 5;
		#endif

		// ensure the 'close' threshold is lower than the 'open' threshold
		if (rssi_close   == rssi_open   && rssi_close   >= 2)
			rssi_close -= 2;
		if (noise_close  == noise_open  && noise_close  <= 125)
			noise_close += 2;
		if (glitch_close == glitch_open && glitch_close <= 253)
			glitch_close += 2;

		pInfo->SquelchOpenRSSIThresh    = (rssi_open    > 255) ? 255 : rssi_open;
		pInfo->SquelchCloseRSSIThresh   = (rssi_close   > 255) ? 255 : rssi_close;
		pInfo->SquelchOpenNoiseThresh   = (noise_open   > 127) ? 127 : noise_open;
		pInfo->SquelchCloseNoiseThresh  = (noise_close  > 127) ? 127 : noise_close;
		pInfo->SquelchOpenGlitchThresh  = (glitch_open  > 255) ? 255 : glitch_open;
		pInfo->SquelchCloseGlitchThresh = (glitch_close > 255) ? 255 : glitch_close;
	}

	// *******************************
	// output power
	
	Band = FREQUENCY_GetBand(pInfo->pTX->Frequency);

	EEPROM_ReadBuffer(0x1ED0 + (Band * 16) + (pInfo->OUTPUT_POWER * 3), Txp, 3);
	const uint8_t p1 = 100;
	const uint8_t p2 = 2;
	const uint8_t p3 = 40;	
	// Robby69 reduced power
	    for(uint8_t p = 0; p < 3; p++)
		{
        if (pInfo->OUTPUT_POWER == OUTPUT_POWER_LOW)Txp[p] /= p1;
		if (pInfo->OUTPUT_POWER == OUTPUT_POWER_MID)Txp[p] /= p2;
		if (pInfo->OUTPUT_POWER == OUTPUT_POWER_HIGH)Txp[p] += p3;
        }

	pInfo->TXP_CalculatedSetting = FREQUENCY_CalculateOutputPower(
		Txp[0],
		Txp[1],
		Txp[2],
		 frequencyBandTable[Band].lower,
		(frequencyBandTable[Band].lower + frequencyBandTable[Band].upper) / 2,
		 frequencyBandTable[Band].upper,
		pInfo->pTX->Frequency);

	
}

void RADIO_ApplyTxOffset(VFO_Info_t *pInfo)
{
	uint32_t Frequency = pInfo->freq_config_RX.Frequency;

	switch (pInfo->TX_OFFSET_FREQUENCY_DIRECTION)
	{
		case TX_OFFSET_FREQUENCY_DIRECTION_OFF:
			break;
		case TX_OFFSET_FREQUENCY_DIRECTION_ADD:
			Frequency += pInfo->TX_OFFSET_FREQUENCY;
			break;
		case TX_OFFSET_FREQUENCY_DIRECTION_SUB:
			Frequency -= pInfo->TX_OFFSET_FREQUENCY;
			break;
	}

	if (Frequency < frequencyBandTable[0].lower)
		Frequency = frequencyBandTable[0].lower;
	else
	if (Frequency > frequencyBandTable[ARRAY_SIZE(frequencyBandTable) - 1].upper)
		Frequency = frequencyBandTable[ARRAY_SIZE(frequencyBandTable) - 1].upper;

	pInfo->freq_config_TX.Frequency = Frequency;
}

static void RADIO_SelectCurrentVfo(void)
{
	// if crossband is active and DW not the gCurrentVfo is gTxVfo (gTxVfo/TX_VFO is only ever changed by the user) 
	// otherwise it is set to gRxVfo which is set to gTxVfo in RADIO_SelectVfos
	// so in the end gCurrentVfo is equal to gTxVfo unless dual watch changes it on incomming transmition (again, this can only happen when XB off)
	// note: it is called only in certain situations so could be not up-to-date
 	gCurrentVfo = (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF || gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) ? gRxVfo : gTxVfo;
}

void RADIO_SelectVfos(void)
{
	// if crossband without DW is used then RX_VFO is the opposite to the TX_VFO
	gEeprom.RX_VFO = (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF || gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) ? gEeprom.TX_VFO : !gEeprom.TX_VFO;

	gTxVfo = &gEeprom.VfoInfo[gEeprom.TX_VFO];
	gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];

	RADIO_SelectCurrentVfo();
}

void RADIO_SetupRegisters(bool switchToForeground)
{
	AUDIO_AudioPathOff();

	gEnableSpeaker = false;

	BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);

	BK4819_FilterBandwidth_t Bandwidth = gRxVfo->CHANNEL_BANDWIDTH;

	BK4819_SetFilterBandwidth(Bandwidth, gRxVfo->Modulation != MODULATION_AM);
	
	BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);

	BK4819_SetupPowerAmplifier(0, 0);

	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);

	while (1)
	{
		const uint16_t Status = BK4819_ReadRegister(BK4819_REG_0C);
		if ((Status & 1u) == 0) // INTERRUPT REQUEST
			break;

		BK4819_WriteRegister(BK4819_REG_02, 0);
		SYSTEM_DelayMs(1);
	}
	BK4819_WriteRegister(BK4819_REG_3F, 0);

	// mic gain 0.5dB/step 0 to 31
	BK4819_WriteRegister(BK4819_REG_7D, 0xE940 | (gEeprom.MIC_SENSITIVITY_TUNING & 0x1f));

	uint32_t Frequency;
	#ifdef ENABLE_NOAA
		if (!IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) || !gIsNoaaMode)
			Frequency = gRxVfo->pRX->Frequency;
		else
			Frequency = NoaaFrequencyTable[gNoaaChannel];
	#else
		Frequency = gRxVfo->pRX->Frequency + gEeprom.RX_OFFSET;
	#endif
	BK4819_SetFrequency(Frequency);

	BK4819_SetupSquelch(
		gRxVfo->SquelchOpenRSSIThresh,    gRxVfo->SquelchCloseRSSIThresh,
		gRxVfo->SquelchOpenNoiseThresh,   gRxVfo->SquelchCloseNoiseThresh,
		gRxVfo->SquelchCloseGlitchThresh, gRxVfo->SquelchOpenGlitchThresh);

	BK4819_PickRXFilterPathBasedOnFrequency(Frequency);

	// what does this in do ?
	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);

	// AF RX Gain and DAC
	//BK4819_WriteRegister(BK4819_REG_48, 0xB3A8);  // 1011 00 111010 1000
	BK4819_WriteRegister(BK4819_REG_48,
		(11u << 12)                 |     // ??? .. 0 ~ 15, doesn't seem to make any difference
		( 0u << 10)                 |     // AF Rx Gain-1
		(gEeprom.VOLUME_GAIN << 4) |     // AF Rx Gain-2
		(gEeprom.DAC_GAIN    << 0));     // AF DAC Gain (after Gain-1 and Gain-2)


	uint16_t InterruptMask = BK4819_REG_3F_SQUELCH_FOUND | BK4819_REG_3F_SQUELCH_LOST;

	#ifdef ENABLE_NOAA
		if (!IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE))
	#endif
	{
		if (gRxVfo->Modulation == MODULATION_FM)
		{	// FM
			uint8_t CodeType = gRxVfo->pRX->CodeType;
			uint8_t Code     = gRxVfo->pRX->Code;
			switch (CodeType)
			{
				default:
				case CODE_TYPE_OFF:
					// this only works as a setup function for REG_51
					BK4819_SetCTCSSFrequency(CTCSS_Options[gEeprom.SQL_TONE]);
					// and REG_07 is overwritten by this function
					BK4819_SetTailDetection(CTCSS_Options[gEeprom.SQL_TONE]);

					InterruptMask = BK4819_REG_3F_CxCSS_TAIL | BK4819_REG_3F_SQUELCH_FOUND | BK4819_REG_3F_SQUELCH_LOST;
					break;

				case CODE_TYPE_CONTINUOUS_TONE:
					BK4819_SetCTCSSFrequency(CTCSS_Options[Code]);

					InterruptMask = 0
						| BK4819_REG_3F_CxCSS_TAIL
						| BK4819_REG_3F_CTCSS_FOUND
						| BK4819_REG_3F_CTCSS_LOST
						| BK4819_REG_3F_SQUELCH_FOUND
						| BK4819_REG_3F_SQUELCH_LOST;

					break;

				case CODE_TYPE_DIGITAL:
				case CODE_TYPE_REVERSE_DIGITAL:
					BK4819_SetCDCSSCodeWord(DCS_GetGolayCodeWord(CodeType, Code));
					InterruptMask = 0
						| BK4819_REG_3F_CxCSS_TAIL
						| BK4819_REG_3F_CDCSS_FOUND
						| BK4819_REG_3F_CDCSS_LOST
						| BK4819_REG_3F_SQUELCH_FOUND
						| BK4819_REG_3F_SQUELCH_LOST;
					break;
			}

			if (gRxVfo->SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
				BK4819_EnableScramble(gRxVfo->SCRAMBLING_TYPE - 1);
			else
				BK4819_DisableScramble();
		}
	}
	#ifdef ENABLE_NOAA
		else
		{
			BK4819_SetCTCSSFrequency(2625);
			InterruptMask = 0
				| BK4819_REG_3F_CTCSS_FOUND
				| BK4819_REG_3F_CTCSS_LOST
				| BK4819_REG_3F_SQUELCH_FOUND
				| BK4819_REG_3F_SQUELCH_LOST;
		}
	#endif

	#ifdef ENABLE_VOX
		#ifdef ENABLE_NOAA
			#ifdef ENABLE_FMRADIO
				if (gEeprom.VOX_SWITCH && !gFmRadioMode && !IS_NOAA_CHANNEL(gCurrentVfo->CHANNEL_SAVE) && gCurrentVfo->Modulation == MODULATION_FM)
			#else
				if (gEeprom.VOX_SWITCH && !IS_NOAA_CHANNEL(gCurrentVfo->CHANNEL_SAVE) && gCurrentVfo->Modulation == MODULATION_FM)
			#endif
		#else
			#ifdef ENABLE_FMRADIO
				if (gEeprom.VOX_SWITCH && !gFmRadioMode && gCurrentVfo->Modulation == MODULATION_FM)
			#else
				if (gEeprom.VOX_SWITCH && gCurrentVfo->Modulation == MODULATION_FM)
			#endif
		#endif
		{
			BK4819_EnableVox(gEeprom.VOX1_THRESHOLD, gEeprom.VOX0_THRESHOLD, gEeprom.VOX_DELAY);
			InterruptMask |= BK4819_REG_3F_VOX_FOUND | BK4819_REG_3F_VOX_LOST;
		}
		else
	#endif
		BK4819_DisableVox();

	// RX expander
	BK4819_SetCompander((gRxVfo->Modulation == MODULATION_FM && gRxVfo->Compander >= 2) ? gRxVfo->Compander : 0);

	#if 0
		if (!gRxVfo->DTMF_DECODING_ENABLE && !gSetting_KILLED)
		{
			BK4819_DisableDTMF();
		}
		else
		{
			BK4819_EnableDTMF();
			InterruptMask |= BK4819_REG_3F_DTMF_5TONE_FOUND;
		}
	#else
		if (gCurrentFunction != FUNCTION_TRANSMIT)
		{
			BK4819_DisableDTMF();
			BK4819_EnableDTMF();
			InterruptMask |= BK4819_REG_3F_DTMF_5TONE_FOUND;
		}
		else
		{
			BK4819_DisableDTMF();
		}
	#endif

	// enable/disable BK4819 selected interrupts

	#ifdef ENABLE_MESSENGER
		if(gEeprom.MESSENGER_CONFIG.data.receive)
		{
			MSG_EnableRX(true);
			InterruptMask |= BK4819_REG_3F_FSK_RX_SYNC | BK4819_REG_3F_FSK_RX_FINISHED | BK4819_REG_3F_FSK_FIFO_ALMOST_FULL | BK4819_REG_3F_FSK_TX_FINISHED;
		}
	#endif	

	BK4819_WriteRegister(BK4819_REG_3F, InterruptMask);

	FUNCTION_Init();

	if (switchToForeground)
		FUNCTION_Select(FUNCTION_FOREGROUND);
}

#ifdef ENABLE_NOAA
	void RADIO_ConfigureNOAA(void)
	{
		uint8_t ChanAB;

		gUpdateStatus = true;

		if (gEeprom.NOAA_AUTO_SCAN)
		{
			if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
			{
				if (!IS_NOAA_CHANNEL(gEeprom.ScreenChannel[0]))
				{
					if (!IS_NOAA_CHANNEL(gEeprom.ScreenChannel[1]))
					{
						gIsNoaaMode = false;
						return;
					}
					ChanAB = 1;
				}
				else
					ChanAB = 0;

				if (!gIsNoaaMode)
					gNoaaChannel = gEeprom.VfoInfo[ChanAB].CHANNEL_SAVE - NOAA_CHANNEL_FIRST;

				gIsNoaaMode = true;
				return;
			}

			if (gRxVfo->CHANNEL_SAVE >= NOAA_CHANNEL_FIRST)
			{
				gIsNoaaMode          = true;
				gNoaaChannel         = gRxVfo->CHANNEL_SAVE - NOAA_CHANNEL_FIRST;
				gNOAA_Countdown_10ms = NOAA_countdown_2_10ms;
				gScheduleNOAA        = false;
			}
			else
				gIsNoaaMode = false;
		}
		else
			gIsNoaaMode = false;
	}
#endif

void RADIO_SetTxParameters(void)
{
	
	AUDIO_AudioPathOff();

	gEnableSpeaker = false;

	BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

	BK4819_FilterBandwidth_t Bandwidth = gCurrentVfo->CHANNEL_BANDWIDTH;

	BK4819_SetFilterBandwidth(Bandwidth, gTxVfo->Modulation != MODULATION_AM);

	BK4819_SetFrequency(gCurrentVfo->pTX->Frequency);

	// TX compressor
	BK4819_SetCompander((gRxVfo->Modulation == MODULATION_FM && (gRxVfo->Compander == 1 || gRxVfo->Compander >= 3)) ? gRxVfo->Compander : 0);

	BK4819_PrepareTransmit(gMuteMic);

	SYSTEM_DelayMs(10);

	BK4819_PickRXFilterPathBasedOnFrequency(gCurrentVfo->pTX->Frequency);

	BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, true);

	SYSTEM_DelayMs(5);

	BK4819_SetupPowerAmplifier(gCurrentVfo->TXP_CalculatedSetting, gCurrentVfo->pTX->Frequency);

	SYSTEM_DelayMs(10);

	switch (gCurrentVfo->pTX->CodeType)
	{
		default:
		case CODE_TYPE_OFF:
			BK4819_ExitSubAu();
			break;

		case CODE_TYPE_CONTINUOUS_TONE:
			BK4819_SetCTCSSFrequency(CTCSS_Options[gCurrentVfo->pTX->Code]);
			break;

		case CODE_TYPE_DIGITAL:
		case CODE_TYPE_REVERSE_DIGITAL:
			BK4819_SetCDCSSCodeWord(DCS_GetGolayCodeWord(gCurrentVfo->pTX->CodeType, gCurrentVfo->pTX->Code));
			break;
	}
}

void RADIO_SetModulation(ModulationMode_t modulation)
{
	BK4819_AF_Type_t mod;
	switch(modulation) {
		default:
		case MODULATION_FM:
			mod = BK4819_AF_FM;
			break;
		case MODULATION_AM:
			mod = BK4819_AF_AM;
			break;
		case MODULATION_USB:
			mod = BK4819_AF_BASEBAND2;
			break;

#ifdef ENABLE_BYP_RAW_DEMODULATORS
		case MODULATION_BYP:
			mod = BK4819_AF_UNKNOWN3;
			break;
		case MODULATION_RAW:
			mod = BK4819_AF_BASEBAND1;
			break;
#endif
	}

	BK4819_SetAF(mod);
	BK4819_SetRegValue(afDacGainRegSpec, 0xF);
	BK4819_WriteRegister(BK4819_REG_3D, modulation == MODULATION_USB ? 0 : 0x2AAB);
	BK4819_SetRegValue(afcDisableRegSpec, modulation != MODULATION_FM);
}

void RADIO_SetVfoState(VfoState_t State)
{
	if (State == VFO_STATE_NORMAL)
	{
		VfoState[0] = VFO_STATE_NORMAL;
		VfoState[1] = VFO_STATE_NORMAL;
		gVFOStateResumeCountdown_500ms = 0;
	}
	else
	{
		if (State == VFO_STATE_VOLTAGE_HIGH)
		{
			VfoState[0] = VFO_STATE_VOLTAGE_HIGH;
			VfoState[1] = VFO_STATE_TX_DISABLE;
		}
		else
		{	// 1of11
			const unsigned int vfo = (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF) ? gEeprom.RX_VFO : gEeprom.TX_VFO;
			VfoState[vfo] = State;
		}

		gVFOStateResumeCountdown_500ms = vfo_state_resume_countdown_500ms;
	}

	gUpdateDisplay = true;
}

VfoState_t RADIO_GetVfoState() {
	return VfoState[(gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF) ? gEeprom.RX_VFO : gEeprom.TX_VFO];
}

void RADIO_PrepareTX(void)
{
	VfoState_t State = VFO_STATE_NORMAL;  // default to OK to TX

	if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
	{	// dual-RX is enabled

		gDualWatchCountdown_10ms = dual_watch_count_after_tx_10ms;
		gScheduleDualWatch       = false;

		if (!gRxVfoIsActive)
		{	// use the current RX vfo
			gEeprom.RX_VFO = gEeprom.TX_VFO;
			gRxVfo         = gTxVfo;
			gRxVfoIsActive = true;
		}

		// let the user see that DW is not active
		gDualWatchActive = false;
		gUpdateStatus    = true;
	}

	RADIO_SelectCurrentVfo();

	#if defined(ENABLE_ALARM) && defined(ENABLE_TX1750)
		if (gAlarmState == ALARM_STATE_OFF    ||
		    gAlarmState == ALARM_STATE_TX1750 ||
		   (gAlarmState == ALARM_STATE_ALARM && gEeprom.ALARM_MODE == ALARM_MODE_TONE))
	#elif defined(ENABLE_ALARM)
		if (gAlarmState == ALARM_STATE_OFF    ||
		   (gAlarmState == ALARM_STATE_ALARM && gEeprom.ALARM_MODE == ALARM_MODE_TONE))
	#elif defined(ENABLE_TX1750)
		if (gAlarmState == ALARM_STATE_OFF    ||
		    gAlarmState == ALARM_STATE_TX1750)
	#endif
	{
		#ifndef ENABLE_TX_WHEN_AM
			if (gCurrentVfo->Modulation != MODULATION_FM)
			{	// not allowed to TX if in AM mode
				State = VFO_STATE_TX_DISABLE;
			}
			else
		#endif
		if (gSerialConfigCountDown_500ms > 0)
		{	// TX is disabled or config upload/download in progress
			State = VFO_STATE_TX_DISABLE;
		}
		else
		if(gEeprom.RX_OFFSET!=0)
		{
			// disable TX when using RX_OFFSET to protect the upconverter
			State = VFO_STATE_TX_DISABLE;
		}
		else
		if (TX_freq_check(gCurrentVfo->pTX->Frequency) == 0)
		{	// TX frequency is allowed
			if (gCurrentVfo->BUSY_CHANNEL_LOCK && gCurrentFunction == FUNCTION_RECEIVE)
				State = VFO_STATE_BUSY;          // busy RX'ing a station
			else
			if (gBatteryDisplayLevel == 0)
				State = VFO_STATE_BAT_LOW;       // charge your battery !
			else
			if (gBatteryDisplayLevel > 6)
				State = VFO_STATE_VOLTAGE_HIGH;  // over voltage .. this is being a pain
		}
		else
			State = VFO_STATE_TX_DISABLE;        // TX frequency not allowed
	}

	if (State != VFO_STATE_NORMAL)
	{	// TX not allowed
		RADIO_SetVfoState(State);

		#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
			gAlarmState = ALARM_STATE_OFF;
		#endif

#ifdef ENABLE_DTMF
		gDTMF_ReplyState = DTMF_REPLY_NONE;
#endif
		AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
		return;
	}

	// TX is allowed

#ifdef ENABLE_DTMF
	if (gDTMF_ReplyState == DTMF_REPLY_ANI)
	{
		if (gDTMF_CallMode == DTMF_CALL_MODE_DTMF)
		{
			gDTMF_IsTx                  = true;
			gDTMF_CallState             = DTMF_CALL_STATE_NONE;
			gDTMF_TxStopCountdown_500ms = DTMF_txstop_countdown_500ms;
		}
		else
		{
			gDTMF_CallState = DTMF_CALL_STATE_CALL_OUT;
			gDTMF_IsTx      = false;
		}
	}
#endif

	FUNCTION_Select(FUNCTION_TRANSMIT);

	gTxTimerCountdown_500ms = 0;            // no timeout

	#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
		if (gAlarmState == ALARM_STATE_OFF)
	#endif
	{
		if (gEeprom.TX_TIMEOUT_TIMER == 0)
			gTxTimerCountdown_500ms = 60;   // 30 sec
		else
		if (gEeprom.TX_TIMEOUT_TIMER < (ARRAY_SIZE(gSubMenu_TOT) - 1))
			gTxTimerCountdown_500ms = 120 * gEeprom.TX_TIMEOUT_TIMER;  // minutes
		else
			gTxTimerCountdown_500ms = 120 * 15;  // 15 minutes
	}
	gTxTimeoutReached    = false;

	gFlagEndTransmission = false;

#ifdef ENABLE_DTMF
	gDTMF_ReplyState     = DTMF_REPLY_NONE;
#endif
}

void RADIO_EnableCxCSS(void)
{
	switch (gCurrentVfo->pTX->CodeType) {
	case CODE_TYPE_DIGITAL:
	case CODE_TYPE_REVERSE_DIGITAL:
		BK4819_EnableCDCSS();
		break;
	default:
		BK4819_EnableCTCSS();
		break;
	}

	SYSTEM_DelayMs(200);
}

void RADIO_PrepareCssTX(void)
{
	RADIO_PrepareTX();

	SYSTEM_DelayMs(200);

	RADIO_EnableCxCSS();
	RADIO_SetupRegisters(true);
}

void RADIO_SendEndOfTransmission(bool playRoger)
{
	if (playRoger) {
		if (gEeprom.ROGER == ROGER_MODE_ROGER)
			BK4819_PlayRoger();
		else
		if (gEeprom.ROGER == ROGER_MODE_MDC)
			BK4819_PlayRogerMDC();
	}

	if (gCurrentVfo->DTMF_PTT_ID_TX_MODE == PTT_ID_APOLLO)
		BK4819_PlaySingleTone(2475, 250, 28, gEeprom.DTMF_SIDE_TONE);

	if (
#ifdef ENABLE_DTMF
		gDTMF_CallState == DTMF_CALL_STATE_NONE &&
#endif
	   (gCurrentVfo->DTMF_PTT_ID_TX_MODE == PTT_ID_TX_DOWN ||
	    gCurrentVfo->DTMF_PTT_ID_TX_MODE == PTT_ID_BOTH))
	{	// end-of-tx
		if (gEeprom.DTMF_SIDE_TONE)
		{
			AUDIO_AudioPathOn();
			gEnableSpeaker = true;
			SYSTEM_DelayMs(60);
		}

		BK4819_EnterDTMF_TX(gEeprom.DTMF_SIDE_TONE);

		BK4819_PlayDTMFString(
				gEeprom.DTMF_DOWN_CODE,
				0,
				gEeprom.DTMF_FIRST_CODE_PERSIST_TIME,
				gEeprom.DTMF_HASH_CODE_PERSIST_TIME,
				gEeprom.DTMF_CODE_PERSIST_TIME,
				gEeprom.DTMF_CODE_INTERVAL_TIME);
		
		AUDIO_AudioPathOff();
		gEnableSpeaker = false;
	}

	BK4819_ExitDTMF_TX(true);
}

uint8_t RADIO_ValidMemoryChannelsCount(bool bCheckScanList, uint8_t VFO)
	{
		uint8_t count=0;
		for (int i = MR_CHANNEL_FIRST; i<=MR_CHANNEL_LAST; ++i) {
			if(RADIO_CheckValidChannel(i, bCheckScanList, VFO))
				count++;
		}
		return count;
	}
