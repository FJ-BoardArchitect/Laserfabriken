/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

#include "stm32g4xx_ll_ucpd.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_cortex.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_system.h"
#include "stm32g4xx_ll_utils.h"
#include "stm32g4xx_ll_pwr.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_dma.h"

#include "stm32g4xx_ll_exti.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define TEC_EN_Pin GPIO_PIN_14
#define TEC_EN_GPIO_Port GPIOC
#define TEC_PG_Pin GPIO_PIN_15
#define TEC_PG_GPIO_Port GPIOC
#define LASER_RX_Pin GPIO_PIN_0
#define LASER_RX_GPIO_Port GPIOC
#define LASER_TX_Pin GPIO_PIN_1
#define LASER_TX_GPIO_Port GPIOC
#define LASER_EN_Pin GPIO_PIN_0
#define LASER_EN_GPIO_Port GPIOA
#define LASER_PG_Pin GPIO_PIN_1
#define LASER_PG_GPIO_Port GPIOA
#define STAGE_TX_Pin GPIO_PIN_2
#define STAGE_TX_GPIO_Port GPIOA
#define STAGE_RX_Pin GPIO_PIN_3
#define STAGE_RX_GPIO_Port GPIOA
#define ADC_CS_Pin GPIO_PIN_4
#define ADC_CS_GPIO_Port GPIOA
#define TEC_VMEAS_Pin GPIO_PIN_6
#define TEC_VMEAS_GPIO_Port GPIOA
#define TEC_IMEAS_Pin GPIO_PIN_7
#define TEC_IMEAS_GPIO_Port GPIOA
#define EN_5V_USB_Pin GPIO_PIN_12
#define EN_5V_USB_GPIO_Port GPIOB
#define AUX_EN_Pin GPIO_PIN_13
#define AUX_EN_GPIO_Port GPIOB
#define AUX_PG_Pin GPIO_PIN_14
#define AUX_PG_GPIO_Port GPIOB
#define STAGE_EN_Pin GPIO_PIN_15
#define STAGE_EN_GPIO_Port GPIOB
#define STAGE_PG_Pin GPIO_PIN_6
#define STAGE_PG_GPIO_Port GPIOC
#define STAGE_P1_Pin GPIO_PIN_7
#define STAGE_P1_GPIO_Port GPIOC
#define STAGE_P2_Pin GPIO_PIN_8
#define STAGE_P2_GPIO_Port GPIOC
#define STAGE_P3_Pin GPIO_PIN_9
#define STAGE_P3_GPIO_Port GPIOC
#define STAGE_P4_Pin GPIO_PIN_8
#define STAGE_P4_GPIO_Port GPIOA
#define TEC_POLARITY_Pin GPIO_PIN_9
#define TEC_POLARITY_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
