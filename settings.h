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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "frequencies.h"
#include <helper/battery.h>
#include "radio.h"
#include <driver/backlight.h>
#ifdef ENABLE_MESSENGER
	#include "app/messenger.h"
#endif

enum POWER_OnDisplayMode_t {
	POWER_ON_DISPLAY_MODE_FULL_SCREEN = 0,
	POWER_ON_DISPLAY_MODE_MESSAGE,
	POWER_ON_DISPLAY_MODE_VOLTAGE,
	POWER_ON_DISPLAY_MODE_NONE
};
typedef enum POWER_OnDisplayMode_t POWER_OnDisplayMode_t;

enum {
	F_UNLOCK_PMR,
	F_UNLOCK_ALL,	// enable TX on all frequenciese
	F_LOCK_LEN
};

enum {
	SCAN_RESUME_TO = 0,
	SCAN_RESUME_CO,
	SCAN_RESUME_SE
};

enum {
	CROSS_BAND_OFF = 0,
	CROSS_BAND_CHAN_A,
	CROSS_BAND_CHAN_B
};

enum {
	DUAL_WATCH_OFF = 0,
	DUAL_WATCH_CHAN_A,
	DUAL_WATCH_CHAN_B
};

enum {
	TX_OFFSET_FREQUENCY_DIRECTION_OFF = 0,
	TX_OFFSET_FREQUENCY_DIRECTION_ADD,
	TX_OFFSET_FREQUENCY_DIRECTION_SUB
};

enum {
	OUTPUT_POWER_LOW = 0,
	OUTPUT_POWER_MID,
	OUTPUT_POWER_HIGH
};

enum {
	RX_AGC_SLOW,
	RX_AGC_FAST,
	RX_AGC_LEN
};

enum {
	ACTION_OPT_NONE = 0,
	ACTION_OPT_FLASHLIGHT,
	ACTION_OPT_TOGGLE_PTT,
	ACTION_OPT_POWER,
	ACTION_OPT_MONITOR,
	ACTION_OPT_SCAN,
	ACTION_OPT_FM,
	ACTION_OPT_1750,
	ACTION_OPT_KEYLOCK,
	ACTION_OPT_A_B,
	ACTION_OPT_VFO_MR,
	ACTION_OPT_SWITCH_DEMODUL,
#ifdef ENABLE_BLMIN_TMP_OFF
	ACTION_OPT_BLMIN_TMP_OFF, //BackLight Minimum Temporay OFF
#endif
	ACTION_OPT_BANDWIDTH,
	//ACTION_OPT_SPECTRUM,
	ACTION_OPT_LEN
};

#ifdef ENABLE_VOICE
	enum VOICE_Prompt_t
	{
		VOICE_PROMPT_OFF = 0,
		VOICE_PROMPT_CHINESE,
		VOICE_PROMPT_ENGLISH
	};
	typedef enum VOICE_Prompt_t VOICE_Prompt_t;
#endif

enum ALARM_Mode_t {
	ALARM_MODE_SITE = 0,
	ALARM_MODE_TONE
};
typedef enum ALARM_Mode_t ALARM_Mode_t;

enum ROGER_Mode_t {
	ROGER_MODE_OFF = 0,
	ROGER_MODE_ROGER,
	ROGER_MODE_MDC
};
typedef enum ROGER_Mode_t ROGER_Mode_t;

enum CHANNEL_DisplayMode_t {
	MDF_FREQUENCY = 0,
	MDF_CHANNEL,
	MDF_NAME,
	MDF_NAME_FREQ
};
typedef enum CHANNEL_DisplayMode_t CHANNEL_DisplayMode_t;

typedef struct {
	uint8_t               ScreenChannel[2]; // current channels set in the radio (memory or frequency channels)
	uint8_t               FreqChannel[2]; // last frequency channels used
	uint8_t               MrChannel[2]; // last memory channels used
	#ifdef ENABLE_NOAA
		uint8_t           NoaaChannel[2];
	#endif

	// The actual VFO index (0-upper/1-lower) that is now used for RX, 
	// It is being alternated by dual watch, and flipped by crossband
	uint8_t               RX_VFO;

	// The main VFO index (0-upper/1-lower) selected by the user
	// 
	uint8_t               TX_VFO;

	uint8_t               field7_0xa;
	uint8_t               field8_0xb;

	#ifdef ENABLE_FMRADIO
		uint16_t          FM_FrequencyPlaying;
	#endif

	uint8_t               SQUELCH_LEVEL;
	uint8_t               TX_TIMEOUT_TIMER;
	bool                  KEY_LOCK;
	bool                  VOX_SWITCH;
	uint8_t               VOX_LEVEL; // sensitivity and OFF when 0
	uint8_t				  VOX_DELAY; // vox delay (0-10)
	#ifdef ENABLE_VOICE
		VOICE_Prompt_t    VOICE_PROMPT;
	#endif
	bool                  BEEP_CONTROL;
	uint8_t               CHANNEL_DISPLAY_MODE;
	bool                  VFO_OPEN;
	uint8_t               DUAL_WATCH;
	uint8_t               CROSS_BAND_RX_TX;
	uint8_t               BATTERY_SAVE;
	uint8_t               BACKLIGHT_TIME;
	uint8_t               SCAN_RESUME_MODE;
	uint8_t               SCAN_LIST_DEFAULT;
	bool                  SCAN_LIST_ENABLED[2];
	uint8_t               SCANLIST_PRIORITY_CH1[2];
	uint8_t               SCANLIST_PRIORITY_CH2[2];

	uint8_t               field29_0x26;
	uint8_t               field30_0x27;
	
	uint8_t               field37_0x32;
	uint8_t               field38_0x33;

	bool                  AUTO_KEYPAD_LOCK;
	#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
		ALARM_Mode_t      ALARM_MODE;
	#endif
	POWER_OnDisplayMode_t POWER_ON_DISPLAY_MODE;
	ROGER_Mode_t          ROGER;
	uint8_t               KEY_1_SHORT_PRESS_ACTION;
	uint8_t               KEY_1_LONG_PRESS_ACTION;
	uint8_t               KEY_2_SHORT_PRESS_ACTION;
	uint8_t               KEY_2_LONG_PRESS_ACTION;
	uint8_t               MIC_SENSITIVITY;
	uint8_t               MIC_SENSITIVITY_TUNING;
	uint8_t               CHAN_1_CALL;
#ifdef ENABLE_DTMF
	char                  ANI_DTMF_ID[8];
	char                  KILL_CODE[8];
	char                  REVIVE_CODE[8];
#endif
	char                  DTMF_UP_CODE[16];

	uint8_t               field57_0x6c;
	uint8_t               field58_0x6d;

	char                  DTMF_DOWN_CODE[16];

	uint8_t               field60_0x7e;
	uint8_t               field61_0x7f;

#ifdef ENABLE_DTMF
	char                  DTMF_SEPARATE_CODE;
	char                  DTMF_GROUP_CALL_CODE;
	uint8_t               DTMF_DECODE_RESPONSE;
	uint8_t               DTMF_auto_reset_time;
#endif	
	uint16_t              DTMF_PRELOAD_TIME;
	uint16_t              DTMF_FIRST_CODE_PERSIST_TIME;
	uint16_t              DTMF_HASH_CODE_PERSIST_TIME;
	uint16_t              DTMF_CODE_PERSIST_TIME;
	uint16_t              DTMF_CODE_INTERVAL_TIME;
	bool                  DTMF_SIDE_TONE;
#ifdef ENABLE_DTMF
	bool                  PERMIT_REMOTE_KILL;
#endif
	int16_t               BK4819_XTAL_FREQ_LOW;
	#ifdef ENABLE_NOAA
		bool              NOAA_AUTO_SCAN;
	#endif
	uint8_t               VOLUME_GAIN;
	uint8_t               DAC_GAIN;

	VFO_Info_t            VfoInfo[2];
#ifdef ENABLE_PWRON_PASSWORD
	uint32_t              POWER_ON_PASSWORD;
	uint8_t				  PASSWORD_WRONG_ATTEMPTS;
#endif
#ifdef ENABLE_ENCRYPTION
	char                  ENC_KEY[16];
#endif
#ifdef ENABLE_MESSENGER
	MessengerConfig       MESSENGER_CONFIG;
#endif
	uint16_t              VOX1_THRESHOLD;
	uint16_t              VOX0_THRESHOLD;

	uint8_t               field77_0x95;
	uint8_t               field78_0x96;
	uint8_t               field79_0x97;

	uint8_t 			  KEY_M_LONG_PRESS_ACTION;
	uint8_t               BACKLIGHT_MIN;
#ifdef ENABLE_BLMIN_TMP_OFF
	BLMIN_STAT_t		  BACKLIGHT_MIN_STAT;
#endif
	uint8_t               BACKLIGHT_MAX;
	BATTERY_Type_t		  BATTERY_TYPE;
	uint32_t              RX_OFFSET;
	uint8_t				  RX_AGC;
	uint16_t              SQL_TONE;
} EEPROM_Config_t;

extern EEPROM_Config_t gEeprom;

// RxOffs maximum setting
#define RX_OFFSET_MAX 15000000
#define RX_OFFSET_ADDR 0x0E9C

void SETTINGS_SaveVfoIndices(void);
void SETTINGS_SaveSettings(void);
void SETTINGS_SaveChannelName(uint8_t channel, const char * name);
void SETTINGS_SaveChannel(uint8_t Channel, uint8_t VFO, const VFO_Info_t *pVFO, uint8_t Mode);
void SETTINGS_FetchChannelName(char *s, const int channel);
void SETTINGS_SaveBatteryCalibration(const uint16_t * batteryCalibration);
void SETTINGS_UpdateChannel(uint8_t channel, const VFO_Info_t *pVFO, bool keep);
void SETTINGS_SetVfoFrequency(uint32_t frequency);
#ifdef ENABLE_ENCRYPTION
	void SETTINGS_SaveEncryptionKey();
#endif
#endif
