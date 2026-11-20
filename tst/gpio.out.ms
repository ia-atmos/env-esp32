[drv 0ms] gpio.setup 2 output
[drv 0ms] gpio.write 2 1
read	1
[drv 0ms] gpio.setup 4 input
[drv 0ms] gpio.watch 4 change
gpio	4	1	rising	100
gpio	4	0	falling	250
[drv 250ms] gpio.unwatch 4
[drv 250ms] gpio.setup_pwm 0 25000 10
[drv 250ms] gpio.attach_pwm 19 0
[drv 250ms] gpio.write_pwm 0 0
[drv 250ms] gpio.fade_pwm 0 1023 2000
fade	0	2250	2250
adc	0	0	0
catch	false	gpio.write : GPIO pin 34 cannot be used as output
catch	false	gpio.write_pwm : duty out of range for channel resolution
catch	false	gpio.setup : expected a number as argument 1
