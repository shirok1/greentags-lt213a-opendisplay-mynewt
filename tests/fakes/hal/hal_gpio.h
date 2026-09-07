#define HAL_GPIO_PULL_NONE 0
int hal_gpio_init_out(int pin, int value);
int hal_gpio_init_in(int pin, int pull);
void hal_gpio_write(int pin, int value);
int hal_gpio_read(int pin);
