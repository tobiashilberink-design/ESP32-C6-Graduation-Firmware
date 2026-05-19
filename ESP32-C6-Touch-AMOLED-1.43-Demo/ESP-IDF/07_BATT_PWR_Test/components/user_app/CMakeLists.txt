idf_component_register(
  SRCS "user_app.c"
  PRIV_REQUIRES  
  i2c_bsp 
  main   
  button_bsp
  ui_bsp
  espressif__esp_io_expander_tca9554
  INCLUDE_DIRS "./")
