#include "prisma_touch.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TP_SDA GPIO_NUM_42
#define TP_SCL GPIO_NUM_41
#define TP_IRQ GPIO_NUM_48
#define TP_RST GPIO_NUM_47
#define TP_ADDR 0x15

static const char *TAG="PRISMA_TOUCH";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;

bool prisma_touch_init(void){
 gpio_config_t r={.pin_bit_mask=1ULL<<TP_RST,.mode=GPIO_MODE_OUTPUT};
 if(gpio_config(&r)!=ESP_OK)return false;
 gpio_set_level(TP_RST,0);
 vTaskDelay(pdMS_TO_TICKS(30));
 gpio_set_level(TP_RST,1);
 vTaskDelay(pdMS_TO_TICKS(80));
 i2c_master_bus_config_t bc={.i2c_port=I2C_NUM_0,.sda_io_num=TP_SDA,.scl_io_num=TP_SCL,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
 if(i2c_new_master_bus(&bc,&bus)!=ESP_OK){ESP_LOGE(TAG,"I2C bus failed");return false;}
 i2c_device_config_t dc={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=TP_ADDR,.scl_speed_hz=400000};
 if(i2c_master_bus_add_device(bus,&dc,&dev)!=ESP_OK){ESP_LOGE(TAG,"CST816 add failed");return false;}
 uint8_t reg=0xA7,id=0;
 if(i2c_master_transmit_receive(dev,&reg,1,&id,1,100)!=ESP_OK){ESP_LOGE(TAG,"CST816 not found");return false;}
 ESP_LOGI(TAG,"CST816 READY chip=0x%02X",id);
 return true;
}

bool prisma_touch_read(uint16_t *x,uint16_t *y){
 uint8_t reg=0x02,d[5]={0};
 if(!dev||i2c_master_transmit_receive(dev,&reg,1,d,sizeof(d),50)!=ESP_OK)return false;
 if((d[0]&0x0F)==0)return false;
 *x=((d[1]&0x0F)<<8)|d[2];
 *y=((d[3]&0x0F)<<8)|d[4];
 if(*x>239){*x=239;}
 if(*y>239){*y=239;}
 return true;
}
