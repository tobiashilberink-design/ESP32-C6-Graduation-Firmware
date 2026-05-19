#ifndef USER_AUDIO_BSP_H
#define USER_AUDIO_BSP_H


extern uint8_t audio_Test_flag;

#ifdef __cplusplus
extern "C" {
#endif


void user_audio_bsp_init(void);
void i2s_music(void *args);
void i2s_echo(void *arg);
void audio_playback_set_vol(uint8_t vol);
void user_audio_sr_init(void);
void i2s_audio_Test(void *arg);

#ifdef __cplusplus
}
#endif

#endif // !MY_ADF_BSP_H
