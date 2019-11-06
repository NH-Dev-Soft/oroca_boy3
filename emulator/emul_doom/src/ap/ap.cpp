/*
 * ap.cpp
 *
 *  Created on: 2019. 6. 14.
 *      Author: HanCheol Cho
 */




#include "ap.h"



extern uint32_t _flash_tag_addr;
extern uint32_t _flash_fw_addr;



__attribute__((section(".tag"))) flash_tag_t fw_tag =
    {
     // fw info
     //
     0xAAAA5555,        // magic_number
     "V191106R1",       // version_str
     "OROCABOY3",       // board_str
     "DOOM",           // name
     __DATE__,
     __TIME__,
     (uint32_t)&_flash_tag_addr,
     (uint32_t)&_flash_fw_addr,


     // tag info
     //
    };




extern "C"
{
void D_DoomMain (void);
}

static void threadEmul(void const *argument);


void apInit(void)
{
  //uint32_t *p_data2 = (uint32_t *)0x30000001;
  //p_data2[0] = 1;


  uartOpen(_DEF_UART1, 57600);
  uartOpen(_DEF_UART2, 57600);
  cmdifOpen(_DEF_UART1, 57600);

  uint8_t *p_data[100];
  int i;

  for (i=0; i<100; i++)
  {
    p_data[i] = (uint8_t *)memMalloc(1);
  }
  for (i=0; i<100; i++)
  {
    memFree(p_data[i]);
  }


  osThreadDef(threadEmul, threadEmul, _HW_DEF_RTOS_THREAD_PRI_EMUL, 0, _HW_DEF_RTOS_THREAD_MEM_EMUL);
  if (osThreadCreate(osThread(threadEmul), NULL) != NULL)
  {
    logPrintf("threadEmul \t\t: OK\r\n");
  }
  else
  {
    logPrintf("threadEmul \t\t: Fail\r\n");
    while(1);
  }

}

void apMain(void)
{
  uint32_t pre_time;

  while(1)
  {
    cmdifMain();

    if (millis()-pre_time >= 500)
    {
      pre_time = millis();
      ledToggle(_DEF_LED1);
    }
    osThreadYield();
  }
}



static void threadEmul(void const *argument)
{
  UNUSED(argument);


  D_DoomMain();


  while(1)
  {
    delay(100);
  }
}
