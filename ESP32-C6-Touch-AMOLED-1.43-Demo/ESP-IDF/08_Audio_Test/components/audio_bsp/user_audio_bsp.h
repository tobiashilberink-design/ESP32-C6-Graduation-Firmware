#ifndef USER_AUDIO_BSP_H
#define USER_AUDIO_BSP_H




#ifdef __cplusplus
extern "C" {
#endif


void user_audio_bsp_init(void);
void i2s_music(void *args);
void i2s_echo(void *arg);
void audio_playback_set_vol(uint8_t vol);

#ifdef __cplusplus
}
#endif

#endif // !MY_ADF_BSP_H
