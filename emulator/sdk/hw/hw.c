/*
 * hw.c
 *
 *  Created on: 2019. 6. 14.
 *      Author: HanCheol Cho
 */




#include "hw.h"



void bootCmdif(void);


extern uint32_t _flash_tag_addr;
extern uint32_t _flash_fw_addr;



__attribute__((section(".tag"))) flash_tag_t fw_tag =
    {
     // fw info
     //
     0xAAAA5555,        // magic_number
     "V191101R1",       // version_str
     "OROCABOY3",       // board_str
     "pNesX",           // name
     __DATE__,
     __TIME__,
     (uint32_t)&_flash_tag_addr,
     (uint32_t)&_flash_fw_addr,


     // tag info
     //
    };




void hwInit(void)
{
  bspInit();

  resetInit();
  microsInit();
  millisInit();
  delayInit();
  cmdifInit();
  swtimerInit();

  uartInit();
  uartOpen(_DEF_UART1, 57600);

  logPrintf("\n\n[ Firmware Begin... ]\r\n");
  logPrintf("Booting..Board\t\t: %s\r\n", fw_tag.board_str);
  logPrintf("Booting..Name \t\t: %s\r\n", fw_tag.name_str);
  logPrintf("Booting..Ver  \t\t: %s\r\n", fw_tag.version_str);


  rtcInit();
  logPrintf("ResetBits \t\t: 0x%X\n", (int)rtcReadBackupData(_HW_DEF_RTC_RESET_SRC));

  resetLog();

  pwmInit();
  ledInit();
  gpioInit();
  adcInit();

  sdramInit();
  //qspiInit();
  //qspiEnableMemoryMappedMode();

  flashInit();
  buttonInit();
  i2cInit();
  eepromInit();
  if (eepromValid(0) == true)
  {
    logPrintf("eeprom %dKB \t\t: OK\r\n", (int)eepromGetLength()/1024);
  }
  else
  {
    logPrintf("eeprom %dKB \t\t: Fail\r\n", (int)eepromGetLength()/1024);
  }


  usbInit();
  vcpInit();
  ltdcInit();
  lcdInit();
  dacInit();
  timerInit();
  speakerInit();


  if (sdInit() == true)
  {
    fatfsInit();
  }

  logPrintf("Start...\r\n");

  cmdifAdd("boot", bootCmdif);


  lcdDisplayOn();
}

void hwJumpToBoot(void)
{
  rtcWriteBackupData(_HW_DEF_RTC_BOOT_MODE, (1<<7));
  resetRunSoftReset();
}


void hwJumpToFw(uint32_t addr)
{
  void (**jump_func)(void) = (void (**)(void))(addr + 4);

  bspDeInit();
  (*jump_func)();
}

void hwRunFw(uint32_t fw_index)
{
  uint32_t addr;

  addr  = QSPI_FW_ADDR(fw_index);
  addr += QSPI_FW_TAG;

  logPrintf("hwRunFw : 0x%X\n", (int)addr);
  hwJumpToFw(addr);
}


void bootCmdif(void)
{

  if (cmdifGetParamCnt() == 1)
  {
    if(cmdifHasString("0x5555AAAA", 0) == true)
    {
      cmdifPrintf( "jump to boot\n");
      delay(100);
      hwJumpToBoot();
    }

    if(cmdifHasString("reset", 0) == true)
    {
      cmdifPrintf( "reset\n");
      delay(100);
      rtcWriteBackupData(_HW_DEF_RTC_BOOT_MODE, 0);
      resetRunSoftReset();
    }

  }
}