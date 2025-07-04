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

#include "frequencies.h"
#include "misc.h"
#include "settings.h"

// the BK4819 has 2 bands it covers, 18MHz ~ 630MHz and 760MHz ~ 1300MHz
const freq_band_table_t BX4819_band1 = { 1400000,  63000000}; //Robby69
const freq_band_table_t BX4819_band2 = {76000000, 260000000}; //test offset 2600 was 1300

const freq_band_table_t frequencyBandTable[7] =
{
	#ifndef ENABLE_WIDE_RX
		// QS original
		{.lower =  5000000,  .upper =  7600000},
		{.lower = 10800000,  .upper = 13700000},
		{.lower = 13700000,  .upper = 17400000},
		{.lower = 17400000,  .upper = 35000000},
		{.lower = 35000000,  .upper = 40000000},
		{.lower = 40000000,  .upper = 47000000},
		{.lower = 47000000,  .upper = 60000000}
	#else
		// extended range
		{.lower =  BX4819_band1.lower, .upper =  10800000},
		{.lower = 10800000, .upper =  13700000},
		{.lower = 13700000, .upper =  17400000},
		{.lower = 17400000, .upper =  35000000},
		{.lower = 35000000, .upper =  40000000},
		{.lower = 40000000, .upper =  47000000},
		{.lower = 47000000, .upper = BX4819_band2.upper}
	#endif
};


const uint16_t gStepFrequencyTable[] = {
	250, 500, 625, 1000, 1250, 2500, 833,
	1, 5, 10, 25, 50, 100, 125, 1500, 3000, 5000, 10000, 12500, 25000, 50000
	};

const uint8_t StepMatchedIndexes[] = {
	STEP_2_5kHz, STEP_5kHz, STEP_6_25kHz, STEP_10kHz, STEP_12_5kHz, STEP_25kHz, STEP_8_33kHz,
	STEP_0_01kHz, STEP_0_05kHz, STEP_0_1kHz, STEP_0_25kHz, STEP_0_5kHz, STEP_1kHz, STEP_1_25kHz,
	STEP_15kHz, STEP_30kHz, STEP_50kHz, STEP_100kHz, STEP_125kHz, STEP_250kHz, STEP_500kHz	
};

const uint8_t StepSortedIndexes[] = {
	STEP_0_01kHz, STEP_0_05kHz, STEP_0_1kHz, STEP_0_25kHz, STEP_0_5kHz, STEP_1kHz, STEP_1_25kHz,
	STEP_2_5kHz, STEP_5kHz, STEP_6_25kHz, STEP_8_33kHz, STEP_10kHz, STEP_12_5kHz, STEP_15kHz,
	STEP_25kHz, STEP_30kHz, STEP_50kHz, STEP_100kHz, STEP_125kHz, STEP_250kHz, STEP_500kHz
};

uint8_t FREQUENCY_GetStepIdxFromSortedIdx(uint8_t sortedIdx) 
{
	return StepSortedIndexes[sortedIdx];
}
uint8_t FREQUENCY_GetSortedIdxFromStepIdx(uint8_t stepIdx) 
{
	for(uint8_t i = 0; i < ARRAY_SIZE(gStepFrequencyTable); i++)
		if(StepSortedIndexes[i] == stepIdx)
			return i;
	return 0;
}
uint8_t FREQUENCY_GetStepIdxFromStepFrequency(uint16_t stepFrequency) 
{
	for(uint8_t i = 0; i < ARRAY_SIZE(gStepFrequencyTable); i++)
		if(gStepFrequencyTable[i] == stepFrequency)
			return StepMatchedIndexes[i];
	return 0;
}

FREQUENCY_Band_t FREQUENCY_GetBand(uint32_t Frequency)
{
	int band;
	for (band = ARRAY_SIZE(frequencyBandTable) - 1; band >= 0; band--)
		if (Frequency >= frequencyBandTable[band].lower)
//		if (Frequency <  frequencyBandTable[band].upper)
			return (FREQUENCY_Band_t)band;

	return BAND1_50MHz;
//	return BAND_NONE;
}

uint8_t FREQUENCY_CalculateOutputPower(uint8_t TxpLow, uint8_t TxpMid, uint8_t TxpHigh, int32_t LowerLimit, int32_t Middle, int32_t UpperLimit, int32_t Frequency)
{
	if (Frequency <= LowerLimit)
		return TxpLow;

	if (UpperLimit <= Frequency)
		return TxpHigh;

	if (Frequency <= Middle)
	{
		TxpMid += ((TxpMid - TxpLow) * (Frequency - LowerLimit)) / (Middle - LowerLimit);
		return TxpMid;
	}

	TxpMid += ((TxpHigh - TxpMid) * (Frequency - Middle)) / (UpperLimit - Middle);

	return TxpMid;
}


uint32_t FREQUENCY_RoundToStep(uint32_t freq, uint16_t step)
{
	if(step == 833) {
        uint32_t base = freq/2500*2500;
        int chno = (freq - base) / 700;    // convert entered aviation 8.33Khz channel number scheme to actual frequency. 
        return base + (chno * 833) + (chno == 3);
	}
	if(step == 1)
		return freq;
	return (freq + (step + 1) / 2) / step * step;
}

int TX_freq_check(const uint32_t Frequency)
{	// return '0' if TX frequency is allowed
	// otherwise return '-1'

	if (Frequency < frequencyBandTable[0].lower || Frequency > frequencyBandTable[ARRAY_SIZE(frequencyBandTable) - 1].upper)
		return -1;  // not allowed outside this range

	if (Frequency >= BX4819_band1.upper && Frequency < BX4819_band2.lower)
		return -1;  // BX chip does not work in this range

	switch (gSetting_F_LOCK)
	{
		case F_UNLOCK_PMR:
			if (Frequency >= 44600000 && Frequency <= 44620000)
				return 0;
			break;

		case F_UNLOCK_ALL:
				return 0;
			break;
	}

	// dis-allowed TX frequency
	return -1;
}

int RX_freq_check(const uint32_t Frequency)
{	// return '0' if RX frequency is allowed
	// otherwise return '-1'

	if (Frequency < RX_freq_min() || Frequency > frequencyBandTable[ARRAY_SIZE(frequencyBandTable) - 1].upper)
		return -1;

	if (Frequency >= BX4819_band1.upper && Frequency < BX4819_band2.lower)
		return -1;

	return 0;   // OK frequency
}

uint32_t RX_freq_min()
{
	return gEeprom.RX_OFFSET >= frequencyBandTable[0].lower ? 0 : frequencyBandTable[0].lower - gEeprom.RX_OFFSET;
}

uint32_t Band_freq_min(FREQUENCY_Band_t Band)
{
	return gEeprom.RX_OFFSET >= frequencyBandTable[Band].lower ? 0 : frequencyBandTable[Band].lower - gEeprom.RX_OFFSET;
}