/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "gateway_app.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for linkTask */
osThreadId_t linkTaskHandle;
const osThreadAttr_t linkTask_attributes = {
  .name = "linkTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for fieldTask */
osThreadId_t fieldTaskHandle;
const osThreadAttr_t fieldTask_attributes = {
  .name = "fieldTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for modbusTask */
osThreadId_t modbusTaskHandle;
const osThreadAttr_t modbusTask_attributes = {
  .name = "modbusTask",
  .stack_size = 768 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for supervisorTask */
osThreadId_t supervisorTaskHandle;
const osThreadAttr_t supervisorTask_attributes = {
  .name = "supervisorTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void StartStorageTask(void *argument);

/* USER CODE END FunctionPrototypes */

void StartLinkTask(void *argument);
void StartFieldTask(void *argument);
void StartModbusTask(void *argument);
void StartSupervisorTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  configASSERT(GatewayApp_Init());

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of linkTask */
  linkTaskHandle = osThreadNew(StartLinkTask, NULL, &linkTask_attributes);

  /* creation of fieldTask */
  fieldTaskHandle = osThreadNew(StartFieldTask, NULL, &fieldTask_attributes);

  /* creation of modbusTask */
  modbusTaskHandle = osThreadNew(StartModbusTask, NULL, &modbusTask_attributes);

  /* creation of supervisorTask */
  supervisorTaskHandle = osThreadNew(StartSupervisorTask, NULL, &supervisorTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  const osThreadAttr_t storage_attributes = { .name="storageTask", .stack_size=4096, .priority=osPriorityBelowNormal };
  extern void StartStorageTask(void *argument);
  configASSERT(osThreadNew(StartStorageTask, NULL, &storage_attributes) != NULL);
  configASSERT(linkTaskHandle != NULL);
  configASSERT(fieldTaskHandle != NULL);
  configASSERT(modbusTaskHandle != NULL);
  configASSERT(supervisorTaskHandle != NULL);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartLinkTask */
/**
* @brief Function implementing the linkTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartLinkTask */
void StartLinkTask(void *argument)
{
  /* USER CODE BEGIN StartLinkTask */
  (void)argument;
  GatewayApp_LinkTask();
  vTaskDelete(NULL);
  /* USER CODE END StartLinkTask */
}

/* USER CODE BEGIN Header_StartFieldTask */
/**
* @brief Function implementing the fieldTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartFieldTask */
void StartFieldTask(void *argument)
{
  /* USER CODE BEGIN StartFieldTask */
  (void)argument;
  GatewayApp_FieldTask();
  vTaskDelete(NULL);
  /* USER CODE END StartFieldTask */
}

/* USER CODE BEGIN Header_StartModbusTask */
/**
* @brief Function implementing the modbusTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartModbusTask */
void StartModbusTask(void *argument)
{
  /* USER CODE BEGIN StartModbusTask */
  (void)argument;
  GatewayApp_ModbusTask();
  vTaskDelete(NULL);
  /* USER CODE END StartModbusTask */
}

/* USER CODE BEGIN Header_StartSupervisorTask */
/**
* @brief Function implementing the supervisorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSupervisorTask */
void StartSupervisorTask(void *argument)
{
  /* USER CODE BEGIN StartSupervisorTask */
  (void)argument;
  GatewayApp_SupervisorTask();
  vTaskDelete(NULL);
  /* USER CODE END StartSupervisorTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void StartStorageTask(void *argument) { (void)argument; GatewayApp_StorageTask(); }

/* USER CODE END Application */

