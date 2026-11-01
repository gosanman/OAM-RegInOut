#pragma once

#define DEVICE_ID                       "Reg-InOut"
#define HARDWARE_NAME                   "Reg-InOut"
#define DEVICE_NAME                     "Reg-InOut"

#define INFO1_LED_PIN                   6
#define INFO1_LED_PIN_ACTIVE_ON         HIGH
#define INFO2_LED_PIN                   24
#define INFO2_LED_PIN_ACTIVE_ON         HIGH
#define INFO3_LED_PIN                   22
#define INFO3_LED_PIN_ACTIVE_ON         HIGH

#define FUNC1_BUTTON_PIN                7

#define PROG_LED_PIN                    25
#define PROG_LED_PIN_ACTIVE_ON          HIGH
#define PROG_BUTTON_PIN                 23
#define PROG_BUTTON_PIN_INTERRUPT_ON    FALLING
#define SAVE_INTERRUPT_PIN              3
//#define KNX_SERIAL Serial1
#define KNX_UART_NUM                    0 
#define KNX_UART_RX_PIN                 1
#define KNX_UART_TX_PIN                 0

#define OPENKNX_BI_GPIO_COUNT           2
#define OPENKNX_BI_GPIO_PINS            16, 17 // REG1 App Connector 16=1, 17=2
#define OPENKNX_BI_ONLEVEL              LOW

// monostabile Relais: ein Pin je Kanal, der Pegel ist der Relaiszustand
#define OPENKNX_SWA_CHANNEL_COUNT       2
#define OPENKNX_SWA_PINS                26, 27 // REG1 App Connector 26=4, 27=5
#define OPENKNX_SWA_ACTIVE_ON           HIGH