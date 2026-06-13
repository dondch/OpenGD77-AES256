/* -*- coding: binary; -*- */
/*
 * Copyright (C) 2019-2025 Roger Clark, VK3KYY / G4KYF
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*
 * Translators: JE4SMQ
 *
 *
 * Rev:
 */
#ifndef USER_INTERFACE_LANGUAGES_JAPANESE_H_
#define USER_INTERFACE_LANGUAGES_JAPANESE_H_
/********************************************************************
 *
 * VERY IMPORTANT.
 * This file should not be saved with UTF-8 encoding
 * Use Notepad++ on Windows with ANSI encoding
 * or emacs on Linux with windows-1252-unix encoding
 *
 ********************************************************************/
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
__attribute__((section(".upper_text")))
#endif
const stringsTable_t japaneseLanguage =
{
.magicNumber                            = { LANGUAGE_TAG_MAGIC_NUMBER, LANGUAGE_TAG_VERSION },
.LANGUAGE_NAME 				= "ÆÎÝºÞ", // MaxLen: 16
.menu					= "ÒÆ­-", // MaxLen: 16
.credits				= "¶²ÊÂ¼¬", // MaxLen: 16
.zone					= "¿Þ-Ý", // MaxLen: 16
.rssi					= "RSSI", // MaxLen: 16
.battery				= "ÃÞÝÁ", // MaxLen: 16
.contacts				= "ºÝÀ¸Ä", // MaxLen: 16
.last_heard				= "¼Þ­¼ÝÛ¸Þ", // MaxLen: 16
.firmware_info				= "Ì§-Ñ³ª±¼Þ®³Î³", // MaxLen: 16
.options				= "µÌß¼®Ý", // MaxLen: 16
.display_options			= "Ë®³¼Þ µÌß¼®Ý", // MaxLen: 16
.sound_options				= "µÝ¾²  µÌß¼®Ý", // MaxLen: 16
.channel_details			= "Á¬ÝÈÙ Å²Ö³", // MaxLen: 16
.language				= "Language", // MaxLen: 16
.new_contact				= "New ºÝÀ¸Ä", // MaxLen: 16
.dmr_contacts				= "DMR ºÝÀ¸Ä", // MaxLen: 16
.contact_details			= "ºÝÀ¸ÄÅ²Ö³", // MaxLen: 16
.hotspot_mode				= "Î¯Ä½Îß¯Ä", // MaxLen: 16
.built					= "Built", // MaxLen: 16
.zones					= "¿Þ-Ý", // MaxLen: 16
.keypad					= "·-Êß¯ÄÞ", // MaxLen: 12 (with .ptt)
.ptt					= "PTT", // MaxLen: 12 (with .keypad)
.locked					= "Û¯¸", // MaxLen: 15
.press_sk2_plus_star			= "SK2 + *", // MaxLen: 16
.to_unlock				= "Û¯¸¶²¼Þ®", // MaxLen: 16
.unlocked				= "Û¯¸¶²¼Þ®½ÞÐ", // MaxLen: 15
.power_off				= "ÃÞÝ¹ÞÝ Off...", // MaxLen: 16
.error					= "´×-", // MaxLen: 8
.rx_only				= "¿³¼Ý·Ý¼", // MaxLen: 14
.out_of_band				= "µÌÊÞÝÄÞ", // MaxLen: 14
.timeout				= "À²Ñ±³Ä", // MaxLen: 8
.tg_entry				= "TG Æ­³Ø®¸", // MaxLen: 15
.pc_entry				= "PC Æ­³Ø®¸", // MaxLen: 15
.user_dmr_id				= "Õ-»Þ- DMR ID", // MaxLen: 15
.contact 				= "ºÝÀ¸Ä", // MaxLen: 15
.accept_call				= "Return call to", // MaxLen: 16
.private_call				= "Ìß×²ÍÞ-Äº-Ù", // MaxLen: 16
.squelch				= "½¹ÙÁ", // MaxLen: 8
.quick_menu 				= "¸²¯¸ÒÆ­-", // MaxLen: 16
.filter					= "Ì¨ÙÀ-", // MaxLen: 7 (with ':' + settings: .none, "CC", "CC,TS", "CC,TS,TG")
.all_channels				= "¾ÞÝÁ¬ÝÈÙ", // MaxLen: 16
.gotoChannel				= "Á¬ÝÈÙ²ÄÞ³",  // MaxLen: 11 (" 1024")
.scan					= "½·¬Ý", // MaxLen: 16
.channelToVfo				= "Á¬ÝÈÙ --> VFO", // MaxLen: 16
.vfoToChannel				= "VFO --> Á¬ÝÈÙ", // MaxLen: 16
.vfoToNewChannel			= "VFO --> NewÁ¬ÝÈÙ", // MaxLen: 16
.group					= "¸ÞÙ-Ìß", // MaxLen: 16 (with .type)
.private				= "Ìß×²ÍÞ-Ä", // MaxLen: 16 (with .type)
.all					= "µ-Ù", // MaxLen: 16 (with .type)
.type					= "À²Ìß", // MaxLen: 16 (with .type)
.timeSlot				= "À²Ñ½Û¯Ä", // MaxLen: 16 (plus ':' and  .none, '1' or '2')
.none					= "Å¼", // MaxLen: 16 (with .timeSlot, "Rx CTCSS:" and ""Tx CTCSS:", .filter/.mode/.dmr_beep)
.contact_saved				= "ºÝÀ¸Ä Î¿ÞÝ½Ð", // MaxLen: 16
.duplicate				= "¼Þ­³Ì¸", // MaxLen: 16
.tg					= "TG",  // MaxLen: 8
.pc					= "PC", // MaxLen: 8
.ts					= "TS", // MaxLen: 8
.mode					= "Ó-ÄÞ",  // MaxLen: 12
.colour_code				= "¶×-º-ÄÞ", // MaxLen: 16 (with ':' * .n_a)
.n_a					= "N/A",// MaxLen: 16 (with ':' * .colour_code)
.bandwidth				= "ÊÞÝÄÞÊÊÞ", // MaxLen: 16 (with ':' + .n_a, "25kHz" or "12.5kHz")
.stepFreq				= "½Ã¯Ìß", // MaxLen: 7 (with ':' + xx.xxkHz fitted)
.tot					= "TOT", // MaxLen: 16 (with ':' + .off or 15..3825)
.off					= "µÌ", // MaxLen: 16 (with ':' + .timeout_beep, .band_limits)
.zone_skip				= "¿Þ-Ý ½·¯Ìß", // MaxLen: 16 (with ':' + .yes or .no)
.all_skip				= "µ-Ù ½·¯Ìß", // MaxLen: 16 (with ':' + .yes or .no)
.yes					= "Ê²", // MaxLen: 16 (with ':' + .zone_skip, .all_skip or .factory_reset)
.no					= "²²´", // MaxLen: 16 (with ':' + .zone_skip, .all_skip or .factory_reset)
.tg_list				= "TG Lst", // MaxLen: 16 (with ':' and codeplug group name)
.on					= "µÝ", // MaxLen: 16 (with ':' + .band_limits)
.timeout_beep				= "À²Ñ±³ÄËÞ-Ìß", // MaxLen: 16 (with ':' + .n_a or 5..20 + 's')
.list_full				= "List full",
.dmr_cc_scan				= "CC½·¬Ý", // MaxLen: 12 (with ':' + settings: .on or .off)
.band_limits				= "ÊÞÝÄÞ¾²¹ÞÝ", // MaxLen: 16 (with ':' + .on or .off)
.beep_volume				= "ËÞ-ÌßµÝØ®³", // MaxLen: 16 (with ':' + -24..6 + 'dB')
.dmr_mic_gain				= "DMR Ï²¸¹Þ²Ý", // MaxLen: 16 (with ':' + -33..12 + 'dB')
.fm_mic_gain				= "FM Ï²¸¹Þ²Ý", // MaxLen: 16 (with ':' + 0..31)
.key_long				= "·-ÛÝ¸Þ", // MaxLen: 11 (with ':' + x.xs fitted)
.key_repeat				= "·-ØËß-Ä", // MaxLen: 11 (with ':' + x.xs fitted)
.dmr_filter_timeout			= "Ì¨ÙÀ-TO", // MaxLen: 16 (with ':' + 1..90 + 's')
.brightness				= "±¶Ù»", // MaxLen: 16 (with ':' + 0..100 + '%')
.brightness_off				= "ÃÞ¨Ï-", // MaxLen: 16 (with ':' + 0..100 + '%')
.contrast				= "ºÝÄ×½Ä", // MaxLen: 16 (with ':' + 12..30)
.screen_invert				= "ÊÝÃÝ", // MaxLen: 16
.screen_normal				= "Â³¼Þ®³", // MaxLen: 16
.backlight_timeout			= "À²Ñ±³Ä", // MaxLen: 16 (with ':' + .no to 30s)
.scan_delay				= "½·¬ÝÃÞ¨Ú²", // MaxLen: 16 (with ':' + 1..30 + 's')
.yes___in_uppercase			= "Ê²", // MaxLen: 8 (choice above green/red buttons)
.no___in_uppercase			= "²²´", // MaxLen: 8 (choice above green/red buttons)
.DISMISS				= "¼¯Êß²", // MaxLen: 8 (choice above green/red buttons)
.scan_mode				= "½·¬ÝÓ-ÄÞ", // MaxLen: 16 (with ':' + .hold, .pause or .stop)
.hold					= "Î-ÙÄÞ", // MaxLen: 16 (with ':' + .scan_mode)
.pause					= "Îß-½Þ", // MaxLen: 16 (with ':' + .scan_mode)
.list_empty				= "Ø½ÄÅ¼", // MaxLen: 16
.delete_contact_qm			= "ºÝÀ¸ÄÉ»¸¼Þ®?", // MaxLen: 16
.contact_deleted			= "ºÝÀ¸Ä»¸¼Þ®½Ð", // MaxLen: 16
.contact_used				= "ºÝÀ¸ÄÊ½ÃÞÆ±Ù", // MaxLen: 16
.in_tg_list				= "in TG list", // MaxLen: 16
.select_tx				= "¿³¼Ý¾ÝÀ¸", // MaxLen: 16
.edit_contact				= "ºÝÀ¸Ä ¼­³¾²", // MaxLen: 16
.delete_contact				= "ºÝÀ¸Ä »¸¼Þ®", // MaxLen: 16
.group_call				= "¸ÞÙ-Ìßº-Ù", // MaxLen: 16
.all_call				= "µ-Ùº-Ù", // MaxLen: 16
.tone_scan				= "Ä-Ý½·¬Ý", // MaxLen: 16
.low_battery				= "ÃÞÝÁ-·ÞÚ !!!", // MaxLen: 16
.Auto					= "µ-Ä", // MaxLen 16 (with .mode + ':')
.manual					= "ÏÆ­±Ù",  // MaxLen 16 (with .mode + ':')
.ptt_toggle				= "PTT Ä¸ÞÙ", // MaxLen 16 (with ':' + .on or .off)
.private_call_handling			= "PC³¹Â¹", // MaxLen 16 (with ':' + .on or .off)
.stop					= "½Ä¯Ìß", // Maxlen 16 (with ':' + .scan_mode/.dmr_beep)
.one_line				= "1 ·Þ®³", // MaxLen 16 (with ':' + .contact)
.two_lines				= "2 ·Þ®³", // MaxLen 16 (with ':' + .contact)
.new_channel				= "New Á¬ÝÈÙ", // MaxLen: 16, leave room for a space and four channel digits after
.priority_order				= "DB¼Þ­Ý", // MaxLen 16 (with ':' + 'Cc/DB/TA')
.dmr_beep				= "DMR ËÞ-Ìß", // MaxLen 16 (with ':' + .star/.stop/.both/.none)
.start					= "½À-Ä", // MaxLen 16 (with ':' + .dmr_beep)
.both					= "Ø®³Î³", // MaxLen 16 (with ':' + .dmr_beep)
.vox_threshold                          = "VOX ½Ú¯¼®ÙÄÞ", // MaxLen 16 (with ':' + .off or 1..30)
.vox_tail                               = "VOX Ã-Ù", // MaxLen 16 (with ':' + .n_a or '0.0s')
.audio_prompt				= "µÝ¾²±ÝÅ²",// Maxlen 16 (with ':' + .silent, .beep or .voice_prompt_level_1)
.silent                                 = "Å¼", // Maxlen 16 (with ':' + audio_prompt)
.rx_beep				= "RX beep", // MaxLen 16 (with ':' + .carrier/.talker/.both/.none)
.beep					= "ËÞ-Ìß", // Maxlen 16 (with ':' + audio_prompt)
.voice_prompt_level_1			= "µÝ¾²", // Maxlen 16 (with ':' + audio_prompt, satellite "mode")
.transmitTalkerAliasTS1			= "TA¿³¼Ý TS1", // Maxlen 16 (with ':' + .on or .off)
.squelch_VHF				= "VHF ½¹ÙÁ",// Maxlen 16 (with ':' + XX%)
.squelch_220				= "220 ½¹ÙÁ",// Maxlen 16 (with ':' + XX%)
.squelch_UHF				= "UHF ½¹ÙÁ", // Maxlen 16 (with ':' + XX%)
.display_screen_invert 		= "ÊÞ¯¸¶×-", // Maxlen 16 (with ':' + .screen_normal or .screen_invert)
.openGD77 				= "OpenGD77",// Do not translate
.talkaround 				= "Talkaround", // Maxlen 16 (with ':' + .on , .off or .n_a)
.APRS 					= "APRS", // Maxlen 16 (with ':' + .transmitTalkerAliasTS1 or transmitTalkerAliasTS2)
.no_keys 				= "No Keys", // Maxlen 16 (with ':' + audio_prompt)
.gitCommit				= "Git commit",
.voice_prompt_level_2			= "µÝ¾² L2", // Maxlen 16 (with ':' + audio_prompt)
.voice_prompt_level_3			= "µÝ¾² L3", // Maxlen 16 (with ':' + audio_prompt)
.dmr_filter				= "DMR Ì¨ÙÀ-",// MaxLen: 12 (with ':' + settings: "TG" or "Ct" or "TGL")
.talker					= "Talker",
.dmr_ts_filter				= "TS Ì¨ÙÀ-", // MaxLen: 12 (with ':' + settings: .on or .off)
.dtmf_contact_list			= "DTMF ºÝÀ¸ÄØ½Ä", // Maxlen: 16
.channel_power				= "Ch Pwr", //Displayed as "Ch Power:" + .from_master or "Ch Power:"+ power text e.g. "Power:500mW" . Max total length 16
.from_master				= "Ï½À-",// Displayed if per-channel power is not enabled  the .channel_power
.set_quickkey				= "¸²¯¸·- ¾¯Ä", // MaxLen: 16
.dual_watch				= "ÃÞ­±ÙÜ¯Á", // MaxLen: 16
.info					= "¼Þ®³Î³", // MaxLen: 16 (with ':' + .off or .ts or .pwr or .both)
.pwr					= "Pwr",
.user_power				= "Õ-»Þ-Pwr",
.temperature				= "µÝÄÞ", // MaxLen: 16 (with ':' + .celcius or .fahrenheit)
.celcius				= "°C",
.seconds				= "ËÞ®³",
.radio_info				= "Ñ¾Ý· ¼Þ®³Î³",
.temperature_calibration		= "µÝÄÞÁ®³¾²",
.pin_code				= "±Ý¼®³ÊÞÝºÞ³",
.please_confirm				= "¶¸ÆÝ¼Ã¸ÀÞ»²", // MaxLen: 15
.vfo_freq_bind_mode			= "TRFÚÝÄÞ³",
.overwrite_qm				= "¶·¶´OK?", //Maxlen: 14 chars
.eco_level				= "´ºÚÍÞÙ",
.buttons				= "ÎÞÀÝ",
.leds					= "LEDs",
.scan_dwell_time			= "½·¬ÝÃ²¼¼Þ¶Ý",
.battery_calibration			= "ÃÞÝ±ÂÁ®³¾²",
.low					= "µ¿²",
.high					= "ÊÔ²",
.dmr_id					= "DMR ID",
.scan_on_boot				= "·ÄÞ³¼Þ½·¬Ý",
.dtmf_entry				= "DTMF ´ÝÄØ°",
.name					= "ÅÏ´",
.carrier				= "Carrier",
.zone_empty 				= "Zone empty", // Maxlen: 12 chars.
.time					= "¼Þ¶Ý",
.uptime					= "Uptime",
.hours					= "¼Þ¶Ý",
.minutes				= "ÌÝ",
.satellite				= "´²¾²",
.alarm_time				= "±×°Ñ¼Þ¶Ý",
.location				= "Û¹°¼®Ý",
.date					= "Ë½Þ¹",
.timeZone				= "À²Ñ¿Þ°Ý",
.suspend				= "»½ÍßÝÄÞ",
.pass					= "Êß½", // For satellite screen
.elevation				= "El",
.azimuth				= "Az",
.inHHMMSS				= "in",
.predicting				= "Ö¿³",
.maximum				= "Max",
.satellite_short		= "Sat",
.use_location_short			= "Location", // MaxLen: 16 (with ':' + .yes or .no)
.UTC					= "UTC",
.symbols				= "NSEW", // symbols: N,S,E,W
.not_set				= "¾¯Ä¼Å²",
.general_options		= "¾ÞÝÀ²µÌß¼®Ý",
.radio_options			= "Ñ¾ÝµÌß¼®Ý",
.auto_night				= "¼ÞÄÞ³Ô¶ÝË®³¼Þ", // MaxLen: 16 (with .on or .off)
.dmr_rx_agc				= "DMR ¼Þ­¼ÝAGC",
.speaker_click_suppress			= "¸Ø¯¸µÝ»¸¹ÞÝ.",
.gps					= "GPS",
.end_only				= "End only",
.dmr_crc				= "DMR crc",
.eco					= "Eco",
.safe_power_on				= "±Ý¾ÞÝÃÞÝ¹ÞÝOn", // MaxLen: 16 (with ':' + .on or .off)
.auto_power_off				= "APO", // MaxLen: 16 (with ':' + __._h or .no)
.apo_with_rf				= "APO with RF", // MaxLen: 16 (with ':' + .yes or .no or .n_a)
.brightness_night				= "ÖÙ¼®³Ò²", // MaxLen: 16 (with ':' + 0..100 + %)
.freq_set_VHF			= "VHF¼­³Ê½³",
.gps_acquiring			= "Acquiring",
.altitude				= "º³ÄÞ",
.calibration            = "·¬ØÌÞÚ°¼®Ý",
.freq_set_UHF                = "UHF¼­³Ê½³",
.cal_frequency          = "¼­³Ê½³",
.cal_pwr                = "¼­ÂØ®¸ÚÍÞÙ",
.pwr_set                = "¼­ÂØ®¸¾¯Ä",
.factory_reset          = "µ°ÙØ¾¯Ä",
.rx_tune				= "¼Þ­¼ÝÁ®³¾²",
.transmitTalkerAliasTS2	= "TA Tx TS2", // Maxlen 16 (with ':' + .ta_text, 'APRS' , .both or .off)
.ta_text				= "Ã·½Ä",
.daytime_theme_day			= "Ë Ã°Ï", // MaxLen: 16
.daytime_theme_night			= "ÖÙ Ã°Ï", // MaxLen: 16
.theme_chooser				= "Ã°Ï ¾ÝÀ¸", // Maxlen: 16
.theme_options				= "Ã°Ï µÌß¼®Ý",
.theme_fg_default			= "Ã·½Ä ¼®·Á", // MaxLen: 16 (+ colour rect)
.theme_bg				= "ÊÞ¯¸¸Þ×³ÝÄÞ", // MaxLen: 16 (+ colour rect)
.theme_fg_decoration			= "ÃÞºÚ°¼®Ý", // MaxLen: 16 (+ colour rect)
.theme_fg_text_input			= "Ã·½Ä Æ­³Ø®¸", // MaxLen: 16 (+ colour rect)
.theme_fg_splashscreen			= "¾ÞÝ¹². ·ÄÞ³", // MaxLen: 16 (+ colour rect)
.theme_bg_splashscreen			= "³¼Û. ·ÄÞ³", // MaxLen: 16 (+ colour rect)
.theme_fg_notification			= "Ã·½Ä Á­³².", // MaxLen: 16 (+ colour rect)
.theme_fg_warning_notification		= "¹²º¸ Á­³².", // MaxLen: 16 (+ colour rect)
.theme_fg_error_notification		= "´×°  Á­³²", // MaxLen: 16 (+ colour rect)
.theme_bg_notification                  = "ÊÞ¯¸. Á­³²", // MaxLen: 16 (+ colour rect)
.theme_fg_menu_name			= "ÒÆ­°Ò²", // MaxLen: 16 (+ colour rect)
.theme_bg_menu_name			= "ÒÆ­°Ò² ÊÞ¯¸", // MaxLen: 16 (+ colour rect)
.theme_fg_menu_item			= "ÒÆ­° º³Ó¸", // MaxLen: 16 (+ colour rect)
.theme_fg_menu_item_selected		= "ÒÆ­° Ê²×²Ä", // MaxLen: 16 (+ colour rect)
.theme_fg_options_value			= "µÌß¼®Ý Á", // MaxLen: 16 (+ colour rect)
.theme_fg_header_text			= "Í¯ÀÞ° Ã·½Ä", // MaxLen: 16 (+ colour rect)
.theme_bg_header_text			= "Í¯ÀÞ° Ã·½Ä ÊÞ¯¸", // MaxLen: 16 (+ colour rect)
.theme_fg_rssi_bar			= "RSSI ÊÞ°", // MaxLen: 16 (+ colour rect)
.theme_fg_rssi_bar_s9p			= "RSSI ÊÞ° S9+", // Maxlen: 16 (+colour rect)
.theme_fg_channel_name			= "Á¬ÝÈÙ Ò²", // MaxLen: 16 (+ colour rect)
.theme_fg_channel_contact		= "ºÝÀ¸Ä", // MaxLen: 16 (+ colour rect)
.theme_fg_channel_contact_info		= "ºÝÀ¸Ä ¼Þ®³Î³", // MaxLen: 16 (+ colour rect)
.theme_fg_zone_name			= "¿Þ°Ý Ò²", // MaxLen: 16 (+ colour rect)
.theme_fg_rx_freq			= "¼Þ­¼Ý¼­³Ê½³", // MaxLen: 16 (+ colour rect)
.theme_fg_tx_freq			= "¿³¼Ý¼­³Ê½³", // MaxLen: 16 (+ colour rect)
.theme_fg_css_sql_values		= "CSS/SQL ±À²", // MaxLen: 16 (+ colour rect)
.theme_fg_tx_counter			= "¿³¼Ý ¶³ÝÀ°", // MaxLen: 16 (+ colour rect)
.theme_fg_polar_drawing			= "Polar", // MaxLen: 16 (+ colour rect)
.theme_fg_satellite_colour		= "Sat. spot", // MaxLen: 16 (+ colour rect)
.theme_fg_gps_number			= "GPS number", // MaxLen: 16 (+ colour rect)
.theme_fg_gps_colour			= "GPS spot", // MaxLen: 16 (+ colour rect)
.theme_fg_bd_colour			= "BeiDou spot", // MaxLen: 16 (+ colour rect)
.theme_colour_picker_red		= "±¶", // MaxLen 16 (with ':' + 3 digits value)
.theme_colour_picker_green		= "ÐÄÞØ", // MaxLen 16 (with ':' + 3 digits value)
.theme_colour_picker_blue		= "±µ", // MaxLen 16 (with ':' + 3 digits value)
.volume					= "ÎÞØ­°Ñ", // MaxLen: 8
.roaming				= "Roaming", // MaxLen 16 (with ':' + .on or .off)
.show_distance				= "·®ØË®³¼Þ", // MaxLen 16 (with ':' + .on or .off)
.aprs_options				= "APRS µÌß¼®Ý", // MaxLen 16
.aprs_smart				= "½Ï°Ä", // MaxLen 16 (with ':' + .mode)
.aprs_channel				= "Á¬ÝÈÙ", // MaxLen 16 (with ':' + .location)
.aprs_decay				= "Decay", // MaxLen 16 (with ':' + .on or .off)
.aprs_compress				= "Compress", // MaxLen 16 (with ':' + .on or .off)
.aprs_interval				= "¿³¼Ý¶Ý¶¸", // MaxLen 16 (with ':' + 0.2..60 + 'min')
.aprs_message_interval			= "Msg¶Ý¶¸", // MaxLen 16 (with ':' + 3..30)
.aprs_slow_rate				= "Slow Rate", // MaxLen 16 (with ':' + 1..100 + 'min')
.aprs_fast_rate				= "Fast Rate", // MaxLen 16 (with ':' + 10..180 + 's')
.aprs_low_speed				= "Low Speed", // MaxLen 16 (with ':' + 2..30 + 'km/h')
.aprs_high_speed			= "Hi Speed", // MaxLen 16 (with ':' + 2..90 + 'km/h')
.aprs_turn_angle			= "T. Angle", // MaxLen 16 (with ':' + 5..90 + '<degree>')
.aprs_turn_slope			= "T. Slope", // MaxLen 16 (with ':' + 1..255 + '<degree>/v')
.aprs_turn_time				= "T. Time", // MaxLen 16 (with ':' + 5..180 + 's')
.auto_lock				= "¼ÞÄÞ³Û¯¸", // MaxLen 16 (with ':' + .off or 0.5..15 (.5 step) + 'min')
.trackball				= "Trackball", // MaxLen 16 (with ':' + .on or .off)
.dmr_force_dmo				= "Force DMO", // MaxLen 16 (with ':' + .n_a or .on or .off)
.tx_inhibit				= "TX Inhibit", // MaxLen 16 (with ':' + .on or .off)
.latitude_short				= "Lat", // MaxLen: 16 (with ':' + latitude value)
.longitude_short			= "Lon", // MaxLen: 16 (with ':' + longitude value)
.text_size				= "Text size",//MaxLen: 16 (with ':' + 1 digit)
.last_talker				= "Last talker", // MaxLen: 16 (with ':' + .off or 2 digits value + 's')
.mute					= "Mute", // MaxLen: 16 (with ':' + .on or .off)
};
/********************************************************************
 *
 * VERY IMPORTANT.
 * This file should not be saved with UTF-8 encoding
 * Use Notepad++ on Windows with ANSI encoding
 * or emacs on Linux with windows-1252-unix encoding
 *
 ********************************************************************/
#endif /* USER_INTERFACE_LANGUAGES_JAPANESE_H_ */
