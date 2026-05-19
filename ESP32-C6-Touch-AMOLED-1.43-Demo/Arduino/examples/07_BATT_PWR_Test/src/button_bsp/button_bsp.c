#include "button_bsp.h"
#include "multi_button.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

EventGroupHandle_t key_groups;
struct Button button1;    //申请按键
#define USER_KEY_1 9      //实际的GPIO
#define button1_id 1      //按键的ID
#define button1_active 0  //有效电平

struct Button button2;    //申请按键
#define USER_KEY_2 2      //实际的GPIO
#define button2_id 2      //按键的ID
#define button2_active 0  //有效电平


static void button_press_event(void* btn);

static void clock_task_callback(void *arg)
{
  button_ticks();              
}
uint8_t read_button_GPIO(uint8_t button_id)   
{
	switch (button_id)
  {
    case button1_id:
      return gpio_get_level(USER_KEY_1);
    case button2_id:
      return gpio_get_level(USER_KEY_2);
    default:
      break;
  }
  return 1;
}


static void button_gpio_init(void)
{
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_INPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)0x01<<USER_KEY_1) | ((uint64_t)0x01<<USER_KEY_2);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

}

void user_button_init(void)
{
  key_groups = xEventGroupCreate();
  button_gpio_init();
  button_init(&button1, read_button_GPIO, button1_active , button1_id);              // 初始化 初始化对象 回调函数 触发电平 按键ID
  button_attach(&button1,SINGLE_CLICK,button_press_event);           //单击事件
  button_attach(&button1,DOUBLE_CLICK,button_press_event);           //双击事件
  button_attach(&button1,PRESS_DOWN,button_press_event);             //按下事件
  button_attach(&button1,PRESS_UP,button_press_event);               //弹起事件
  button_attach(&button1,PRESS_REPEAT,button_press_event);           //重复按下事件
  button_attach(&button1,LONG_PRESS_START,button_press_event);       //长按触发一次
  button_attach(&button1,LONG_PRESS_HOLD,button_press_event);        //长按一直触发

  button_init(&button2, read_button_GPIO, button2_active , button2_id);              // 初始化 初始化对象 回调函数 触发电平 按键ID
  button_attach(&button2,SINGLE_CLICK,button_press_event);           //单击事件
  button_attach(&button2,DOUBLE_CLICK,button_press_event);           //双击事件
  button_attach(&button2,PRESS_DOWN,button_press_event);             //按下事件
  button_attach(&button2,PRESS_UP,button_press_event);               //弹起事件
  button_attach(&button2,PRESS_REPEAT,button_press_event);           //重复按下事件
  button_attach(&button2,LONG_PRESS_START,button_press_event);       //长按触发一次
  button_attach(&button2,LONG_PRESS_HOLD,button_press_event);        //长按一直触发

  const esp_timer_create_args_t clock_tick_timer_args = 
  {
    .callback = &clock_task_callback,
    .name = "clock_task",
    .arg = NULL,
  };
  esp_timer_handle_t clock_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&clock_tick_timer_args, &clock_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(clock_tick_timer, 1000 * 5)); 
  button_start(&button1); //启动按键1
  button_start(&button2); //启动按键2
}

static void button_press_event(void* btn)
{
  struct Button *user_button = (struct Button *)btn;
  PressEvent event = get_button_event(user_button);
  uint8_t buttonID = user_button->button_id;
  uint32_t eventBits_ = 0x00;
  switch (event)
  {
    case SINGLE_CLICK:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,0);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,7);
          break;
        }
      }
      break;
    }
    case DOUBLE_CLICK:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,1);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,8);
          break;
        }
      }
      break;
    }
    case PRESS_DOWN:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,2);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,9);
          break;
        }
      }
      break;
    }
    case PRESS_UP:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,3);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,10);
          break;
        }
      }
      break;
    }
    case PRESS_REPEAT:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,4);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,11);
          break;
        }
      }
      break;
    }
    case LONG_PRESS_START:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,5);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,12);
          break;
        }
      }
      break;
    }
    case LONG_PRESS_HOLD:
    {
      switch (buttonID)
      {
        case button1_id:
        {
          SET_BIT(eventBits_,6);
          break;
        }
        case button2_id:
        {
          SET_BIT(eventBits_,13);
          break;
        }
      }
      break;
    }
    default:
      return;
  }
  xEventGroupSetBits(key_groups,eventBits_);
  //ESP_LOGE("even","%ld",eventBits_);
}



/*
事件:
SINGLE_CLICK :单击
DOUBLE_CLICK :双击
PRESS_DOWN :按下
PRESS_UP :弹起事件
PRESS_REPEAT :重复按下
LONG_PRESS_START :长按触发一次
LONG_PRESS_HOLD :长按一直触发
*/
