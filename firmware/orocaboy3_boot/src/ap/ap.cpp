/*
 * ap.cpp
 *
 *  Created on: 2019. 6. 14.
 *      Author: HanCheol Cho
 */




#include "ap.h"
#include "boot/boot.h"


#define MAX_BOOT_CH     1



#define BOOT_MODE_LOADER      0
#define BOOT_MODE_CMDIF       1
#define BOOT_MODE_JUMP_FW     2


uint8_t boot_mode = BOOT_MODE_LOADER;



static cmd_t cmd_boot[MAX_BOOT_CH];


void apInit(void)
{
  uartOpen(_DEF_UART1, 57600);
  cmdifOpen(_DEF_UART1, 57600);

  cmdInit(&cmd_boot[0]);
  cmdBegin(&cmd_boot[0], _DEF_UART1, 57600);


  if (buttonGetPressed(_DEF_BUTTON1) == true || hwGetResetCount() == 2)
  {
    boot_mode = BOOT_MODE_CMDIF;
  }
  else if (hwGetResetCount() == 1)
  {
    boot_mode = BOOT_MODE_LOADER;
  }
  else
  {
    boot_mode = BOOT_MODE_JUMP_FW;
  }


  switch(boot_mode)
  {
    case BOOT_MODE_LOADER:
      logPrintf("boot begin...\r\n");
      break;

    case BOOT_MODE_CMDIF:
      logPrintf("cmdif begin...\r\n");
      break;

    case BOOT_MODE_JUMP_FW:
      logPrintf("jump fw...\r\n");

      if (bootVerifyCrc() != true)
      {
        logPrintf("fw crc    \t\t: Fail\r\n");
        logPrintf("boot begin...\r\n");
        boot_mode = BOOT_MODE_LOADER;
      }
      else
      {
        delay(100);
        bootJumpToFw();
      }
      break;
  }

}

void apMain(void)
{
  uint32_t pre_time;
  uint32_t i;


  while(1)
  {
    if (boot_mode == BOOT_MODE_LOADER)
    {
      for (i=0; i<MAX_BOOT_CH; i++)
      {
        if (cmdReceivePacket(&cmd_boot[i]) == true)
        {
          bootProcessCmd(&cmd_boot[i]);
        }
      }
    }
    else
    {
      cmdifMain();
    }

    if (millis()-pre_time >= 100)
    {
      pre_time = millis();
      ledToggle(_DEF_LED1);
    }
  }
}
