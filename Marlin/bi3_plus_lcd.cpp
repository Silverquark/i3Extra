#include "temperature.h"
#include "ultralcd.h"
#include "bi3_plus_lcd.h"

#include "Marlin.h"
#include "language.h"
#include "cardreader.h"
#include "temperature.h"
#include "stepper.h"
#include "configuration_store.h"
#include "utility.h"
#include "watchdog.h"

#if ENABLED(PRINTCOUNTER)
#include "printcounter.h"
#include "duration_t.h"
#endif

#define OPMODE_NONE 0
#define OPMODE_LEVEL_INIT 1
#define OPMODE_LOAD_FILAMENT 2
#define OPMODE_UNLOAD_FILAMENT 3
#define OPMODE_MOVE 4
#define OPMODE_AUTO_PID 5

uint8_t lcdBuff[26];
uint16_t fileIndex = 0;
millis_t nextOpTime, nextLcdUpdate = 0;
uint8_t opMode = OPMODE_NONE;
uint8_t eventCnt = 0;
uint8_t tempGraphUpdate = 0;
uint8_t lastPage = -1;

//init OK
void lcdSetup() {
  Serial2.begin(115200);

  lcdSendMarlinVersion();
  lcdShowPage(0x01);
}

//lcd status update OK
void lcdTask() {
  readLcdSerial();

  millis_t ms = millis();
  executeLoopedOperation(ms);
  lcdStatusUpdate(ms);
}

void executeLoopedOperation(millis_t ms) {
  if ((opMode != OPMODE_NONE) && (ELAPSED(ms, nextOpTime))) {
    if (opMode == OPMODE_LEVEL_INIT) {
      if (SBI(axis_homed, X_AXIS) && SBI(axis_homed, Y_AXIS) && SBI(axis_homed, Z_AXIS)) {
      //if (axis_homed[X_AXIS] && axis_homed[Y_AXIS] && axis_homed[Z_AXIS]) {//stuck if levelling problem?
        opMode = OPMODE_NONE;
        lcdShowPage(56);//level 2 menu
      }
      else
        nextOpTime = ms + 200;
    }
    else if (opMode == OPMODE_UNLOAD_FILAMENT) {
      if (thermalManager.current_temperature[0] >= thermalManager.target_temperature[0] - 10)
        enqueue_and_echo_commands_P(PSTR("G1 E-1 F120"));
      nextOpTime = ms + 500;
    }
    else if (opMode == OPMODE_LOAD_FILAMENT) {
      if (thermalManager.current_temperature[0] >= thermalManager.target_temperature[0] - 10)
        enqueue_and_echo_commands_P(PSTR("G1 E1 F120"));
      nextOpTime = ms + 500;
    }
  }
}

void lcdStatusUpdate(millis_t ms) {
  if (ELAPSED(ms, nextLcdUpdate)) {
    nextLcdUpdate = ms + 500;

    lcdBuff[0] = 0x5A;
    lcdBuff[1] = 0xA5;
    lcdBuff[2] = 0x0F; //data length
    lcdBuff[3] = 0x82; //write data to sram
    lcdBuff[4] = 0x00; //starting at 0 vp
    lcdBuff[5] = 0x00;

    int tmp = thermalManager.target_temperature[0];
    lcdBuff[6] = highByte(tmp);//0x00 target extruder temp
    lcdBuff[7] = lowByte(tmp);

    tmp = thermalManager.degHotend(0);
    lcdBuff[8] = highByte(tmp);//0x01 extruder temp
    lcdBuff[9] = lowByte(tmp);

    lcdBuff[10] = 0x00;//0x02 target bed temp
    lcdBuff[11] = thermalManager.target_temperature_bed;

    lcdBuff[12] = 0x00;//0x03 bed temp
    lcdBuff[13] = thermalManager.degBed();

    lcdBuff[14] = 0x00; //0x04 fan speed
    lcdBuff[15] = (int16_t)fanSpeeds[0] * 100 / 256;

    lcdBuff[16] = 0x00;//0x05 card progress
    lcdBuff[17] = card.percentDone();

    Serial2.write(lcdBuff, 18);

    if (tempGraphUpdate) {
      if (tempGraphUpdate == 2) {
        tempGraphUpdate = 1;
        updateGraphData();
      }
      else
        tempGraphUpdate = 2;
    }
  }
}

//show page OK
void lcdShowPage(uint8_t pageNumber) {
  lcdBuff[0] = 0x5A;//frame header
  lcdBuff[1] = 0xA5;

  lcdBuff[2] = 0x04;//data length

  lcdBuff[3] = 0x80;//command - write data to register
  lcdBuff[4] = 0x03;

  lcdBuff[5] = 0x00;
  lcdBuff[6] = pageNumber;

  Serial2.write(lcdBuff, 7);
}

//show page OK
uint8_t lcdgetCurrentPage() {
  lcdBuff[0] = 0x5A;//frame header
  lcdBuff[1] = 0xA5;

  lcdBuff[2] = 0x03;//data length

  lcdBuff[3] = 0x81;//command - write read to register
  lcdBuff[4] = 0x03;//register 0x03

  lcdBuff[5] = 0x02;//2bytes

  Serial2.write(lcdBuff, 6);

  uint8_t bytesRead = Serial2.readBytes(lcdBuff, 8);
  if ((bytesRead == 8) && (lcdBuff[0] == 0x5A) && (lcdBuff[1] == 0xA5)) {
    return (uint8_t)lcdBuff[7];
  }

}

//receive data from lcd OK
void readLcdSerial() {
  if (Serial2.available() > 0) {
    if (Serial2.read() != 0x5A)
      return;

    while (Serial2.available() < 1);

    if (Serial2.read() != 0xA5)
      return;

    while (Serial2.available() < 3);

    Serial2.read();//data length
    Serial2.read();//command

    if (Serial2.read() != 4) //VP MSB
      return;

    while (Serial2.available() < 4);

    uint8_t lcdCommand = Serial2.read(); // VP LSB
    Serial2.read();// LEN ?
    Serial2.read(); //KEY VALUE MSB
    uint8_t lcdData = Serial2.read(); //KEY VALUE LSB

    switch (lcdCommand) {
      case 0x32: {//SD list navigation up/down OK
          if (card.sdprinting)
            lcdShowPage(33); //show print menu
          else {
            if (lcdData == 0) {
              card.initsd();
              if (card.cardOK)
                fileIndex = card.getnrfilenames() - 1;
            }

            if (card.cardOK) {
              uint16_t fileCnt = card.getnrfilenames();
              card.getWorkDirName();//??

              if (fileCnt > 5) {
                if (lcdData == 1) { //UP
                  if ((fileIndex + 5) < fileCnt)
                    fileIndex += 5;
                }
                else if (lcdData == 2) { //DOWN
                  if (fileIndex >= 5)
                    fileIndex -= 5;
                }
              }

              lcdBuff[0] = 0x5A;
              lcdBuff[1] = 0xA5;
              lcdBuff[2] = 0x9F;
              lcdBuff[3] = 0x82;
              lcdBuff[4] = 0x01;
              lcdBuff[5] = 0x00;
              Serial2.write(lcdBuff, 6);

              for (uint8_t i = 0; i < 6; i++) {
                card.getfilename(fileIndex - i);
                strncpy(lcdBuff, card.longFilename, 26);
                Serial2.write(lcdBuff, 26);
              }

              lcdShowPage(31); //show sd card menu
            }
          }
          break;
        }
      case 0x33: {//FILE SELECT OK
          if (card.cardOK) {
            if (((fileIndex+10) - lcdData) >= 10)
            {
              card.getfilename(fileIndex - lcdData);

              lcdBuff[0] = 0x5A;
              lcdBuff[1] = 0xA5;
              lcdBuff[2] = 0x1D;
              lcdBuff[3] = 0x82;
              lcdBuff[4] = 0x01;
              lcdBuff[5] = 0x4E;
              Serial2.write(lcdBuff, 6);
              strncpy(lcdBuff, card.longFilename, 26);
              Serial2.write(lcdBuff, 26);

              card.openFile(card.filename, true);
              card.startFileprint();
              print_job_timer.start();

              tempGraphUpdate = 2;

              lcdShowPage(33);//print menu
            }
          }
          break;
        }
      case 0x35: {//print stop OK
          card.stopSDPrint();
          clear_command_queue();
          quickstop_stepper();
          print_job_timer.stop();
          thermalManager.disable_all_heaters();
#if FAN_COUNT > 0
          for (uint8_t i = 0; i < FAN_COUNT; i++) fanSpeeds[i] = 0;
#endif
          tempGraphUpdate = 0;
          lcdShowPage(11); //main menu
          break;
        }
      case 0x36: {//print pause OK
          card.pauseSDPrint();
          print_job_timer.pause();
#if ENABLED(PARK_HEAD_ON_PAUSE)
          enqueue_and_echo_commands_P(PSTR("M125"));
#endif
          break;
        }
      case 0x37: {//print start OK
#if ENABLED(PARK_HEAD_ON_PAUSE)
          enqueue_and_echo_commands_P(PSTR("M24"));
#else
          card.startFileprint();
          print_job_timer.start();
#endif
          break;
        }
      case 0x3C: { //Preheat options
          if (lcdData == 0) {
            //Serial.println(thermalManager.target_temperature[0]);
            //writing preset temps to lcd
            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x0F; //data length
            lcdBuff[3] = 0x82; //write data to sram
            lcdBuff[4] = 0x05; //starting at 0x0570 vp
            lcdBuff[5] = 0x70;

            int tmp = planner.preheat_preset1_hotend;
            lcdBuff[6] = highByte(tmp);
            lcdBuff[7] = lowByte(tmp);
            tmp = planner.preheat_preset1_bed;
            lcdBuff[8] = 0x00;
            lcdBuff[9] = lowByte(tmp);
            tmp = planner.preheat_preset2_hotend;
            lcdBuff[10] = highByte(tmp);
            lcdBuff[11] = lowByte(tmp);
            tmp = planner.preheat_preset2_bed;
            lcdBuff[12] = 0x00;
            lcdBuff[13] = lowByte(tmp);
            tmp = planner.preheat_preset3_hotend;
            lcdBuff[14] = highByte(tmp);
            lcdBuff[15] = lowByte(tmp);
            tmp = planner.preheat_preset3_bed;
            lcdBuff[16] = 0x00;
            lcdBuff[17] = lowByte(tmp);

            Serial2.write(lcdBuff, 18);

            lcdShowPage(39);//open preheat screen
            //Serial.println(thermalManager.target_temperature[0]);
          }
          else {
            //Serial.println(thermalManager.target_temperature[0]);
            //read presets

            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x04; //data length
            lcdBuff[3] = 0x83; //read sram
            lcdBuff[4] = 0x05; //vp 0570
            lcdBuff[5] = 0x70;
            lcdBuff[6] = 0x06; //length

            Serial2.write(lcdBuff, 7);

            //read user entered values from sram
            uint8_t bytesRead = Serial2.readBytes(lcdBuff, 19);
            if ((bytesRead == 19) && (lcdBuff[0] == 0x5A) && (lcdBuff[1] == 0xA5)) {
              planner.preheat_preset1_hotend = (int16_t)lcdBuff[7] * 256 + lcdBuff[8];
              planner.preheat_preset1_bed = (int8_t)lcdBuff[10];
              planner.preheat_preset2_hotend = (int16_t)lcdBuff[11] * 256 + lcdBuff[12];
              planner.preheat_preset2_bed = (int8_t)lcdBuff[14];
              planner.preheat_preset3_hotend = (int16_t)lcdBuff[15] * 256 + lcdBuff[16];
              planner.preheat_preset3_bed = (int8_t)lcdBuff[18];
              enqueue_and_echo_commands_P(PSTR("M500"));
              char command[20];
              if (lcdData == 1) {
                //thermalManager.setTargetHotend(planner.preheat_preset1_hotend, 0);
                //Serial.println(thermalManager.target_temperature[0]);
                sprintf(command, "M104 S%d", planner.preheat_preset1_hotend); //build heat up command (extruder)
                enqueue_and_echo_command((const char*)&command); //enque heat command
                sprintf(command, "M140 S%d", planner.preheat_preset1_bed); //build heat up command (bed)
                enqueue_and_echo_command((const char*)&command); //enque heat command
              } else if (lcdData == 2) {
                sprintf(command, "M104 S%d", planner.preheat_preset2_hotend); //build heat up command (extruder)
                enqueue_and_echo_command((const char*)&command); //enque heat command
                sprintf(command, "M140 S%d", planner.preheat_preset2_bed); //build heat up command (bed)
                enqueue_and_echo_command((const char*)&command); //enque heat command
              } else if (lcdData == 3) {
                sprintf(command, "M104 S%d", planner.preheat_preset3_hotend); //build heat up command (extruder)
                enqueue_and_echo_command((const char*)&command); //enque heat command
                sprintf(command, "M140 S%d", planner.preheat_preset3_bed); //build heat up command (bed)
                enqueue_and_echo_command((const char*)&command); //enque heat command
              }
            }
          }
        }
      case 0x34: {//cool down OK
          thermalManager.disable_all_heaters();
          break;
        }
      case 0x3E: {//send pid/motor config to lcd OK
          lcdBuff[0] = 0x5A;
          lcdBuff[1] = 0xA5;

          lcdBuff[2] = 0x11;

          lcdBuff[3] = 0x82;

          lcdBuff[4] = 0x03;
          lcdBuff[5] = 0x24;

          uint16_t tmp = planner.axis_steps_per_mm[X_AXIS] * 10;
          lcdBuff[6] = highByte(tmp);
          lcdBuff[7] = lowByte(tmp);

          tmp = planner.axis_steps_per_mm[Y_AXIS] * 10;
          lcdBuff[8] = highByte(tmp);
          lcdBuff[9] = lowByte(tmp);

          tmp = planner.axis_steps_per_mm[Z_AXIS] * 10;
          lcdBuff[10] = highByte(tmp);
          lcdBuff[11] = lowByte(tmp);

          tmp = planner.axis_steps_per_mm[E_AXIS] * 10;
          lcdBuff[12] = highByte(tmp);
          lcdBuff[13] = lowByte(tmp);

          tmp = PID_PARAM(Kp, 0) * 10;
          lcdBuff[14] = highByte(tmp);
          lcdBuff[15] = lowByte(tmp);

          tmp = unscalePID_i(PID_PARAM(Ki, 0)) * 10;
          lcdBuff[16] = highByte(tmp);
          lcdBuff[17] = lowByte(tmp);

          tmp = unscalePID_d(PID_PARAM(Kd, 0)) * 10;
          lcdBuff[18] = highByte(tmp);
          lcdBuff[19] = lowByte(tmp);

          Serial2.write(lcdBuff, 20);

          if (lcdData)
            lcdShowPage(45);//show pid screen
          else
            lcdShowPage(47);//show motor screen
          break;
        }
      case 0x3F: {//save pid/motor config OK
          lcdBuff[0] = 0x5A;
          lcdBuff[1] = 0xA5;

          lcdBuff[2] = 0x04;

          lcdBuff[3] = 0x83;

          lcdBuff[4] = 0x03;
          lcdBuff[5] = 0x24;

          lcdBuff[6] = 0x07;

          Serial2.write(lcdBuff, 7);

          uint8_t bytesRead = Serial2.readBytes(lcdBuff, 21);
          if ((bytesRead == 21) && (lcdBuff[0] == 0x5A) && (lcdBuff[1] == 0xA5)) {
            planner.axis_steps_per_mm[X_AXIS] = (float)((uint16_t)lcdBuff[7] * 256 + lcdBuff[8]) / 10;
            //Serial.println(lcdBuff[7]);
            //Serial.println(lcdBuff[8]);
            //Serial.println(lcdBuff[9]);
            //Serial.println(lcdBuff[10]);
            planner.axis_steps_per_mm[Y_AXIS] = (float)((uint16_t)lcdBuff[9] * 256 + lcdBuff[10]) / 10;
            planner.axis_steps_per_mm[Z_AXIS] = (float)((uint16_t)lcdBuff[11] * 256 + lcdBuff[12]) / 10;
            planner.axis_steps_per_mm[E_AXIS] = (float)((uint16_t)lcdBuff[13] * 256 + lcdBuff[14]) / 10;

            PID_PARAM(Kp, 0) = (float)((uint16_t)lcdBuff[15] * 256 + lcdBuff[16]) / 10;
            PID_PARAM(Ki, 0) = scalePID_i((float)((uint16_t)lcdBuff[17] * 256 + lcdBuff[18]) / 10);
            PID_PARAM(Kd, 0) = scalePID_d((float)((uint16_t)lcdBuff[19] * 256 + lcdBuff[20]) / 10);

            enqueue_and_echo_commands_P(PSTR("M500"));
            lcdShowPage(43);//show system menu
          }
          break;
        }
      case 0x42: {//factory reset OK
          enqueue_and_echo_commands_P(PSTR("M502"));
          enqueue_and_echo_commands_P(PSTR("M500"));
          break;
        }
      case 0x47: {//print config open OK
          lcdBuff[0] = 0x5A;
          lcdBuff[1] = 0xA5;

          lcdBuff[2] = 0x0B;

          lcdBuff[3] = 0x82;

          lcdBuff[4] = 0x03;
          lcdBuff[5] = 0x2B;

          lcdBuff[6] = highByte(feedrate_percentage); //0x2B
          lcdBuff[7] = lowByte(feedrate_percentage);

          int temp = thermalManager.degTargetHotend(0);
          lcdBuff[8] = highByte(temp); //0x2C
          lcdBuff[9] = lowByte(temp);

          lcdBuff[10] = 0x00;//0x2D
          lcdBuff[11] = (int)thermalManager.degTargetBed();

          lcdBuff[12] = 0x00;//0x2E
          lcdBuff[13] = (uint16_t)fanSpeeds[0] * 100 / 256;

          Serial2.write(lcdBuff, 14);

          lcdShowPage(35);//print config
          break;
        }
      case 0x40: {//print config save OK
          lcdBuff[0] = 0x5A;
          lcdBuff[1] = 0xA5;

          lcdBuff[2] = 0x04;//4 byte

          lcdBuff[3] = 0x83;//command

          lcdBuff[4] = 0x03;// start addr
          lcdBuff[5] = 0x2B;

          lcdBuff[6] = 0x04; //4 vp

          Serial2.write(lcdBuff, 7);

          uint8_t bytesRead = Serial2.readBytes(lcdBuff, 15);
          if ((bytesRead == 15) && (lcdBuff[0] == 0x5A) && (lcdBuff[1] == 0xA5)) {
            feedrate_percentage = (uint16_t)lcdBuff[7] * 256 + lcdBuff[8];
            thermalManager.setTargetHotend((uint16_t)lcdBuff[9] * 256 + lcdBuff[10], 0);

            thermalManager.setTargetBed(lcdBuff[12]);
            fanSpeeds[0] = (uint16_t)lcdBuff[14] * 256 / 100;
            lcdShowPage(33);// show print menu
          }
          break;
        }
      case 0x4A: {//load/unload filament back OK
          opMode = OPMODE_NONE;
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G90")); // absolute mode
          thermalManager.setTargetHotend(0, 0);
          lcdShowPage(49);//filament menu
          break;
        }
      case 0x4C: {//level menu OK
          if (lcdData == 0) {
            lcdShowPage(55); //level 1
            //axis_homed[X_AXIS] = axis_homed[Y_AXIS] = axis_homed[Z_AXIS] = false;
            SBI(axis_homed, X_AXIS) = SBI(axis_homed, Y_AXIS) = SBI(axis_homed, Z_AXIS) = false;
            enqueue_and_echo_commands_P(PSTR("G90")); //absolute mode
            enqueue_and_echo_commands_P((PSTR("G28")));//homeing
            nextOpTime = millis() + 200;
            opMode = OPMODE_LEVEL_INIT;
          }
          else if (lcdData == 1) { //fl
            enqueue_and_echo_commands_P((PSTR("G1 Z10 F2000")));
            enqueue_and_echo_commands_P((PSTR("G1 X30 Y30 F6000")));
            enqueue_and_echo_commands_P((PSTR("G1 Z0 F1000")));
          }
          else if (lcdData == 2) { //rr
            enqueue_and_echo_commands_P((PSTR("G1 Z10 F2000")));
            enqueue_and_echo_commands_P((PSTR("G1 X170 Y170 F6000")));
            enqueue_and_echo_commands_P((PSTR("G1 Z0 F1000")));
          }
          else if (lcdData == 3) { //fr
            enqueue_and_echo_commands_P((PSTR("G1 Z10 F2000")));
            enqueue_and_echo_commands_P((PSTR("G1 X170 Y30 F6000")));
            enqueue_and_echo_commands_P((PSTR("G1 Z0 F1000")));
          }
          else if (lcdData == 4) { //rl
            enqueue_and_echo_commands_P((PSTR("G1 Z10 F2000")));
            enqueue_and_echo_commands_P((PSTR("G1 X30 Y170 F6000")));
            enqueue_and_echo_commands_P((PSTR("G1 Z0 F1000")));
          }
          else if (lcdData == 5) { //c
            enqueue_and_echo_commands_P((PSTR("G1 Z10 F2000")));
            enqueue_and_echo_commands_P((PSTR("G1 X100 Y100 F6000")));
            enqueue_and_echo_commands_P((PSTR("G1 Z0 F1000")));
          }
          else if (lcdData == 6) { //back
            enqueue_and_echo_commands_P((PSTR("G1 Z30 F2000")));
            lcdShowPage(37); //tool menu
          }
          break;
        }

      case 0x51: { //load_unload_menu

          if (lcdData == 0) {
            //writing default temp to lcd
            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x05; //data length
            lcdBuff[3] = 0x82; //write data to sram
            lcdBuff[4] = 0x05; //starting at 0x0500 vp
            lcdBuff[5] = 0x20;
            lcdBuff[6] = 0x00;
            lcdBuff[7] = 0xC8; //extruder temp (200)
            Serial2.write(lcdBuff, 8);

            lcdShowPage(49);//open load/unload_menu
          }
          else if (lcdData == 1 || lcdData == 2) {
            //read bed/hotend temp

            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x04; //data length
            lcdBuff[3] = 0x83; //read sram
            lcdBuff[4] = 0x05; //vp 0520
            lcdBuff[5] = 0x20;
            lcdBuff[6] = 0x01; //length

            Serial2.write(lcdBuff, 7);

            //read user entered values from sram
            uint8_t bytesRead = Serial2.readBytes(lcdBuff, 9);
            if ((bytesRead == 9) && (lcdBuff[0] == 0x5A) && (lcdBuff[1] == 0xA5)) {
              int16_t hotendTemp = (int16_t)lcdBuff[7] * 256 + lcdBuff[8];
              Serial.println(hotendTemp);
              char command[20];
              thermalManager.setTargetHotend(hotendTemp, 0);
              sprintf(command, "M104 S%d", hotendTemp); //build auto pid command (extruder)
              //enqueue_and_echo_command((const char*)&command); //enque pid command
              enqueue_and_echo_commands_P(PSTR("G91")); // relative mode
              nextOpTime = millis() + 500;
              if (lcdData == 1) {
                opMode = OPMODE_LOAD_FILAMENT;
              }
              else if (lcdData == 2) {
                opMode = OPMODE_UNLOAD_FILAMENT;
              }
            }
          }
          break;
        }
      case 0x00: {
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G91"));
          enqueue_and_echo_commands_P(PSTR("G1 X5 F3000"));
          enqueue_and_echo_commands_P(PSTR("G90"));
          break;
        }
      case 0x01: {
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G91"));
          enqueue_and_echo_commands_P(PSTR("G1 X-5 F3000"));
          enqueue_and_echo_commands_P(PSTR("G90"));

          break;
        }
      case 0x02: {
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G91"));
          enqueue_and_echo_commands_P(PSTR("G1 Y5 F3000"));
          enqueue_and_echo_commands_P(PSTR("G90"));

          break;
        }
      case 0x03: {
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G91"));
          enqueue_and_echo_commands_P(PSTR("G1 Y-5 F3000"));
          enqueue_and_echo_commands_P(PSTR("G90"));

          break;
        }
      case 0x04: {
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G91"));
          enqueue_and_echo_commands_P(PSTR("G1 Z0.5 F3000"));
          enqueue_and_echo_commands_P(PSTR("G90"));

          break;
        }
      case 0x05: {
          clear_command_queue();
          enqueue_and_echo_commands_P(PSTR("G91"));
          enqueue_and_echo_commands_P(PSTR("G1 Z-0.5 F3000"));
          enqueue_and_echo_commands_P(PSTR("G90"));

          break;
        }
      case 0x06: {
          if (thermalManager.degHotend(0) >= 180) {
            clear_command_queue();
            enqueue_and_echo_commands_P(PSTR("G91"));
            enqueue_and_echo_commands_P(PSTR("G1 E1 F120"));
            enqueue_and_echo_commands_P(PSTR("G90"));
          }
          break;
        }
      case 0x07: {
          if (thermalManager.degHotend(0) >= 180) {
            clear_command_queue();
            enqueue_and_echo_commands_P(PSTR("G91"));
            enqueue_and_echo_commands_P(PSTR("G1 E-1 F120"));
            enqueue_and_echo_commands_P(PSTR("G90"));
          }
          break;
        }
      case 0x54: {//disable motors OK!!!
          enqueue_and_echo_commands_P(PSTR("M84"));
          //axis_homed[X_AXIS] = axis_homed[Y_AXIS] = axis_homed[Z_AXIS] = false;
          SBI(axis_homed, X_AXIS) = SBI(axis_homed, Y_AXIS) = SBI(axis_homed, Z_AXIS) = false;
          break;
        }
      case 0x43: {//home x OK!!!
          enqueue_and_echo_commands_P(PSTR("G28 X0"));
          break;
        }
      case 0x44: {//home y OK!!!
          enqueue_and_echo_commands_P(PSTR("G28 Y0"));
          break;
        }
      case 0x45: {//home z OK!!!
          enqueue_and_echo_commands_P(PSTR("G28 Z0"));
          break;
        }
      case 0x1C: {//home xyz OK!!!
          enqueue_and_echo_commands_P(PSTR("G28"));
          break;
        }
      case 0x5B: { //stats menu
          //sending stats to lcd
          lcdSendStats();

          lcdShowPage(59);//open stats screen on lcd
          break;
        }
      case 0x5C: { //auto pid menu

          if (lcdData == 0) {
            //writing default temp to lcd
            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x05; //data length
            lcdBuff[3] = 0x82; //write data to sram
            lcdBuff[4] = 0x05; //starting at 0x0500 vp
            lcdBuff[5] = 0x20;
            lcdBuff[6] = 0x00;
            lcdBuff[7] = 0xC8; //extruder temp (200)
            Serial2.write(lcdBuff, 8);

            lcdShowPage(61);//open auto pid screen
          }
          else if (lcdData == 1) { //auto pid start button pressed (1=hotend,2=bed)
            //read bed/hotend temp

            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x04; //data length
            lcdBuff[3] = 0x83; //read sram
            lcdBuff[4] = 0x05; //vp 0520
            lcdBuff[5] = 0x20;
            lcdBuff[6] = 0x01; //length

            Serial2.write(lcdBuff, 7);

            //read user entered values from sram
            uint8_t bytesRead = Serial2.readBytes(lcdBuff, 9);
            if ((bytesRead == 9) && (lcdBuff[0] == 0x5A) && (lcdBuff[1] == 0xA5)) {
              uint16_t hotendTemp = (uint16_t)lcdBuff[7] * 256 + lcdBuff[8];
              //Serial.println(hotendTemp);
              char command[20];
              sprintf(command, "M303 S%d E0 C8 U1", hotendTemp); //build auto pid command (extruder)
              enqueue_and_echo_command("M106 S255"); //Turn on fan
              enqueue_and_echo_command((const char*)&command); //enque pid command
              tempGraphUpdate = 2;

            }
          }
          break;
        }
      case 0x3D: { //Close temp screen
          if (lcdData == 1) {
            tempGraphUpdate = 0;
            Serial.println(lastPage);
            lcdShowPage(lastPage);
          }
          else {
            lastPage = lcdgetCurrentPage();
            Serial.println(lastPage);
            tempGraphUpdate = 2;
            lcdShowPage(63);
          }
        }
      case 0x55: { //enter print menu without selecting file
          tempGraphUpdate = 2;
          if(card.sdprinting == false)
          {
            lcdBuff[0] = 0x5A;
            lcdBuff[1] = 0xA5;
            lcdBuff[2] = 0x1D;
            lcdBuff[3] = 0x82;
            lcdBuff[4] = 0x01;
            lcdBuff[5] = 0x4E;
            Serial2.write(lcdBuff, 6);
            strncpy(lcdBuff, "No SD print", 26);
            Serial2.write(lcdBuff, 26);
          }
          lcdShowPage(33);//print menu
        }
      /*case 0xFF: {
          lcdShowPage(58); //enable lcd bridge mode
          while (1) {
            watchdog_reset();
            if (Serial.available())
              Serial2.write(Serial.read());
            if (Serial2.available())
              Serial.write(Serial2.read());
          }
          break;
        }*/
      default:
        break;
    }
  }
}

void lcdSendMarlinVersion() {
  lcdBuff[0] = 0x5A;
  lcdBuff[1] = 0xA5;
  lcdBuff[2] = 0x12; //data length
  lcdBuff[3] = 0x82; //write data to sram
  lcdBuff[4] = 0x05; //starting at 0x0500 vp
  lcdBuff[5] = 0x00;

  strncpy((char*)lcdBuff + 6, SHORT_BUILD_VERSION, 15);
  Serial2.write(lcdBuff, 21);
}

void lcdSendStats() {
  char buffer[21];
  printStatistics stats = print_job_timer.getStats();
  duration_t elapsed = stats.printTime;

  lcdBuff[0] = 0x5A;
  lcdBuff[1] = 0xA5;
  lcdBuff[2] = 0x07;//data length
  lcdBuff[3] = 0x82;//write data to sram
  lcdBuff[4] = 0x05; //starting at 0x5040 vp
  lcdBuff[5] = 0x40;
  //Total prints (including aborted)
  lcdBuff[6] = highByte((int16_t)stats.totalPrints);
  lcdBuff[7] = lowByte((int16_t)stats.totalPrints);
  //Finished prints
  lcdBuff[8] = highByte((int16_t)stats.finishedPrints);
  lcdBuff[9] = lowByte((int16_t)stats.finishedPrints);
  Serial2.write(lcdBuff, 10);

  //total print time
  lcdBuff[0] = 0x5A;
  lcdBuff[1] = 0xA5;
  lcdBuff[2] = 0x12; //data length
  lcdBuff[3] = 0x82; //write data to sram
  lcdBuff[4] = 0x05; //starting at 0x0542 vp
  lcdBuff[5] = 0x42;

  elapsed.toString(buffer);
  strncpy((char*)lcdBuff + 6, buffer, 15);
  Serial2.write(lcdBuff, 21);

  //longest print time
  lcdBuff[0] = 0x5A;
  lcdBuff[1] = 0xA5;
  lcdBuff[2] = 0x12; //data length
  lcdBuff[3] = 0x82; //write data to sram
  lcdBuff[4] = 0x05; //starting at 0x054D vp
  lcdBuff[5] = 0x4D;
  elapsed = stats.longestPrint;
  elapsed.toString(buffer);
  strncpy((char*)lcdBuff + 6, buffer, 15);
  Serial2.write(lcdBuff, 21);

  //total filament used
  lcdBuff[0] = 0x5A;
  lcdBuff[1] = 0xA5;
  lcdBuff[2] = 0x12; //data length
  lcdBuff[3] = 0x82; //write data to sram
  lcdBuff[4] = 0x05; //starting at 0x0558 vp
  lcdBuff[5] = 0x58;
  sprintf_P(buffer, PSTR("%ld.%im"), long(stats.filamentUsed / 1000), int(stats.filamentUsed / 100) % 10);
  strncpy((char*)lcdBuff + 6, buffer, 15);
  Serial2.write(lcdBuff, 21);

}

void updateGraphData() {
  lcdBuff[0] = 0x5A;
  lcdBuff[1] = 0xA5;
  lcdBuff[2] = 0x06; //data length
  lcdBuff[3] = 0x84; //update curve
  lcdBuff[4] = 0x03; //channels 0,1
  int16_t hotend = round(thermalManager.degHotend(0));
  lcdBuff[5] = highByte(hotend);//TODOME
  lcdBuff[6] = lowByte(hotend);//TODOME
  lcdBuff[7] = 0x00;//0x03 bed temp
  lcdBuff[8] = thermalManager.degBed();

  Serial2.write(lcdBuff, 9);
}
