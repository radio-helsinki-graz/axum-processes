/****************************************************************************
**
** Copyright (C) 2007-2009 D&R Electronica Weesp B.V. All rights reserved.
**
** This file is part of the Axum/MambaNet digital mixing system.
**
** This file may be used under the terms of the GNU General Public
** License version 2.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of
** this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/
#include "common.h"
#include "engine.h"
#include "db.h"
#include "dsp.h"
#include "mbn.h"
#include "backup.h"
#include "limits.h"

#include <stdio.h>
#include <stdlib.h>         //for atoi
#include <unistd.h>         //for STDIN_FILENO/close/write
#include <fcntl.h>          //for GET_FL/SET_FL/O_XXXXXX/FNDELAY
#include <string.h>         //for memcpy/strncpy
#include <termios.h>            //for termios
#include <sys/ioctl.h>          //for ioctl
#include <float.h>
#include "engine_functions.h"

#include <arpa/inet.h>      //for AF_PACKET/SOCK_DGRAM/htons/ntohs/socket/bind/sendto
#include <linux/if_arp.h>   //for ETH_P_ALL/ifreq/sockaddr_ll/ETH_ALEN etc...
#include <sys/time.h>       //for setittimer
#include <sys/times.h>      //for tms and times()
#include <sys/signal.h>     //for SIGALRM
#include <sys/un.h>         //for sockaddr_un (UNIX sockets)

#include <errno.h>          //errno and EINTR
#include <time.h>               //nanosleep
#include <sys/mman.h>       //for mmap, PROT_READ, PROT_WRITE

#include <math.h>               //for pow10, log10

#include "ddpci2040.h"

#include <pthread.h>
#include <time.h>
#include <sys/errno.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>

/* deadlock warning: don't use sql_lock() within this mutex */
pthread_mutex_t get_node_info_mutex;
pthread_mutex_t axum_data_mutex;

#define LOG_DEBUG_ENABLED

#ifdef LOG_DEBUG_ENABLED
  #define LOG_DEBUG(...) log_write(__VA_ARGS__)
#else
  #define LOG_DEBUG(...)
#endif

#define DEFAULT_UNIX_HWPARENT_PATH "/tmp/hwparent.socket"
#define DEFAULT_UNIX_MAMBANET_PATH "/tmp/axum-gateway.socket"
#define DEFAULT_ETH_DEV            "eth0"
#define DEFAULT_DB_STR             "dbname='axum' user='axum'"
#define DEFAULT_LOG_FILE           "/var/log/axum-engine.log"
#define DEFAULT_BACKUP_FILE        "/var/lib/axum/.backup"

#ifndef UNIX_PATH_MAX
# define UNIX_PATH_MAX 108
#endif

#define PCB_MAJOR_VERSION        1
#define PCB_MINOR_VERSION        0

#define FIRMWARE_MAJOR_VERSION   2
#define FIRMWARE_MINOR_VERSION   5

#define MANUFACTURER_ID          0x0001 //D&R
#define PRODUCT_ID               0x000E //Axum Engine

#define NR_OF_STATIC_OBJECTS    (1023-1023)
#define NR_OF_OBJECTS            NR_OF_STATIC_OBJECTS

#define SELECT_TIMEOUT          10000

/********************************/
/* global declarations          */
/********************************/
struct mbn_interface *itf;
struct mbn_handler *mbn;
char error[MBN_ERRSIZE];

struct mbn_node_info this_node =
{
  0, MBN_ADDR_SERVICES_ENGINE,          //MambaNet address, Services
  "Axum Engine (Linux)",                //Description
  "Axum-Engine",                        //Name
  MANUFACTURER_ID, PRODUCT_ID, 0x0001,
  0, 0,                                 //Hw revision
  FIRMWARE_MAJOR_VERSION, FIRMWARE_MINOR_VERSION, //Fw revision
  0, 0,                                 //FPGA revision
  NR_OF_OBJECTS,                        //Number of objects
  0,                                    //Default engine address
  {0x0000, 0x0000, 0x0000},             //Hardware parent
  0                                     //Service request
};

int verbose = 0;

int AxumApplicationAndDSPInitialized = 0;

int cntDebugObject=1024;
int cntDebugNodeObject=0;
float cntFloatDebug = 0;

unsigned char VUMeter = 0;
long LevelMeterFrequency = 5; //20Hz
long PhaseMeterFrequency = 10; //10Hz

AXUM_DATA_STRUCT AxumData;
matrix_sources_struct matrix_sources;
preset_pos_struct presets;

float dBLevel[256];
float Phase[128];
float SummingdBLevel[64];
float SummingPhase[32];
unsigned int BackplaneMambaNetAddress = 0x00000000;

DSP_HANDLER_STRUCT *dsp_handler;

AXUM_FUNCTION_INFORMATION_STRUCT *SourceFunctions[NUMBER_OF_SOURCES+4][NUMBER_OF_SOURCE_FUNCTIONS];
AXUM_FUNCTION_INFORMATION_STRUCT *ModuleFunctions[NUMBER_OF_MODULES+4][NUMBER_OF_MODULE_FUNCTIONS];
AXUM_FUNCTION_INFORMATION_STRUCT *BussFunctions[NUMBER_OF_BUSSES+4][NUMBER_OF_BUSS_FUNCTIONS];
AXUM_FUNCTION_INFORMATION_STRUCT *MonitorBussFunctions[NUMBER_OF_MONITOR_BUSSES+4][NUMBER_OF_MONITOR_BUSS_FUNCTIONS];
AXUM_FUNCTION_INFORMATION_STRUCT *DestinationFunctions[NUMBER_OF_DESTINATIONS+4][NUMBER_OF_DESTINATION_FUNCTIONS];
AXUM_FUNCTION_INFORMATION_STRUCT *GlobalFunctions[NUMBER_OF_GLOBAL_FUNCTIONS];
AXUM_FUNCTION_INFORMATION_STRUCT *ConsoleFunctions[NUMBER_OF_CONSOLES][NUMBER_OF_CONSOLE_FUNCTIONS];

float Position2dB[1024];
unsigned short int dB2Position[1500];
unsigned int PulseTime;

unsigned char TraceValue;           //To set the MambaNet trace (0x01=packets, 0x02=address table)
bool dump_packages;                 //To debug the incoming (unsigned char *)data

//int NetworkFileDescriptor;          //identifies the used network device
int DB_fd;

unsigned char EthernetReceiveBuffer[4096];
int cntEthernetReceiveBufferTop;
int cntEthernetReceiveBufferBottom;
unsigned char EthernetMambaNetDecodeBuffer[128];
unsigned char cntEthernetMambaNetDecodeBuffer;

char TTYDevice[256];                    //Buffer to store the serial device name
char NetworkInterface[256];     //Buffer to store the networ device name

unsigned char LocalMACAddress[6];  //Buffer to store local MAC Address

int EthernetInterfaceIndex = -1;

unsigned long cntMillisecondTimer;
unsigned long PreviousCount_Second;
unsigned long PreviousCount_SignalDetect;
unsigned long PreviousCount_LevelMeter;
unsigned long PreviousCount_PhaseMeter;
unsigned long PreviousCount_BroadcastPing;
unsigned long cntBroadcastPing;

int LinuxIfIndex;
struct CONSOLE_PRESET_SWITCH_STRUCT {
  bool PreviousState;
  bool State;
  unsigned int TimerValue;
} ConsolePresetSwitch[32] = {{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},
                             {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},
                             {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},
                             {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}};

struct PROGRAMMED_DEFAULT_SWITCH_STRUCT {
  bool PreviousState;
  bool State;
  unsigned int TimerValue;
} ProgrammedDefaultSwitch[4] = {{0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}};

void *thread(void *vargp);

ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationList = NULL;

//#define ADDRESS_TABLE_SIZE 65536
//ONLINE_NODE_INFORMATION_STRUCT OnlineNodeInformation[ADDRESS_TABLE_SIZE];

//sqlite3 *axum_engine_db;
//sqlite3 *node_templates_db;

int CallbackNodeIndex = -1;
unsigned char use_eth = 0;

void node_info_lock(int l)
{
  if (l)
  {
    pthread_mutex_lock(&get_node_info_mutex);
  }
  else
  {
    pthread_mutex_unlock(&get_node_info_mutex);
  }
}

void axum_data_lock(int l)
{
  if (l)
  {
    pthread_mutex_lock(&axum_data_mutex);
  }
  else
  {
    pthread_mutex_unlock(&axum_data_mutex);
  }
}

void init(int argc, char **argv)
{
  //struct mbn_interface *itf;
  //char err[MBN_ERRSIZE];
  char ethdev[50];
  char dbstr[256];
  pthread_mutexattr_t mattr;
  int c;
  char oem_name[32];
  char cmdline[1024];
  char socket_path[UNIX_PATH_MAX];

  use_eth = 0;

  pthread_mutexattr_init(&mattr);
  //pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&get_node_info_mutex, &mattr);

  strcpy(ethdev, DEFAULT_ETH_DEV);
  strcpy(dbstr, DEFAULT_DB_STR);
  strcpy(log_file, DEFAULT_LOG_FILE);
  strcpy(hwparent_path, DEFAULT_UNIX_HWPARENT_PATH);
  strcpy(socket_path, DEFAULT_UNIX_MAMBANET_PATH);
  strcpy(backup_file, DEFAULT_BACKUP_FILE);

  /* parse options */
  while((c = getopt(argc, argv, "e:d:l:g:i:f:v")) != -1) {
    switch(c) {
      case 'e':
        if(strlen(optarg) > 50) {
          fprintf(stderr, "Too long device name.\n");
          exit(1);
        }
        strcpy(ethdev, optarg);
        use_eth = 1;
        break;
      case 'i':
        if(sscanf(optarg, "%hd", &(this_node.UniqueIDPerProduct)) != 1) {
          fprintf(stderr, "Invalid UniqueIDPerProduct");
          exit(1);
        }
        if (this_node.UniqueIDPerProduct < 1)
        {
          fprintf(stderr, "Unique ID not found or out of range\n");
          exit(1);
        }
        break;
      case 'd':
        if(strlen(optarg) > 256) {
          fprintf(stderr, "Too long database connection string!\n");
          exit(1);
        }
        strcpy(dbstr, optarg);
        break;
      case 'g':
        strcpy(hwparent_path, optarg);
        break;
      case 'l':
        strcpy(log_file, optarg);
        break;
      case 'v':
        verbose = 1;
        break;
      case 'f':
        if (dsp_force_eeprom_prg(optarg))
        {
          printf("PCI2040 EEPROM programmed in forced mode (%s).\n", optarg);
        }
        else
        {
          fprintf(stderr, "PCI2040 EEPROM NOT programmed in forced mode (%s).\n", optarg);
        }
        exit(1);
        break;
      default:
        fprintf(stderr, "Usage: %s [-e dev] [-u path] [-g path] [-d str] [-l path] [-i id]\n", argv[0]);
        fprintf(stderr, "  -e dev   Ethernet device for MambaNet communication.\n");
        fprintf(stderr, "  -i id    UniqueIDPerProduct for the MambaNet node.\n");
        fprintf(stderr, "  -g path  Hardware parent or path to gateway socket.\n");
        fprintf(stderr, "  -l path  Path to log file.\n");
        fprintf(stderr, "  -d str   PostgreSQL database connection options.\n");
        fprintf(stderr, "  -v       Verbose output.\n");
        fprintf(stderr, "  -f dev   force EEPROM programming on device 'dev'.\n");
        exit(1);
    }
  }

  if (!verbose)
    daemonize();

  if (!verbose)
    log_open();

  if (oem_name_short(oem_name, 32))
  {
    strncpy(this_node.Name, oem_name, 32);
    strcat(this_node.Name, " Engine");

    strncpy(this_node.Description, oem_name, 32);
    strcat(this_node.Description, " Engine (Linux)");
  }

  log_write("------------------------------------------------");
  log_write("Try to start the %s", this_node.Name);
  log_write("Version %d.%d, compiled at %s (%s)", this_node.FirmwareMajorRevision, this_node.FirmwareMinorRevision, __DATE__, __TIME__);
  sprintf(cmdline, "command line:");
  for (int cnt=0; cnt<argc; cnt++)
  {
    strcat(cmdline, " ");
    strcat(cmdline, argv[cnt]);
  }
  log_write(cmdline);
  log_write(mbnVersion());

  hwparent(&this_node);
  log_write("hwparent %04X:%04X:%04X", this_node.HardwareParent[0], this_node.HardwareParent[1], this_node.HardwareParent[2]);

  db_open(dbstr);

  DB_fd = db_get_fd();
  if(DB_fd < 0)
  {
    printf("Invalid PostgreSQL socket\n");
    log_close();
    exit(1);
  }

  db_lock(1);
  db_get_matrix_sources();
  db_lock(0);

  dsp_handler = dsp_open();
  if (dsp_handler == NULL)
  {
    db_close();
    log_close();
    exit(1);
  }


  if (!use_eth)
  {
    if ((itf=mbnUnixOpen(socket_path, NULL, error)) == NULL)
    {
      fprintf(stderr, "Error opening unix socket: %s", error);
      dsp_close(dsp_handler);
      db_close();
      log_close();
      exit(1);
    }
  }
  else
  {
    if ((itf=mbnEthernetOpen(ethdev, error)) == NULL)
    {
      fprintf(stderr, "Error opening ethernet device: %s", error);
      dsp_close(dsp_handler);
      db_close();
      log_close();
      exit(1);
    }
  }


  if (!verbose)
    daemonize_finish();

  log_write("Axum Engine Initialized");
}


void *timer_thread_loop(void *arg)
{
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 10000;

  while (!main_quit)
  {
    int ReturnValue = select(0, NULL, NULL, NULL, &timeout);
    if ((ReturnValue == 0) || ((ReturnValue<0) && (errno == EINTR)))
    {//upon SIGALARM this happens :(
    }
    else if (ReturnValue<0)
    { //error
      log_write("select() failed: %s\n", strerror(errno));
    }
    if ((timeout.tv_sec == 0) && (timeout.tv_usec == 0))
    {
      Timer100HzDone(0);
      timeout.tv_sec = 0;
      timeout.tv_usec = 10000;
    }
  }
  return NULL;
  arg = NULL;
}

void mWriteLogMessage(struct mbn_handler *mbn, char *msg) {
  log_write(msg);
  return;
  mbn = NULL;
}

void mOnlineStatus(struct mbn_handler *mbn, unsigned long addr, char valid)
{
  log_write("OnlineStatus on %08lX %s", addr, valid ? "validated" : "invalid");
  return;
  mbn = NULL;
}

int main(int argc, char *argv[])
{
  fd_set readfs;

  initialize_axum_data_struct();

  init(argc, argv);

  AxumApplicationAndDSPInitialized = 1;
  log_write("Parameters in DSPs initialized");

  //Slot configuration, former rack organization
  db_lock(1);
  db_empty_slot_config();

  //Presets
  db_read_src_preset(1, 1280);
  //db_read_buss_preset(unsigned short int first_preset, unsigned short int last_preset);
  db_read_buss_preset_rows(1, 1280);
  db_read_monitor_buss_preset_rows(1, 1280);
  db_read_console_preset(1, 32);
  db_read_routing_preset(1, 128);

  //Source configuration
  db_read_src_config(1, 1280);

  //module_configuration
  db_read_module_config(1, 128, 0xFF, 1);

  //buss_configuration
  db_read_buss_config(1, 16, 0xFF);

  //monitor_buss_configuration
  db_read_monitor_buss_config(1, 16, 0xFF);

  //extern_source_configuration
  db_read_extern_src_config(1, 4);

  //talkback_configuration
  db_read_talkback_config(1, 16);

  //global_configuration
  db_read_global_config(1);

  //destination_configuration
  db_read_dest_config(1, 1280);

  //position to db
  db_read_db_to_position();
  db_lock(0);

  //Update default values of EQ to the current values
  axum_data_lock(1);
  for (int cntModule=0; cntModule<128; cntModule++)
  {
    for (int cntEQBand=0; cntEQBand<6; cntEQBand++)
    {
      AxumData.ModuleData[cntModule].EQBand[cntEQBand].Level = AxumData.ModuleData[cntModule].Defaults.EQBand[cntEQBand].Level;
      AxumData.ModuleData[cntModule].EQBand[cntEQBand].Frequency = AxumData.ModuleData[cntModule].Defaults.EQBand[cntEQBand].Frequency;
      AxumData.ModuleData[cntModule].EQBand[cntEQBand].Bandwidth = AxumData.ModuleData[cntModule].Defaults.EQBand[cntEQBand].Bandwidth;
      AxumData.ModuleData[cntModule].EQBand[cntEQBand].Slope = AxumData.ModuleData[cntModule].Defaults.EQBand[cntEQBand].Slope;
      AxumData.ModuleData[cntModule].EQBand[cntEQBand].Type = AxumData.ModuleData[cntModule].Defaults.EQBand[cntEQBand].Type;
    }
  }
  axum_data_lock(0);

  //Check for backup
  axum_data_lock(1);
  if (backup_open((void *)&AxumData, sizeof(AxumData), &axum_data_mutex, !AxumData.StartupState))
  { //Backup loaded, clear rack-config and set processing data
    if (AxumData.StartupState)
    {
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        for (int cntBand=0; cntBand<6; cntBand++)
        {
          SetAxum_EQ(cntModule, cntBand);
        }
        SetAxum_ModuleProcessing(cntModule);
        SetAxum_ModuleMixMinus(cntModule, 0);
        SetAxum_BussLevels(cntModule);
      }
      SetAxum_BussMasterLevels();
      for (int cntBuss=0; cntBuss<16; cntBuss++)
      {
        SetAxum_MonitorBuss(cntBuss);
      }
    }
  }
  for (unsigned char cntSlot=0; cntSlot<42; cntSlot++)
  {
    AxumData.RackOrganization[cntSlot] = 0x00000000;
  }
  axum_data_lock(0);

  if((mbn = mbnInit(&this_node, NULL, itf, error)) == NULL) {
    fprintf(stderr, "mbnInit: %s\n", error);
    dsp_close(dsp_handler);
    db_close();
    log_close();
    exit(1);
  }

  //Set required callbacks
  mbnSetAddressTableChangeCallback(mbn, mAddressTableChange);
  mbnSetSensorDataResponseCallback(mbn, mSensorDataResponse);
  mbnSetSensorDataChangedCallback(mbn, mSensorDataChanged);
  mbnSetErrorCallback(mbn, mError);
  mbnSetAcknowledgeTimeoutCallback(mbn, mAcknowledgeTimeout);
  mbnSetAcknowledgeReplyCallback(mbn, mAcknowledgeReply);
  mbnSetWriteLogMessageCallback(mbn, mWriteLogMessage);
  mbnSetOnlineStatusCallback(mbn, mOnlineStatus);

  //start interface for the mbn-handler
  mbnStartInterface(itf, error);
  log_write("Axum engine process started");

  node_info_lock(1);
  InitalizeAllObjectListPerFunction();
  node_info_lock(0);

  //**************************************************************/
  //Initialize Timer thread
  //**************************************************************/
  cntBroadcastPing = 6;
  pthread_t timer_thread;
  pthread_create(&timer_thread, NULL, timer_thread_loop, NULL);

  while (!main_quit)
  {
    //Set the sources which wakes the idle-wait process 'select'
    FD_ZERO(&readfs);
    FD_SET(DB_fd, &readfs);

    // block (process is in idle mode) until input becomes available
    int ReturnValue = select(DB_fd+1, &readfs, NULL, NULL, NULL);
    if ((ReturnValue == 0) || ((ReturnValue<0) && (errno == EINTR)))
    {//upon SIGALARM this happens :(
    }
    else if (ReturnValue<0)
    { //error
      log_write("select() failed: %s\n", strerror(errno));
    }
    else
    {//no error or non-blocked signal)
      //Test if the database notifier generated an event.

      //node_info_lock required for db_read_node_config
      axum_data_lock(1);
      node_info_lock(1);
      db_lock(1);
      db_processnotifies();
      db_lock(0);
      node_info_lock(0);
      axum_data_lock(0);
    }
  }
  log_write("Closing Engine");

  axum_data_lock(1);
  backup_close(0);
  axum_data_lock(0);

  log_write("node_info_lock");
  node_info_lock(1);
  DeleteAllObjectListPerFunction();
  node_info_lock(0);

  log_write("dsp_close");
  dsp_close(dsp_handler);

  if (mbn)
  {
    mbnFree(mbn);
  }
  db_close();

  node_info_lock(1);
  ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = OnlineNodeInformationList;
  while (OnlineNodeInformationElement != NULL)
  {
    ONLINE_NODE_INFORMATION_STRUCT *DeleteOnlineNodeInformationElement = OnlineNodeInformationElement;;
    OnlineNodeInformationElement = OnlineNodeInformationElement->Next;
    if (DeleteOnlineNodeInformationElement->SensorReceiveFunction != NULL)
    {
      delete[] DeleteOnlineNodeInformationElement->SensorReceiveFunction;
    }
    if (DeleteOnlineNodeInformationElement->ObjectInformation != NULL)
    {
      delete[] DeleteOnlineNodeInformationElement->ObjectInformation;
    }
    delete DeleteOnlineNodeInformationElement;
  }
  OnlineNodeInformationList = NULL;
  node_info_lock(0);

  log_close();

  return 0;
}

//normally response on a sensor change
int mSensorDataChanged(struct mbn_handler *mbn, struct mbn_message *message, short unsigned int object, unsigned char type, union mbn_data data)
{
  axum_data_lock(1);
  node_info_lock(1);
  ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = GetOnlineNodeInformation(message->AddressFrom);
  if (OnlineNodeInformationElement == NULL)
  {
    log_write("[mSensorDataChanged] OnlineNodeInformationElement not found for address: 0x%08X", message->AddressFrom);
    node_info_lock(0);
    axum_data_lock(0);
    return 1;
  }
  if (object>=(OnlineNodeInformationElement->UsedNumberOfCustomObjects+1024))
  {
    log_write("[mSensorDataChanged] Object: %d is unknown, this node (0x%08x), contains %d objects", object, message->AddressFrom, OnlineNodeInformationElement->UsedNumberOfCustomObjects);
    node_info_lock(0);
    axum_data_lock(0);
    return 1;
  }
  else if (object<1024)
  {
    log_write("[mSensorDataChanged] On node 0x%08X a sensor change is not allowed for object: %d (<1024)", message->AddressFrom, object);
    node_info_lock(0);
    axum_data_lock(0);
    return 1;
  }

//  if (OnlineNodeInformationElement->ManufacturerID == 2)
//  {
//    debug_mambanet_data(message->AddressFrom, object, type, data);
//  }

  if (OnlineNodeInformationElement->SensorReceiveFunction != NULL)
  {
    SENSOR_RECEIVE_FUNCTION_STRUCT *SensorReceiveFunction = &OnlineNodeInformationElement->SensorReceiveFunction[object-1024];
    unsigned char SensorAllowed = 1;
    if ((OnlineNodeInformationElement->UserLevelFromConsole>0) && (OnlineNodeInformationElement->UserLevelFromConsole<=NUMBER_OF_CONSOLES))
    {
      unsigned char ConsoleNr = OnlineNodeInformationElement->UserLevelFromConsole-1;
      if (AxumData.ConsoleData[ConsoleNr].UserLevel < 6)
      {
        if (!SensorReceiveFunction->ActiveInUserLevel[AxumData.ConsoleData[ConsoleNr].UserLevel])
        {
          SensorAllowed = 0;
          SensorReceiveFunction->ChangedWhileSensorNotAllowed = 1;
        }
      }
    }

    if (SensorAllowed)
    {
      SensorReceiveFunction->LastChangedTime = cntMillisecondTimer;
      if (!AxumData.AutoMomentary)
      {
        SensorReceiveFunction->PreviousLastChangedTime = SensorReceiveFunction->LastChangedTime;
      }

      int SensorReceiveFunctionNumber = SensorReceiveFunction->FunctionNr;
      int DataType = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataType;
      int DataSize = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataSize;
      float DataMinimal = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataMinimal;
      float DataMaximal = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataMaximal;

      if (SensorReceiveFunctionNumber != -1)
      {
        unsigned char SensorReceiveFunctionType = (SensorReceiveFunctionNumber>>24)&0xFF;

        switch (SensorReceiveFunctionType)
        {
          case MODULE_FUNCTIONS:
          {   //Module
            unsigned int ModuleNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;
            unsigned char ConsoleNr = AxumData.ModuleData[ModuleNr].Console;

            if ((ModuleNr >= NUMBER_OF_MODULES) && (ModuleNr<(NUMBER_OF_MODULES+NUMBER_OF_CONSOLES)))
            {
              int ConsoleNr = ModuleNr-NUMBER_OF_MODULES;
              ModuleNr = AxumData.ConsoleData[ConsoleNr].SelectedModule;
            }

            switch (FunctionNr)
            {
              case MODULE_FUNCTION_LABEL:
              {   //No sensor change available
              }
              break;
              case MODULE_FUNCTION_SOURCE:
              case MODULE_FUNCTION_PRESET_1A:
              case MODULE_FUNCTION_PRESET_1B:
              case MODULE_FUNCTION_PRESET_2A:
              case MODULE_FUNCTION_PRESET_2B:
              case MODULE_FUNCTION_PRESET_3A:
              case MODULE_FUNCTION_PRESET_3B:
              case MODULE_FUNCTION_PRESET_4A:
              case MODULE_FUNCTION_PRESET_4B:
              {   //Source
                int CurrentSource = AxumData.ModuleData[ModuleNr].TemporySourceLocal;
                int CurrentPreset = 0;
                int CurrentRoutingPreset = 0;

                if (type == MBN_DATATYPE_SINT)
                {
                  unsigned char Pool = 8;
                  if (AxumData.ConsoleData[ConsoleNr].SourcePool < 2)
                  {
                    Pool = (ConsoleNr*2)+AxumData.ConsoleData[ConsoleNr].SourcePool;
                  }
                  CurrentSource = AdjustModuleSource(CurrentSource, data.SInt, Pool);
                  AxumData.ModuleData[ModuleNr].TemporySourceLocal = CurrentSource;

                  unsigned int FunctionNrToSent = (ModuleNr<<12);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE);
                }
                else if (type == MBN_DATATYPE_STATE)
                {
                  CurrentSource = AxumData.ModuleData[ModuleNr].SelectedSource;
                  CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;

                  if (data.State)
                  {
                    switch (FunctionNr)
                    {
                      case MODULE_FUNCTION_SOURCE:
                      {
                        if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                        {
                          unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                          CurrentPreset = AxumData.SourceData[SourceNr].DefaultProcessingPreset;
                        }
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_1A:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source1A;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset1A;
                        CurrentRoutingPreset = 1;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_1B:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source1B;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset1B;
                        CurrentRoutingPreset = 2;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_2A:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source2A;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset2A;
                        CurrentRoutingPreset = 3;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_2B:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source2B;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset2B;
                        CurrentRoutingPreset = 4;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_3A:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source3A;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset3A;
                        CurrentRoutingPreset = 5;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_3B:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source3B;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset3B;
                        CurrentRoutingPreset = 6;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_4A:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source4A;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset4A;
                        CurrentRoutingPreset = 7;
                      }
                      break;
                      case MODULE_FUNCTION_PRESET_4B:
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].Source4B;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset4B;
                        CurrentRoutingPreset = 8;
                      }
                      break;
                    }

                    if (MixMinusSourceUsed(CurrentSource-1) != -1)
                    {
                      CurrentSource = AxumData.ModuleData[ModuleNr].SelectedSource;
                      CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
                    }
                  }
                }

                if (FunctionNr != MODULE_FUNCTION_SOURCE)
                {
                  int SourceActive = 0;
                  if (AxumData.ModuleData[ModuleNr].On)
                  {
                    if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
                    {
                      SourceActive = 1;
                    }
                  }

                  if ((!SourceActive) || (AxumData.ModuleData[ModuleNr].OverruleActive))
                  {
                    if (data.State)
                    {
                      if (CurrentSource != 0)
                      { //Source 'none', is no change
                        DoAxum_SetNewSource(ModuleNr, CurrentSource, AxumData.ModuleData[ModuleNr].OverruleActive);
                      }
                      if (CurrentPreset != 0)
                      { //Preset 'none', is no change
                        DoAxum_LoadProcessingPreset(ModuleNr, CurrentPreset, 0, 0, 0);
                      }
                      if (CurrentRoutingPreset != 0)
                      {
                        DoAxum_LoadRoutingPreset(ModuleNr, CurrentRoutingPreset, 0, 0, 0);
                      }
                    }
                  }
                  else
                  {
                    if (data.State)
                    {
//                      AxumData.ModuleData[ModuleNr].WaitingSource = CurrentSource;
//                      AxumData.ModuleData[ModuleNr].WaitingProcessingPreset = CurrentPreset;
//                      AxumData.ModuleData[ModuleNr].WaitingRoutingPreset = CurrentRoutingPreset;
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_PRESET_A:
              case MODULE_FUNCTION_PRESET_B:
              case MODULE_FUNCTION_PRESET_A_B:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      int CurrentSource = 0;
                      int CurrentPreset = 0;
                      int CurrentRoutingPreset = 0;
                      int PresetNr = FunctionNr-MODULE_FUNCTION_PRESET_A;

                      if (FunctionNr == MODULE_FUNCTION_PRESET_A_B)
                      {
                        if (ModulePresetActive(ModuleNr, (AxumData.ModuleData[ModuleNr].ModulePreset<<1)+1))
                        {
                          PresetNr = 1;
                        }
                        else
                        {
                          PresetNr = 0;
                        }
                      }
                      PresetNr += AxumData.ModuleData[ModuleNr].ModulePreset<<1;

                      switch (PresetNr)
                      {
                        case 0:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source1A;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset1A;
                          CurrentRoutingPreset = 1;
                        }
                        break;
                        case 1:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source1B;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset1B;
                          CurrentRoutingPreset = 2;
                        }
                        break;
                        case 2:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source2A;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset2A;
                          CurrentRoutingPreset = 3;
                        }
                        break;
                        case 3:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source2B;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset2B;
                          CurrentRoutingPreset = 4;
                        }
                        break;
                        case 4:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source3A;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset3A;
                          CurrentRoutingPreset = 5;
                        }
                        break;
                        case 5:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source3B;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset3B;
                          CurrentRoutingPreset = 6;
                        }
                        break;
                        case 6:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source4A;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset4A;
                          CurrentRoutingPreset = 7;
                        }
                        break;
                        case 7:
                        {
                          CurrentSource = AxumData.ModuleData[ModuleNr].Source4B;
                          CurrentPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset4B;
                          CurrentRoutingPreset = 8;
                        }
                        break;
                      }

                      if (MixMinusSourceUsed(CurrentSource-1) != -1)
                      {
                        CurrentSource = AxumData.ModuleData[ModuleNr].SelectedSource;
                        CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
                      }

                      int SourceActive = 0;
                      if (AxumData.ModuleData[ModuleNr].On)
                      {
                        if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
                        {
                          SourceActive = 1;
                        }
                      }

                      if ((!SourceActive) || (AxumData.ModuleData[ModuleNr].OverruleActive))
                      {
                        if (CurrentSource != 0)
                        { //Source 'none', is no change
                          DoAxum_SetNewSource(ModuleNr, CurrentSource, AxumData.ModuleData[ModuleNr].OverruleActive);
                        }
                        if (CurrentPreset != 0)
                        { //Preset 'none', is no change
                          DoAxum_LoadProcessingPreset(ModuleNr, CurrentPreset, 0, 0, 0);
                        }
                        if (CurrentRoutingPreset != 0)
                        { //RoutingPreset 'none', is no change
                          DoAxum_LoadRoutingPreset(ModuleNr, CurrentRoutingPreset, 0, 0, 0);
                        }
                      }
                      else
                      {
                        //AxumData.ModuleData[ModuleNr].WaitingSource = CurrentSource;
                        //AxumData.ModuleData[ModuleNr].WaitingProcessingPreset = CurrentPreset;
                        //AxumData.ModuleData[ModuleNr].WaitingRoutingPreset = CurrentRoutingPreset;
                      }
                    }
                  }
                  break;
                }
              }
              break;
              case MODULE_FUNCTION_PRESET:
              {
                int CurrentPreset = AxumData.ModuleData[ModuleNr].TemporyPresetLocal;
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      DoAxum_LoadProcessingPreset(ModuleNr, CurrentPreset, 0, 0, 0);
                    }
                  }
                  break;
                  case MBN_DATATYPE_SINT:
                  {
                    unsigned char Pool = 8;
                    if (AxumData.ConsoleData[ConsoleNr].PresetPool < 2)
                    {
                      Pool = (ConsoleNr*2)+AxumData.ConsoleData[ConsoleNr].PresetPool;
                    }
                    AxumData.ModuleData[ModuleNr].TemporyPresetLocal = AdjustModulePreset(CurrentPreset, data.SInt, Pool);
                  }
                  break;
                }
              }
              break;
              case MODULE_FUNCTION_SOURCE_PHANTOM:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                  {
                    unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                    unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                    if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM))
                    {
                      if (data.State)
                      {
                        AxumData.SourceData[SourceNr].Phantom = !AxumData.SourceData[SourceNr].Phantom;
                      }
                      else
                      {
                        int delay_time = (SensorReceiveFunction->LastChangedTime-cntMillisecondTimer);
                        if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                        {
                          if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                          {
                            AxumData.SourceData[SourceNr].Phantom = !AxumData.SourceData[SourceNr].Phantom;
                          }
                        }
                      }

                      CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
                        {
                          FunctionNrToSent = (cntModule<<12);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_PHANTOM);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_SOURCE_PAD:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                  {
                    unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                    unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                    if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PAD))
                    {
                      if (data.State)
                      {
                        AxumData.SourceData[SourceNr].Pad = !AxumData.SourceData[SourceNr].Pad;
                      }
                      else
                      {
                        int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                        if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                        {
                          if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                          {
                            AxumData.SourceData[SourceNr].Pad = !AxumData.SourceData[SourceNr].Pad;
                          }
                        }
                      }

                      CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_PAD);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
                        {
                          FunctionNrToSent = (cntModule<<12);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_PAD);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_SOURCE_GAIN_LEVEL:
              {
                if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                {
                  unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                  unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                  if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
                  {
                    float min, max, def;
                    CheckObjectRange(FunctionNrToSent | SOURCE_FUNCTION_GAIN, &min, &max, &def);

                    if (type == MBN_DATATYPE_UINT)
                    {
                      AxumData.SourceData[SourceNr].Gain = (((float)(max-min)*(data.UInt-DataMinimal))/(DataMaximal-DataMinimal))+min;

                    }
                    else if (type == MBN_DATATYPE_SINT)
                    {
                      AxumData.SourceData[SourceNr].Gain += (float)data.SInt/10;
                      if (AxumData.SourceData[SourceNr].Gain<min)
                      {
                        AxumData.SourceData[SourceNr].Gain = min;
                      }
                      else if (AxumData.SourceData[SourceNr].Gain>max)
                      {
                        AxumData.SourceData[SourceNr].Gain = max;
                      }
                    }

                    CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_GAIN);

                    for (int cntModule=0; cntModule<128; cntModule++)
                    {
                      if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
                      {
                        FunctionNrToSent = (cntModule<<12);
                        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_GAIN_LEVEL);
                      }
                    }
                    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_SOURCE_GAIN);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_SOURCE_GAIN_LEVEL_RESET:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                  {
                    unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                    unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                    if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
                    {
                      float min, max, def;
                      CheckObjectRange(FunctionNrToSent | SOURCE_FUNCTION_GAIN, &min, &max, &def);

                      if (data.State)
                      {
                        if (AxumData.SourceData[SourceNr].Gain != AxumData.SourceData[SourceNr].DefaultGain)
                        {
                          AxumData.SourceData[SourceNr].Gain = AxumData.SourceData[SourceNr].DefaultGain;
                          CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_GAIN);

                          for (int cntModule=0; cntModule<128; cntModule++)
                          {
                            if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
                            {
                              FunctionNrToSent = (cntModule<<12);
                              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_GAIN_LEVEL);
                            }
                          }
                          DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_SOURCE_GAIN);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_INSERT_ON_OFF:
              { //Insert on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].InsertOnOff = !AxumData.ModuleData[ModuleNr].InsertOnOff;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].InsertOnOff = !AxumData.ModuleData[ModuleNr].InsertOnOff;
                      }
                    }
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSent = (ModuleNr<<12) | MODULE_FUNCTION_INSERT_ON_OFF;
                  CheckObjectsToSent(FunctionNrToSent);
                }
              }
              break;
              case MODULE_FUNCTION_PHASE_ON_OFF:
              {   //Phase
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].PhaseOnOff = !AxumData.ModuleData[ModuleNr].PhaseOnOff;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].PhaseOnOff = !AxumData.ModuleData[ModuleNr].PhaseOnOff;
                      }
                    }
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE_ON_OFF);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE);
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_PHASE);
                }
              }
              break;
              case MODULE_FUNCTION_GAIN_LEVEL:
              {   //Gain
                if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].Gain += (float)data.SInt/10;
                  if (AxumData.ModuleData[ModuleNr].Gain<-20)
                  {
                    AxumData.ModuleData[ModuleNr].Gain = -20;
                  }
                  else if (AxumData.ModuleData[ModuleNr].Gain>20)
                  {
                    AxumData.ModuleData[ModuleNr].Gain = 20;
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_GAIN);
                }
                else if (type == MBN_DATATYPE_UINT)
                {
                  AxumData.ModuleData[ModuleNr].Gain = (((float)40*(data.UInt-DataMinimal))/(DataMaximal-DataMinimal))-20;

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_GAIN);
                }
              }
              break;
              case MODULE_FUNCTION_GAIN_LEVEL_RESET:
              { //Gain reset
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    if (AxumData.ModuleData[ModuleNr].Gain != 0)
                    {
                      AxumData.ModuleData[ModuleNr].Gain = 0;
                      SetAxum_ModuleProcessing(ModuleNr);

                      unsigned int DisplayFunctionNr = (ModuleNr<<12) | MODULE_FUNCTION_GAIN_LEVEL;
                      CheckObjectsToSent(DisplayFunctionNr);

                      DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_GAIN);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_LOW_CUT_FREQUENCY:
              { //Low cut frequency
                if (type == MBN_DATATYPE_SINT)
                {
                  unsigned int old_freq = AxumData.ModuleData[ModuleNr].Filter.Frequency;
                  if (data.SInt>=0)
                  {
                    AxumData.ModuleData[ModuleNr].Filter.Frequency *= 1+((float)data.SInt/100);
                    if (old_freq == AxumData.ModuleData[ModuleNr].Filter.Frequency)
                    {
                      AxumData.ModuleData[ModuleNr].Filter.Frequency++;
                    }
                  }
                  else
                  {
                    AxumData.ModuleData[ModuleNr].Filter.Frequency /= 1+((float)-data.SInt/100);
                    if (old_freq == AxumData.ModuleData[ModuleNr].Filter.Frequency)
                    {
                      AxumData.ModuleData[ModuleNr].Filter.Frequency--;
                    }
                  }

                  if (AxumData.ModuleData[ModuleNr].Filter.Frequency <= 20)
                  {
                    AxumData.ModuleData[ModuleNr].Filter.Frequency = 20;
                  }
                  else if (AxumData.ModuleData[ModuleNr].Filter.Frequency > 15000)
                  {
                    AxumData.ModuleData[ModuleNr].Filter.Frequency = 15000;
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_LOW_CUT);
                }
              }
              break;
              case MODULE_FUNCTION_LOW_CUT_ON_OFF:
              { //Low cut on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].FilterOnOff = !AxumData.ModuleData[ModuleNr].FilterOnOff;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].FilterOnOff = !AxumData.ModuleData[ModuleNr].FilterOnOff;
                      }
                    }
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_LOW_CUT_ON_OFF);
                  CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_LOW_CUT_FREQUENCY);

                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_LOW_CUT);
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_LEVEL:
              case MODULE_FUNCTION_EQ_BAND_2_LEVEL:
              case MODULE_FUNCTION_EQ_BAND_3_LEVEL:
              case MODULE_FUNCTION_EQ_BAND_4_LEVEL:
              case MODULE_FUNCTION_EQ_BAND_5_LEVEL:
              case MODULE_FUNCTION_EQ_BAND_6_LEVEL:
              { //EQ Level
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_LEVEL)/(MODULE_FUNCTION_EQ_BAND_2_LEVEL-MODULE_FUNCTION_EQ_BAND_1_LEVEL);
                float OldLevel = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level;
                switch (type)
                {
                  case MBN_DATATYPE_SINT:
                  {
                    AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level += (float)data.SInt/10;
                    if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level<-AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = -AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range;
                    }
                    else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level>AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range;
                    }
                  }
                  break;
                  case MBN_DATATYPE_FLOAT:
                  {
                    if ((data.Float>=-18) && (data.Float<=18))
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = data.Float;
                      if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level<-AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range)
                      {
                        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = -AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range;
                      }
                      else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level>AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range)
                      {
                        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range;
                      }
                    }
                  }
                  break;
                }
                if (OldLevel != AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level)
                {
                  SetAxum_EQ(ModuleNr, BandNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL)*BandNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL+FunctionOffset);
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_FREQUENCY:
              case MODULE_FUNCTION_EQ_BAND_2_FREQUENCY:
              case MODULE_FUNCTION_EQ_BAND_3_FREQUENCY:
              case MODULE_FUNCTION_EQ_BAND_4_FREQUENCY:
              case MODULE_FUNCTION_EQ_BAND_5_FREQUENCY:
              case MODULE_FUNCTION_EQ_BAND_6_FREQUENCY:
              { //EQ Frequency
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY)/(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY);
                unsigned int OldFrequency = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency;
                switch (type)
                {
                  case MBN_DATATYPE_SINT:
                  {
                    if (data.SInt>=0)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency *= 1+((float)data.SInt/100);
                      if (OldFrequency == AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency)
                      {
                        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency++;
                      }
                    }
                    else
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency /= 1+((float)-data.SInt/100);
                      if (OldFrequency == AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency)
                      {
                        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency--;
                      }
                    }

                    if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency<20)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = 20;
                    }
                    else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency>15000)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = 15000;
                    }
                  }
                  break;
                  case MBN_DATATYPE_UINT:
                  {
                    if ((data.UInt>=20) && (data.UInt<=15000))
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = data.UInt;
                    }
                  }
                  break;
                }
                if (OldFrequency != AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency)
                {
                  SetAxum_EQ(ModuleNr, BandNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY)*BandNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY+FunctionOffset);
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH:
              case MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH:
              case MODULE_FUNCTION_EQ_BAND_3_BANDWIDTH:
              case MODULE_FUNCTION_EQ_BAND_4_BANDWIDTH:
              case MODULE_FUNCTION_EQ_BAND_5_BANDWIDTH:
              case MODULE_FUNCTION_EQ_BAND_6_BANDWIDTH:
              { //EQ Bandwidth
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH)/(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH);
                float OldBandwidth = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth;
                switch (type)
                {
                  case MBN_DATATYPE_SINT:
                  {
                    AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth += (float)data.SInt/10;

                    if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth<0.1)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = 0.1;
                    }
                    else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth>10)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = 10;
                    }
                  }
                  break;
                  case MBN_DATATYPE_FLOAT:
                  {
                    if ((data.Float>=0.1) && (data.Float<=10))
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = data.Float;

                      if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth<0.1)
                      {
                        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = 0.1;
                      }
                      else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth>10)
                      {
                        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = 10;
                      }
                    }
                  }
                  break;
                }

                if (OldBandwidth != AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth)
                {
                  SetAxum_EQ(ModuleNr, BandNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH)*BandNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH+FunctionOffset);
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_LEVEL_RESET:
              case MODULE_FUNCTION_EQ_BAND_2_LEVEL_RESET:
              case MODULE_FUNCTION_EQ_BAND_3_LEVEL_RESET:
              case MODULE_FUNCTION_EQ_BAND_4_LEVEL_RESET:
              case MODULE_FUNCTION_EQ_BAND_5_LEVEL_RESET:
              case MODULE_FUNCTION_EQ_BAND_6_LEVEL_RESET:
              { //EQ Level reset
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_LEVEL_RESET)/(MODULE_FUNCTION_EQ_BAND_2_LEVEL_RESET-MODULE_FUNCTION_EQ_BAND_1_LEVEL_RESET);
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level != 0)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = 0;

                      SetAxum_EQ(ModuleNr, BandNr);

                      unsigned int FunctionNrToSend = ModuleNr<<12;
                      CheckObjectsToSent(FunctionNrToSend + (MODULE_FUNCTION_EQ_BAND_1_LEVEL+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_LEVEL-MODULE_FUNCTION_EQ_BAND_1_LEVEL))));

                      int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL)*BandNr;
                      DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL+FunctionOffset);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_FREQUENCY_RESET:
              case MODULE_FUNCTION_EQ_BAND_2_FREQUENCY_RESET:
              case MODULE_FUNCTION_EQ_BAND_3_FREQUENCY_RESET:
              case MODULE_FUNCTION_EQ_BAND_4_FREQUENCY_RESET:
              case MODULE_FUNCTION_EQ_BAND_5_FREQUENCY_RESET:
              case MODULE_FUNCTION_EQ_BAND_6_FREQUENCY_RESET:
              { //EQ Frequency reset
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY_RESET)/(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY_RESET-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY_RESET);
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    unsigned int Frequency = AxumData.ModuleData[ModuleNr].Defaults.EQBand[BandNr].Frequency;
                    int CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
                    if (CurrentPreset > 0)
                    {
                      Frequency = AxumData.PresetData[CurrentPreset-1].EQBand[BandNr].Frequency;
                    }

                    if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency != Frequency)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = Frequency;

                      SetAxum_EQ(ModuleNr, BandNr);

                      unsigned int FunctionNrToSend = ModuleNr<<12;
                      CheckObjectsToSent(FunctionNrToSend + (MODULE_FUNCTION_EQ_BAND_1_FREQUENCY+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY))));

                      int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY)*BandNr;
                      DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY+FunctionOffset);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH_RESET:
              case MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH_RESET:
              case MODULE_FUNCTION_EQ_BAND_3_BANDWIDTH_RESET:
              case MODULE_FUNCTION_EQ_BAND_4_BANDWIDTH_RESET:
              case MODULE_FUNCTION_EQ_BAND_5_BANDWIDTH_RESET:
              case MODULE_FUNCTION_EQ_BAND_6_BANDWIDTH_RESET:
              { //EQ Bandwidth reset
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH_RESET)/(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH_RESET-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH_RESET);
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    float Bandwidth = AxumData.ModuleData[ModuleNr].Defaults.EQBand[BandNr].Bandwidth;
                    int CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
                    if (CurrentPreset > 0)
                    {
                      Bandwidth = AxumData.PresetData[CurrentPreset-1].EQBand[BandNr].Bandwidth;
                    }

                    if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth != Bandwidth)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = Bandwidth;

                      SetAxum_EQ(ModuleNr, BandNr);

                      unsigned int FunctionNrToSend = ModuleNr<<12;
                      CheckObjectsToSent(FunctionNrToSend + (MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH))));

                      int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH)*BandNr;
                      DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH+FunctionOffset);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_EQ_BAND_1_TYPE:
              case MODULE_FUNCTION_EQ_BAND_2_TYPE:
              case MODULE_FUNCTION_EQ_BAND_3_TYPE:
              case MODULE_FUNCTION_EQ_BAND_4_TYPE:
              case MODULE_FUNCTION_EQ_BAND_5_TYPE:
              case MODULE_FUNCTION_EQ_BAND_6_TYPE:
              { //EQ Type
                int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_TYPE)/(MODULE_FUNCTION_EQ_BAND_2_TYPE-MODULE_FUNCTION_EQ_BAND_1_TYPE);
                int OldType = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type;

                switch (type)
                {
                  case MBN_DATATYPE_SINT:
                  {
                    int Type = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type;
                    Type += data.SInt;

                    if (Type<OFF)
                    {
                      Type = OFF;
                    }
                    else if (Type>NOTCH)
                    {
                      Type = NOTCH;
                    }
                    AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type = (FilterType)Type;
                  }
                  break;
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State<=7)
                    {
                      AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type = (FilterType)data.State;
                    }
                  }
                }

                if (OldType != AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type)
                {
                  SetAxum_EQ(ModuleNr, BandNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_EQ_BAND_2_TYPE-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE)*BandNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_BAND_1_TYPE+FunctionOffset);
                }
              }
              break;
              case MODULE_FUNCTION_EQ_ON_OFF:
              { //EQ on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].EQOnOff = !AxumData.ModuleData[ModuleNr].EQOnOff;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].EQOnOff = !AxumData.ModuleData[ModuleNr].EQOnOff;
                      }
                    }
                  }

                  for (int cntBand=0; cntBand<6; cntBand++)
                  {
                    SetAxum_EQ(ModuleNr, cntBand);
                  }

                  unsigned int FunctionNrToSent = (ModuleNr<<12) | MODULE_FUNCTION_EQ_ON_OFF;
                  CheckObjectsToSent(FunctionNrToSent);

                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EQ_ON_OFF);
                }
              }
              break;
      //                              case MODULE_FUNCTION_EQ_TYPE_A:
      //                              case MODULE_FUNCTION_EQ_TYPE_B:
      //                              { //EQ Type A&B
      //                              }
      //                              break;
              case MODULE_FUNCTION_AGC_THRESHOLD:
              { //Dynamics amount
                if (type == MBN_DATATYPE_UINT)
                {
                  AxumData.ModuleData[ModuleNr].AGCThreshold = ((30*data.UInt)/(DataMaximal-DataMinimal))-30;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].AGCThreshold += ((float)data.SInt/2);
                  if (AxumData.ModuleData[ModuleNr].AGCThreshold < -30)
                  {
                    AxumData.ModuleData[ModuleNr].AGCThreshold = -30;
                  }
                  else if (AxumData.ModuleData[ModuleNr].AGCThreshold > 0)
                  {
                    AxumData.ModuleData[ModuleNr].AGCThreshold = 0;
                  }
                }

                SetAxum_ModuleProcessing(ModuleNr);

                unsigned int FunctionNrToSend = ModuleNr<<12;
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_AGC_THRESHOLD);
              }
              break;
              case MODULE_FUNCTION_AGC_AMOUNT:
              { //Dynamics amount
                if (type == MBN_DATATYPE_UINT)
                {
                  AxumData.ModuleData[ModuleNr].AGCRatio = (19*data.UInt)/(DataMaximal-DataMinimal)+1;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].AGCRatio += ((float)data.SInt)/10;
                  if (AxumData.ModuleData[ModuleNr].AGCRatio < 1)
                  {
                    AxumData.ModuleData[ModuleNr].AGCRatio = 1;
                  }
                  else if (AxumData.ModuleData[ModuleNr].AGCRatio > 25)
                  {
                    AxumData.ModuleData[ModuleNr].AGCRatio = 25;
                  }
                }

                SetAxum_ModuleProcessing(ModuleNr);

                unsigned int FunctionNrToSend = ModuleNr<<12;
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_AGC);
              }
              break;
              case MODULE_FUNCTION_DYNAMICS_ON_OFF:
              { //Dynamics on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].DynamicsOnOff = !AxumData.ModuleData[ModuleNr].DynamicsOnOff;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].DynamicsOnOff = !AxumData.ModuleData[ModuleNr].DynamicsOnOff;
                      }
                    }
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSent = (ModuleNr<<12) | MODULE_FUNCTION_DYNAMICS_ON_OFF;
                  CheckObjectsToSent(FunctionNrToSent);
                }
              }
              break;
              case MODULE_FUNCTION_EXPANDER_THRESHOLD:
              { //Dynamics amount
                if (type == MBN_DATATYPE_UINT)
                {
                  AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold = ((50*data.UInt)/(DataMaximal-DataMinimal))-50;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold += ((float)data.SInt/2);
                  if (AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold < -50)
                  {
                    AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold = -50;
                  }
                  else if (AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold > 0)
                  {
                    AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold = 0;
                  }
                }

                SetAxum_ModuleProcessing(ModuleNr);

                unsigned int FunctionNrToSend = ModuleNr<<12;
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_EXPANDER_THRESHOLD);
              }
              break;
              case MODULE_FUNCTION_MONO_ON_OFF:
              { //Mono
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].MonoOnOff = !AxumData.ModuleData[ModuleNr].MonoOnOff;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].MonoOnOff = !AxumData.ModuleData[ModuleNr].MonoOnOff;
                      }
                    }
                  }

                  SetAxum_ModuleProcessing(ModuleNr);

                  unsigned int FunctionNrToSent = (ModuleNr<<12) | MODULE_FUNCTION_MONO_ON_OFF;
                  CheckObjectsToSent(FunctionNrToSent);

                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_MONO);
                }
              }
              break;
              case MODULE_FUNCTION_PAN:
              { //Pan
                if (type == MBN_DATATYPE_UINT)
                {
                  AxumData.ModuleData[ModuleNr].Panorama = ((data.UInt-DataMinimal)*1023)/(DataMaximal-DataMinimal);
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].Panorama += data.SInt;
                  if (AxumData.ModuleData[ModuleNr].Panorama< 0)
                  {
                    AxumData.ModuleData[ModuleNr].Panorama = 0;
                  }
                  else if (AxumData.ModuleData[ModuleNr].Panorama > 1023)
                  {
                    AxumData.ModuleData[ModuleNr].Panorama = 1023;
                  }
                }

                SetAxum_BussLevels(ModuleNr);

                unsigned int FunctionNrToSend = ModuleNr<<12;
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_PAN);
              }
              break;
              case MODULE_FUNCTION_PAN_RESET:
              { //Pan reset
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    if (AxumData.ModuleData[ModuleNr].Panorama != 512)
                    {
                      AxumData.ModuleData[ModuleNr].Panorama = 512;
                      SetAxum_BussLevels(ModuleNr);

                      unsigned int FunctionNrToSent = (ModuleNr<<12);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PAN);

                      DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_PAN);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_MODULE_LEVEL:
              {   //Module level
                float CurrentLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

                if (type == MBN_DATATYPE_UINT)
                {
                  int Position = (data.SInt*1023)/(DataMaximal-DataMinimal);
                  float dB = Position2dB[Position];
                  dB -= AxumData.LevelReserve;

                  AxumData.ModuleData[ModuleNr].FaderLevel = dB;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].FaderLevel += data.SInt;
                  if (AxumData.ModuleData[ModuleNr].FaderLevel < -140)
                  {
                    AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                  }
                  else
                  {
                    if (AxumData.ModuleData[ModuleNr].FaderLevel > (10-AxumData.LevelReserve))
                    {
                      AxumData.ModuleData[ModuleNr].FaderLevel = (10-AxumData.LevelReserve);
                    }
                  }
                }
                else if (type == MBN_DATATYPE_FLOAT)
                {
                  AxumData.ModuleData[ModuleNr].FaderLevel = data.Float;
                  if (AxumData.ModuleData[ModuleNr].FaderLevel < -140)
                  {
                    AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                  }
                  else
                  {
                    if (AxumData.ModuleData[ModuleNr].FaderLevel > (10-AxumData.LevelReserve))
                    {
                      AxumData.ModuleData[ModuleNr].FaderLevel = (10-AxumData.LevelReserve);
                    }
                  }
                }
                float NewLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

                SetAxum_BussLevels(ModuleNr);

                unsigned int FunctionNrToSend = ModuleNr<<12;
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_MODULE_LEVEL);

                if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                    ((CurrentLevel>-80) && (NewLevel<=-80)))
                { //fader on changed
                  DoAxum_ModuleStatusChanged(ModuleNr, 1);

                  unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_OFF);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON_OFF);

                  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                  {
                    int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource - matrix_sources.src_offset.min.source;

                    unsigned int FunctionNrToSend  = 0x05000000 | (SourceNr<<12);
                    CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON);
                    CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_OFF);
                    CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
                    CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                    CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                    CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
                  }

                  unsigned char On = AxumData.ModuleData[ModuleNr].On;
                  DoAxum_StartStopTrigger(ModuleNr, CurrentLevel, NewLevel, On, On);
                }
              }
              break;
              case MODULE_FUNCTION_MODULE_ON:
              case MODULE_FUNCTION_MODULE_OFF:
              case MODULE_FUNCTION_MODULE_ON_OFF:
              {   //Module on
                //Module off
                //Module on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  int CurrentOn = AxumData.ModuleData[ModuleNr].On;
                  if (data.State)
                  {
                    switch (FunctionNr)
                    {
                      case MODULE_FUNCTION_MODULE_ON:
                      {
                        AxumData.ModuleData[ModuleNr].On = 1;
                      }
                      break;
                      case MODULE_FUNCTION_MODULE_OFF:
                      {
                        AxumData.ModuleData[ModuleNr].On = 0;
                      }
                      break;
                      case MODULE_FUNCTION_MODULE_ON_OFF:
                      {
                        AxumData.ModuleData[ModuleNr].On = !AxumData.ModuleData[ModuleNr].On;
                      }
                      break;
                    }
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        switch (FunctionNr)
                        {
                          case MODULE_FUNCTION_MODULE_ON_OFF:
                          {
                            AxumData.ModuleData[ModuleNr].On = !AxumData.ModuleData[ModuleNr].On;
                          }
                          break;
                        }
                      }
                    }
                  }
                  int NewOn = AxumData.ModuleData[ModuleNr].On;

                  SetAxum_BussLevels(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON);
                  CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_OFF);
                  CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON_OFF);

                  if (CurrentOn != NewOn)
                  { //module on changed
                    DoAxum_ModuleStatusChanged(ModuleNr, 1);

                    CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                    CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                    CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);

                    if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                    {
                      unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                      FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_OFF);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON_OFF);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
                    }

                    float FaderLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
                    DoAxum_StartStopTrigger(ModuleNr, FaderLevel, FaderLevel, CurrentOn, NewOn);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_LEVEL:
              case MODULE_FUNCTION_BUSS_3_4_LEVEL:
              case MODULE_FUNCTION_BUSS_5_6_LEVEL:
              case MODULE_FUNCTION_BUSS_7_8_LEVEL:
              case MODULE_FUNCTION_BUSS_9_10_LEVEL:
              case MODULE_FUNCTION_BUSS_11_12_LEVEL:
              case MODULE_FUNCTION_BUSS_13_14_LEVEL:
              case MODULE_FUNCTION_BUSS_15_16_LEVEL:
              case MODULE_FUNCTION_BUSS_17_18_LEVEL:
              case MODULE_FUNCTION_BUSS_19_20_LEVEL:
              case MODULE_FUNCTION_BUSS_21_22_LEVEL:
              case MODULE_FUNCTION_BUSS_23_24_LEVEL:
              case MODULE_FUNCTION_BUSS_25_26_LEVEL:
              case MODULE_FUNCTION_BUSS_27_28_LEVEL:
              case MODULE_FUNCTION_BUSS_29_30_LEVEL:
              case MODULE_FUNCTION_BUSS_31_32_LEVEL:
              { //Buss level
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_LEVEL)/(MODULE_FUNCTION_BUSS_3_4_LEVEL-MODULE_FUNCTION_BUSS_1_2_LEVEL);
                unsigned char CurrentPresetState[8];
                unsigned char cntPreset;

                for (cntPreset=0; cntPreset<8; cntPreset++)
                {
                  CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
                }
                if (type == MBN_DATATYPE_UINT)
                {
                  int Position = ((data.UInt-DataMinimal)*1023)/(DataMaximal-DataMinimal);
                  AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = Position2dB[Position];
                  AxumData.ModuleData[ModuleNr].Buss[BussNr].Level -= AxumData.LevelReserve;

                  SetAxum_BussLevels(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_BUSS_1_2-MODULE_CONTROL_MODE_BUSS_3_4)*BussNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_BUSS_1_2+FunctionOffset);
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].Buss[BussNr].Level += data.SInt;
                  if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level < -140)
                  {
                    AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = -140;
                  }
                  else
                  {
                    if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level > (10-AxumData.LevelReserve))
                    {
                      AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = (10-AxumData.LevelReserve);
                    }
                  }
                  SetAxum_BussLevels(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_BUSS_1_2-MODULE_CONTROL_MODE_BUSS_3_4)*BussNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_BUSS_1_2+FunctionOffset);
                }
                else if (type == MBN_DATATYPE_FLOAT)
                {
                  AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = data.Float;
                  if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level < -140)
                  {
                    AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = -140;
                  }
                  else
                  {
                    if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level > (10-AxumData.LevelReserve))
                    {
                      AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = (10-AxumData.LevelReserve);
                    }
                  }
                  SetAxum_BussLevels(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MODULE_CONTROL_MODE_BUSS_1_2-MODULE_CONTROL_MODE_BUSS_3_4)*BussNr;
                  DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_BUSS_1_2+FunctionOffset);
                }
                for (cntPreset=0; cntPreset<8; cntPreset++)
                {
                  if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
                  {
                    unsigned int FunctionNrToSent = (ModuleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

                    //if A or B
                    unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
                    if (cntPreset == PresetNr)
                    {
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                    }
                    else if (cntPreset == (PresetNr+1))
                    {
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_3_4_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_5_6_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_7_8_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_9_10_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_11_12_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_13_14_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_15_16_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_17_18_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_19_20_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_21_22_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_23_24_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_25_26_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_27_28_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_29_30_LEVEL_RESET:
              case MODULE_FUNCTION_BUSS_31_32_LEVEL_RESET:
              { //Buss level reset
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_LEVEL_RESET)/(MODULE_FUNCTION_BUSS_3_4_LEVEL_RESET-MODULE_FUNCTION_BUSS_1_2_LEVEL_RESET);
                unsigned char CurrentPresetState[8];
                unsigned char cntPreset;

                for (cntPreset=0; cntPreset<8; cntPreset++)
                {
                  CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
                }
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level != 0)
                    {
                      AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = 0;
                      SetAxum_BussLevels(ModuleNr);

                      unsigned int DisplayFunctionNr = (ModuleNr<<12);
                      CheckObjectsToSent(DisplayFunctionNr | (MODULE_FUNCTION_BUSS_1_2_LEVEL+(BussNr*(MODULE_FUNCTION_BUSS_3_4_LEVEL-MODULE_FUNCTION_BUSS_1_2_LEVEL))));

                      int FunctionOffset = (MODULE_CONTROL_MODE_BUSS_1_2-MODULE_CONTROL_MODE_BUSS_3_4)*BussNr;
                      DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_BUSS_1_2+FunctionOffset);
                    }
                  }
                }
                for (cntPreset=0; cntPreset<8; cntPreset++)
                {
                  if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
                  {
                    unsigned int FunctionNrToSent = (ModuleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

                    //if A or B
                    unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
                    if (cntPreset == PresetNr)
                    {
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                    }
                    else if (cntPreset == (PresetNr+1))
                    {
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_ON:
              case MODULE_FUNCTION_BUSS_3_4_ON:
              case MODULE_FUNCTION_BUSS_5_6_ON:
              case MODULE_FUNCTION_BUSS_7_8_ON:
              case MODULE_FUNCTION_BUSS_9_10_ON:
              case MODULE_FUNCTION_BUSS_11_12_ON:
              case MODULE_FUNCTION_BUSS_13_14_ON:
              case MODULE_FUNCTION_BUSS_15_16_ON:
              case MODULE_FUNCTION_BUSS_17_18_ON:
              case MODULE_FUNCTION_BUSS_19_20_ON:
              case MODULE_FUNCTION_BUSS_21_22_ON:
              case MODULE_FUNCTION_BUSS_23_24_ON:
              case MODULE_FUNCTION_BUSS_25_26_ON:
              case MODULE_FUNCTION_BUSS_27_28_ON:
              case MODULE_FUNCTION_BUSS_29_30_ON:
              case MODULE_FUNCTION_BUSS_31_32_ON:
              {
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_ON)/(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON);

                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    unsigned char NewState = 1;
                    DoAxum_SetBussOnOff(ModuleNr, BussNr, NewState, 0);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_OFF:
              case MODULE_FUNCTION_BUSS_3_4_OFF:
              case MODULE_FUNCTION_BUSS_5_6_OFF:
              case MODULE_FUNCTION_BUSS_7_8_OFF:
              case MODULE_FUNCTION_BUSS_9_10_OFF:
              case MODULE_FUNCTION_BUSS_11_12_OFF:
              case MODULE_FUNCTION_BUSS_13_14_OFF:
              case MODULE_FUNCTION_BUSS_15_16_OFF:
              case MODULE_FUNCTION_BUSS_17_18_OFF:
              case MODULE_FUNCTION_BUSS_19_20_OFF:
              case MODULE_FUNCTION_BUSS_21_22_OFF:
              case MODULE_FUNCTION_BUSS_23_24_OFF:
              case MODULE_FUNCTION_BUSS_25_26_OFF:
              case MODULE_FUNCTION_BUSS_27_28_OFF:
              case MODULE_FUNCTION_BUSS_29_30_OFF:
              case MODULE_FUNCTION_BUSS_31_32_OFF:
              {
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_OFF)/(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF);
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    unsigned char NewState = 0;
                    DoAxum_SetBussOnOff(ModuleNr, BussNr, NewState, 0);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_ON_OFF:
              case MODULE_FUNCTION_BUSS_3_4_ON_OFF:
              case MODULE_FUNCTION_BUSS_5_6_ON_OFF:
              case MODULE_FUNCTION_BUSS_7_8_ON_OFF:
              case MODULE_FUNCTION_BUSS_9_10_ON_OFF:
              case MODULE_FUNCTION_BUSS_11_12_ON_OFF:
              case MODULE_FUNCTION_BUSS_13_14_ON_OFF:
              case MODULE_FUNCTION_BUSS_15_16_ON_OFF:
              case MODULE_FUNCTION_BUSS_17_18_ON_OFF:
              case MODULE_FUNCTION_BUSS_19_20_ON_OFF:
              case MODULE_FUNCTION_BUSS_21_22_ON_OFF:
              case MODULE_FUNCTION_BUSS_23_24_ON_OFF:
              case MODULE_FUNCTION_BUSS_25_26_ON_OFF:
              case MODULE_FUNCTION_BUSS_27_28_ON_OFF:
              case MODULE_FUNCTION_BUSS_29_30_ON_OFF:
              case MODULE_FUNCTION_BUSS_31_32_ON_OFF:
              { //Buss on/off
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_ON_OFF)/(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF);
                unsigned char NewState = AxumData.ModuleData[ModuleNr].Buss[BussNr].On;

                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    NewState = !AxumData.ModuleData[ModuleNr].Buss[BussNr].On;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        NewState = !AxumData.ModuleData[ModuleNr].Buss[BussNr].On;
                      }
                    }
                  }
                  DoAxum_SetBussOnOff(ModuleNr, BussNr, NewState, 0);
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_PRE:
              case MODULE_FUNCTION_BUSS_3_4_PRE:
              case MODULE_FUNCTION_BUSS_5_6_PRE:
              case MODULE_FUNCTION_BUSS_7_8_PRE:
              case MODULE_FUNCTION_BUSS_9_10_PRE:
              case MODULE_FUNCTION_BUSS_11_12_PRE:
              case MODULE_FUNCTION_BUSS_13_14_PRE:
              case MODULE_FUNCTION_BUSS_15_16_PRE:
              case MODULE_FUNCTION_BUSS_17_18_PRE:
              case MODULE_FUNCTION_BUSS_19_20_PRE:
              case MODULE_FUNCTION_BUSS_21_22_PRE:
              case MODULE_FUNCTION_BUSS_23_24_PRE:
              case MODULE_FUNCTION_BUSS_25_26_PRE:
              case MODULE_FUNCTION_BUSS_27_28_PRE:
              case MODULE_FUNCTION_BUSS_29_30_PRE:
              case MODULE_FUNCTION_BUSS_31_32_PRE:
              { //Buss pre
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_PRE)/(MODULE_FUNCTION_BUSS_3_4_PRE-MODULE_FUNCTION_BUSS_1_2_PRE);
                unsigned char CurrentPresetState[8];
                unsigned char cntPreset;

                for (cntPreset=0; cntPreset<8; cntPreset++)
                {
                  CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
                }
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].Buss[BussNr].PreModuleLevel = !AxumData.ModuleData[ModuleNr].Buss[BussNr].PreModuleLevel;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.ModuleData[ModuleNr].Buss[BussNr].PreModuleLevel = !AxumData.ModuleData[ModuleNr].Buss[BussNr].PreModuleLevel;
                      }
                    }
                  }

                  SetAxum_BussLevels(ModuleNr);

                  unsigned int FunctionNrToSend = ModuleNr<<12;
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                }
                for (cntPreset=0; cntPreset<8; cntPreset++)
                {
                  if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
                  {
                    unsigned int FunctionNrToSent = (ModuleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

                    //if A or B
                    unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
                    if (cntPreset == PresetNr)
                    {
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                    }
                    else if (cntPreset == (PresetNr+1))
                    {
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_BALANCE:
              case MODULE_FUNCTION_BUSS_3_4_BALANCE:
              case MODULE_FUNCTION_BUSS_5_6_BALANCE:
              case MODULE_FUNCTION_BUSS_7_8_BALANCE:
              case MODULE_FUNCTION_BUSS_9_10_BALANCE:
              case MODULE_FUNCTION_BUSS_11_12_BALANCE:
              case MODULE_FUNCTION_BUSS_13_14_BALANCE:
              case MODULE_FUNCTION_BUSS_15_16_BALANCE:
              case MODULE_FUNCTION_BUSS_17_18_BALANCE:
              case MODULE_FUNCTION_BUSS_19_20_BALANCE:
              case MODULE_FUNCTION_BUSS_21_22_BALANCE:
              case MODULE_FUNCTION_BUSS_23_24_BALANCE:
              case MODULE_FUNCTION_BUSS_25_26_BALANCE:
              case MODULE_FUNCTION_BUSS_27_28_BALANCE:
              case MODULE_FUNCTION_BUSS_29_30_BALANCE:
              case MODULE_FUNCTION_BUSS_31_32_BALANCE:
              { //Buss balance
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_BALANCE)/(MODULE_FUNCTION_BUSS_3_4_BALANCE-MODULE_FUNCTION_BUSS_1_2_BALANCE);

                if (type == MBN_DATATYPE_UINT)
                {
                  AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = ((data.UInt-DataMinimal)*1023)/(DataMaximal-DataMinimal);
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance += data.SInt;
                  if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance< 0)
                  {
                    AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = 0;
                  }
                  else if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance > 1023)
                  {
                    AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = 1023;
                  }
                }

                SetAxum_BussLevels(ModuleNr);

                unsigned int FunctionNrToSend = ModuleNr<<12;
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                int FunctionOffset = (MODULE_CONTROL_MODE_BUSS_1_2_BALANCE-MODULE_CONTROL_MODE_BUSS_3_4_BALANCE)*BussNr;
                DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_BUSS_1_2_BALANCE+FunctionOffset);
              }
              break;
              case MODULE_FUNCTION_BUSS_1_2_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_3_4_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_5_6_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_7_8_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_9_10_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_11_12_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_13_14_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_15_16_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_17_18_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_19_20_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_21_22_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_23_24_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_25_26_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_27_28_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_29_30_BALANCE_RESET:
              case MODULE_FUNCTION_BUSS_31_32_BALANCE_RESET:
              { //Buss pre
                int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_BALANCE_RESET)/(MODULE_FUNCTION_BUSS_3_4_BALANCE_RESET-MODULE_FUNCTION_BUSS_1_2_BALANCE_RESET);

                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = 512;
                    SetAxum_BussLevels(ModuleNr);
                    unsigned int DisplayFunctionNumber = (ModuleNr<<12);
                    CheckObjectsToSent(DisplayFunctionNumber | (MODULE_FUNCTION_BUSS_1_2_BALANCE+(BussNr*(MODULE_FUNCTION_BUSS_3_4_BALANCE-MODULE_FUNCTION_BUSS_1_2_BALANCE))));

                    int FunctionOffset = (MODULE_CONTROL_MODE_BUSS_1_2_BALANCE-MODULE_CONTROL_MODE_BUSS_3_4_BALANCE)*BussNr;
                    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_BUSS_1_2_BALANCE+FunctionOffset);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_SOURCE_START:
              case MODULE_FUNCTION_SOURCE_STOP:
              case MODULE_FUNCTION_SOURCE_START_STOP:
              {   //Start
                if (type == MBN_DATATYPE_STATE)
                {
                  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                  {
                    int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                    char UpdateObjects = 0;

                    if (data.State)
                    {
                      switch (FunctionNr)
                      {
                        case MODULE_FUNCTION_SOURCE_START:
                        {
                          AxumData.SourceData[SourceNr].Start = 1;
                        }
                        break;
                        case MODULE_FUNCTION_SOURCE_STOP:
                        {
                          AxumData.SourceData[SourceNr].Start = 0;
                        }
                        break;
                        case MODULE_FUNCTION_SOURCE_START_STOP:
                        {
                          AxumData.SourceData[SourceNr].Start = !AxumData.SourceData[SourceNr].Start;
                        }
                        break;
                      }
                      UpdateObjects = 1;
                    }
                    else
                    {
                      int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                      if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                      {
                        if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                        {
                          switch (FunctionNr)
                          {
                            case MODULE_FUNCTION_SOURCE_START_STOP:
                            {
                              AxumData.SourceData[SourceNr].Start = !AxumData.SourceData[SourceNr].Start;
                              UpdateObjects = 1;
                            }
                            break;
                          }
                        }
                      }
                    }


                    if (UpdateObjects)
                    { //Only if pushed or changed
                      unsigned int DisplayFunctionNr = 0x05000000 | (SourceNr<<12);
                      CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_START);
                      CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_STOP);
                      CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_START_STOP);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                        {
                          DisplayFunctionNr = (cntModule<<12);
                          CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_START);
                          CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_STOP);
                          CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_START_STOP);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_COUGH_ON_OFF:
              { //Cough on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  if (AxumData.ModuleData[ModuleNr].Cough != data.State)
                  {
                    AxumData.ModuleData[ModuleNr].Cough = data.State;
                    SetAxum_BussLevels(ModuleNr);

                    unsigned int FunctionNrToSend = ModuleNr<<12;
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                    {
                      unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                      FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_COUGH_ON_OFF);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_SOURCE_ALERT:
              { //Alert
                if (type == MBN_DATATYPE_STATE)
                {
                  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                  {
                    int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                    int OldState = AxumData.SourceData[SourceNr].Alert;

                    AxumData.SourceData[SourceNr].Alert = data.State;

                    if (OldState != AxumData.SourceData[SourceNr].Alert)
                    {
                      unsigned int DisplayFunctionNr = 0x05000000 | (SourceNr<<12) | SOURCE_FUNCTION_ALERT;
                      CheckObjectsToSent(DisplayFunctionNr);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                        {
                          DisplayFunctionNr = (cntModule<<12) | MODULE_FUNCTION_SOURCE_ALERT;
                          CheckObjectsToSent(DisplayFunctionNr);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_CONTROL:
              case MODULE_FUNCTION_CONTROL_1:
              case MODULE_FUNCTION_CONTROL_2:
              case MODULE_FUNCTION_CONTROL_3:
              case MODULE_FUNCTION_CONTROL_4:
              {   //Control 1-4
                unsigned int FunctionNrToSend = ModuleNr<<12;
                ModeControllerSensorChange(FunctionNrToSend | FunctionNr, type, data, DataType, DataSize, DataMinimal, DataMaximal);
              }
              break;
              case MODULE_FUNCTION_CONTROL_LABEL:
              case MODULE_FUNCTION_CONTROL_1_LABEL:
              case MODULE_FUNCTION_CONTROL_2_LABEL:
              case MODULE_FUNCTION_CONTROL_3_LABEL:
              case MODULE_FUNCTION_CONTROL_4_LABEL:
              {   //Control 1 label, no receive
              }
              break;
              case MODULE_FUNCTION_CONTROL_RESET:
              case MODULE_FUNCTION_CONTROL_1_RESET:
              case MODULE_FUNCTION_CONTROL_2_RESET:
              case MODULE_FUNCTION_CONTROL_3_RESET:
              case MODULE_FUNCTION_CONTROL_4_RESET:
              {   //Control 1 reset
                unsigned int FunctionNrToSend = ModuleNr<<12;
                ModeControllerResetSensorChange(FunctionNrToSend | FunctionNr, type, data, DataType, DataSize, DataMinimal, DataMaximal);
              }
              break;
              case MODULE_FUNCTION_ROUTING_PRESET_1A:
              case MODULE_FUNCTION_ROUTING_PRESET_1B:
              case MODULE_FUNCTION_ROUTING_PRESET_2A:
              case MODULE_FUNCTION_ROUTING_PRESET_2B:
              case MODULE_FUNCTION_ROUTING_PRESET_3A:
              case MODULE_FUNCTION_ROUTING_PRESET_3B:
              case MODULE_FUNCTION_ROUTING_PRESET_4A:
              case MODULE_FUNCTION_ROUTING_PRESET_4B:
              {
                unsigned char PresetNr = FunctionNr - MODULE_FUNCTION_ROUTING_PRESET_1A;
                bool SourceActive = false;

                if (AxumData.ModuleData[ModuleNr].On)
                {
                  if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
                  {
                    SourceActive = 1;
                  }
                }
                if (type == MBN_DATATYPE_STATE)
                {
                  if ((!SourceActive) || (AxumData.ModuleData[ModuleNr].OverruleActive))
                  {
                    if (data.State)
                    {
                      DoAxum_LoadRoutingPreset(ModuleNr, PresetNr, 0, 0, 0);
                    }
                  }
                  else
                  {
                    if (data.State)
                    {
                      AxumData.ModuleData[ModuleNr].WaitingRoutingPreset = PresetNr;
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_PHASE:
              {
                if (type == MBN_DATATYPE_SINT)
                {
                  unsigned char OldPhase = AxumData.ModuleData[ModuleNr].Phase;
                  AxumData.ModuleData[ModuleNr].Phase += data.SInt;
                  AxumData.ModuleData[ModuleNr].Phase &= 0x03;

                  if (OldPhase != AxumData.ModuleData[ModuleNr].Phase)
                  {
                    SetAxum_ModuleProcessing(ModuleNr);
                    unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE);

                    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_PHASE);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_MONO:
              {
                if (type == MBN_DATATYPE_SINT)
                {
                  unsigned char OldMono = AxumData.ModuleData[ModuleNr].Mono;

                  AxumData.ModuleData[ModuleNr].Mono += data.SInt;
                  AxumData.ModuleData[ModuleNr].Mono &= 0x03;

                  if (AxumData.ModuleData[ModuleNr].Mono != OldMono)
                  {
                    SetAxum_ModuleProcessing(ModuleNr);

                    unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO);

                    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_MONO);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_FADER_AND_ON_ACTIVE:
              case MODULE_FUNCTION_FADER_AND_ON_INACTIVE:
              case MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE:
              {   //fader on and on active
                //fader on and on inactive
                if (type == MBN_DATATYPE_STATE)
                {
                  float CurrentLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
                  float CurrentOn = AxumData.ModuleData[ModuleNr].On;
                  if (data.State)
                  {
                    switch (FunctionNr)
                    {
                      case MODULE_FUNCTION_FADER_AND_ON_ACTIVE:
                      {
                        AxumData.ModuleData[ModuleNr].FaderLevel = 0;
                        AxumData.ModuleData[ModuleNr].On = 1;
                      }
                      break;
                      case MODULE_FUNCTION_FADER_AND_ON_INACTIVE:
                      {
                        AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                        AxumData.ModuleData[ModuleNr].On = 0;
                      }
                      break;
                      case MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE:
                      {
                        if ((AxumData.ModuleData[ModuleNr].FaderLevel >= -80) && (AxumData.ModuleData[ModuleNr].On))
                        {
                          AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                          AxumData.ModuleData[ModuleNr].On = 0;
                        }
                        else
                        {
                          AxumData.ModuleData[ModuleNr].FaderLevel = 0;
                          AxumData.ModuleData[ModuleNr].On = 1;
                        }
                      }
                    }
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                      }
                    }
                  }
                  float NewLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
                  int NewOn = AxumData.ModuleData[ModuleNr].On;

                  SetAxum_BussLevels(ModuleNr);

                  unsigned int FunctionNrToSend = ((ModuleNr)<<12);
                  if (CurrentOn != NewOn)
                  {
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_OFF);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON_OFF);
                  }
                  if (CurrentLevel != NewLevel)
                  {
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_LEVEL);
                    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_MODULE_LEVEL);
                  }

                  if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                      ((CurrentLevel>-80) && (NewLevel<=-80)) ||
                      (CurrentOn != NewOn))
                  { //fader on changed
                    DoAxum_ModuleStatusChanged(ModuleNr, 1);

                    FunctionNrToSend = ((ModuleNr)<<12);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_ON);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_OFF);
                    CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_ON_OFF);

                    if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                    {
                      unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                      FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_OFF);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON_OFF);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_OFF);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                      CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
                    }

                    DoAxum_StartStopTrigger(ModuleNr, CurrentLevel, NewLevel, CurrentOn, NewOn);
                  }
                }
              }
              break;
              case MODULE_FUNCTION_FADER_ON:
              case MODULE_FUNCTION_FADER_OFF:
              case MODULE_FUNCTION_FADER_ON_OFF:
              {   //Module on
                //Module off
                //Module on/off
                float CurrentLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    switch (FunctionNr)
                    {
                      case MODULE_FUNCTION_FADER_ON:
                      {
                        AxumData.ModuleData[ModuleNr].FaderLevel = 0;
                      }
                      break;
                      case MODULE_FUNCTION_FADER_OFF:
                      {
                        AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                      }
                      break;
                      case MODULE_FUNCTION_FADER_ON_OFF:
                      {
                        if (AxumData.ModuleData[ModuleNr].FaderLevel<-80)
                        {
                          AxumData.ModuleData[ModuleNr].FaderLevel = 0;
                        }
                        else
                        {
                          AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                        }
                      }
                      break;
                    }
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        switch (FunctionNr)
                        {
                          case MODULE_FUNCTION_FADER_ON_OFF:
                          {
                            if (AxumData.ModuleData[ModuleNr].FaderLevel<-80)
                            {
                              AxumData.ModuleData[ModuleNr].FaderLevel = 0;
                            }
                            else
                            {
                              AxumData.ModuleData[ModuleNr].FaderLevel = -140;
                            }
                          }
                          break;
                        }
                      }
                    }
                  }
                  float NewLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

                  if (NewLevel != CurrentLevel)
                  {
                    SetAxum_BussLevels(ModuleNr);

                    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
                    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_LEVEL);

                    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_MODULE_LEVEL);

                    if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                        ((CurrentLevel>-80) && (NewLevel<=-80)))
                    { //fader on changed
                      DoAxum_ModuleStatusChanged(ModuleNr, 1);

                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_OFF);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON_OFF);

                      if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
                      {
                        unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource - matrix_sources.src_offset.min.source;
                        unsigned int FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_OFF);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
                      }

                      unsigned char On = AxumData.ModuleData[ModuleNr].On;
                      DoAxum_StartStopTrigger(ModuleNr, CurrentLevel, NewLevel, On, On);
                    }
                  }
                }
              }
              break;
              case MODULE_FUNCTION_TALKBACK_1_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_2_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_3_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_4_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_5_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_6_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_7_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_8_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_9_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_10_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_11_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_12_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_13_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_14_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_15_TO_REL_DEST:
              case MODULE_FUNCTION_TALKBACK_16_TO_REL_DEST:
              {
                int TalkbackNr = FunctionNr-MODULE_FUNCTION_TALKBACK_1_TO_REL_DEST;
                if (type == MBN_DATATYPE_STATE)
                {
                  DoAxum_TalkbackToRelatedDestination(ModuleNr, TalkbackNr, data.State, 1);
                }
              }
              break;
              case MODULE_FUNCTION_SELECT_1:
              case MODULE_FUNCTION_SELECT_2:
              case MODULE_FUNCTION_SELECT_3:
              case MODULE_FUNCTION_SELECT_4:
              {
                int ConsoleNr = FunctionNr-MODULE_FUNCTION_SELECT_1;
                unsigned int NewSelectedModuleNr = AxumData.ConsoleData[ConsoleNr].SelectedModule;

                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      NewSelectedModuleNr = ModuleNr;
                    }
                  }
                  break;
                }

                SetSelectedModule(ConsoleNr, NewSelectedModuleNr);
              }
              break;
            }
          }
          break;
          case BUSS_FUNCTIONS:
          {   //Busses
            unsigned int BussNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((BussNr>=NUMBER_OF_BUSSES) && (BussNr<(NUMBER_OF_BUSSES+NUMBER_OF_CONSOLES)))
            {
              int ConsoleNr = BussNr-NUMBER_OF_BUSSES;
              BussNr = AxumData.ConsoleData[ConsoleNr].SelectedBuss;
            }

            switch (FunctionNr)
            {
              case BUSS_FUNCTION_MASTER_LEVEL:
              {
                float OldLevel = AxumData.BussMasterData[BussNr].Level;

                if (type == MBN_DATATYPE_UINT)
                {
                  int Position = ((data.UInt-DataMinimal)*1023)/(DataMaximal-DataMinimal);
                  AxumData.BussMasterData[BussNr].Level = Position2dB[Position];
                  AxumData.BussMasterData[BussNr].Level -= 10;//AxumData.LevelReserve;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.BussMasterData[BussNr].Level += data.SInt;
                  if (AxumData.BussMasterData[BussNr].Level < -140)
                  {
                    AxumData.BussMasterData[BussNr].Level = -140;
                  }
                  else
                  {
                    if (AxumData.BussMasterData[BussNr].Level > 0)
                    {
                      AxumData.BussMasterData[BussNr].Level = 0;
                    }
                  }
                }
                else if (type == MBN_DATATYPE_FLOAT)
                {
                  AxumData.BussMasterData[BussNr].Level = data.Float;
                  if (AxumData.BussMasterData[BussNr].Level < -140)
                  {
                    AxumData.BussMasterData[BussNr].Level = -140;
                  }
                  else
                  {
                    if (AxumData.BussMasterData[BussNr].Level > 0)
                    {
                      AxumData.BussMasterData[BussNr].Level = 0;
                    }
                  }
                }

                if (AxumData.BussMasterData[BussNr].Level != OldLevel)
                {
                  SetAxum_BussMasterLevels();

                  unsigned int FunctionNrToSend = 0x01000000 | (BussNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  int FunctionOffset = (MASTER_CONTROL_MODE_BUSS_3_4-MASTER_CONTROL_MODE_BUSS_1_2)*BussNr;
                  DoAxum_UpdateMasterControlMode(MASTER_CONTROL_MODE_BUSS_1_2+FunctionOffset);
                }
              }
              break;
              case BUSS_FUNCTION_MASTER_LEVEL_RESET:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    if (AxumData.BussMasterData[BussNr].Level != 0)
                    {
                      AxumData.BussMasterData[BussNr].Level = 0;

                      SetAxum_BussMasterLevels();

                      unsigned int FunctionNrToSend = 0x01000000 | (BussNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | BUSS_FUNCTION_MASTER_LEVEL);

                      int FunctionOffset = (MASTER_CONTROL_MODE_BUSS_3_4-MASTER_CONTROL_MODE_BUSS_1_2)*BussNr;
                      DoAxum_UpdateMasterControlMode(MASTER_CONTROL_MODE_BUSS_1_2+FunctionOffset);
                    }
                  }
                }
              }
              break;
              case BUSS_FUNCTION_MASTER_ON_OFF:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldOn = AxumData.BussMasterData[BussNr].On;
                  if (data.State)
                  {
                    AxumData.BussMasterData[BussNr].On = !AxumData.BussMasterData[BussNr].On;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.BussMasterData[BussNr].On = !AxumData.BussMasterData[BussNr].On;
                      }
                    }
                  }

                  if (AxumData.BussMasterData[BussNr].On != OldOn)
                  {
                    SetAxum_BussMasterLevels();

                    unsigned int FunctionNrToSend = 0x01000000 | (BussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                  }
                }
              }
              break;
              case BUSS_FUNCTION_MASTER_PRE:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  int NrOfBussPre = 0;
                  int NrOfModules = 0;
                  bool OldBussPre = 0;
                  bool NewBussPre = 0;
                  for (int cntModule=0; cntModule<128; cntModule++)
                  {
                    if ((AxumData.ModuleData[cntModule].Buss[BussNr].Assigned) && (AxumData.ModuleData[cntModule].Console == AxumData.BussMasterData[BussNr].Console))
                    {
                      NrOfModules++;
                      if (AxumData.ModuleData[cntModule].Buss[BussNr].PreModuleLevel)
                      {
                        NrOfBussPre++;
                      }
                    }
                  }
                  if ((NrOfBussPre*2) > NrOfModules)
                  {
                    NewBussPre = 1;
                    OldBussPre = 1;
                  }

                  if (data.State)
                  {
                    NewBussPre = !OldBussPre;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        NewBussPre = !OldBussPre;
                      }
                    }
                  }

                  if (NewBussPre != OldBussPre)
                  {
                    for (int cntModule=0; cntModule<128; cntModule++)
                    {
                      if ((AxumData.ModuleData[cntModule].Buss[BussNr].Assigned) && (AxumData.ModuleData[cntModule].Console == AxumData.BussMasterData[BussNr].Console))
                      {
                        AxumData.ModuleData[cntModule].Buss[BussNr].PreModuleLevel = NewBussPre;
                        SetAxum_BussLevels(cntModule);
                      }
                    }

                    unsigned int FunctionNrToSend = 0x01000000 | (BussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                  }
                }
              }
              break;
              case BUSS_FUNCTION_LABEL:
              {   //No sensor change available
              }
              break;
              case BUSS_FUNCTION_SELECT_1:
              case BUSS_FUNCTION_SELECT_2:
              case BUSS_FUNCTION_SELECT_3:
              case BUSS_FUNCTION_SELECT_4:
              {
                int ConsoleNr = FunctionNr-BUSS_FUNCTION_SELECT_1;
                unsigned int NewSelectedBussNr = AxumData.ConsoleData[ConsoleNr].SelectedBuss;

                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      NewSelectedBussNr = BussNr;
                    }
                  }
                  break;
                }

                SetSelectedBuss(ConsoleNr, NewSelectedBussNr);
              }
              break;
              case BUSS_FUNCTION_RESET:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      DoAxum_BussReset(BussNr);
                    }
                  }
                  break;
                }
              }
              break;
              case BUSS_FUNCTION_TALKBACK_1:
              case BUSS_FUNCTION_TALKBACK_2:
              case BUSS_FUNCTION_TALKBACK_3:
              case BUSS_FUNCTION_TALKBACK_4:
              case BUSS_FUNCTION_TALKBACK_5:
              case BUSS_FUNCTION_TALKBACK_6:
              case BUSS_FUNCTION_TALKBACK_7:
              case BUSS_FUNCTION_TALKBACK_8:
              case BUSS_FUNCTION_TALKBACK_9:
              case BUSS_FUNCTION_TALKBACK_10:
              case BUSS_FUNCTION_TALKBACK_11:
              case BUSS_FUNCTION_TALKBACK_12:
              case BUSS_FUNCTION_TALKBACK_13:
              case BUSS_FUNCTION_TALKBACK_14:
              case BUSS_FUNCTION_TALKBACK_15:
              case BUSS_FUNCTION_TALKBACK_16:
              {
                int TalkbackNr = FunctionNr-BUSS_FUNCTION_TALKBACK_1;

                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldTalkbackActive = 0;
                  bool OldTalkback = AxumData.BussMasterData[BussNr].Talkback[TalkbackNr];
                  for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
                  {
                    OldTalkbackActive |= AxumData.BussMasterData[BussNr].Talkback[cntTalkback];
                  }

                  AxumData.BussMasterData[BussNr].Talkback[TalkbackNr] = data.State;

                  if (AxumData.BussMasterData[BussNr].Talkback[TalkbackNr] != OldTalkback)
                  {
                    //Check talkbacks, if dim is required
                    int NewTalkbackActive = 0;
                    for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
                    {
                      NewTalkbackActive |= AxumData.BussMasterData[BussNr].Talkback[cntTalkback];
                    }

                    unsigned int FunctionNrToSend = 0x01000000 | (BussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    if (NewTalkbackActive != OldTalkbackActive)
                    {
                      AxumData.BussMasterData[BussNr].Dim = NewTalkbackActive;
                    }

                    for (int cntDestination=0; cntDestination<1280; cntDestination++)
                    {
                      if (AxumData.DestinationData[cntDestination].Source == (int)(matrix_sources.src_offset.min.buss+BussNr))
                      {
                        unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);

                        CheckObjectsToSent(DisplayFunctionNr | (DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1 + ((DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1)*TalkbackNr)));
                        CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
                      }
                    }
                  }
                }
              }
              break;
              default:
              { //not implemented function
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTIONS:
          {   //Monitor Busses
            int MonitorBussNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((MonitorBussNr>=NUMBER_OF_MONITOR_BUSSES) && (MonitorBussNr<(NUMBER_OF_MONITOR_BUSSES+NUMBER_OF_CONSOLES)))
            {
              int ConsoleNr = MonitorBussNr-NUMBER_OF_MONITOR_BUSSES;
              MonitorBussNr = AxumData.ConsoleData[ConsoleNr].SelectedMonitorBuss;
            }

            if (type == MBN_DATATYPE_STATE)
            {
              if (((FunctionNr>=MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF) && (FunctionNr<=MONITOR_BUSS_FUNCTION_EXT_8_ON_OFF)) ||
                  ((FunctionNr>=MONITOR_BUSS_FUNCTION_BUSS_1_2_ON) && (FunctionNr<=MONITOR_BUSS_FUNCTION_EXT_8_ON)) ||
                  ((FunctionNr>=MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF) && (FunctionNr<=MONITOR_BUSS_FUNCTION_EXT_8_OFF)))
              {
                bool NewMonitorSwitchState = false;
                bool PreventDoingInterlock = false;
                int BussNr = 0;

                switch (FunctionNr)
                {
                  case MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_3_4_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_5_6_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_7_8_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_9_10_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_11_12_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_13_14_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_15_16_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_17_18_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_19_20_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_21_22_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_23_24_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_25_26_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_27_28_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_29_30_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_31_32_ON_OFF:
                  {
                    BussNr = FunctionNr - MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF;

                    NewMonitorSwitchState = AxumData.Monitor[MonitorBussNr].Buss[BussNr];

                    if (data.State)
                    {
                      NewMonitorSwitchState = !AxumData.Monitor[MonitorBussNr].Buss[BussNr];
                    }
                    else
                    {
                      int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                      PreventDoingInterlock = true;
                      if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                      {
                        if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                        {
                          NewMonitorSwitchState = !AxumData.Monitor[MonitorBussNr].Buss[BussNr];
                          PreventDoingInterlock = false;
                        }
                      }
                    }
                  }
                  break;
                  case MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_2_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_3_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_4_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_5_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_6_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_7_ON_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_8_ON_OFF:
                  {
                    int ExtNr = FunctionNr - MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF;
                    if (AxumData.ExternSource[MonitorBussNr/4].InterlockSafe[ExtNr])
                    {
                      PreventDoingInterlock = true;
                    }
                    BussNr = ExtNr+16;

                    NewMonitorSwitchState = AxumData.Monitor[MonitorBussNr].Ext[ExtNr];

                    if (data.State)
                    {
                      NewMonitorSwitchState = !AxumData.Monitor[MonitorBussNr].Ext[ExtNr];
                    }
                    else
                    {
                      int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                      PreventDoingInterlock = true;
                      if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                      {
                        if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                        {
                          NewMonitorSwitchState = !AxumData.Monitor[MonitorBussNr].Ext[ExtNr];
                          PreventDoingInterlock = false;
                        }
                      }
                    }
                  }
                  break;
                  case MONITOR_BUSS_FUNCTION_BUSS_1_2_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_3_4_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_5_6_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_7_8_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_9_10_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_11_12_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_13_14_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_15_16_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_17_18_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_19_20_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_21_22_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_23_24_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_25_26_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_27_28_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_29_30_ON:
                  case MONITOR_BUSS_FUNCTION_BUSS_31_32_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_1_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_2_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_3_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_4_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_5_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_6_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_7_ON:
                  case MONITOR_BUSS_FUNCTION_EXT_8_ON:
                  {
                    BussNr = FunctionNr-MONITOR_BUSS_FUNCTION_BUSS_1_2_ON;
                    NewMonitorSwitchState = true;
                  }
                  break;
                  case MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_3_4_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_5_6_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_7_8_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_9_10_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_11_12_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_13_14_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_15_16_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_17_18_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_19_20_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_21_22_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_23_24_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_25_26_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_27_28_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_29_30_OFF:
                  case MONITOR_BUSS_FUNCTION_BUSS_31_32_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_1_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_2_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_3_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_4_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_5_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_6_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_7_OFF:
                  case MONITOR_BUSS_FUNCTION_EXT_8_OFF:
                  {
                    BussNr = FunctionNr-MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF;
                    NewMonitorSwitchState = false;
                  }
                  break;
                }

                DoAxum_SetCRMBussOnOff(MonitorBussNr, BussNr, NewMonitorSwitchState, PreventDoingInterlock);
              }
            }

            switch (FunctionNr)
            {
              case MONITOR_BUSS_FUNCTION_MUTE:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldMute = AxumData.Monitor[MonitorBussNr].Mute;
                  if (data.State)
                  {
                    AxumData.Monitor[MonitorBussNr].Mute = !AxumData.Monitor[MonitorBussNr].Mute;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.Monitor[MonitorBussNr].Mute = !AxumData.Monitor[MonitorBussNr].Mute;
                      }
                    }
                  }

                  if (AxumData.Monitor[MonitorBussNr].Mute != OldMute)
                  {
                    unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    for (int cntDestination=0; cntDestination<1280; cntDestination++)
                    {
                      if (AxumData.DestinationData[cntDestination].Source == ((int)(matrix_sources.src_offset.min.monitor_buss+MonitorBussNr)))
                      {
                        unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                        CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_MUTE_AND_MONITOR_MUTE);
                      }
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_DIM:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldDim = AxumData.Monitor[MonitorBussNr].Dim;

                  if (data.State)
                  {
                    AxumData.Monitor[MonitorBussNr].Dim = !AxumData.Monitor[MonitorBussNr].Dim;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.Monitor[MonitorBussNr].Dim = !AxumData.Monitor[MonitorBussNr].Dim;
                      }
                    }
                  }

                  if (AxumData.Monitor[MonitorBussNr].Dim != OldDim)
                  {
                    unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    for (int cntDestination=0; cntDestination<1280; cntDestination++)
                    {
                      if (AxumData.DestinationData[cntDestination].Source == ((int)(matrix_sources.src_offset.min.monitor_buss+MonitorBussNr)))
                      {
                        unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                        CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
                      }
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_PHONES_LEVEL:
              {
                float OldLevel = AxumData.Monitor[MonitorBussNr].PhonesLevel;

                if (type == MBN_DATATYPE_UINT)
                {
                  int Position = (data.UInt*1023)/(DataMaximal-DataMinimal);
                  float dB = Position2dB[Position];
                  dB +=10; //20dB reserve

                  AxumData.Monitor[MonitorBussNr].PhonesLevel = dB;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.Monitor[MonitorBussNr].PhonesLevel += (float)data.SInt/10;
                  if (AxumData.Monitor[MonitorBussNr].PhonesLevel<-140)
                  {
                    AxumData.Monitor[MonitorBussNr].PhonesLevel = -140;
                  }
                  else
                  {
                    if (AxumData.Monitor[MonitorBussNr].PhonesLevel > 20)
                    {
                      AxumData.Monitor[MonitorBussNr].PhonesLevel = 20;
                    }
                  }
                }

                if (AxumData.Monitor[MonitorBussNr].PhonesLevel != OldLevel)
                {
                  unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  for (int cntDestination=0; cntDestination<1280; cntDestination++)
                  {
                    if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
                    {
                      unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                      CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_MONITOR_PHONES_LEVEL);
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_MONO:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldMono = AxumData.Monitor[MonitorBussNr].Mono;

                  if (data.State)
                  {
                    AxumData.Monitor[MonitorBussNr].Mono = !AxumData.Monitor[MonitorBussNr].Mono;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.Monitor[MonitorBussNr].Mono = !AxumData.Monitor[MonitorBussNr].Mono;
                      }
                    }
                  }

                  if (AxumData.Monitor[MonitorBussNr].Mono != OldMono)
                  {
                    unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    for (int cntDestination=0; cntDestination<1280; cntDestination++)
                    {
                      if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
                      {
                        unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                        CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_MONO_AND_MONITOR_MONO);
                      }
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_PHASE:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldPhase = AxumData.Monitor[MonitorBussNr].Phase;

                  if (data.State)
                  {
                    AxumData.Monitor[MonitorBussNr].Phase = !AxumData.Monitor[MonitorBussNr].Phase;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.Monitor[MonitorBussNr].Phase = !AxumData.Monitor[MonitorBussNr].Phase;
                      }
                    }
                  }

                  if (AxumData.Monitor[MonitorBussNr].Phase != OldPhase)
                  {
                    unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    for (int cntDestination=0; cntDestination<1280; cntDestination++)
                    {
                      if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
                      {
                        unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                        CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_PHASE_AND_MONITOR_PHASE);
                      }
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_SPEAKER_LEVEL:
              {
                float OldLevel = AxumData.Monitor[MonitorBussNr].SpeakerLevel;

                if (type == MBN_DATATYPE_UINT)
                {
                  int Position = (data.UInt*1023)/(DataMaximal-DataMinimal);
                  float dB = Position2dB[Position];
                  dB += 10;

                  AxumData.Monitor[MonitorBussNr].SpeakerLevel = dB;
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.Monitor[MonitorBussNr].SpeakerLevel += (float)data.SInt/10;
                  if (AxumData.Monitor[MonitorBussNr].SpeakerLevel<-140)
                  {
                    AxumData.Monitor[MonitorBussNr].SpeakerLevel = -140;
                  }
                  else
                  {
                    if (AxumData.Monitor[MonitorBussNr].SpeakerLevel > 20)
                    {
                      AxumData.Monitor[MonitorBussNr].SpeakerLevel = 20;
                    }
                  }
                }

                if (AxumData.Monitor[MonitorBussNr].SpeakerLevel != OldLevel)
                {
                  unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                  for (int cntDestination=0; cntDestination<1280; cntDestination++)
                  {
                    if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
                    {
                      unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                      CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_MONITOR_SPEAKER_LEVEL);
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_TALKBACK_1:
              case MONITOR_BUSS_FUNCTION_TALKBACK_2:
              case MONITOR_BUSS_FUNCTION_TALKBACK_3:
              case MONITOR_BUSS_FUNCTION_TALKBACK_4:
              case MONITOR_BUSS_FUNCTION_TALKBACK_5:
              case MONITOR_BUSS_FUNCTION_TALKBACK_6:
              case MONITOR_BUSS_FUNCTION_TALKBACK_7:
              case MONITOR_BUSS_FUNCTION_TALKBACK_8:
              case MONITOR_BUSS_FUNCTION_TALKBACK_9:
              case MONITOR_BUSS_FUNCTION_TALKBACK_10:
              case MONITOR_BUSS_FUNCTION_TALKBACK_11:
              case MONITOR_BUSS_FUNCTION_TALKBACK_12:
              case MONITOR_BUSS_FUNCTION_TALKBACK_13:
              case MONITOR_BUSS_FUNCTION_TALKBACK_14:
              case MONITOR_BUSS_FUNCTION_TALKBACK_15:
              case MONITOR_BUSS_FUNCTION_TALKBACK_16:
              {
                int TalkbackNr = FunctionNr-MONITOR_BUSS_FUNCTION_TALKBACK_1;
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldTalkbackActive = 0;
                  bool OldTalkback = AxumData.Monitor[MonitorBussNr].Talkback[TalkbackNr];
                  for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
                  {
                    OldTalkbackActive |= AxumData.Monitor[MonitorBussNr].Talkback[cntTalkback];
                  }

                  AxumData.Monitor[MonitorBussNr].Talkback[TalkbackNr] = data.State;

                  if (AxumData.Monitor[MonitorBussNr].Talkback[TalkbackNr] != OldTalkback)
                  {
                    //Check talkbacks, if dim is required
                    int NewTalkbackActive = 0;
                    for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
                    {
                      NewTalkbackActive |= AxumData.Monitor[MonitorBussNr].Talkback[cntTalkback];
                    }

                    unsigned int FunctionNrToSend = 0x02000000 | (MonitorBussNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    if (NewTalkbackActive != OldTalkbackActive)
                    {
                      AxumData.Monitor[MonitorBussNr].Dim = NewTalkbackActive;
                      unsigned int FunctionNrToSent = 0x02000000 | (MonitorBussNr<<12);
                      CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_DIM));
                    }

                    for (int cntDestination=0; cntDestination<1280; cntDestination++)
                    {
                      if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
                      {
                        unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);

                        CheckObjectsToSent(DisplayFunctionNr | (DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1 + ((DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1)*TalkbackNr)));
                        CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
                      }
                    }
                  }
                }
              }
              break;
              case MONITOR_BUSS_FUNCTION_SELECT_1:
              case MONITOR_BUSS_FUNCTION_SELECT_2:
              case MONITOR_BUSS_FUNCTION_SELECT_3:
              case MONITOR_BUSS_FUNCTION_SELECT_4:
              {
                int ConsoleNr = FunctionNr-MONITOR_BUSS_FUNCTION_SELECT_1;
                unsigned int NewSelectedMonitorBussNr = AxumData.ConsoleData[ConsoleNr].SelectedMonitorBuss;

                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      NewSelectedMonitorBussNr = MonitorBussNr;
                    }
                  }
                  break;
                }

                SetSelectedMonitorBuss(ConsoleNr, NewSelectedMonitorBussNr);
              }
              break;
              default:
              { //not implemented function
              }
              break;
            }
          }
          break;
          case CONSOLE_FUNCTIONS:
          {
            unsigned int ConsoleNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            switch (FunctionNr)
            {
              case CONSOLE_FUNCTION_CONTROL_MODE_SOURCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_PROCESSING_PRESET:
              case CONSOLE_FUNCTION_CONTROL_MODE_SOURCE_GAIN:
              case CONSOLE_FUNCTION_CONTROL_MODE_PHANTOM_ON_OFF:
              case CONSOLE_FUNCTION_CONTROL_MODE_PAD_ON_OFF:
              case CONSOLE_FUNCTION_CONTROL_MODE_GAIN:
              case CONSOLE_FUNCTION_CONTROL_MODE_PHASE:
              case CONSOLE_FUNCTION_CONTROL_MODE_LOW_CUT:
              case CONSOLE_FUNCTION_CONTROL_MODE_INSERT_ON_OFF:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_TYPE:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_TYPE:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_TYPE:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_TYPE:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_TYPE:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_TYPE:
              case CONSOLE_FUNCTION_CONTROL_MODE_EQ_ON_OFF:
              case CONSOLE_FUNCTION_CONTROL_MODE_EXP_THRESHOLD:
              case CONSOLE_FUNCTION_CONTROL_MODE_AGC_THRESHOLD:
              case CONSOLE_FUNCTION_CONTROL_MODE_AGC:
              case CONSOLE_FUNCTION_CONTROL_MODE_DYNAMICS_ON_OFF:
              case CONSOLE_FUNCTION_CONTROL_MODE_MONO:
              case CONSOLE_FUNCTION_CONTROL_MODE_PAN:
              case CONSOLE_FUNCTION_CONTROL_MODE_MODULE_LEVEL:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30_BALANCE:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32:
              case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32_BALANCE:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  int ReceivedControlMode = -2;
                  int NewControlMode = -2;

                  AxumData.ConsoleData[ConsoleNr].ControlModeTimerValue = 0;

                  //Convert GLOBAL_FUNCTION_CONTROL_MODE to CONTROL_MODE number
                  ReceivedControlMode = GetControlModeFromConsoleFunctionNr(SensorReceiveFunctionNumber);

                  //Set the new control mode number depending on the state
                  if (data.State)
                  {
                    NewControlMode = ReceivedControlMode;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        if (AxumData.ConsoleData[ConsoleNr].ControlMode == ReceivedControlMode)
                        {
                          NewControlMode = AxumData.ConsoleData[ConsoleNr].ControlMode;
                        }
                      }
                    }
                  }

                  if (NewControlMode > -2)
                  {
                    if (AxumData.ConsoleData[ConsoleNr].ControlMode == NewControlMode)
                    {
                      AxumData.ConsoleData[ConsoleNr].ControlMode = -1;
                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    }
                    else
                    {
                      unsigned int OldFunctionNumber = GetConsoleFunctionNrFromControlMode(ConsoleNr);
                      AxumData.ConsoleData[ConsoleNr].ControlMode = NewControlMode;
                      CheckObjectsToSent(OldFunctionNumber);
                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    }

                    for (int cntModule=0; cntModule<128; cntModule++)
                    {
                      switch (NewControlMode)
                      {
                        case MODULE_CONTROL_MODE_SOURCE:
                        {
                          AxumData.ModuleData[cntModule].TemporySourceControlMode[ConsoleNr] = AxumData.ModuleData[cntModule].SelectedSource;
                        }
                        break;
                        case MODULE_CONTROL_MODE_MODULE_PRESET:
                        {
                          AxumData.ModuleData[cntModule].TemporyPresetControlMode[ConsoleNr] = AxumData.ModuleData[cntModule].SelectedProcessingPreset;
                        }
                        break;
                      }
                      unsigned int FunctionNrToSent = (cntModule<<12);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1_LABEL);
                    }
                  }

                  unsigned int FunctionNrToSent = 0x03000000 | (ConsoleNr<<12);
                  CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_CONTROL_MODE_ACTIVE);

                  //update Master & control mode
                  FunctionNrToSent = 0x03000000 | (ConsoleNr<<12);
                  for (int cntBuss=0; cntBuss<16; cntBuss++)
                  {
                    CheckObjectsToSent(FunctionNrToSent | (CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2+cntBuss));
                  }
                }
              }
              break;
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_3_4:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_5_6:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_7_8:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_9_10:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_11_12:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_13_14:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_15_16:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_17_18:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_19_20:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_21_22:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_23_24:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_25_26:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_27_28:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_29_30:
              case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_31_32:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    char NewMasterControlMode = FunctionNr-CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2;
                    unsigned int OldFunctionNumber = 0x00000000;
                    int OldBussNr = -1;
                    int NewBussNr = -1;

                    if (NewMasterControlMode<16)
                    {
                      NewBussNr = NewMasterControlMode;
                    }

                    if (AxumData.ConsoleData[ConsoleNr].MasterControlMode != MASTER_CONTROL_MODE_NONE)
                    {
                      OldFunctionNumber = 0x03000000 | (ConsoleNr<<12) | (AxumData.ConsoleData[ConsoleNr].MasterControlMode+CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2);
                      if (AxumData.ConsoleData[ConsoleNr].MasterControlMode < 16)
                      {
                        OldBussNr = AxumData.ConsoleData[ConsoleNr].MasterControlMode;
                      }
                    }

                    if (AxumData.ConsoleData[ConsoleNr].MasterControlMode != NewMasterControlMode)
                    {
                      AxumData.ConsoleData[ConsoleNr].MasterControlMode = NewMasterControlMode;
                    }
                    else
                    {
                      AxumData.ConsoleData[ConsoleNr].MasterControlMode = MASTER_CONTROL_MODE_NONE;
                    }
                    if (OldFunctionNumber != 0x00000000)
                    {
                      CheckObjectsToSent(OldFunctionNumber);
                    }
                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);

                    unsigned int FunctionNrToSent = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MASTER_CONTROL);
                    if (OldBussNr != -1)
                    {
                      CheckObjectsToSent(FunctionNrToSent | (CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2+OldBussNr));
                    }
                    if (NewBussNr != -1)
                    {
                      CheckObjectsToSent(FunctionNrToSent | (CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2+NewBussNr));
                    }
                  }
                }
              }
              break;
              case CONSOLE_FUNCTION_MASTER_CONTROL:
              {
                MasterModeControllerSensorChange(ConsoleNr, type, data, DataType, DataSize, DataMinimal, DataMaximal);
              }
              break;
              case CONSOLE_FUNCTION_MASTER_CONTROL_RESET:
              {
                MasterModeControllerResetSensorChange(ConsoleNr, type, data, DataType, DataSize, DataMinimal, DataMaximal);
              }
              break;
              case CONSOLE_FUNCTION_CONSOLE_TO_PROGRAMMED_DEFAULTS:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  ProgrammedDefaultSwitch[ConsoleNr].State = data.State;
                }
              }
              break;
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_3_4:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_5_6:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_7_8:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_9_10:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_11_12:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_13_14:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_15_16:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_17_18:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_19_20:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_21_22:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_23_24:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_25_26:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_27_28:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_29_30:
              case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_31_32:
              {
                int BussNr = FunctionNr-CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2;

                if (type == MBN_DATATYPE_STATE)
                {
                  if (data.State)
                  {
                    //First set master control mode
                    char NewMasterControlMode = BussNr;
                    unsigned int OldFunctionNumber = 0x00000000;
                    unsigned int OldConsoleFunctionNr = 0x00000000;
                    bool TurnOff = false;
                    int NewControlMode = MODULE_CONTROL_MODE_BUSS_1_2+(BussNr*(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2));

                    AxumData.ConsoleData[ConsoleNr].ControlModeTimerValue = 0;

                    if (AxumData.ConsoleData[ConsoleNr].MasterControlMode == NewMasterControlMode)
                    {
                      TurnOff = true;
                    }

                    if (AxumData.ConsoleData[ConsoleNr].MasterControlMode != MASTER_CONTROL_MODE_NONE)
                    {
                      OldFunctionNumber = 0x03000000 | (ConsoleNr<<12) | (AxumData.ConsoleData[ConsoleNr].MasterControlMode+CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2);
                    }

                    for (int cntBuss=0; cntBuss<16; cntBuss++)
                    {
                      char MasterControlModeToCheck = cntBuss;

                      if (AxumData.ConsoleData[ConsoleNr].MasterControlMode == MasterControlModeToCheck)
                      {
                        OldConsoleFunctionNr  = CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2+cntBuss;
                      }
                    }

                    if (TurnOff)
                    {
                      AxumData.ConsoleData[ConsoleNr].MasterControlMode = MASTER_CONTROL_MODE_NONE;
                    }
                    else
                    {
                      AxumData.ConsoleData[ConsoleNr].MasterControlMode = NewMasterControlMode;
                    }
                    if (OldFunctionNumber != 0x00000000)
                    {
                      CheckObjectsToSent(OldFunctionNumber);
                    }
                    int NewFunctionNr = AxumData.ConsoleData[ConsoleNr].MasterControlMode+CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2;
                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | NewFunctionNr);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_MASTER_CONTROL);

                    //Set control mode
                    if (TurnOff)
                    {
                      int ModuleControlModeToCheck = MODULE_CONTROL_MODE_BUSS_1_2+(BussNr*(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2));
                      if (AxumData.ConsoleData[ConsoleNr].ControlMode == ModuleControlModeToCheck)
                      {
                        unsigned int OldFunctionNr = GetConsoleFunctionNrFromControlMode(ConsoleNr);
                        AxumData.ConsoleData[ConsoleNr].ControlMode = -1;
                        CheckObjectsToSent(OldFunctionNr);
                      }
                    }
                    else
                    {
                      unsigned int OldFunctionNr = GetConsoleFunctionNrFromControlMode(ConsoleNr);
                      AxumData.ConsoleData[ConsoleNr].ControlMode = NewControlMode;
                      unsigned int NewFunctionNr = GetConsoleFunctionNrFromControlMode(ConsoleNr);
                      CheckObjectsToSent(OldFunctionNr);
                      CheckObjectsToSent(NewFunctionNr);
                    }

                    for (int cntModule=0; cntModule<128; cntModule++)
                    {
                      switch (NewControlMode)
                      {
                        case MODULE_CONTROL_MODE_SOURCE:
                        {
                          AxumData.ModuleData[cntModule].TemporySourceControlMode[ConsoleNr] = AxumData.ModuleData[cntModule].SelectedSource;
                        }
                        break;
                        case MODULE_CONTROL_MODE_MODULE_PRESET:
                        {
                          AxumData.ModuleData[cntModule].TemporyPresetControlMode[ConsoleNr] = AxumData.ModuleData[cntModule].SelectedProcessingPreset;
                        }
                        break;
                      }
                      unsigned int FunctionNrToSent = (cntModule<<12);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
                      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
                      switch (ConsoleNr)
                      {
                        case 0:
                        {
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1_LABEL);
                        }
                        break;
                        case 1:
                        {
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2_LABEL);
                        }
                        break;
                        case 2:
                        {
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3_LABEL);
                        }
                        break;
                        case 3:
                        {
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4_LABEL);
                        }
                        break;
                      }
                    }

                    unsigned int FunctionNrToSent = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | FunctionNr);

                    if (OldConsoleFunctionNr)
                    {
                      CheckObjectsToSent(FunctionNrToSent | OldConsoleFunctionNr);
                    }
                  }
                }
              }
              break;
//              case CONSOLE_FUNCTION_CONSOLE_PRESET:
              case CONSOLE_FUNCTION_MODULE_SELECT:
              {
                unsigned int NewSelectedModuleNr = AxumData.ConsoleData[ConsoleNr].SelectedModule;

                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<128)
                    {
                      NewSelectedModuleNr = data.UInt;
                    }
                  }
                  break;
                  case MBN_DATATYPE_SINT:
                  {
                    NewSelectedModuleNr += data.SInt;
                    NewSelectedModuleNr %= 128;
                  }
                  break;
                }

                SetSelectedModule(ConsoleNr, NewSelectedModuleNr);
                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);
              }
              break;
              case CONSOLE_FUNCTION_BUSS_SELECT:
              {
                unsigned int NewSelectedBussNr = AxumData.ConsoleData[ConsoleNr].SelectedBuss;

                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<128)
                    {
                      NewSelectedBussNr = data.UInt;
                    }
                  }
                  break;
                  case MBN_DATATYPE_SINT:
                  {
                    NewSelectedBussNr += data.SInt;
                    NewSelectedBussNr %= 128;
                  }
                  break;
                }

                SetSelectedBuss(ConsoleNr, NewSelectedBussNr);
                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);
              }
              break;
              case CONSOLE_FUNCTION_MONITOR_BUSS_SELECT:
              {
                unsigned int NewSelectedMonitorBussNr = AxumData.ConsoleData[ConsoleNr].SelectedMonitorBuss;

                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<128)
                    {
                      NewSelectedMonitorBussNr = data.UInt;
                    }
                  }
                  break;
                  case MBN_DATATYPE_SINT:
                  {
                    NewSelectedMonitorBussNr += data.SInt;
                    NewSelectedMonitorBussNr %= 128;
                  }
                  break;
                }

                SetSelectedMonitorBuss(ConsoleNr, NewSelectedMonitorBussNr);
                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);
              }
              break;
              case CONSOLE_FUNCTION_SOURCE_SELECT:
              {
                unsigned int NewSelectedSourceNr = AxumData.ConsoleData[ConsoleNr].SelectedSource;

                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<128)
                    {
                      NewSelectedSourceNr = data.UInt;
                    }
                  }
                  break;
                  case MBN_DATATYPE_SINT:
                  {
                    NewSelectedSourceNr += data.SInt;
                    NewSelectedSourceNr %= 128;
                  }
                  break;
                }

                SetSelectedSource(ConsoleNr, NewSelectedSourceNr);
                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);
              }
              break;
              case CONSOLE_FUNCTION_DESTINATION_SELECT:
              {
                unsigned int NewSelectedDestinationNr = AxumData.ConsoleData[ConsoleNr].SelectedDestination;

                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<128)
                    {
                      NewSelectedDestinationNr = data.UInt;
                    }
                  }
                  break;
                  case MBN_DATATYPE_SINT:
                  {
                    NewSelectedDestinationNr += data.SInt;
                    NewSelectedDestinationNr %= 128;
                  }
                  break;
                }

                SetSelectedDestination(ConsoleNr, NewSelectedDestinationNr);
                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | FunctionNr);
              }
              break;
              case CONSOLE_FUNCTION_CHIPCARD_CHANGE:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      OnlineNodeInformationElement->Account.UsernameReceived = 0;
                      OnlineNodeInformationElement->Account.PasswordReceived = 0;

                      for (int cntObject=1024; cntObject<OnlineNodeInformationElement->UsedNumberOfCustomObjects+1024; cntObject++)
                      {
                        SENSOR_RECEIVE_FUNCTION_STRUCT *ConnectedEngineFunction = &OnlineNodeInformationElement->SensorReceiveFunction[cntObject-1024];

                        if ((int)ConnectedEngineFunction->FunctionNr != -1)
                        {
                          unsigned int EngineFunctionType = (ConnectedEngineFunction->FunctionNr>>24)&0xFF;
                          unsigned int EngineFunctionSeqNr = (ConnectedEngineFunction->FunctionNr>>12)&0xFFF;
                          unsigned int EngineFunctionNr = ConnectedEngineFunction->FunctionNr&0xFFF;

                          if (EngineFunctionSeqNr == ConsoleNr)
                          {
                            if (EngineFunctionType == CONSOLE_FUNCTIONS)
                            {
                              if (EngineFunctionNr==((unsigned int)CONSOLE_FUNCTION_CHIPCARD_USER))
                              {
                                mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, cntObject, 1);
                              }
                              else if (EngineFunctionNr==((unsigned int)CONSOLE_FUNCTION_CHIPCARD_PASS))
                              {
                                mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, cntObject, 1);
                              }
                            }
                          }
                        }
                      }
                    }
                    else
                    { //Send user/pass idle
                      if (AxumData.ConsoleData[ConsoleNr].LogoutToIdle)
                      {
                        if ((strncmp(AxumData.ConsoleData[ConsoleNr].ActiveUsername, OnlineNodeInformationElement->Account.Username, 32) == 0) &&
                            (strncmp(AxumData.ConsoleData[ConsoleNr].ActivePassword, OnlineNodeInformationElement->Account.Password, 32) == 0))
                        {
                          unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                          memset(AxumData.ConsoleData[ConsoleNr].Username, 0, 32);
                          memset(AxumData.ConsoleData[ConsoleNr].Password, 0, 16);
                          memset(AxumData.ConsoleData[ConsoleNr].ActiveUsername, 0, 32);
                          memset(AxumData.ConsoleData[ConsoleNr].ActivePassword, 0, 16);
                          //Idle, source/preset pool A
                          AxumData.ConsoleData[ConsoleNr].UserLevel = 0;
                          AxumData.ConsoleData[ConsoleNr].SourcePool = 0;
                          AxumData.ConsoleData[ConsoleNr].PresetPool = 0;
                          AxumData.ConsoleData[ConsoleNr].LogoutToIdle = 0;
                          AxumData.ConsoleData[ConsoleNr].ConsolePreset = 0;

                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_USER_PASS));
                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_USER));
                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_PASS));
                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_USER_LEVEL));


                          db_lock(1);
                          db_update_account(ConsoleNr, AxumData.ConsoleData[ConsoleNr].ActiveUsername, AxumData.ConsoleData[ConsoleNr].ActivePassword);
                          db_lock(0);
                        }
                      }
                      OnlineNodeInformationElement->Account.UsernameReceived = 0;
                      OnlineNodeInformationElement->Account.PasswordReceived = 0;
                      memset(OnlineNodeInformationElement->Account.Username, 0, 32);
                      memset(OnlineNodeInformationElement->Account.Password, 0, 16);

                      db_lock(1);
                      db_update_chipcard_account(ConsoleNr, OnlineNodeInformationElement->Account.Username, OnlineNodeInformationElement->Account.Password);
                      db_lock(0);
                    }
                  }
                }
              }
              break;
              case CONSOLE_FUNCTION_CHIPCARD_USER:
              { //Maybe not allowed, only atomic sensor changes on user/pass?
                switch (type)
                {
                  case MBN_DATATYPE_OCTETS:
                  {
                    OnlineNodeInformationElement->Account.UsernameReceived = 1;
                    strncpy(OnlineNodeInformationElement->Account.Username, (char *)data.Octets, 32);
                    strncpy(AxumData.ConsoleData[ConsoleNr].Username, (char *)data.Octets, 32);
                    if (OnlineNodeInformationElement->Account.PasswordReceived)
                    {
                      strncpy(AxumData.ConsoleData[ConsoleNr].Password, OnlineNodeInformationElement->Account.Password, 16);
                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_USER_PASS);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_USER_LEVEL);

                      db_lock(1);
                      db_read_user(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                      db_update_chipcard_account(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                      db_lock(0);
                    }

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_USER);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_CHIPCARD_PASS:
              { //Maybe not allowed, only atomic sensor changes on user/pass?
                switch (type)
                {
                  case MBN_DATATYPE_OCTETS:
                  {
                    OnlineNodeInformationElement->Account.PasswordReceived = 1;
                    strncpy(OnlineNodeInformationElement->Account.Password, (char *)data.Octets, 16);
                    strncpy(AxumData.ConsoleData[ConsoleNr].Password, (char *)data.Octets, 16);
                    if (OnlineNodeInformationElement->Account.UsernameReceived)
                    {
                      strncpy(AxumData.ConsoleData[ConsoleNr].Username, OnlineNodeInformationElement->Account.Username, 32);
                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_USER_PASS);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_USER_LEVEL);

                      db_lock(1);
                      db_read_user(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                      db_update_chipcard_account(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                      db_lock(0);
                    }

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_PASS);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_WRITE_CHIPCARD_USER_PASS:
              {
                switch (type)
                {
                  case MBN_DATATYPE_OCTETS:
                  {
                    char *DataString = (char *)data.Octets;
                    strncpy(AxumData.ConsoleData[ConsoleNr].UsernameToWrite, DataString, 32);
                    strncpy(AxumData.ConsoleData[ConsoleNr].PasswordToWrite, &DataString[32], 16);

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_CHIPCARD_USER);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_CHIPCARD_PASS);
                  }
                  break;
                }
              }
              break;
//              case CONSOLE_FUNCTION_UPDATE_USER: //Only atomic update allowed
//              case CONSOLE_FUNCTION_UPDATE_PASS: //Only atomic update allowed
              case CONSOLE_FUNCTION_UPDATE_USER_PASS:
              {
                switch (type)
                {
                  case MBN_DATATYPE_OCTETS:
                  {
                    char *data_str = (char *)data.Octets;

                    OnlineNodeInformationElement->Account.UsernameReceived = 1;
                    OnlineNodeInformationElement->Account.PasswordReceived = 1;
                    strncpy(OnlineNodeInformationElement->Account.Username, data_str, 32);
                    strncpy(OnlineNodeInformationElement->Account.Password, &(data_str[32]), 16);
                    strncpy(AxumData.ConsoleData[ConsoleNr].Username, data_str, 32);
                    strncpy(AxumData.ConsoleData[ConsoleNr].Password, &(data_str[32]), 16);

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_USER_PASS));
                    CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_USER));
                    CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_PASS));
                    CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_USER_LEVEL));

                    db_read_user(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_USER_LEVEL:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    AxumData.ConsoleData[ConsoleNr].UserLevel = data.State;

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_USER_LEVEL);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_DOT_COUNT_UPDOWN:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      AxumData.ConsoleData[ConsoleNr].DotCountUpDown = !AxumData.ConsoleData[ConsoleNr].DotCountUpDown;

                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_DOT_COUNT_UPDOWN);
                    }
                  }
                  break;
                  case MBN_DATATYPE_UINT:
                  {
                    AxumData.ConsoleData[ConsoleNr].DotCountUpDown = data.UInt;

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_DOT_COUNT_UPDOWN);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_PROGRAM_ENDTIME_ENABLE:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeEnable = !AxumData.ConsoleData[ConsoleNr].ProgramEndTimeEnable;

                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_ENABLE);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME);
                    }
                  }
                  break;
                  case MBN_DATATYPE_UINT:
                  {
                    AxumData.ConsoleData[ConsoleNr].ProgramEndTimeEnable = data.UInt;

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_ENABLE);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_PROGRAM_ENDTIME:
              {
                switch (type)
                {
                  case MBN_DATATYPE_OCTETS:
                  {
                    unsigned char Hours;
                    unsigned char Minutes;
                    unsigned char Seconds;

                    if (sscanf((char *)data.Octets, "%02hhd:%02hhd:%02hhd", &Hours, &Minutes, &Seconds) == 2)
                    {
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeHours = Hours;
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeMinutes = Minutes;
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeSeconds = Seconds;

                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_HOURS);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_MINUTES);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_SECONDS);
                    }
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_PROGRAM_ENDTIME_HOURS:
              {
                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<23)
                    {
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeHours = data.UInt;
                    }
                    else
                    {
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeHours = 99;
                    }

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_HOURS);
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_PROGRAM_ENDTIME_MINUTES:
              {
                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<60)
                    {
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeMinutes = data.UInt;

                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_MINUTES);
                    }
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_PROGRAM_ENDTIME_SECONDS:
              {
                switch (type)
                {
                  case MBN_DATATYPE_UINT:
                  {
                    if (data.UInt<60)
                    {
                      AxumData.ConsoleData[ConsoleNr].ProgramEndTimeSeconds = data.UInt;

                      unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME);
                      CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_PROGRAM_ENDTIME_SECONDS);
                    }
                  }
                  break;
                }
              }
              break;
              case CONSOLE_FUNCTION_COUNT_DOWN_TIMER:
              {
                switch (type)
                {
                  case MBN_DATATYPE_FLOAT:
                  {
                    float TimerValue = data.Float;

                    if (TimerValue<0)
                    {
                      TimerValue = 0;
                    }
                    else if (TimerValue>60)
                    {
                      TimerValue = 60;
                    }
                    AxumData.ConsoleData[ConsoleNr].CountDownTimer = TimerValue;

                    unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_COUNT_DOWN_TIMER);
                  }
                  break;
                }
              }
              break;
            }
          }
          break;
          case GLOBAL_FUNCTIONS:
          {   //Global
            unsigned int GlobalNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if (GlobalNr == 0)
            {
              switch (FunctionNr)
              {
                case GLOBAL_FUNCTION_REDLIGHT_1:
                case GLOBAL_FUNCTION_REDLIGHT_2:
                case GLOBAL_FUNCTION_REDLIGHT_3:
                case GLOBAL_FUNCTION_REDLIGHT_4:
                case GLOBAL_FUNCTION_REDLIGHT_5:
                case GLOBAL_FUNCTION_REDLIGHT_6:
                case GLOBAL_FUNCTION_REDLIGHT_7:
                case GLOBAL_FUNCTION_REDLIGHT_8:
                {
                  int RedlightNr = FunctionNr-GLOBAL_FUNCTION_REDLIGHT_1;
                  bool OldState = AxumData.Redlight[RedlightNr];

                  if (type == MBN_DATATYPE_STATE)
                  {
                    if (data.State)
                    {
                      AxumData.Redlight[RedlightNr] = !AxumData.Redlight[RedlightNr];
                    }
                    else
                    {
                      int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                      if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                      {
                        if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                        {
                          AxumData.Redlight[RedlightNr] = !AxumData.Redlight[RedlightNr];
                        }
                      }
                    }

                    if (AxumData.Redlight[RedlightNr] != OldState)
                    {
                      unsigned int FunctionNrToSend = 0x04000000;
                      CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    }
                  }
                }
                break;
                case GLOBAL_FUNCTION_CONSOLE_PRESET_1:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_2:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_3:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_4:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_5:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_6:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_7:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_8:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_9:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_10:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_11:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_12:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_13:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_14:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_15:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_16:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_17:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_18:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_19:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_20:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_21:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_22:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_23:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_24:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_25:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_26:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_27:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_28:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_29:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_30:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_31:
                case GLOBAL_FUNCTION_CONSOLE_PRESET_32:
                {
                  int PresetNr = FunctionNr-GLOBAL_FUNCTION_CONSOLE_PRESET_1;

                  if (type == MBN_DATATYPE_STATE)
                  {
                    ConsolePresetSwitch[PresetNr].State = data.State;
                  }
                }
                break;
              }
            }
          }
          break;
          case SOURCE_FUNCTIONS:
          { //Source
            int SourceNr = ((SensorReceiveFunctionNumber>>12)&0xFFF);
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((SourceNr>=NUMBER_OF_SOURCES) && (SourceNr<(NUMBER_OF_SOURCES+4)))
            {
              SourceNr = AxumData.ConsoleData[SourceNr-NUMBER_OF_SOURCES].SelectedSource;
            }

            switch (FunctionNr)
            {
              case SOURCE_FUNCTION_MODULE_ON:
              case SOURCE_FUNCTION_MODULE_OFF:
              case SOURCE_FUNCTION_MODULE_ON_OFF:
              {   //Module on
                //Module off
                //Module on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  for (int cntModule=0; cntModule<128; cntModule++)
                  {
                    if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                    {
                      int CurrentOn = AxumData.ModuleData[cntModule].On;

                      if (data.State)
                      {
                        switch (FunctionNr)
                        {
                          case SOURCE_FUNCTION_MODULE_ON:
                          {
                            AxumData.ModuleData[cntModule].On = 1;
                          }
                          break;
                          case SOURCE_FUNCTION_MODULE_OFF:
                          {
                            AxumData.ModuleData[cntModule].On = 0;
                          }
                          break;
                          case SOURCE_FUNCTION_MODULE_ON_OFF:
                          {
                            AxumData.ModuleData[cntModule].On = !AxumData.ModuleData[cntModule].On;
                          }
                          break;
                        }
                      }
                      else
                      {
                        int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                        if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                        {
                          if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                          {
                            switch (FunctionNr)
                            {
                              case SOURCE_FUNCTION_MODULE_ON_OFF:
                              {
                                AxumData.ModuleData[cntModule].On = !AxumData.ModuleData[cntModule].On;
                              }
                              break;
                            }
                          }
                        }
                      }
                      int NewOn = AxumData.ModuleData[cntModule].On;

                      if (NewOn != CurrentOn)
                      {
                        SetAxum_BussLevels(cntModule);

                        unsigned int FunctionNrToSend = ((cntModule)<<12);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_OFF);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON_OFF);

                        DoAxum_ModuleStatusChanged(cntModule, 1);

                        FunctionNrToSend = ((cntModule)<<12);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);

                        FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_OFF);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON_OFF);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);

                        float FaderLevel = AxumData.ModuleData[cntModule].FaderLevel;
                        DoAxum_StartStopTrigger(cntModule, FaderLevel, FaderLevel, CurrentOn, NewOn);
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_MODULE_FADER_ON:
              case SOURCE_FUNCTION_MODULE_FADER_OFF:
              case SOURCE_FUNCTION_MODULE_FADER_ON_OFF:
              { //fader on
                //fader off
                //fader on/off
                if (type == MBN_DATATYPE_STATE)
                {
                  for (int cntModule=0; cntModule<128; cntModule++)
                  {
                    if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                    {
                      float CurrentLevel = AxumData.ModuleData[cntModule].FaderLevel;
                      if (data.State)
                      {
                        switch (FunctionNr)
                        {
                          case SOURCE_FUNCTION_MODULE_FADER_ON:
                          {
                            AxumData.ModuleData[cntModule].FaderLevel = 0;
                          }
                          break;
                          case SOURCE_FUNCTION_MODULE_FADER_OFF:
                          {
                            AxumData.ModuleData[cntModule].FaderLevel = -140;
                          }
                          break;
                          case SOURCE_FUNCTION_MODULE_FADER_ON_OFF:
                          {
                            if (AxumData.ModuleData[cntModule].FaderLevel>-80)
                            {
                              AxumData.ModuleData[cntModule].FaderLevel = -140;
                            }
                            else
                            {
                              AxumData.ModuleData[cntModule].FaderLevel = 0;
                            }
                          }
                          break;
                        }
                      }
                      else
                      {
                        int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                        if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                        {
                          if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                          {
                            switch (FunctionNr)
                            {
                              case SOURCE_FUNCTION_MODULE_FADER_ON_OFF:
                              {
                                if (AxumData.ModuleData[cntModule].FaderLevel>-80)
                                {
                                  AxumData.ModuleData[cntModule].FaderLevel = -140;
                                }
                                else
                                {
                                  AxumData.ModuleData[cntModule].FaderLevel = 0;
                                }
                              }
                              break;
                            }
                          }
                        }
                      }
                      float NewLevel = AxumData.ModuleData[cntModule].FaderLevel;

                      if (NewLevel != CurrentLevel)
                      {
                        SetAxum_BussLevels(cntModule);

                        unsigned int FunctionNrToSend = ((cntModule)<<12);
                        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_LEVEL);

                        if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                            ((CurrentLevel>-80) && (NewLevel<=-80)))
                        { //fader on changed
                          DoAxum_ModuleStatusChanged(cntModule, 1);

                          FunctionNrToSend = ((cntModule)<<12);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
                          CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_ON);
                          CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_OFF);
                          CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_ON_OFF);

                          FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_OFF);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);

                          unsigned char On = AxumData.ModuleData[cntModule].On;
                          DoAxum_StartStopTrigger(cntModule, CurrentLevel, NewLevel, On, On);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE:
              case SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE:
              case SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE:
              {   //fader on and on active
                //fader on and on inactive
                if (type == MBN_DATATYPE_STATE)
                {
                  for (int cntModule=0; cntModule<128; cntModule++)
                  {
                    if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                    {
                      float CurrentLevel = AxumData.ModuleData[cntModule].FaderLevel;
                      float CurrentOn = AxumData.ModuleData[cntModule].On;
                      if (data.State)
                      {
                        switch (FunctionNr)
                        {
                          case SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE:
                          {
                            AxumData.ModuleData[cntModule].FaderLevel = 0;
                            AxumData.ModuleData[cntModule].On = 1;
                          }
                          break;
                          case SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE:
                          {
                            AxumData.ModuleData[cntModule].FaderLevel = -140;
                            AxumData.ModuleData[cntModule].On = 0;
                          }
                          break;
                          case SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE:
                          {
                            if ((AxumData.ModuleData[cntModule].FaderLevel >= -80) && (AxumData.ModuleData[cntModule].On))
                            {
                              AxumData.ModuleData[cntModule].FaderLevel = -140;
                              AxumData.ModuleData[cntModule].On = 0;
                            }
                            else
                            {
                              AxumData.ModuleData[cntModule].FaderLevel = 0;
                              AxumData.ModuleData[cntModule].On = 1;
                            }
                          }
                          break;
                        }
                      }
                      else
                      {
                        int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                        if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                        {
                          if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                          {
                          }
                        }
                      }
                      float NewLevel = AxumData.ModuleData[cntModule].FaderLevel;
                      int NewOn = AxumData.ModuleData[cntModule].On;

                      if ((NewOn != CurrentOn)  ||
                          (NewLevel != CurrentLevel))
                      {
                        SetAxum_BussLevels(cntModule);

                        unsigned int FunctionNrToSend = ((cntModule)<<12);
                        if (NewOn != CurrentOn)
                        {
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_OFF);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_ON_OFF);
                        }
                        if (NewLevel != CurrentLevel)
                        {
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_MODULE_LEVEL);
                        }

                        if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                            ((CurrentLevel>-80) && (NewLevel<=-80)) ||
                            (CurrentOn != NewOn))
                        { //fader on changed
                          DoAxum_ModuleStatusChanged(cntModule, 1);

                          FunctionNrToSend = ((cntModule)<<12);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
                          if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                              ((CurrentLevel>-80) && (NewLevel<=-80)))
                          {
                            CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_ON);
                            CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_OFF);
                            CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_FADER_ON_OFF);
                          }

                          FunctionNrToSend = 0x05000000 | (SourceNr<<12);
                          if (CurrentOn != NewOn)
                          {
                            CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON);
                            CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_OFF);
                            CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_ON_OFF);
                          }
                          if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                              ((CurrentLevel>-80) && (NewLevel<=-80)))
                          {
                            CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON);
                            CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_OFF);
                            CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
                          }
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                          CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);

                          DoAxum_StartStopTrigger(cntModule, CurrentLevel, NewLevel, CurrentOn, NewOn);
                        }
                      }
                    }
                  }
                }
              }
              break;
              break;
              case SOURCE_FUNCTION_MODULE_BUSS_1_2_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_3_4_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_5_6_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_7_8_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_9_10_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_11_12_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_13_14_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_15_16_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_17_18_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_19_20_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_21_22_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_23_24_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_25_26_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_27_28_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_29_30_ON:
              case SOURCE_FUNCTION_MODULE_BUSS_31_32_ON:
              {  //Buss 1/2 on
                int BussNr = (FunctionNr-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON)/(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON);

                for (int cntModule=0; cntModule<128; cntModule++)
                {
                  if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                  {
                    unsigned char NewState = 1;

                    DoAxum_SetBussOnOff(cntModule, BussNr, NewState, 0);
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_5_6_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_7_8_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_9_10_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_11_12_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_13_14_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_15_16_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_17_18_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_19_20_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_21_22_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_23_24_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_25_26_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_27_28_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_29_30_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_31_32_OFF:
              {  //Buss 1/2 off
                int BussNr = (FunctionNr-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF)/(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF);

                for (int cntModule=0; cntModule<128; cntModule++)
                {
                  if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                  {
                    unsigned char NewState = 0;
                    DoAxum_SetBussOnOff(cntModule, BussNr, NewState, 0);
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_5_6_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_7_8_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_9_10_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_11_12_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_13_14_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_15_16_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_17_18_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_19_20_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_21_22_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_23_24_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_25_26_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_27_28_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_29_30_ON_OFF:
              case SOURCE_FUNCTION_MODULE_BUSS_31_32_ON_OFF:
              {  //Buss 1/2 on/off
                int BussNr = (FunctionNr-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF)/(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF);
                if (type == MBN_DATATYPE_STATE)
                {
                  for (int cntModule=0; cntModule<128; cntModule++)
                  {
                    unsigned char NewState = AxumData.ModuleData[cntModule].Buss[BussNr].On;
                    if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                    {
                      if (data.State)
                      {
                        NewState = !AxumData.ModuleData[cntModule].Buss[BussNr].On;
                      }
                      else
                      {
                        int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                        if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                        {
                          if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                          {
                            NewState = !AxumData.ModuleData[cntModule].Buss[BussNr].On;
                          }
                        }
                      }
                      DoAxum_SetBussOnOff(cntModule, BussNr, NewState, 0);
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_MODULE_COUGH_ON_OFF:
              {  //Cough
                if (type == MBN_DATATYPE_STATE)
                {
                  DoAxum_SetCough(SourceNr, data.State);
                }
              }
              break;
              case SOURCE_FUNCTION_START:
              case SOURCE_FUNCTION_STOP:
              case SOURCE_FUNCTION_START_STOP:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldState = AxumData.SourceData[SourceNr].Start;

                  if (data.State)
                  {
                    switch (FunctionNr)
                    {
                      case SOURCE_FUNCTION_START:
                      {
                        AxumData.SourceData[SourceNr].Start = 1;
                      }
                      break;
                      case SOURCE_FUNCTION_STOP:
                      {
                        AxumData.SourceData[SourceNr].Start = 0;
                      }
                      break;
                      case SOURCE_FUNCTION_START_STOP:
                      {
                        AxumData.SourceData[SourceNr].Start = !AxumData.SourceData[SourceNr].Start;
                      }
                      break;
                    }
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        switch (FunctionNr)
                        {
                          case SOURCE_FUNCTION_START_STOP:
                          {
                            AxumData.SourceData[SourceNr].Start = !AxumData.SourceData[SourceNr].Start;
                          }
                          break;
                        }
                      }
                    }
                  }


                  if (AxumData.SourceData[SourceNr].Start != OldState)
                  { //only if pressed or changed
                    unsigned int DisplayFunctionNr = 0x05000000 | (SourceNr<<12);
                    CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_START);
                    CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_STOP);
                    CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_START_STOP);

                    for (int cntModule=0; cntModule<128; cntModule++)
                    {
                      if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                      {
                        DisplayFunctionNr = (cntModule<<12);
                        CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_START);
                        CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_STOP);
                        CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_START_STOP);
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_PHANTOM:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                  if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM))
                  {
                    bool OldState = AxumData.SourceData[SourceNr].Phantom;
                    if (data.State)
                    {
                      AxumData.SourceData[SourceNr].Phantom = !AxumData.SourceData[SourceNr].Phantom;
                    }
                    else
                    {
                      int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                      if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                      {
                        if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                        {
                          AxumData.SourceData[SourceNr].Phantom = !AxumData.SourceData[SourceNr].Phantom;
                        }
                      }
                    }

                    if (AxumData.SourceData[SourceNr].Phantom != OldState)
                    {
                      CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                        {
                          FunctionNrToSent = (cntModule<<12);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_PHANTOM);
                          DoAxum_UpdateModuleControlMode(cntModule, MODULE_CONTROL_MODE_PHANTOM_ON_OFF);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_PAD:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                  if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PAD))
                  {
                    bool OldState = AxumData.SourceData[SourceNr].Pad;
                    if (data.State)
                    {
                      AxumData.SourceData[SourceNr].Pad = !AxumData.SourceData[SourceNr].Pad;
                    }
                    else
                    {
                      int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                      if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                      {
                        if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                        {
                          AxumData.SourceData[SourceNr].Pad = !AxumData.SourceData[SourceNr].Pad;
                        }
                      }
                    }

                    if (AxumData.SourceData[SourceNr].Pad != OldState)
                    {
                      unsigned int DisplayFunctionNr = 0x05000000 | (SourceNr<<12) | SOURCE_FUNCTION_PAD;
                      CheckObjectsToSent(DisplayFunctionNr);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                        {
                          DisplayFunctionNr = (cntModule<<12) | MODULE_FUNCTION_SOURCE_PAD;
                          CheckObjectsToSent(DisplayFunctionNr);
                          DoAxum_UpdateModuleControlMode(cntModule, MODULE_CONTROL_MODE_PAD_ON_OFF);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_GAIN:
              {
                unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
                {
                  float min, max, def;
                  CheckObjectRange(FunctionNrToSent | SOURCE_FUNCTION_GAIN, &min, &max, &def);


                  if (type == MBN_DATATYPE_SINT)
                  {
                    float OldLevel = AxumData.SourceData[SourceNr].Gain;

                    AxumData.SourceData[SourceNr].Gain += (float)data.SInt/10;
                    if (AxumData.SourceData[SourceNr].Gain < min)
                    {
                      AxumData.SourceData[SourceNr].Gain = min;
                    }
                    else if (AxumData.SourceData[SourceNr].Gain > max)
                    {
                      AxumData.SourceData[SourceNr].Gain = max;
                    }

                    if (AxumData.SourceData[SourceNr].Gain != OldLevel)
                    {
                      unsigned int DisplayFunctionNr = 0x05000000 | ((SourceNr)<<12) | SOURCE_FUNCTION_GAIN;
                      CheckObjectsToSent(DisplayFunctionNr);

                      for (int cntModule=0; cntModule<128; cntModule++)
                      {
                        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                        {
                          unsigned int FunctionNrToSent = (cntModule<<12);
                          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_GAIN_LEVEL);

                          DoAxum_UpdateModuleControlMode(cntModule, MODULE_CONTROL_MODE_SOURCE_GAIN);
                        }
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_ALERT:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldState = AxumData.SourceData[SourceNr].Alert;

                  AxumData.SourceData[SourceNr].Alert = data.State;

                  if (AxumData.SourceData[SourceNr].Alert != OldState)
                  {
                    unsigned int DisplayFunctionNr = 0x05000000 | (SourceNr<<12) | SOURCE_FUNCTION_ALERT;
                    CheckObjectsToSent(DisplayFunctionNr);

                    for (int cntModule=0; cntModule<128; cntModule++)
                    {
                      if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                      {
                        DisplayFunctionNr = (cntModule<<12) | MODULE_FUNCTION_SOURCE_ALERT;
                        CheckObjectsToSent(DisplayFunctionNr);
                      }
                    }
                  }
                }
              }
              break;
              case SOURCE_FUNCTION_SELECT_1:
              case SOURCE_FUNCTION_SELECT_2:
              case SOURCE_FUNCTION_SELECT_3:
              case SOURCE_FUNCTION_SELECT_4:
              {
                int SelectNr = FunctionNr-SOURCE_FUNCTION_SELECT_1;
                unsigned int NewSelectedSourceNr = AxumData.ConsoleData[SelectNr].SelectedSource;

                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      NewSelectedSourceNr = SourceNr;
                    }
                  }
                  break;
                }

                SetSelectedSource(SelectNr, NewSelectedSourceNr);
              }
              break;
              case SOURCE_FUNCTION_COUGH_COMM_1:
              case SOURCE_FUNCTION_COUGH_COMM_2:
              {
                unsigned char CommNr = FunctionNr-SOURCE_FUNCTION_COUGH_COMM_1;

                DoAxum_SetCough(SourceNr, data.State);
                DoAxum_SetComm(SourceNr, CommNr, data.State);
              }
              break;
            }
          }
          break;
          case DESTINATION_FUNCTIONS:
          { //Destination
            unsigned int DestinationNr = ((SensorReceiveFunctionNumber>>12)&0xFFF);
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((DestinationNr>=NUMBER_OF_DESTINATIONS) && (DestinationNr<(NUMBER_OF_DESTINATIONS+4)))
            {
              DestinationNr = AxumData.ConsoleData[DestinationNr-NUMBER_OF_DESTINATIONS].SelectedBuss;
            }

            switch (FunctionNr)
            {
              case DESTINATION_FUNCTION_LABEL:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_SOURCE:
              {
                if (type == MBN_DATATYPE_SINT)
                {
                  unsigned char Pool = 8;
                  AxumData.DestinationData[DestinationNr].Source = (int)AdjustDestinationSource(AxumData.DestinationData[DestinationNr].Source, data.SInt, Pool);

                  SetAxum_DestinationSource(DestinationNr);

                  unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_MONITOR_SPEAKER_LEVEL);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_MONITOR_PHONES_LEVEL);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_MUTE_AND_MONITOR_MUTE);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_MONO_AND_MONITOR_MONO);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_PHASE_AND_MONITOR_PHASE);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_3_AND_MONITOR_TALKBACK_3);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_4_AND_MONITOR_TALKBACK_4);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_5_AND_MONITOR_TALKBACK_5);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_6_AND_MONITOR_TALKBACK_6);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_7_AND_MONITOR_TALKBACK_7);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_8_AND_MONITOR_TALKBACK_8);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_9_AND_MONITOR_TALKBACK_9);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_10_AND_MONITOR_TALKBACK_10);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_11_AND_MONITOR_TALKBACK_11);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_12_AND_MONITOR_TALKBACK_12);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_13_AND_MONITOR_TALKBACK_13);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_14_AND_MONITOR_TALKBACK_14);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_15_AND_MONITOR_TALKBACK_15);
                  CheckObjectsToSent(FunctionNrToSend+DESTINATION_FUNCTION_TALKBACK_16_AND_MONITOR_TALKBACK_16);
                }
              }
              break;
              case DESTINATION_FUNCTION_MONITOR_SPEAKER_LEVEL:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_MONITOR_PHONES_LEVEL:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_LEVEL:
              {
                float OldLevel = AxumData.DestinationData[DestinationNr].Level;
                if (type == MBN_DATATYPE_UINT)
                {
                  int Position = (data.UInt*1023)/(DataMaximal-DataMinimal);
                  AxumData.DestinationData[DestinationNr].Level = Position2dB[Position];
                }
                else if (type == MBN_DATATYPE_SINT)
                {
                  AxumData.DestinationData[DestinationNr].Level += (float)data.SInt/10;
                  if (AxumData.DestinationData[DestinationNr].Level<-140)
                  {
                    AxumData.DestinationData[DestinationNr].Level = -140;
                  }
                  else
                  {
                    if (AxumData.DestinationData[DestinationNr].Level > 10)
                    {
                      AxumData.DestinationData[DestinationNr].Level = 10;
                    }
                  }
                  unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                }
                else if (type == MBN_DATATYPE_FLOAT)
                {
                  AxumData.DestinationData[DestinationNr].Level = data.Float;
                  if (AxumData.DestinationData[DestinationNr].Level<-140)
                  {
                    AxumData.DestinationData[DestinationNr].Level = -140;
                  }
                  else
                  {
                    if (AxumData.DestinationData[DestinationNr].Level > 10)
                    {
                      AxumData.DestinationData[DestinationNr].Level = 10;
                    }
                  }
                }
                if (AxumData.DestinationData[DestinationNr].Level != OldLevel)
                {
                  unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                }
              }
              break;
              case DESTINATION_FUNCTION_MUTE:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldState = AxumData.DestinationData[DestinationNr].Mute;
                  if (data.State)
                  {
                    AxumData.DestinationData[DestinationNr].Mute = !AxumData.DestinationData[DestinationNr].Mute;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.DestinationData[DestinationNr].Mute = !AxumData.DestinationData[DestinationNr].Mute;
                      }
                    }
                  }

                  if (AxumData.DestinationData[DestinationNr].Mute != OldState)
                  {
                    unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_MUTE_AND_MONITOR_MUTE);
                  }
                }
              }
              break;
              case DESTINATION_FUNCTION_MUTE_AND_MONITOR_MUTE:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_DIM:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldState = AxumData.DestinationData[DestinationNr].Dim;
                  if (data.State)
                  {
                    AxumData.DestinationData[DestinationNr].Dim = !AxumData.DestinationData[DestinationNr].Dim;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.DestinationData[DestinationNr].Dim = !AxumData.DestinationData[DestinationNr].Dim;
                      }
                    }
                  }

                  if (AxumData.DestinationData[DestinationNr].Dim != OldState)
                  {
                    unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
                  }
                }
              }
              break;
              case DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_MONO:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldState = AxumData.DestinationData[DestinationNr].Mono;
                  if (data.State)
                  {
                    AxumData.DestinationData[DestinationNr].Mono = !AxumData.DestinationData[DestinationNr].Mono;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.DestinationData[DestinationNr].Mono = !AxumData.DestinationData[DestinationNr].Mono;
                      }
                    }
                  }

                  if (AxumData.DestinationData[DestinationNr].Mono != OldState)
                  {
                    unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_MONO_AND_MONITOR_MONO);
                  }
                }
              }
              break;
              case DESTINATION_FUNCTION_MONO_AND_MONITOR_MONO:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_PHASE:
              {
                if (type == MBN_DATATYPE_STATE)
                {
                  bool OldState = AxumData.DestinationData[DestinationNr].Phase;
                  if (data.State)
                  {
                    AxumData.DestinationData[DestinationNr].Phase = !AxumData.DestinationData[DestinationNr].Phase;
                  }
                  else
                  {
                    int delay_time = (SensorReceiveFunction->LastChangedTime-SensorReceiveFunction->PreviousLastChangedTime)*10;
                    if (SensorReceiveFunction->TimeBeforeMomentary>=0)
                    {
                      if (delay_time>=SensorReceiveFunction->TimeBeforeMomentary)
                      {
                        AxumData.DestinationData[DestinationNr].Phase = !AxumData.DestinationData[DestinationNr].Phase;
                      }
                    }
                  }

                  if (AxumData.DestinationData[DestinationNr].Phase != OldState)
                  {
                    unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_PHASE_AND_MONITOR_PHASE);
                  }
                }
              }
              break;
              case DESTINATION_FUNCTION_PHASE_AND_MONITOR_PHASE:
              { //No sensor change available
              }
              break;
              case DESTINATION_FUNCTION_TALKBACK_1:
              case DESTINATION_FUNCTION_TALKBACK_2:
              case DESTINATION_FUNCTION_TALKBACK_3:
              case DESTINATION_FUNCTION_TALKBACK_4:
              case DESTINATION_FUNCTION_TALKBACK_5:
              case DESTINATION_FUNCTION_TALKBACK_6:
              case DESTINATION_FUNCTION_TALKBACK_7:
              case DESTINATION_FUNCTION_TALKBACK_8:
              case DESTINATION_FUNCTION_TALKBACK_9:
              case DESTINATION_FUNCTION_TALKBACK_10:
              case DESTINATION_FUNCTION_TALKBACK_11:
              case DESTINATION_FUNCTION_TALKBACK_12:
              case DESTINATION_FUNCTION_TALKBACK_13:
              case DESTINATION_FUNCTION_TALKBACK_14:
              case DESTINATION_FUNCTION_TALKBACK_15:
              case DESTINATION_FUNCTION_TALKBACK_16:
              {
                int TalkbackNr = (FunctionNr-DESTINATION_FUNCTION_TALKBACK_1)/(DESTINATION_FUNCTION_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1);

                if (type == MBN_DATATYPE_STATE)
                {
                  int OldTalkbackActive = 0;
                  for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
                  {
                    OldTalkbackActive |= AxumData.DestinationData[DestinationNr].Talkback[cntTalkback];
                  }
                  bool OldState = AxumData.DestinationData[DestinationNr].Talkback[TalkbackNr];

                  AxumData.DestinationData[DestinationNr].Talkback[TalkbackNr] = data.State;

                  int NewTalkbackActive = 0;
                  for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
                  {
                    NewTalkbackActive |= AxumData.DestinationData[DestinationNr].Talkback[cntTalkback];
                  }

                  unsigned int FunctionNrToSend = 0x06000000 | (DestinationNr<<12);

                  if (AxumData.DestinationData[DestinationNr].Talkback[TalkbackNr] != OldState)
                  {
                    CheckObjectsToSent(FunctionNrToSend | FunctionNr);
                    CheckObjectsToSent(FunctionNrToSend | (DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1 + ((DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1)*TalkbackNr)));
                  }

                  if (NewTalkbackActive != OldTalkbackActive)
                  {
                    AxumData.DestinationData[DestinationNr].Dim = NewTalkbackActive;
                    CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_DIM);
                    CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
                  }
                }
              }
              break;
              case DESTINATION_FUNCTION_ROUTING:
              {
                if (type == MBN_DATATYPE_SINT)
                {
                  unsigned char OldRouting = AxumData.DestinationData[DestinationNr].Routing;

                  AxumData.DestinationData[DestinationNr].Routing += data.SInt%3;

                  if (AxumData.DestinationData[DestinationNr].Routing != OldRouting)
                  {
                    SetAxum_DestinationSource(DestinationNr);

                    unsigned int FunctionNrToSent = 0x06000000 | (DestinationNr<<12);
                    CheckObjectsToSent(FunctionNrToSent | DESTINATION_FUNCTION_ROUTING);
                  }
                }
              }
              break;
              case DESTINATION_FUNCTION_SELECT_1:
              case DESTINATION_FUNCTION_SELECT_2:
              case DESTINATION_FUNCTION_SELECT_3:
              case DESTINATION_FUNCTION_SELECT_4:
              {
                int SelectNr = FunctionNr-DESTINATION_FUNCTION_SELECT_1;
                unsigned int NewSelectedDestinationNr = AxumData.ConsoleData[SelectNr].SelectedDestination;

                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      NewSelectedDestinationNr = DestinationNr;
                    }
                  }
                  break;
                }

                SetSelectedDestination(SelectNr, NewSelectedDestinationNr);
              }
              break;
            }
          }
          break;
        }
      }
      SensorReceiveFunction->PreviousLastChangedTime = SensorReceiveFunction->LastChangedTime;
    }
  }
  node_info_lock(0);
  axum_data_lock(0);

  return 0;

  mbn=NULL;
}

//normally response on GetSensorData
int mSensorDataResponse(struct mbn_handler *mbn, struct mbn_message *message, short unsigned int object, unsigned char type, union mbn_data data)
{
  axum_data_lock(1);
  node_info_lock(1);

  ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = GetOnlineNodeInformation(message->AddressFrom);

  if (OnlineNodeInformationElement == NULL)
  {
    log_write("[mSensorDataResponse] OnlineNodeInformationElement not found for address: 0x%08X", message->AddressFrom);
    node_info_lock(0);
    axum_data_lock(0);
    return 1;
  }
  if (object>=(OnlineNodeInformationElement->UsedNumberOfCustomObjects+1024))
  {
    log_write("[mSensorDataResponse] Object: %d is unknown, this node (0x%08x), contains %d objects", object, message->AddressFrom, OnlineNodeInformationElement->UsedNumberOfCustomObjects);
    node_info_lock(0);
    axum_data_lock(0);
    return 1;
  }

  //fixed objects
  unsigned char CheckDBTemplateCount = 0;

  switch (type)
  {
    case MBN_DATATYPE_UINT:
    {
      if (object == 7)
      {   //Major firmware id
        if ((OnlineNodeInformationElement->ManufacturerID == 0x0001) &&
            (OnlineNodeInformationElement->ProductID == 0x000C) &&
            (data.UInt == 1))
        {   //Backplane must have major version 1
          if ((OnlineNodeInformationElement->ManufacturerID == this_node.HardwareParent[0]) &&
              (OnlineNodeInformationElement->ProductID == this_node.HardwareParent[1]) &&
              (OnlineNodeInformationElement->UniqueIDPerProduct == this_node.HardwareParent[2]))
          {
            if (BackplaneMambaNetAddress != OnlineNodeInformationElement->MambaNetAddress)
            { //Initialize all routing
              BackplaneMambaNetAddress = OnlineNodeInformationElement->MambaNetAddress;

              SetBackplaneClock();

              for (int cntModule=0; cntModule<128; cntModule++)
              {
                SetAxum_ModuleSource(cntModule);
                SetAxum_ModuleMixMinus(cntModule, 0);
                SetAxum_ModuleInsertSource(cntModule);
                SetAxum_BussLevels(cntModule);
              }
              SetAxum_BussMasterLevels();

              for (int cntDSPCard=0; cntDSPCard<4; cntDSPCard++)
              {
                SetAxum_ExternSources(cntDSPCard);

                //enable buss summing
                if (dsp_card_available(dsp_handler, cntDSPCard))
                {
                  unsigned int ObjectNumber = 1026+dsp_handler->dspcard[cntDSPCard].slot;
                  mbn_data data;

                  data.State = 1;//enabled
                  mbnSetActuatorData(mbn, BackplaneMambaNetAddress, ObjectNumber, MBN_DATATYPE_STATE, 1, data ,1);
                }
              }

              for (int cntDestination=0; cntDestination<1280; cntDestination++)
              {
                SetAxum_DestinationSource(cntDestination);
              }

              for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
              {
                SetAxum_TalkbackSource(cntTalkback);
              }
            }
          }
        }
        if (OnlineNodeInformationElement->FirmwareMajorRevision == -1)
        {
          OnlineNodeInformationElement->FirmwareMajorRevision = data.UInt;
          CheckDBTemplateCount = 1;
        }
      }
      if (object == 13)
      {
        if (OnlineNodeInformationElement->OnlineNumberOfCustomObjects == -1)
        {
          OnlineNodeInformationElement->OnlineNumberOfCustomObjects = data.UInt;
          if (data.UInt == 0)
          { //no objects, no init required
            OnlineNodeInformationElement->InitializationFinished = 1;
          }
          CheckDBTemplateCount = 1;
        }
      }

      if (CheckDBTemplateCount)
      {
        if ((OnlineNodeInformationElement->FirmwareMajorRevision != -1) &&
            (OnlineNodeInformationElement->OnlineNumberOfCustomObjects > 0))
        {
          db_lock(1);
          OnlineNodeInformationElement->TemplateNumberOfCustomObjects = db_read_template_count(OnlineNodeInformationElement->ManufacturerID, OnlineNodeInformationElement->ProductID, OnlineNodeInformationElement->FirmwareMajorRevision);

          if (OnlineNodeInformationElement->OnlineNumberOfCustomObjects == OnlineNodeInformationElement->TemplateNumberOfCustomObjects)
          {
            db_read_template_info(OnlineNodeInformationElement, 1);

            if (AxumData.ExternClock == OnlineNodeInformationElement->MambaNetAddress)
            {
              if (OnlineNodeInformationElement->EnableWCObjectNr != 0)
              {
                mbn_data data;

                data.State = 1;
                mbnSetActuatorData(mbn, OnlineNodeInformationElement->MambaNetAddress, OnlineNodeInformationElement->EnableWCObjectNr, MBN_DATATYPE_STATE, 1, data , 1);
                log_write("Enable extern clock 0x%08X (obj %d)", OnlineNodeInformationElement->MambaNetAddress, OnlineNodeInformationElement->EnableWCObjectNr);
              }
            }

            if (OnlineNodeInformationElement->SlotNumberObjectNr != -1)
            {
              log_write("Get slot from 0x%08X", OnlineNodeInformationElement->MambaNetAddress);
              mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, OnlineNodeInformationElement->SlotNumberObjectNr, 1);
            }

            db_read_node_defaults(OnlineNodeInformationElement, 1024, OnlineNodeInformationElement->UsedNumberOfCustomObjects+1023, 0, 0);
            db_read_node_config(OnlineNodeInformationElement, 1024, OnlineNodeInformationElement->UsedNumberOfCustomObjects+1023);
            OnlineNodeInformationElement->InitializationFinished = 1;
          }

          db_lock(0);
        }
      }
      if ((object>=1024) && (((signed int)object) == OnlineNodeInformationElement->SlotNumberObjectNr))
      {
        db_lock(1);
        for (unsigned char cntSlot=0; cntSlot<42; cntSlot++)
        {
          if (cntSlot != data.UInt)
          { //other slot then current inserted
            if (AxumData.RackOrganization[cntSlot] == message->AddressFrom)
            {
              AxumData.RackOrganization[cntSlot] = 0;
              db_delete_slot_config(cntSlot);
            }
          }
        }
        if (AxumData.RackOrganization[data.UInt] != message->AddressFrom)
        {
          AxumData.RackOrganization[data.UInt] = message->AddressFrom;

          log_write("0x%08lX found at slot: %d", message->AddressFrom, data.UInt+1);
          db_insert_slot_config(data.UInt, message->AddressFrom, 0, 0);
        }
        db_lock(0);

        //if a slot number exists, check the number of input/output channels.
        if (OnlineNodeInformationElement->InputChannelCountObjectNr != -1)
        {
          mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, OnlineNodeInformationElement->InputChannelCountObjectNr, 1);
        }
        if (OnlineNodeInformationElement->OutputChannelCountObjectNr != -1)
        {
          mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, OnlineNodeInformationElement->OutputChannelCountObjectNr, 1);
        }

        //Check for source if I/O card changed.
        for (int cntSource=0; cntSource<1280; cntSource++)
        {
          if (  (AxumData.SourceData[cntSource].InputData[0].MambaNetAddress == message->AddressFrom) ||
                (AxumData.SourceData[cntSource].InputData[1].MambaNetAddress == message->AddressFrom))
          { //this source is changed, update modules!
            //Found source 'cntSource'

            //Set Phantom, Pad and Gain
            unsigned int FunctionNrToSend = 0x05000000 | (cntSource<<12);
            CheckObjectsToSent(FunctionNrToSend | SOURCE_FUNCTION_PHANTOM);
            CheckObjectsToSent(FunctionNrToSend | SOURCE_FUNCTION_PAD);
            CheckObjectsToSent(FunctionNrToSend | SOURCE_FUNCTION_GAIN);

            for (int cntModule=0; cntModule<128; cntModule++)
            {
              if (AxumData.ModuleData[cntModule].SelectedSource == (cntSource+matrix_sources.src_offset.min.source))
              {
                //Found source @ module 'cntModule'
                SetAxum_ModuleSource(cntModule);
                SetAxum_ModuleMixMinus(cntModule, 0);

                unsigned int FunctionNrToSend = (cntModule<<12);
                CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_SOURCE_PHANTOM);
                CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_SOURCE_PAD);
                CheckObjectsToSent(FunctionNrToSend | MODULE_FUNCTION_SOURCE_GAIN_LEVEL);
              }
            }
            for (int cntDSPCard=0; cntDSPCard<4; cntDSPCard++)
            {
              for (int cntExt=0; cntExt<8; cntExt++)
              {
                if (AxumData.ExternSource[cntDSPCard].Ext[cntExt] == (cntSource+matrix_sources.src_offset.min.source))
                {
                  //Found source @ extern input 'cntDSPCard'
                  SetAxum_ExternSources(cntDSPCard);
                }
              }
            }
            for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
            {
              if (AxumData.Talkback[cntTalkback].Source == (cntSource+matrix_sources.src_offset.min.source))
              {
                //Found source @ talkback 'cntTalkback'
                SetAxum_TalkbackSource(cntTalkback);
              }
            }
            for (int cntDestination=0; cntDestination<1280; cntDestination++)
            {
              if (AxumData.DestinationData[cntDestination].Source == (cntSource+matrix_sources.src_offset.min.source))
              {
                //Found source @ destination 'cntDestination';
                SetAxum_DestinationSource(cntDestination);
              }
            }
          }
        }

        //Check for destination if I/O card changed.
        for (int cntDestination=0; cntDestination<1280; cntDestination++)
        {
          if (  (AxumData.DestinationData[cntDestination].OutputData[0].MambaNetAddress == message->AddressFrom) ||
                (AxumData.DestinationData[cntDestination].OutputData[1].MambaNetAddress == message->AddressFrom))
          { //this source is changed, update modules!
            //Found destination 'cntDestination';
            SetAxum_DestinationSource(cntDestination);

            unsigned int FunctionNrToSend = 0x06000000 | (cntDestination<<12);
            CheckObjectsToSent(FunctionNrToSend | DESTINATION_FUNCTION_LEVEL);
          }
        }
      }
      else if (OnlineNodeInformationElement->SlotNumberObjectNr != -1)
      {
        //Check for Channel Counts
        if (((signed int)object) == OnlineNodeInformationElement->InputChannelCountObjectNr)
        {
          db_lock(1);
          db_update_slot_config_input_ch_cnt(message->AddressFrom, data.UInt);
          db_lock(0);
        }
        else if (((signed int)object) == OnlineNodeInformationElement->OutputChannelCountObjectNr)
        {
          db_lock(1);
          db_update_slot_config_output_ch_cnt(message->AddressFrom, data.UInt);
          db_lock(0);
        }
      }
      if (object>=1024)
      {
        float DataMinimal = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataMinimal;
        float DataMaximal = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataMaximal;
        int FunctionNr = OnlineNodeInformationElement->SensorReceiveFunction[object-1024].FunctionNr;

        if ((FunctionNr&0xFF000FFF) == (0x02000000 | MONITOR_BUSS_FUNCTION_SPEAKER_LEVEL))
        {
          int Position = (data.UInt*1023)/(DataMaximal-DataMinimal);
          float dB = Position2dB[Position];
          int MonitorBussNr = (FunctionNr>>12)&0xFFF;
          dB += 10;

          if (AxumData.Monitor[MonitorBussNr].SpeakerLevel != dB)
          {
            AxumData.Monitor[MonitorBussNr].SpeakerLevel = dB;
            CheckObjectsToSent(FunctionNr);

            for (int cntDestination=0; cntDestination<1280; cntDestination++)
            {
              if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
              {
                unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_MONITOR_SPEAKER_LEVEL);
              }
            }
          }
        }
        if ((FunctionNr&0xFF000FFF) == (0x02000000 | MONITOR_BUSS_FUNCTION_PHONES_LEVEL))
        {
          int Position = (data.UInt*1023)/(DataMaximal-DataMinimal);
          float dB = Position2dB[Position];
          int MonitorBussNr = (FunctionNr>>12)&0xFFF;
          dB += 10;

          if (AxumData.Monitor[MonitorBussNr].PhonesLevel != dB)
          {
            AxumData.Monitor[MonitorBussNr].PhonesLevel = dB;
            CheckObjectsToSent(FunctionNr);

            for (int cntDestination=0; cntDestination<1280; cntDestination++)
            {
              if (AxumData.DestinationData[cntDestination].Source == (matrix_sources.src_offset.min.monitor_buss+MonitorBussNr))
              {
                unsigned int DisplayFunctionNr = 0x06000000 | (cntDestination<<12);
                CheckObjectsToSent(DisplayFunctionNr | DESTINATION_FUNCTION_MONITOR_PHONES_LEVEL);
              }
            }
          }
        }
      }
    }
    break;
  }

  //returned custom object-data that depends on function configuration
  if (object>=1024)
  {
    if (OnlineNodeInformationElement->SensorReceiveFunction != NULL)
    {
      SENSOR_RECEIVE_FUNCTION_STRUCT *SensorReceiveFunction = &OnlineNodeInformationElement->SensorReceiveFunction[object-1024];
      int SensorReceiveFunctionNumber = SensorReceiveFunction->FunctionNr;
      //int DataType = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataType;
      //int DataSize = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataSize;
      //float DataMinimal = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataMinimal;
      //float DataMaximal = OnlineNodeInformationElement->ObjectInformation[object-1024].SensorDataMaximal;

      if (SensorReceiveFunctionNumber != -1)
      {
        unsigned char SensorReceiveFunctionType = (SensorReceiveFunctionNumber>>24)&0xFF;

        switch (SensorReceiveFunctionType)
        {
          case MODULE_FUNCTIONS:
          {   //Module
            unsigned int ModuleNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((ModuleNr >= NUMBER_OF_MODULES) && (ModuleNr<(NUMBER_OF_MODULES+4)))
            {
              ModuleNr = AxumData.ConsoleData[ModuleNr-NUMBER_OF_MODULES].SelectedModule;
            }

            switch (FunctionNr)
            {
            }
          }
          break;
          case BUSS_FUNCTIONS:
          {   //Busses
            unsigned int BussNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((BussNr>=NUMBER_OF_BUSSES) && (BussNr<(NUMBER_OF_BUSSES+4)))
            {
              BussNr = AxumData.ConsoleData[BussNr-NUMBER_OF_BUSSES].SelectedBuss;
            }

            switch (FunctionNr)
            {
            }
          }
          break;
          case MONITOR_BUSS_FUNCTIONS:
          {   //Monitor Busses
            int MonitorBussNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((MonitorBussNr>=NUMBER_OF_MONITOR_BUSSES) && (MonitorBussNr<(NUMBER_OF_MONITOR_BUSSES+4)))
            {
              MonitorBussNr = AxumData.ConsoleData[MonitorBussNr-NUMBER_OF_MONITOR_BUSSES].SelectedMonitorBuss;
            }

            switch (FunctionNr)
            {
            }
          }
          break;
          case CONSOLE_FUNCTIONS:
          {
            unsigned int ConsoleNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            switch (FunctionNr)
            {
              case CONSOLE_FUNCTION_CHIPCARD_CHANGE:
              {
                switch (type)
                {
                  case MBN_DATATYPE_STATE:
                  {
                    if (data.State)
                    {
                      OnlineNodeInformationElement->Account.UsernameReceived = 0;
                      OnlineNodeInformationElement->Account.PasswordReceived = 0;

                      for (int cntObject=1024; cntObject<OnlineNodeInformationElement->UsedNumberOfCustomObjects+1024; cntObject++)
                      {
                        SENSOR_RECEIVE_FUNCTION_STRUCT *ConnectedEngineFunction = &OnlineNodeInformationElement->SensorReceiveFunction[cntObject-1024];

                        if ((int)ConnectedEngineFunction->FunctionNr != -1)
                        {
                          unsigned int EngineFunctionType = (ConnectedEngineFunction->FunctionNr>>24)&0xFF;
                          unsigned int EngineFunctionSeqNr = (ConnectedEngineFunction->FunctionNr>>12)&0xFFF;
                          unsigned int EngineFunctionNr = ConnectedEngineFunction->FunctionNr&0xFFF;

                          if (EngineFunctionSeqNr == ConsoleNr)
                          {
                            if (EngineFunctionType == CONSOLE_FUNCTIONS)
                            {
                              if (EngineFunctionNr==((unsigned int)CONSOLE_FUNCTION_CHIPCARD_USER))
                              {
                                mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, cntObject, 1);
                              }
                              else if (EngineFunctionNr==((unsigned int)CONSOLE_FUNCTION_CHIPCARD_PASS))
                              {
                                mbnGetSensorData(mbn, OnlineNodeInformationElement->MambaNetAddress, cntObject, 1);
                              }
                            }
                          }
                        }
                      }
                    }
                    else
                    { //Send user/pass idle
                      if (AxumData.ConsoleData[ConsoleNr].LogoutToIdle)
                      {
                        if ((strncmp(AxumData.ConsoleData[ConsoleNr].ActiveUsername, OnlineNodeInformationElement->Account.Username, 32) == 0) &&
                            (strncmp(AxumData.ConsoleData[ConsoleNr].ActivePassword, OnlineNodeInformationElement->Account.Password, 32) == 0))
                        {
                          unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                          memset(AxumData.ConsoleData[ConsoleNr].Username, 0, 32);
                          memset(AxumData.ConsoleData[ConsoleNr].Password, 0, 16);
                          memset(AxumData.ConsoleData[ConsoleNr].ActiveUsername, 0, 32);
                          memset(AxumData.ConsoleData[ConsoleNr].ActivePassword, 0, 16);
                          //Idle, source/preset pool A
                          AxumData.ConsoleData[ConsoleNr].UserLevel = 0;
                          AxumData.ConsoleData[ConsoleNr].SourcePool = 0;
                          AxumData.ConsoleData[ConsoleNr].PresetPool = 0;
                          AxumData.ConsoleData[ConsoleNr].LogoutToIdle = 0;
                          AxumData.ConsoleData[ConsoleNr].ConsolePreset = 0;

                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_USER_PASS));
                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_USER));
                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_UPDATE_PASS));
                          CheckObjectsToSent(FunctionNrToSend | (CONSOLE_FUNCTION_USER_LEVEL));


                          db_lock(1);
                          db_update_account(ConsoleNr, AxumData.ConsoleData[ConsoleNr].ActiveUsername, AxumData.ConsoleData[ConsoleNr].ActivePassword);
                          db_lock(0);
                        }
                      }
                      OnlineNodeInformationElement->Account.UsernameReceived = 0;
                      OnlineNodeInformationElement->Account.PasswordReceived = 0;
                      memset(OnlineNodeInformationElement->Account.Username, 0, 32);
                      memset(OnlineNodeInformationElement->Account.Password, 0, 16);

                      db_lock(1);
                      db_update_chipcard_account(ConsoleNr, OnlineNodeInformationElement->Account.Username, OnlineNodeInformationElement->Account.Password);
                      db_lock(0);
                    }
                  }
                }
              }
              break;
              case CONSOLE_FUNCTION_CHIPCARD_USER:
              {
                OnlineNodeInformationElement->Account.UsernameReceived = 1;
                strncpy(OnlineNodeInformationElement->Account.Username, (char *)data.Octets, 32);
                strncpy(AxumData.ConsoleData[ConsoleNr].Username, (char *)data.Octets, 32);
                if (OnlineNodeInformationElement->Account.PasswordReceived)
                {
                  strncpy(AxumData.ConsoleData[ConsoleNr].Password, OnlineNodeInformationElement->Account.Password, 16);
                  unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_USER_PASS);
                  CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_USER_LEVEL);

                  db_lock(1);
                  db_read_user(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                  db_update_chipcard_account(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                  db_lock(0);
                }
                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_USER);
              }
              break;
              case CONSOLE_FUNCTION_CHIPCARD_PASS:
              {
                OnlineNodeInformationElement->Account.PasswordReceived = 1;
                strncpy(OnlineNodeInformationElement->Account.Password, (char *)data.Octets, 16);
                strncpy(AxumData.ConsoleData[ConsoleNr].Password, (char *)data.Octets, 16);
                if (OnlineNodeInformationElement->Account.UsernameReceived)
                {
                  strncpy(AxumData.ConsoleData[ConsoleNr].Username, OnlineNodeInformationElement->Account.Username, 32);
                  unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                  CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_USER_PASS);
                  CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_USER_LEVEL);

                  db_lock(1);
                  db_read_user(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                  db_update_chipcard_account(ConsoleNr, AxumData.ConsoleData[ConsoleNr].Username, AxumData.ConsoleData[ConsoleNr].Password);
                  db_lock(0);
                }

                unsigned int FunctionNrToSend = 0x03000000 | (ConsoleNr<<12);
                CheckObjectsToSent(FunctionNrToSend | CONSOLE_FUNCTION_UPDATE_PASS);
              }
              break;
            }
          }
          case GLOBAL_FUNCTIONS:
          {   //Global
            unsigned int GlobalNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if (GlobalNr == 0)
            {
              switch (FunctionNr)
              {
              }
            }
          }
          break;
          case SOURCE_FUNCTIONS:
          { //Source
            int SourceNr = ((SensorReceiveFunctionNumber>>12)&0xFFF);
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((SourceNr>=NUMBER_OF_SOURCES) && (SourceNr<(NUMBER_OF_SOURCES+4)))
            {
              SourceNr = AxumData.ConsoleData[SourceNr-NUMBER_OF_SOURCES].SelectedSource;
            }

            switch (FunctionNr)
            {
            }
          }
          break;
          case DESTINATION_FUNCTIONS:
          { //Destination
            unsigned int DestinationNr = ((SensorReceiveFunctionNumber>>12)&0xFFF);
            unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

            if ((DestinationNr>=NUMBER_OF_DESTINATIONS) && (DestinationNr<(NUMBER_OF_DESTINATIONS+4)))
            {
              DestinationNr = AxumData.ConsoleData[DestinationNr-NUMBER_OF_DESTINATIONS].SelectedBuss;
            }

            switch (FunctionNr)
            {
            }
          }
          break;
        }
      }
    }
  }
  node_info_lock(0);
  axum_data_lock(0);
  return 0;
}

void mAddressTableChange(struct mbn_handler *mbn, struct mbn_address_node *old_info, struct mbn_address_node *new_info)
{
  axum_data_lock(1);
  node_info_lock(1);
  if (old_info == NULL)
  {
    log_write("Add node with MambaNet address: %08lX, Services: %02X", new_info->MambaNetAddr, new_info->Services);
    ONLINE_NODE_INFORMATION_STRUCT *NewOnlineNodeInformationElement = new ONLINE_NODE_INFORMATION_STRUCT;
    NewOnlineNodeInformationElement->Next = NULL;
    NewOnlineNodeInformationElement->MambaNetAddress = new_info->MambaNetAddr;
    NewOnlineNodeInformationElement->ManufacturerID = new_info->ManufacturerID;
    NewOnlineNodeInformationElement->ProductID = new_info->ProductID;
    NewOnlineNodeInformationElement->UniqueIDPerProduct = new_info->UniqueIDPerProduct;
    NewOnlineNodeInformationElement->FirmwareMajorRevision = -1;
    NewOnlineNodeInformationElement->UserLevelFromConsole = 0;
    NewOnlineNodeInformationElement->TimerRequestDone = 0;
    NewOnlineNodeInformationElement->InitializationFinished = 0;
    NewOnlineNodeInformationElement->SlotNumberObjectNr = -1;
    NewOnlineNodeInformationElement->InputChannelCountObjectNr = -1;
    NewOnlineNodeInformationElement->OutputChannelCountObjectNr = -1;
    NewOnlineNodeInformationElement->EnableWCObjectNr = -1;
    NewOnlineNodeInformationElement->Parent.ManufacturerID = 0;
    NewOnlineNodeInformationElement->Parent.ProductID = 0;
    NewOnlineNodeInformationElement->Parent.UniqueIDPerProduct = 0;
    NewOnlineNodeInformationElement->UsedNumberOfCustomObjects = 0;
    NewOnlineNodeInformationElement->OnlineNumberOfCustomObjects = -1;
    NewOnlineNodeInformationElement->TemplateNumberOfCustomObjects = -1;
    NewOnlineNodeInformationElement->SensorReceiveFunction = NULL;
    NewOnlineNodeInformationElement->ObjectInformation = NULL;
    NewOnlineNodeInformationElement->Account.UsernameReceived = 0;
    NewOnlineNodeInformationElement->Account.PasswordReceived = 0;
    memset(NewOnlineNodeInformationElement->Account.Username, 0, 33);
    memset(NewOnlineNodeInformationElement->Account.Password, 0, 17);
    //update element from database
    db_lock(1);
    db_read_node_info(NewOnlineNodeInformationElement);
    db_lock(0);

    if (OnlineNodeInformationList == NULL)
    {
      OnlineNodeInformationList = NewOnlineNodeInformationElement;
    }
    else
    {
      ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = OnlineNodeInformationList;
      while ((OnlineNodeInformationElement->Next != NULL) && (OnlineNodeInformationElement->MambaNetAddress != new_info->MambaNetAddr))
      {
        OnlineNodeInformationElement = OnlineNodeInformationElement->Next;
      }

      if (OnlineNodeInformationElement->MambaNetAddress == new_info->MambaNetAddr)
      {
        log_write("WARNING: MambaNet address 0x%08X already found in node inforamtion list", new_info->MambaNetAddr);
      }
      else
      {
        OnlineNodeInformationElement->Next = NewOnlineNodeInformationElement;
      }
    }

    /*if (mbn->node.Services&0x80)
    {
      unsigned int ObjectNr = 7;//Firmware major revision;
      mbnGetSensorData(mbn, new_info->MambaNetAddr, ObjectNr, 1);
      log_write("Get firmware: %08lX", new_info->MambaNetAddr);
    }*/
  }
  else if (new_info == NULL)
  {
    bool removed = false;
    if (old_info->MambaNetAddr != 0x00000000)
    {
      if (OnlineNodeInformationList == NULL)
      {
        log_write("Error removing NodeInformationElement of address 0x%08lX", old_info->MambaNetAddr);
      }
      else
      {
        ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = OnlineNodeInformationList;
        ONLINE_NODE_INFORMATION_STRUCT *PreviousOnlineNodeInformationElement = NULL;

        while ((!removed) && (OnlineNodeInformationElement != NULL))
        {
          if (OnlineNodeInformationElement->MambaNetAddress == old_info->MambaNetAddr)
          {
            if (OnlineNodeInformationElement == OnlineNodeInformationList)
            {
              OnlineNodeInformationList = OnlineNodeInformationElement->Next;
            }
            else if (PreviousOnlineNodeInformationElement != NULL)
            {
              PreviousOnlineNodeInformationElement->Next = OnlineNodeInformationElement->Next;
            }
            else
            {
              log_write("WARNING: PreviousOnlineNodeInformationElement == NULL");
            }

            //Adjust function lists
            for (int cntObject=0; cntObject<OnlineNodeInformationElement->UsedNumberOfCustomObjects; cntObject++)
            {
              int FunctionNr = OnlineNodeInformationElement->SensorReceiveFunction[cntObject].FunctionNr;
              OnlineNodeInformationElement->SensorReceiveFunction[cntObject].FunctionNr = -1;
              if (FunctionNr != -1)
              {
                MakeObjectListPerFunction(FunctionNr);
              }
            }

            if (OnlineNodeInformationElement->SensorReceiveFunction != NULL)
            {
              delete[] OnlineNodeInformationElement->SensorReceiveFunction;
            }
            if (OnlineNodeInformationElement->ObjectInformation != NULL)
            {
              delete[] OnlineNodeInformationElement->ObjectInformation;
            }

            delete OnlineNodeInformationElement;
            removed = true;
          }
          else
          {
            PreviousOnlineNodeInformationElement = OnlineNodeInformationElement;
            OnlineNodeInformationElement = OnlineNodeInformationElement->Next;
          }
        }

        //remove audio routing if set..
        db_lock(1);
        for (unsigned char cntSlot=0; cntSlot<42; cntSlot++)
        {
          if (AxumData.RackOrganization[cntSlot] == old_info->MambaNetAddr)
          {
            AxumData.RackOrganization[cntSlot] = 0;
            db_delete_slot_config(cntSlot);
          }
        }
        db_lock(0);

        for (int cntSource=0; cntSource<1280; cntSource++)
        {
          unsigned char RemoveSource = 0;

          for (int cntInput=0; cntInput<8; cntInput++)
          {
            if (AxumData.SourceData[cntSource].InputData[cntInput].MambaNetAddress == old_info->MambaNetAddr)
            {
              RemoveSource = 1;
            }
          }
          if (RemoveSource)
          {
            //Found source 'cntSource'
            for (int cntModule=0; cntModule<128; cntModule++)
            {
              if (AxumData.ModuleData[cntModule].SelectedSource == (cntSource+matrix_sources.src_offset.min.source))
              {
                //Found source @ module 'cntModule'
                SetAxum_ModuleSource(cntModule);
                SetAxum_ModuleMixMinus(cntModule, 0);
              }
            }
            for (int cntDSPCard=0; cntDSPCard<4; cntDSPCard++)
            {
              for (int cntExt=0; cntExt<8; cntExt++)
              {
                if (AxumData.ExternSource[cntDSPCard].Ext[cntExt] == (cntSource+matrix_sources.src_offset.min.source))
                {
                  //Found source @ extern input 'cntDSPCard'
                  SetAxum_ExternSources(cntDSPCard);
                }
              }
            }
            for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
            {
              if (AxumData.Talkback[cntTalkback].Source == (cntSource+matrix_sources.src_offset.min.source))
              {
                //Found source @ talkback 'cntTalkback'
                SetAxum_TalkbackSource(cntTalkback);
              }
            }
            for (int cntDestination=0; cntDestination<1280; cntDestination++)
            {
              if (AxumData.DestinationData[cntDestination].Source == (cntSource+matrix_sources.src_offset.min.source))
              {
                //Found source @ destination 'cntDestination';
                SetAxum_DestinationSource(cntDestination);
              }
            }
          }
        }

        //Check for destination if I/O card changed.
        for (int cntDestination=0; cntDestination<1280; cntDestination++)
        {
          unsigned char RemoveDestination = 0;
          for (int cntOutput=0; cntOutput<8; cntOutput++)
          {
            if (AxumData.DestinationData[cntDestination].OutputData[cntOutput].MambaNetAddress == old_info->MambaNetAddr)
            {
              RemoveDestination = 1;
            }
          }
          if (RemoveDestination)
          {
            //Found destination 'cntDestination';
            SetAxum_DestinationSource(cntDestination);
          }
        }
      }
      if (removed)
      {
        log_write("Removed node with MambaNet address: %08lX", old_info->MambaNetAddr);
      }
      else
      {
        log_write("WARNING: node with MambaNet address: %08lX not removed!", old_info->MambaNetAddr);
      }
    }
    else
    {
      log_write("WARNING: We skipped the remove node with address 0x00000000");
    }
  }
  else
  {
    bool Found = false;
    ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = OnlineNodeInformationList;
    ONLINE_NODE_INFORMATION_STRUCT *FoundOnlineNodeInformationElement = NULL;

    while ((!Found) && (OnlineNodeInformationElement != NULL))
    {
      if (OnlineNodeInformationElement->MambaNetAddress == old_info->MambaNetAddr)
      {
        Found = true;
        FoundOnlineNodeInformationElement = OnlineNodeInformationElement;
      }
      OnlineNodeInformationElement = OnlineNodeInformationElement->Next;
    }

    if (FoundOnlineNodeInformationElement != NULL)
    {
      log_write("change OnlineNodeInformation");
    }
    log_write("Address changed: %08lX => %08lX", old_info->MambaNetAddr, new_info->MambaNetAddr);
  }
  node_info_lock(0);
  axum_data_lock(0);
  mbn = NULL;
}

void mError(struct mbn_handler *m, int code, char *str) {
  log_write("MambaNet Error: %s (%d)", str, code);
  m=NULL;
}

void mAcknowledgeTimeout(struct mbn_handler *m, struct mbn_message *msg) {
  log_write("Acknowledge timeout for message to %08lX, obj: %d", msg->AddressTo, msg->Message.Object.Number);
  m=NULL;
}

void mAcknowledgeReply(struct mbn_handler *m, struct mbn_message *request, struct mbn_message *reply, int retries) {
  if (retries>0)
  {
    log_write("Acknowledge reply for message to %08lX, obj: %d, retries: %d", reply->AddressFrom, reply->Message.Object.Number, retries);
  }
  m=NULL;
  request=NULL;
}

int First = 1;
char CurrentLinkStatus = 0;
int PreviousInitializedNodes = 0;
int PreviousNodeCount = 0;

void Timer100HzDone(int Value)
{
  char LinkStatus;
  int InitializedNodes;
  int NodeCount;

  if (First)
  { //First time wait 60 seconds before starting meters
    cntMillisecondTimer = 6000;
    PreviousCount_LevelMeter = cntMillisecondTimer+6000;
    PreviousCount_SignalDetect = cntMillisecondTimer+6000;
    PreviousCount_PhaseMeter = cntMillisecondTimer+6000;

    //dummy read meters to empty level buffers
    dsp_read_buss_levelmeters(dsp_handler, SummingdBLevel);
    dsp_read_module_levelmeters(dsp_handler, dBLevel);
    dsp_read_buss_phasemeters(dsp_handler, SummingPhase);
    dsp_read_module_phasemeters(dsp_handler, Phase);
    First = 0;
 }

  if (((int)(cntMillisecondTimer-PreviousCount_LevelMeter))>LevelMeterFrequency)
  {
    PreviousCount_LevelMeter = cntMillisecondTimer;
    dsp_read_buss_levelmeters(dsp_handler, SummingdBLevel);

    //buss audio level
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      CheckObjectsToSent(0x01000000 | (cntBuss<<12) | BUSS_FUNCTION_AUDIO_LEVEL_LEFT);
      CheckObjectsToSent(0x01000000 | (cntBuss<<12) | BUSS_FUNCTION_AUDIO_LEVEL_RIGHT);
    }

    //monitor buss audio level
    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      CheckObjectsToSent(0x02000000 | (cntMonitorBuss<<12) | MONITOR_BUSS_FUNCTION_AUDIO_LEVEL_LEFT);
      CheckObjectsToSent(0x02000000 | (cntMonitorBuss<<12) | MONITOR_BUSS_FUNCTION_AUDIO_LEVEL_RIGHT);
    }
  }

  if (((int)(cntMillisecondTimer-PreviousCount_PhaseMeter))>PhaseMeterFrequency)
  {
    PreviousCount_PhaseMeter = cntMillisecondTimer;
    dsp_read_buss_phasemeters(dsp_handler, SummingPhase);

    //buss audio level
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      CheckObjectsToSent(0x01000000 | (cntBuss<<12) | BUSS_FUNCTION_AUDIO_PHASE);
    }

    //monitor buss audio level
    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      CheckObjectsToSent(0x02000000 | (cntMonitorBuss<<12) | MONITOR_BUSS_FUNCTION_AUDIO_PHASE);
    }

    dsp_read_module_phasemeters(dsp_handler, Phase);
    for (int cntModule=0; cntModule<128; cntModule++)
    {
      CheckObjectsToSent((cntModule<<12) | MODULE_FUNCTION_AUDIO_PHASE);
    }
  }

  if ((cntMillisecondTimer-PreviousCount_SignalDetect)>10)
  {
    PreviousCount_SignalDetect = cntMillisecondTimer;
    dsp_read_module_levelmeters(dsp_handler, dBLevel);

    axum_data_lock(1);
    for (int cntModule=0; cntModule<128; cntModule++)
    {
      int FirstChannelNr = cntModule<<1;
      unsigned int DisplayFunctionNumber;

      CheckObjectsToSent((cntModule<<12) | MODULE_FUNCTION_AUDIO_LEVEL_LEFT);
      CheckObjectsToSent((cntModule<<12) | MODULE_FUNCTION_AUDIO_LEVEL_RIGHT);

      if (((dBLevel[FirstChannelNr]+AxumData.Headroom)>-30) || ((dBLevel[FirstChannelNr+1]+AxumData.Headroom)>-30))
      {   //Signal
        if (!AxumData.ModuleData[cntModule].Signal)
        {
          AxumData.ModuleData[cntModule].Signal = 1;

          DisplayFunctionNumber = (cntModule<<12) | MODULE_FUNCTION_SIGNAL;
          CheckObjectsToSent(DisplayFunctionNumber);
        }

        if ((dBLevel[FirstChannelNr]>-3) || (dBLevel[FirstChannelNr+1]>-3))
        {   //Peak
          if (!AxumData.ModuleData[cntModule].Peak)
          {
            AxumData.ModuleData[cntModule].Peak = 1;
            DisplayFunctionNumber = (cntModule<<12) | MODULE_FUNCTION_PEAK;
            CheckObjectsToSent(DisplayFunctionNumber);
          }
        }
        else
        {
          if (AxumData.ModuleData[cntModule].Peak)
          {
            AxumData.ModuleData[cntModule].Peak = 0;
            DisplayFunctionNumber = (cntModule<<12) | MODULE_FUNCTION_PEAK;
            CheckObjectsToSent(DisplayFunctionNumber);
          }
        }
      }
      else
      {
        if (AxumData.ModuleData[cntModule].Signal)
        {
          AxumData.ModuleData[cntModule].Signal = 0;
          DisplayFunctionNumber = (cntModule<<12) | MODULE_FUNCTION_SIGNAL;
          CheckObjectsToSent(DisplayFunctionNumber);

          if (AxumData.ModuleData[cntModule].Peak)
          {
            AxumData.ModuleData[cntModule].Peak = 0;
            DisplayFunctionNumber = (cntModule<<12) | MODULE_FUNCTION_PEAK;
            CheckObjectsToSent(DisplayFunctionNumber);
          }
        }
      }
    }
    axum_data_lock(0);
  }

  cntMillisecondTimer++;
//  if ((cntMillisecondTimer-PreviousCount_Second)>50)
  if ((cntMillisecondTimer-PreviousCount_Second)>5)
  {
    PreviousCount_Second = cntMillisecondTimer;

    //Check for firmware requests
    if (mbn->node.Services&0x80)
    {
      ONLINE_NODE_INFORMATION_STRUCT *TimerWalkOnlineNodeInformationElement = NULL;
      bool RequestDone = false;

      axum_data_lock(1);
      node_info_lock(1);

      //We may only start from the first element because multiple threads
      TimerWalkOnlineNodeInformationElement = OnlineNodeInformationList;
      while ((TimerWalkOnlineNodeInformationElement != NULL) && (!RequestDone))
      {
        if ((TimerWalkOnlineNodeInformationElement->FirmwareMajorRevision == -1) ||
            (TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects == -1) ||
            ((TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects != TimerWalkOnlineNodeInformationElement->TemplateNumberOfCustomObjects) &&
             (TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects)))
        {
          if ((TimerWalkOnlineNodeInformationElement->MambaNetAddress != 0x00000000) &&
             (!TimerWalkOnlineNodeInformationElement->TimerRequestDone))
          {
            if (TimerWalkOnlineNodeInformationElement->FirmwareMajorRevision == -1)
            {
              unsigned int ObjectNr = 7; //Firmware major revision
              log_write("timer: Get firmware 0x%08X", TimerWalkOnlineNodeInformationElement->MambaNetAddress);
              mbnGetSensorData(mbn, TimerWalkOnlineNodeInformationElement->MambaNetAddress, ObjectNr, 0);
              RequestDone = true;
              TimerWalkOnlineNodeInformationElement->TimerRequestDone = true;
            }
            else if (TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects == -1)
            {
              unsigned int ObjectNr = 13; //Number of custom objects
              log_write("timer: Get number of custom objects 0x%08X", TimerWalkOnlineNodeInformationElement->MambaNetAddress);
              mbnGetSensorData(mbn, TimerWalkOnlineNodeInformationElement->MambaNetAddress, ObjectNr, 0);
              RequestDone = true;
              TimerWalkOnlineNodeInformationElement->TimerRequestDone = true;
            }
            if ((TimerWalkOnlineNodeInformationElement->FirmwareMajorRevision != -1) &&
                (TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects > 0) &&
                (TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects != TimerWalkOnlineNodeInformationElement->TemplateNumberOfCustomObjects))
            {
              log_write("timer: Get template number of custom objects 0x%08X, (%d/%d)", TimerWalkOnlineNodeInformationElement->MambaNetAddress,
                                                                                      TimerWalkOnlineNodeInformationElement->TemplateNumberOfCustomObjects,
                                                                                      TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects);
              db_lock(1);
              TimerWalkOnlineNodeInformationElement->TemplateNumberOfCustomObjects =
                db_read_template_count(TimerWalkOnlineNodeInformationElement->ManufacturerID,
                TimerWalkOnlineNodeInformationElement->ProductID,
                TimerWalkOnlineNodeInformationElement->FirmwareMajorRevision);

              if (TimerWalkOnlineNodeInformationElement->OnlineNumberOfCustomObjects == TimerWalkOnlineNodeInformationElement->TemplateNumberOfCustomObjects)
              {
                log_write("database filled with the template from 0x%08X, %d objects", TimerWalkOnlineNodeInformationElement->MambaNetAddress,
                                                                                     TimerWalkOnlineNodeInformationElement->TemplateNumberOfCustomObjects);

                db_read_template_info(TimerWalkOnlineNodeInformationElement, 1);

                if (AxumData.ExternClock == TimerWalkOnlineNodeInformationElement->MambaNetAddress)
                {
                  if (TimerWalkOnlineNodeInformationElement->EnableWCObjectNr != 0)
                  {
                    mbn_data data;

                    data.State = 1;
                    mbnSetActuatorData(mbn, TimerWalkOnlineNodeInformationElement->MambaNetAddress, TimerWalkOnlineNodeInformationElement->EnableWCObjectNr, MBN_DATATYPE_STATE, 1, data , 1);
                    log_write("Enable extern clock 0x%08X (obj %d)", TimerWalkOnlineNodeInformationElement->MambaNetAddress, TimerWalkOnlineNodeInformationElement->EnableWCObjectNr);
                  }
                }
                if (TimerWalkOnlineNodeInformationElement->SlotNumberObjectNr != -1)
                {
                  mbnGetSensorData(mbn, TimerWalkOnlineNodeInformationElement->MambaNetAddress, TimerWalkOnlineNodeInformationElement->SlotNumberObjectNr, 1);
                  RequestDone = true;
                  TimerWalkOnlineNodeInformationElement->TimerRequestDone = true;
                }

                db_read_node_defaults(TimerWalkOnlineNodeInformationElement, 1024, TimerWalkOnlineNodeInformationElement->UsedNumberOfCustomObjects+1023, 0, 0);
                db_read_node_config(TimerWalkOnlineNodeInformationElement, 1024, TimerWalkOnlineNodeInformationElement->UsedNumberOfCustomObjects+1023);
                TimerWalkOnlineNodeInformationElement->InitializationFinished = 1;
              }
              db_lock(0);
            }
          }
        }
        TimerWalkOnlineNodeInformationElement = TimerWalkOnlineNodeInformationElement->Next;
      }
      //Last in list, clear the TimerRequestDone bits
      if (TimerWalkOnlineNodeInformationElement == NULL)
      {
        TimerWalkOnlineNodeInformationElement = OnlineNodeInformationList;
        while (TimerWalkOnlineNodeInformationElement != NULL)
        {
          TimerWalkOnlineNodeInformationElement->TimerRequestDone = false;
          TimerWalkOnlineNodeInformationElement = TimerWalkOnlineNodeInformationElement->Next;
        }
      }
      node_info_lock(0);
      axum_data_lock(0);
    }
  }

  if (cntBroadcastPing)
  {
    if ((cntMillisecondTimer-PreviousCount_BroadcastPing)> 500)
    {
      mbnSendPingRequest(mbn, MBN_BROADCAST_ADDRESS);
      cntBroadcastPing--;
      PreviousCount_BroadcastPing = cntMillisecondTimer;
    }
  }

  axum_data_lock(1);
  for (int cntConsolePreset=0; cntConsolePreset<32; cntConsolePreset++)
  {
    if (ConsolePresetSwitch[cntConsolePreset].PreviousState != ConsolePresetSwitch[cntConsolePreset].State)
    {
      ConsolePresetSwitch[cntConsolePreset].PreviousState = ConsolePresetSwitch[cntConsolePreset].State;
      if (ConsolePresetSwitch[cntConsolePreset].State)
      {
        ConsolePresetSwitch[cntConsolePreset].TimerValue = 0;
      }
    }
    else if (ConsolePresetSwitch[cntConsolePreset].State)
    {
      if (ConsolePresetSwitch[cntConsolePreset].TimerValue<(AxumData.ConsolePresetData[cntConsolePreset].ForcedRecallTime+10))
      {
        int Safe = ConsolePresetSwitch[cntConsolePreset].TimerValue-AxumData.ConsolePresetData[cntConsolePreset].SafeRecallTime;
        int Forced = ConsolePresetSwitch[cntConsolePreset].TimerValue-AxumData.ConsolePresetData[cntConsolePreset].ForcedRecallTime;
        if ((Forced>=0) && (Forced<10))
        {
          DoAxum_LoadConsolePreset(cntConsolePreset+1, 0, 1);
          ConsolePresetSwitch[cntConsolePreset].State = 0;
        }
        else if ((Safe>=0) && (Safe<10))
        {
          DoAxum_LoadConsolePreset(cntConsolePreset+1, 0, 0);
        }
        ConsolePresetSwitch[cntConsolePreset].TimerValue += 10;
      }
    }
  }
  axum_data_lock(0);

  axum_data_lock(1);
  for (unsigned char cntConsole=0; cntConsole<4; cntConsole++)
  {
    if (ProgrammedDefaultSwitch[cntConsole].PreviousState != ProgrammedDefaultSwitch[cntConsole].State)
    {
      ProgrammedDefaultSwitch[cntConsole].PreviousState = ProgrammedDefaultSwitch[cntConsole].State;
      if (ProgrammedDefaultSwitch[cntConsole].State)
      {
        ProgrammedDefaultSwitch[cntConsole].TimerValue = 0;
      }
    }
    else if (ProgrammedDefaultSwitch[cntConsole].State)
    {
      if (ProgrammedDefaultSwitch[cntConsole].TimerValue<3000)
      {
        ProgrammedDefaultSwitch[cntConsole].TimerValue += 10;
        if (ProgrammedDefaultSwitch[cntConsole].TimerValue == 1000)
        {
          db_lock(1);
          //module_configuration
          db_read_module_config(1, 128, cntConsole, 1);

          //buss_configuration
          db_read_buss_config(1, 16, cntConsole);

          //monitor_buss_configuration
          db_read_monitor_buss_config(1, 16, cntConsole);
          db_lock(0);

          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].Console == cntConsole)
            {
              AxumData.ModuleData[cntModule].WaitingSource = 0;
              AxumData.ModuleData[cntModule].WaitingProcessingPreset = 0;
              AxumData.ModuleData[cntModule].WaitingRoutingPreset = 0;
            }
          }

          int OldConsolePreset = AxumData.ConsoleData[cntConsole].SelectedConsolePreset;
          if (OldConsolePreset != 0)
          {
            unsigned int FunctionNrToSent = 0x04000000;
            AxumData.ConsoleData[cntConsole].SelectedConsolePreset = 0;
            CheckObjectsToSent(FunctionNrToSent | (GLOBAL_FUNCTION_CONSOLE_PRESET_1+OldConsolePreset-1));

            FunctionNrToSent = 0x03000000 | (cntConsole<<12);
            CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_CONSOLE_PRESET);
          }
        }
      }
    }
  }
  axum_data_lock(0);

  axum_data_lock(1);
  for (unsigned char cntConsole=0; cntConsole<4; cntConsole++)
  {
    if (AxumData.ConsoleData[cntConsole].ControlModeTimerValue<10000)
    {
      AxumData.ConsoleData[cntConsole].ControlModeTimerValue += 10;
      if (AxumData.ConsoleData[cntConsole].ControlModeTimerValue == 10000)
      { //set control mode to none
        unsigned int OldFunctionNumber = GetConsoleFunctionNrFromControlMode(cntConsole);
        AxumData.ConsoleData[cntConsole].ControlMode = -1;
        CheckObjectsToSent(OldFunctionNumber);

        for (int cntModule=0; cntModule<128; cntModule++)
        {
          if (AxumData.ModuleData[cntModule].Console == cntConsole)
          {
            DoAxum_UpdateModuleControlMode(cntModule, MODULE_CONTROL_MODE_NONE);
            DoAxum_UpdateModuleControlModeLabel(cntModule, MODULE_CONTROL_MODE_NONE);
          }
        }

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_CONTROL_MODE_ACTIVE);

        OldFunctionNumber = GetConsoleFunctionNrFromControlMode(cntConsole);
        AxumData.ConsoleData[cntConsole].ControlMode = -1;
        CheckObjectsToSent(OldFunctionNumber);

        for (int cntModule=0; cntModule<128; cntModule++)
        {
          if (AxumData.ModuleData[cntModule].Console == cntConsole)
          {
            DoAxum_UpdateModuleControlMode(cntModule, MODULE_CONTROL_MODE_NONE);
            DoAxum_UpdateModuleControlModeLabel(cntModule, MODULE_CONTROL_MODE_NONE);
          }
        }
        FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_CONTROL_MODE_ACTIVE);
      }
    }
    if (AxumData.ConsoleData[cntConsole].SelectedModuleTimeout>0)
    {
      AxumData.ConsoleData[cntConsole].SelectedModuleTimeout -= 10;
      if (AxumData.ConsoleData[cntConsole].SelectedModuleTimeout <= 0)
      {
        AxumData.ConsoleData[cntConsole].SelectedModuleTimeout = 0;

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MODULE_SELECT_ACTIVE);
      }
    }
    if (AxumData.ConsoleData[cntConsole].SelectedBussTimeout>0)
    {
      AxumData.ConsoleData[cntConsole].SelectedBussTimeout -= 10;
      if (AxumData.ConsoleData[cntConsole].SelectedBussTimeout <= 0)
      {
        AxumData.ConsoleData[cntConsole].SelectedBussTimeout = 0;

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_BUSS_SELECT_ACTIVE);
      }
    }
    if (AxumData.ConsoleData[cntConsole].SelectedMonitorBussTimeout>0)
    {
      AxumData.ConsoleData[cntConsole].SelectedMonitorBussTimeout -= 10;
      if (AxumData.ConsoleData[cntConsole].SelectedMonitorBussTimeout <= 0)
      {
        AxumData.ConsoleData[cntConsole].SelectedMonitorBussTimeout = 0;

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MONITOR_BUSS_SELECT_ACTIVE);
      }
    }
    if (AxumData.ConsoleData[cntConsole].SelectedSourceTimeout>0)
    {
      AxumData.ConsoleData[cntConsole].SelectedSourceTimeout -= 10;
      if (AxumData.ConsoleData[cntConsole].SelectedSourceTimeout <= 0)
      {
        AxumData.ConsoleData[cntConsole].SelectedSourceTimeout = 0;

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_SOURCE_SELECT_ACTIVE);
      }
    }
    if (AxumData.ConsoleData[cntConsole].SelectedDestinationTimeout>0)
    {
      AxumData.ConsoleData[cntConsole].SelectedDestinationTimeout -= 10;
      if (AxumData.ConsoleData[cntConsole].SelectedDestinationTimeout <= 0)
      {
        AxumData.ConsoleData[cntConsole].SelectedDestinationTimeout = 0;

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_DESTINATION_SELECT_ACTIVE);
      }
    }
  }
  axum_data_lock(0);

  //Check if all nodes are initialized
  node_info_lock(1);
  //We may only start from the first element because multiple threads
  InitializedNodes = 0;
  NodeCount = 0;
  ONLINE_NODE_INFORMATION_STRUCT *TimerWalkOnlineNodeInformationElement = OnlineNodeInformationList;
  while (TimerWalkOnlineNodeInformationElement != NULL)
  {
    if (TimerWalkOnlineNodeInformationElement->InitializationFinished)
    {
      InitializedNodes++;
    }
    NodeCount++;
    TimerWalkOnlineNodeInformationElement = TimerWalkOnlineNodeInformationElement->Next;
  }
  node_info_lock(0);

  axum_data_lock(1);
  if ((PreviousInitializedNodes != InitializedNodes) ||
      (PreviousNodeCount != NodeCount))
  {
    AxumData.PercentInitialized = (100*InitializedNodes)/NodeCount;
    log_write("%d/%d nodes, %d%% initialized", InitializedNodes, NodeCount, AxumData.PercentInitialized);
    unsigned int FunctionNrToSent = 0x04000000;
    CheckObjectsToSent(FunctionNrToSent | GLOBAL_FUNCTION_INITIALIZATION_STATUS);

    if (AxumData.PercentInitialized == 100)
    {
      log_write("Initialization ready!");
      PreviousCount_LevelMeter = cntMillisecondTimer;
      PreviousCount_SignalDetect = cntMillisecondTimer;
      PreviousCount_PhaseMeter = cntMillisecondTimer;
    }

    PreviousInitializedNodes = InitializedNodes;
    PreviousNodeCount = NodeCount;
  }
  axum_data_lock(0);

  if (use_eth)
  {
    if ((LinkStatus = mbnEthernetMIILinkStatus(mbn->itf, error)) == -1)
    {
    }
    else if (CurrentLinkStatus != LinkStatus)
    {
      log_write("Link %s", LinkStatus ? "up" : "down");
      CurrentLinkStatus = LinkStatus;
    }
  }

  Value = 0;
}

int delay_ms(double sleep_time)
{
  struct timespec tv;

  sleep_time /= 1000;

  /* Construct the timespec from the number of whole seconds... */
  tv.tv_sec = (time_t) sleep_time;
  /* ... and the remainder in nanoseconds. */
  tv.tv_nsec = (long) ((sleep_time - tv.tv_sec) * 1e+9);

  while (1)
  {
    /* Sleep for the time specified in tv. If interrupted by a signal, place the remaining time left to sleep back into tv. */
    int rval = nanosleep (&tv, &tv);
    if (rval == 0)
    {
      /* Completed the entire sleep time; all done. */
      return 0;
    }
    else if (errno == EINTR)
    {
      /* Interrupted by a signal. Try again. */
      continue;
    }
    else
    {
      /* Some other error; bail out. */
      return rval;
    }
  }
  return 0;
}

int delay_us(double sleep_time)
{
  struct timespec tv;

  sleep_time /= 1000000;

  /* Construct the timespec from the number of whole seconds... */
  tv.tv_sec = (time_t) sleep_time;
  /* ... and the remainder in nanoseconds. */
  tv.tv_nsec = (long) ((sleep_time - tv.tv_sec) * 1e+9);

  while (1)
  {
    /* Sleep for the time specified in tv. If interrupted by a signal, place the remaining time left to sleep back into tv. */
    int rval = nanosleep (&tv, &tv);
    if (rval == 0)
    {
      /* Completed the entire sleep time; all done. */
      return 0;
    }
    else if (errno == EINTR)
    {
      /* Interrupted by a signal. Try again. */
      continue;
    }
    else
    {
      /* Some other error; bail out. */
      return rval;
    }
  }
  return 0;
}

float CalculateEQ(float *Coefficients, float Gain, int Frequency, float Bandwidth, float Slope, FilterType Type)
{
  double a0=1, a1=0 ,a2=0; //<- Zero coefficients
  double b0=1, b1=0 ,b2=0; //<- Pole coefficients
  unsigned char Zolzer = 1;
  unsigned int FSamplerate = AxumData.Samplerate;

  if (!Zolzer)
  {
    double A = pow(10, (double)Gain/40);
    double Omega = (2*M_PI*Frequency)/FSamplerate;
    double Cs = cos(Omega);
    double Sn = sin(Omega);
    double Q = Sn/(log(2)*Bandwidth*Omega);
    double Alpha;

    if (Type==PEAKINGEQ)
    {
      Alpha = Sn*sinhl(1/(2*Q))*2*pow(10, (double)abs(Gain)/40);
    }
    else
    {
      Alpha = Sn*sinhl(1/(2*Q));
    }

    switch (Type)
    {
    case OFF:
    {
      a0 = 1;
      a1 = 0;
      a2 = 0;

      b0 = 1;
      b1 = 0;
      b2 = 0;
    }
    break;
    case LPF:
    {// LPF
      a0 = (1 - Cs)/2;
      a1 = 1 - Cs;
      a2 = (1 - Cs)/2;

      b0 = 1 + Alpha;
      b1 = -2*Cs;
      b2 = 1 - Alpha;
    }
    break;
    case HPF:
    {// HPF
      a0 = (1 + Cs)/2;
      a1 = -1 - Cs;
      a2 = (1 + Cs)/2;

      b0 = 1 + Alpha;
      b1 = -2*Cs;
      b2 = 1 - Alpha;
    }
    break;
    case BPF:
    {// BPF
      a0 = Alpha;
      a1 = 0;
      a2 = -Alpha;

      b0 = 1 + Alpha;
      b1 = -2*Cs;
      b2 = 1 - Alpha;
    }
    break;
    case NOTCH:
    {// notch
      if (Gain<0)
      {
        a0 = 1 + pow(10, (double)Gain/20)*Sn*sinh(1/(2*Q));
        a1 = -2*Cs;
        a2 = 1 - pow(10, (double)Gain/20)*Sn*sinh(1/(2*Q));

        b0 = 1 + Sn*sinh(1/(2*Q));
        b1 = -2*Cs;
        b2 = 1 - Sn*sinh(1/(2*Q));
      }
      else
      {
        a0 = 1 + Sn*sinh(1/(2*Q));
        a1 = -2*Cs;
        a2 = 1 - Sn*sinh(1/(2*Q));

        b0 = 1 + pow(10, (double)-Gain/20)*Sn*sinh(1/(2*Q));
        b1 = -2*Cs;
        b2 = 1 - pow(10, (double)-Gain/20)*Sn*sinh(1/(2*Q));
      }
    }
    break;
    case PEAKINGEQ:
    {   //Peaking EQ
      a0 = 1 + Alpha*A;
      a1 = -2*Cs;
      a2 = 1 - Alpha*A;

      b0 = 1 + Alpha/A;
      b1 = -2*Cs;
      b2 = 1 - Alpha/A;
    }
    break;
    case LOWSHELF:
    {// lowShelf
      a0 =   A*((A+1) - ((A-1)*Cs));// + (Beta*Sn));
      a1 = 2*A*((A-1) - ((A+1)*Cs));
      a2 =   A*((A+1) - ((A-1)*Cs));// - (Beta*Sn));

      b0 =      (A+1) + ((A-1)*Cs);// + (Beta*Sn);
      b1 =    -2*((A-1) + ((A+1)*Cs));
      b2 =         (A+1) + ((A-1)*Cs);// - (Beta*Sn);
    }
    break;
    case HIGHSHELF:
    {// highShelf
      a0 =    A*((A+1) + ((A-1)*Cs));// + (Beta*Sn));
      a1 = -2*A*((A-1) + ((A+1)*Cs));
      a2 =    A*((A+1) + ((A-1)*Cs));// - (Beta*Sn));

      b0 =      (A+1) - ((A-1)*Cs);// + (Beta*Sn);
      b1 =     2*((A-1) - ((A+1)*Cs));
      b2 =         (A+1) - ((A-1)*Cs);// - (Beta*Sn);
    }
    break;
    }
  }
  else
  {
    double Omega = (2*M_PI*Frequency)/FSamplerate;
    double Cs = cos(Omega);
    double Sn = sin(Omega);
    double Q = Sn/(log(2)*Bandwidth*Omega);
    double Alpha;

    if (Type==PEAKINGEQ)
    {
      Alpha = Sn*sinhl(1/(2*Q))*2*pow(10, (double)abs(Gain)/40);
    }
    else
    {
      Alpha = Sn*sinhl(1/(2*Q));
    }

    double K = tan(Omega/2);
    switch (Type)
    {
    case OFF:
    {
      a0 = 1;
      a1 = 0;
      a2 = 0;

      b0 = 1;
      b1 = 0;
      b2 = 0;
    }
    break;
    case LPF:
    {
      a0 = (K*K)/(1+sqrt(2)*K+(K*K));
      a1 = (2*(K*K))/(1+sqrt(2)*K+(K*K));
      a2 = (K*K)/(1+sqrt(2)*K+(K*K));

      b0 = 1;
      b1 = (2*((K*K)-1))/(1+sqrt(2)*K+(K*K));
      b2 = (1-sqrt(2)*K+(K*K))/(1+sqrt(2)*K+(K*K));
    }
    break;
    case HPF:
    {// HPF
      a0 = 1/(1+sqrt(2)*K+(K*K));
      a1 = -2/(1+sqrt(2)*K+(K*K));
      a2 = 1/(1+sqrt(2)*K+(K*K));

      b0 = 1;
      b1 = (2*((K*K)-1))/(1+sqrt(2)*K+(K*K));
      b2 = (1-sqrt(2)*K+(K*K))/(1+sqrt(2)*K+(K*K));
    }
    break;
    case BPF:
    {// BPF
      a0 = Alpha;
      a1 = 0;
      a2 = -Alpha;

      b0 = 1 + Alpha;
      b1 = -2*Cs;
      b2 = 1 - Alpha;
    }
    break;
    case NOTCH:
    {// notch
      if (Gain<0)
      {
        a0 = 1+ pow(10,(double)Gain/20)*Alpha;
        a1 = -2*Cs;
        a2 = 1- pow(10,(double)Gain/20)*Alpha;

        b0 = 1 + Alpha;
        b1 = -2*Cs;
        b2 = 1 - Alpha;
      }
      else
      {
        double A = pow(10,(double)Gain/20);

        a0 = (1+((A*K)/Q)+(K*K))/(1+(K/Q)+(K*K));
        a1 = (2*((K*K)-1))/(1+(K/Q)+(K*K));
        a2 = (1-((A*K)/Q)+(K*K))/(1+(K/Q)+(K*K));

        b0 = 1;
        b1 = (2*((K*K)-1))/(1+(K/Q)+(K*K));
        b2 = (1-(K/Q)+(K*K))/(1+(K/Q)+(K*K));
      }
    }
    break;
    case PEAKINGEQ:
    {   //Peaking EQ
      if (Gain>0)
      {
        float A = pow(10,(double)Gain/20);
        a0 = (1+((A*K)/Q)+(K*K))/(1+(K/Q)+(K*K));
        a1 = (2*((K*K)-1))/(1+(K/Q)+(K*K));
        a2 = (1-((A*K)/Q)+(K*K))/(1+(K/Q)+(K*K));

        b0 = 1;
        b1 = (2*((K*K)-1))/(1+(K/Q)+(K*K));
        b2 = (1-(K/Q)+(K*K))/(1+(K/Q)+(K*K));
      }
      else
      {
        float A = pow(10,(double)-Gain/20);

        a0 = (1+(K/Q)+(K*K))/(1+((A*K)/Q)+(K*K));
        a1 = (2*((K*K)-1))/(1+((A*K)/Q)+(K*K));
        a2 = (1-(K/Q)+(K*K))/(1+((A*K)/Q)+(K*K));

        b0 = 1;
        b1 = (2*((K*K)-1))/(1+((A*K)/Q)+(K*K));
        b2 = (1-((A*K)/Q)+(K*K))/(1+((A*K)/Q)+(K*K));
      }
    }
    break;
    case LOWSHELF:
    {// lowShelf
      if (Gain>0)
      {
        double A = pow(10,(double)Gain/20);
        a0 = (1+(sqrt(2*A*Slope)*K)+(A*K*K))/(1+(sqrt(2*Slope)*K)+(K*K));
        a1 = (2*((A*K*K)-1))/(1+(sqrt(2*Slope)*K)+(K*K));
        a2 = (1-(sqrt(2*A*Slope)*K)+(A*K*K))/(1+(sqrt(2*Slope)*K)+(K*K));

        b0 = 1;
        b1 = (2*((K*K)-1))/(1+(sqrt(2*Slope)*K)+(K*K));
        b2 = (1-(sqrt(2*Slope)*K)+(K*K))/(1+(sqrt(2*Slope)*K)+(K*K));
      }
      else
      {
        double A = pow(10,(double)-Gain/20);
        a0 = (1+(sqrt(2*Slope)*K)+(K*K))/(1+(sqrt(2*A*Slope)*K)+(A*K*K));
        a1 = (2*((K*K)-1))/(1+(sqrt(2*A*Slope)*K)+(A*K*K));
        a2 = (1-(sqrt(2*Slope)*K)+(K*K))/(1+(sqrt(2*A*Slope)*K)+(A*K*K));

        b0 = 1;
        b1 = (2*((A*K*K)-1))/(1+(sqrt(2*A*Slope)*K)+(A*K*K));
        b2 = (1-(sqrt(2*A*Slope)*K)+(A*K*K))/(1+(sqrt(2*A*Slope)*K)+(A*K*K));
      }
    }
    break;
    case HIGHSHELF:
    {// highShelf
      if (Gain>0)
      {
        double A = pow(10,(double)Gain/20);
        a0 = (A+(sqrt(2*A*Slope)*K)+(K*K))/(1+(sqrt(2*Slope)*K)+(K*K));
        a1 = (2*((K*K)-A))/(1+(sqrt(2*Slope)*K)+(K*K));
        a2 = (A-(sqrt(2*A*Slope)*K)+(K*K))/(1+(sqrt(2*Slope)*K)+(K*K));

        b0 = 1;
        b1 = (2*((K*K)-1))/(1+(sqrt(2*Slope)*K)+(K*K));
        b2 = (1-(sqrt(2*Slope)*K)+(K*K))/(1+(sqrt(2*Slope)*K)+(K*K));
      }
      else
      {
        double A = pow(10,(double)-Gain/20);
        a0 = (1+(sqrt(2*Slope)*K)+(K*K))/(A+(sqrt(2*A*Slope)*K)+(K*K));
        a1 = (2*((K*K)-1))/(A+(sqrt(2*A*Slope)*K)+(K*K));
        a2 = (1-(sqrt(2*Slope)*K)+(K*K))/(A+(sqrt(2*A*Slope)*K)+(K*K));

        b0 = 1;
        b1 = (2*(((K*K)/A)-1))/(1+(sqrt((2*Slope)/A)*K)+((K*K)/A));
        b2 = (1-(sqrt((2*Slope)/A)*K)+((K*K)/A))/(1+(sqrt((2*Slope)/A)*K)+((K*K)/A));
      }
    }
    break;
    }
  }

  Coefficients[0] = a0;
  Coefficients[1] = a1;
  Coefficients[2] = a2;
  Coefficients[3] = b0;
  Coefficients[4] = b1;
  Coefficients[5] = b2;

  return a0/b0;
}

void SetBackplaneRouting(unsigned int FormInputNr, unsigned int ChannelNr)
{
  int ObjectNr = 1032+ChannelNr;

  if (BackplaneMambaNetAddress != 0x00000000)
  {
    mbn_data data;

    data.UInt = FormInputNr;
    mbnSetActuatorData(mbn, BackplaneMambaNetAddress, ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
  }
}

void SetBackplaneClock()
{
  mbn_data data;
  int ObjectNr = 1030;
  data.State = AxumData.ExternClock;

  if (BackplaneMambaNetAddress != 0x00000000)
  {
    mbnSetActuatorData(mbn, BackplaneMambaNetAddress, ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
  }

  ObjectNr = 1024;
  switch (AxumData.Samplerate)
  {
    case 32000:
    {
      data.State = 0;
    }
    break;
    case 44100:
    {
      data.State = 1;
    }
    break;
    default:
    {
      data.State = 2;
    }
    break;
  }

  if (BackplaneMambaNetAddress != 0x00000000)
  {
    mbnSetActuatorData(mbn, BackplaneMambaNetAddress, ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
  }
}

void SetAxum_EQ(unsigned char ModuleNr, unsigned char BandNr)
{
  unsigned int SystemChannelNr = ModuleNr<<1;
  unsigned char DSPCardNr = (SystemChannelNr/64);
  unsigned char DSPCardChannelNr = SystemChannelNr%64;

  DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

  for (int cntChannel=0; cntChannel<2; cntChannel++)
  {
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].EQBand[BandNr].On = AxumData.ModuleData[ModuleNr].EQOnOff;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].EQBand[BandNr].Level = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].EQBand[BandNr].Frequency = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].EQBand[BandNr].Bandwidth = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].EQBand[BandNr].Slope  = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Slope;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].EQBand[BandNr].Type = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type;
  }

  for (int cntChannel=0; cntChannel<2; cntChannel++)
  {
    dsp_set_eq(dsp_handler, SystemChannelNr+cntChannel, BandNr);
  }
}

void SetAxum_ModuleProcessing(unsigned int ModuleNr)
{
  unsigned int SystemChannelNr = ModuleNr<<1;
  unsigned char DSPCardNr = (SystemChannelNr/64);
  unsigned char DSPCardChannelNr = SystemChannelNr%64;

  DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

  //A = Own channel
  dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputALevel = 0;
  dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputBLevel = -140;
  //A = Paired channel
  dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputALevel = 0;
  dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputBLevel = -140;
  if (AxumData.ModuleData[ModuleNr].MonoOnOff)
  {
    switch (AxumData.ModuleData[ModuleNr].Mono)
    {
      case 1:
      {
        //A = Own channel
        dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputALevel = 0;
        dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputBLevel = -140;
        //A = Paired channel
        dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputALevel = -140;
        dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputBLevel = 0;
      }
      break;
      case 2:
      {
        //A = Own channel
        dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputALevel = -140;
        dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputBLevel = 0;
        //A = Paired channel
        dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputALevel = 0;
        dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputBLevel = -140;
      }
      break;
      case 3:
      {
        //A = Own channel
        dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputALevel = -6;
        dspcard->data.ChannelData[DSPCardChannelNr+0].MonoInputBLevel = -6;
        //A = Paired channel
        dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputALevel = -6;
        dspcard->data.ChannelData[DSPCardChannelNr+1].MonoInputBLevel = -6;
      }
      break;
    }
  }

  for (int cntChannel=0; cntChannel<2; cntChannel++)
  {
    if (AxumData.ModuleData[ModuleNr].InsertSource>0)
    { // only turn on the insert if there is a source.
      dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Insert = AxumData.ModuleData[ModuleNr].InsertOnOff;
    }
    else
    {
      dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Insert = 0;
    }
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Gain = AxumData.ModuleData[ModuleNr].Gain;

    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Filter.On = AxumData.ModuleData[ModuleNr].FilterOnOff;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Filter.Level = AxumData.ModuleData[ModuleNr].Filter.Level;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Filter.Frequency = AxumData.ModuleData[ModuleNr].Filter.Frequency;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Filter.Bandwidth = AxumData.ModuleData[ModuleNr].Filter.Bandwidth;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Filter.Slope = AxumData.ModuleData[ModuleNr].Filter.Slope;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Filter.Type = AxumData.ModuleData[ModuleNr].Filter.Type;



    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Dynamics.Percent = 100-(100/pow(2.715, (log(AxumData.ModuleData[ModuleNr].AGCRatio)/log(2))));
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Dynamics.On = AxumData.ModuleData[ModuleNr].DynamicsOnOff;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Dynamics.Threshold = AxumData.ModuleData[ModuleNr].AGCThreshold;
    dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Dynamics.DownwardExpanderThreshold = AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold;
  }
  dspcard->data.ChannelData[DSPCardChannelNr+0].PhaseReverse = AxumData.ModuleData[ModuleNr].PhaseOnOff && (AxumData.ModuleData[ModuleNr].Phase&0x01);
  dspcard->data.ChannelData[DSPCardChannelNr+1].PhaseReverse = AxumData.ModuleData[ModuleNr].PhaseOnOff && (AxumData.ModuleData[ModuleNr].Phase&0x02);

  for (int cntChannel=0; cntChannel<2; cntChannel++)
  {
    dsp_set_ch(dsp_handler, SystemChannelNr+cntChannel);
  }
}

void SetAxum_BussLevels(unsigned int ModuleNr)
{
  unsigned int SystemChannelNr = ModuleNr<<1;
  unsigned char DSPCardNr = (SystemChannelNr/64);
  unsigned char DSPCardChannelNr = SystemChannelNr%64;
  float PanoramadB[2] = {-200, -200};

  DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

  //Panorama
  float RightPos = AxumData.ModuleData[ModuleNr].Panorama;
  float LeftPos = 1023-RightPos;
  if (LeftPos>0)
  {
    PanoramadB[0] = 20*log10(LeftPos/1023);
  }
  if (RightPos>0)
  {
    PanoramadB[1] = 20*log10(RightPos/1023);
  }
  //Balance
  if ((LeftPos>0) && (LeftPos<512))
  {
    PanoramadB[0] = 20*log10(LeftPos/512);
  }
  else if (LeftPos>=512)
  {
    PanoramadB[0] = 0;
  }

  if ((RightPos>0) && (RightPos<512))
  {
    PanoramadB[1] = 20*log10(RightPos/512);
  }
  else if (RightPos>=512)
  {
    PanoramadB[1] = 0;
  }

  //Buss 1-16
  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    float BussBalancedB[2] = {-200, -200};

    //Buss balance
    float RightPos = AxumData.ModuleData[ModuleNr].Buss[cntBuss].Balance;
    float LeftPos = 1023-RightPos;
    if (LeftPos>0)
    {
      BussBalancedB[0] = 20*log10(LeftPos/1023);
    }
    if (RightPos>0)
    {
      BussBalancedB[1] = 20*log10(RightPos/1023);
    }
    //Balance
    if ((LeftPos>0) && (LeftPos<512))
    {
      BussBalancedB[0] = 20*log10(LeftPos/512);
    }
    else if (LeftPos>=512)
    {
      BussBalancedB[0] = 0;
    }

    if ((RightPos>0) && (RightPos<512))
    {
      BussBalancedB[1] = 20*log10(RightPos/512);
    }
    else if (RightPos>=512)
    {
      BussBalancedB[1] = 0;
    }

    for (int cntChannel=0; cntChannel<2; cntChannel++)
    {
      dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].Level = -140; //0 dB?
      dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].Level = -140; //0 dB?
      dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].On = 0;
      dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].On = 0;

      if (AxumData.ModuleData[ModuleNr].Buss[cntBuss].Assigned)
      {
        if (AxumData.BussMasterData[cntBuss].Mono)
        {
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].On = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].On = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].Level = AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].Level = AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].Level += BussBalancedB[0];
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].Level += BussBalancedB[1];
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].Level += -6;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].Level += -6;

          if (!AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreModuleLevel)
          {
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].Level += AxumData.ModuleData[ModuleNr].FaderLevel;
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].Level += AxumData.ModuleData[ModuleNr].FaderLevel;
          }
          if (!AxumData.BussMasterData[cntBuss].PreModuleBalance)
          {
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].Level += PanoramadB[0];
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].Level += PanoramadB[1];
          }
          if (!AxumData.BussMasterData[cntBuss].PreModuleOn)
          {
            if (!AxumData.ModuleData[ModuleNr].On)
            {
              dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].On = 0;
              dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].On = 0;
            }
          }
          if ((AxumData.ModuleData[ModuleNr].Cough) && (AxumData.BussMasterData[cntBuss].Exclusive<2))
          {
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+0].On = 0;
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+1].On = 0;
          }
        }
        else
        { //Stereo
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].On = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].Level = AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level;
          dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].Level += BussBalancedB[cntChannel];

          if (!AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreModuleLevel)
          {
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].Level += AxumData.ModuleData[ModuleNr].FaderLevel;
          }

          if (!AxumData.BussMasterData[cntBuss].PreModuleBalance)
          {
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].Level += PanoramadB[cntChannel];
          }

          if (!AxumData.BussMasterData[cntBuss].PreModuleOn)
          {
            if (!AxumData.ModuleData[ModuleNr].On)
            {
              dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].On = 0;
            }
          }
          if ((AxumData.ModuleData[ModuleNr].Cough) && (AxumData.BussMasterData[cntBuss].Exclusive<2))
          {
            dspcard->data.ChannelData[DSPCardChannelNr+cntChannel].Buss[(cntBuss*2)+cntChannel].On = 0;
          }
        }
      }
    }
  }

  for (int cntChannel=0; cntChannel<2; cntChannel++)
  {
    dsp_set_buss_lvl(dsp_handler, SystemChannelNr+cntChannel);
  }
}

void axum_get_mtrx_chs_from_src(int src, unsigned int *l_ch, unsigned int *r_ch)
{
  *l_ch = 0;
  *r_ch = 0;
  if ((src>=matrix_sources.src_offset.min.buss) && (src<=matrix_sources.src_offset.max.buss))
  {
    int BussNr = src-matrix_sources.src_offset.min.buss;
    *l_ch = 1793+(BussNr*2)+0;
    *r_ch = 1793+(BussNr*2)+1;
  }
  else if ((src>=matrix_sources.src_offset.min.insert_out) && (src<=matrix_sources.src_offset.max.insert_out))
  {
    int ModuleNr = src-matrix_sources.src_offset.min.insert_out;

    unsigned char DSPCardNr = (ModuleNr/32);
    if (dsp_card_available(dsp_handler, DSPCardNr))
    {
      DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

      unsigned int FirstDSPChannelNr = 481+(dspcard->slot*5*32);
      //Make sure channel nummers for a new DSP card start a 0 (e.g. Insert out 32 is the second DSP card, ch 0)
      *l_ch = FirstDSPChannelNr+((ModuleNr-(32*DSPCardNr))*2)+0;
      *r_ch = FirstDSPChannelNr+((ModuleNr-(32*DSPCardNr))*2)+1;
    }
    else
    {
      log_write("[axum_get_mtrx_chs_from_src] Module insert out %d muted, no DSP Card\n", ModuleNr+1);
    }
  }
  else if ((src>=matrix_sources.src_offset.min.monitor_buss) && (src<=matrix_sources.src_offset.max.monitor_buss))
  {
    int BussNr = src-matrix_sources.src_offset.min.monitor_buss;

    unsigned char DSPCardNr = (BussNr/4);

    if (dsp_card_available(dsp_handler, DSPCardNr))
    {
      DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

      unsigned int FirstDSPChannelNr = 577+(dspcard->slot*5*32);
      //Make sure channel nummers for a new DSP card start a 0 (e.g. Monitor buss 4 is the second DSP card, ch 0)
      *l_ch = FirstDSPChannelNr+((BussNr-(DSPCardNr*4))*2)+0;
      *r_ch = FirstDSPChannelNr+((BussNr-(DSPCardNr*4))*2)+1;
    }
    else
    {
      log_write("[axum_get_mtrx_chs_from_src] Monitor buss %d muted, no DSP Card\n", BussNr+1);
    }
  }
  else if ((src>=matrix_sources.src_offset.min.mixminus) && (src<=matrix_sources.src_offset.max.mixminus))
  {
    int ModuleNr = src-matrix_sources.src_offset.min.mixminus;

    unsigned char DSPCardNr = (ModuleNr/32);
    if (dsp_card_available(dsp_handler, DSPCardNr))
    {
      DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

      unsigned int FirstDSPChannelNr = 545+(dspcard->slot*5*32);

      //N-1 are mono's.
      //Make sure channel nummers for a new DSP card start a 0 (e.g. Mix minus 32 is the second DSP card, ch 0)
      *l_ch = FirstDSPChannelNr+(ModuleNr-(DSPCardNr*32));
      *r_ch = FirstDSPChannelNr+(ModuleNr-(DSPCardNr*32));
    }
    else
    {
      log_write("[axum_get_mtrx_chs_from_src] Module N-1 %d muted, no DSP Card\n", ModuleNr+1);
    }
  }
  else if ((src>=matrix_sources.src_offset.min.source) && (src<=matrix_sources.src_offset.max.source))
  {
    int SourceNr = src-matrix_sources.src_offset.min.source;
    char SourceFound = 0;

    //Get slot number from MambaNet Address
    for (int cntSlot=0; cntSlot<15; cntSlot++)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.SourceData[SourceNr].InputData[0].MambaNetAddress)
      {
        *l_ch = cntSlot*32;
        SourceFound=1;
      }
      if (AxumData.RackOrganization[cntSlot] == AxumData.SourceData[SourceNr].InputData[1].MambaNetAddress)
      {
        *r_ch = cntSlot*32;
        SourceFound=1;
      }
    }
    for (int cntSlot=15; cntSlot<19; cntSlot++)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.SourceData[SourceNr].InputData[0].MambaNetAddress)
      {
        *l_ch = 480+((cntSlot-15)*32*5);
        SourceFound=1;
      }
      if (AxumData.RackOrganization[cntSlot] == AxumData.SourceData[SourceNr].InputData[1].MambaNetAddress)
      {
        *r_ch = 480+((cntSlot-15)*32*5);
        SourceFound=1;
      }
    }
    for (int cntSlot=21; cntSlot<42; cntSlot++)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.SourceData[SourceNr].InputData[0].MambaNetAddress)
      {
        *l_ch = 1120+((cntSlot-21)*32);
        SourceFound=1;
      }
      if (AxumData.RackOrganization[cntSlot] == AxumData.SourceData[SourceNr].InputData[1].MambaNetAddress)
      {
        *r_ch = 1120+((cntSlot-21)*32);
        SourceFound=1;
      }
    }

    if (SourceFound)
    {
      *l_ch += AxumData.SourceData[SourceNr].InputData[0].SubChannel;
      *r_ch += AxumData.SourceData[SourceNr].InputData[1].SubChannel;

      //Because 0 = mute, add one per channel
      *l_ch += 1;
      *r_ch += 1;
    }

    //MambaNet address == 0, force mute!
    if (AxumData.SourceData[SourceNr].InputData[0].MambaNetAddress == 0)
    {
      *l_ch = 0;
    }
    if (AxumData.SourceData[SourceNr].InputData[1].MambaNetAddress == 0)
    {
      *r_ch = 0;
    }
  }
}

void SetAxum_ModuleSource(unsigned int ModuleNr)
{
  unsigned int SystemChannelNr = ModuleNr<<1;
  unsigned char DSPCardNr = (SystemChannelNr/64);
  unsigned int ToChannelNr;
  unsigned int Input1, Input2;

  if (dsp_card_available(dsp_handler, DSPCardNr))
  {
    DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

    ToChannelNr = 480+(dspcard->slot*5*32)+(SystemChannelNr%64);

    axum_get_mtrx_chs_from_src(AxumData.ModuleData[ModuleNr].SelectedSource, &Input1, &Input2);

    SetBackplaneRouting(Input1, ToChannelNr+0);
    SetBackplaneRouting(Input2, ToChannelNr+1);
  }
}

void SetAxum_ExternSources(unsigned int MonitorBussPerFourNr)
{
  unsigned char DSPCardNr = MonitorBussPerFourNr;
  if (dsp_card_available(dsp_handler, DSPCardNr))
  {
    DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

    //four stereo busses
    for (int cntExt=0; cntExt<8; cntExt++)
    {
      unsigned int ToChannelNr = 480+(dspcard->slot*5*32)+128+(cntExt*2);
      unsigned int Input1, Input2;

      axum_get_mtrx_chs_from_src(AxumData.ExternSource[MonitorBussPerFourNr].Ext[cntExt], &Input1, &Input2);

      SetBackplaneRouting(Input1, ToChannelNr+0);
      SetBackplaneRouting(Input2, ToChannelNr+1);
    }
  }
}

void SetAxum_ModuleMixMinus(unsigned int ModuleNr, int OldSource)
{
  int cntDestination=0;
  int DestinationNr = -1;


  while (cntDestination<1280)
  {
    if ((OldSource != 0) && (AxumData.DestinationData[cntDestination].MixMinusSource == OldSource))
    {
      DestinationNr = cntDestination;

      if ((OldSource>=matrix_sources.src_offset.min.source) && (OldSource<=matrix_sources.src_offset.max.source))
      {
        char RemoveMixMinus = 1;
        for (unsigned int cntModule=0; cntModule<128; cntModule++)
        {
          if (OldSource == AxumData.ModuleData[cntModule].SelectedSource)
          {
            if (ModuleNr != cntModule)
            {
              RemoveMixMinus = 0;
            }
          }
        }

        if (RemoveMixMinus)
        {
          //MixMinus need to be removed, use default source
          AxumData.DestinationData[DestinationNr].MixMinusActive = 0;
          SetAxum_DestinationSource(DestinationNr);
        }
      }
    }
    if ((AxumData.DestinationData[cntDestination].MixMinusSource == AxumData.ModuleData[ModuleNr].SelectedSource) &&
       ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source)))
    {
      DestinationNr = cntDestination;

      //MixMinus
      int BussToUse = -1;
      int ModuleToUse = -1;
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (AxumData.DestinationData[cntDestination].MixMinusSource == AxumData.ModuleData[cntModule].SelectedSource)
        {
          for (int cntBuss=0; cntBuss<16; cntBuss++)
          {
            if ((AxumData.ModuleData[cntModule].Buss[cntBuss].Assigned) && (AxumData.ModuleData[cntModule].Buss[cntBuss].On))
            {
              if (BussToUse == -1)
              {
                ModuleToUse = cntModule;
                BussToUse = cntBuss;
              }
            }
          }
        }
      }

      if (BussToUse != -1)
      {
        unsigned int SystemChannelNr = ModuleToUse<<1;
        unsigned char DSPCardNr = (SystemChannelNr/64);
        unsigned char DSPCardChannelNr = SystemChannelNr%64;

        DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

        //Use buss 'BussToUse', for MixMinus at module 'ModuleToUse'

        dspcard->data.MixMinusData[DSPCardChannelNr+0].Buss = (BussToUse<<1)+0;
        dspcard->data.MixMinusData[DSPCardChannelNr+1].Buss = (BussToUse<<1)+1;

        for (int cntChannel=0; cntChannel<2; cntChannel++)
        {
          dsp_set_mixmin(dsp_handler, SystemChannelNr+cntChannel);
        }

        AxumData.DestinationData[DestinationNr].MixMinusActive = 1;
        SetAxum_DestinationSource(DestinationNr);
      }
      else
      {
        //No buss routed, use default source

        AxumData.DestinationData[DestinationNr].MixMinusActive = 0;
        SetAxum_DestinationSource(DestinationNr);
      }
    }
    cntDestination++;
  }
}

void SetAxum_ModuleInsertSource(unsigned int ModuleNr)
{
  unsigned int SystemChannelNr = ModuleNr<<1;
  unsigned char DSPCardNr = (SystemChannelNr/64);

  if (dsp_card_available(dsp_handler, DSPCardNr))
  {
    DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

    unsigned int ToChannelNr = 480+64+(dspcard->slot*5*32)+(SystemChannelNr%64);
    unsigned int Input1, Input2;

    axum_get_mtrx_chs_from_src(AxumData.ModuleData[ModuleNr].InsertSource, &Input1, &Input2);

    SetBackplaneRouting(Input1, ToChannelNr+0);
    SetBackplaneRouting(Input2, ToChannelNr+1);
  }
}

void SetAxum_RemoveOutputRouting(unsigned int OutputMambaNetAddress, unsigned char OutputSubChannel)
{
  int Output = -1;

  //Get slot number from MambaNet Address
  for (int cntSlot=0; cntSlot<15; cntSlot++)
  {
    if (OutputMambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == OutputMambaNetAddress)
      {
        Output = cntSlot*32;
      }
    }
  }
  for (int cntSlot=15; cntSlot<19; cntSlot++)
  {
    if (OutputMambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == OutputMambaNetAddress)
      {
        Output = 480+((cntSlot-15)*32*5);
      }
    }
  }
  for (int cntSlot=21; cntSlot<42; cntSlot++)
  {
    if (OutputMambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == OutputMambaNetAddress)
      {
        Output = 1120+((cntSlot-21)*32);
      }
    }
  }

  if (Output != -1)
  {
    Output += OutputSubChannel;
  }

  if (Output>-1)
  {
    SetBackplaneRouting(0, Output);
  }
}

void SetAxum_DestinationSource(unsigned int DestinationNr)
{
  int Output1 = -1;
  int Output2 = -1;
  unsigned int FromChannel1 = 0;
  unsigned int FromChannel2 = 0;

  //Get slot number from MambaNet Address
  for (int cntSlot=0; cntSlot<15; cntSlot++)
  {
    if (AxumData.DestinationData[DestinationNr].OutputData[0].MambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.DestinationData[DestinationNr].OutputData[0].MambaNetAddress)
      {
        Output1 = cntSlot*32;
      }
    }
    if (AxumData.DestinationData[DestinationNr].OutputData[1].MambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.DestinationData[DestinationNr].OutputData[1].MambaNetAddress)
      {
        Output2 = cntSlot*32;
      }
    }
  }
  for (int cntSlot=15; cntSlot<19; cntSlot++)
  {
    if (AxumData.DestinationData[DestinationNr].OutputData[0].MambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.DestinationData[DestinationNr].OutputData[0].MambaNetAddress)
      {
        Output1 = 480+((cntSlot-15)*32*5);
      }
    }
    if (AxumData.DestinationData[DestinationNr].OutputData[1].MambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.DestinationData[DestinationNr].OutputData[1].MambaNetAddress)
      {
        Output2 = 480+((cntSlot-15)*32*5);
      }
    }
  }
  for (int cntSlot=21; cntSlot<42; cntSlot++)
  {
    if (AxumData.DestinationData[DestinationNr].OutputData[0].MambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.DestinationData[DestinationNr].OutputData[0].MambaNetAddress)
      {
        Output1 = 1120+((cntSlot-21)*32);
      }
    }
    if (AxumData.DestinationData[DestinationNr].OutputData[1].MambaNetAddress)
    {
      if (AxumData.RackOrganization[cntSlot] == AxumData.DestinationData[DestinationNr].OutputData[1].MambaNetAddress)
      {
        Output2 = 1120+((cntSlot-21)*32);
      }
    }
  }

  if (Output1 != -1)
  {
    Output1 += AxumData.DestinationData[DestinationNr].OutputData[0].SubChannel;
  }
  if (Output2 != -1)
  {
    Output2 += AxumData.DestinationData[DestinationNr].OutputData[1].SubChannel;
  }

  //N-1
  if ((Output1>-1) || (Output2>-1))
  {
    if ((AxumData.DestinationData[DestinationNr].MixMinusSource != 0) && (AxumData.DestinationData[DestinationNr].MixMinusActive))
    {
      int cntModule=0;
      int MixMinusNr = -1;
      while ((MixMinusNr == -1) && (cntModule<128))
      {
        if ((AxumData.ModuleData[cntModule].SelectedSource == AxumData.DestinationData[DestinationNr].MixMinusSource))
        {
          for (int cntBuss=0; cntBuss<16; cntBuss++)
          {
            if ((AxumData.ModuleData[cntModule].Buss[cntBuss].Assigned) && (AxumData.ModuleData[cntModule].Buss[cntBuss].On))
            {
              MixMinusNr = cntModule;
            }
          }
        }
        cntModule++;
      }
      unsigned char DSPCardNr = (MixMinusNr/32);

      if (dsp_card_available(dsp_handler, DSPCardNr))
      {
        DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

        unsigned int FirstDSPChannelNr = 545+(dspcard->slot*5*32);

        FromChannel1 = FirstDSPChannelNr+(MixMinusNr&0x1F);
        FromChannel2 = FirstDSPChannelNr+(MixMinusNr&0x1F);
      }
      else
      {
        log_write("[SetAxum_DestinationSource] MixMinus %d muted, no DSP card", (MixMinusNr+1));
        FromChannel1 = 0;
        FromChannel2 = 0;
      }
    }
    else if (AxumData.DestinationData[DestinationNr].CommActive)
    {
      log_write("[SetAxum_DestinationSource] Communication buss %d", AxumData.DestinationData[DestinationNr].CommBuss);
      axum_get_mtrx_chs_from_src(AxumData.DestinationData[DestinationNr].CommBuss+matrix_sources.src_offset.min.buss, &FromChannel1, &FromChannel2);
    }
    else
    {
      axum_get_mtrx_chs_from_src(AxumData.DestinationData[DestinationNr].Source, &FromChannel1, &FromChannel2);
    }

    //Check routing of the destination;
    switch (AxumData.DestinationData[DestinationNr].Routing)
    {
      case 0:
      {//Stereo, do nothing
      }
      break;
      case 1:
      {//Left
        FromChannel2 = FromChannel1;
      }
      break;
      case 2:
      {//Right
        FromChannel1 = FromChannel2;
      }
      break;
    }

    if (Output1>-1)
    {
      SetBackplaneRouting(FromChannel1, Output1);
    }
    if (Output2>-1)
    {
      SetBackplaneRouting(FromChannel2, Output2);
    }
  }
}

void SetAxum_TalkbackSource(unsigned int TalkbackNr)
{
  int Output1 = 1792+(TalkbackNr*2);
  int Output2 = Output1+1;
  unsigned int FromChannel1 = 0;
  unsigned int FromChannel2 = 0;

  axum_get_mtrx_chs_from_src(AxumData.Talkback[TalkbackNr].Source, &FromChannel1, &FromChannel2);

  if (Output1>-1)
  {
    SetBackplaneRouting(FromChannel1, Output1);
  }
  if (Output2>-1)
  {
    SetBackplaneRouting(FromChannel2, Output2);
  }
}

void SetAxum_BussMasterLevels()
{
  for (int cntDSPCard=0; cntDSPCard<4; cntDSPCard++)
  {
    DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[cntDSPCard];

    //stereo buss 1-16
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      dspcard->data.BussMasterData[(cntBuss*2)+0].Level = AxumData.BussMasterData[cntBuss].Level;
      dspcard->data.BussMasterData[(cntBuss*2)+1].Level = AxumData.BussMasterData[cntBuss].Level;
      dspcard->data.BussMasterData[(cntBuss*2)+0].On = AxumData.BussMasterData[cntBuss].On;
      dspcard->data.BussMasterData[(cntBuss*2)+1].On = AxumData.BussMasterData[cntBuss].On;
    }
  }
  dsp_set_buss_mstr_lvl(dsp_handler);
}

void SetAxum_MonitorBuss(unsigned int MonitorBussNr)
{
  unsigned int MonitorChannelNr = MonitorBussNr<<1;
  unsigned char DSPCardNr = (MonitorChannelNr/8);
  unsigned char DSPCardMonitorChannelNr = MonitorChannelNr%8;

  DSPCARD_STRUCT *dspcard = &dsp_handler->dspcard[DSPCardNr];

  for (int cntMonitorInput=0; cntMonitorInput<48; cntMonitorInput++)
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[cntMonitorInput] = -140;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[cntMonitorInput] = -140;
  }

  //Check if an automatic switch buss is active
  char MonitorBussDimActive = 0;
  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    if ((AxumData.Monitor[MonitorBussNr].AutoSwitchingBuss[cntBuss]) &&
        (AxumData.Monitor[MonitorBussNr].Buss[cntBuss]))
    {
      MonitorBussDimActive = 1;
    }
  }

  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    if (AxumData.Monitor[MonitorBussNr].Buss[cntBuss])
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[(cntBuss*2)+0] = 0;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[(cntBuss*2)+1] = 0;

      if (MonitorBussDimActive)
      {
        if (!AxumData.Monitor[MonitorBussNr].AutoSwitchingBuss[cntBuss])
        {
          dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[(cntBuss*2)+0] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
          dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[(cntBuss*2)+1] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
        }
      }
    }
  }

  if (AxumData.Monitor[MonitorBussNr].Ext[0])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[32] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[33] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[32] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[33] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[1])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[34] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[35] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[34] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[35] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[2])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[36] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[37] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[36] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[37] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[3])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[38] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[39] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[38] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[39] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[4])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[40] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[41] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[40] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[41] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[5])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[42] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[43] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[42] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[43] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[6])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[44] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[45] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[44] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[45] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }
  if (AxumData.Monitor[MonitorBussNr].Ext[7])
  {
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[46] = 0;
    dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[47] = 0;
    if (MonitorBussDimActive)
    {
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].Level[46] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
      dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].Level[47] += AxumData.Monitor[MonitorBussNr].SwitchingDimLevel;
    }
  }

  dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+0].MasterLevel = 0;
  dspcard->data.MonitorChannelData[DSPCardMonitorChannelNr+1].MasterLevel = 0;

  dsp_set_monitor_buss(dsp_handler, MonitorChannelNr+0);
  dsp_set_monitor_buss(dsp_handler, MonitorChannelNr+1);
}

void CheckObjectsToSent(unsigned int SensorReceiveFunctionNumber, unsigned int MambaNetAddress)
{
  unsigned int FunctionType = (SensorReceiveFunctionNumber>>24)&0xFF;
  unsigned int FunctionNumber = (SensorReceiveFunctionNumber>>12)&0xFFF;
  unsigned int Function = SensorReceiveFunctionNumber&0xFFF;
  AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = NULL;

  switch (FunctionType)
  {
    case MODULE_FUNCTIONS:
    {   //Module
      if (FunctionNumber<NUMBER_OF_MODULES)
      {
        WalkAxumFunctionInformationStruct = ModuleFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_MODULES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_MODULES].SelectedModule;
      }
    }
    break;
    case BUSS_FUNCTIONS:
    {   //Buss
      if (FunctionNumber<NUMBER_OF_BUSSES)
      {
        WalkAxumFunctionInformationStruct = BussFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_BUSSES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_BUSSES].SelectedBuss;
      }
    }
    break;
    case MONITOR_BUSS_FUNCTIONS:
    {   //Monitor Buss
      if (FunctionNumber<NUMBER_OF_MONITOR_BUSSES)
      {
        WalkAxumFunctionInformationStruct = MonitorBussFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_MONITOR_BUSSES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_MONITOR_BUSSES].SelectedMonitorBuss;
      }
    }
    break;
    case CONSOLE_FUNCTIONS:
    {
      if (FunctionNumber<NUMBER_OF_CONSOLES)
      {
        WalkAxumFunctionInformationStruct = ConsoleFunctions[FunctionNumber][Function];
      }
    }
    break;
    case GLOBAL_FUNCTIONS:
    {   //Global
      WalkAxumFunctionInformationStruct = GlobalFunctions[Function];
    }
    break;
    case SOURCE_FUNCTIONS:
    {   //Source
      if (FunctionNumber<NUMBER_OF_SOURCES)
      {
        WalkAxumFunctionInformationStruct = SourceFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_SOURCES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_SOURCES].SelectedSource;
      }
    }
    break;
    case DESTINATION_FUNCTIONS:
    {   //Destination
      if (FunctionNumber<NUMBER_OF_DESTINATIONS)
      {
        WalkAxumFunctionInformationStruct = DestinationFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_DESTINATIONS+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_DESTINATIONS].SelectedDestination;
      }
    }
    break;
  }
  while (WalkAxumFunctionInformationStruct != NULL)
  {
    if ((MambaNetAddress == 0x00000000) || (MambaNetAddress == WalkAxumFunctionInformationStruct->MambaNetAddress))
    {
      unsigned int FunctionNrToSend = (FunctionType<<24) | (FunctionNumber<<12) | Function;
      SentDataToObject(FunctionNrToSend, WalkAxumFunctionInformationStruct);
    }
    WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
  }

  //And check the select functions as well
  for (int cntConsole=0; cntConsole<4; cntConsole++)
  {
    switch (FunctionType)
    {
      case MODULE_FUNCTIONS:
      {   //Module
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedModule)
        {
          WalkAxumFunctionInformationStruct = ModuleFunctions[NUMBER_OF_MODULES+cntConsole][Function];
        }
      }
      break;
      case BUSS_FUNCTIONS:
      {   //Buss
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedBuss)
        {
          WalkAxumFunctionInformationStruct = BussFunctions[NUMBER_OF_BUSSES+cntConsole][Function];
        }
      }
      break;
      case MONITOR_BUSS_FUNCTIONS:
      {   //Monitor Buss
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedMonitorBuss)
        {
          WalkAxumFunctionInformationStruct = MonitorBussFunctions[NUMBER_OF_MONITOR_BUSSES+cntConsole][Function];
        }
      }
      break;
      case SOURCE_FUNCTIONS:
      {   //Source
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedSource)
        {
          WalkAxumFunctionInformationStruct = SourceFunctions[NUMBER_OF_SOURCES+cntConsole][Function];
        }
      }
      break;
      case DESTINATION_FUNCTIONS:
      {   //Destination
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedDestination)
        {
          WalkAxumFunctionInformationStruct = DestinationFunctions[NUMBER_OF_DESTINATIONS+cntConsole][Function];
        }
      }
      break;
    }
    while (WalkAxumFunctionInformationStruct != NULL)
    {
      if ((MambaNetAddress == 0x00000000) || (MambaNetAddress == WalkAxumFunctionInformationStruct->MambaNetAddress))
      {
        unsigned int FunctionNrToSend = (FunctionType<<24) | (FunctionNumber<<12) | Function;
        SentDataToObject(FunctionNrToSend, WalkAxumFunctionInformationStruct);
      }
      WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
    }
  }
}

void SentDataToObject(unsigned int SensorReceiveFunctionNumber, AXUM_FUNCTION_INFORMATION_STRUCT *InfoObjectToSend)
{
  unsigned char SensorReceiveFunctionType = (SensorReceiveFunctionNumber>>24)&0xFF;
  char LCDText[65];
  mbn_data data;

  switch (SensorReceiveFunctionType)
  {
    case MODULE_FUNCTIONS:
    {   //Module
      unsigned int ModuleNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
      unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;
      //unsigned int Active = 0;

      if ((ModuleNr >= NUMBER_OF_MODULES) && (ModuleNr<(NUMBER_OF_MODULES+4)))
      {
        ModuleNr = AxumData.ConsoleData[ModuleNr-NUMBER_OF_MODULES].SelectedModule;
      }

      switch (FunctionNr)
      {
        case MODULE_FUNCTION_LABEL:
        { //Label
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              if (ModuleNr<9)
              {
                sprintf(LCDText, " Mod %d  ", ModuleNr+1);
              }
              else if (ModuleNr<99)
              {
                sprintf(LCDText, " Mod %d ", ModuleNr+1);
              }
              else
              {
                sprintf(LCDText, "Mod %d ", ModuleNr+1);
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_SOURCE:
        { //Source
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              GetSourceLabel(AxumData.ModuleData[ModuleNr].TemporySourceLocal, LCDText, 8);

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_PRESET:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              GetPresetLabel(AxumData.ModuleData[ModuleNr].TemporyPresetLocal, LCDText, 8);

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_PRESET_1A:
        case MODULE_FUNCTION_PRESET_1B:
        case MODULE_FUNCTION_PRESET_2A:
        case MODULE_FUNCTION_PRESET_2B:
        case MODULE_FUNCTION_PRESET_3A:
        case MODULE_FUNCTION_PRESET_3B:
        case MODULE_FUNCTION_PRESET_4A:
        case MODULE_FUNCTION_PRESET_4B:
        { //Not implemented
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              unsigned char PresetNr = GetPresetNrFromFunctionNr(FunctionNr);
              if (PresetNr>0)
              {
                data.State = ModulePresetActive(ModuleNr, PresetNr);
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_PRESET_A:
        case MODULE_FUNCTION_PRESET_B:
        case MODULE_FUNCTION_PRESET_A_B:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              int PresetNr = (FunctionNr-MODULE_FUNCTION_PRESET_A)+1;
              if (FunctionNr == MODULE_FUNCTION_PRESET_A_B)
              {
                PresetNr = 2;
              }
              PresetNr += AxumData.ModuleData[ModuleNr].ModulePreset<<1;
              if (PresetNr>0)
              {
                data.State = ModulePresetActive(ModuleNr, PresetNr);
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_SOURCE_PHANTOM:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
              {
                int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
                {
                  data.State = 0;
                  if (AxumData.ModuleData[ModuleNr].SelectedSource != 0)
                  {
                    data.State = AxumData.SourceData[SourceNr].Phantom;
                  }
                  mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
                }
              }
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_SOURCE_PAD:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
              {
                int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

                if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PAD))
                {
                  data.State = 0;
                  if (AxumData.ModuleData[ModuleNr].SelectedSource != 0)
                  {
                    data.State = AxumData.SourceData[SourceNr].Pad;
                  }
                  mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
                }
              }
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_SOURCE_GAIN_LEVEL:
        {
          if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
          {
            int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
            unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_UINT:
              {
                if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
                {
                  float min, max, def;
                  CheckObjectRange(FunctionNrToSent | SOURCE_FUNCTION_GAIN, &min, &max, &def);

                  int Position = (((AxumData.SourceData[SourceNr].Gain-min)*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/(max-min))+InfoObjectToSend->ActuatorDataMinimal;
                  data.UInt = Position;
                }
                else
                {
                  data.UInt = 0;
                }
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
                {
                  sprintf(LCDText,     "%5.1fdB", AxumData.SourceData[SourceNr].Gain);
                }
                else
                {
                  sprintf(LCDText, "Not used");
                }
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
            }
          }
          else
          {
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_UINT:
              {
                data.UInt = 0;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                sprintf(LCDText, "- dB");
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
            }
          }
        }
        break;
        case MODULE_FUNCTION_SOURCE_GAIN_LEVEL_RESET:
        {
        }
        break;
        case MODULE_FUNCTION_INSERT_ON_OFF:
        { //Insert on/off
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].InsertOnOff;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_PHASE_ON_OFF: //Phase
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].PhaseOnOff;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_GAIN_LEVEL: //Gain level
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              int Position = (((AxumData.ModuleData[ModuleNr].Gain+20)*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/40)+InfoObjectToSend->ActuatorDataMinimal;

              data.UInt = Position;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText,     "%5.1fdB", AxumData.ModuleData[ModuleNr].Gain);

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_GAIN_LEVEL_RESET:  //Gain level reset
        {
        }
        break;
        case MODULE_FUNCTION_LOW_CUT_FREQUENCY: //Low cut frequency
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              if (!AxumData.ModuleData[ModuleNr].FilterOnOff)
              {
                sprintf(LCDText, "  Off  ");
              }
              else
              {
                sprintf(LCDText, "%5dHz", AxumData.ModuleData[ModuleNr].Filter.Frequency);
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ModuleData[ModuleNr].Filter.Frequency;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_LOW_CUT_ON_OFF:  //Low cut on/off
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].FilterOnOff;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_LEVEL: //EQ level
        case MODULE_FUNCTION_EQ_BAND_2_LEVEL:
        case MODULE_FUNCTION_EQ_BAND_3_LEVEL:
        case MODULE_FUNCTION_EQ_BAND_4_LEVEL:
        case MODULE_FUNCTION_EQ_BAND_5_LEVEL:
        case MODULE_FUNCTION_EQ_BAND_6_LEVEL:
        {
          int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_LEVEL)/(MODULE_FUNCTION_EQ_BAND_2_LEVEL-MODULE_FUNCTION_EQ_BAND_1_LEVEL);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "%5.1fdB", AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_FREQUENCY: //EQ frequency
        case MODULE_FUNCTION_EQ_BAND_2_FREQUENCY:
        case MODULE_FUNCTION_EQ_BAND_3_FREQUENCY:
        case MODULE_FUNCTION_EQ_BAND_4_FREQUENCY:
        case MODULE_FUNCTION_EQ_BAND_5_FREQUENCY:
        case MODULE_FUNCTION_EQ_BAND_6_FREQUENCY:
        {
          int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY)/(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY);

          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "%5dHz", AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH: //EQ bandwidth
        case MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH:
        case MODULE_FUNCTION_EQ_BAND_3_BANDWIDTH:
        case MODULE_FUNCTION_EQ_BAND_4_BANDWIDTH:
        case MODULE_FUNCTION_EQ_BAND_5_BANDWIDTH:
        case MODULE_FUNCTION_EQ_BAND_6_BANDWIDTH:
        {
          int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH)/(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH);

          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "%5.1f Q", AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_LEVEL_RESET: //EQ level reset
        case MODULE_FUNCTION_EQ_BAND_2_LEVEL_RESET:
        case MODULE_FUNCTION_EQ_BAND_3_LEVEL_RESET:
        case MODULE_FUNCTION_EQ_BAND_4_LEVEL_RESET:
        case MODULE_FUNCTION_EQ_BAND_5_LEVEL_RESET:
        case MODULE_FUNCTION_EQ_BAND_6_LEVEL_RESET:
        {
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_FREQUENCY_RESET: //EQ frequency reset
        case MODULE_FUNCTION_EQ_BAND_2_FREQUENCY_RESET:
        case MODULE_FUNCTION_EQ_BAND_3_FREQUENCY_RESET:
        case MODULE_FUNCTION_EQ_BAND_4_FREQUENCY_RESET:
        case MODULE_FUNCTION_EQ_BAND_5_FREQUENCY_RESET:
        case MODULE_FUNCTION_EQ_BAND_6_FREQUENCY_RESET:
        {
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH_RESET: //EQ bandwidth reset
        case MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH_RESET:
        case MODULE_FUNCTION_EQ_BAND_3_BANDWIDTH_RESET:
        case MODULE_FUNCTION_EQ_BAND_4_BANDWIDTH_RESET:
        case MODULE_FUNCTION_EQ_BAND_5_BANDWIDTH_RESET:
        case MODULE_FUNCTION_EQ_BAND_6_BANDWIDTH_RESET:
        {
        }
        break;
        case MODULE_FUNCTION_EQ_BAND_1_TYPE:  //EQ type
        case MODULE_FUNCTION_EQ_BAND_2_TYPE:
        case MODULE_FUNCTION_EQ_BAND_3_TYPE:
        case MODULE_FUNCTION_EQ_BAND_4_TYPE:
        case MODULE_FUNCTION_EQ_BAND_5_TYPE:
        case MODULE_FUNCTION_EQ_BAND_6_TYPE:
        {
          int BandNr = (FunctionNr-MODULE_FUNCTION_EQ_BAND_1_TYPE)/(MODULE_FUNCTION_EQ_BAND_2_TYPE-MODULE_FUNCTION_EQ_BAND_1_TYPE);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              switch (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type)
              {
                case OFF:
                {
                  sprintf(LCDText, "  Off   ");
                }
                break;
                case HPF:
                {
                  sprintf(LCDText, "HighPass");
                }
                break;
                case LOWSHELF:
                {
                  sprintf(LCDText, "LowShelf");
                }
                break;
                case PEAKINGEQ:
                {
                  sprintf(LCDText, "Peaking ");
                }
                break;
                case HIGHSHELF:
                {
                  sprintf(LCDText, "Hi Shelf");
                }
                break;
                case LPF:
                {
                  sprintf(LCDText, "Low Pass");
                }
                break;
                case BPF:
                {
                  sprintf(LCDText, "BandPass");
                }
                break;
                case NOTCH:
                {
                  sprintf(LCDText, "  Notch ");
                }
                break;
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_EQ_ON_OFF:
        { //EQ on/off
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].EQOnOff;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
    //        case MODULE_FUNCTION_EQ_TYPE_A:
    //        case MODULE_FUNCTION_EQ_TYPE_B:
    //        {
    //        }
    //        break;
        case MODULE_FUNCTION_AGC_THRESHOLD:
        { //Dynamics threshold
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = (((AxumData.ModuleData[ModuleNr].AGCThreshold+30)*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/30)+InfoObjectToSend->ActuatorDataMinimal;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "%5.1fdB", AxumData.ModuleData[ModuleNr].AGCThreshold);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_AGC_AMOUNT:
        { //Dynamics amount
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = (((AxumData.ModuleData[ModuleNr].AGCRatio-1)*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/19)+InfoObjectToSend->ActuatorDataMinimal;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, " 1:%1.1f ", AxumData.ModuleData[ModuleNr].AGCRatio);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_DYNAMICS_ON_OFF:
        { //Dynamics on/off
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].DynamicsOnOff;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_EXPANDER_THRESHOLD:
        { //Downward expander threshold
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = (((AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold+50)*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/50)+InfoObjectToSend->ActuatorDataMinimal;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "%5.1fdB", AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_MONO_ON_OFF:
        { //Mono
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].MonoOnOff;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_PAN:
        { //Pan
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = ((AxumData.ModuleData[ModuleNr].Panorama*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              unsigned char Types[4] = {'[','|','|',']'};
              unsigned char Pos = AxumData.ModuleData[ModuleNr].Panorama/128;
              unsigned char Type = (AxumData.ModuleData[ModuleNr].Panorama%128)/32;

              sprintf(LCDText, "        ");
              if (AxumData.ModuleData[ModuleNr].Panorama == 0)
              {
                sprintf(LCDText, "Left    ");
              }
              else if ((AxumData.ModuleData[ModuleNr].Panorama == 511) || (AxumData.ModuleData[ModuleNr].Panorama == 512))
              {
                sprintf(LCDText, " Center ");
              }
              else if (AxumData.ModuleData[ModuleNr].Panorama == 1023)
              {
                sprintf(LCDText, "   Right");
              }
              else
              {
                LCDText[Pos] = Types[Type];
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_PAN_RESET:
        { //Pan reset
        }
        break;
        case MODULE_FUNCTION_MODULE_LEVEL:
        { //Module level
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              int dB = 0;

              dB = ((AxumData.ModuleData[ModuleNr].FaderLevel+AxumData.LevelReserve)*10)+1400;

              if (dB<0)
              {
                dB = 0;
              }
              else if (dB>=1500)
              {
                dB = 1499;
              }
              int Position = dB2Position[dB];
              Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
              if (Position<InfoObjectToSend->ActuatorDataMinimal)
              {
                Position = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Position>InfoObjectToSend->ActuatorDataMaximal)
              {
                Position = InfoObjectToSend->ActuatorDataMaximal;
              }

              data.UInt = Position;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 0);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, " %4.0f dB", AxumData.ModuleData[ModuleNr].FaderLevel);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = AxumData.ModuleData[ModuleNr].FaderLevel;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 0);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_MODULE_ON:
        { //Module on
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_MODULE_OFF:
        { //Module off
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = !AxumData.ModuleData[ModuleNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_MODULE_ON_OFF:
        { //Module on/off
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_LEVEL:
        case MODULE_FUNCTION_BUSS_3_4_LEVEL:
        case MODULE_FUNCTION_BUSS_5_6_LEVEL:
        case MODULE_FUNCTION_BUSS_7_8_LEVEL:
        case MODULE_FUNCTION_BUSS_9_10_LEVEL:
        case MODULE_FUNCTION_BUSS_11_12_LEVEL:
        case MODULE_FUNCTION_BUSS_13_14_LEVEL:
        case MODULE_FUNCTION_BUSS_15_16_LEVEL:
        case MODULE_FUNCTION_BUSS_17_18_LEVEL:
        case MODULE_FUNCTION_BUSS_19_20_LEVEL:
        case MODULE_FUNCTION_BUSS_21_22_LEVEL:
        case MODULE_FUNCTION_BUSS_23_24_LEVEL:
        case MODULE_FUNCTION_BUSS_25_26_LEVEL:
        case MODULE_FUNCTION_BUSS_27_28_LEVEL:
        case MODULE_FUNCTION_BUSS_29_30_LEVEL:
        case MODULE_FUNCTION_BUSS_31_32_LEVEL:
        { //Aux Level
          int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_LEVEL)/(MODULE_FUNCTION_BUSS_3_4_LEVEL-MODULE_FUNCTION_BUSS_1_2_LEVEL);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              int dB = 0;
              dB = ((AxumData.ModuleData[ModuleNr].Buss[BussNr].Level+AxumData.LevelReserve)*10)+1400;

              if (dB<0)
              {
                dB = 0;
              }
              else if (dB>=1500)
              {
                dB = 1499;
              }
              int Position = dB2Position[dB];
              Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
              if (Position<InfoObjectToSend->ActuatorDataMinimal)
              {
                Position = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Position>InfoObjectToSend->ActuatorDataMaximal)
              {
                Position = InfoObjectToSend->ActuatorDataMaximal;
              }

              data.UInt = Position;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              if (AxumData.ModuleData[ModuleNr].Buss[BussNr].On)
              {
                sprintf(LCDText, " %4.0f dB", AxumData.ModuleData[ModuleNr].Buss[BussNr].Level);
              }
              else
              {
                sprintf(LCDText, "  Off   ");
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_3_4_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_5_6_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_7_8_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_9_10_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_11_12_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_13_14_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_15_16_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_17_18_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_19_20_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_21_22_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_23_24_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_25_26_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_27_28_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_29_30_LEVEL_RESET:
        case MODULE_FUNCTION_BUSS_31_32_LEVEL_RESET:
        { //Aux Level reset
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_ON:
        case MODULE_FUNCTION_BUSS_3_4_ON:
        case MODULE_FUNCTION_BUSS_5_6_ON:
        case MODULE_FUNCTION_BUSS_7_8_ON:
        case MODULE_FUNCTION_BUSS_9_10_ON:
        case MODULE_FUNCTION_BUSS_11_12_ON:
        case MODULE_FUNCTION_BUSS_13_14_ON:
        case MODULE_FUNCTION_BUSS_15_16_ON:
        case MODULE_FUNCTION_BUSS_17_18_ON:
        case MODULE_FUNCTION_BUSS_19_20_ON:
        case MODULE_FUNCTION_BUSS_21_22_ON:
        case MODULE_FUNCTION_BUSS_23_24_ON:
        case MODULE_FUNCTION_BUSS_25_26_ON:
        case MODULE_FUNCTION_BUSS_27_28_ON:
        case MODULE_FUNCTION_BUSS_29_30_ON:
        case MODULE_FUNCTION_BUSS_31_32_ON:
        { //Buss on/off
          int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_ON)/(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].Buss[BussNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_OFF:
        case MODULE_FUNCTION_BUSS_3_4_OFF:
        case MODULE_FUNCTION_BUSS_5_6_OFF:
        case MODULE_FUNCTION_BUSS_7_8_OFF:
        case MODULE_FUNCTION_BUSS_9_10_OFF:
        case MODULE_FUNCTION_BUSS_11_12_OFF:
        case MODULE_FUNCTION_BUSS_13_14_OFF:
        case MODULE_FUNCTION_BUSS_15_16_OFF:
        case MODULE_FUNCTION_BUSS_17_18_OFF:
        case MODULE_FUNCTION_BUSS_19_20_OFF:
        case MODULE_FUNCTION_BUSS_21_22_OFF:
        case MODULE_FUNCTION_BUSS_23_24_OFF:
        case MODULE_FUNCTION_BUSS_25_26_OFF:
        case MODULE_FUNCTION_BUSS_27_28_OFF:
        case MODULE_FUNCTION_BUSS_29_30_OFF:
        case MODULE_FUNCTION_BUSS_31_32_OFF:
        { //Buss on/off
          int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_OFF)/(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = !AxumData.ModuleData[ModuleNr].Buss[BussNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_ON_OFF:
        case MODULE_FUNCTION_BUSS_3_4_ON_OFF:
        case MODULE_FUNCTION_BUSS_5_6_ON_OFF:
        case MODULE_FUNCTION_BUSS_7_8_ON_OFF:
        case MODULE_FUNCTION_BUSS_9_10_ON_OFF:
        case MODULE_FUNCTION_BUSS_11_12_ON_OFF:
        case MODULE_FUNCTION_BUSS_13_14_ON_OFF:
        case MODULE_FUNCTION_BUSS_15_16_ON_OFF:
        case MODULE_FUNCTION_BUSS_17_18_ON_OFF:
        case MODULE_FUNCTION_BUSS_19_20_ON_OFF:
        case MODULE_FUNCTION_BUSS_21_22_ON_OFF:
        case MODULE_FUNCTION_BUSS_23_24_ON_OFF:
        case MODULE_FUNCTION_BUSS_25_26_ON_OFF:
        case MODULE_FUNCTION_BUSS_27_28_ON_OFF:
        case MODULE_FUNCTION_BUSS_29_30_ON_OFF:
        case MODULE_FUNCTION_BUSS_31_32_ON_OFF:
        { //Buss on/off
          int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_ON_OFF)/(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].Buss[BussNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_PRE:
        case MODULE_FUNCTION_BUSS_3_4_PRE:
        case MODULE_FUNCTION_BUSS_5_6_PRE:
        case MODULE_FUNCTION_BUSS_7_8_PRE:
        case MODULE_FUNCTION_BUSS_9_10_PRE:
        case MODULE_FUNCTION_BUSS_11_12_PRE:
        case MODULE_FUNCTION_BUSS_13_14_PRE:
        case MODULE_FUNCTION_BUSS_15_16_PRE:
        case MODULE_FUNCTION_BUSS_17_18_PRE:
        case MODULE_FUNCTION_BUSS_19_20_PRE:
        case MODULE_FUNCTION_BUSS_21_22_PRE:
        case MODULE_FUNCTION_BUSS_23_24_PRE:
        case MODULE_FUNCTION_BUSS_25_26_PRE:
        case MODULE_FUNCTION_BUSS_27_28_PRE:
        case MODULE_FUNCTION_BUSS_29_30_PRE:
        case MODULE_FUNCTION_BUSS_31_32_PRE:
        { //Buss pre
          int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_PRE)/(MODULE_FUNCTION_BUSS_3_4_PRE-MODULE_FUNCTION_BUSS_1_2_PRE);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].Buss[BussNr].PreModuleLevel;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_BALANCE:
        case MODULE_FUNCTION_BUSS_3_4_BALANCE:
        case MODULE_FUNCTION_BUSS_5_6_BALANCE:
        case MODULE_FUNCTION_BUSS_7_8_BALANCE:
        case MODULE_FUNCTION_BUSS_9_10_BALANCE:
        case MODULE_FUNCTION_BUSS_11_12_BALANCE:
        case MODULE_FUNCTION_BUSS_13_14_BALANCE:
        case MODULE_FUNCTION_BUSS_15_16_BALANCE:
        case MODULE_FUNCTION_BUSS_17_18_BALANCE:
        case MODULE_FUNCTION_BUSS_19_20_BALANCE:
        case MODULE_FUNCTION_BUSS_21_22_BALANCE:
        case MODULE_FUNCTION_BUSS_23_24_BALANCE:
        case MODULE_FUNCTION_BUSS_25_26_BALANCE:
        case MODULE_FUNCTION_BUSS_27_28_BALANCE:
        case MODULE_FUNCTION_BUSS_29_30_BALANCE:
        case MODULE_FUNCTION_BUSS_31_32_BALANCE:
        { //Buss balance
          int BussNr = (FunctionNr-MODULE_FUNCTION_BUSS_1_2_BALANCE)/(MODULE_FUNCTION_BUSS_3_4_BALANCE-MODULE_FUNCTION_BUSS_1_2_BALANCE);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = ((AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              unsigned char Types[4] = {'[','|','|',']'};
              unsigned char Pos = AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance/128;
              unsigned char Type = (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance%128)/32;

              sprintf(LCDText, "        ");
              if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 0)
              {
                sprintf(LCDText, "Left    ");
              }
              else if ((AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 511) || (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 512))
              {
                sprintf(LCDText, " Center ");
              }
              else if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 1023)
              {
                sprintf(LCDText, "   Right");
              }
              else
              {
                LCDText[Pos] = Types[Type];
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_BUSS_1_2_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_3_4_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_5_6_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_7_8_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_9_10_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_11_12_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_13_14_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_15_16_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_17_18_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_19_20_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_21_22_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_23_24_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_25_26_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_27_28_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_29_30_BALANCE_RESET:
        case MODULE_FUNCTION_BUSS_31_32_BALANCE_RESET:
        { //Buss balance reset
        }
        break;
        case MODULE_FUNCTION_PHASE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              if (AxumData.ModuleData[ModuleNr].PhaseOnOff)
              {
                switch (AxumData.ModuleData[ModuleNr].Phase)
                {
                  case 0x00:
                  {
                    sprintf(LCDText, " Normal ");
                  }
                  break;
                  case 0x01:
                  {
                    sprintf(LCDText, "  Left  ");
                  }
                  break;
                  case 0x02:
                  {
                    sprintf(LCDText, "  Right ");
                  }
                  break;
                  case 0x03:
                  {
                    sprintf(LCDText, "  Both  ");
                  }
                  break;
                }
              }
              else
              {
                sprintf(LCDText, "  Off   ");
              }
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
          }
        }
        break;
        case MODULE_FUNCTION_MONO:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              if (AxumData.ModuleData[ModuleNr].MonoOnOff)
              {
                switch (AxumData.ModuleData[ModuleNr].Mono)
                {
                  case 0x00:
                  {
                    sprintf(LCDText, " Stereo ");
                  }
                  break;
                  case 0x01:
                  {
                    sprintf(LCDText, "  Left  ");
                  }
                  break;
                  case 0x02:
                  {
                    sprintf(LCDText, "  Right ");
                  }
                  break;
                  case 0x03:
                  {
                    sprintf(LCDText, "  Mono  ");
                  }
                  break;
                }
              }
              else
              {
                sprintf(LCDText, "  Off   ");
              }
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
          }
        }
        break;
        case MODULE_FUNCTION_SELECT_1:
        case MODULE_FUNCTION_SELECT_2:
        case MODULE_FUNCTION_SELECT_3:
        case MODULE_FUNCTION_SELECT_4:
        {
          int ConsoleNr = FunctionNr-MODULE_FUNCTION_SELECT_1;
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = 0;
              if (ModuleNr == AxumData.ConsoleData[ConsoleNr].SelectedModule)
              {
                data.State = 1;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_CONSOLE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ModuleData[ModuleNr].Console;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 1, data, 1);
            }
            break;
          }
        }
        break;
      }
      if (((FunctionNr>=MODULE_FUNCTION_SOURCE_START) && (FunctionNr<MODULE_FUNCTION_CONTROL_1)) ||
          ((FunctionNr>=MODULE_FUNCTION_FADER_AND_ON_ACTIVE) && (FunctionNr<=MODULE_FUNCTION_FADER_ON_OFF)) ||
          ((FunctionNr>=MODULE_FUNCTION_TALKBACK_1_TO_REL_DEST) && (FunctionNr<=MODULE_FUNCTION_TALKBACK_16_TO_REL_DEST)))
      { //all state functions
        bool Active;

        Active = 0;
        switch (FunctionNr)
        {
          case MODULE_FUNCTION_SOURCE_START:
          { //Start
            if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
            {
              int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
              Active = AxumData.SourceData[SourceNr].Start;
            }
          }
          break;
          case MODULE_FUNCTION_SOURCE_STOP:
          { //Stop
            if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
            {
              int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
              Active = !AxumData.SourceData[SourceNr].Start;
            }
          }
          break;
          case MODULE_FUNCTION_SOURCE_START_STOP:
          { //Start/Stop
            if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
            {
              int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
              Active = AxumData.SourceData[SourceNr].Start;
            }
          }
          break;
          case MODULE_FUNCTION_COUGH_ON_OFF:
          { //Cough on/off
            Active = AxumData.ModuleData[ModuleNr].Cough;
          }
          break;
          case MODULE_FUNCTION_SOURCE_ALERT:
          { //Alert
            if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
            {
              int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
              Active = AxumData.SourceData[SourceNr].Alert;
            }
          }
          break;
          case MODULE_FUNCTION_FADER_AND_ON_ACTIVE:
          case MODULE_FUNCTION_FADER_AND_ON_INACTIVE:
          case MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE:
          {
            Active = 0;
            if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
            {
              if (AxumData.ModuleData[ModuleNr].On)
              {
                Active = 1;
              }
            }

            if (FunctionNr == MODULE_FUNCTION_FADER_AND_ON_INACTIVE)
            {
              Active = !Active;
            }
          }
          break;
          case MODULE_FUNCTION_FADER_ON:
          case MODULE_FUNCTION_FADER_OFF:
          case MODULE_FUNCTION_FADER_ON_OFF:
          {
            Active = 0;
            if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
            {
              Active = 1;
            }

            if (FunctionNr == MODULE_FUNCTION_FADER_OFF)
            {
              Active = !Active;
            }
          }
          break;
          case MODULE_FUNCTION_TALKBACK_1_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_2_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_3_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_4_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_5_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_6_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_7_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_8_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_9_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_10_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_11_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_12_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_13_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_14_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_15_TO_REL_DEST:
          case MODULE_FUNCTION_TALKBACK_16_TO_REL_DEST:
          {
            int TalkbackNr = FunctionNr-MODULE_FUNCTION_TALKBACK_1_TO_REL_DEST;

            Active = AxumData.ModuleData[ModuleNr].TalkbackToRelatedDestination[TalkbackNr];
          }
          break;
        }
        data.State = Active;
        mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
      }
      switch (FunctionNr)
      {
        case MODULE_FUNCTION_CONTROL:
        case MODULE_FUNCTION_CONTROL_1:
        case MODULE_FUNCTION_CONTROL_2:
        case MODULE_FUNCTION_CONTROL_3:
        case MODULE_FUNCTION_CONTROL_4:
        { //Control 1-4
          ModeControllerSetData(SensorReceiveFunctionNumber, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, InfoObjectToSend->ActuatorDataType, InfoObjectToSend->ActuatorDataSize, InfoObjectToSend->ActuatorDataMinimal, InfoObjectToSend->ActuatorDataMaximal);
        }
        break;
        case MODULE_FUNCTION_CONTROL_LABEL:
        case MODULE_FUNCTION_CONTROL_1_LABEL:
        case MODULE_FUNCTION_CONTROL_2_LABEL:
        case MODULE_FUNCTION_CONTROL_3_LABEL:
        case MODULE_FUNCTION_CONTROL_4_LABEL:
        { //Control 1-4 label
          ModeControllerSetLabel(SensorReceiveFunctionNumber, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, InfoObjectToSend->ActuatorDataType, InfoObjectToSend->ActuatorDataSize, InfoObjectToSend->ActuatorDataMinimal, InfoObjectToSend->ActuatorDataMaximal);
        }
        break;
        case MODULE_FUNCTION_CONTROL_RESET:
        case MODULE_FUNCTION_CONTROL_1_RESET:
        case MODULE_FUNCTION_CONTROL_2_RESET:
        case MODULE_FUNCTION_CONTROL_3_RESET:
        case MODULE_FUNCTION_CONTROL_4_RESET:
        { //Control 1-4 reset
        }
        break;
        case MODULE_FUNCTION_PEAK:
        { //Peak
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].Peak;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_SIGNAL:
        { //Signal
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ModuleData[ModuleNr].Signal;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_AUDIO_LEVEL_LEFT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = dBLevel[(ModuleNr*2)+0]+AxumData.Headroom;

              if (data.Float<InfoObjectToSend->ActuatorDataMinimal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (data.Float>InfoObjectToSend->ActuatorDataMaximal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMaximal;
              }

              if (InfoObjectToSend->ActuatorData != data.Float)
              {
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, InfoObjectToSend->ActuatorDataSize, data, 0);
                InfoObjectToSend->ActuatorData = data.Float;
              }
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_AUDIO_LEVEL_RIGHT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = dBLevel[(ModuleNr*2)+1]+AxumData.Headroom;

              if (data.Float<InfoObjectToSend->ActuatorDataMinimal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (data.Float>InfoObjectToSend->ActuatorDataMaximal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMaximal;
              }

              if (InfoObjectToSend->ActuatorData != data.Float)
              {
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, InfoObjectToSend->ActuatorDataSize, data, 0);
                InfoObjectToSend->ActuatorData = data.Float;
              }
            }
            break;
          }
        }
        break;
        case MODULE_FUNCTION_AUDIO_PHASE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = Phase[ModuleNr];
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 0);
            }
            break;
          }
        }
        break;
      }
    }
    break;
    case BUSS_FUNCTIONS:
    {   //Busses
      unsigned int BussNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
      unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

      if ((BussNr >= NUMBER_OF_BUSSES) && (BussNr<(NUMBER_OF_BUSSES+4)))
      {
        BussNr = AxumData.ConsoleData[BussNr-NUMBER_OF_BUSSES].SelectedBuss;
      }

      switch (FunctionNr)
      {
        case BUSS_FUNCTION_MASTER_LEVEL:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              int dB = 0;
              dB = ((AxumData.BussMasterData[BussNr].Level+10)*10)+1400;

              if (dB<0)
              {
                dB = 0;
              }
              else if (dB>=1500)
              {
                dB = 1499;
              }
              int Position = dB2Position[dB];
              Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
              if (Position<InfoObjectToSend->ActuatorDataMinimal)
              {
                Position = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Position>InfoObjectToSend->ActuatorDataMaximal)
              {
                Position = InfoObjectToSend->ActuatorDataMaximal;
              }

              data.UInt = Position;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, " %4.0f dB", AxumData.BussMasterData[BussNr].Level);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_FLOAT:
            {
              float Level = AxumData.BussMasterData[BussNr].Level;
              if (Level<InfoObjectToSend->ActuatorDataMinimal)
              {
                Level = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Level>InfoObjectToSend->ActuatorDataMaximal)
              {
                Level = InfoObjectToSend->ActuatorDataMaximal;
              }
              data.Float = Level;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case BUSS_FUNCTION_MASTER_LEVEL_RESET:
        { //No implementation
        }
        break;
        case BUSS_FUNCTION_MASTER_ON_OFF:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.BussMasterData[BussNr].On;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case BUSS_FUNCTION_MASTER_PRE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              int NrOfBussPre = 0;
              int NrOfModules = 0;
              bool BussPre = 0;
              for (int cntModule=0; cntModule<128; cntModule++)
              {
                if ((AxumData.ModuleData[cntModule].Buss[BussNr].Assigned) && (AxumData.ModuleData[cntModule].Console == AxumData.BussMasterData[BussNr].Console))
                {
                  NrOfModules++;
                  if (AxumData.ModuleData[cntModule].Buss[BussNr].PreModuleLevel)
                  {
                    NrOfBussPre++;
                  }
                }
              }
              if ((NrOfBussPre*2) > NrOfModules)
              {
                BussPre = 1;
              }

              data.State = BussPre;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case BUSS_FUNCTION_LABEL:
        { //Buss label
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              data.Octets = (unsigned char *)AxumData.BussMasterData[BussNr].Label;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case BUSS_FUNCTION_AUDIO_LEVEL_LEFT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = SummingdBLevel[(BussNr*2)+0]+AxumData.Headroom;

              if (data.Float<InfoObjectToSend->ActuatorDataMinimal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (data.Float>InfoObjectToSend->ActuatorDataMaximal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMaximal;
              }

              if (InfoObjectToSend->ActuatorData != data.Float)
              {
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, InfoObjectToSend->ActuatorDataSize, data, 0);
                InfoObjectToSend->ActuatorData = data.Float;
              }
            }
          }
        }
        break;
        case BUSS_FUNCTION_AUDIO_LEVEL_RIGHT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = SummingdBLevel[(BussNr*2)+1]+AxumData.Headroom;

              if (data.Float<InfoObjectToSend->ActuatorDataMinimal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (data.Float>InfoObjectToSend->ActuatorDataMaximal)
              {
                data.Float = InfoObjectToSend->ActuatorDataMaximal;
              }

              if (InfoObjectToSend->ActuatorData != data.Float)
              {
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, InfoObjectToSend->ActuatorDataSize, data, 0);
                InfoObjectToSend->ActuatorData = data.Float;
              }
            }
          }
          break;
        }
        break;
        case BUSS_FUNCTION_SELECT_1:
        case BUSS_FUNCTION_SELECT_2:
        case BUSS_FUNCTION_SELECT_3:
        case BUSS_FUNCTION_SELECT_4:
        {
          int ConsoleNr = FunctionNr-BUSS_FUNCTION_SELECT_1;
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = 0;
              if (BussNr == AxumData.ConsoleData[ConsoleNr].SelectedBuss)
              {
                data.State = 1;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case BUSS_FUNCTION_AUDIO_PHASE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = SummingPhase[BussNr];
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 0);
            }
            break;
          }
        }
        break;
        case BUSS_FUNCTION_RESET:
        {
          unsigned char Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].Buss[BussNr].On)
            {
              Active = 1;
            }
          }
          data.State = Active;
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case BUSS_FUNCTION_TALKBACK_1:
        case BUSS_FUNCTION_TALKBACK_2:
        case BUSS_FUNCTION_TALKBACK_3:
        case BUSS_FUNCTION_TALKBACK_4:
        case BUSS_FUNCTION_TALKBACK_5:
        case BUSS_FUNCTION_TALKBACK_6:
        case BUSS_FUNCTION_TALKBACK_7:
        case BUSS_FUNCTION_TALKBACK_8:
        case BUSS_FUNCTION_TALKBACK_9:
        case BUSS_FUNCTION_TALKBACK_10:
        case BUSS_FUNCTION_TALKBACK_11:
        case BUSS_FUNCTION_TALKBACK_12:
        case BUSS_FUNCTION_TALKBACK_13:
        case BUSS_FUNCTION_TALKBACK_14:
        case BUSS_FUNCTION_TALKBACK_15:
        case BUSS_FUNCTION_TALKBACK_16:
        {
          int TalkbackNr = FunctionNr - BUSS_FUNCTION_TALKBACK_1;
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.BussMasterData[BussNr].Talkback[TalkbackNr];
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
      }
    }
    break;
    case MONITOR_BUSS_FUNCTIONS:
    {   //Monitor Busses
      unsigned int MonitorBussNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
      int FunctionNr = SensorReceiveFunctionNumber&0xFFF;
      unsigned int Active = 0;

      if ((MonitorBussNr >= NUMBER_OF_MONITOR_BUSSES) && (MonitorBussNr<(NUMBER_OF_MONITOR_BUSSES+4)))
      {
        MonitorBussNr = AxumData.ConsoleData[MonitorBussNr-NUMBER_OF_MONITOR_BUSSES].SelectedMonitorBuss;
      }

      if (((FunctionNr>=MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF) && (FunctionNr<=MONITOR_BUSS_FUNCTION_EXT_8_ON_OFF)) ||
          ((FunctionNr>=MONITOR_BUSS_FUNCTION_BUSS_1_2_ON) && (FunctionNr<=MONITOR_BUSS_FUNCTION_EXT_8_ON)) ||
          ((FunctionNr>=MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF) && (FunctionNr<=MONITOR_BUSS_FUNCTION_EXT_8_OFF)))
      {
        switch (FunctionNr)
        {
          case MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_3_4_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_5_6_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_7_8_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_9_10_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_11_12_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_13_14_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_15_16_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_17_18_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_19_20_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_21_22_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_23_24_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_25_26_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_27_28_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_29_30_ON_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_31_32_ON_OFF:
          { //Prog
            int BussNr = FunctionNr-MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF;
            Active = AxumData.Monitor[MonitorBussNr].Buss[BussNr];
          }
          break;
          case MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_2_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_3_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_4_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_5_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_6_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_7_ON_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_8_ON_OFF:
          { //Ext 1
            int ExtNr = FunctionNr-MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF;
            Active = AxumData.Monitor[MonitorBussNr].Ext[ExtNr];
          }
          break;
          case MONITOR_BUSS_FUNCTION_BUSS_1_2_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_3_4_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_5_6_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_7_8_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_9_10_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_11_12_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_13_14_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_15_16_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_17_18_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_19_20_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_21_22_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_23_24_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_25_26_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_27_28_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_29_30_ON:
          case MONITOR_BUSS_FUNCTION_BUSS_31_32_ON:
          { //Prog
            int BussNr = FunctionNr-MONITOR_BUSS_FUNCTION_BUSS_1_2_ON;
            Active = AxumData.Monitor[MonitorBussNr].Buss[BussNr];
          }
          break;
          case MONITOR_BUSS_FUNCTION_EXT_1_ON:
          case MONITOR_BUSS_FUNCTION_EXT_2_ON:
          case MONITOR_BUSS_FUNCTION_EXT_3_ON:
          case MONITOR_BUSS_FUNCTION_EXT_4_ON:
          case MONITOR_BUSS_FUNCTION_EXT_5_ON:
          case MONITOR_BUSS_FUNCTION_EXT_6_ON:
          case MONITOR_BUSS_FUNCTION_EXT_7_ON:
          case MONITOR_BUSS_FUNCTION_EXT_8_ON:
          { //Ext 1
            int ExtNr = FunctionNr-MONITOR_BUSS_FUNCTION_EXT_1_ON;
            Active = AxumData.Monitor[MonitorBussNr].Ext[ExtNr];
          }
          break;
          case MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_3_4_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_5_6_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_7_8_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_9_10_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_11_12_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_13_14_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_15_16_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_17_18_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_19_20_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_21_22_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_23_24_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_25_26_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_27_28_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_29_30_OFF:
          case MONITOR_BUSS_FUNCTION_BUSS_31_32_OFF:
          { //Prog
            int BussNr = FunctionNr-MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF;
            Active = !AxumData.Monitor[MonitorBussNr].Buss[BussNr];
          }
          break;
          case MONITOR_BUSS_FUNCTION_EXT_1_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_2_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_3_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_4_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_5_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_6_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_7_OFF:
          case MONITOR_BUSS_FUNCTION_EXT_8_OFF:
          { //Ext 1
            int ExtNr = FunctionNr-MONITOR_BUSS_FUNCTION_EXT_1_OFF;
            Active = !AxumData.Monitor[MonitorBussNr].Ext[ExtNr];
          }
          break;
        }

        data.State = Active;
        mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
      }
      else
      {
        switch (FunctionNr)
        {
          case MONITOR_BUSS_FUNCTION_MUTE:
          { //Mute
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = AxumData.Monitor[MonitorBussNr].Mute;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_DIM:
          { //Dim
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = AxumData.Monitor[MonitorBussNr].Dim;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_PHONES_LEVEL:
          { //Phones level
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_UINT:
              {
                int dB = 0;
                dB = ((AxumData.Monitor[MonitorBussNr].PhonesLevel-10)*10)+1400;

                if (dB<0)
                {
                  dB = 0;
                }
                else if (dB>=1500)
                {
                  dB = 1499;
                }
                int Position = dB2Position[dB];
                Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
                if (Position<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Position = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Position>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Position = InfoObjectToSend->ActuatorDataMaximal;
                }

                data.UInt = Position;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                sprintf(LCDText, " %4.0f dB", AxumData.Monitor[MonitorBussNr].PhonesLevel);
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
              case MBN_DATATYPE_FLOAT:
              {
                float Level = AxumData.Monitor[MonitorBussNr].PhonesLevel;
                if (Level<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Level = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Level>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Level = InfoObjectToSend->ActuatorDataMaximal;
                }
                data.Float = Level;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_MONO:
          { //Mono
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = AxumData.Monitor[MonitorBussNr].Mono;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_PHASE:
          { //Phase
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = AxumData.Monitor[MonitorBussNr].Phase;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_SPEAKER_LEVEL:
          { //Speaker level
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_UINT:
              {
                int dB = 0;
                dB = ((AxumData.Monitor[MonitorBussNr].SpeakerLevel-10)*10)+1400;

                if (dB<0)
                {
                  dB = 0;
                }
                else if (dB>=1500)
                {
                  dB = 1499;
                }
                int Position = dB2Position[dB];
                Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
                if (Position<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Position = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Position>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Position = InfoObjectToSend->ActuatorDataMaximal;
                }

                data.UInt = Position;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                sprintf(LCDText, " %4.0f dB", AxumData.Monitor[MonitorBussNr].SpeakerLevel);
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
              case MBN_DATATYPE_FLOAT:
              {
                float Level = AxumData.Monitor[MonitorBussNr].SpeakerLevel;
                if (Level<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Level = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Level>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Level = InfoObjectToSend->ActuatorDataMaximal;
                }
                data.Float = Level;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_TALKBACK_1:
          case MONITOR_BUSS_FUNCTION_TALKBACK_2:
          case MONITOR_BUSS_FUNCTION_TALKBACK_3:
          case MONITOR_BUSS_FUNCTION_TALKBACK_4:
          case MONITOR_BUSS_FUNCTION_TALKBACK_5:
          case MONITOR_BUSS_FUNCTION_TALKBACK_6:
          case MONITOR_BUSS_FUNCTION_TALKBACK_7:
          case MONITOR_BUSS_FUNCTION_TALKBACK_8:
          case MONITOR_BUSS_FUNCTION_TALKBACK_9:
          case MONITOR_BUSS_FUNCTION_TALKBACK_10:
          case MONITOR_BUSS_FUNCTION_TALKBACK_11:
          case MONITOR_BUSS_FUNCTION_TALKBACK_12:
          case MONITOR_BUSS_FUNCTION_TALKBACK_13:
          case MONITOR_BUSS_FUNCTION_TALKBACK_14:
          case MONITOR_BUSS_FUNCTION_TALKBACK_15:
          case MONITOR_BUSS_FUNCTION_TALKBACK_16:
          {
            int TalkbackNr = FunctionNr - MONITOR_BUSS_FUNCTION_TALKBACK_1;
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = AxumData.Monitor[MonitorBussNr].Talkback[TalkbackNr];
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_AUDIO_LEVEL_LEFT:
          {
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_FLOAT:
              {
                data.Float = SummingdBLevel[32+(MonitorBussNr*2)+0]+AxumData.Headroom;

                if (data.Float<InfoObjectToSend->ActuatorDataMinimal)
                {
                  data.Float = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (data.Float>InfoObjectToSend->ActuatorDataMaximal)
                {
                  data.Float = InfoObjectToSend->ActuatorDataMaximal;
                }

                if (InfoObjectToSend->ActuatorData != data.Float)
                {
                  mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, InfoObjectToSend->ActuatorDataSize, data, 0);
                  InfoObjectToSend->ActuatorData = data.Float;
                }
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_AUDIO_LEVEL_RIGHT:
          {
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_FLOAT:
              {
                data.Float = SummingdBLevel[32+(MonitorBussNr*2)+1]+AxumData.Headroom;

                if (data.Float<InfoObjectToSend->ActuatorDataMinimal)
                {
                  data.Float = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (data.Float>InfoObjectToSend->ActuatorDataMaximal)
                {
                  data.Float = InfoObjectToSend->ActuatorDataMaximal;
                }

                if (InfoObjectToSend->ActuatorData != data.Float)
                {
                  mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, InfoObjectToSend->ActuatorDataSize, data, 0);
                  InfoObjectToSend->ActuatorData = data.Float;
                }
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_LABEL:
          { //Buss label
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_OCTETS:
              {
                data.Octets = (unsigned char *)AxumData.Monitor[MonitorBussNr].Label;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_SELECT_1:
          case MONITOR_BUSS_FUNCTION_SELECT_2:
          case MONITOR_BUSS_FUNCTION_SELECT_3:
          case MONITOR_BUSS_FUNCTION_SELECT_4:
          {
            int ConsoleNr = FunctionNr-MONITOR_BUSS_FUNCTION_SELECT_1;
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = 0;
                if (MonitorBussNr == AxumData.ConsoleData[ConsoleNr].SelectedMonitorBuss)
                {
                  data.State = 1;
                }
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case MONITOR_BUSS_FUNCTION_AUDIO_PHASE:
          {
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_FLOAT:
              {
                data.Float = SummingPhase[16+MonitorBussNr];
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 0);
              }
              break;
            }
          }
          break;
        }
      }
    }
    break;
    case CONSOLE_FUNCTIONS:
    {
      int ConsoleNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
      int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

      switch (FunctionNr)
      {
        case CONSOLE_FUNCTION_CONTROL_MODE_ACTIVE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if (AxumData.ConsoleData[ConsoleNr].ControlMode != MODULE_CONTROL_MODE_NONE)
              {
                data.State = 1;
              }
              else
              {
                data.State = 0;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_CONTROL_MODE_SOURCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_PROCESSING_PRESET:
        case CONSOLE_FUNCTION_CONTROL_MODE_SOURCE_GAIN:
        case CONSOLE_FUNCTION_CONTROL_MODE_PHANTOM_ON_OFF:
        case CONSOLE_FUNCTION_CONTROL_MODE_PAD_ON_OFF:
        case CONSOLE_FUNCTION_CONTROL_MODE_GAIN:
        case CONSOLE_FUNCTION_CONTROL_MODE_PHASE:
        case CONSOLE_FUNCTION_CONTROL_MODE_LOW_CUT:
        case CONSOLE_FUNCTION_CONTROL_MODE_INSERT_ON_OFF:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_TYPE:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_TYPE:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_TYPE:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_TYPE:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_TYPE:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_TYPE:
        case CONSOLE_FUNCTION_CONTROL_MODE_EQ_ON_OFF:
        case CONSOLE_FUNCTION_CONTROL_MODE_EXP_THRESHOLD:
        case CONSOLE_FUNCTION_CONTROL_MODE_AGC_THRESHOLD:
        case CONSOLE_FUNCTION_CONTROL_MODE_AGC:
        case CONSOLE_FUNCTION_CONTROL_MODE_DYNAMICS_ON_OFF:
        case CONSOLE_FUNCTION_CONTROL_MODE_MONO:
        case CONSOLE_FUNCTION_CONTROL_MODE_PAN:
        case CONSOLE_FUNCTION_CONTROL_MODE_MODULE_LEVEL:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30_BALANCE:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32:
        case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32_BALANCE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              unsigned char Active = 0;

              int CorrespondingControlMode = GetControlModeFromConsoleFunctionNr(SensorReceiveFunctionNumber);

              if (AxumData.ConsoleData[ConsoleNr].ControlMode == CorrespondingControlMode)
              {
                Active = 1;
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_3_4:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_5_6:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_7_8:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_9_10:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_11_12:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_13_14:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_15_16:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_17_18:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_19_20:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_21_22:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_23_24:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_25_26:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_27_28:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_29_30:
        case CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_31_32:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              unsigned char Active = 0;
              unsigned char CorrespondingControlMode = FunctionNr-CONSOLE_FUNCTION_MASTER_CONTROL_MODE_BUSS_1_2;
              if (AxumData.ConsoleData[ConsoleNr].MasterControlMode == CorrespondingControlMode)
              {
                Active = 1;
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_MASTER_CONTROL:
        {
          MasterModeControllerSetData(ConsoleNr, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, InfoObjectToSend->ActuatorDataType, InfoObjectToSend->ActuatorDataSize, InfoObjectToSend->ActuatorDataMinimal, InfoObjectToSend->ActuatorDataMaximal);
        }
        break;
//        case CONSOLE_FUNCTION_CONSOLE_TO_PROGRAMMED_DEFAULTS:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_3_4:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_5_6:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_7_8:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_9_10:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_11_12:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_13_14:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_15_16:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_17_18:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_19_20:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_21_22:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_23_24:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_25_26:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_27_28:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_29_30:
        case CONSOLE_FUNCTION_CONTROL_MODES_BUSS_31_32:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              int BussNr = FunctionNr-CONSOLE_FUNCTION_CONTROL_MODES_BUSS_1_2;
              char NewMasterControlMode = BussNr;
              bool Active = false;

              if (AxumData.ConsoleData[ConsoleNr].MasterControlMode == NewMasterControlMode)
              {
                 Active = true;
              }
              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_CONSOLE_PRESET:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              GetConsolePresetLabel(AxumData.ConsoleData[ConsoleNr].SelectedConsolePreset, LCDText, 8);

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_MODULE_SELECT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].SelectedModule;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              if (AxumData.ConsoleData[ConsoleNr].SelectedModule<9)
              {
                sprintf(LCDText, " Mod %d  ", AxumData.ConsoleData[ConsoleNr].SelectedModule+1);
              }
              else if (AxumData.ConsoleData[ConsoleNr].SelectedModule<99)
              {
                sprintf(LCDText, " Mod %d ", AxumData.ConsoleData[ConsoleNr].SelectedModule+1);
              }
              else
              {
                sprintf(LCDText, "Mod %d ", AxumData.ConsoleData[ConsoleNr].SelectedModule+1);
              }
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_BUSS_SELECT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].SelectedBuss;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "Buss %d", AxumData.ConsoleData[ConsoleNr].SelectedBuss+1);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_MONITOR_BUSS_SELECT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].SelectedMonitorBuss;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "Mon %d", AxumData.ConsoleData[ConsoleNr].SelectedMonitorBuss+1);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_SOURCE_SELECT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].SelectedSource;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "Src %d", AxumData.ConsoleData[ConsoleNr].SelectedSource+1);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_DESTINATION_SELECT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].SelectedDestination;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, "Src %d", AxumData.ConsoleData[ConsoleNr].SelectedDestination+1);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
//        case CONSOLE_FUNCTION_CHIPCARD_CHANGE:
        break;
        case CONSOLE_FUNCTION_CHIPCARD_USER:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              strncpy(LCDText, AxumData.ConsoleData[ConsoleNr].UsernameToWrite, 32);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 32, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_CHIPCARD_PASS:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              strncpy(LCDText, AxumData.ConsoleData[ConsoleNr].PasswordToWrite, 16);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 16, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_WRITE_CHIPCARD_USER_PASS:
        {
        }
        break;
        case CONSOLE_FUNCTION_UPDATE_USER:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              strncpy(LCDText, AxumData.ConsoleData[ConsoleNr].Username, 32);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 32, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_UPDATE_PASS:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              strncpy(LCDText, AxumData.ConsoleData[ConsoleNr].Password, 16);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 16, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_UPDATE_USER_PASS:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              memcpy(LCDText, AxumData.ConsoleData[ConsoleNr].Username, 32);
              memcpy(&LCDText[32], AxumData.ConsoleData[ConsoleNr].Password, 16);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 48, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_USER_LEVEL:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ConsoleData[ConsoleNr].UserLevel;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              char UserLevelNames[7][21] = {"Idle", "Unknown user", "Operator 1", "Operator 2", "Supervisor 1", "Supervisor 2", "Administrator"};
              data.Octets = (unsigned char *)UserLevelNames[AxumData.ConsoleData[ConsoleNr].UserLevel];
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 13, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_DOT_COUNT_UPDOWN:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ConsoleData[ConsoleNr].DotCountUpDown;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_PROGRAM_ENDTIME_ENABLE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.ConsoleData[ConsoleNr].ProgramEndTimeEnable;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_PROGRAM_ENDTIME:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              if (AxumData.ConsoleData[ConsoleNr].ProgramEndTimeHours<24)
              {
                sprintf(LCDText, "%02d:%02d:%02d", AxumData.ConsoleData[ConsoleNr].ProgramEndTimeHours, AxumData.ConsoleData[ConsoleNr].ProgramEndTimeMinutes, AxumData.ConsoleData[ConsoleNr].ProgramEndTimeSeconds);
              }
              else
              {
                sprintf(LCDText, "xx:%02d:%02d", AxumData.ConsoleData[ConsoleNr].ProgramEndTimeMinutes, AxumData.ConsoleData[ConsoleNr].ProgramEndTimeSeconds);
              }
              if (!AxumData.ConsoleData[ConsoleNr].ProgramEndTimeEnable)
              {
                sprintf(LCDText, "No end time!");
              }
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, strlen(LCDText), data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_PROGRAM_ENDTIME_HOURS:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].ProgramEndTimeHours;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_PROGRAM_ENDTIME_MINUTES:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].ProgramEndTimeMinutes;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_PROGRAM_ENDTIME_SECONDS:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              data.UInt = AxumData.ConsoleData[ConsoleNr].ProgramEndTimeSeconds;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_COUNT_DOWN_TIMER:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              data.Float = AxumData.ConsoleData[ConsoleNr].CountDownTimer;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_MODULE_SELECT_ACTIVE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if (AxumData.ConsoleData[ConsoleNr].SelectedModuleTimeout)
              {
                data.State = 1;
              }
              else
              {
                data.State = 0;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_BUSS_SELECT_ACTIVE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if (AxumData.ConsoleData[ConsoleNr].SelectedBussTimeout)
              {
                data.State = 1;
              }
              else
              {
                data.State = 0;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_MONITOR_BUSS_SELECT_ACTIVE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if (AxumData.ConsoleData[ConsoleNr].SelectedMonitorBussTimeout)
              {
                data.State = 1;
              }
              else
              {
                data.State = 0;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_SOURCE_SELECT_ACTIVE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if (AxumData.ConsoleData[ConsoleNr].SelectedSourceTimeout)
              {
                data.State = 1;
              }
              else
              {
                data.State = 0;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case CONSOLE_FUNCTION_DESTINATION_SELECT_ACTIVE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              if (AxumData.ConsoleData[ConsoleNr].SelectedDestinationTimeout)
              {
                data.State = 1;
              }
              else
              {
                data.State = 0;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
      }
    }
    break;
    case GLOBAL_FUNCTIONS:
    {   //Global
      unsigned int GlobalNr = (SensorReceiveFunctionNumber>>12)&0xFFF;
      unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;

      if (GlobalNr == 0)
      {
        switch (FunctionNr)
        {
          case GLOBAL_FUNCTION_REDLIGHT_1:
          case GLOBAL_FUNCTION_REDLIGHT_2:
          case GLOBAL_FUNCTION_REDLIGHT_3:
          case GLOBAL_FUNCTION_REDLIGHT_4:
          case GLOBAL_FUNCTION_REDLIGHT_5:
          case GLOBAL_FUNCTION_REDLIGHT_6:
          case GLOBAL_FUNCTION_REDLIGHT_7:
          case GLOBAL_FUNCTION_REDLIGHT_8:
          {
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = AxumData.Redlight[FunctionNr-GLOBAL_FUNCTION_REDLIGHT_1];
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case GLOBAL_FUNCTION_CONSOLE_PRESET_1:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_2:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_3:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_4:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_5:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_6:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_7:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_8:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_9:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_10:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_11:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_12:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_13:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_14:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_15:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_16:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_17:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_18:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_19:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_20:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_21:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_22:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_23:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_24:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_25:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_26:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_27:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_28:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_29:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_30:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_31:
          case GLOBAL_FUNCTION_CONSOLE_PRESET_32:
          {
            unsigned int PresetNr = FunctionNr-GLOBAL_FUNCTION_CONSOLE_PRESET_1+1;
            unsigned char ConsoleActiveBits = 0;
            unsigned char PresetActiveBits = 0;

            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                for (int cntConsole=0; cntConsole<4; cntConsole++)
                {
                  if (AxumData.ConsolePresetData[PresetNr-1].Console[cntConsole])
                  {
                    ConsoleActiveBits |= 0x01<<cntConsole;

                    if (AxumData.ConsoleData[cntConsole].SelectedConsolePreset == PresetNr)
                    {
                      PresetActiveBits |= 0x01<<cntConsole;
                    }
                  }
                }

                if (ConsoleActiveBits == PresetActiveBits)
                {
                  data.State = 1;
                }
                else
                {
                  data.State = 0;
                }

                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
            }
          }
          break;
          case GLOBAL_FUNCTION_INITIALIZATION_STATUS:
          {
            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_STATE:
              {
                data.State = (AxumData.PercentInitialized == 100) ? 1 : 0;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
              }
              break;
              case MBN_DATATYPE_UINT:
              {
                data.UInt = AxumData.PercentInitialized;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, 1, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                if (AxumData.PercentInitialized < 100)
                {
                  sprintf(LCDText, "%d%%", AxumData.PercentInitialized);
                }
                else
                {
                  sprintf(LCDText, "Ready");
                }
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, strlen(LCDText), data, 1);
              }
              break;
            }
          }
          break;
        }
      }
    }
    break;
    case SOURCE_FUNCTIONS:
    {   //Source
      int SourceNr = ((SensorReceiveFunctionNumber>>12)&0xFFF);
      unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;
      unsigned char Active = 0;

      if ((SourceNr >= NUMBER_OF_SOURCES) && (SourceNr<(NUMBER_OF_SOURCES+4)))
      {
        SourceNr = AxumData.ConsoleData[SourceNr-NUMBER_OF_SOURCES].SelectedSource;
      }

      switch (FunctionNr)
      {
        case SOURCE_FUNCTION_MODULE_ON:
        case SOURCE_FUNCTION_MODULE_OFF:
        case SOURCE_FUNCTION_MODULE_ON_OFF:
        {
          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].On)
              {
                Active = 1;
              }
            }
          }

          if (FunctionNr == SOURCE_FUNCTION_MODULE_OFF)
          {
            data.State = !Active;
          }
          else
          {
            data.State = Active;
          }
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_MODULE_FADER_ON:
        case SOURCE_FUNCTION_MODULE_FADER_OFF:
        case SOURCE_FUNCTION_MODULE_FADER_ON_OFF:
        {
          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].FaderLevel>-80)
              {
                Active = 1;
              }
            }
          }

          if (FunctionNr == SOURCE_FUNCTION_MODULE_FADER_OFF)
          {
            data.State = !Active;
          }
          else
          {
            data.State = Active;
          }
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE:
        case SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE:
        case SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE:
        {
          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].FaderLevel>-80)
              {
                if (AxumData.ModuleData[cntModule].On)
                {
                  Active = 1;
                }
              }
            }
          }

          if (FunctionNr == SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE)
          {
            data.State = !Active;
          }
          else
          {
            data.State = Active;
          }
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_MODULE_BUSS_1_2_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_3_4_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_5_6_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_7_8_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_9_10_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_11_12_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_13_14_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_15_16_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_17_18_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_19_20_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_21_22_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_23_24_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_25_26_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_27_28_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_29_30_ON:
        case SOURCE_FUNCTION_MODULE_BUSS_31_32_ON:
        {
          int BussNr = (FunctionNr-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON)/(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON);

          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].Buss[BussNr].On)
              {
                Active = 1;
              }
            }
          }

          data.State = Active;
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_5_6_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_7_8_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_9_10_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_11_12_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_13_14_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_15_16_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_17_18_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_19_20_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_21_22_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_23_24_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_25_26_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_27_28_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_29_30_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_31_32_OFF:
        {
          int BussNr = (FunctionNr-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF)/(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF);

          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].Buss[BussNr].On)
              {
                Active = 1;
              }
            }
          }

          data.State = !Active;
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_5_6_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_7_8_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_9_10_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_11_12_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_13_14_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_15_16_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_17_18_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_19_20_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_21_22_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_23_24_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_25_26_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_27_28_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_29_30_ON_OFF:
        case SOURCE_FUNCTION_MODULE_BUSS_31_32_ON_OFF:
        {
          int BussNr = (FunctionNr-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF)/(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF);

          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].Buss[BussNr].On)
              {
                Active = 1;
              }
            }
          }
          data.State = Active;
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_MODULE_COUGH_ON_OFF:
        {
          Active = 0;
          for (int cntModule=0; cntModule<128; cntModule++)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              if (AxumData.ModuleData[cntModule].Cough)
              {
                Active = 1;
              }
            }
          }

          data.State = Active;
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_START:
        case SOURCE_FUNCTION_STOP:
        case SOURCE_FUNCTION_START_STOP:
        {
          if (FunctionNr == SOURCE_FUNCTION_STOP)
          {
            data.State = !AxumData.SourceData[SourceNr].Start;
          }
          else
          {
            data.State = AxumData.SourceData[SourceNr].Start;
          }
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_PHANTOM:
        {
          data.State = AxumData.SourceData[SourceNr].Phantom;
          mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
        }
        break;
        case SOURCE_FUNCTION_PAD:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.SourceData[SourceNr].Pad;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case SOURCE_FUNCTION_GAIN:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_FLOAT:
            {
              float Level = AxumData.SourceData[SourceNr].Gain;
              if (Level<InfoObjectToSend->ActuatorDataMinimal)
              {
                Level = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Level>InfoObjectToSend->ActuatorDataMaximal)
              {
                Level = InfoObjectToSend->ActuatorDataMaximal;
              }
              data.Float = Level;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case SOURCE_FUNCTION_ALERT:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.SourceData[SourceNr].Alert;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case SOURCE_FUNCTION_SELECT_1:
        case SOURCE_FUNCTION_SELECT_2:
        case SOURCE_FUNCTION_SELECT_3:
        case SOURCE_FUNCTION_SELECT_4:
        {
          int ConsoleNr = FunctionNr-SOURCE_FUNCTION_SELECT_1;
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = 0;
              if (SourceNr == (int)AxumData.ConsoleData[ConsoleNr].SelectedSource)
              {
                data.State = 1;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case SOURCE_FUNCTION_LABEL:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              data.Octets = (unsigned char *)AxumData.SourceData[SourceNr].SourceName;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case SOURCE_FUNCTION_COUGH_COMM_1:
        case SOURCE_FUNCTION_COUGH_COMM_2:
        {
          unsigned char CommNr = FunctionNr-SOURCE_FUNCTION_COUGH_COMM_1;
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.SourceData[SourceNr].CoughComm[CommNr];
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
          }
        }
        break;
      }
    }
    break;
    case DESTINATION_FUNCTIONS:
    {   //Destination
      unsigned int DestinationNr = ((SensorReceiveFunctionNumber>>12)&0xFFF);
      unsigned int FunctionNr = SensorReceiveFunctionNumber&0xFFF;
      unsigned int Active = 0;

      if ((DestinationNr >= NUMBER_OF_DESTINATIONS) && (DestinationNr<(NUMBER_OF_DESTINATIONS+4)))
      {
        DestinationNr = AxumData.ConsoleData[DestinationNr-NUMBER_OF_DESTINATIONS].SelectedDestination;
      }

      switch (FunctionNr)
      {
        case DESTINATION_FUNCTION_LABEL:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              data.Octets = (unsigned char *)AxumData.DestinationData[DestinationNr].DestinationName;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_SOURCE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              GetSourceLabel(AxumData.DestinationData[DestinationNr].Source, LCDText, 8);

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_MONITOR_SPEAKER_LEVEL:
        {
          if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
          {
            int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;

            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_UINT:
              {
                int dB = ((AxumData.Monitor[MonitorBussNr].SpeakerLevel-10)*10)+1400;
                if (dB<0)
                {
                  dB = 0;
                }
                else if (dB>=1500)
                {
                  dB = 1499;
                }
                int Position = dB2Position[dB];
                Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
                if (Position<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Position = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Position>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Position = InfoObjectToSend->ActuatorDataMaximal;
                }

                data.UInt = Position;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                sprintf(LCDText, " %4.0f dB", AxumData.Monitor[MonitorBussNr].SpeakerLevel);
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
              case MBN_DATATYPE_FLOAT:
              {
                float Level = AxumData.Monitor[MonitorBussNr].SpeakerLevel;
                if (Level<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Level = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Level>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Level = InfoObjectToSend->ActuatorDataMaximal;
                }
                data.Float = Level;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
              }
              break;
            }
          }
        }
        break;
        case DESTINATION_FUNCTION_MONITOR_PHONES_LEVEL:
        {
          if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
          {
            int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;

            switch (InfoObjectToSend->ActuatorDataType)
            {
              case MBN_DATATYPE_UINT:
              {
                int dB = 0;
                dB = ((AxumData.Monitor[MonitorBussNr].PhonesLevel-10)*10)+1400;

                if (dB<0)
                {
                  dB = 0;
                }
                else if (dB>=1500)
                {
                  dB = 1499;
                }
                int Position = dB2Position[dB];
                Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
                if (Position<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Position = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Position>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Position = InfoObjectToSend->ActuatorDataMaximal;
                }

                data.UInt = Position;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
              }
              break;
              case MBN_DATATYPE_OCTETS:
              {
                sprintf(LCDText, " %4.0f dB", AxumData.Monitor[MonitorBussNr].PhonesLevel);
                data.Octets = (unsigned char *)LCDText;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
              }
              break;
              case MBN_DATATYPE_FLOAT:
              {
                float Level = AxumData.Monitor[MonitorBussNr].PhonesLevel;
                if (Level<InfoObjectToSend->ActuatorDataMinimal)
                {
                  Level = InfoObjectToSend->ActuatorDataMinimal;
                }
                else if (Level>InfoObjectToSend->ActuatorDataMaximal)
                {
                  Level = InfoObjectToSend->ActuatorDataMaximal;
                }
                data.Float = Level;
                mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
              }
              break;
            }
          }
        }
        break;
        case DESTINATION_FUNCTION_LEVEL:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_UINT:
            {
              int dB = 0;
              dB = ((AxumData.DestinationData[DestinationNr].Level)*10)+1400;

              if (dB<0)
              {
                dB = 0;
              }
              else if (dB>=1500)
              {
                dB = 1499;
              }
              int Position = dB2Position[dB];
              Position = ((dB2Position[dB]*(InfoObjectToSend->ActuatorDataMaximal-InfoObjectToSend->ActuatorDataMinimal))/1023)+InfoObjectToSend->ActuatorDataMinimal;
              if (Position<InfoObjectToSend->ActuatorDataMinimal)
              {
                Position = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Position>InfoObjectToSend->ActuatorDataMaximal)
              {
                Position = InfoObjectToSend->ActuatorDataMaximal;
              }

              data.UInt = Position;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_UINT, InfoObjectToSend->ActuatorDataSize, data, 1);
            }
            break;
            case MBN_DATATYPE_OCTETS:
            {
              sprintf(LCDText, " %4.0f dB", AxumData.DestinationData[DestinationNr].Level);
              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
            case MBN_DATATYPE_FLOAT:
            {
              float Level = AxumData.DestinationData[DestinationNr].Level;
              if (Level<InfoObjectToSend->ActuatorDataMinimal)
              {
                Level = InfoObjectToSend->ActuatorDataMinimal;
              }
              else if (Level>InfoObjectToSend->ActuatorDataMaximal)
              {
                Level = InfoObjectToSend->ActuatorDataMaximal;
              }
              data.Float = Level;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_FLOAT, 2, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_MUTE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.DestinationData[DestinationNr].Mute;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_MUTE_AND_MONITOR_MUTE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              Active = AxumData.DestinationData[DestinationNr].Mute;

              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
              {
                int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;
                if (AxumData.Monitor[MonitorBussNr].Mute)
                {
                  Active = 1;
                }
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_DIM:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.DestinationData[DestinationNr].Dim;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              Active = AxumData.DestinationData[DestinationNr].Dim;

              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
              {
                int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;
                if (AxumData.Monitor[MonitorBussNr].Dim)
                {
                  Active = 1;
                }
              }
              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.buss))
              {
                int BussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.buss;
                if (AxumData.BussMasterData[BussNr].Dim)
                {
                  Active = 1;
                }
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_MONO:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              Active = AxumData.DestinationData[DestinationNr].Mono;

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_MONO_AND_MONITOR_MONO:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              Active = AxumData.DestinationData[DestinationNr].Mono;

              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
              {
                int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;
                if (AxumData.Monitor[MonitorBussNr].Mono)
                {
                  Active = 1;
                }
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_PHASE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.DestinationData[DestinationNr].Phase;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_PHASE_AND_MONITOR_PHASE:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              Active = AxumData.DestinationData[DestinationNr].Phase;

              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
              {
                int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;
                if (AxumData.Monitor[MonitorBussNr].Phase)
                {
                  Active = 1;
                }
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_TALKBACK_1:
        case DESTINATION_FUNCTION_TALKBACK_2:
        case DESTINATION_FUNCTION_TALKBACK_3:
        case DESTINATION_FUNCTION_TALKBACK_4:
        case DESTINATION_FUNCTION_TALKBACK_5:
        case DESTINATION_FUNCTION_TALKBACK_6:
        case DESTINATION_FUNCTION_TALKBACK_7:
        case DESTINATION_FUNCTION_TALKBACK_8:
        case DESTINATION_FUNCTION_TALKBACK_9:
        case DESTINATION_FUNCTION_TALKBACK_10:
        case DESTINATION_FUNCTION_TALKBACK_11:
        case DESTINATION_FUNCTION_TALKBACK_12:
        case DESTINATION_FUNCTION_TALKBACK_13:
        case DESTINATION_FUNCTION_TALKBACK_14:
        case DESTINATION_FUNCTION_TALKBACK_15:
        case DESTINATION_FUNCTION_TALKBACK_16:
        {
          int TalkbackNr = (FunctionNr-DESTINATION_FUNCTION_TALKBACK_1)/(DESTINATION_FUNCTION_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1);
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = AxumData.DestinationData[DestinationNr].Talkback[TalkbackNr];
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1:
        case DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2:
        case DESTINATION_FUNCTION_TALKBACK_3_AND_MONITOR_TALKBACK_3:
        case DESTINATION_FUNCTION_TALKBACK_4_AND_MONITOR_TALKBACK_4:
        case DESTINATION_FUNCTION_TALKBACK_5_AND_MONITOR_TALKBACK_5:
        case DESTINATION_FUNCTION_TALKBACK_6_AND_MONITOR_TALKBACK_6:
        case DESTINATION_FUNCTION_TALKBACK_7_AND_MONITOR_TALKBACK_7:
        case DESTINATION_FUNCTION_TALKBACK_8_AND_MONITOR_TALKBACK_8:
        case DESTINATION_FUNCTION_TALKBACK_9_AND_MONITOR_TALKBACK_9:
        case DESTINATION_FUNCTION_TALKBACK_10_AND_MONITOR_TALKBACK_10:
        case DESTINATION_FUNCTION_TALKBACK_11_AND_MONITOR_TALKBACK_11:
        case DESTINATION_FUNCTION_TALKBACK_12_AND_MONITOR_TALKBACK_12:
        case DESTINATION_FUNCTION_TALKBACK_13_AND_MONITOR_TALKBACK_13:
        case DESTINATION_FUNCTION_TALKBACK_14_AND_MONITOR_TALKBACK_14:
        case DESTINATION_FUNCTION_TALKBACK_15_AND_MONITOR_TALKBACK_15:
        case DESTINATION_FUNCTION_TALKBACK_16_AND_MONITOR_TALKBACK_16:
        {
          int TalkbackNr = (FunctionNr-DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1)/(DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1);

          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              Active = AxumData.DestinationData[DestinationNr].Talkback[TalkbackNr];

              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.monitor_buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.monitor_buss))
              {
                int MonitorBussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.monitor_buss;
                if (AxumData.Monitor[MonitorBussNr].Talkback[TalkbackNr])
                {
                  Active = 1;
                }
              }
              if ((AxumData.DestinationData[DestinationNr].Source>=matrix_sources.src_offset.min.buss) && (AxumData.DestinationData[DestinationNr].Source<=matrix_sources.src_offset.max.buss))
              {
                int BussNr = AxumData.DestinationData[DestinationNr].Source-matrix_sources.src_offset.min.buss;
                if (AxumData.BussMasterData[BussNr].Talkback[TalkbackNr])
                {
                  Active = 1;
                }
              }

              data.State = Active;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_ROUTING:
        {
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_OCTETS:
            {
              switch (AxumData.DestinationData[DestinationNr].Routing)
              {
                case 0:
                {
                  sprintf(LCDText, " Stereo ");
                }
                break;
                case 1:
                {
                  sprintf(LCDText, "Left    ");
                }
                break;
                case 2:
                {
                  sprintf(LCDText, "   Right");
                }
                break;
              }

              data.Octets = (unsigned char *)LCDText;
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);
            }
            break;
          }
        }
        break;
        case DESTINATION_FUNCTION_SELECT_1:
        case DESTINATION_FUNCTION_SELECT_2:
        case DESTINATION_FUNCTION_SELECT_3:
        case DESTINATION_FUNCTION_SELECT_4:
        {
          int ConsoleNr = FunctionNr-DESTINATION_FUNCTION_SELECT_1;
          switch (InfoObjectToSend->ActuatorDataType)
          {
            case MBN_DATATYPE_STATE:
            {
              data.State = 0;
              if (DestinationNr == AxumData.ConsoleData[ConsoleNr].SelectedDestination)
              {
                data.State = 1;
              }
              mbnSetActuatorData(mbn, InfoObjectToSend->MambaNetAddress, InfoObjectToSend->ObjectNr, MBN_DATATYPE_STATE, 1, data, 1);
            }
            break;
          }
        }
        break;
      }
    }
    break;
  }
}


//Initialize object list per function
void InitalizeAllObjectListPerFunction()
{
  //Module
  for (int cntModule=0; cntModule<NUMBER_OF_MODULES; cntModule++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_MODULE_FUNCTIONS; cntFunction++)
    {
      ModuleFunctions[cntModule][cntFunction] = NULL;
    }
  }

  //Buss
  for (int cntBuss=0; cntBuss<NUMBER_OF_BUSSES; cntBuss++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_BUSS_FUNCTIONS; cntFunction++)
    {
      BussFunctions[cntBuss][cntFunction] = NULL;
    }
  }

  //Monitor Buss
  for (int cntBuss=0; cntBuss<NUMBER_OF_MONITOR_BUSSES; cntBuss++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_MONITOR_BUSS_FUNCTIONS; cntFunction++)
    {
      MonitorBussFunctions[cntBuss][cntFunction] = NULL;
    }
  }

  //Console
  for (int cntConsole=0; cntConsole<NUMBER_OF_CONSOLES; cntConsole++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_MONITOR_BUSS_FUNCTIONS; cntFunction++)
    {
      ConsoleFunctions[cntConsole][cntFunction] = NULL;
    }
  }

  //Global
  for (int cntFunction=0; cntFunction<NUMBER_OF_GLOBAL_FUNCTIONS; cntFunction++)
  {
    GlobalFunctions[cntFunction] = NULL;
  }

  //Source
  for (int cntSource=0; cntSource<NUMBER_OF_SOURCES; cntSource++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_SOURCE_FUNCTIONS; cntFunction++)
    {
      SourceFunctions[cntSource][cntFunction] = NULL;
    }
  }

  //Destination
  for (int cntDestination=0; cntDestination<NUMBER_OF_DESTINATIONS; cntDestination++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_DESTINATION_FUNCTIONS; cntFunction++)
    {
      DestinationFunctions[cntDestination][cntFunction] = NULL;
    }
  }
}

//Make object list per functions
void MakeObjectListPerFunction(unsigned int SensorReceiveFunctionNumber)
{
  unsigned char FunctionType = (SensorReceiveFunctionNumber>>24)&0xFF;
  unsigned int FunctionNumber = (SensorReceiveFunctionNumber>>12)&0xFFF;
  unsigned int Function = SensorReceiveFunctionNumber&0xFFF;
  AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = NULL;

  //Clear function list
  switch (FunctionType)
  {
    case MODULE_FUNCTIONS:
    {   //Module
      WalkAxumFunctionInformationStruct = ModuleFunctions[FunctionNumber][Function];
      ModuleFunctions[FunctionNumber][Function] = NULL;
    }
    break;
    case BUSS_FUNCTIONS:
    {   //Buss
      WalkAxumFunctionInformationStruct = BussFunctions[FunctionNumber][Function];
      BussFunctions[FunctionNumber][Function] = NULL;
    }
    break;
    case MONITOR_BUSS_FUNCTIONS:
    {   //Monitor Buss
      WalkAxumFunctionInformationStruct = MonitorBussFunctions[FunctionNumber][Function];
      MonitorBussFunctions[FunctionNumber][Function] = NULL;
    }
    break;
    case CONSOLE_FUNCTIONS:
    {   //Console
      WalkAxumFunctionInformationStruct = ConsoleFunctions[FunctionNumber][Function];
      ConsoleFunctions[FunctionNumber][Function] = NULL;
    }
    break;
    case GLOBAL_FUNCTIONS:
    {   //Global
      WalkAxumFunctionInformationStruct = GlobalFunctions[Function];
      GlobalFunctions[Function] = NULL;
    }
    break;
    case SOURCE_FUNCTIONS:
    {   //Source
      WalkAxumFunctionInformationStruct = SourceFunctions[FunctionNumber][Function];
      SourceFunctions[FunctionNumber][Function] = NULL;
    }
    break;
    case DESTINATION_FUNCTIONS:
    {   //Destination
      WalkAxumFunctionInformationStruct = DestinationFunctions[FunctionNumber][Function];
      DestinationFunctions[FunctionNumber][Function] = NULL;
    }
    break;
  }
  while (WalkAxumFunctionInformationStruct != NULL)
  {
    AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
    WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
    delete AxumFunctionInformationStructToDelete;
  }

  ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = OnlineNodeInformationList;
  while (OnlineNodeInformationElement != NULL)
  {
    if (OnlineNodeInformationElement->MambaNetAddress != 0)
    {
      if (OnlineNodeInformationElement->SensorReceiveFunction != NULL)
      {
        for (int cntObject=0; cntObject<OnlineNodeInformationElement->UsedNumberOfCustomObjects; cntObject++)
        {
          if (OnlineNodeInformationElement->SensorReceiveFunction[cntObject].FunctionNr == SensorReceiveFunctionNumber)
          {
            if (OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataType != MBN_DATATYPE_NODATA)
            {
              AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToAdd = new AXUM_FUNCTION_INFORMATION_STRUCT;

              AxumFunctionInformationStructToAdd->MambaNetAddress = OnlineNodeInformationElement->MambaNetAddress;
              AxumFunctionInformationStructToAdd->ObjectNr = 1024+cntObject;
              AxumFunctionInformationStructToAdd->ActuatorDataType = OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataType;
              AxumFunctionInformationStructToAdd->ActuatorDataSize = OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataSize;
              AxumFunctionInformationStructToAdd->ActuatorDataMinimal = OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataMinimal;
              AxumFunctionInformationStructToAdd->ActuatorDataMaximal = OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataMaximal;
              AxumFunctionInformationStructToAdd->ActuatorDataDefault = OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataDefault;
              AxumFunctionInformationStructToAdd->ActuatorData = OnlineNodeInformationElement->ObjectInformation[cntObject].ActuatorDataDefault;
              AxumFunctionInformationStructToAdd->Next = (void *)WalkAxumFunctionInformationStruct;

              switch (FunctionType)
              {
                case MODULE_FUNCTIONS:
                {   //Module
                  ModuleFunctions[FunctionNumber][Function] = AxumFunctionInformationStructToAdd;
                }
                break;
                case BUSS_FUNCTIONS:
                {   //Buss
                  BussFunctions[FunctionNumber][Function] = AxumFunctionInformationStructToAdd;
                }
                break;
                case MONITOR_BUSS_FUNCTIONS:
                {   //Monitor Buss
                  MonitorBussFunctions[FunctionNumber][Function] = AxumFunctionInformationStructToAdd;
                }
                break;
                case CONSOLE_FUNCTIONS:
                {   //Console
                  ConsoleFunctions[FunctionNumber][Function] = AxumFunctionInformationStructToAdd;
                }
                break;
                case GLOBAL_FUNCTIONS:
                {   //Global
                  GlobalFunctions[Function] = AxumFunctionInformationStructToAdd;
                }
                break;
                case SOURCE_FUNCTIONS:
                {   //Source
                  SourceFunctions[FunctionNumber][Function] = AxumFunctionInformationStructToAdd;
                }
                break;
                case DESTINATION_FUNCTIONS:
                {   //Output
                  DestinationFunctions[FunctionNumber][Function] = AxumFunctionInformationStructToAdd;
                }
                break;
              }
              WalkAxumFunctionInformationStruct = AxumFunctionInformationStructToAdd;
            }
          }
        }
      }
    }
    OnlineNodeInformationElement = OnlineNodeInformationElement->Next;
  }

  //Debug print the function list
  WalkAxumFunctionInformationStruct = NULL;
  switch (FunctionType)
  {
    case MODULE_FUNCTIONS:
    {   //Module
      WalkAxumFunctionInformationStruct = ModuleFunctions[FunctionNumber][Function];
    }
    break;
    case BUSS_FUNCTIONS:
    {   //Buss
      WalkAxumFunctionInformationStruct = BussFunctions[FunctionNumber][Function];
    }
    break;
    case MONITOR_BUSS_FUNCTIONS:
    {   //Monitor Buss
      WalkAxumFunctionInformationStruct = MonitorBussFunctions[FunctionNumber][Function];
    }
    break;
    case CONSOLE_FUNCTIONS:
    {   //Console
      WalkAxumFunctionInformationStruct = ConsoleFunctions[FunctionNumber][Function];
    }
    break;
    case GLOBAL_FUNCTIONS:
    {   //Global
      WalkAxumFunctionInformationStruct = GlobalFunctions[Function];
    }
    break;
    case SOURCE_FUNCTIONS:
    {   //Source
      WalkAxumFunctionInformationStruct = SourceFunctions[FunctionNumber][Function];
    }
    break;
    case DESTINATION_FUNCTIONS:
    {   //Destination
      WalkAxumFunctionInformationStruct = DestinationFunctions[FunctionNumber][Function];
    }
    break;
  }
  while (WalkAxumFunctionInformationStruct != NULL)
  {
    WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
  }
}

//Delete all object list per function
void DeleteAllObjectListPerFunction()
{
  //Module
  for (int cntModule=0; cntModule<NUMBER_OF_MODULES; cntModule++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_MODULE_FUNCTIONS; cntFunction++)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = ModuleFunctions[cntModule][cntFunction];
      ModuleFunctions[cntModule][cntFunction] = NULL;

      while (WalkAxumFunctionInformationStruct != NULL)
      {
        AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
        WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
        delete AxumFunctionInformationStructToDelete;
      }
    }
  }

  //Buss
  for (int cntBuss=0; cntBuss<NUMBER_OF_BUSSES; cntBuss++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_BUSS_FUNCTIONS; cntFunction++)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = BussFunctions[cntBuss][cntFunction];
      BussFunctions[cntBuss][cntFunction] = NULL;

      while (WalkAxumFunctionInformationStruct != NULL)
      {
        AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
        WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
        delete AxumFunctionInformationStructToDelete;
      }
    }
  }

  //Monitor Buss
  for (int cntBuss=0; cntBuss<NUMBER_OF_MONITOR_BUSSES; cntBuss++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_MONITOR_BUSS_FUNCTIONS; cntFunction++)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = MonitorBussFunctions[cntBuss][cntFunction];
      MonitorBussFunctions[cntBuss][cntFunction] = NULL;

      while (WalkAxumFunctionInformationStruct != NULL)
      {
        AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
        WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
        delete AxumFunctionInformationStructToDelete;
      }
    }
  }

  //Console
  for (int cntConsole=0; cntConsole<NUMBER_OF_CONSOLES; cntConsole++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_GLOBAL_FUNCTIONS; cntFunction++)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = ConsoleFunctions[cntConsole][cntFunction];
      ConsoleFunctions[cntConsole][cntFunction] = NULL;

      while (WalkAxumFunctionInformationStruct != NULL)
      {
        AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
        WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
        delete AxumFunctionInformationStructToDelete;
      }
    }
  }

  //Global
  for (int cntFunction=0; cntFunction<NUMBER_OF_GLOBAL_FUNCTIONS; cntFunction++)
  {
    AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = GlobalFunctions[cntFunction];
    GlobalFunctions[cntFunction] = NULL;

    while (WalkAxumFunctionInformationStruct != NULL)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
      WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
      delete AxumFunctionInformationStructToDelete;
    }
  }

  //Source
  for (int cntSource=0; cntSource<NUMBER_OF_SOURCES; cntSource++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_SOURCE_FUNCTIONS; cntFunction++)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = SourceFunctions[cntSource][cntFunction];
      SourceFunctions[cntSource][cntFunction] = NULL;

      while (WalkAxumFunctionInformationStruct != NULL)
      {
        AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
        WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
        delete AxumFunctionInformationStructToDelete;
      }
    }
  }

  //Destination
  for (int cntDestination=0; cntDestination<NUMBER_OF_DESTINATIONS; cntDestination++)
  {
    for (int cntFunction=0; cntFunction<NUMBER_OF_DESTINATION_FUNCTIONS; cntFunction++)
    {
      AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = DestinationFunctions[cntDestination][cntFunction];
      DestinationFunctions[cntDestination][cntFunction] = NULL;

      while (WalkAxumFunctionInformationStruct != NULL)
      {
        AXUM_FUNCTION_INFORMATION_STRUCT *AxumFunctionInformationStructToDelete = WalkAxumFunctionInformationStruct;
        WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
        delete AxumFunctionInformationStructToDelete;
      }
    }
  }
}

void ModeControllerSensorChange(unsigned int SensorReceiveFunctionNr, unsigned char type, mbn_data data, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  unsigned int ModuleNr = (SensorReceiveFunctionNr>>12)&0xFFF;
  unsigned int FunctionNr = SensorReceiveFunctionNr&0xFFF;
  char ControlMode = -1;
  unsigned char ControlNr;

  if (FunctionNr == MODULE_FUNCTION_CONTROL)
  {
    ControlNr = AxumData.ModuleData[ModuleNr].Console;
  }
  else
  {
    ControlNr = (FunctionNr-MODULE_FUNCTION_CONTROL_1)/(MODULE_FUNCTION_CONTROL_2-MODULE_FUNCTION_CONTROL_1);
  }

  AxumData.ConsoleData[ControlNr].ControlModeTimerValue = 0;

  ControlMode = AxumData.ConsoleData[ControlNr].ControlMode;

  if (type == MBN_DATATYPE_SINT)
  {
    switch (ControlMode)
    {
      case MODULE_CONTROL_MODE_SOURCE:
      {   //Source
        int CurrentSource = AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr];
        unsigned char Pool = 8;
        if (AxumData.ConsoleData[ControlNr].SourcePool < 2)
        {
          Pool = (ControlNr*2)+AxumData.ConsoleData[ControlNr].SourcePool;
        }
        AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr] = AdjustModuleSource(CurrentSource, data.SInt, Pool);

        if (AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr] != CurrentSource)
        {
          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);
          DoAxum_UpdateModuleControlModeLabel(ModuleNr, ControlMode);
        }
      }
      break;
      case MODULE_CONTROL_MODE_MODULE_PRESET:
      {
        int CurrentPreset = AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr];
        unsigned char Pool = 8;
        if (AxumData.ConsoleData[ControlNr].PresetPool < 2)
        {
          Pool = (ControlNr*2)+AxumData.ConsoleData[ControlNr].PresetPool;
        }
        AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr] = AdjustModulePreset(CurrentPreset, data.SInt, Pool);

        if (AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr] != CurrentPreset)
        {
          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);
          DoAxum_UpdateModuleControlModeLabel(ModuleNr, ControlMode);
        }
      }
      break;
      case MODULE_CONTROL_MODE_SOURCE_GAIN:
      {   //Source gain
        if ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source))
        {
          int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
          unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

          if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
          {
            float min, max, def;
            CheckObjectRange(FunctionNrToSent | SOURCE_FUNCTION_GAIN, &min, &max, &def);

            float OldLevel = AxumData.SourceData[SourceNr].Gain;

            AxumData.SourceData[SourceNr].Gain += (float)data.SInt/10;
            if (AxumData.SourceData[SourceNr].Gain < min)
            {
              AxumData.SourceData[SourceNr].Gain = min;
            }
            else if (AxumData.SourceData[SourceNr].Gain > max)
            {
              AxumData.SourceData[SourceNr].Gain = max;
            }

            if (AxumData.SourceData[SourceNr].Gain != OldLevel)
            {
              unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);
              CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_GAIN);

              for (int cntModule=0; cntModule<128; cntModule++)
              {
                if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
                {
                  unsigned int FunctionNrToSent = (cntModule<<12);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_GAIN_LEVEL);

                  DoAxum_UpdateModuleControlMode(cntModule, ControlMode);
                }
              }
            }
          }
        }
      }
      break;
      case MODULE_CONTROL_MODE_GAIN:
      {   //Gain
        float OldLevel = AxumData.ModuleData[ModuleNr].Gain;

        AxumData.ModuleData[ModuleNr].Gain += (float)data.SInt/10;
        if (AxumData.ModuleData[ModuleNr].Gain < -20)
        {
          AxumData.ModuleData[ModuleNr].Gain = -20;
        }
        else if (AxumData.ModuleData[ModuleNr].Gain > 20)
        {
          AxumData.ModuleData[ModuleNr].Gain = 20;
        }

        if (AxumData.ModuleData[ModuleNr].Gain != OldLevel)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_GAIN_LEVEL);
        }
      }
      break;
      case MODULE_CONTROL_MODE_PHASE:
      {   //Phase reverse
        unsigned char OldPhase = AxumData.ModuleData[ModuleNr].Phase;

        AxumData.ModuleData[ModuleNr].Phase += data.SInt;
        AxumData.ModuleData[ModuleNr].Phase &= 0x03;

        if (AxumData.ModuleData[ModuleNr].Phase != OldPhase)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE);
        }
      }
      break;
      case MODULE_CONTROL_MODE_LOW_CUT:
      {   //LowCut
        unsigned int OldFrequency = AxumData.ModuleData[ModuleNr].Filter.Frequency;

        if (data.SInt>=0)
        {
          AxumData.ModuleData[ModuleNr].Filter.Frequency *= 1+((float)data.SInt/100);
          if (OldFrequency == AxumData.ModuleData[ModuleNr].Filter.Frequency)
          {
            AxumData.ModuleData[ModuleNr].Filter.Frequency++;
          }
        }
        else
        {
          AxumData.ModuleData[ModuleNr].Filter.Frequency /= 1+((float)-data.SInt/100);
          if (OldFrequency == AxumData.ModuleData[ModuleNr].Filter.Frequency)
          {
            AxumData.ModuleData[ModuleNr].Filter.Frequency--;
          }
        }

        if (AxumData.ModuleData[ModuleNr].Filter.Frequency <= 20)
        {
          AxumData.ModuleData[ModuleNr].Filter.Frequency = 20;
        }
        else if (AxumData.ModuleData[ModuleNr].Filter.Frequency > 15000)
        {
          AxumData.ModuleData[ModuleNr].Filter.Frequency = 15000;
        }

        if (AxumData.ModuleData[ModuleNr].Filter.Frequency != OldFrequency)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_LOW_CUT_FREQUENCY);
        }
      }
      break;
      case MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL:
      case MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL:
      case MODULE_CONTROL_MODE_EQ_BAND_3_LEVEL:
      case MODULE_CONTROL_MODE_EQ_BAND_4_LEVEL:
      case MODULE_CONTROL_MODE_EQ_BAND_5_LEVEL:
      case MODULE_CONTROL_MODE_EQ_BAND_6_LEVEL:
      {   //EQ level
        int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL)/(MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL);
        float OldLevel = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level;

        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level += (float)data.SInt/10;
        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level<-AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = -AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range;
        }
        else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level>AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Range;
        }

        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level != OldLevel)
        {
          SetAxum_EQ(ModuleNr, BandNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_LEVEL+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_LEVEL-MODULE_FUNCTION_EQ_BAND_1_LEVEL))));
        }
      }
      break;
      case MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
      case MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
      case MODULE_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
      case MODULE_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
      case MODULE_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
      case MODULE_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
      {   //EQ frequency
        int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY)/(MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY);
        unsigned int OldFrequency = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency;

        if (data.SInt>=0)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency *= 1+((float)data.SInt/100);
          if (OldFrequency == AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency)
          {
            AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency++;
          }
        }
        else
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency /= 1+((float)-data.SInt/100);
          if (OldFrequency == AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency)
          {
            AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency++;
          }
        }

        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency<20)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = 20;
        }
        else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency>15000)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = 15000;
        }

        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency != OldFrequency)
        {
          SetAxum_EQ(ModuleNr, BandNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_FREQUENCY+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY))));
        }
      }
      break;
      case MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
      case MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
      case MODULE_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
      case MODULE_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
      case MODULE_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
      case MODULE_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
      {   //EQ Bandwidth
        int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH)/(MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH);
        float OldBandwidth = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth;

        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth += (float)data.SInt/10;

        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth<0.1)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = 0.1;
        }
        else if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth>10)
        {
          AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = 10;
        }

        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth != OldBandwidth)
        {
          SetAxum_EQ(ModuleNr, BandNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH))));
        }
      }
      break;
      case MODULE_CONTROL_MODE_EQ_BAND_1_TYPE:
      case MODULE_CONTROL_MODE_EQ_BAND_2_TYPE:
      case MODULE_CONTROL_MODE_EQ_BAND_3_TYPE:
      case MODULE_CONTROL_MODE_EQ_BAND_4_TYPE:
      case MODULE_CONTROL_MODE_EQ_BAND_5_TYPE:
      case MODULE_CONTROL_MODE_EQ_BAND_6_TYPE:
      {   //EQ type
        int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE)/(MODULE_CONTROL_MODE_EQ_BAND_2_TYPE-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE);
        int OldType = AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type;
        int Type = OldType;

        Type += data.SInt;

        if (Type<OFF)
        {
          Type = OFF;
        }
        else if (Type>NOTCH)
        {
          Type = NOTCH;
        }
        AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type = (FilterType)Type;

        if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type != OldType)
        {
          SetAxum_EQ(ModuleNr, BandNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_TYPE+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_TYPE-MODULE_FUNCTION_EQ_BAND_1_TYPE))));
        }
      }
      break;
      case MODULE_CONTROL_MODE_AGC_THRESHOLD:
      {   //Dynamics
        float OldAGCThreshold = AxumData.ModuleData[ModuleNr].AGCThreshold;

        AxumData.ModuleData[ModuleNr].AGCThreshold += ((float)data.SInt/2);
        if (AxumData.ModuleData[ModuleNr].AGCThreshold < -30)
        {
          AxumData.ModuleData[ModuleNr].AGCThreshold = -30;
        }
        else if (AxumData.ModuleData[ModuleNr].AGCThreshold > 0)
        {
          AxumData.ModuleData[ModuleNr].AGCThreshold = 0;
        }

        if (AxumData.ModuleData[ModuleNr].AGCThreshold != OldAGCThreshold)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_AGC_THRESHOLD);
        }
      }
      break;
      case MODULE_CONTROL_MODE_AGC:
      {   //AGC
        float OldAmount = AxumData.ModuleData[ModuleNr].AGCRatio;

        AxumData.ModuleData[ModuleNr].AGCRatio += ((float)data.SInt)/10;
        if (AxumData.ModuleData[ModuleNr].AGCRatio < 1)
        {
          AxumData.ModuleData[ModuleNr].AGCRatio = 1;
        }
        else if (AxumData.ModuleData[ModuleNr].AGCRatio > 25)
        {
          AxumData.ModuleData[ModuleNr].AGCRatio = 25;
        }

        if (AxumData.ModuleData[ModuleNr].AGCRatio != OldAmount)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_AGC_AMOUNT);
        }
      }
      break;
      case MODULE_CONTROL_MODE_EXPANDER_THRESHOLD:
      {   //Downward expander
        float OldExpanderThreshold = AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold;

        AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold += ((float)data.SInt/2);
        if (AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold < -50)
        {
          AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold = -50;
        }
        else if (AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold > 0)
        {
          AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold = 0;
        }

        if (AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold != OldExpanderThreshold)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_EXPANDER_THRESHOLD);
        }
      }
      break;
      case MODULE_CONTROL_MODE_MONO:
      { //Mono
        unsigned char OldMono = AxumData.ModuleData[ModuleNr].Mono;

        AxumData.ModuleData[ModuleNr].Mono += data.SInt;
        AxumData.ModuleData[ModuleNr].Mono &= 0x03;

        if (AxumData.ModuleData[ModuleNr].Mono != OldMono)
        {
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO);
        }
      }
      break;
      case MODULE_CONTROL_MODE_PAN:
      {   //Panorama
        int OldPanorama = AxumData.ModuleData[ModuleNr].Panorama;

        AxumData.ModuleData[ModuleNr].Panorama += data.SInt;
        if (AxumData.ModuleData[ModuleNr].Panorama< 0)
        {
          AxumData.ModuleData[ModuleNr].Panorama = 0;
        }
        else if (AxumData.ModuleData[ModuleNr].Panorama > 1023)
        {
          AxumData.ModuleData[ModuleNr].Panorama = 1023;
        }

        if (AxumData.ModuleData[ModuleNr].Panorama != OldPanorama)
        {
          SetAxum_BussLevels(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PAN);
        }
      }
      break;
      case MODULE_CONTROL_MODE_MODULE_LEVEL:
      {   //Module level
        float CurrentLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

        AxumData.ModuleData[ModuleNr].FaderLevel += data.SInt;
        if (AxumData.ModuleData[ModuleNr].FaderLevel < -140)
        {
          AxumData.ModuleData[ModuleNr].FaderLevel = -140;
        }
        else
        {
          if (AxumData.ModuleData[ModuleNr].FaderLevel > (10-AxumData.LevelReserve))
          {
            AxumData.ModuleData[ModuleNr].FaderLevel = (10-AxumData.LevelReserve);
          }
        }
        float NewLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

        if (CurrentLevel!=NewLevel)
        {
          SetAxum_BussLevels(ModuleNr);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_LEVEL);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);
        }

        if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
            ((CurrentLevel>-80) && (NewLevel<=-80)))
        { //fader on changed
          DoAxum_ModuleStatusChanged(ModuleNr, 1);

          unsigned int FunctionNrToSent = ((ModuleNr)<<12);
          CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
          CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
          CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_OFF);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON_OFF);

          if ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source))
          {
            unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
            FunctionNrToSent = 0x05000000 | (SourceNr<<12);
            CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_ON);
            CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_OFF);
            CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
            CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
            CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
            CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
          }
          unsigned char On = AxumData.ModuleData[ModuleNr].On;
          DoAxum_StartStopTrigger(ModuleNr, CurrentLevel, NewLevel, On, On);
        }
      }
      break;
      case MODULE_CONTROL_MODE_BUSS_1_2:
      case MODULE_CONTROL_MODE_BUSS_3_4:
      case MODULE_CONTROL_MODE_BUSS_5_6:
      case MODULE_CONTROL_MODE_BUSS_7_8:
      case MODULE_CONTROL_MODE_BUSS_9_10:
      case MODULE_CONTROL_MODE_BUSS_11_12:
      case MODULE_CONTROL_MODE_BUSS_13_14:
      case MODULE_CONTROL_MODE_BUSS_15_16:
      case MODULE_CONTROL_MODE_BUSS_17_18:
      case MODULE_CONTROL_MODE_BUSS_19_20:
      case MODULE_CONTROL_MODE_BUSS_21_22:
      case MODULE_CONTROL_MODE_BUSS_23_24:
      case MODULE_CONTROL_MODE_BUSS_25_26:
      case MODULE_CONTROL_MODE_BUSS_27_28:
      case MODULE_CONTROL_MODE_BUSS_29_30:
      case MODULE_CONTROL_MODE_BUSS_31_32:
      {   //Aux
        int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2)/(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2);
        float CurrentLevel = AxumData.ModuleData[ModuleNr].Buss[BussNr].Level;
        unsigned char CurrentPresetState[8];
        unsigned char cntPreset;

        for (cntPreset=0; cntPreset<8; cntPreset++)
        {
          CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
        }

        AxumData.ModuleData[ModuleNr].Buss[BussNr].Level += data.SInt;
        if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level < -140)
        {
          AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = -140;
        }
        else
        {
          if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Level > (10-AxumData.LevelReserve))
          {
            AxumData.ModuleData[ModuleNr].Buss[BussNr].Level = (10-AxumData.LevelReserve);
          }
        }
        float NewLevel = AxumData.ModuleData[ModuleNr].Buss[BussNr].Level;

        if (NewLevel != CurrentLevel)
        {
          SetAxum_BussLevels(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_LEVEL+(BussNr*(MODULE_FUNCTION_BUSS_3_4_LEVEL-MODULE_FUNCTION_BUSS_1_2_LEVEL))));

          if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
              ((CurrentLevel>-80) && (NewLevel<=-80)))
          { //level changed
            DoAxum_ModuleStatusChanged(ModuleNr, 0);
          }
        }
        for (cntPreset=0; cntPreset<8; cntPreset++)
        {
          if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
          {
            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

            //if A or B
            unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
            if (cntPreset == PresetNr)
            {
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
            }
            else if (cntPreset == (PresetNr+1))
            {
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
            }
          }
        }
      }
      break;
      case MODULE_CONTROL_MODE_BUSS_1_2_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_3_4_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_5_6_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_7_8_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_9_10_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_11_12_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_13_14_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_15_16_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_17_18_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_19_20_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_21_22_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_23_24_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_25_26_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_27_28_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_29_30_BALANCE:
      case MODULE_CONTROL_MODE_BUSS_31_32_BALANCE:
      {
        int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE)/(MODULE_CONTROL_MODE_BUSS_3_4_BALANCE-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE);
        int OldBalance = AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance;

        AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance += data.SInt;
        if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance< 0)
        {
          AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = 0;
        }
        else if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance > 1023)
        {
          AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = 1023;
        }

        if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance != OldBalance)
        {
          SetAxum_BussLevels(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = 0x00000000 | (ModuleNr<<12);

          CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_BALANCE+(BussNr*(MODULE_FUNCTION_BUSS_3_4_BALANCE-MODULE_FUNCTION_BUSS_1_2_BALANCE))));
        }
      }
      break;
      case MODULE_CONTROL_MODE_EQ_ON_OFF:
      {
      }
      break;
      case MODULE_CONTROL_MODE_PHANTOM_ON_OFF:
      {
      }
      break;
      case MODULE_CONTROL_MODE_PAD_ON_OFF:
      {
      }
      break;
      case MODULE_CONTROL_MODE_INSERT_ON_OFF:
      {
      }
      break;
      case MODULE_CONTROL_MODE_DYNAMICS_ON_OFF:
      {
      }
      break;
    }
  }
  SetSelectedModule(ControlNr, ModuleNr);

  SensorReceiveFunctionNr = 0;
  type = 0;
  data.State = 0;
  DataType = 0;
  DataSize = 0;
  DataMinimal = 0;
  DataMaximal = 0;
}

void ModeControllerResetSensorChange(unsigned int SensorReceiveFunctionNr, unsigned char type, mbn_data data, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  unsigned int ModuleNr = (SensorReceiveFunctionNr>>12)&0xFFF;
  unsigned int FunctionNr = SensorReceiveFunctionNr&0xFFF;
  char ControlMode = -1;
  unsigned char ControlNr;

  if (FunctionNr == MODULE_FUNCTION_CONTROL_RESET)
  {
    ControlNr = AxumData.ModuleData[ModuleNr].Console;
  }
  else
  {
    ControlNr = (FunctionNr-MODULE_FUNCTION_CONTROL_1_RESET)/(MODULE_FUNCTION_CONTROL_2_RESET-MODULE_FUNCTION_CONTROL_1_RESET);
  }

  AxumData.ConsoleData[ControlNr].ControlModeTimerValue = 0;

  ControlMode = AxumData.ConsoleData[ControlNr].ControlMode;

  if (type == MBN_DATATYPE_STATE)
  {
    if (data.State)
    {
      switch (ControlMode)
      {
        case MODULE_CONTROL_MODE_NONE:
        {
        }
        break;
        case MODULE_CONTROL_MODE_SOURCE:
        {
          if (DoAxum_SetNewSource(ModuleNr, AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr], 0))
          {
            int NewProcessingPreset = 0;
            if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
            {
              unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
              if (AxumData.SourceData[SourceNr].DefaultProcessingPreset<=1280)
              {
                NewProcessingPreset = AxumData.SourceData[SourceNr].DefaultProcessingPreset;
              }
            }
            DoAxum_LoadProcessingPreset(ModuleNr, NewProcessingPreset, 1, 0, 0);
            //Always load module default routing if used...
            DoAxum_LoadRoutingPreset(ModuleNr, 0, 1, 0, 0);

            DoAxum_UpdateModuleControlModeLabel(ModuleNr, ControlMode);
          }
          else
          {
            //AxumData.ModuleData[ModuleNr].WaitingSource = AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr];
          }
        }
        break;
        case MODULE_CONTROL_MODE_MODULE_PRESET:
        {
          bool ModuleActive = false;
          if (AxumData.ModuleData[ModuleNr].On)
          {
            if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
            {
              ModuleActive = 1;
            }
          }

          if (!ModuleActive)
          {
            DoAxum_LoadProcessingPreset(ModuleNr, AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr], 0, 0, 0);
          }
          else
          {
            AxumData.ModuleData[ModuleNr].WaitingProcessingPreset = AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr];
          }
        }
        break;
        case MODULE_CONTROL_MODE_SOURCE_GAIN:
        {   //Source gain
          if ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source))
          {
            int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
            unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

            if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
            {
              float min, max, def;
              CheckObjectRange(FunctionNrToSent | SOURCE_FUNCTION_GAIN, &min, &max, &def);

              if (AxumData.SourceData[SourceNr].Gain != AxumData.SourceData[SourceNr].DefaultGain)
              {
                AxumData.SourceData[SourceNr].Gain = AxumData.SourceData[SourceNr].DefaultGain;

                DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

                unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);
                CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_GAIN);

                for (int cntModule=0; cntModule<128; cntModule++)
                {
                  if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+1))
                  {
                    DoAxum_UpdateModuleControlMode(cntModule, ControlMode);
                    FunctionNrToSent = (cntModule<<12);
                    CheckObjectsToSent(FunctionNrToSent|MODULE_FUNCTION_SOURCE_GAIN_LEVEL);
                  }
                }
              }
            }
          }
        }
        break;
        case MODULE_CONTROL_MODE_GAIN:
        {   //Gain
          if (AxumData.ModuleData[ModuleNr].Gain != 0)
          {
            AxumData.ModuleData[ModuleNr].Gain = 0;
            SetAxum_ModuleProcessing(ModuleNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            FunctionNrToSent = (ModuleNr<<12) | MODULE_FUNCTION_GAIN_LEVEL;
            CheckObjectsToSent(FunctionNrToSent);
          }
        }
        break;
        case MODULE_CONTROL_MODE_PHASE:
        {  //Phase reverse
          if (AxumData.ModuleData[ModuleNr].PhaseOnOff)
          {
            AxumData.ModuleData[ModuleNr].PhaseOnOff = 0;
          }
          else
          {
            AxumData.ModuleData[ModuleNr].PhaseOnOff = 1;
          }
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE);
        }
        break;
        case MODULE_CONTROL_MODE_LOW_CUT:
        {   //LowCut filter
          AxumData.ModuleData[ModuleNr].FilterOnOff = !AxumData.ModuleData[ModuleNr].FilterOnOff;

          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);
          unsigned int FunctionNrToSent = (ModuleNr<<12);

          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_LOW_CUT_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_LOW_CUT_FREQUENCY);
        }
        break;
        case MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL:
        case MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL:
        case MODULE_CONTROL_MODE_EQ_BAND_3_LEVEL:
        case MODULE_CONTROL_MODE_EQ_BAND_4_LEVEL:
        case MODULE_CONTROL_MODE_EQ_BAND_5_LEVEL:
        case MODULE_CONTROL_MODE_EQ_BAND_6_LEVEL:
        {   //EQ Level
          int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL)/(MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL);

          if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level != 0)
          {
            AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level = 0;
            SetAxum_EQ(ModuleNr, BandNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_LEVEL+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_LEVEL-MODULE_FUNCTION_EQ_BAND_1_LEVEL))));
          }
        }
        break;
        case MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
        case MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
        case MODULE_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
        case MODULE_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
        case MODULE_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
        case MODULE_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
        {   //EQ frequency
          int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY)/(MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY);
          unsigned int Frequency = AxumData.ModuleData[ModuleNr].Defaults.EQBand[BandNr].Frequency;
          int CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
          if (CurrentPreset > 0)
          {
            Frequency = AxumData.PresetData[CurrentPreset-1].EQBand[BandNr].Frequency;
          }

          if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency != Frequency)
          {
            AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency = Frequency;

            SetAxum_EQ(ModuleNr, BandNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_FREQUENCY+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY))));
          }
        }
        break;
        case MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
        case MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
        case MODULE_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
        case MODULE_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
        case MODULE_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
        case MODULE_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
        {   //EQ bandwidth
          int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH)/(MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH);
          float Bandwidth = AxumData.ModuleData[ModuleNr].Defaults.EQBand[BandNr].Bandwidth;
          int CurrentPreset = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
          if (CurrentPreset > 0)
          {
            Bandwidth = AxumData.PresetData[CurrentPreset-1].EQBand[BandNr].Bandwidth;
          }

          if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth != Bandwidth)
          {
            AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth = Bandwidth;
            SetAxum_EQ(ModuleNr, BandNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);
            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH))));
          }
        }
        break;
        case MODULE_CONTROL_MODE_EQ_BAND_1_TYPE:
        case MODULE_CONTROL_MODE_EQ_BAND_2_TYPE:
        case MODULE_CONTROL_MODE_EQ_BAND_3_TYPE:
        case MODULE_CONTROL_MODE_EQ_BAND_4_TYPE:
        case MODULE_CONTROL_MODE_EQ_BAND_5_TYPE:
        case MODULE_CONTROL_MODE_EQ_BAND_6_TYPE:
        {   //EQ Type
          int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE)/(MODULE_CONTROL_MODE_EQ_BAND_2_TYPE-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE);

          if (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type != AxumData.ModuleData[ModuleNr].Defaults.EQBand[BandNr].Type)
          {
            AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type = AxumData.ModuleData[ModuleNr].Defaults.EQBand[BandNr].Type;
            SetAxum_EQ(ModuleNr, BandNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_TYPE+(BandNr*(MODULE_FUNCTION_EQ_BAND_2_TYPE-MODULE_FUNCTION_EQ_BAND_1_TYPE))));
          }
        }
        break;
        case MODULE_CONTROL_MODE_AGC_THRESHOLD:
        { //Dynamics
          AxumData.ModuleData[ModuleNr].DynamicsOnOff = !AxumData.ModuleData[ModuleNr].DynamicsOnOff;
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_DYNAMICS_ON_OFF);
        }
        break;
        case MODULE_CONTROL_MODE_AGC:
        { //Dynamics
          AxumData.ModuleData[ModuleNr].DynamicsOnOff = !AxumData.ModuleData[ModuleNr].DynamicsOnOff;
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_DYNAMICS_ON_OFF);
        }
        break;
        case MODULE_CONTROL_MODE_EXPANDER_THRESHOLD:
        {   //Expander
          AxumData.ModuleData[ModuleNr].DynamicsOnOff = !AxumData.ModuleData[ModuleNr].DynamicsOnOff;
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_DYNAMICS_ON_OFF);
        }
        break;
        case MODULE_CONTROL_MODE_MONO:
        {   //Mono
          AxumData.ModuleData[ModuleNr].MonoOnOff = !AxumData.ModuleData[ModuleNr].MonoOnOff;
          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO);
        }
        break;
        case MODULE_CONTROL_MODE_PAN:
        {   //Panorama
          if (AxumData.ModuleData[ModuleNr].Panorama != 512)
          {
            AxumData.ModuleData[ModuleNr].Panorama = 512;
            SetAxum_BussLevels(ModuleNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PAN);
          }
        }
        break;
        case MODULE_CONTROL_MODE_MODULE_LEVEL:
        {   //Module level
          float CurrentLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
          AxumData.ModuleData[ModuleNr].FaderLevel = 0;
          float NewLevel = AxumData.ModuleData[ModuleNr].FaderLevel;

          if (NewLevel != CurrentLevel)
          {
            SetAxum_BussLevels(ModuleNr);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_LEVEL);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            if (((CurrentLevel<=-80) && (NewLevel>-80)) ||
                ((CurrentLevel>-80) && (NewLevel<=-80)))
            { //fader on changed
              DoAxum_ModuleStatusChanged(ModuleNr, 1);

              FunctionNrToSent = ((ModuleNr)<<12);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_OFF);
              CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON_OFF);

              if ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source))
              {
                unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
                FunctionNrToSent = 0x05000000 | (SourceNr<<12);
                CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_ON);
                CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_OFF);
                CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
                CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
                CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
                CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
              }

              unsigned char On = AxumData.ModuleData[ModuleNr].On;
              DoAxum_StartStopTrigger(ModuleNr, CurrentLevel, NewLevel, On, On);
            }
          }
        }
        break;
        case MODULE_CONTROL_MODE_BUSS_1_2:
        case MODULE_CONTROL_MODE_BUSS_3_4:
        case MODULE_CONTROL_MODE_BUSS_5_6:
        case MODULE_CONTROL_MODE_BUSS_7_8:
        case MODULE_CONTROL_MODE_BUSS_9_10:
        case MODULE_CONTROL_MODE_BUSS_11_12:
        case MODULE_CONTROL_MODE_BUSS_13_14:
        case MODULE_CONTROL_MODE_BUSS_15_16:
        case MODULE_CONTROL_MODE_BUSS_17_18:
        case MODULE_CONTROL_MODE_BUSS_19_20:
        case MODULE_CONTROL_MODE_BUSS_21_22:
        case MODULE_CONTROL_MODE_BUSS_23_24:
        case MODULE_CONTROL_MODE_BUSS_25_26:
        case MODULE_CONTROL_MODE_BUSS_27_28:
        case MODULE_CONTROL_MODE_BUSS_29_30:
        case MODULE_CONTROL_MODE_BUSS_31_32:
        {   //Buss
          int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2)/(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2);
          unsigned char NewState = !AxumData.ModuleData[ModuleNr].Buss[BussNr].On;

          DoAxum_SetBussOnOff(ModuleNr, BussNr, NewState, 0);
        }
        break;
        case MODULE_CONTROL_MODE_BUSS_1_2_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_3_4_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_5_6_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_7_8_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_9_10_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_11_12_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_13_14_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_15_16_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_17_18_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_19_20_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_21_22_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_23_24_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_25_26_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_27_28_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_29_30_BALANCE:
        case MODULE_CONTROL_MODE_BUSS_31_32_BALANCE:
        {   //Buss
          int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE)/(MODULE_CONTROL_MODE_BUSS_3_4_BALANCE-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE);

          if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance != 512)
          {
            AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance = 512;
            SetAxum_BussLevels(ModuleNr);

            DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

            unsigned int FunctionNrToSent = (ModuleNr<<12);
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_BALANCE+(BussNr*(MODULE_FUNCTION_BUSS_3_4_BALANCE-MODULE_FUNCTION_BUSS_1_2_BALANCE))));
          }
        }
        break;
        case MODULE_CONTROL_MODE_EQ_ON_OFF:
        {
          AxumData.ModuleData[ModuleNr].EQOnOff = !AxumData.ModuleData[ModuleNr].EQOnOff;

          for (int cntBand=0; cntBand<6; cntBand++)
          {
            SetAxum_EQ(ModuleNr, cntBand);
          }

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_EQ_ON_OFF);
        }
        break;
        case MODULE_CONTROL_MODE_PHANTOM_ON_OFF:
        {
          if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
          {
            unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
            unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

            if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM))
            {
              AxumData.SourceData[SourceNr].Phantom = !AxumData.SourceData[SourceNr].Phantom;

              CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM);

              for (int cntModule=0; cntModule<128; cntModule++)
              {
                if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
                {
                  unsigned int FunctionNrToSent = (cntModule<<12);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_PHANTOM);

                  DoAxum_UpdateModuleControlMode(cntModule, ControlMode);
                }
              }
            }
          }
        }
        break;
        case MODULE_CONTROL_MODE_PAD_ON_OFF:
        {
          if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
          {
            unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
            unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

            if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PAD))
            {
              AxumData.SourceData[SourceNr].Pad = !AxumData.SourceData[SourceNr].Pad;

              unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);
              CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_PAD);

              for (int cntModule=0; cntModule<128; cntModule++)
              {
                if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
                {
                  FunctionNrToSent = (cntModule<<12);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_SOURCE_PAD);

                  DoAxum_UpdateModuleControlMode(cntModule, ControlMode);
                }
              }
            }
          }
        }
        break;
        case MODULE_CONTROL_MODE_INSERT_ON_OFF:
        {
          AxumData.ModuleData[ModuleNr].InsertOnOff = !AxumData.ModuleData[ModuleNr].InsertOnOff;

          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_INSERT_ON_OFF);
        }
        break;
        case MODULE_CONTROL_MODE_DYNAMICS_ON_OFF:
        {
          AxumData.ModuleData[ModuleNr].DynamicsOnOff = !AxumData.ModuleData[ModuleNr].DynamicsOnOff;

          SetAxum_ModuleProcessing(ModuleNr);

          DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

          unsigned int FunctionNrToSent = (ModuleNr<<12);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_DYNAMICS_ON_OFF);
        }
        break;
      }
      SetSelectedModule(ControlNr, ModuleNr);
    }
  }

  SensorReceiveFunctionNr = 0;
  type = 0;
  data.State = NULL;
  DataType = 0;
  DataSize = 0;
  DataMinimal = 0;
  DataMaximal = 0;
}

void ModeControllerSetData(unsigned int SensorReceiveFunctionNr, unsigned int MambaNetAddress, unsigned int ObjectNr, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  unsigned int ModuleNr = (SensorReceiveFunctionNr>>12)&0xFFF;
  unsigned int FunctionNr = SensorReceiveFunctionNr&0xFFF;
  char LCDText[9];
  char ControlMode = -1;
  unsigned char ControlNr;
  mbn_data data;

  if (FunctionNr == MODULE_FUNCTION_CONTROL)
  {
    ControlNr = AxumData.ModuleData[ModuleNr].Console;
  }
  else
  {
    ControlNr = (FunctionNr-MODULE_FUNCTION_CONTROL_1)/(MODULE_FUNCTION_CONTROL_2-MODULE_FUNCTION_CONTROL_1);
  }

  ControlMode = AxumData.ConsoleData[ControlNr].ControlMode;

  switch (ControlMode)
  {
    case MODULE_CONTROL_MODE_NONE:
    {
      GetSourceLabel(AxumData.ModuleData[ModuleNr].SelectedSource, LCDText, 8);
    }
    break;
    case MODULE_CONTROL_MODE_SOURCE:
    {
      GetSourceLabel(AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr], LCDText, 8);
    }
    break;
    case MODULE_CONTROL_MODE_MODULE_PRESET:
    {
      GetPresetLabel(AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr], LCDText, 8);
    }
    break;
    case MODULE_CONTROL_MODE_SOURCE_GAIN:
    {
      if ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source))
      {
        unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
        unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

        if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_GAIN))
        {
          sprintf(LCDText, "%5.1fdB", AxumData.SourceData[SourceNr].Gain);
        }
        else
        {
          sprintf(LCDText, "Not used");
        }
      }
      else
      {
        sprintf(LCDText, "Not used");
      }
    }
    break;
    case MODULE_CONTROL_MODE_GAIN:
    {
      sprintf(LCDText,     "%5.1fdB", AxumData.ModuleData[ModuleNr].Gain);
    }
    break;
    case MODULE_CONTROL_MODE_PHASE:
    {
      if (AxumData.ModuleData[ModuleNr].PhaseOnOff)
      {
        switch (AxumData.ModuleData[ModuleNr].Phase)
        {
          case 0x00:
          {
            sprintf(LCDText, " Normal ");
          }
          break;
          case 0x01:
          {
            sprintf(LCDText, "  Left  ");
          }
          break;
          case 0x02:
          {
            sprintf(LCDText, "  Right ");
          }
          break;
          case 0x03:
          {
            sprintf(LCDText, "  Both  ");
          }
          break;
        }
      }
      else
      {
        sprintf(LCDText, "  Off   ");
      }
    }
    break;
    case MODULE_CONTROL_MODE_LOW_CUT:
    {
      if (!AxumData.ModuleData[ModuleNr].FilterOnOff)
      {
        sprintf(LCDText, "  Off  ");
      }
      else
      {
        sprintf(LCDText, "%5dHz", AxumData.ModuleData[ModuleNr].Filter.Frequency);
      }
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_3_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_4_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_5_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_6_LEVEL:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL)/(MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL);
      sprintf(LCDText, "%5.1fdB", AxumData.ModuleData[ModuleNr].EQBand[BandNr].Level);
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY)/(MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY);
      sprintf(LCDText, "%5dHz", AxumData.ModuleData[ModuleNr].EQBand[BandNr].Frequency);
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH)/(MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH);
      sprintf(LCDText, "%5.1f Q", AxumData.ModuleData[ModuleNr].EQBand[BandNr].Bandwidth);
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_2_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_3_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_4_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_5_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_6_TYPE:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE)/(MODULE_CONTROL_MODE_EQ_BAND_2_TYPE-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE);
      switch (AxumData.ModuleData[ModuleNr].EQBand[BandNr].Type)
      {
      case OFF:
      {
        sprintf(LCDText, "  Off  ");
      }
      break;
      case HPF:
      {
        sprintf(LCDText, "HighPass");
      }
      break;
      case LOWSHELF:
      {
        sprintf(LCDText, "LowShelf");
      }
      break;
      case PEAKINGEQ:
      {
        sprintf(LCDText, "Peaking ");
      }
      break;
      case HIGHSHELF:
      {
        sprintf(LCDText, "Hi Shelf");
      }
      break;
      case LPF:
      {
        sprintf(LCDText, "Low Pass");
      }
      break;
      case BPF:
      {
        sprintf(LCDText, "BandPass");
      }
      break;
      case NOTCH:
      {
        sprintf(LCDText, "  Notch ");
      }
      break;
      }
    }
    break;
    case MODULE_CONTROL_MODE_AGC_THRESHOLD:
    {
      sprintf(LCDText, "%5.1fdB", AxumData.ModuleData[ModuleNr].AGCThreshold);
    }
    break;
    case MODULE_CONTROL_MODE_AGC:
    {
      sprintf(LCDText, " 1:%1.1f ", AxumData.ModuleData[ModuleNr].AGCRatio);
    }
    break;
    case MODULE_CONTROL_MODE_EXPANDER_THRESHOLD:
    {
      sprintf(LCDText, "%5.1fdB", AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold);
    }
    break;
    case MODULE_CONTROL_MODE_MONO:
    {
      if (AxumData.ModuleData[ModuleNr].MonoOnOff)
      {
        switch (AxumData.ModuleData[ModuleNr].Mono)
        {
          case 0x00:
          {
            sprintf(LCDText, " Stereo ");
          }
          break;
          case 0x01:
          {
            sprintf(LCDText, "  Left  ");
          }
          break;
          case 0x02:
          {
            sprintf(LCDText, "  Right ");
          }
          break;
          case 0x03:
          {
            sprintf(LCDText, "  Mono  ");
          }
          break;
        }
      }
      else
      {
        sprintf(LCDText, "   Off  ");
      }
    }
    break;
    case MODULE_CONTROL_MODE_PAN:
    {
      unsigned char Types[4] = {'[','|','|',']'};
      unsigned char Pos = AxumData.ModuleData[ModuleNr].Panorama/128;
      unsigned char Type = (AxumData.ModuleData[ModuleNr].Panorama%128)/32;

      sprintf(LCDText, "        ");
      if (AxumData.ModuleData[ModuleNr].Panorama == 0)
      {
        sprintf(LCDText, "Left    ");
      }
      else if ((AxumData.ModuleData[ModuleNr].Panorama == 511) || (AxumData.ModuleData[ModuleNr].Panorama == 512))
      {
        sprintf(LCDText, " Center ");
      }
      else if (AxumData.ModuleData[ModuleNr].Panorama == 1023)
      {
        sprintf(LCDText, "   Right");
      }
      else
      {
        LCDText[Pos] = Types[Type];
      }
    }
    break;
    case MODULE_CONTROL_MODE_MODULE_LEVEL:
    {
      sprintf(LCDText, " %4.0f dB", AxumData.ModuleData[ModuleNr].FaderLevel);
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_1_2:
    case MODULE_CONTROL_MODE_BUSS_3_4:
    case MODULE_CONTROL_MODE_BUSS_5_6:
    case MODULE_CONTROL_MODE_BUSS_7_8:
    case MODULE_CONTROL_MODE_BUSS_9_10:
    case MODULE_CONTROL_MODE_BUSS_11_12:
    case MODULE_CONTROL_MODE_BUSS_13_14:
    case MODULE_CONTROL_MODE_BUSS_15_16:
    case MODULE_CONTROL_MODE_BUSS_17_18:
    case MODULE_CONTROL_MODE_BUSS_19_20:
    case MODULE_CONTROL_MODE_BUSS_21_22:
    case MODULE_CONTROL_MODE_BUSS_23_24:
    case MODULE_CONTROL_MODE_BUSS_25_26:
    case MODULE_CONTROL_MODE_BUSS_27_28:
    case MODULE_CONTROL_MODE_BUSS_29_30:
    case MODULE_CONTROL_MODE_BUSS_31_32:
    {
      int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2)/(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2);
      if (AxumData.ModuleData[ModuleNr].Buss[BussNr].On)
      {
        sprintf(LCDText, " %4.0f dB", AxumData.ModuleData[ModuleNr].Buss[BussNr].Level);
      }
      else
      {
        sprintf(LCDText, "  Off   ");
      }
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_1_2_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_3_4_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_5_6_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_7_8_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_9_10_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_11_12_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_13_14_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_15_16_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_17_18_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_19_20_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_21_22_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_23_24_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_25_26_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_27_28_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_29_30_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_31_32_BALANCE:
    {
      int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE)/(MODULE_CONTROL_MODE_BUSS_3_4_BALANCE-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE);

      unsigned char Types[4] = {'[','|','|',']'};
      unsigned char Pos = AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance/128;
      unsigned char Type = (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance%128)/32;

      sprintf(LCDText, "        ");
      if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 0)
      {
        sprintf(LCDText, "Left    ");
      }
      else if ((AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 511) || (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 512))
      {
        sprintf(LCDText, " Center ");
      }
      else if (AxumData.ModuleData[ModuleNr].Buss[BussNr].Balance == 1023)
      {
        sprintf(LCDText, "   Right");
      }
      else
      {
        LCDText[Pos] = Types[Type];
      }
    }
    break;
    default:
    {
      sprintf(LCDText, "UNKNOWN ");
    }
    break;
    case MODULE_CONTROL_MODE_EQ_ON_OFF:
    {
      if (AxumData.ModuleData[ModuleNr].EQOnOff)
      {
        sprintf(LCDText, "   On   ");
      }
      else
      {
        sprintf(LCDText, "   Off  ");
      }
    }
    break;
    case MODULE_CONTROL_MODE_PHANTOM_ON_OFF:
    {
      if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
      {
        unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
        unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

        if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PHANTOM))
        {
          if (AxumData.SourceData[SourceNr].Phantom)
          {
            sprintf(LCDText, "   On   ");
          }
          else
          {
            sprintf(LCDText, "   Off  ");
          }
        }
        else
        {
          sprintf(LCDText, "Not used");
        }
      }
      else
      {
        sprintf(LCDText, "Not used");
      }
    }
    break;
    case MODULE_CONTROL_MODE_PAD_ON_OFF:
    {
      if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
      {
        unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
        unsigned int FunctionNrToSent = 0x05000000 | (SourceNr<<12);

        if (NrOfObjectsAttachedToFunction(FunctionNrToSent | SOURCE_FUNCTION_PAD))
        {
          if (AxumData.SourceData[SourceNr].Pad)
          {
            sprintf(LCDText, "   On   ");
          }
          else
          {
            sprintf(LCDText, "   Off  ");
          }
        }
        else
        {
          sprintf(LCDText, "Not used");
        }
      }
      else
      {
        sprintf(LCDText, "Not used");
      }
    }
    break;
    case MODULE_CONTROL_MODE_INSERT_ON_OFF:
    {
      if (AxumData.ModuleData[ModuleNr].InsertOnOff)
      {
        sprintf(LCDText, "   On   ");
      }
      else
      {
        sprintf(LCDText, "   Off  ");
      }
    }
    break;
    case MODULE_CONTROL_MODE_DYNAMICS_ON_OFF:
    {
      if (AxumData.ModuleData[ModuleNr].DynamicsOnOff)
      {
        sprintf(LCDText, "   On   ");
      }
      else
      {
        sprintf(LCDText, "   Off  ");
      }
    }
    break;
  }

  data.Octets = (unsigned char *)LCDText;
  mbnSetActuatorData(mbn, MambaNetAddress, ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);

  SensorReceiveFunctionNr = 0;
  MambaNetAddress = 0;
  ObjectNr = 0;
  DataType = 0;
  DataSize = 0;
  DataMinimal = 0;
  DataMaximal = 0;
}

void ModeControllerSetLabel(unsigned int SensorReceiveFunctionNr, unsigned int MambaNetAddress, unsigned int ObjectNr, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  unsigned int ModuleNr = (SensorReceiveFunctionNr>>12)&0xFFF;
  unsigned int FunctionNr = SensorReceiveFunctionNr&0xFFF;
  char LCDText[9];
  char ControlMode = -1;
  unsigned char ControlNr;
  mbn_data data;

  if (FunctionNr == MODULE_FUNCTION_CONTROL_LABEL)
  {
    ControlNr = AxumData.ModuleData[ModuleNr].Console;
  }
  else
  {
    ControlNr = (FunctionNr-MODULE_FUNCTION_CONTROL_1_LABEL)/(MODULE_FUNCTION_CONTROL_2_LABEL-MODULE_FUNCTION_CONTROL_1_LABEL);
  }

  ControlMode = AxumData.ConsoleData[ControlNr].ControlMode;

  switch (ControlMode)
  {
    case MODULE_CONTROL_MODE_NONE:
    {
      if (ModuleNr<9)
      {
        sprintf(LCDText, " Mod %d  ", ModuleNr+1);
      }
      else if (ModuleNr<99)
      {
        sprintf(LCDText, " Mod %d ", ModuleNr+1);
      }
      else
      {
        sprintf(LCDText, "Mod %d ", ModuleNr+1);
      }
    }
    break;
    case MODULE_CONTROL_MODE_SOURCE:
    {
      if (AxumData.ModuleData[ModuleNr].SelectedSource != AxumData.ModuleData[ModuleNr].TemporySourceControlMode[ControlNr])
      {
        sprintf(LCDText," Source ");
      }
      else
      {
        sprintf(LCDText,">Source<");
      }
    }
    break;
    case MODULE_CONTROL_MODE_MODULE_PRESET:
    {
      if (AxumData.ModuleData[ModuleNr].SelectedProcessingPreset != AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[ControlNr])
      {
        sprintf(LCDText," Preset ");
      }
      else
      {
        sprintf(LCDText,">Preset<");
      }
    }
    break;
    case MODULE_CONTROL_MODE_SOURCE_GAIN:
    {
      sprintf(LCDText,"Src gain");
    }
    break;
    case MODULE_CONTROL_MODE_GAIN:
    {
      sprintf(LCDText,"  Gain  ");
    }
    break;
    case MODULE_CONTROL_MODE_PHASE:
    {
      sprintf(LCDText," Phase  ");
    }
    break;
    case MODULE_CONTROL_MODE_LOW_CUT:
    {
      sprintf(LCDText,"Low cut ");
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_3_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_4_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_5_LEVEL:
    case MODULE_CONTROL_MODE_EQ_BAND_6_LEVEL:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL)/(MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL-MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL);
      sprintf(LCDText,"EQ%d lvl ", BandNr+1);
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
    case MODULE_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY)/(MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY-MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY);
      sprintf(LCDText,"EQ%d freq", BandNr+1);
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
    case MODULE_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH)/(MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH-MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH);
      sprintf(LCDText," EQ%d Q  ", BandNr+1);
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_2_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_3_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_4_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_5_TYPE:
    case MODULE_CONTROL_MODE_EQ_BAND_6_TYPE:
    {
      int BandNr = (ControlMode-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE)/(MODULE_CONTROL_MODE_EQ_BAND_2_TYPE-MODULE_CONTROL_MODE_EQ_BAND_1_TYPE);
      sprintf(LCDText,"EQ%d type", BandNr+1);
    }
    break;
    case MODULE_CONTROL_MODE_AGC_THRESHOLD:
    {
      sprintf(LCDText," AGC Th ");
    }
    break;
    case MODULE_CONTROL_MODE_AGC:
    {
      sprintf(LCDText,"  AGC   ");
    }
    break;
    case MODULE_CONTROL_MODE_EXPANDER_THRESHOLD:
    {
      sprintf(LCDText,"D-Exp Th");
    }
    break;
    case MODULE_CONTROL_MODE_MONO:
    {
      sprintf(LCDText,"  Mono  ");
    }
    break;
    case MODULE_CONTROL_MODE_PAN:
    {
      sprintf(LCDText,"  Pan   ");
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_1_2:
    case MODULE_CONTROL_MODE_BUSS_3_4:
    case MODULE_CONTROL_MODE_BUSS_5_6:
    case MODULE_CONTROL_MODE_BUSS_7_8:
    case MODULE_CONTROL_MODE_BUSS_9_10:
    case MODULE_CONTROL_MODE_BUSS_11_12:
    case MODULE_CONTROL_MODE_BUSS_13_14:
    case MODULE_CONTROL_MODE_BUSS_15_16:
    case MODULE_CONTROL_MODE_BUSS_17_18:
    case MODULE_CONTROL_MODE_BUSS_19_20:
    case MODULE_CONTROL_MODE_BUSS_21_22:
    case MODULE_CONTROL_MODE_BUSS_23_24:
    case MODULE_CONTROL_MODE_BUSS_25_26:
    case MODULE_CONTROL_MODE_BUSS_27_28:
    case MODULE_CONTROL_MODE_BUSS_29_30:
    case MODULE_CONTROL_MODE_BUSS_31_32:
    {
      int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2)/(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2);
      strncpy(LCDText, AxumData.BussMasterData[BussNr].Label, 8);
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_1_2_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_3_4_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_5_6_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_7_8_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_9_10_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_11_12_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_13_14_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_15_16_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_17_18_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_19_20_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_21_22_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_23_24_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_25_26_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_27_28_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_29_30_BALANCE:
    case MODULE_CONTROL_MODE_BUSS_31_32_BALANCE:
    {
      int BussNr = (ControlMode-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE)/(MODULE_CONTROL_MODE_BUSS_3_4_BALANCE-MODULE_CONTROL_MODE_BUSS_1_2_BALANCE);
      strncpy(LCDText, AxumData.BussMasterData[BussNr].Label, 8);
    }
    break;
    case MODULE_CONTROL_MODE_MODULE_LEVEL:
    {
      sprintf(LCDText," Level  ");
    }
    break;
    case MODULE_CONTROL_MODE_EQ_ON_OFF:
    {
      sprintf(LCDText,"   EQ   ");
    }
    break;
    case MODULE_CONTROL_MODE_PHANTOM_ON_OFF:
    {
      sprintf(LCDText,"Phantom ");
    }
    break;
    case MODULE_CONTROL_MODE_PAD_ON_OFF:
    {
      sprintf(LCDText,"  Pad   ");
    }
    break;
    case MODULE_CONTROL_MODE_INSERT_ON_OFF:
    {
      sprintf(LCDText," Insert ");
    }
    break;
    case MODULE_CONTROL_MODE_DYNAMICS_ON_OFF:
    {
      sprintf(LCDText,"  Dyn   ");
    }
    break;
  }

  data.Octets = (unsigned char *)LCDText;
  mbnSetActuatorData(mbn, MambaNetAddress, ObjectNr, MBN_DATATYPE_OCTETS, 8, data, 1);

  DataType = 0;
  DataSize = 0;
  DataMinimal = 0;
  DataMaximal = 0;
}

void MasterModeControllerSensorChange(unsigned int ConsoleNr, unsigned char type, mbn_data data, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  int MasterControlMode = -1;

  MasterControlMode = AxumData.ConsoleData[ConsoleNr].MasterControlMode;

  if (type == MBN_DATATYPE_UINT)
  {
    int Position = (data.UInt*1023)/(DataMaximal-DataMinimal);
    switch (MasterControlMode)
    {
      case MASTER_CONTROL_MODE_BUSS_1_2:
      case MASTER_CONTROL_MODE_BUSS_3_4:
      case MASTER_CONTROL_MODE_BUSS_5_6:
      case MASTER_CONTROL_MODE_BUSS_7_8:
      case MASTER_CONTROL_MODE_BUSS_9_10:
      case MASTER_CONTROL_MODE_BUSS_11_12:
      case MASTER_CONTROL_MODE_BUSS_13_14:
      case MASTER_CONTROL_MODE_BUSS_15_16:
      case MASTER_CONTROL_MODE_BUSS_17_18:
      case MASTER_CONTROL_MODE_BUSS_19_20:
      case MASTER_CONTROL_MODE_BUSS_21_22:
      case MASTER_CONTROL_MODE_BUSS_23_24:
      case MASTER_CONTROL_MODE_BUSS_25_26:
      case MASTER_CONTROL_MODE_BUSS_27_28:
      case MASTER_CONTROL_MODE_BUSS_29_30:
      case MASTER_CONTROL_MODE_BUSS_31_32:
      { //master level
        int BussNr = MasterControlMode-MASTER_CONTROL_MODE_BUSS_1_2;
        float dB = Position2dB[Position];
        //dB -= AxumData.LevelReserve;
        dB -= 10;

        if (AxumData.BussMasterData[BussNr].Level != dB)
        {
          AxumData.BussMasterData[BussNr].Level = dB;

          SetAxum_BussMasterLevels();
          DoAxum_UpdateMasterControlMode(MasterControlMode);

          unsigned int FunctionNrToSent = 0x01000000 | (BussNr<<12);
          CheckObjectsToSent(FunctionNrToSent | BUSS_FUNCTION_MASTER_LEVEL);
        }
      }
      break;
    }
  }
  else if (type == MBN_DATATYPE_SINT)
  {
    switch (MasterControlMode)
    {
      case MASTER_CONTROL_MODE_BUSS_1_2:
      case MASTER_CONTROL_MODE_BUSS_3_4:
      case MASTER_CONTROL_MODE_BUSS_5_6:
      case MASTER_CONTROL_MODE_BUSS_7_8:
      case MASTER_CONTROL_MODE_BUSS_9_10:
      case MASTER_CONTROL_MODE_BUSS_11_12:
      case MASTER_CONTROL_MODE_BUSS_13_14:
      case MASTER_CONTROL_MODE_BUSS_15_16:
      case MASTER_CONTROL_MODE_BUSS_17_18:
      case MASTER_CONTROL_MODE_BUSS_19_20:
      case MASTER_CONTROL_MODE_BUSS_21_22:
      case MASTER_CONTROL_MODE_BUSS_23_24:
      case MASTER_CONTROL_MODE_BUSS_25_26:
      case MASTER_CONTROL_MODE_BUSS_27_28:
      case MASTER_CONTROL_MODE_BUSS_29_30:
      case MASTER_CONTROL_MODE_BUSS_31_32:
      { //master level
        int BussNr = MasterControlMode-MASTER_CONTROL_MODE_BUSS_1_2;
        float OldLevel = AxumData.BussMasterData[BussNr].Level;

        AxumData.BussMasterData[BussNr].Level += data.SInt;
        if (AxumData.BussMasterData[BussNr].Level<-140)
        {
          AxumData.BussMasterData[BussNr].Level = -140;
        }
        else
        {
          if (AxumData.BussMasterData[BussNr].Level>0)
          {
            AxumData.BussMasterData[BussNr].Level = 0;
          }
        }

        if (AxumData.BussMasterData[BussNr].Level != OldLevel)
        {
          SetAxum_BussMasterLevels();
          DoAxum_UpdateMasterControlMode(MasterControlMode);

          unsigned int FunctionNrToSent = 0x01000000 | (BussNr<<12);
          CheckObjectsToSent(FunctionNrToSent | BUSS_FUNCTION_MASTER_LEVEL);
        }
      }
      break;
    }
  }
  type = 0;
  data.State = 0;
  DataType = 0;
  DataSize = 0;
  DataMinimal = 0;
  DataMaximal = 0;
}

void MasterModeControllerResetSensorChange(unsigned int ConsoleNr, unsigned char type, mbn_data data, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  int MasterControlMode = -1;

  MasterControlMode = AxumData.ConsoleData[ConsoleNr].MasterControlMode;

  if (type == MBN_DATATYPE_STATE)
  {
    if (data.State)
    {
      switch (MasterControlMode)
      {
        case MASTER_CONTROL_MODE_BUSS_1_2:
        case MASTER_CONTROL_MODE_BUSS_3_4:
        case MASTER_CONTROL_MODE_BUSS_5_6:
        case MASTER_CONTROL_MODE_BUSS_7_8:
        case MASTER_CONTROL_MODE_BUSS_9_10:
        case MASTER_CONTROL_MODE_BUSS_11_12:
        case MASTER_CONTROL_MODE_BUSS_13_14:
        case MASTER_CONTROL_MODE_BUSS_15_16:
        case MASTER_CONTROL_MODE_BUSS_17_18:
        case MASTER_CONTROL_MODE_BUSS_19_20:
        case MASTER_CONTROL_MODE_BUSS_21_22:
        case MASTER_CONTROL_MODE_BUSS_23_24:
        case MASTER_CONTROL_MODE_BUSS_25_26:
        case MASTER_CONTROL_MODE_BUSS_27_28:
        case MASTER_CONTROL_MODE_BUSS_29_30:
        case MASTER_CONTROL_MODE_BUSS_31_32:
        { //master level
          int BussNr = MasterControlMode - MASTER_CONTROL_MODE_BUSS_1_2;

          if (AxumData.BussMasterData[BussNr].Level != 0)
          {
            AxumData.BussMasterData[BussNr].Level = 0;

            SetAxum_BussMasterLevels();
            DoAxum_UpdateMasterControlMode(MasterControlMode);

            unsigned int FunctionNrToSent = 0x01000000 | (BussNr<<12);
            CheckObjectsToSent(FunctionNrToSent | BUSS_FUNCTION_MASTER_LEVEL);
          }
        }
        break;
      }
    }
  }
  type = 0;
  data.State = 0;
  DataType = 0;
  DataSize = 0;
  DataMinimal = 0;
  DataMaximal = 0;
}


void MasterModeControllerSetData(unsigned int ConsoleNr, unsigned int MambaNetAddress, unsigned int ObjectNr, unsigned char DataType, unsigned char DataSize, float DataMinimal, float DataMaximal)
{
  int MasterControlMode = -1;
  float MasterLevel = -140;
  unsigned char Mask = 0x00;
  mbn_data data;

  MasterControlMode = AxumData.ConsoleData[ConsoleNr].MasterControlMode;

  switch (MasterControlMode)
  {
    case MASTER_CONTROL_MODE_BUSS_1_2:
    case MASTER_CONTROL_MODE_BUSS_3_4:
    case MASTER_CONTROL_MODE_BUSS_5_6:
    case MASTER_CONTROL_MODE_BUSS_7_8:
    case MASTER_CONTROL_MODE_BUSS_9_10:
    case MASTER_CONTROL_MODE_BUSS_11_12:
    case MASTER_CONTROL_MODE_BUSS_13_14:
    case MASTER_CONTROL_MODE_BUSS_15_16:
    case MASTER_CONTROL_MODE_BUSS_17_18:
    case MASTER_CONTROL_MODE_BUSS_19_20:
    case MASTER_CONTROL_MODE_BUSS_21_22:
    case MASTER_CONTROL_MODE_BUSS_23_24:
    case MASTER_CONTROL_MODE_BUSS_25_26:
    case MASTER_CONTROL_MODE_BUSS_27_28:
    case MASTER_CONTROL_MODE_BUSS_29_30:
    case MASTER_CONTROL_MODE_BUSS_31_32:
    { //busses
      int BussNr = MasterControlMode-MASTER_CONTROL_MODE_BUSS_1_2;
      MasterLevel = AxumData.BussMasterData[BussNr].Level;
    }
    break;
  }

  switch (DataType)
  {
    case MBN_DATATYPE_UINT:
    {
      int dB = (MasterLevel*10)+1400;
      if (dB<0)
      {
        dB = 0;
      }
      else if (dB>=1500)
      {
        dB = 1499;
      }
      int Position = ((dB2Position[dB]*(DataMaximal-DataMinimal))/1023)+DataMinimal;

      data.UInt = Position;
      mbnSetActuatorData(mbn, MambaNetAddress, ObjectNr, MBN_DATATYPE_UINT, 2, data, 1);
    }
    break;
    case MBN_DATATYPE_BITS:
    {
      int dB = ((MasterLevel+AxumData.LevelReserve)*10)+1400;
      if (dB<0)
      {
        dB = 0;
      }
      else if (dB>=1500)
      {
        dB = 1499;
      }
      int NrOfLEDs = ((dB2Position[dB]*DataMaximal)/1023)+1;
      for (char cntBit=0; cntBit<NrOfLEDs; cntBit++)
      {
        Mask |= 0x01<<cntBit;
      }
      if (MasterLevel<-80)
      {
        Mask = 0x00;
      }

      data.Bits[0] = Mask;
      mbnSetActuatorData(mbn, MambaNetAddress, ObjectNr, MBN_DATATYPE_BITS, 1, data, 1);
    }
    break;
  }
  DataSize = 0;
}

void DoAxum_BussReset(int BussNr)
{
  //first set all busses off
  for (int cntModule=0; cntModule<128; cntModule++)
  {
    unsigned char NewState = 0;
    DoAxum_SetBussOnOff(cntModule, BussNr, NewState, 0);
  }
}

int SourceActive(int InputSourceNr)
{
  unsigned int cntModule;
  unsigned int cntBuss;
  unsigned char Active = 0;

  for (cntModule=0; cntModule<128; cntModule++)
  {
    if (AxumData.ModuleData[cntModule].SelectedSource == (InputSourceNr+matrix_sources.src_offset.min.source))
    {
      unsigned char ModuleLevelActive = 0;
      unsigned char ModuleOnActive = 0;

      if (AxumData.ModuleData[cntModule].FaderLevel>-80)
      {   //fader open
        ModuleLevelActive = 1;
      }
      if (AxumData.ModuleData[cntModule].On)
      { //module on
        ModuleOnActive = 1;
      }
      for (cntBuss=0; cntBuss<16; cntBuss++)
      {
        unsigned char BussPreModuleLevelActive = 0;
        unsigned char BussPreModuleOnActive = 0;
        unsigned char BussLevelActive = 0;
        unsigned char BussOnActive = 0;
        unsigned char LevelActive = 0;
        unsigned char OnActive = 0;

        if (AxumData.ModuleData[cntModule].Buss[cntBuss].PreModuleLevel)
        {
          BussPreModuleLevelActive = 1;
        }
        if (AxumData.BussMasterData[cntBuss].PreModuleOn)
        {
          BussPreModuleOnActive = 1;
        }
        if (AxumData.ModuleData[cntModule].Buss[cntBuss].Level>-80)
        {
          BussLevelActive = 1;
        }
        if (AxumData.ModuleData[cntModule].Buss[cntBuss].On)
        {
          BussOnActive = 1;
        }

        if (BussLevelActive)
        {
          LevelActive |= ModuleLevelActive;
          if (BussPreModuleLevelActive)
          {
            LevelActive = 1;
          }
        }

        if (BussOnActive)
        {
          OnActive |= ModuleOnActive;
          if (BussPreModuleOnActive)
          {
            OnActive = 1;
          }
        }

        Active |= (OnActive && LevelActive);
      }
    }
  }

  return Active;
}

void DoAxum_ModuleStatusChanged(int ModuleNr, int ByModule)
{
  unsigned char Redlight[8] = {0,0,0,0,0,0,0,0};
  unsigned char MonitorMute[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

  if ((AxumData.ModuleData[ModuleNr].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource<=matrix_sources.src_offset.max.source))
  {
    int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
    unsigned int CurrentSourceActive = AxumData.SourceData[SourceNr].Active;
    unsigned int NewSourceActive = 0;

    NewSourceActive = SourceActive(SourceNr);

    if (CurrentSourceActive != NewSourceActive)
    {
      AxumData.SourceData[SourceNr].Active = NewSourceActive;

      //Check current state or redlights/mutes
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (AxumData.ModuleData[cntModule].SelectedSource != 0)
        {
          if ((AxumData.ModuleData[cntModule].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[cntModule].SelectedSource<=matrix_sources.src_offset.max.source))
          {
            int SourceToCheck = AxumData.ModuleData[cntModule].SelectedSource-matrix_sources.src_offset.min.source;
            if (AxumData.SourceData[SourceToCheck].Active)
            {
              for (int cntRedlight=0; cntRedlight<8; cntRedlight++)
              {
                if (AxumData.SourceData[SourceToCheck].Redlight[cntRedlight])
                {
                  Redlight[cntRedlight] = 1;
                }
              }
              for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
              {
                if (AxumData.SourceData[SourceToCheck].MonitorMute[cntMonitorBuss])
                {
                  MonitorMute[cntMonitorBuss] = 1;
                }
              }
            }
          }
        }
      }

      //redlights
      for (int cntRedlight=0; cntRedlight<8; cntRedlight++)
      {
        if (Redlight[cntRedlight] != AxumData.Redlight[cntRedlight])
        {
          AxumData.Redlight[cntRedlight] = Redlight[cntRedlight];

          unsigned int FunctionNrToSent = 0x04000000 | (GLOBAL_FUNCTION_REDLIGHT_1+(cntRedlight*(GLOBAL_FUNCTION_REDLIGHT_2-GLOBAL_FUNCTION_REDLIGHT_1)));
          CheckObjectsToSent(FunctionNrToSent);
        }
      }

      //mutes
      for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
      {
        if (MonitorMute[cntMonitorBuss] != AxumData.Monitor[cntMonitorBuss].Mute)
        {
          AxumData.Monitor[cntMonitorBuss].Mute = MonitorMute[cntMonitorBuss];
          unsigned int FunctionNrToSent = 0x02000000 | (cntMonitorBuss<<12);

          CheckObjectsToSent(FunctionNrToSent | MONITOR_BUSS_FUNCTION_MUTE);
          for (int cntDestination=0; cntDestination<1280; cntDestination++)
          {
            if (AxumData.DestinationData[cntDestination].Source == (17+cntMonitorBuss))
            {
              FunctionNrToSent = 0x06000000 | (cntDestination<<12);
              CheckObjectsToSent(FunctionNrToSent | DESTINATION_FUNCTION_MUTE_AND_MONITOR_MUTE);
            }
          }
        }
      }
    }
  }

  char ModuleActive = 0;
  if (AxumData.ModuleData[ModuleNr].On == 1)
  { //module turned on
    if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
    {   //fader open
      ModuleActive = 1;
    }
  }

  if (ByModule)
  {
    if (ModuleActive)
    {
      //Module active, check global buss reset
      for (int cntBuss=0; cntBuss<16; cntBuss++)
      {
        if (AxumData.ModuleData[ModuleNr].Buss[cntBuss].Assigned)
        {
          if (AxumData.BussMasterData[cntBuss].GlobalBussReset)
          {
            DoAxum_BussReset(cntBuss);
          }
        }
      }
    }
  }

  if (!ModuleActive)
  {
    if (AxumData.ModuleData[ModuleNr].WaitingSource != 0)
    {
      int SourceNr = AxumData.ModuleData[ModuleNr].WaitingSource;
      DoAxum_SetNewSource(ModuleNr, SourceNr, 0);
      AxumData.ModuleData[ModuleNr].WaitingSource = 0;
    }
    if (AxumData.ModuleData[ModuleNr].WaitingProcessingPreset != 0)
    {
      int PresetNr = AxumData.ModuleData[ModuleNr].WaitingProcessingPreset;

      DoAxum_LoadProcessingPreset(ModuleNr, PresetNr, 0, 0, 0);
      AxumData.ModuleData[ModuleNr].WaitingProcessingPreset = 0;
    }
    if (AxumData.ModuleData[ModuleNr].WaitingRoutingPreset != 0)
    {
      int PresetNr = AxumData.ModuleData[ModuleNr].WaitingRoutingPreset;
      DoAxum_LoadRoutingPreset(ModuleNr, PresetNr, 0, 0, 0);
      AxumData.ModuleData[ModuleNr].WaitingRoutingPreset = 0;
    }
  }
}

int MixMinusSourceUsed(int CurrentSource)
{
  int ModuleNr = -1;
  int cntModule = 0;
  while ((cntModule<128) && (ModuleNr == -1))
  {
    if (cntModule != ModuleNr)
    {
      if (AxumData.ModuleData[cntModule].SelectedSource == (CurrentSource+matrix_sources.src_offset.min.source))
      {
        int cntDestination = 0;
        while ((cntDestination<1280) && (ModuleNr == -1))
        {
          if (AxumData.DestinationData[cntDestination].MixMinusSource == (CurrentSource+matrix_sources.src_offset.min.source))
          {
            ModuleNr = cntModule;
          }
          cntDestination++;
        }
      }
    }
    cntModule++;
  }
  return ModuleNr;
}

void GetSourceLabel(int SourceNr, char *TextString, int MaxLength)
{
  if (SourceNr == -1)
  {
    strncpy(TextString, "Mute", MaxLength);
  }
  if (SourceNr == 0)
  {
    strncpy(TextString, "None", MaxLength);
  }
  else if ((SourceNr >= matrix_sources.src_offset.min.buss) && (SourceNr <= matrix_sources.src_offset.max.buss))
  {
    int BussNr = SourceNr-matrix_sources.src_offset.min.buss;
    strncpy(TextString, AxumData.BussMasterData[BussNr].Label, MaxLength);
  }
  else if ((SourceNr >= matrix_sources.src_offset.min.insert_out) && (SourceNr <= matrix_sources.src_offset.max.insert_out))
  {
    int ModuleNr = SourceNr-matrix_sources.src_offset.min.insert_out;
    char InsertText[32];

    //eventual depending on MaxLength
    sprintf(InsertText, "Ins. %3d", ModuleNr+1);

    strncpy(TextString, InsertText, MaxLength);
  }
  else if ((SourceNr >= matrix_sources.src_offset.min.monitor_buss) && (SourceNr <= matrix_sources.src_offset.max.monitor_buss))
  {
    int BussNr = SourceNr-matrix_sources.src_offset.min.monitor_buss;
    strncpy(TextString, AxumData.Monitor[BussNr].Label, MaxLength);
  }
  else if ((SourceNr >= matrix_sources.src_offset.min.mixminus) && (SourceNr <= matrix_sources.src_offset.max.mixminus))
  {
    int ModuleNr = SourceNr-matrix_sources.src_offset.min.mixminus;
    char MixMinusText[32];

    //eventual depending on MaxLength
    sprintf(MixMinusText, "N-1 %3d", ModuleNr+1);

    strncpy(TextString, MixMinusText, MaxLength);
  }
  else if ((SourceNr >= matrix_sources.src_offset.min.source) && (SourceNr <= matrix_sources.src_offset.max.source))
  {
    int LabelSourceNr = SourceNr-matrix_sources.src_offset.min.source;
    strncpy(TextString, AxumData.SourceData[LabelSourceNr].SourceName, MaxLength);
  }
}

int AdjustModuleSource(int CurrentSource, int Offset, unsigned char Pool)
{
  char check_for_next_pos;
  int cntPos;
  int StartPos;
  int CurrentPos;
  int PosBefore;
  int PosAfter;

  //Determin the current position
  CurrentPos = -1;
  PosBefore = -1;
  PosAfter = MAX_POS_LIST_SIZE;
  for (cntPos=0; cntPos<MAX_POS_LIST_SIZE; cntPos++)
  {
    if (matrix_sources.pos[cntPos].src != INT_MIN)
    {
      if (matrix_sources.pos[cntPos].src == CurrentSource)
      {
        CurrentPos = cntPos;
      }
      else if (matrix_sources.pos[cntPos].src < CurrentSource)
      {
        if (cntPos>PosBefore)
        {
          PosBefore = cntPos;
        }
      }
      else if (matrix_sources.pos[cntPos].src > CurrentSource)
      {
        if (cntPos<PosAfter)
        {
          PosAfter = cntPos;
        }
      }
    }
  }

  //If current position not found...
  if (CurrentPos == -1)
  {
    if (Offset<0)
    {
      CurrentPos = PosBefore;
    }
    else
    {
      CurrentPos = PosAfter;
    }
  }

  if (CurrentPos != -1)
  {
    while (Offset != 0)
    {
      StartPos = CurrentPos;
      if (Offset>0)
      {
        check_for_next_pos = 1;
        while (check_for_next_pos)
        {
          check_for_next_pos = 0;

          CurrentPos++;
          if (CurrentPos>=MAX_POS_LIST_SIZE)
          {
            CurrentPos = 0;
          }

          CurrentSource = matrix_sources.pos[CurrentPos].src;

          //check if hybrid is used
          if (CurrentSource > 0)
          {
            if (MixMinusSourceUsed(CurrentSource) != -1)
            {
              check_for_next_pos = 1;
            }
          }

          //not active, go further.
          if (!matrix_sources.pos[CurrentPos].active)
          {
            check_for_next_pos = 1;
          }

          if (Pool<8)
          {
            if (!matrix_sources.pos[CurrentPos].pool[Pool])
            {
              check_for_next_pos = 1;
            }
          }

          if (CurrentPos == StartPos)
          { //Looped through all sources, no step found...
            check_for_next_pos = 0;
          }
        }
        Offset--;
      }
      else
      {
        check_for_next_pos = 1;
        while (check_for_next_pos)
        {
          check_for_next_pos = 0;

          CurrentPos--;
          if (CurrentPos<0)
          {
            CurrentPos = MAX_POS_LIST_SIZE-1;
          }

          CurrentSource = matrix_sources.pos[CurrentPos].src;

          //check if hybrid is used
          if (CurrentSource > 0)
          {
            if (MixMinusSourceUsed(CurrentSource) != -1)
            {
              check_for_next_pos = 1;
            }
          }

          //not active, go further.
          if (!matrix_sources.pos[CurrentPos].active)
          {
            check_for_next_pos = 1;
          }

          if (Pool<8)
          {
            if (!matrix_sources.pos[CurrentPos].pool[Pool])
            {
              check_for_next_pos = 1;
            }
          }

          if (CurrentPos == StartPos)
          { //Looped through all sources, no step found...
            check_for_next_pos = 0;
          }
        }
        Offset++;
      }
    }
  }

  return CurrentSource;
}

bool DoAxum_SetNewSource(int ModuleNr, int NewSource, int Forced)
{
  int OldSource = AxumData.ModuleData[ModuleNr].SelectedSource;
  int OldSourceActive = 0;
  unsigned char CurrentPresetState[8];
  unsigned char cntPreset;

  if (NewSource != 0)
  {
    for (cntPreset=0; cntPreset<8; cntPreset++)
    {
      CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
    }

    if (AxumData.ModuleData[ModuleNr].On)
    {
      if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
      {
        OldSourceActive = 1;
      }
    }

    if (OldSource != NewSource)
    {
      if ((!OldSourceActive) || (Forced))
      {
        AxumData.ModuleData[ModuleNr].SelectedSource = NewSource;
        AxumData.ModuleData[ModuleNr].Cough = 0;

        SetAxum_ModuleSource(ModuleNr);
        SetAxum_ModuleMixMinus(ModuleNr, OldSource);

        unsigned int FunctionNrToSent = (ModuleNr<<12);
        if ((AxumData.ConsoleData[0].ControlMode == MODULE_CONTROL_MODE_SOURCE) || (AxumData.ConsoleData[0].ControlMode == MODULE_CONTROL_MODE_NONE))
        {
          if (AxumData.ConsoleData[0].ControlMode == MODULE_CONTROL_MODE_SOURCE)
          {
            AxumData.ModuleData[ModuleNr].TemporySourceControlMode[0] = AxumData.ModuleData[ModuleNr].SelectedSource;
          }
        }
        if ((AxumData.ConsoleData[1].ControlMode  == MODULE_CONTROL_MODE_SOURCE) || (AxumData.ConsoleData[1].ControlMode == MODULE_CONTROL_MODE_NONE))
        {
          if (AxumData.ConsoleData[1].ControlMode == MODULE_CONTROL_MODE_SOURCE)
          {
            AxumData.ModuleData[ModuleNr].TemporySourceControlMode[1] = AxumData.ModuleData[ModuleNr].SelectedSource;
          }
        }
        if ((AxumData.ConsoleData[2].ControlMode == MODULE_CONTROL_MODE_SOURCE) || (AxumData.ConsoleData[2].ControlMode == MODULE_CONTROL_MODE_NONE))
        {
          if (AxumData.ConsoleData[2].ControlMode == MODULE_CONTROL_MODE_SOURCE)
          {
            AxumData.ModuleData[ModuleNr].TemporySourceControlMode[2] = AxumData.ModuleData[ModuleNr].SelectedSource;
          }
        }
        if ((AxumData.ConsoleData[3].ControlMode == MODULE_CONTROL_MODE_SOURCE) || (AxumData.ConsoleData[3].ControlMode == MODULE_CONTROL_MODE_NONE))
        {
          if (AxumData.ConsoleData[3].ControlMode == MODULE_CONTROL_MODE_SOURCE)
          {
            AxumData.ModuleData[ModuleNr].TemporySourceControlMode[3] = AxumData.ModuleData[ModuleNr].SelectedSource;
          }
        }
        DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_SOURCE);
        DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_NONE);

        AxumData.ModuleData[ModuleNr].TemporySourceLocal = AxumData.ModuleData[ModuleNr].SelectedSource;
        CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE);

        if ((OldSource>=matrix_sources.src_offset.min.source) && (OldSource<=matrix_sources.src_offset.max.source))
        {
          unsigned int SourceNr = OldSource-matrix_sources.src_offset.min.source;
          FunctionNrToSent = 0x05000000 | (SourceNr<<12);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_1_2_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_3_4_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_5_6_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_5_6_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_5_6_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_7_8_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_7_8_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_7_8_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_9_10_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_9_10_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_9_10_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_11_12_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_11_12_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_11_12_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_13_14_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_13_14_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_13_14_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_15_16_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_15_16_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_15_16_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_17_18_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_17_18_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_17_18_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_19_20_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_19_20_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_19_20_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_21_22_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_21_22_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_21_22_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_23_24_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_23_24_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_23_24_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_25_26_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_25_26_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_25_26_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_27_28_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_27_28_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_27_28_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_29_30_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_29_30_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_29_30_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_31_32_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_31_32_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_31_32_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_COUGH_ON_OFF);
        }

        if ((NewSource>=matrix_sources.src_offset.min.source) && (NewSource<=matrix_sources.src_offset.max.source))
        {
          unsigned int SourceNr = NewSource-matrix_sources.src_offset.min.source;
          FunctionNrToSent = 0x05000000 | (SourceNr<<12);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_1_2_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_3_4_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_5_6_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_5_6_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_5_6_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_7_8_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_7_8_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_7_8_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_9_10_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_9_10_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_9_10_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_11_12_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_11_12_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_11_12_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_13_14_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_13_14_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_13_14_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_15_16_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_15_16_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_15_16_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_17_18_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_17_18_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_17_18_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_19_20_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_19_20_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_19_20_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_21_22_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_21_22_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_21_22_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_23_24_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_23_24_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_23_24_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_25_26_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_25_26_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_25_26_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_27_28_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_27_28_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_27_28_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_29_30_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_29_30_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_29_30_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_31_32_ON);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_31_32_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_BUSS_31_32_ON_OFF);
          CheckObjectsToSent(FunctionNrToSent+SOURCE_FUNCTION_MODULE_COUGH_ON_OFF);
        }

        for (int cntModule=0; cntModule<128; cntModule++)
        {
          if (AxumData.ModuleData[cntModule].SelectedSource == AxumData.ModuleData[ModuleNr].SelectedSource)
          {
            FunctionNrToSent = (cntModule<<12);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_PHANTOM);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_PAD);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_GAIN_LEVEL);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_GAIN_LEVEL_RESET);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_START);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_STOP);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_START_STOP);
            CheckObjectsToSent(FunctionNrToSent+MODULE_FUNCTION_SOURCE_ALERT);
          }
        }
      }
    }

    for (cntPreset=0; cntPreset<8; cntPreset++)
    {
      if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
      {
        unsigned int FunctionNrToSent = (ModuleNr<<12);
        CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

        //if A or B
        unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
        if (cntPreset == PresetNr)
        {
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
        }
        else if (cntPreset == (PresetNr+1))
        {
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
        }
      }
    }
    DoAxum_UpdateModuleControlMode(ModuleNr, MODULE_CONTROL_MODE_SOURCE);
    DoAxum_UpdateModuleControlModeLabel(ModuleNr, MODULE_CONTROL_MODE_SOURCE);
  }
  return ((!OldSourceActive) || (Forced));
}

void DoAxum_SetBussOnOff(int ModuleNr, int BussNr, unsigned char NewState, int LoadPreset)
{
  unsigned char ModuleActive = 0;
  unsigned char CurrentPresetState[8];
  unsigned char cntPreset;
  unsigned char BussActive[16];
  unsigned char NewBussActive[16];
  unsigned char BussToMonitorBussActive[16][16];
  unsigned char NewBussToMonitorBussActive[16][16];
  unsigned char ExtToMonitorBussActive[16][8];
  unsigned char NewExtToMonitorBussActive[16][8];
  unsigned char CommExclusiveBuss = 0;
  unsigned char ExclusiveBuss = 0;
  unsigned char ActiveDumpRecBuss[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  unsigned char ActiveCommBuss[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  unsigned char DumpActive = 0;
  unsigned char CommActive = 0;
  unsigned char MixMinusInUse = 0;
  unsigned char PreviousExclusiveState = 0;

  int SelectedSource = AxumData.ModuleData[ModuleNr].SelectedSource;
  int cntDestination;

  if (AxumData.ModuleData[ModuleNr].On)
  {
    if (AxumData.ModuleData[ModuleNr].FaderLevel>-80)
    {
      ModuleActive = 1;
    }
  }

  cntDestination = 0;
  while (cntDestination<1280)
  {
    if ((SelectedSource != 0) && (AxumData.DestinationData[cntDestination].MixMinusSource == SelectedSource))
    {
      MixMinusInUse = 1;
    }
    cntDestination++;
  }

  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    if (AxumData.ModuleData[ModuleNr].Buss[cntBuss].On)
    {
      if (AxumData.BussMasterData[cntBuss].Exclusive == 1)
      {
        ActiveDumpRecBuss[cntBuss] = 1;
        DumpActive = 1;
      }
      else if (AxumData.BussMasterData[cntBuss].Exclusive)
      {
        if ((SelectedSource >= matrix_sources.src_offset.min.source) && (SelectedSource <= matrix_sources.src_offset.max.source))
        {
          unsigned int SourceNr = SelectedSource-matrix_sources.src_offset.min.source;
          if (AxumData.SourceData[SourceNr].CoughComm[0] || AxumData.SourceData[SourceNr].CoughComm[1])
          {
            ActiveCommBuss[cntBuss] = 1;
            CommActive = 1;
          }
        }
      }
    }
  }

  if (AxumData.BussMasterData[BussNr].Exclusive == 1)
  {
    ExclusiveBuss = 1;
  }
  else if ((AxumData.BussMasterData[BussNr].Exclusive == 2) || (AxumData.BussMasterData[BussNr].Exclusive == 3))
  {
    CommExclusiveBuss = 1;
  }
  if ((AxumData.ModuleData[ModuleNr].Buss[BussNr].PreviousOn[BussNr]) && (!NewState))
  {
    PreviousExclusiveState = 1;
  }

  if (AxumData.ModuleData[ModuleNr].Buss[BussNr].On != NewState)
  {
    for (cntPreset=0; cntPreset<8; cntPreset++)
    {
      CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
    }
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      BussActive[cntBuss] = 0;
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (AxumData.ModuleData[cntModule].Buss[cntBuss].On)
        {
          BussActive[cntBuss] = 1;
        }
      }
    }
    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      for (int cntBuss=0; cntBuss<16; cntBuss++)
      {
        BussToMonitorBussActive[cntMonitorBuss][cntBuss] = 0;
        if (AxumData.Monitor[cntMonitorBuss].Buss[cntBuss])
        {
          BussToMonitorBussActive[cntMonitorBuss][cntBuss] = 1;
        }
      }
      for (int cntExt=0; cntExt<8; cntExt++)
      {
        ExtToMonitorBussActive[cntMonitorBuss][cntExt] = 0;
        if (AxumData.Monitor[cntMonitorBuss].Ext[cntExt])
        {
          ExtToMonitorBussActive[cntMonitorBuss][cntExt] = 1;
        }
      }
    }

    AxumData.ModuleData[ModuleNr].Buss[BussNr].On = NewState;


    if (((ExclusiveBuss) || (MixMinusInUse && CommExclusiveBuss && (!ModuleActive || PreviousExclusiveState))) && (!LoadPreset))
    {
      if (!ModuleActive)
      {
        if (!AxumData.ModuleData[ModuleNr].Buss[BussNr].On)
        {  //return to normal routing
          for (int cntBuss=0; cntBuss<16; cntBuss++)
          {
            if (cntBuss != BussNr)
            {
              if (AxumData.ModuleData[ModuleNr].Buss[cntBuss].On != AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[BussNr])
              {
                AxumData.ModuleData[ModuleNr].Buss[cntBuss].On = AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[BussNr];

                unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
                CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON))));
                CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_OFF+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF))));
                CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON_OFF+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF))));

                if ((SelectedSource >= matrix_sources.src_offset.min.source) && (SelectedSource <= matrix_sources.src_offset.max.source))
                {
                  unsigned int SourceNr = SelectedSource-matrix_sources.src_offset.min.source;
                  FunctionNrToSent = 0x05000000 | (SourceNr<<12);
                  CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON))));
                  CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF))));
                  CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF))));
                }
              }
            }
            else
            {
              AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[BussNr] = 0;
            }
          }
        }
        else
        {  //turn off other routing
          for (int cntBuss=0; cntBuss<16; cntBuss++)
          {
            if (cntBuss != BussNr)
            {
              AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[BussNr] = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
              if (AxumData.ModuleData[ModuleNr].Buss[cntBuss].On != 0)
              {
                AxumData.ModuleData[ModuleNr].Buss[cntBuss].On = 0;

                unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
                CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON))));
                CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_OFF+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF))));
                CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON_OFF+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF))));

                if ((SelectedSource >= matrix_sources.src_offset.min.source) && (SelectedSource <= matrix_sources.src_offset.max.source))
                {
                  unsigned int SourceNr = SelectedSource-matrix_sources.src_offset.min.source;
                  FunctionNrToSent = 0x05000000 | (SourceNr<<12);
                  CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON))));
                  CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF))));
                  CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF))));
                }
              }
            }
            else
            {
              AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[BussNr] = 1;
            }
          }
        }
      }
      else
      {
        AxumData.ModuleData[ModuleNr].Buss[BussNr].On = !AxumData.ModuleData[ModuleNr].Buss[BussNr].On;
      }
    }

    //Check for communication state
    if ((SelectedSource >= matrix_sources.src_offset.min.source) && (SelectedSource <= matrix_sources.src_offset.max.source))
    {
      int SourceNr = SelectedSource-matrix_sources.src_offset.min.source;

      int cntDestination=0;
      while (cntDestination<1280)
      {
        if (cntDestination == AxumData.SourceData[SourceNr].RelatedDest)
        {
          unsigned char DestCommActive = 0;
          if (CommExclusiveBuss)
          {
            if (AxumData.ModuleData[ModuleNr].Buss[BussNr].On)
            {
              DestCommActive = 1;
            }
          }

          if ((DestCommActive) && (!ModuleActive))
          {
            AxumData.DestinationData[cntDestination].CommBuss = BussNr;
            AxumData.DestinationData[cntDestination].CommActive = 1;
          }
          else if (AxumData.DestinationData[cntDestination].CommActive)
          {
            if (AxumData.DestinationData[cntDestination].CommBuss == BussNr)
            {
              AxumData.DestinationData[cntDestination].CommActive = 0;
            }
          }
          SetAxum_DestinationSource(cntDestination);
          log_write("Comm buss:%d, dest:%d => active? %d (by module:%d)", BussNr, cntDestination, DestCommActive, ModuleNr);
        }
        cntDestination++;
      }
    }
    SetAxum_BussLevels(ModuleNr);
    SetAxum_ModuleMixMinus(ModuleNr, 0);

    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON+(BussNr*(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON))));
    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_OFF+(BussNr*(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF))));
    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON_OFF+(BussNr*(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF))));
    //To adjust text if level object = octetstring
    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_LEVEL+(BussNr*(MODULE_FUNCTION_BUSS_3_4_LEVEL-MODULE_FUNCTION_BUSS_1_2_LEVEL))));

    int ControlMode = MODULE_CONTROL_MODE_BUSS_1_2+(BussNr*(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2));
    DoAxum_UpdateModuleControlMode(ModuleNr, ControlMode);

    if ((SelectedSource>=matrix_sources.src_offset.min.source) && (SelectedSource<=matrix_sources.src_offset.max.source))
    {
      unsigned int SourceNr = SelectedSource-matrix_sources.src_offset.min.source;
      FunctionNrToSent = 0x05000000 | (SourceNr<<12);
      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON+(BussNr*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON))));
      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF+(BussNr*(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF))));
      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF+(BussNr*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF))));
    }

    //Do interlock
    if ((AxumData.BussMasterData[BussNr].Interlock) && (!LoadPreset))
    {
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (ModuleNr != cntModule)
        {
          if (AxumData.ModuleData[cntModule].Buss[BussNr].On)
          {
            unsigned char CurrentPresetState[8];
            unsigned char cntPreset;

            for (cntPreset=0; cntPreset<8; cntPreset++)
            {
              CurrentPresetState[cntPreset] = ModulePresetActive(cntModule, cntPreset+1);
            }
            AxumData.ModuleData[cntModule].Buss[BussNr].On = 0;

            if (AxumData.BussMasterData[BussNr].Exclusive == 1)
            {
              for (int cntBuss=0; cntBuss<16; cntBuss++)
              {
                if (cntBuss != BussNr)
                {
                  if (AxumData.ModuleData[cntModule].Buss[cntBuss].On != AxumData.ModuleData[cntModule].Buss[cntBuss].PreviousOn[BussNr])
                  {
                    AxumData.ModuleData[cntModule].Buss[cntBuss].On = AxumData.ModuleData[cntModule].Buss[cntBuss].PreviousOn[BussNr];

                    unsigned int FunctionNrToSent = ((cntModule<<12)&0xFFF000);
                    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON))));
                    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_OFF+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF))));
                    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON_OFF+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF))));

                    if ((AxumData.ModuleData[cntModule].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[cntModule].SelectedSource<=matrix_sources.src_offset.max.source))
                    {
                      unsigned int SourceNr = AxumData.ModuleData[cntModule].SelectedSource-matrix_sources.src_offset.min.source;
                      FunctionNrToSent = 0x05000000 | (SourceNr<<12);
                      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON))));
                      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF))));
                      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF+(cntBuss*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF))));
                    }
                  }
                }
              }
            }

            SetAxum_BussLevels(cntModule);
            SetAxum_ModuleMixMinus(cntModule, 0);

            unsigned int FunctionNrToSent = ((cntModule<<12)&0xFFF000);
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON+(BussNr*(MODULE_FUNCTION_BUSS_3_4_ON-MODULE_FUNCTION_BUSS_1_2_ON))));
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_OFF+(BussNr*(MODULE_FUNCTION_BUSS_3_4_OFF-MODULE_FUNCTION_BUSS_1_2_OFF))));
            CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_ON_OFF+(BussNr*(MODULE_FUNCTION_BUSS_3_4_ON_OFF-MODULE_FUNCTION_BUSS_1_2_ON_OFF))));

            int ControlMode = MODULE_CONTROL_MODE_BUSS_1_2+(BussNr*(MODULE_CONTROL_MODE_BUSS_3_4-MODULE_CONTROL_MODE_BUSS_1_2));
            DoAxum_UpdateModuleControlMode(cntModule, ControlMode);

            if ((AxumData.ModuleData[cntModule].SelectedSource>=matrix_sources.src_offset.min.source) && (AxumData.ModuleData[cntModule].SelectedSource<=matrix_sources.src_offset.max.source))
            {
              int SourceNr = AxumData.ModuleData[cntModule].SelectedSource-matrix_sources.src_offset.min.source;
              FunctionNrToSent = 0x05000000 | (SourceNr<<12);
              CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON+(BussNr*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON))));
              CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF+(BussNr*(SOURCE_FUNCTION_MODULE_BUSS_3_4_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_OFF))));
              CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF+(BussNr*(SOURCE_FUNCTION_MODULE_BUSS_3_4_ON_OFF-SOURCE_FUNCTION_MODULE_BUSS_1_2_ON_OFF))));
            }

            for (cntPreset=0; cntPreset<8; cntPreset++)
            {
              if (CurrentPresetState[cntPreset] != ModulePresetActive(cntModule, cntPreset+1))
              {
                unsigned int FunctionNrToSent = (cntModule<<12);
                CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

                //if A or B
                unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
                if (cntPreset == PresetNr)
                {
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                }
                else if (cntPreset == (PresetNr+1))
                {
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
                  CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
                }
              }
            }
          }
        }
      }
    }

    //Check active buss auto switching
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      NewBussActive[cntBuss] = 0;
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (AxumData.ModuleData[cntModule].Buss[cntBuss].On)
        {
          NewBussActive[cntBuss] = 1;
        }
      }
    }

    unsigned char AutoSwitchingExclusiveActive = 0;
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      if (NewBussActive[cntBuss] != BussActive[cntBuss])
      {
        //Check buss reset
        FunctionNrToSent = 0x01000000 | ((cntBuss<<12)&0xFFF000);
        CheckObjectsToSent(FunctionNrToSent | BUSS_FUNCTION_RESET);

        //Set buss default if not active
        for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
        {
          AutoSwitchingExclusiveActive = 0;
          if (AxumData.Monitor[cntMonitorBuss].AutoSwitchingBuss[cntBuss])
          {
            AxumData.Monitor[cntMonitorBuss].Buss[cntBuss] = NewBussActive[cntBuss];

            if ((NewBussActive[cntBuss]) && (AxumData.BussMasterData[cntBuss].Exclusive == 1))
            {
              AutoSwitchingExclusiveActive = 1;
            }
          }

          if (AutoSwitchingExclusiveActive)
          {
            for (int cntBuss2=0; cntBuss2<16; cntBuss2++)
            {
              if ((NewBussActive[cntBuss2]) && (AxumData.BussMasterData[cntBuss2].Exclusive == 1))
              {
                AxumData.Monitor[cntMonitorBuss].Buss[cntBuss2] = 1;
              }
              else
              {
                AxumData.Monitor[cntMonitorBuss].Buss[cntBuss2] = 0;
              }
            }
            for (int cntExt=0; cntExt<8; cntExt++)
            {
              AxumData.Monitor[cntMonitorBuss].Ext[cntExt] = 0;
            }
          }
        }
      }
    }

    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      unsigned int MonitorBussActive = 0;
      int cntBuss;
      for (cntBuss=0; cntBuss<16; cntBuss++)
      {
        if (AxumData.Monitor[cntMonitorBuss].Buss[cntBuss])
        {
          MonitorBussActive = 1;
        }
      }
      int cntExt;
      for (cntExt=0; cntExt<8; cntExt++)
      {
        if (AxumData.Monitor[cntMonitorBuss].Ext[cntExt])
        {
          MonitorBussActive = 1;
        }
      }

      if (!MonitorBussActive)
      {
        int DefaultSelection = AxumData.Monitor[cntMonitorBuss].DefaultSelection;
        if (DefaultSelection<16)
        {
          AxumData.Monitor[cntMonitorBuss].Buss[DefaultSelection] = 1;
        }
        else if (DefaultSelection<24)
        {
          int ExtNr = DefaultSelection-16;
          AxumData.Monitor[cntMonitorBuss].Ext[ExtNr] = 1;
        }
      }
    }

    //Not sure what to do below... possibly check all channels?
    DoAxum_ModuleStatusChanged(ModuleNr, 0);

    bool BussPre = 0;
    for (int cntModule=0; cntModule<128; cntModule++)
    {
      if ((AxumData.ModuleData[cntModule].Buss[BussNr].Assigned) && (AxumData.ModuleData[cntModule].Console == AxumData.BussMasterData[BussNr].Console))
      {
        if (AxumData.ModuleData[cntModule].Buss[BussNr].PreModuleLevel)
        {
          BussPre = 1;
        }
      }
    }

    //make functional because the eventual monitor mute is already set
    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      for (int cntBuss=0; cntBuss<16; cntBuss++)
      {
        NewBussToMonitorBussActive[cntMonitorBuss][cntBuss] = 0;
        if (AxumData.Monitor[cntMonitorBuss].Buss[cntBuss])
        {
          NewBussToMonitorBussActive[cntMonitorBuss][cntBuss] = 1;
        }
      }
      for (int cntExt=0; cntExt<8; cntExt++)
      {
        NewExtToMonitorBussActive[cntMonitorBuss][cntExt] = 0;
        if (AxumData.Monitor[cntMonitorBuss].Ext[cntExt])
        {
          NewExtToMonitorBussActive[cntMonitorBuss][cntExt] = 1;
        }
      }
    }
    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      for (int cntBuss=0; cntBuss<16; cntBuss++)
      {
        if (NewBussToMonitorBussActive[cntMonitorBuss][cntBuss] != BussToMonitorBussActive[cntMonitorBuss][cntBuss])
        {
          SetAxum_MonitorBuss(cntMonitorBuss);

          unsigned int FunctionNrToSent = 0x02000000 | (cntMonitorBuss<<12);
          CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_ON+cntBuss));
          CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF+cntBuss));
          CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF+cntBuss));
        }
      }
      for (int cntExt=0; cntExt<8; cntExt++)
      {
        if (NewExtToMonitorBussActive[cntMonitorBuss][cntExt] != ExtToMonitorBussActive[cntMonitorBuss][cntExt])
        {
          SetAxum_MonitorBuss(cntMonitorBuss);

          unsigned int FunctionNrToSent = 0x02000000 | (cntMonitorBuss<<12);
          CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_ON+cntExt));
          CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_OFF+cntExt));
          CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF+cntExt));
        }
      }
    }

    for (cntPreset=0; cntPreset<8; cntPreset++)
    {
      if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
      {
        unsigned int FunctionNrToSent = (ModuleNr<<12);
        CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

        //if A or B
        unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
        if (cntPreset == PresetNr)
        {
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
        }
        else if (cntPreset == (PresetNr+1))
        {
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
          CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
        }
      }
    }
  }
}

void initialize_axum_data_struct()
{
  for (int cntSource=0; cntSource<1280; cntSource++)
  {
    AxumData.SourceData[cntSource].SourceName[0] = 0x00;
    for (int cntInput=0; cntInput<8; cntInput++)
    {
      AxumData.SourceData[cntSource].InputData[cntInput].MambaNetAddress = 0x00000000;
      AxumData.SourceData[cntSource].InputData[cntInput].SubChannel = -1;
    }
    AxumData.SourceData[cntSource].DefaultProcessingPreset = 0;
    AxumData.SourceData[cntSource].StartTrigger = 0;
    AxumData.SourceData[cntSource].StopTrigger = 0;
    AxumData.SourceData[cntSource].RelatedDest = -1;

    for (int cntRedlight=0; cntRedlight<8; cntRedlight++)
    {
      AxumData.SourceData[cntSource].Redlight[cntRedlight] = 0;
    }
    for (int cntMonitorMute=0; cntMonitorMute<16; cntMonitorMute++)
    {
      AxumData.SourceData[cntSource].MonitorMute[cntMonitorMute] = 0;
    }
    AxumData.SourceData[cntSource].Active = 0;
    AxumData.SourceData[cntSource].Start = 0;
    AxumData.SourceData[cntSource].Phantom = 0;
    AxumData.SourceData[cntSource].Pad = 0;
    AxumData.SourceData[cntSource].Gain = 30;
    AxumData.SourceData[cntSource].DefaultGain = 30;
    AxumData.SourceData[cntSource].Alert = 0;

    AxumData.SourceData[cntSource].CoughComm[0] = 0;
    AxumData.SourceData[cntSource].CoughComm[1] = 0;
  }

  for (int cntPreset=0; cntPreset<1280; cntPreset++)
  {
    AxumData.PresetData[cntPreset].PresetName[0] = 0x00;
    AxumData.PresetData[cntPreset].Type = 0;
    //AxumData.PresetData[cntPreset].Pos = 0;

    AxumData.PresetData[cntPreset].UseGain = false;
    AxumData.PresetData[cntPreset].Gain = 0;

    AxumData.PresetData[cntPreset].UseFilter = false;
    AxumData.PresetData[cntPreset].Filter.Level = 0;
    AxumData.PresetData[cntPreset].Filter.Frequency = 80;
    AxumData.PresetData[cntPreset].Filter.Bandwidth = 1;
    AxumData.PresetData[cntPreset].Filter.Slope = 1;
    AxumData.PresetData[cntPreset].Filter.Type = HPF;
    AxumData.PresetData[cntPreset].FilterOnOff = false;

    AxumData.PresetData[cntPreset].UseInsert = false;
    AxumData.PresetData[cntPreset].InsertOnOff = false;

    AxumData.PresetData[cntPreset].UsePhase = false;
    AxumData.PresetData[cntPreset].Phase = 0x03;
    AxumData.PresetData[cntPreset].PhaseOnOff = false;

    AxumData.PresetData[cntPreset].UseMono = false;
    AxumData.PresetData[cntPreset].Mono = 0;
    AxumData.PresetData[cntPreset].MonoOnOff = false;

    AxumData.PresetData[cntPreset].UseEQ = false;

    AxumData.PresetData[cntPreset].EQBand[0].Level = 0;
    AxumData.PresetData[cntPreset].EQBand[0].Frequency = 12000;
    AxumData.PresetData[cntPreset].EQBand[0].Bandwidth = 1;
    AxumData.PresetData[cntPreset].EQBand[0].Slope = 1;
    AxumData.PresetData[cntPreset].EQBand[0].Type = PEAKINGEQ;

    AxumData.PresetData[cntPreset].EQBand[1].Level = 0;
    AxumData.PresetData[cntPreset].EQBand[1].Frequency = 4000;
    AxumData.PresetData[cntPreset].EQBand[1].Bandwidth = 1;
    AxumData.PresetData[cntPreset].EQBand[1].Slope = 1;
    AxumData.PresetData[cntPreset].EQBand[1].Type = PEAKINGEQ;

    AxumData.PresetData[cntPreset].EQBand[2].Level = 0;
    AxumData.PresetData[cntPreset].EQBand[2].Frequency = 800;
    AxumData.PresetData[cntPreset].EQBand[2].Bandwidth = 1;
    AxumData.PresetData[cntPreset].EQBand[2].Slope = 1;
    AxumData.PresetData[cntPreset].EQBand[2].Type = PEAKINGEQ;

    AxumData.PresetData[cntPreset].EQBand[3].Level = 0;
    AxumData.PresetData[cntPreset].EQBand[3].Frequency = 120;
    AxumData.PresetData[cntPreset].EQBand[3].Bandwidth = 1;
    AxumData.PresetData[cntPreset].EQBand[3].Slope = 1;
    AxumData.PresetData[cntPreset].EQBand[3].Type = LOWSHELF;

    AxumData.PresetData[cntPreset].EQBand[4].Level = 0;
    AxumData.PresetData[cntPreset].EQBand[4].Frequency = 300;
    AxumData.PresetData[cntPreset].EQBand[4].Bandwidth = 1;
    AxumData.PresetData[cntPreset].EQBand[4].Slope = 1;
    AxumData.PresetData[cntPreset].EQBand[4].Type = HPF;

    AxumData.PresetData[cntPreset].EQBand[5].Level = 0;
    AxumData.PresetData[cntPreset].EQBand[5].Frequency = 3000;
    AxumData.PresetData[cntPreset].EQBand[5].Bandwidth = 1;
    AxumData.PresetData[cntPreset].EQBand[5].Slope = 1;
    AxumData.PresetData[cntPreset].EQBand[5].Type = LPF;
    AxumData.PresetData[cntPreset].EQOnOff = false;

    AxumData.PresetData[cntPreset].UseDynamics = false;
    AxumData.PresetData[cntPreset].AGCRatio = 1;
    AxumData.PresetData[cntPreset].AGCThreshold = -20;
    AxumData.PresetData[cntPreset].DynamicsOnOff = false;
    AxumData.PresetData[cntPreset].DownwardExpanderThreshold = -30;

    AxumData.PresetData[cntPreset].UseModule = false;
    AxumData.PresetData[cntPreset].Panorama = 512;
    AxumData.PresetData[cntPreset].FaderLevel = 0;
    AxumData.PresetData[cntPreset].ModuleState = false;

    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      AxumData.BussPresetData[cntPreset][cntBuss].Use = false;
      AxumData.BussPresetData[cntPreset][cntBuss].Level = 0;
      AxumData.BussPresetData[cntPreset][cntBuss].On = false;
    }
    for (int cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
    {
      for (int cntBuss=0; cntBuss<24; cntBuss++)
      {
        AxumData.MonitorBussPresetData[cntPreset][cntMonitorBuss].Use[cntBuss] = 0;
        AxumData.MonitorBussPresetData[cntPreset][cntMonitorBuss].On[cntBuss] = 0;
      }
    }
  }
  for (int cntPreset=0; cntPreset<32; cntPreset++)
  {
    AxumData.ConsolePresetData[cntPreset].Label[0] = 0;
    AxumData.ConsolePresetData[cntPreset].Console[0] = 0;
    AxumData.ConsolePresetData[cntPreset].Console[1] = 0;
    AxumData.ConsolePresetData[cntPreset].Console[2] = 0;
    AxumData.ConsolePresetData[cntPreset].Console[3] = 0;
    AxumData.ConsolePresetData[cntPreset].ModulePreset = -1;
    AxumData.ConsolePresetData[cntPreset].MixMonitorPreset = -1;
    AxumData.ConsolePresetData[cntPreset].SafeRecallTime = 1;
    AxumData.ConsolePresetData[cntPreset].ForcedRecallTime = 3;
  }

  for (int cntDestination=0; cntDestination<1280; cntDestination++)
  {
    AxumData.DestinationData[cntDestination].DestinationName[0] = 0x00;
    for (int cntOutput=0; cntOutput<8; cntOutput++)
    {
      AxumData.DestinationData[cntDestination].OutputData[cntOutput].MambaNetAddress = 0x00000000;
      AxumData.DestinationData[cntDestination].OutputData[cntOutput].SubChannel = -1;
    }

    AxumData.DestinationData[cntDestination].Source = 0;
    AxumData.DestinationData[cntDestination].Level = -140;
    AxumData.DestinationData[cntDestination].Mute = 0;
    AxumData.DestinationData[cntDestination].Dim = 0;
    AxumData.DestinationData[cntDestination].Mono = 0;
    AxumData.DestinationData[cntDestination].Phase = 0;
    AxumData.DestinationData[cntDestination].Routing = 0;
    AxumData.DestinationData[cntDestination].Talkback[0] = 0;
    AxumData.DestinationData[cntDestination].Talkback[1] = 0;
    AxumData.DestinationData[cntDestination].Talkback[2] = 0;
    AxumData.DestinationData[cntDestination].Talkback[3] = 0;

    AxumData.DestinationData[cntDestination].MixMinusSource = 0;
    AxumData.DestinationData[cntDestination].MixMinusActive = 0;

    AxumData.DestinationData[cntDestination].CommBuss = 0;
    AxumData.DestinationData[cntDestination].CommActive = 0;
  }

  for (int cntModule=0; cntModule<128; cntModule++)
  {
    AxumData.ModuleData[cntModule].Console = 0;
    AxumData.ModuleData[cntModule].TemporySourceLocal = 0;
    AxumData.ModuleData[cntModule].TemporySourceControlMode[0]= 0;
    AxumData.ModuleData[cntModule].TemporySourceControlMode[1]= 0;
    AxumData.ModuleData[cntModule].TemporySourceControlMode[2]= 0;
    AxumData.ModuleData[cntModule].TemporySourceControlMode[3]= 0;
    AxumData.ModuleData[cntModule].SelectedSource = 0;
    AxumData.ModuleData[cntModule].TemporyPresetLocal = 0;
    AxumData.ModuleData[cntModule].TemporyPresetControlMode[0] = 0;
    AxumData.ModuleData[cntModule].TemporyPresetControlMode[1] = 0;
    AxumData.ModuleData[cntModule].TemporyPresetControlMode[2] = 0;
    AxumData.ModuleData[cntModule].TemporyPresetControlMode[3] = 0;
    AxumData.ModuleData[cntModule].SelectedProcessingPreset = 0;
    AxumData.ModuleData[cntModule].ModulePreset = 0;
    AxumData.ModuleData[cntModule].Source1A = 0;
    AxumData.ModuleData[cntModule].Source1B = 0;
    AxumData.ModuleData[cntModule].Source2A = 0;
    AxumData.ModuleData[cntModule].Source2B = 0;
    AxumData.ModuleData[cntModule].Source3A = 0;
    AxumData.ModuleData[cntModule].Source3B = 0;
    AxumData.ModuleData[cntModule].Source4A = 0;
    AxumData.ModuleData[cntModule].Source4B = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset1A = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset1B = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset2A = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset2B = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset3A = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset3B = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset4A = 0;
    AxumData.ModuleData[cntModule].ProcessingPreset4B = 0;
    AxumData.ModuleData[cntModule].OverruleActive = 0;
    AxumData.ModuleData[cntModule].WaitingSource = 0;
    AxumData.ModuleData[cntModule].WaitingProcessingPreset = 0;
    AxumData.ModuleData[cntModule].WaitingRoutingPreset = 0;
    AxumData.ModuleData[cntModule].InsertSource = 0;
    AxumData.ModuleData[cntModule].InsertOnOff = 0;
    AxumData.ModuleData[cntModule].Gain = 0;

    AxumData.ModuleData[cntModule].Phase = 0x03;
    AxumData.ModuleData[cntModule].PhaseOnOff = 0;

    AxumData.ModuleData[cntModule].Filter.Level = 0;
    AxumData.ModuleData[cntModule].Filter.Frequency = 80;
    AxumData.ModuleData[cntModule].Filter.Bandwidth = 1;
    AxumData.ModuleData[cntModule].Filter.Slope = 1;
    AxumData.ModuleData[cntModule].Filter.Type = HPF;
    AxumData.ModuleData[cntModule].FilterOnOff = 0;

    AxumData.ModuleData[cntModule].EQBand[0].Level = 0;
    AxumData.ModuleData[cntModule].EQBand[0].Frequency = 12000;
    AxumData.ModuleData[cntModule].EQBand[0].Bandwidth = 1;
    AxumData.ModuleData[cntModule].EQBand[0].Slope = 1;
    AxumData.ModuleData[cntModule].EQBand[0].Type = PEAKINGEQ;

    AxumData.ModuleData[cntModule].EQBand[1].Level = 0;
    AxumData.ModuleData[cntModule].EQBand[1].Frequency = 4000;
    AxumData.ModuleData[cntModule].EQBand[1].Bandwidth = 1;
    AxumData.ModuleData[cntModule].EQBand[1].Slope = 1;
    AxumData.ModuleData[cntModule].EQBand[1].Type = PEAKINGEQ;

    AxumData.ModuleData[cntModule].EQBand[2].Level = 0;
    AxumData.ModuleData[cntModule].EQBand[2].Frequency = 800;
    AxumData.ModuleData[cntModule].EQBand[2].Bandwidth = 1;
    AxumData.ModuleData[cntModule].EQBand[2].Slope = 1;
    AxumData.ModuleData[cntModule].EQBand[2].Type = PEAKINGEQ;

    AxumData.ModuleData[cntModule].EQBand[3].Level = 0;
    AxumData.ModuleData[cntModule].EQBand[3].Frequency = 120;
    AxumData.ModuleData[cntModule].EQBand[3].Bandwidth = 1;
    AxumData.ModuleData[cntModule].EQBand[3].Slope = 1;
    AxumData.ModuleData[cntModule].EQBand[3].Type = LOWSHELF;

    AxumData.ModuleData[cntModule].EQBand[4].Level = 0;
    AxumData.ModuleData[cntModule].EQBand[4].Frequency = 300;
    AxumData.ModuleData[cntModule].EQBand[4].Bandwidth = 1;
    AxumData.ModuleData[cntModule].EQBand[4].Slope = 1;
    AxumData.ModuleData[cntModule].EQBand[4].Type = HPF;

    AxumData.ModuleData[cntModule].EQBand[5].Level = 0;
    AxumData.ModuleData[cntModule].EQBand[5].Frequency = 3000;
    AxumData.ModuleData[cntModule].EQBand[5].Bandwidth = 1;
    AxumData.ModuleData[cntModule].EQBand[5].Slope = 1;
    AxumData.ModuleData[cntModule].EQBand[5].Type = LPF;

    AxumData.ModuleData[cntModule].EQOnOff = 1;

    AxumData.ModuleData[cntModule].AGCRatio = 1;
    AxumData.ModuleData[cntModule].AGCThreshold = -20;
    AxumData.ModuleData[cntModule].DynamicsOnOff = 0;
    AxumData.ModuleData[cntModule].DownwardExpanderThreshold = -30;

    AxumData.ModuleData[cntModule].Panorama = 512;
    AxumData.ModuleData[cntModule].Mono = 0x03;
    AxumData.ModuleData[cntModule].MonoOnOff = 0;

    AxumData.ModuleData[cntModule].FaderLevel = -140;
    AxumData.ModuleData[cntModule].FaderTouch = 0;
    AxumData.ModuleData[cntModule].On = 0;
    AxumData.ModuleData[cntModule].Cough = 0;

    AxumData.ModuleData[cntModule].Signal = 0;
    AxumData.ModuleData[cntModule].Peak = 0;

    for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
    {
      AxumData.ModuleData[cntModule].TalkbackToRelatedDestination[cntTalkback] = 0;
    }

    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      AxumData.ModuleData[cntModule].Buss[cntBuss].Level = 0; //0dB
      AxumData.ModuleData[cntModule].Buss[cntBuss].On = 0;
      for (int cntBuss2=0; cntBuss2<16; cntBuss2++)
      {
        AxumData.ModuleData[cntModule].Buss[cntBuss].PreviousOn[cntBuss2] = 0;
      }
      AxumData.ModuleData[cntModule].Buss[cntBuss].Balance = 512;
      AxumData.ModuleData[cntModule].Buss[cntBuss].PreModuleLevel = 0;
      AxumData.ModuleData[cntModule].Buss[cntBuss].Assigned = 1;

      for (int cntPreset=0; cntPreset<4; cntPreset++)
      {
        AxumData.ModuleData[cntModule].RoutingPreset[cntPreset][cntBuss].Use = 0;
        AxumData.ModuleData[cntModule].RoutingPreset[cntPreset][cntBuss].Level = 0; //0dB
        AxumData.ModuleData[cntModule].RoutingPreset[cntPreset][cntBuss].On = 0;
        AxumData.ModuleData[cntModule].RoutingPreset[cntPreset][cntBuss].Balance = 512;
        AxumData.ModuleData[cntModule].RoutingPreset[cntPreset][cntBuss].PreModuleLevel = 0;
      }
    }

    AxumData.ModuleData[cntModule].Defaults.InsertUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.InsertSource = 0;
    AxumData.ModuleData[cntModule].Defaults.InsertOnOff = 0;

    AxumData.ModuleData[cntModule].Defaults.GainUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.Gain = 0;

    AxumData.ModuleData[cntModule].Defaults.PhaseUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.Phase = 0x03;
    AxumData.ModuleData[cntModule].Defaults.PhaseOnOff = 0;

    AxumData.ModuleData[cntModule].Defaults.MonoUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.Mono = 0x03;
    AxumData.ModuleData[cntModule].Defaults.MonoOnOff = 0;

    AxumData.ModuleData[cntModule].Defaults.FilterUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.Filter.Level = 0;
    AxumData.ModuleData[cntModule].Defaults.Filter.Frequency = 80;
    AxumData.ModuleData[cntModule].Defaults.Filter.Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.Filter.Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.Filter.Type = HPF;
    AxumData.ModuleData[cntModule].Defaults.FilterOnOff = 0;

    AxumData.ModuleData[cntModule].Defaults.EQUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[0].Level = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[0].Frequency = 12000;
    AxumData.ModuleData[cntModule].Defaults.EQBand[0].Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[0].Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[0].Type = PEAKINGEQ;

    AxumData.ModuleData[cntModule].Defaults.EQBand[1].Level = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[1].Frequency = 4000;
    AxumData.ModuleData[cntModule].Defaults.EQBand[1].Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[1].Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[1].Type = PEAKINGEQ;

    AxumData.ModuleData[cntModule].Defaults.EQBand[2].Level = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[2].Frequency = 800;
    AxumData.ModuleData[cntModule].Defaults.EQBand[2].Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[2].Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[2].Type = PEAKINGEQ;

    AxumData.ModuleData[cntModule].Defaults.EQBand[3].Level = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[3].Frequency = 120;
    AxumData.ModuleData[cntModule].Defaults.EQBand[3].Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[3].Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[3].Type = LOWSHELF;

    AxumData.ModuleData[cntModule].Defaults.EQBand[4].Level = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[4].Frequency = 300;
    AxumData.ModuleData[cntModule].Defaults.EQBand[4].Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[4].Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[4].Type = HPF;

    AxumData.ModuleData[cntModule].Defaults.EQBand[5].Level = 0;
    AxumData.ModuleData[cntModule].Defaults.EQBand[5].Frequency = 3000;
    AxumData.ModuleData[cntModule].Defaults.EQBand[5].Bandwidth = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[5].Slope = 1;
    AxumData.ModuleData[cntModule].Defaults.EQBand[5].Type = LPF;

    AxumData.ModuleData[cntModule].Defaults.EQOnOff = 1;

    AxumData.ModuleData[cntModule].Defaults.DynamicsUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.DownwardExpanderThreshold = -20;
    AxumData.ModuleData[cntModule].Defaults.AGCRatio = 1;
    AxumData.ModuleData[cntModule].Defaults.AGCThreshold = -20;
    AxumData.ModuleData[cntModule].Defaults.DynamicsOnOff = 0;

    AxumData.ModuleData[cntModule].Defaults.ModuleUsePreset = 0;
    AxumData.ModuleData[cntModule].Defaults.Panorama = 512;
    AxumData.ModuleData[cntModule].Defaults.FaderLevel = -140;
    AxumData.ModuleData[cntModule].Defaults.On = 0;

    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      AxumData.ModuleData[cntModule].Defaults.Buss[cntBuss].Use = false;
      AxumData.ModuleData[cntModule].Defaults.Buss[cntBuss].Level = 0; //0dB
      AxumData.ModuleData[cntModule].Defaults.Buss[cntBuss].On = 0;
      AxumData.ModuleData[cntModule].Defaults.Buss[cntBuss].Balance = 512;
      AxumData.ModuleData[cntModule].Defaults.Buss[cntBuss].PreModuleLevel = 0;
    }
  }

  AxumData.ConsoleData[0].ControlMode = MODULE_CONTROL_MODE_NONE;
  AxumData.ConsoleData[1].ControlMode = MODULE_CONTROL_MODE_NONE;
  AxumData.ConsoleData[2].ControlMode = MODULE_CONTROL_MODE_NONE;
  AxumData.ConsoleData[3].ControlMode = MODULE_CONTROL_MODE_NONE;
  AxumData.ConsoleData[0].MasterControlMode = MASTER_CONTROL_MODE_NONE;
  AxumData.ConsoleData[1].MasterControlMode = MASTER_CONTROL_MODE_NONE;
  AxumData.ConsoleData[2].MasterControlMode = MASTER_CONTROL_MODE_NONE;
  AxumData.ConsoleData[3].MasterControlMode = MASTER_CONTROL_MODE_NONE;
  AxumData.ConsoleData[0].ControlModeTimerValue = 10000;
  AxumData.ConsoleData[1].ControlModeTimerValue = 10000;
  AxumData.ConsoleData[2].ControlModeTimerValue = 10000;
  AxumData.ConsoleData[3].ControlModeTimerValue = 10000;

  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    AxumData.BussMasterData[cntBuss].Label[0] = 0;
    AxumData.BussMasterData[cntBuss].Level = 0;
    AxumData.BussMasterData[cntBuss].On = 1;

    AxumData.BussMasterData[cntBuss].Console = 0;

    AxumData.BussMasterData[cntBuss].PreModuleOn = 0;
    AxumData.BussMasterData[cntBuss].PreModuleBalance = 0;

    AxumData.BussMasterData[cntBuss].Mono = 0;

    AxumData.BussMasterData[cntBuss].Interlock = 0;
    AxumData.BussMasterData[cntBuss].Exclusive = 0;
    AxumData.BussMasterData[cntBuss].GlobalBussReset = 0;

    for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
    {
      AxumData.BussMasterData[cntBuss].Talkback[cntTalkback] = 0;
    }
    AxumData.BussMasterData[cntBuss].Dim = 0;
  }

  AxumData.Redlight[0] = 0;
  AxumData.Redlight[1] = 0;
  AxumData.Redlight[2] = 0;
  AxumData.Redlight[3] = 0;
  AxumData.Redlight[4] = 0;
  AxumData.Redlight[5] = 0;
  AxumData.Redlight[6] = 0;
  AxumData.Redlight[7] = 0;

  AxumData.Samplerate = 48000;
  AxumData.ExternClock = 0;
  AxumData.Headroom = 20;
  AxumData.LevelReserve = 0;
  AxumData.AutoMomentary = false;
  AxumData.StartupState = false;
  for (int cntConsole=0; cntConsole<4; cntConsole++)
  {
    AxumData.ConsoleData[cntConsole].SelectedConsolePreset = 0;
    AxumData.ConsoleData[cntConsole].SelectedModule = 0;
    AxumData.ConsoleData[cntConsole].SelectedModuleTimeout = 0;
    AxumData.ConsoleData[cntConsole].SelectedBuss = 0;
    AxumData.ConsoleData[cntConsole].SelectedBussTimeout = 0;
    AxumData.ConsoleData[cntConsole].SelectedMonitorBuss = 0;
    AxumData.ConsoleData[cntConsole].SelectedMonitorBussTimeout = 0;
    AxumData.ConsoleData[cntConsole].SelectedSource = 0;
    AxumData.ConsoleData[cntConsole].SelectedSourceTimeout = 0;
    AxumData.ConsoleData[cntConsole].SelectedDestination = 0;
    AxumData.ConsoleData[cntConsole].SelectedDestinationTimeout = 0;
    memset(AxumData.ConsoleData[cntConsole].Username, 0, 33);
    memset(AxumData.ConsoleData[cntConsole].Password, 0, 17);
    memset(AxumData.ConsoleData[cntConsole].ActiveUsername, 0, 33);
    memset(AxumData.ConsoleData[cntConsole].ActivePassword, 0, 17);
    memset(AxumData.ConsoleData[cntConsole].UsernameToWrite, 0, 33);
    memset(AxumData.ConsoleData[cntConsole].PasswordToWrite, 0, 17);
    AxumData.ConsoleData[cntConsole].UserLevel = 0;
    AxumData.ConsoleData[cntConsole].SourcePool = 2;
    AxumData.ConsoleData[cntConsole].PresetPool = 2;
    AxumData.ConsoleData[cntConsole].LogoutToIdle = 1;
    AxumData.ConsoleData[cntConsole].ConsolePreset = 0;
    AxumData.ConsoleData[cntConsole].DotCountUpDown = 0;
    AxumData.ConsoleData[cntConsole].ProgramEndTimeEnable = 0;
    AxumData.ConsoleData[cntConsole].ProgramEndTimeHours = 0;
    AxumData.ConsoleData[cntConsole].ProgramEndTimeMinutes = 0;
    AxumData.ConsoleData[cntConsole].ProgramEndTimeSeconds = 0;
    AxumData.ConsoleData[cntConsole].CountDownTimer = 0;
  }

  for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
  {
    AxumData.Talkback[cntTalkback].Source = 0;
  }

  for (int cntMonitor=0; cntMonitor<16; cntMonitor++)
  {
    AxumData.Monitor[cntMonitor].Label[0] = 0;
    AxumData.Monitor[cntMonitor].Console = 0;
    AxumData.Monitor[cntMonitor].Interlock = 0;
    AxumData.Monitor[cntMonitor].DefaultSelection = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[0] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[1] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[2] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[3] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[4] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[5] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[6] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[7] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[8] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[9] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[10] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[11] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[12] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[13] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[14] = 0;
    AxumData.Monitor[cntMonitor].AutoSwitchingBuss[15] = 0;
    AxumData.Monitor[cntMonitor].SwitchingDimLevel = -140;
    AxumData.Monitor[cntMonitor].Buss[0] = 0;
    AxumData.Monitor[cntMonitor].Buss[1] = 0;
    AxumData.Monitor[cntMonitor].Buss[2] = 0;
    AxumData.Monitor[cntMonitor].Buss[3] = 0;
    AxumData.Monitor[cntMonitor].Buss[4] = 0;
    AxumData.Monitor[cntMonitor].Buss[5] = 0;
    AxumData.Monitor[cntMonitor].Buss[6] = 0;
    AxumData.Monitor[cntMonitor].Buss[7] = 0;
    AxumData.Monitor[cntMonitor].Buss[8] = 0;
    AxumData.Monitor[cntMonitor].Buss[9] = 0;
    AxumData.Monitor[cntMonitor].Buss[10] = 0;
    AxumData.Monitor[cntMonitor].Buss[11] = 0;
    AxumData.Monitor[cntMonitor].Buss[12] = 0;
    AxumData.Monitor[cntMonitor].Buss[13] = 0;
    AxumData.Monitor[cntMonitor].Buss[14] = 0;
    AxumData.Monitor[cntMonitor].Buss[15] = 0;
    AxumData.Monitor[cntMonitor].Ext[0] = 0;
    AxumData.Monitor[cntMonitor].Ext[1] = 0;
    AxumData.Monitor[cntMonitor].Ext[2] = 0;
    AxumData.Monitor[cntMonitor].Ext[3] = 0;
    AxumData.Monitor[cntMonitor].Ext[4] = 0;
    AxumData.Monitor[cntMonitor].Ext[5] = 0;
    AxumData.Monitor[cntMonitor].Ext[6] = 0;
    AxumData.Monitor[cntMonitor].Ext[7] = 0;
    AxumData.Monitor[cntMonitor].PhonesLevel = -140;
    AxumData.Monitor[cntMonitor].SpeakerLevel = -140;
    AxumData.Monitor[cntMonitor].Dim = 0;
    AxumData.Monitor[cntMonitor].Mute = 0;
  }

  for (int cntMonitor=0; cntMonitor<4; cntMonitor++)
  {
    AxumData.ExternSource[cntMonitor].Ext[0] = 0;
    AxumData.ExternSource[cntMonitor].Ext[1] = 0;
    AxumData.ExternSource[cntMonitor].Ext[2] = 0;
    AxumData.ExternSource[cntMonitor].Ext[3] = 0;
    AxumData.ExternSource[cntMonitor].Ext[4] = 0;
    AxumData.ExternSource[cntMonitor].Ext[5] = 0;
    AxumData.ExternSource[cntMonitor].Ext[6] = 0;
    AxumData.ExternSource[cntMonitor].Ext[7] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[0] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[1] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[2] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[3] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[4] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[5] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[6] = 0;
    AxumData.ExternSource[cntMonitor].InterlockSafe[7] = 0;
  }

  AxumData.PercentInitialized = 0;
}

ONLINE_NODE_INFORMATION_STRUCT *GetOnlineNodeInformation(unsigned long int addr)
{
  bool Found = false;
  ONLINE_NODE_INFORMATION_STRUCT *OnlineNodeInformationElement = OnlineNodeInformationList;
  ONLINE_NODE_INFORMATION_STRUCT *FoundOnlineNodeInformationElement = NULL;

  while ((!Found) && (OnlineNodeInformationElement != NULL))
  {
    if (OnlineNodeInformationElement->MambaNetAddress == addr)
    {
      Found = true;
      FoundOnlineNodeInformationElement = OnlineNodeInformationElement;
    }
    OnlineNodeInformationElement = OnlineNodeInformationElement->Next;
  }

  return FoundOnlineNodeInformationElement;
}

void DoAxum_LoadProcessingPreset(unsigned char ModuleNr, int NewProcessingPresetNr, unsigned char OverrideAtSourceSelect, unsigned char UseModuleDefaults, unsigned char SetAllObjects)
{
  bool SetModuleProcessing = false;
  bool SetModuleControllers = false;
  bool SetBussProcessing = false;
  unsigned char cntEQ;
  //parameters per module
  float Gain = AxumData.ModuleData[ModuleNr].Gain;
  unsigned int Frequency = AxumData.ModuleData[ModuleNr].Filter.Frequency;
  bool FilterOnOff = AxumData.ModuleData[ModuleNr].FilterOnOff;
  unsigned int InsertSource = AxumData.ModuleData[ModuleNr].InsertSource;
  bool InsertOnOff = AxumData.ModuleData[ModuleNr].InsertOnOff;
  unsigned char Phase = AxumData.ModuleData[ModuleNr].Phase;
  bool PhaseOnOff = AxumData.ModuleData[ModuleNr].PhaseOnOff;
  unsigned char Mono = AxumData.ModuleData[ModuleNr].Mono;
  bool MonoOnOff = AxumData.ModuleData[ModuleNr].MonoOnOff;

  bool EQOnOff = AxumData.ModuleData[ModuleNr].EQOnOff;
  AXUM_EQ_BAND_PRESET_STRUCT EQBand[6];
  float AGCRatio = AxumData.ModuleData[ModuleNr].AGCRatio;
  float AGCThreshold = AxumData.ModuleData[ModuleNr].AGCThreshold;
  bool DynamicsOnOff = AxumData.ModuleData[ModuleNr].DynamicsOnOff;
  float DownwardExpanderThreshold = AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold;
  float FaderLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
  bool ModuleState = AxumData.ModuleData[ModuleNr].On;
  int Panorama = AxumData.ModuleData[ModuleNr].Panorama;
  int PresetNr = AxumData.ModuleData[ModuleNr].SelectedProcessingPreset;
  unsigned char CurrentPresetState[8];
  unsigned char cntPreset;

  if (NewProcessingPresetNr == -1)
  {
    UseModuleDefaults = true;
  }

  for (cntPreset=0; cntPreset<8; cntPreset++)
  {
    CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
  }

  for (int cntEQ=0; cntEQ<6; cntEQ++)
  {
    EQBand[cntEQ].Range = AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Range;
    EQBand[cntEQ].Level = AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Level;
    EQBand[cntEQ].Frequency = AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Frequency;
    EQBand[cntEQ].Bandwidth = AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Bandwidth;
    EQBand[cntEQ].Slope = AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Slope;
    EQBand[cntEQ].Type = AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Type;
  }

  if (NewProcessingPresetNr>0)
  {
    AXUM_PRESET_DATA_STRUCT *PresetData = &AxumData.PresetData[NewProcessingPresetNr-1];

    if ((PresetData->UseGain) || (AxumData.ModuleData[ModuleNr].Defaults.GainUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseGain)
      {
        Gain = PresetData->Gain;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        Gain = AxumData.ModuleData[ModuleNr].Defaults.Gain;
      }
    }

    if ((PresetData->UseFilter) || (AxumData.ModuleData[ModuleNr].Defaults.FilterUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseFilter)
      {
        Frequency = PresetData->Filter.Frequency;
        FilterOnOff = PresetData->FilterOnOff;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        Frequency = AxumData.ModuleData[ModuleNr].Defaults.Filter.Frequency;
        FilterOnOff = AxumData.ModuleData[ModuleNr].Defaults.FilterOnOff;
      }
    }

    if ((PresetData->UseInsert) || (AxumData.ModuleData[ModuleNr].Defaults.InsertUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseInsert)
      {
        InsertSource = AxumData.ModuleData[ModuleNr].Defaults.InsertSource;
        InsertOnOff = PresetData->InsertOnOff;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        InsertSource = AxumData.ModuleData[ModuleNr].Defaults.InsertSource;
        InsertOnOff = AxumData.ModuleData[ModuleNr].Defaults.InsertOnOff;
      }
    }

    if ((PresetData->UsePhase) || (AxumData.ModuleData[ModuleNr].Defaults.PhaseUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UsePhase)
      {
        Phase = PresetData->Phase;
        PhaseOnOff = PresetData->PhaseOnOff;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        Phase = AxumData.ModuleData[ModuleNr].Defaults.Phase;
        PhaseOnOff = AxumData.ModuleData[ModuleNr].Defaults.PhaseOnOff;
      }
    }
    if ((PresetData->UseMono) || (AxumData.ModuleData[ModuleNr].Defaults.MonoUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseMono)
      {
        Mono = PresetData->Mono;
        MonoOnOff = PresetData->MonoOnOff;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        Mono = AxumData.ModuleData[ModuleNr].Defaults.Mono;
        MonoOnOff = AxumData.ModuleData[ModuleNr].Defaults.MonoOnOff;
      }
    }

    if ((PresetData->UseEQ) || (AxumData.ModuleData[ModuleNr].Defaults.EQUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseEQ)
      {
        EQOnOff = PresetData->EQOnOff;
        for (cntEQ=0; cntEQ<6; cntEQ++)
        {
          EQBand[cntEQ].Range = PresetData->EQBand[cntEQ].Range;
          EQBand[cntEQ].Level = PresetData->EQBand[cntEQ].Level;
          EQBand[cntEQ].Frequency = PresetData->EQBand[cntEQ].Frequency;
          EQBand[cntEQ].Bandwidth = PresetData->EQBand[cntEQ].Bandwidth;
          EQBand[cntEQ].Slope = PresetData->EQBand[cntEQ].Slope;
          EQBand[cntEQ].Type = PresetData->EQBand[cntEQ].Type;
        }
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        EQOnOff = AxumData.ModuleData[ModuleNr].Defaults.EQOnOff;
        for (cntEQ=0; cntEQ<6; cntEQ++)
        {
          EQBand[cntEQ].Range = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Range;
          EQBand[cntEQ].Level = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Level;
          EQBand[cntEQ].Frequency = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Frequency;
          EQBand[cntEQ].Bandwidth = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Bandwidth;
          EQBand[cntEQ].Slope = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Slope;
          EQBand[cntEQ].Type = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Type;
        }
      }
    }

    if ((PresetData->UseDynamics) || (AxumData.ModuleData[ModuleNr].Defaults.DynamicsUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseDynamics)
      {
        AGCRatio = PresetData->AGCRatio;
        AGCThreshold = PresetData->AGCThreshold;
        DynamicsOnOff = PresetData->DynamicsOnOff;
        DownwardExpanderThreshold = PresetData->DownwardExpanderThreshold;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        AGCRatio = AxumData.ModuleData[ModuleNr].Defaults.AGCRatio;
        AGCThreshold = AxumData.ModuleData[ModuleNr].Defaults.AGCThreshold;
        DynamicsOnOff = AxumData.ModuleData[ModuleNr].Defaults.DynamicsOnOff;
        DownwardExpanderThreshold = AxumData.ModuleData[ModuleNr].Defaults.DownwardExpanderThreshold;
      }
    }

    if ((PresetData->UseModule) || (AxumData.ModuleData[ModuleNr].Defaults.ModuleUsePreset) || (UseModuleDefaults))
    {
      if (PresetData->UseModule)
      {
        Panorama = PresetData->Panorama;
        FaderLevel = PresetData->FaderLevel;
        ModuleState = PresetData->ModuleState;
      }
      else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
      {
        Panorama = AxumData.ModuleData[ModuleNr].Defaults.Panorama;
        FaderLevel = AxumData.ModuleData[ModuleNr].Defaults.FaderLevel;
        ModuleState = AxumData.ModuleData[ModuleNr].Defaults.On;
      }
    }

    PresetNr = NewProcessingPresetNr;
  }
  else if ((OverrideAtSourceSelect) || (UseModuleDefaults))
  {
    if ((AxumData.ModuleData[ModuleNr].Defaults.GainUsePreset) || (UseModuleDefaults))
    {
      Gain = AxumData.ModuleData[ModuleNr].Defaults.Gain;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.FilterUsePreset) || (UseModuleDefaults))
    {
      Frequency = AxumData.ModuleData[ModuleNr].Defaults.Filter.Frequency;
      FilterOnOff = AxumData.ModuleData[ModuleNr].Defaults.FilterOnOff;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.InsertUsePreset) || (UseModuleDefaults))
    {
      InsertSource = AxumData.ModuleData[ModuleNr].Defaults.InsertSource;
      InsertOnOff = AxumData.ModuleData[ModuleNr].Defaults.InsertOnOff;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.PhaseUsePreset) || (UseModuleDefaults))
    {
      Phase = AxumData.ModuleData[ModuleNr].Defaults.Phase;
      PhaseOnOff = AxumData.ModuleData[ModuleNr].Defaults.PhaseOnOff;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.MonoUsePreset) || (UseModuleDefaults))
    {
      Mono = AxumData.ModuleData[ModuleNr].Defaults.Mono;
      MonoOnOff = AxumData.ModuleData[ModuleNr].Defaults.MonoOnOff;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.EQUsePreset) || (UseModuleDefaults))
    {
      for (cntEQ=0; cntEQ<6; cntEQ++)
      {
        EQBand[cntEQ].Range = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Range;
        EQBand[cntEQ].Level = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Level;
        EQBand[cntEQ].Frequency = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Frequency;
        EQBand[cntEQ].Bandwidth = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Bandwidth;
        EQBand[cntEQ].Slope = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Slope;
        EQBand[cntEQ].Type = AxumData.ModuleData[ModuleNr].Defaults.EQBand[cntEQ].Type;
      }
      EQOnOff = AxumData.ModuleData[ModuleNr].Defaults.EQOnOff;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.DynamicsUsePreset) || (UseModuleDefaults))
    {
      DownwardExpanderThreshold = AxumData.ModuleData[ModuleNr].Defaults.DownwardExpanderThreshold;
      AGCRatio = AxumData.ModuleData[ModuleNr].Defaults.AGCRatio;
      AGCThreshold = AxumData.ModuleData[ModuleNr].Defaults.AGCThreshold;
      DynamicsOnOff = AxumData.ModuleData[ModuleNr].Defaults.DynamicsOnOff;
    }

    if ((AxumData.ModuleData[ModuleNr].Defaults.ModuleUsePreset) || (UseModuleDefaults))
    {
      Panorama = AxumData.ModuleData[ModuleNr].Defaults.Panorama;
      FaderLevel = AxumData.ModuleData[ModuleNr].Defaults.FaderLevel;
      ModuleState = AxumData.ModuleData[ModuleNr].Defaults.On;
    }

    if (((AxumData.ModuleData[ModuleNr].Defaults.GainUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.FilterUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.InsertUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.PhaseUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.MonoUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.EQUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.DynamicsUsePreset) ||
        (AxumData.ModuleData[ModuleNr].Defaults.ModuleUsePreset) || (UseModuleDefaults)) ||
       (NewProcessingPresetNr == -1))

    {
      PresetNr = NewProcessingPresetNr;
    }
  }

  if (AxumData.ModuleData[ModuleNr].SelectedProcessingPreset != PresetNr)
  {
    AxumData.ModuleData[ModuleNr].SelectedProcessingPreset = PresetNr;

    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET);
    SetModuleControllers = true;
  }


  //Set if there is a difference
  if ((AxumData.ModuleData[ModuleNr].Gain != Gain) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);

    AxumData.ModuleData[ModuleNr].Gain = Gain;
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_GAIN_LEVEL);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].Filter.Frequency != Frequency) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].Filter.Frequency = Frequency;

    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_LOW_CUT_FREQUENCY);
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }

  if ((AxumData.ModuleData[ModuleNr].FilterOnOff != FilterOnOff) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].FilterOnOff = FilterOnOff;

    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_LOW_CUT_ON_OFF);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }

  if ((AxumData.ModuleData[ModuleNr].InsertSource != InsertSource) || (SetAllObjects))
  {
    AxumData.ModuleData[ModuleNr].InsertSource = InsertSource;
    SetAxum_ModuleInsertSource(ModuleNr);
    SetModuleControllers = true;
  }

  if ((AxumData.ModuleData[ModuleNr].InsertOnOff != InsertOnOff) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].InsertOnOff = InsertOnOff;
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_INSERT_ON_OFF);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }

  if ((AxumData.ModuleData[ModuleNr].Phase != Phase) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].Phase = Phase;
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].PhaseOnOff != PhaseOnOff) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].PhaseOnOff = PhaseOnOff;
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE_ON_OFF);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PHASE);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }

  if ((AxumData.ModuleData[ModuleNr].Mono != Mono) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].Mono = Mono;
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].MonoOnOff != MonoOnOff) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    AxumData.ModuleData[ModuleNr].MonoOnOff = MonoOnOff;
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO_ON_OFF);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MONO);

    SetBussProcessing = true;
    SetModuleControllers = true;
  }

  if ((AxumData.ModuleData[ModuleNr].EQOnOff != EQOnOff) || (SetAllObjects))
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);

    AxumData.ModuleData[ModuleNr].EQOnOff = EQOnOff;
    for (int cntBand=0; cntBand<6; cntBand++)
    {
      SetAxum_EQ(ModuleNr, cntBand);
    }

    FunctionNrToSent = (ModuleNr<<12) | MODULE_FUNCTION_EQ_ON_OFF;
    CheckObjectsToSent(FunctionNrToSent);

    SetModuleControllers = true;
  }

  for (cntEQ=0; cntEQ<6; cntEQ++)
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    bool EQBandChanged = false;

    if (AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Range != EQBand[cntEQ].Range)
    {
      AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Range = EQBand[cntEQ].Range;

      if (EQBand[cntEQ].Level<-EQBand[cntEQ].Range)
      {
        EQBand[cntEQ].Level = -EQBand[cntEQ].Range;
      }
      else if (EQBand[cntEQ].Level>EQBand[cntEQ].Range)
      {
        EQBand[cntEQ].Level = EQBand[cntEQ].Range;
      }
    }

    if ((AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Level != EQBand[cntEQ].Level) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Level = EQBand[cntEQ].Level;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_LEVEL+(cntEQ*(MODULE_FUNCTION_EQ_BAND_2_LEVEL-MODULE_FUNCTION_EQ_BAND_1_LEVEL))));
      EQBandChanged = true;
    }
    if ((AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Frequency != EQBand[cntEQ].Frequency) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Frequency = EQBand[cntEQ].Frequency;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_FREQUENCY+(cntEQ*(MODULE_FUNCTION_EQ_BAND_2_FREQUENCY-MODULE_FUNCTION_EQ_BAND_1_FREQUENCY))));
      EQBandChanged = true;
    }
    if ((AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Bandwidth != EQBand[cntEQ].Bandwidth) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Bandwidth = EQBand[cntEQ].Bandwidth;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH+(cntEQ*(MODULE_FUNCTION_EQ_BAND_2_BANDWIDTH-MODULE_FUNCTION_EQ_BAND_1_BANDWIDTH))));
      EQBandChanged = true;
    }
    if ((AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Type != EQBand[cntEQ].Type) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].EQBand[cntEQ].Type = EQBand[cntEQ].Type;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_EQ_BAND_1_TYPE+(cntEQ*(MODULE_FUNCTION_EQ_BAND_2_TYPE-MODULE_FUNCTION_EQ_BAND_1_TYPE))));
      EQBandChanged = true;
    }

    if (EQBandChanged)
    {
      SetAxum_EQ(ModuleNr, cntEQ);
      SetModuleControllers = true;
    }
  }
  if ((AxumData.ModuleData[ModuleNr].AGCRatio != AGCRatio) || (SetAllObjects))
  {
    AxumData.ModuleData[ModuleNr].AGCRatio = AGCRatio;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_AGC_AMOUNT);
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].AGCThreshold != AGCThreshold) || (SetAllObjects))
  {
    AxumData.ModuleData[ModuleNr].AGCThreshold = AGCThreshold;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_AGC_THRESHOLD);
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold != DownwardExpanderThreshold) || (SetAllObjects))
  {
    AxumData.ModuleData[ModuleNr].DownwardExpanderThreshold = DownwardExpanderThreshold;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_EXPANDER_THRESHOLD);
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].DynamicsOnOff != DynamicsOnOff) || (SetAllObjects))
  {
    AxumData.ModuleData[ModuleNr].DynamicsOnOff = DynamicsOnOff;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_DYNAMICS_ON_OFF);
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].Panorama != Panorama) || (SetAllObjects))
  {
    AxumData.ModuleData[ModuleNr].Panorama = Panorama;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PAN);

    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].FaderLevel != FaderLevel) || (SetAllObjects))
  {
    float CurrentLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
    AxumData.ModuleData[ModuleNr].FaderLevel = FaderLevel;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_LEVEL);

    if (((CurrentLevel<=-80) && (FaderLevel>-80)) ||
        ((CurrentLevel>-80) && (FaderLevel<=-80)))
    { //fader on changed
      DoAxum_ModuleStatusChanged(ModuleNr, 1);

      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_OFF);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_ON_OFF);

      if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
      {
        unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource - matrix_sources.src_offset.min.source;
        FunctionNrToSent = 0x05000000 | (SourceNr<<12);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_ON);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_OFF);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_ON_OFF);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
      }

      unsigned char On = AxumData.ModuleData[ModuleNr].On;
      DoAxum_StartStopTrigger(ModuleNr, CurrentLevel, FaderLevel, On, On);
    }
    SetBussProcessing = true;
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }
  if ((AxumData.ModuleData[ModuleNr].On != ModuleState) || (SetAllObjects))
  {
    bool CurrentOn = AxumData.ModuleData[ModuleNr].On;

    AxumData.ModuleData[ModuleNr].On = ModuleState;
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_ON);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_OFF);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_MODULE_ON_OFF);

    if (CurrentOn != AxumData.ModuleData[ModuleNr].On)
    { //module on changed
      DoAxum_ModuleStatusChanged(ModuleNr, 1);

      FunctionNrToSent = (ModuleNr<<12);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_INACTIVE);
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_FADER_AND_ON_ACTIVE_INACTIVE);

      if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
      {
        unsigned int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource-matrix_sources.src_offset.min.source;
        FunctionNrToSent = 0x05000000 | (SourceNr<<12);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_ON);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_OFF);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_ON_OFF);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_INACTIVE);
        CheckObjectsToSent(FunctionNrToSent | SOURCE_FUNCTION_MODULE_FADER_AND_ON_ACTIVE_INACTIVE);
      }
      float FaderLevel = AxumData.ModuleData[ModuleNr].FaderLevel;
      DoAxum_StartStopTrigger(ModuleNr, FaderLevel, FaderLevel, CurrentOn, AxumData.ModuleData[ModuleNr].On);
    }
    SetBussProcessing = true;
    SetModuleProcessing = true;
    SetModuleControllers = true;
  }


  if (SetModuleProcessing)
  {
    SetAxum_ModuleProcessing(ModuleNr);
  }

  if (SetBussProcessing)
  {
    SetAxum_BussLevels(ModuleNr);
  }

  if (SetModuleControllers)
  {
    unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);

    if (AxumData.ConsoleData[0].ControlMode == MODULE_CONTROL_MODE_MODULE_PRESET)
    {
      AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[0] = PresetNr;
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1_LABEL);
    }
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1);
    if (AxumData.ConsoleData[1].ControlMode == MODULE_CONTROL_MODE_MODULE_PRESET)
    {
      AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[1] = PresetNr;
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2_LABEL);
    }
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2);
    if (AxumData.ConsoleData[2].ControlMode == MODULE_CONTROL_MODE_MODULE_PRESET)
    {
      AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[2] = PresetNr;
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3_LABEL);
    }
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3);
    if (AxumData.ConsoleData[3].ControlMode == MODULE_CONTROL_MODE_MODULE_PRESET)
    {
      AxumData.ModuleData[ModuleNr].TemporyPresetControlMode[3] = PresetNr;
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4_LABEL);
    }
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4);

    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
  }

  for (cntPreset=0; cntPreset<8; cntPreset++)
  {
    if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
    {
      unsigned int FunctionNrToSent = (ModuleNr<<12);
      CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

      //if A or B
      unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
      if (cntPreset == PresetNr)
      {
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
      }
      else if (cntPreset == (PresetNr+1))
      {
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
      }
    }
  }
}

void DoAxum_LoadRoutingPreset(unsigned char ModuleNr, int PresetNr, unsigned char OverrideAtSourceSelect, unsigned char UseModuleDefaults, unsigned char SetAllObjects)
{
  unsigned char cntBuss;
  bool BussChanged = false;
  bool SetModuleControllers = false;
  unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);
  AXUM_ROUTING_PRESET_DATA_STRUCT *SelectedRoutingPreset = NULL;
  unsigned char NewExclusiveActive = 0;
  unsigned char CurrentExclusiveActive = 0;
  unsigned char CurrentPresetState[8];
  unsigned char cntPreset;

  if (PresetNr == -1)
  {
    UseModuleDefaults = true;
  }

  for (cntPreset=0; cntPreset<8; cntPreset++)
  {
    CurrentPresetState[cntPreset] = ModulePresetActive(ModuleNr, cntPreset+1);
  }

  //First check if the preset enables an exclusive buss.
  for (cntBuss=0; cntBuss<16; cntBuss++)
  {
    if (AxumData.BussMasterData[cntBuss].Exclusive == 1)
    {
      if (AxumData.ModuleData[ModuleNr].Buss[cntBuss].On)
      {
        CurrentExclusiveActive = 1;
      }

      SelectedRoutingPreset = NULL;
      if (PresetNr>0)
      {
        SelectedRoutingPreset = &AxumData.ModuleData[ModuleNr].RoutingPreset[PresetNr-1][cntBuss];
      }

      if ((SelectedRoutingPreset != NULL) && (SelectedRoutingPreset->Use))
      {
        if (SelectedRoutingPreset->On)
        {
          NewExclusiveActive = 1;
        }
      }
      else if ((AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].Use) && ((OverrideAtSourceSelect) || (UseModuleDefaults)))
      {
        if (AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].On)
        {
          NewExclusiveActive = 1;
        }
      }
      else
      {
        NewExclusiveActive = CurrentExclusiveActive;
      }
    }
  }

  //Special case found where an exlusive buss is active in this preset
  if ((CurrentExclusiveActive==0) && (NewExclusiveActive==1))
  {
    for (cntBuss=0; cntBuss<16; cntBuss++)
    {
      for (int cntBuss2=0; cntBuss2<16; cntBuss2++)
      {
        AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[cntBuss2] = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
      }
    }
  }

  //Now set the preset as programmes
  for (cntBuss=0; cntBuss<16; cntBuss++)
  {
    float Level = AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level;
    bool On = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
    signed int Balance = AxumData.ModuleData[ModuleNr].Buss[cntBuss].Balance;
    bool PreModuleLevel = AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreModuleLevel;

    //Special case found where an exlusive buss is active in this preset
    if ((CurrentExclusiveActive==0) && (NewExclusiveActive==1))
    {
      for (int cntBuss2=0; cntBuss2<16; cntBuss2++)
      {
        AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[cntBuss2] = AxumData.ModuleData[ModuleNr].Buss[cntBuss].On;
      }
    }
    if ((CurrentExclusiveActive==1) && (NewExclusiveActive==0))
    {
      if (!AxumData.BussMasterData[cntBuss].Exclusive == 1)
      {
        On = AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreviousOn[cntBuss];
      }
    }

    if (PresetNr>0)
    {
      SelectedRoutingPreset = &AxumData.ModuleData[ModuleNr].RoutingPreset[PresetNr-1][cntBuss];
    }

    if ((SelectedRoutingPreset != NULL) && (SelectedRoutingPreset->Use))
    {
      Level = SelectedRoutingPreset->Level;
      On = SelectedRoutingPreset->On;
      Balance = SelectedRoutingPreset->Balance;
      PreModuleLevel = SelectedRoutingPreset->PreModuleLevel;
    }
    else if (((AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].Use) && (OverrideAtSourceSelect)) || (UseModuleDefaults))
    {
      Level = AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].Level;
      On = AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].On;
      Balance = AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].Balance;
      PreModuleLevel = AxumData.ModuleData[ModuleNr].Defaults.Buss[cntBuss].PreModuleLevel;
    }
    else if ((CurrentExclusiveActive==0) && (NewExclusiveActive==1))
    {
      if (!AxumData.BussMasterData[cntBuss].Exclusive == 1)
      {
        On = 0;
      }
    }

    if((AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level != Level) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level = Level;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_LEVEL+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_LEVEL-MODULE_FUNCTION_BUSS_1_2_LEVEL))));
      BussChanged = true;
      SetModuleControllers = true;
    }
    if((AxumData.ModuleData[ModuleNr].Buss[cntBuss].On != On) || (SetAllObjects))
    {
      DoAxum_SetBussOnOff(ModuleNr, cntBuss, On, 1);
      SetModuleControllers = true;
    }
    if((AxumData.ModuleData[ModuleNr].Buss[cntBuss].Balance != Balance) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].Buss[cntBuss].Balance = Balance;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_BALANCE+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_BALANCE-MODULE_FUNCTION_BUSS_1_2_BALANCE))));
      BussChanged = true;
      SetModuleControllers = true;
    }
    if((AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreModuleLevel != PreModuleLevel) || (SetAllObjects))
    {
      AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreModuleLevel = PreModuleLevel;
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_BUSS_1_2_PRE+(cntBuss*(MODULE_FUNCTION_BUSS_3_4_PRE-MODULE_FUNCTION_BUSS_1_2_PRE))));
      BussChanged = true;
      SetModuleControllers = true;
    }
  }

  if (BussChanged)
  {
    SetAxum_BussLevels(ModuleNr);
  }
  if (SetModuleControllers)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3);
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4);
  }

  for (cntPreset=0; cntPreset<8; cntPreset++)
  {
    if (CurrentPresetState[cntPreset] != ModulePresetActive(ModuleNr, cntPreset+1))
    {
      unsigned int FunctionNrToSent = (ModuleNr<<12);
      CheckObjectsToSent(FunctionNrToSent | GetModuleFunctionNrFromPresetNr(cntPreset+1));

      //if A or B
      unsigned char PresetNr = AxumData.ModuleData[ModuleNr].ModulePreset<<1;
      if (cntPreset == PresetNr)
      {
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
      }
      else if (cntPreset == (PresetNr+1))
      {
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
        CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
      }
    }
  }
}

void DoAxum_LoadBussMasterPreset(unsigned char PresetNr, char *Console, bool SetAllObjects)
{
  int cntBuss;

  for (cntBuss=0; cntBuss<16; cntBuss++)
  {
    if (Console[AxumData.BussMasterData[cntBuss].Console])
    {
      float Level = AxumData.BussMasterData[cntBuss].Level;
      bool On = AxumData.BussMasterData[cntBuss].On;
      if (AxumData.BussPresetData[PresetNr][cntBuss].Use)
      {
        Level = AxumData.BussPresetData[PresetNr][cntBuss].Level;
        On = AxumData.BussPresetData[PresetNr][cntBuss].On;
      }

      unsigned int FunctionNrToSent = 0x01000000 | ((cntBuss<<12)&0xFFF000);
      if ((AxumData.BussMasterData[cntBuss].Level != Level) || SetAllObjects)
      {
        AxumData.BussMasterData[cntBuss].Level = Level;

        CheckObjectsToSent(FunctionNrToSent | BUSS_FUNCTION_MASTER_LEVEL);
      }
      if ((AxumData.BussMasterData[cntBuss].On != On) || SetAllObjects)
      {
        AxumData.BussMasterData[cntBuss].On = On;

        CheckObjectsToSent(FunctionNrToSent | BUSS_FUNCTION_MASTER_ON_OFF);
      }

      SetAxum_BussMasterLevels();

      FunctionNrToSent = 0x03000000 | (AxumData.BussMasterData[cntBuss].Console<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MASTER_CONTROL);
    }
  }
}

void DoAxum_LoadMonitorBussPreset(unsigned char PresetNr, char *Console, bool SetAllObjects)
{
  int cntMonitorBuss;
  int cntBuss;

  for (cntMonitorBuss=0; cntMonitorBuss<16; cntMonitorBuss++)
  {
    if (Console[AxumData.Monitor[cntMonitorBuss].Console])
    {
      bool Change = false;
      for (cntBuss=0; cntBuss<24; cntBuss++)
      {
        bool On;
        bool *CurrentBussOn;
        if (cntBuss<16)
        {
          CurrentBussOn = &AxumData.Monitor[cntMonitorBuss].Buss[cntBuss];
        }
        else if (cntBuss<24)
        {
          CurrentBussOn = &AxumData.Monitor[cntMonitorBuss].Ext[cntBuss-16];
        }
        On = *CurrentBussOn;

        if ((AxumData.MonitorBussPresetData[PresetNr][cntMonitorBuss].Use[cntBuss]) || (SetAllObjects))
        {
          On = AxumData.MonitorBussPresetData[PresetNr][cntMonitorBuss].On[cntBuss];
        }

        if ((*CurrentBussOn != On) || (SetAllObjects))
        {
          *CurrentBussOn = On;
          Change = true;

          unsigned int FunctionNrToSent = 0x02000000 | ((cntMonitorBuss<<12)&0xFFF000);
          if (cntBuss<16)
          {
            CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_ON+cntBuss));
            CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF+cntBuss));
            CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF+cntBuss));
          }
          else if (cntBuss<24)
          {
            CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_ON+(cntBuss-16)));
            CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_OFF+(cntBuss-16)));
            CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF+(cntBuss-16)));
          }
        }
      }

      if (Change)
      {
        SetAxum_MonitorBuss(cntMonitorBuss);
      }
    }
  }
}

void DoAxum_LoadConsolePreset(unsigned char PresetNr, bool SetAllObjects, bool DisableActiveCheck)
{
  if ((PresetNr>0) && (PresetNr<33))
  {
    char ModulePreset = AxumData.ConsolePresetData[PresetNr-1].ModulePreset;
    int MixMonitorPresetNr = AxumData.ConsolePresetData[PresetNr-1].MixMonitorPreset;
    int CurrentSource = 0;
    int CurrentPreset = 0;
    int CurrentRoutingPreset = -1;

//    PreviousCount_LevelMeter = cntMillisecondTimer+200;
//    PreviousCount_SignalDetect = cntMillisecondTimer+200;
//   PreviousCount_PhaseMeter = cntMillisecondTimer+200;

    if ((ModulePreset>-1) && (ModulePreset<8))
    {
      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (AxumData.ConsolePresetData[PresetNr-1].Console[AxumData.ModuleData[cntModule].Console])
        {
          unsigned char CurrentModulePreset = AxumData.ModuleData[cntModule].ModulePreset;

          switch (ModulePreset)
          {
            case 0:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source1A;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset1A;
              AxumData.ModuleData[cntModule].ModulePreset = 0;
            }
            break;
            case 1:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source1B;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset1B;
              AxumData.ModuleData[cntModule].ModulePreset = 0;
            }
            break;
            case 2:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source2A;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset2A;
              AxumData.ModuleData[cntModule].ModulePreset = 1;
            }
            break;
            case 3:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source2B;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset2B;
              AxumData.ModuleData[cntModule].ModulePreset = 1;
            }
            break;
            case 4:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source3A;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset3A;
              AxumData.ModuleData[cntModule].ModulePreset = 2;
            }
            break;
            case 5:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source3B;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset3B;
              AxumData.ModuleData[cntModule].ModulePreset = 2;
            }
            break;
            case 6:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source4A;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset4A;
              AxumData.ModuleData[cntModule].ModulePreset = 3;
            }
            break;
            case 7:
            {
              CurrentSource = AxumData.ModuleData[cntModule].Source4B;
              CurrentPreset = AxumData.ModuleData[cntModule].ProcessingPreset4B;
              AxumData.ModuleData[cntModule].ModulePreset = 3;
            }
            break;
          }
          CurrentRoutingPreset = ModulePreset+1;

          int SourceActive = 0;
          if (AxumData.ModuleData[cntModule].On)
          {
            if (AxumData.ModuleData[cntModule].FaderLevel>-80)
            {
              SourceActive = 1;
            }
          }

          if ((!SourceActive) || (AxumData.ModuleData[cntModule].OverruleActive) || (DisableActiveCheck))
          {
            if (CurrentSource != 0)
            { //if Source 'none', do nothing
              DoAxum_SetNewSource(cntModule, CurrentSource, DisableActiveCheck | AxumData.ModuleData[cntModule].OverruleActive);
            }
            if (CurrentPreset != 0)
            { //if Preset 'none', do nothing
              DoAxum_LoadProcessingPreset(cntModule, CurrentPreset, 0, 0, 0);
            }
            if (CurrentRoutingPreset != 0)
            { //if RoutingPreset 'none', do nothing
              DoAxum_LoadRoutingPreset(cntModule, CurrentRoutingPreset, 0, 0, 0);
            }
          }
          else
          {
            AxumData.ModuleData[cntModule].WaitingSource = CurrentSource;
            AxumData.ModuleData[cntModule].WaitingProcessingPreset = CurrentPreset;
          }

          if (CurrentModulePreset != AxumData.ModuleData[cntModule].ModulePreset)
          {
            unsigned int FunctionNrToSent =(cntModule<<12);
            CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A);
            CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_B);
            CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_PRESET_A_B);
          }
        }
      }
    }

    if ((MixMonitorPresetNr>=0) && (MixMonitorPresetNr<1280))
    {
      DoAxum_LoadBussMasterPreset(MixMonitorPresetNr, AxumData.ConsolePresetData[PresetNr-1].Console, SetAllObjects);
      DoAxum_LoadMonitorBussPreset(MixMonitorPresetNr, AxumData.ConsolePresetData[PresetNr-1].Console, SetAllObjects);
    }

    for (int cntConsole=0; cntConsole<4; cntConsole++)
    {
      if (AxumData.ConsolePresetData[PresetNr-1].Console[cntConsole])
      {
        int OldSelectedConsolePreset = AxumData.ConsoleData[cntConsole].SelectedConsolePreset;
        AxumData.ConsoleData[cntConsole].SelectedConsolePreset = PresetNr;

        if ((OldSelectedConsolePreset != 0) && (OldSelectedConsolePreset != PresetNr))
        {
          unsigned int FunctionNrToSent = 0x04000000;
          CheckObjectsToSent(FunctionNrToSent | (GLOBAL_FUNCTION_CONSOLE_PRESET_1+OldSelectedConsolePreset-1));
        }

        unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
        CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_CONSOLE_PRESET);
      }
    }
    unsigned int FunctionNrToSent = 0x04000000;
    CheckObjectsToSent(FunctionNrToSent | (GLOBAL_FUNCTION_CONSOLE_PRESET_1+PresetNr-1));
  }
}

unsigned int NrOfObjectsAttachedToFunction(unsigned int FunctionNumberToCheck)
{
  unsigned char FunctionType = (FunctionNumberToCheck>>24)&0xFF;
  unsigned int FunctionNumber = (FunctionNumberToCheck>>12)&0xFFF;
  unsigned int Function = FunctionNumberToCheck&0xFFF;
  AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = NULL;
  int NumberOfObjects = 0;

  //Clear function list
  switch (FunctionType)
  {
    case MODULE_FUNCTIONS:
    {   //Module
      WalkAxumFunctionInformationStruct = ModuleFunctions[FunctionNumber][Function];
    }
    break;
    case BUSS_FUNCTIONS:
    {   //Buss
      WalkAxumFunctionInformationStruct = BussFunctions[FunctionNumber][Function];
    }
    break;
    case MONITOR_BUSS_FUNCTIONS:
    {   //Monitor Buss
      WalkAxumFunctionInformationStruct = MonitorBussFunctions[FunctionNumber][Function];
    }
    break;
    case CONSOLE_FUNCTIONS:
    {   //Console
      WalkAxumFunctionInformationStruct = ConsoleFunctions[FunctionNumber][Function];
    }
    break;
    case GLOBAL_FUNCTIONS:
    {   //Global
      WalkAxumFunctionInformationStruct = GlobalFunctions[Function];
    }
    break;
    case SOURCE_FUNCTIONS:
    {   //Source
      WalkAxumFunctionInformationStruct = SourceFunctions[FunctionNumber][Function];
    }
    break;
    case DESTINATION_FUNCTIONS:
    {   //Destination
      WalkAxumFunctionInformationStruct = DestinationFunctions[FunctionNumber][Function];
    }
    break;
  }
  while (WalkAxumFunctionInformationStruct != NULL)
  {
    NumberOfObjects++;
    WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
  }
  return NumberOfObjects;
}

int AdjustModulePreset(int CurrentPreset, int Offset, unsigned char Pool)
{
  char check_for_next_pos;

  int cntPos;
  int StartPos;
  int CurrentPos;
  int PosBefore;
  int PosAfter;

  //Determin the current position
  CurrentPos = -1;
  PosBefore = -1;
  PosAfter = MAX_NR_OF_PRESETS+2;
  for (cntPos=0; cntPos<MAX_NR_OF_PRESETS+2; cntPos++)
  {
    if (presets.pos[cntPos].filled)
    {
      if (presets.pos[cntPos].number == (signed short int)CurrentPreset)
      {
        CurrentPos = cntPos;
      }
      else if (presets.pos[cntPos].number < (signed short int)CurrentPreset)
      {
        if (cntPos>PosBefore)
        {
          PosBefore = cntPos;
        }
      }
      else if (presets.pos[cntPos].number > (signed short int)CurrentPreset)
      {
        if (cntPos<PosAfter)
        {
          PosAfter = cntPos;
        }
      }
    }
  }

  //If current position not found...
  if (CurrentPos == -1)
  {
    if (Offset<0)
    {
      CurrentPos = PosBefore;
    }
    else
    {
      CurrentPos = PosAfter;
    }
  }

  if (CurrentPos != -1)
  {
    while (Offset != 0)
    {
      StartPos = CurrentPos;
      if (Offset>0)
      {
        check_for_next_pos = 1;
        while (check_for_next_pos)
        {
          check_for_next_pos = 0;

          CurrentPos++;
          if (CurrentPos>=MAX_NR_OF_PRESETS+2)
          {
            CurrentPos = 0;
          }

          CurrentPreset = presets.pos[CurrentPos].number;

          //not active, go further.
          if (!presets.pos[CurrentPos].filled)
          {
            check_for_next_pos = 1;
          }

          if (Pool<8)
          {
            if (!presets.pos[CurrentPos].pool[Pool])
            {
              check_for_next_pos = 1;
            }
          }

          if (CurrentPos == StartPos)
          { //Looped through all sources, no step found...
            check_for_next_pos = 0;
          }
        }
        Offset--;
      }
      else
      {
        check_for_next_pos = 1;
        while (check_for_next_pos)
        {
          check_for_next_pos = 0;

          CurrentPos--;
          if (CurrentPos<0)
          {
            CurrentPos = (MAX_NR_OF_PRESETS+2)-1;
          }

          CurrentPreset = presets.pos[CurrentPos].number;

          //not active, go further.
          if (!presets.pos[CurrentPos].filled)
          {
            check_for_next_pos = 1;
          }

          if (Pool<8)
          {
            if (!presets.pos[CurrentPos].pool[Pool])
            {
              check_for_next_pos = 1;
            }
          }

          if (CurrentPos == StartPos)
          { //Looped through all sources, no step found...
            check_for_next_pos = 0;
          }
        }
        Offset++;
      }
    }
  }

  return CurrentPreset;
}

void GetPresetLabel(int PresetNr, char *TextString, int MaxLength)
{
  if (PresetNr == -1)
  {
    strncpy(TextString, "Default", MaxLength);
  }
  else if (PresetNr == 0)
  {
    strncpy(TextString, "None", MaxLength);
  }
  else if (PresetNr<MAX_NR_OF_PRESETS)
  {
    strncpy(TextString, AxumData.PresetData[PresetNr-1].PresetName, MaxLength);
  }
}

void GetConsolePresetLabel(unsigned int ConsolePresetNr, char *TextString, int MaxLength)
{
  if (ConsolePresetNr == 0)
  {
    strncpy(TextString, "None", MaxLength);
  }
  else if (ConsolePresetNr<32)
  {
    strncpy(TextString, AxumData.ConsolePresetData[ConsolePresetNr-1].Label, MaxLength);
  }
}

int GetControlModeFromConsoleFunctionNr(unsigned int CheckFunctionNr)
{
  unsigned int ControlMode = -1;
  unsigned int FunctionType = (CheckFunctionNr>>24)&0xFF;
  unsigned int FunctionNr = CheckFunctionNr&0xFFF;

  if (FunctionType == 3)
  {

    switch (FunctionNr)
    {
      case CONSOLE_FUNCTION_CONTROL_MODE_SOURCE:
      {
        ControlMode = MODULE_CONTROL_MODE_SOURCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_SOURCE_GAIN:
      {
        ControlMode = MODULE_CONTROL_MODE_SOURCE_GAIN;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_GAIN:
      {
        ControlMode = MODULE_CONTROL_MODE_GAIN;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_PHASE:
      {
        ControlMode = MODULE_CONTROL_MODE_PHASE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_LOW_CUT:
      {
        ControlMode = MODULE_CONTROL_MODE_LOW_CUT;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
      {
       ControlMode = MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_TYPE:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_1_TYPE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_TYPE:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_2_TYPE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_3_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_3_FREQUENCY;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_3_BANDWIDTH;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_TYPE:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_3_TYPE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_4_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_4_FREQUENCY;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_4_BANDWIDTH;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_TYPE:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_4_TYPE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_5_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_5_FREQUENCY;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_5_BANDWIDTH;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_TYPE:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_5_TYPE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_6_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_6_FREQUENCY;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_6_BANDWIDTH;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_TYPE:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_BAND_6_TYPE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_AGC:
      {
        ControlMode = MODULE_CONTROL_MODE_AGC;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_MONO:
      {
        ControlMode = MODULE_CONTROL_MODE_MONO;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_PAN:
      {
        ControlMode = MODULE_CONTROL_MODE_PAN;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_MODULE_LEVEL:
      {
        ControlMode = MODULE_CONTROL_MODE_MODULE_LEVEL;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_1_2;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_1_2_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_3_4;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_3_4_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_5_6;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_5_6_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_7_8;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_7_8_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_9_10;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_9_10_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_11_12;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_11_12_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_13_14;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_13_14_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_15_16;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_15_16_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_17_18;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_17_18_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_19_20;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_19_20_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_21_22;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_21_22_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_23_24;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_23_24_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_25_26;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_25_26_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_27_28;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_27_28_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_29_30;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_29_30_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_31_32;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32_BALANCE:
      {
        ControlMode = MODULE_CONTROL_MODE_BUSS_31_32_BALANCE;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EQ_ON_OFF:
      {
        ControlMode = MODULE_CONTROL_MODE_EQ_ON_OFF;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_PHANTOM_ON_OFF:
      {
        ControlMode = MODULE_CONTROL_MODE_PHANTOM_ON_OFF;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_PAD_ON_OFF:
      {
        ControlMode = MODULE_CONTROL_MODE_PAD_ON_OFF;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_AGC_THRESHOLD:
      {
        ControlMode = MODULE_CONTROL_MODE_AGC_THRESHOLD;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_EXP_THRESHOLD:
      {
        ControlMode = MODULE_CONTROL_MODE_EXPANDER_THRESHOLD;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_PROCESSING_PRESET:
      {
        ControlMode = MODULE_CONTROL_MODE_MODULE_PRESET;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_INSERT_ON_OFF:
      {
        ControlMode = MODULE_CONTROL_MODE_INSERT_ON_OFF;
      }
      break;
      case CONSOLE_FUNCTION_CONTROL_MODE_DYNAMICS_ON_OFF:
      {
        ControlMode = MODULE_CONTROL_MODE_DYNAMICS_ON_OFF;
      }
      break;
    }
  }

  return ControlMode;
}

unsigned int GetConsoleFunctionNrFromControlMode(unsigned int ConsoleNr)
{
  unsigned int FunctionNr = 0x03000000 | (ConsoleNr<<12);

  switch (AxumData.ConsoleData[ConsoleNr].ControlMode)
  {
    case MODULE_CONTROL_MODE_NONE:
    {
    }
    break;
    case MODULE_CONTROL_MODE_SOURCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_SOURCE;
    }
    break;
    case MODULE_CONTROL_MODE_SOURCE_GAIN:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_SOURCE_GAIN;
    }
    break;
    case MODULE_CONTROL_MODE_GAIN:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_GAIN;
    }
    break;
    case MODULE_CONTROL_MODE_PHASE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_PHASE;
    }
    break;
    case MODULE_CONTROL_MODE_LOW_CUT:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_LOW_CUT;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_FREQUENCY:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_FREQUENCY;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_BANDWIDTH:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_BANDWIDTH;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_1_TYPE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_1_TYPE;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_2_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_2_FREQUENCY:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_FREQUENCY;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_2_BANDWIDTH:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_BANDWIDTH;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_2_TYPE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_2_TYPE;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_3_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_3_FREQUENCY:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_FREQUENCY;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_3_BANDWIDTH:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_BANDWIDTH;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_3_TYPE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_3_TYPE;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_4_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_4_FREQUENCY:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_FREQUENCY;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_4_BANDWIDTH:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_BANDWIDTH;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_4_TYPE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_4_TYPE;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_5_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_5_FREQUENCY:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_FREQUENCY;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_5_BANDWIDTH:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_BANDWIDTH;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_5_TYPE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_5_TYPE;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_6_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_6_FREQUENCY:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_FREQUENCY;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_6_BANDWIDTH:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_BANDWIDTH;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_BAND_6_TYPE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_BAND_6_TYPE;
    }
    break;
    case MODULE_CONTROL_MODE_AGC:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_AGC;
    }
    break;
    case MODULE_CONTROL_MODE_MONO:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_MONO;
    }
    break;
    case MODULE_CONTROL_MODE_PAN:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_PAN;
    }
    break;
    case MODULE_CONTROL_MODE_MODULE_LEVEL:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_MODULE_LEVEL;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_1_2:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_1_2_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_1_2_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_3_4:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_3_4_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_3_4_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_5_6:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_5_6_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_5_6_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_7_8:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_7_8_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_7_8_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_9_10:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_9_10_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_9_10_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_11_12:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_11_12_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_11_12_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_13_14:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_13_14_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_13_14_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_15_16:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_15_16_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_15_16_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_17_18:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_17_18_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_17_18_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_19_20:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_19_20_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_19_20_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_21_22:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_21_22_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_21_22_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_23_24:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_23_24_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_23_24_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_25_26:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_25_26_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_25_26_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_27_28:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_27_28_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_27_28_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_29_30:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_29_30_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_29_30_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_31_32:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32;
    }
    break;
    case MODULE_CONTROL_MODE_BUSS_31_32_BALANCE:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_BUSS_31_32_BALANCE;
    }
    break;
    case MODULE_CONTROL_MODE_EQ_ON_OFF:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EQ_ON_OFF;
    }
    break;
    case MODULE_CONTROL_MODE_PHANTOM_ON_OFF:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_PHANTOM_ON_OFF;
    }
    break;
    case MODULE_CONTROL_MODE_PAD_ON_OFF:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_PAD_ON_OFF;
    }
    break;
    case MODULE_CONTROL_MODE_AGC_THRESHOLD:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_AGC_THRESHOLD;
    }
    break;
    case MODULE_CONTROL_MODE_EXPANDER_THRESHOLD:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_EXP_THRESHOLD;
    }
    break;
    case MODULE_CONTROL_MODE_MODULE_PRESET:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_PROCESSING_PRESET;
    }
    break;
    case MODULE_CONTROL_MODE_INSERT_ON_OFF:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_INSERT_ON_OFF;
    }
    break;
    case MODULE_CONTROL_MODE_DYNAMICS_ON_OFF:
    {
      FunctionNr |= CONSOLE_FUNCTION_CONTROL_MODE_DYNAMICS_ON_OFF;
    }
    break;
  }

  return FunctionNr;
}

void DoAxum_UpdateModuleControlModeLabel(unsigned char ModuleNr, int ControlMode)
{
  unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);

  if (AxumData.ConsoleData[0].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1_LABEL);
    if(AxumData.ModuleData[ModuleNr].Console == 0)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
    }
  }
  if (AxumData.ConsoleData[1].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2_LABEL);
    if(AxumData.ModuleData[ModuleNr].Console == 1)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
    }
  }
  if (AxumData.ConsoleData[2].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3_LABEL);
    if(AxumData.ModuleData[ModuleNr].Console == 2)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
    }
  }
  if (AxumData.ConsoleData[3].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4_LABEL);
    if(AxumData.ModuleData[ModuleNr].Console == 3)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_LABEL);
    }
  }
}

void DoAxum_UpdateModuleControlMode(unsigned char ModuleNr, int ControlMode)
{
  unsigned int FunctionNrToSent = ((ModuleNr<<12)&0xFFF000);

  if (AxumData.ConsoleData[0].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_1);
    if(AxumData.ModuleData[ModuleNr].Console == 0)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
    }
  }
  if (AxumData.ConsoleData[1].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_2);
    if(AxumData.ModuleData[ModuleNr].Console == 1)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
    }
  }
  if (AxumData.ConsoleData[2].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_3);
    if(AxumData.ModuleData[ModuleNr].Console == 2)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
    }
  }
  if (AxumData.ConsoleData[3].ControlMode == ControlMode)
  {
    CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL_4);
    if(AxumData.ModuleData[ModuleNr].Console == 3)
    {
      CheckObjectsToSent(FunctionNrToSent | MODULE_FUNCTION_CONTROL);
    }
  }
}

void DoAxum_UpdateMasterControlMode(int ControlMode)
{
  for (int cntConsole=0; cntConsole<4; cntConsole++)
  {
    if (AxumData.ConsoleData[cntConsole].MasterControlMode == ControlMode)
    {
      unsigned int FunctionNrToSent = 0x03000000 | (cntConsole<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MASTER_CONTROL);
    }
  }
}

unsigned char ModulePresetActive(int ModuleNr, unsigned char PresetNr)
{
  int ModulePresetSource = 0;
  int ModulePresetPreset = 0;
  unsigned char cntBuss;
  unsigned char SourceEqual = 0;
  unsigned char PresetEqual = 0;
  unsigned char RoutingEqual = 1;
  unsigned char Active = 0;
  int RoutingPreset = 0;
  AXUM_ROUTING_PRESET_DATA_STRUCT *SelectedRoutingPreset = NULL;

  if (PresetNr>0)
  {
    switch (PresetNr-1)
    {
      case 0:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source1A;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset1A;
      }
      break;
      case 1:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source1B;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset1B;
      }
      break;
      case 2:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source2A;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset2A;
      }
      break;
      case 3:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source2B;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset2B;
      }
      break;
      case 4:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source3A;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset3A;
      }
      break;
      case 5:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source3B;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset3B;
      }
      break;
      case 6:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source4A;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset4A;
      }
      break;
      case 7:
      {
        ModulePresetSource = AxumData.ModuleData[ModuleNr].Source4B;
        ModulePresetPreset = AxumData.ModuleData[ModuleNr].ProcessingPreset4B;
      }
      break;
    }

    for (cntBuss=0; cntBuss<16; cntBuss++)
    {
      SelectedRoutingPreset = &AxumData.ModuleData[ModuleNr].RoutingPreset[PresetNr-1][cntBuss];

      if ((SelectedRoutingPreset != NULL) && (SelectedRoutingPreset->Use))
      {
        RoutingPreset = 1;
        if (!((SelectedRoutingPreset->On             == AxumData.ModuleData[ModuleNr].Buss[cntBuss].On) &&
             (SelectedRoutingPreset->Level          == AxumData.ModuleData[ModuleNr].Buss[cntBuss].Level) &&
             (SelectedRoutingPreset->On             == AxumData.ModuleData[ModuleNr].Buss[cntBuss].On) &&
             (SelectedRoutingPreset->Balance        == AxumData.ModuleData[ModuleNr].Buss[cntBuss].Balance) &&
             (SelectedRoutingPreset->PreModuleLevel == AxumData.ModuleData[ModuleNr].Buss[cntBuss].PreModuleLevel)))
        { //if routing not equal
          RoutingEqual = 0;
        }
      }
    }

    if ((ModulePresetSource == 0) || (AxumData.ModuleData[ModuleNr].SelectedSource == ModulePresetSource))
    {
      SourceEqual = 1;
    }
    else if ((ModulePresetSource == -1) && (AxumData.ModuleData[ModuleNr].SelectedSource == 0))
    {
      SourceEqual = 1;
    }
    if ((ModulePresetPreset == 0) || (AxumData.ModuleData[ModuleNr].SelectedProcessingPreset == ModulePresetPreset))
    {
      PresetEqual = 1;
    }
    if ((SourceEqual) && (PresetEqual) && (RoutingEqual))
    {
      Active = 1;
    }
    if ((ModulePresetSource == 0) && (ModulePresetPreset == 0) && (RoutingPreset == 0))
    {
      Active = 0;
    }
  }

  return Active;
}

unsigned char GetPresetNrFromFunctionNr(unsigned int FunctionNr)
{
  int PresetNr = 0;
  switch (FunctionNr)
  {
    case MODULE_FUNCTION_PRESET_1A:
    {
      PresetNr = 1;
    }
    break;
    case MODULE_FUNCTION_PRESET_1B:
    {
      PresetNr = 2;
    }
    break;
    case MODULE_FUNCTION_PRESET_2A:
    {
      PresetNr = 3;
    }
    break;
    case MODULE_FUNCTION_PRESET_2B:
    {
      PresetNr = 4;
    }
    break;
    case MODULE_FUNCTION_PRESET_3A:
    {
      PresetNr = 5;
    }
    break;
    case MODULE_FUNCTION_PRESET_3B:
    {
      PresetNr = 6;
    }
    break;
    case MODULE_FUNCTION_PRESET_4A:
    {
      PresetNr = 7;
    }
    break;
    case MODULE_FUNCTION_PRESET_4B:
    {
      PresetNr = 8;
    }
    break;
  }

  return PresetNr;
}

unsigned int GetModuleFunctionNrFromPresetNr(unsigned char PresetNr)
{
  int FunctionNr = 0xFFFFFFFF;
  switch (PresetNr)
  {
    case 1:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_1A;
    }
    break;
    case 2:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_1B;
    }
    break;
    case 3:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_2A;
    }
    break;
    case 4:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_2B;
    }
    break;
    case 5:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_3A;
    }
    break;
    case 6:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_3B;
    }
    break;
    case 7:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_4A;
    }
    break;
    case 8:
    {
      FunctionNr = MODULE_FUNCTION_PRESET_4B;
    }
    break;
  }

  return FunctionNr;
}

void debug_mambanet_data(unsigned int addr, unsigned int object, unsigned char type, union mbn_data data)
{
  char TypeString[64] = "";
  char DataString[64] = "";

  switch (type)
  {
    case MBN_DATATYPE_NODATA:
    {
      sprintf(TypeString,"NO DATA");
    }
    break;
    case MBN_DATATYPE_UINT:
    {
      sprintf(TypeString,"UNSIGNED INTEGER");
      sprintf(DataString,"%ld", data.UInt);
    }
    break;
    case MBN_DATATYPE_SINT:
    {
      sprintf(TypeString,"SIGNED INTEGER");
      sprintf(DataString,"%ld", data.SInt);
    }
    break;
    case MBN_DATATYPE_STATE:
    {
      sprintf(TypeString,"STATE");
      sprintf(DataString,"%ld", data.State);
    }
    break;
    case MBN_DATATYPE_OCTETS:
    {
      sprintf(TypeString,"OCTETS");
      sprintf(DataString,"%s", data.Octets);
    }
    break;
    case MBN_DATATYPE_FLOAT:
    {
      sprintf(TypeString,"FLOAT");
      sprintf(DataString,"%f", data.Float);
    }
    break;
    case MBN_DATATYPE_BITS:
    {
      sprintf(TypeString,"BITS");
    }
    break;
    case MBN_DATATYPE_OBJINFO:
    {
      sprintf(TypeString,"OBJINFO");
    }
    break;
    case MBN_DATATYPE_ERROR:
    {
    }
    break;
  }
  log_write("[mSensorDataChanged] addr:0x%08X, obj: %d, type:%s, data:%s", addr, object, TypeString, DataString);
}

void DoAxum_SetCRMBussOnOff(int MonitorBussNr, int BussNr, unsigned char NewState, int PreventDoingInterlock)
{
  unsigned char CurrentMonitorBussState[24];
  unsigned char BussActive[16];
  unsigned char MonitorBussAutoSwitchingActive[16];

  //store current Monitor on/off states
  for (int cntBuss=0; cntBuss<24; cntBuss++)
  {
    if (cntBuss<16)
    {
      CurrentMonitorBussState[cntBuss] = AxumData.Monitor[MonitorBussNr].Buss[cntBuss];
    }
    else
    {
      CurrentMonitorBussState[cntBuss] = AxumData.Monitor[MonitorBussNr].Ext[cntBuss-16];
    }
  }

  //store current active auto switching busses
  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    BussActive[cntBuss] = 0;
    for (int cntModule=0; cntModule<128; cntModule++)
    {
      if (AxumData.ModuleData[cntModule].Buss[cntBuss].On)
      {
        BussActive[cntBuss] = 1;
      }
    }
  }
  //Store routing set by Automatic switching
  for (int cntBuss=0; cntBuss<16; cntBuss++)
  {
    MonitorBussAutoSwitchingActive[cntBuss] = 0;
    if ((AxumData.Monitor[MonitorBussNr].AutoSwitchingBuss[cntBuss]) &&
        (AxumData.Monitor[MonitorBussNr].Buss[cntBuss]) &&
        (BussActive[cntBuss]))
    {
      MonitorBussAutoSwitchingActive[cntBuss] = 1;
    }
  }

  if (BussNr<16)
  {
    AxumData.Monitor[MonitorBussNr].Buss[BussNr] = NewState;
  }
  else
  {
    AxumData.Monitor[MonitorBussNr].Ext[BussNr-16] = NewState;
  }

  //Check interlock and turn off all others.
  if ((AxumData.Monitor[MonitorBussNr].Interlock) && (!PreventDoingInterlock))
  {
    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      if (cntBuss != BussNr)
      {
        if (AxumData.Monitor[MonitorBussNr].Buss[cntBuss])
        {
          if (!MonitorBussAutoSwitchingActive[cntBuss])
          {
            AxumData.Monitor[MonitorBussNr].Buss[cntBuss] = 0;
          }
        }
      }
    }
    for (int cntExt=0; cntExt<8; cntExt++)
    {
      if ((cntExt+16) != BussNr)
      {
        if ((AxumData.Monitor[MonitorBussNr].Ext[cntExt]) &&
            (!AxumData.ExternSource[MonitorBussNr/4].InterlockSafe[cntExt]))
        {
          AxumData.Monitor[MonitorBussNr].Ext[cntExt] = 0;
        }
      }
    }
  }

  //if no active routing, use default routing
  unsigned int MonitorBussActive = 0;
  int cntBuss;
  for (cntBuss=0; cntBuss<16; cntBuss++)
  {
    if (AxumData.Monitor[MonitorBussNr].Buss[cntBuss])
    {
      MonitorBussActive = 1;
    }
  }
  int cntExt;
  for (cntExt=0; cntExt<8; cntExt++)
  {
    if (AxumData.Monitor[MonitorBussNr].Ext[cntExt])
    {
      MonitorBussActive = 1;
    }
  }

  if (!MonitorBussActive)
  {
    int DefaultSelection = AxumData.Monitor[MonitorBussNr].DefaultSelection;
    if (DefaultSelection<16)
    {
      int BussNr = DefaultSelection;
      AxumData.Monitor[MonitorBussNr].Buss[BussNr] = 1;
    }
    else if (DefaultSelection<24)
    {
      int ExtNr = DefaultSelection-16;
      AxumData.Monitor[MonitorBussNr].Ext[ExtNr] = 1;
    }
  }

  SetAxum_MonitorBuss(MonitorBussNr);

  for (int cntBuss=0; cntBuss<24; cntBuss++)
  {
    unsigned int FunctionNrToSent = 0x02000000 | (MonitorBussNr<<12);
    if (cntBuss<16)
    {
      if (CurrentMonitorBussState[cntBuss] != AxumData.Monitor[MonitorBussNr].Buss[cntBuss])
      {
        CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_ON+cntBuss));
        CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_OFF+cntBuss));
        CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_BUSS_1_2_ON_OFF+cntBuss));
      }
    }
    else
    {
      int ExtNr = cntBuss-16;
      if (CurrentMonitorBussState[cntBuss] != AxumData.Monitor[MonitorBussNr].Ext[ExtNr])
      {
        CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_ON+ExtNr));
        CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_OFF+ExtNr));
        CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_EXT_1_ON_OFF+ExtNr));
      }
    }
  }
}


void DoAxum_StartStopTrigger(unsigned int ModuleNr, float CurrentLevel, float NewLevel, unsigned char CurrentOn, unsigned char NewOn)
{
  unsigned char UpdateStartStopObjects = 0;
  unsigned char OtherFaderActive = 0;
  unsigned char OtherFaderAndOnActive = 0;
  unsigned char OtherOnActive = 0;

  if ((AxumData.ModuleData[ModuleNr].SelectedSource >= matrix_sources.src_offset.min.source) && (AxumData.ModuleData[ModuleNr].SelectedSource <= matrix_sources.src_offset.max.source))
  {
    int SourceNr = AxumData.ModuleData[ModuleNr].SelectedSource - matrix_sources.src_offset.min.source;

    //Check the source on other modules
    for (unsigned int cntModule=0; cntModule<128; cntModule++)
    {
      if (cntModule != ModuleNr)
      {
        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
        {
          if (AxumData.ModuleData[cntModule].FaderLevel>-80)
          {
            OtherFaderActive = 1;
            if (AxumData.ModuleData[cntModule].On)
            {
              OtherFaderAndOnActive =1;
            }
          }
          if (AxumData.ModuleData[cntModule].On)
          {
            OtherOnActive = 1;
          }
        }
      }
    }

    //start
    if (!AxumData.SourceData[SourceNr].Start)
    {
      switch (AxumData.SourceData[SourceNr].StartTrigger)
      {
        case 1:
        { //module fader on
          if ((CurrentLevel<=-80) && (NewLevel>-80))
          {
            AxumData.SourceData[SourceNr].Start = 1;
            UpdateStartStopObjects = 1;
          }
        }
        break;
        case 2:
        { //module on
          if ((CurrentOn != NewOn) && (NewOn == 1))
          {
            AxumData.SourceData[SourceNr].Start = 1;
            UpdateStartStopObjects = 1;
          }
        }
        break;
        case 3:
        { //module fader and on active
          if ((((CurrentLevel<=-80) && (NewLevel>-80)) && (NewOn == 1)) ||
              (((CurrentOn != NewOn) && (NewOn == 1)) && (NewLevel>-80)))
          {
            AxumData.SourceData[SourceNr].Start = 1;
            UpdateStartStopObjects = 1;
          }
        }
        break;
      }
    }

    //stop
    if (AxumData.SourceData[SourceNr].Start)
    {
      switch (AxumData.SourceData[SourceNr].StopTrigger)
      {
        case 1:
        { //module fader off
          if ((CurrentLevel>-80) && (NewLevel<=-80))
          {
            if (!OtherFaderActive)
            {
              AxumData.SourceData[SourceNr].Start = 0;
              UpdateStartStopObjects = 1;
            }
          }
        }
        break;
        case 2:
        { //module on
          if ((CurrentOn != NewOn) && (NewOn == 0))
          {
            if (!OtherOnActive)
            {
              AxumData.SourceData[SourceNr].Start = 0;
              UpdateStartStopObjects = 1;
            }
          }
        }
        break;
        case 3:
        { //module fader and on active
          if (((CurrentLevel>-80) && (NewLevel<=-80)) ||
              ((CurrentOn != NewOn) && (NewOn == 0)))
          {
            if (!OtherFaderAndOnActive)
            {
              AxumData.SourceData[SourceNr].Start = 0;
              UpdateStartStopObjects = 1;
            }
          }
        }
        break;
      }
    }

    if (UpdateStartStopObjects)
    { //Only if pushed or changed
      unsigned int DisplayFunctionNr = 0x05000000 | (SourceNr<<12);
      CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_START);
      CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_STOP);
      CheckObjectsToSent(DisplayFunctionNr | SOURCE_FUNCTION_START_STOP);

      for (int cntModule=0; cntModule<128; cntModule++)
      {
        if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
        {
          DisplayFunctionNr = (cntModule<<12);
          CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_START);
          CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_STOP);
          CheckObjectsToSent(DisplayFunctionNr | MODULE_FUNCTION_SOURCE_START_STOP);
        }
      }
    }
  }
}

void SetSelectedModule(unsigned char SelectNr, unsigned int NewModuleNr)
{
  if ((SelectNr<4) && (NewModuleNr<128))
  {
    unsigned int OldSelectedModuleNr = AxumData.ConsoleData[SelectNr].SelectedModule;

    AxumData.ConsoleData[SelectNr].SelectedModule = NewModuleNr;
    AxumData.ConsoleData[SelectNr].SelectedModuleTimeout = SELECT_TIMEOUT;
    unsigned int FunctionNrToSent = 0x03000000 | (SelectNr<<12);
    CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MODULE_SELECT_ACTIVE);

    if (OldSelectedModuleNr != AxumData.ConsoleData[SelectNr].SelectedModule)
    {
      unsigned int FunctionNrToSent = ((OldSelectedModuleNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_SELECT_1+SelectNr));

      for (int cntFunction=0; cntFunction<NUMBER_OF_MODULE_FUNCTIONS; cntFunction++)
      {
        FunctionNrToSent = ((NUMBER_OF_MODULES+SelectNr)<<12);
        CheckObjectsToSent(FunctionNrToSent | cntFunction);
      }

      FunctionNrToSent = ((NewModuleNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_SELECT_1+SelectNr));

      FunctionNrToSent = 0x03000000 | (SelectNr<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MODULE_SELECT);
    }
  }
}

void SetSelectedBuss(unsigned char SelectNr, unsigned int NewBussNr)
{
  if ((SelectNr<4) && (NewBussNr<16))
  {
    unsigned int OldSelectedBussNr = AxumData.ConsoleData[SelectNr].SelectedBuss;

    AxumData.ConsoleData[SelectNr].SelectedBuss = NewBussNr;
    AxumData.ConsoleData[SelectNr].SelectedBussTimeout = SELECT_TIMEOUT;
    unsigned int FunctionNrToSent = 0x03000000 | (SelectNr<<12);
    CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_BUSS_SELECT_ACTIVE);

    if (OldSelectedBussNr != AxumData.ConsoleData[SelectNr].SelectedBuss)
    {
      unsigned int FunctionNrToSent = 0x01000000 | ((OldSelectedBussNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (BUSS_FUNCTION_SELECT_1+SelectNr));

      for (int cntFunction=0; cntFunction<NUMBER_OF_BUSS_FUNCTIONS; cntFunction++)
      {
        FunctionNrToSent = 0x01000000 | ((NUMBER_OF_BUSSES+SelectNr)<<12);
        CheckObjectsToSent(FunctionNrToSent | cntFunction);
      }

      FunctionNrToSent = 0x01000000 | ((NewBussNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (BUSS_FUNCTION_SELECT_1+SelectNr));

      FunctionNrToSent = 0x03000000 | (SelectNr<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_BUSS_SELECT);
    }
  }
}

void SetSelectedMonitorBuss(unsigned char SelectNr, unsigned int NewMonitorBussNr)
{
  if ((SelectNr<4) && (NewMonitorBussNr<16))
  {
    unsigned int OldSelectedMonitorBussNr = AxumData.ConsoleData[SelectNr].SelectedMonitorBuss;

    AxumData.ConsoleData[SelectNr].SelectedMonitorBuss = NewMonitorBussNr;
    AxumData.ConsoleData[SelectNr].SelectedMonitorBussTimeout = SELECT_TIMEOUT;
    unsigned int FunctionNrToSent = 0x03000000 | (SelectNr<<12);
    CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MONITOR_BUSS_SELECT_ACTIVE);

    if (OldSelectedMonitorBussNr != AxumData.ConsoleData[SelectNr].SelectedMonitorBuss)
    {
      unsigned int FunctionNrToSent = 0x02000000 | ((OldSelectedMonitorBussNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_SELECT_1+SelectNr));

      for (int cntFunction=0; cntFunction<NUMBER_OF_MONITOR_BUSS_FUNCTIONS; cntFunction++)
      {
        FunctionNrToSent = 0x02000000 | ((NUMBER_OF_MONITOR_BUSSES+SelectNr)<<12);
        CheckObjectsToSent(FunctionNrToSent | cntFunction);
      }

      FunctionNrToSent = 0x02000000 | ((NewMonitorBussNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (MONITOR_BUSS_FUNCTION_SELECT_1+SelectNr));

      FunctionNrToSent = 0x03000000 | (SelectNr<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_MONITOR_BUSS_SELECT);
    }
  }
}

void SetSelectedSource(unsigned char SelectNr, unsigned int NewSourceNr)
{
  if ((SelectNr<4) && (NewSourceNr<1279))
  {
    unsigned int OldSelectedSourceNr = AxumData.ConsoleData[SelectNr].SelectedSource;

    AxumData.ConsoleData[SelectNr].SelectedSource = NewSourceNr;
    AxumData.ConsoleData[SelectNr].SelectedSourceTimeout = SELECT_TIMEOUT;
    unsigned int FunctionNrToSent = 0x03000000 | (SelectNr<<12);
    CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_SOURCE_SELECT_ACTIVE);

    if (OldSelectedSourceNr != AxumData.ConsoleData[SelectNr].SelectedSource)
    {
      unsigned int FunctionNrToSent = 0x05000000 | ((OldSelectedSourceNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_SELECT_1+SelectNr));

      for (int cntFunction=0; cntFunction<NUMBER_OF_SOURCE_FUNCTIONS; cntFunction++)
      {
        FunctionNrToSent = 0x05000000 | ((NUMBER_OF_SOURCES+SelectNr)<<12);
        CheckObjectsToSent(FunctionNrToSent | cntFunction);
      }

      FunctionNrToSent = 0x05000000 | ((NewSourceNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (SOURCE_FUNCTION_SELECT_1+SelectNr));

      FunctionNrToSent = 0x03000000 | (SelectNr<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_SOURCE_SELECT);
    }
  }
}

void SetSelectedDestination(unsigned char SelectNr, unsigned int NewDestinationNr)
{
  if ((SelectNr<4) && (NewDestinationNr<1279))
  {
    unsigned int OldSelectedDestinationNr = AxumData.ConsoleData[SelectNr].SelectedDestination;

    AxumData.ConsoleData[SelectNr].SelectedDestination = NewDestinationNr;
    AxumData.ConsoleData[SelectNr].SelectedDestinationTimeout = SELECT_TIMEOUT;
    unsigned int FunctionNrToSent = 0x03000000 | (SelectNr<<12);
    CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_DESTINATION_SELECT_ACTIVE);

    if (OldSelectedDestinationNr != AxumData.ConsoleData[SelectNr].SelectedDestination)
    {
      unsigned int FunctionNrToSent = 0x06000000 | ((OldSelectedDestinationNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (DESTINATION_FUNCTION_SELECT_1+SelectNr));

      for (int cntFunction=0; cntFunction<NUMBER_OF_DESTINATION_FUNCTIONS; cntFunction++)
      {
        FunctionNrToSent = 0x06000000 | ((NUMBER_OF_DESTINATIONS+SelectNr)<<12);
        CheckObjectsToSent(FunctionNrToSent | cntFunction);
      }

      FunctionNrToSent = 0x06000000 | ((NewDestinationNr)<<12);
      CheckObjectsToSent(FunctionNrToSent | (DESTINATION_FUNCTION_SELECT_1+SelectNr));

      FunctionNrToSent = 0x03000000 | (SelectNr<<12);
      CheckObjectsToSent(FunctionNrToSent | CONSOLE_FUNCTION_DESTINATION_SELECT);
    }
  }
}

void DoAxum_TalkbackToRelatedDestination(unsigned char ModuleNr, unsigned char TalkbackNr, unsigned char NewState, unsigned char Dimming)
{
  int cntDestination=0;
  int SelectedSource = AxumData.ModuleData[ModuleNr].SelectedSource;
  bool MixMinusInUse = false;
  bool UpdateDestinations = false;
  int SourceNr = -1;
  if ((SelectedSource >= matrix_sources.src_offset.min.source) && (SelectedSource <= matrix_sources.src_offset.max.source))
  {
    SourceNr = SelectedSource-matrix_sources.src_offset.min.source;
  }

  while (cntDestination<1280)
  {
    if ((SelectedSource != 0) && (AxumData.DestinationData[cntDestination].MixMinusSource == SelectedSource))
    {
      MixMinusInUse = true;
    }
    cntDestination++;
  }

  if (MixMinusInUse)
  {
    AxumData.ModuleData[ModuleNr].TalkbackToRelatedDestination[TalkbackNr] = NewState;
    UpdateDestinations = true;
  }
  else if (AxumData.ModuleData[ModuleNr].TalkbackToRelatedDestination[TalkbackNr])
  {
    AxumData.ModuleData[ModuleNr].TalkbackToRelatedDestination[TalkbackNr] = false;
    UpdateDestinations = true;
  }
  else
  { //Find related destination
    if (SourceNr != -1)
    {
      AxumData.ModuleData[ModuleNr].TalkbackToRelatedDestination[TalkbackNr] = NewState;
      UpdateDestinations = true;
    }
  }

  if (UpdateDestinations)
  {
    int CurrentSource = AxumData.ModuleData[ModuleNr].SelectedSource;

    cntDestination=0;
    while (cntDestination<1280)
    {
      if (((CurrentSource != 0) && (AxumData.DestinationData[cntDestination].MixMinusSource == CurrentSource)) ||
          (cntDestination == AxumData.SourceData[SourceNr].RelatedDest))
      {
        AxumData.DestinationData[cntDestination].Talkback[TalkbackNr] = AxumData.ModuleData[ModuleNr].TalkbackToRelatedDestination[TalkbackNr];

        int TalkbackActive = 0;
        for (int cntTalkback=0; cntTalkback<16; cntTalkback++)
        {
          TalkbackActive |= AxumData.DestinationData[cntDestination].Talkback[cntTalkback];
        }

        unsigned int FunctionNrToSent = 0x06000000 | (cntDestination<<12);
        CheckObjectsToSent(FunctionNrToSent | (DESTINATION_FUNCTION_TALKBACK_1+(TalkbackNr*(DESTINATION_FUNCTION_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1))));
        CheckObjectsToSent(FunctionNrToSent | (DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1 + ((DESTINATION_FUNCTION_TALKBACK_2_AND_MONITOR_TALKBACK_2-DESTINATION_FUNCTION_TALKBACK_1_AND_MONITOR_TALKBACK_1)*TalkbackNr)));
        if (Dimming)
        {
          AxumData.DestinationData[cntDestination].Dim = TalkbackActive;
          CheckObjectsToSent(FunctionNrToSent | DESTINATION_FUNCTION_DIM);
          CheckObjectsToSent(FunctionNrToSent | DESTINATION_FUNCTION_DIM_AND_MONITOR_DIM);
        }
      }
      cntDestination++;
    }
    unsigned int FunctionNrToSent = ((ModuleNr)<<12);
    CheckObjectsToSent(FunctionNrToSent | (MODULE_FUNCTION_TALKBACK_1_TO_REL_DEST+TalkbackNr));
  }
}

void DoAxum_SetCough(int SourceNr, unsigned char NewState)
{
  for (int cntModule=0; cntModule<128; cntModule++)
  {
    if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
    {
      if (AxumData.ModuleData[cntModule].Cough != NewState)
      {
        AxumData.ModuleData[cntModule].Cough = NewState;

        SetAxum_BussLevels(cntModule);

        unsigned int FunctionNrToSend = ((cntModule)<<12);
        CheckObjectsToSent(FunctionNrToSend+MODULE_FUNCTION_COUGH_ON_OFF);

        FunctionNrToSend = 0x05000000 | (SourceNr<<12);
        CheckObjectsToSent(FunctionNrToSend+SOURCE_FUNCTION_MODULE_COUGH_ON_OFF);
      }
    }
  }
}

void DoAxum_SetComm(int SourceNr, unsigned char CommNr, unsigned char NewState)
{
  if (AxumData.SourceData[SourceNr].CoughComm[CommNr] != NewState)
  {
    AxumData.SourceData[SourceNr].CoughComm[CommNr] = NewState;

    for (int cntBuss=0; cntBuss<16; cntBuss++)
    {
      if (AxumData.BussMasterData[cntBuss].Exclusive == 2+CommNr)
      {
        for (int cntModule=0; cntModule<128; cntModule++)
        {
          if (AxumData.ModuleData[cntModule].Console == AxumData.BussMasterData[cntBuss].Console)
          {
            if (AxumData.ModuleData[cntModule].SelectedSource == (SourceNr+matrix_sources.src_offset.min.source))
            {
              DoAxum_SetBussOnOff(cntModule, cntBuss, NewState, 0);
            }
          }
        }
      }
    }

    unsigned int FunctionNrToSend = 0x05000000 | (SourceNr<<12);
    CheckObjectsToSent(FunctionNrToSend | (SOURCE_FUNCTION_COUGH_COMM_1+CommNr));
  }
}

void CheckObjectRange(unsigned int SensorReceiveFunctionNumber, float *min, float *max, float *def, unsigned int MambaNetAddress)
{
  unsigned int FunctionType = (SensorReceiveFunctionNumber>>24)&0xFF;
  unsigned int FunctionNumber = (SensorReceiveFunctionNumber>>12)&0xFFF;
  unsigned int Function = SensorReceiveFunctionNumber&0xFFF;
  AXUM_FUNCTION_INFORMATION_STRUCT *WalkAxumFunctionInformationStruct = NULL;

  *min = FLT_MAX;
  *max = FLT_MIN;
  *def = 0;

  switch (FunctionType)
  {
    case MODULE_FUNCTIONS:
    {   //Module
      if (FunctionNumber<NUMBER_OF_MODULES)
      {
        WalkAxumFunctionInformationStruct = ModuleFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_MODULES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_MODULES].SelectedModule;
      }
    }
    break;
    case BUSS_FUNCTIONS:
    {   //Buss
      if (FunctionNumber<NUMBER_OF_BUSSES)
      {
        WalkAxumFunctionInformationStruct = BussFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_BUSSES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_BUSSES].SelectedBuss;
      }
    }
    break;
    case MONITOR_BUSS_FUNCTIONS:
    {   //Monitor Buss
      if (FunctionNumber<NUMBER_OF_MONITOR_BUSSES)
      {
        WalkAxumFunctionInformationStruct = MonitorBussFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_MONITOR_BUSSES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_MONITOR_BUSSES].SelectedMonitorBuss;
      }
    }
    break;
    case CONSOLE_FUNCTIONS:
    {
      if (FunctionNumber<NUMBER_OF_CONSOLES)
      {
        WalkAxumFunctionInformationStruct = ConsoleFunctions[FunctionNumber][Function];
      }
    }
    break;
    case GLOBAL_FUNCTIONS:
    {   //Global
      WalkAxumFunctionInformationStruct = GlobalFunctions[Function];
    }
    break;
    case SOURCE_FUNCTIONS:
    {   //Source
      if (FunctionNumber<NUMBER_OF_SOURCES)
      {
        WalkAxumFunctionInformationStruct = SourceFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_SOURCES+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_SOURCES].SelectedSource;
      }
    }
    break;
    case DESTINATION_FUNCTIONS:
    {   //Destination
      if (FunctionNumber<NUMBER_OF_DESTINATIONS)
      {
        WalkAxumFunctionInformationStruct = DestinationFunctions[FunctionNumber][Function];
      }
      else if (FunctionNumber<(NUMBER_OF_DESTINATIONS+4))
      {
        FunctionNumber = AxumData.ConsoleData[FunctionNumber-NUMBER_OF_DESTINATIONS].SelectedDestination;
      }
    }
    break;
  }
  while (WalkAxumFunctionInformationStruct != NULL)
  {
    if ((MambaNetAddress == 0x00000000) || (MambaNetAddress == WalkAxumFunctionInformationStruct->MambaNetAddress))
    {
      if (WalkAxumFunctionInformationStruct->ActuatorDataMaximal>*max)
      {
        *max = WalkAxumFunctionInformationStruct->ActuatorDataMaximal;
      }
      if (WalkAxumFunctionInformationStruct->ActuatorDataMinimal<*min)
      {
        *min = WalkAxumFunctionInformationStruct->ActuatorDataMinimal;
      }
      *def = WalkAxumFunctionInformationStruct->ActuatorDataDefault;
    }
    WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
  }

  //And check the select functions as well
  for (int cntConsole=0; cntConsole<4; cntConsole++)
  {
    switch (FunctionType)
    {
      case MODULE_FUNCTIONS:
      {   //Module
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedModule)
        {
          WalkAxumFunctionInformationStruct = ModuleFunctions[NUMBER_OF_MODULES+cntConsole][Function];
        }
      }
      break;
      case BUSS_FUNCTIONS:
      {   //Buss
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedBuss)
        {
          WalkAxumFunctionInformationStruct = BussFunctions[NUMBER_OF_BUSSES+cntConsole][Function];
        }
      }
      break;
      case MONITOR_BUSS_FUNCTIONS:
      {   //Monitor Buss
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedMonitorBuss)
        {
          WalkAxumFunctionInformationStruct = MonitorBussFunctions[NUMBER_OF_MONITOR_BUSSES+cntConsole][Function];
        }
      }
      break;
      case SOURCE_FUNCTIONS:
      {   //Source
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedSource)
        {
          WalkAxumFunctionInformationStruct = SourceFunctions[NUMBER_OF_SOURCES+cntConsole][Function];
        }
      }
      break;
      case DESTINATION_FUNCTIONS:
      {   //Destination
        if (FunctionNumber == AxumData.ConsoleData[cntConsole].SelectedDestination)
        {
          WalkAxumFunctionInformationStruct = DestinationFunctions[NUMBER_OF_DESTINATIONS+cntConsole][Function];
        }
      }
      break;
    }
    while (WalkAxumFunctionInformationStruct != NULL)
    {
      if ((MambaNetAddress == 0x00000000) || (MambaNetAddress == WalkAxumFunctionInformationStruct->MambaNetAddress))
      {
        if (WalkAxumFunctionInformationStruct->ActuatorDataMaximal>*max)
        {
          *max = WalkAxumFunctionInformationStruct->ActuatorDataMaximal;
        }
        if (WalkAxumFunctionInformationStruct->ActuatorDataMinimal<*min)
        {
          *min = WalkAxumFunctionInformationStruct->ActuatorDataMinimal;
        }
        *def = WalkAxumFunctionInformationStruct->ActuatorDataDefault;
      }
      WalkAxumFunctionInformationStruct = (AXUM_FUNCTION_INFORMATION_STRUCT *)WalkAxumFunctionInformationStruct->Next;
    }
  }
}

