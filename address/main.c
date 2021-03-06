
#include "common.h"
#include "main.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#include <mbn.h>
#include <unistd.h>

#define DEFAULT_UNIX_HWPARENT_PATH  "/tmp/hwparent.socket"
#define DEFAULT_UNIX_MAMBANET_PATH  "/tmp/axum-gateway.socket"
#define DEFAULT_ETH_DEV             "eth0"
#define DEFAULT_DB_STR              "dbname='axum' user='axum'"
#define DEFAULT_LOG_FILE            "/var/log/axum-address.log"

#ifndef UNIX_PATH_MAX
# define UNIX_PATH_MAX 108
#endif


/* node info */
struct mbn_node_info this_node = {
  0, MBN_ADDR_SERVICES_SERVER,
  "MambaNet Address Server",
  "Axum Address Server",
  0x0001, 0x000F, 0x0001,
  0, 0, /* HW */
  0, 1, /* FW */
  0, 0, /* FPGA */
  0, 0, /* objects, engine */
  {0,0,0}, /* parent */
  0 /* service */
};


/* global variables */
struct mbn_handler *mbn;

void set_address(unsigned long addr, unsigned short man, unsigned short prod, unsigned short id, unsigned long engine, unsigned char serv) {
  struct mbn_message msg;

  if(addr == 0)
    serv &= ~MBN_ADDR_SERVICES_VALID;
  else
    serv |= MBN_ADDR_SERVICES_VALID;
  msg.MessageType = MBN_MSGTYPE_ADDRESS;
  msg.AcknowledgeReply = 0;
  msg.AddressTo = MBN_BROADCAST_ADDRESS;
  msg.Message.Address.Action = MBN_ADDR_ACTION_RESPONSE;
  msg.Message.Address.MambaNetAddr = addr;
  msg.Message.Address.ManufacturerID = man;
  msg.Message.Address.ProductID = prod;
  msg.Message.Address.UniqueIDPerProduct = id;
  msg.Message.Address.Services = serv;
  msg.Message.Address.EngineAddr = engine;
  mbnSendMessage(mbn, &msg, MBN_SEND_IGNOREVALID);
}


void node_online(struct db_node *node) {
  struct db_node addr, id;
  int addr_f, id_f;
  union mbn_data dat;

  if ((node->ManufacturerID != 0) && (node->ProductID != 0) && (node->UniqueIDPerProduct != 0))
  {
    /* check DB for MambaNet Address and UniqueID */
    memset((void *)&addr, 0, sizeof(struct db_node));
    memset((void *)&id, 0, sizeof(struct db_node));
    addr_f = db_getnode(&addr, node->MambaNetAddr);
    id_f = db_nodebyid(&id, node->ManufacturerID, node->ProductID, node->UniqueIDPerProduct);

    /* Reset node address when the previous search didn't return the exact same node */
    if((addr_f && !id_f) || (!addr_f && id_f)
        || (addr_f && id_f && memcmp((void *)&addr, (void *)&id, sizeof(struct db_node)) != 0)) {
      log_write("Address mismatch for %04X:%04X:%04X (%08lX), resetting valid bit",
        node->ManufacturerID, node->ProductID, node->UniqueIDPerProduct, node->MambaNetAddr);
      set_address_struct(0, *node);
      return;
    }

    /* not in the DB at all? Add it. */
    if(!addr_f) {
      log_write("New validated node found on the network but not in DB: %08lX (%04X:%04X:%04X)",
        node->MambaNetAddr, node->ManufacturerID, node->ProductID, node->UniqueIDPerProduct);
      node->flags |= DB_FLAGS_REFRESH;
      node->FirstSeen = node->LastSeen = time(NULL);
      db_setnode(0, node);
    }
    /* we don't have its name? get it! */
    if(!addr_f || addr.flags & DB_FLAGS_REFRESH) {
      mbnGetActuatorData(mbn, node->MambaNetAddr, MBN_NODEOBJ_NAME, 1);
      mbnGetSensorData(mbn, node->MambaNetAddr, MBN_NODEOBJ_HWPARENT, 1);
    }
    /* not active or something changed? update! */
    if(addr_f && (!addr.Active || addr.Services != node->Services || addr.EngineAddr != node->EngineAddr)) {
      addr.Active = 1;
      addr.Services = node->Services;
      addr.EngineAddr = node->EngineAddr;
      db_setnode(node->MambaNetAddr, &addr);
    }
    /* name was changed in the DB? send it to the node */
    if(addr_f && addr.flags & DB_FLAGS_SETNAME) {
      addr.flags &= ~DB_FLAGS_SETNAME;
      dat.Octets = (unsigned char *)addr.Name;
      mbnSetActuatorData(mbn, node->MambaNetAddr, MBN_NODEOBJ_NAME, MBN_DATATYPE_OCTETS, 32, dat, 1);
      db_setnode(node->MambaNetAddr, &addr);
    }
    /* always get firmware major revision to detect changes in the object list */
    mbnGetSensorData(mbn, node->MambaNetAddr, MBN_NODEOBJ_FWMAJOR, 1);
  }
  else
  {
    log_write("Invalid MediaAccessID for %04X:%04X:%04X (%08lX), doing nothing",
        node->ManufacturerID, node->ProductID, node->UniqueIDPerProduct, node->MambaNetAddr);
  }
}


void mAddressTableChange(struct mbn_handler *m, struct mbn_address_node *old, struct mbn_address_node *new) {
  struct db_node node;

  db_lock(1);
  /* new node online, check with the DB */
  if(new) {
    node.Name[0] = 0;
    node.Parent[0] = node.Parent[1] = node.Parent[2] = 0;
    node.ManufacturerID = new->ManufacturerID;
    node.ProductID = new->ProductID;
    node.UniqueIDPerProduct = new->UniqueIDPerProduct;
    node.MambaNetAddr = new->MambaNetAddr;
    node.EngineAddr = new->EngineAddr;
    node.Services = new->Services;
    node.Active = 1;
    node.FirmMajor = -1;
    node_online(&node);
  }

  /* node went offline, update status */
  if(old && !new) {
    if(db_getnode(&node, old->MambaNetAddr) && node.Active) {
      node.Active = 0;
      db_setnode(old->MambaNetAddr, &node);
    }
  }

  db_lock(0);
  m++;
}


int mSensorDataResponse(struct mbn_handler *m, struct mbn_message *msg, unsigned short obj, unsigned char type, union mbn_data dat) {
  struct db_node node;
  unsigned char *p = dat.Octets;

  if(!(obj == MBN_NODEOBJ_HWPARENT && type == MBN_DATATYPE_OCTETS)
      && !(obj == MBN_NODEOBJ_FWMAJOR && type == MBN_DATATYPE_UINT))
    return 1;

  db_lock(1);
  if(db_getnode(&node, msg->AddressFrom)) {
    if(obj == MBN_NODEOBJ_HWPARENT) {
      node.Parent[0] = (unsigned short)(p[0]<<8) + p[1];
      node.Parent[1] = (unsigned short)(p[2]<<8) + p[3];
      node.Parent[2] = (unsigned short)(p[4]<<8) + p[5];
      log_write("Received hardware parent of %08lX: %04X:%04X:%04X",
        msg->AddressFrom, node.Parent[0], node.Parent[1], node.Parent[2]);
    } else {
      node.FirmMajor = dat.UInt;
      log_write("Received firmware major revision of %08lX: %d", msg->AddressFrom, dat.UInt);
    }
    db_setnode(msg->AddressFrom, &node);
  }
  db_lock(0);
  return 0;
  m++;
}

#define WHITESPACE_CHARS  " \f\n\r\t\v"

char *trim(char *string)
{
  char *result = NULL, *ptr = NULL;

  if (string)
  {
    ptr = string;
    while (strchr(WHITESPACE_CHARS, *ptr))
      ++ptr;
    result = strdup(ptr);
    for (ptr = result; *ptr; ptr++);
    for (--ptr; strchr(WHITESPACE_CHARS, *ptr); --ptr);
    *(++ptr) = '\0';
  }
  return result;
}



int mActuatorDataResponse(struct mbn_handler *m, struct mbn_message *msg, unsigned short obj, unsigned char type, union mbn_data dat) {
  struct db_node node;
  unsigned char s, e, i, x;

  if(obj != MBN_NODEOBJ_NAME || type != MBN_DATATYPE_OCTETS)
    return 1;

  db_lock(1);
  if(db_getnode(&node, msg->AddressFrom)) {
    strncpy(node.Name, (char *)dat.Octets, 32);

    i = 0;
    x = 0;
    s = 0;
    e = 31;
    while ((strchr(WHITESPACE_CHARS, node.Name[s])) && (s<32))
      s++;
    while ((strchr(WHITESPACE_CHARS, node.Name[e])) && (e>0))
      e--;

    for (i=s; i<e; i++)
    {
      node.Name[x++] = node.Name[i];
    }
    node.Name[31] = '\0';

    node.flags &= ~DB_FLAGS_REFRESH;
    log_write("Received name of %08lX: %s", msg->AddressFrom, node.Name);
    db_setnode(msg->AddressFrom, &node);
  }
  db_lock(0);
  return 0;
  m++;
}


int mReceiveMessage(struct mbn_handler *m, struct mbn_message *msg) {
  struct mbn_message_address *nfo = &(msg->Message.Address);
  struct db_node node;

  /* ignore everything but address information messages */
  if(!(msg->MessageType == MBN_MSGTYPE_ADDRESS && nfo->Action == MBN_ADDR_ACTION_INFO))
    return 0;

  /* valid node, update LastSeen */
  if(nfo->MambaNetAddr > 0 && nfo->Services & MBN_ADDR_SERVICES_VALID) {
    db_lock(1);
    if(db_getnode(&node, nfo->MambaNetAddr)) {
      node.LastSeen = time(NULL);
      db_setnode(node.MambaNetAddr, &node);
    }
    db_lock(0);
    return 0;
  }

  /* invalid, check manufacter ID */
  if ((nfo->ManufacturerID == 0) || (nfo->ProductID == 0) || (nfo->UniqueIDPerProduct == 0)) {
    log_write("Invalid MediaAccessID %04X:%04X:%04X, Address request not processed",
      nfo->ManufacturerID, nfo->ProductID, nfo->UniqueIDPerProduct, nfo->MambaNetAddr);
    return 0;
  }
  /* invalid, update its address status */
  db_lock(1);

  /* found UniqueMediaAccessID in the DB? reply with its old address */
  if(db_nodebyid(&node, nfo->ManufacturerID, nfo->ProductID, nfo->UniqueIDPerProduct)) {
    if (time(NULL)-node.LastSeen > 5)
    {
      log_write("Address request of %04X:%04X:%04X, sent %08lX",
        node.ManufacturerID, node.ProductID, node.UniqueIDPerProduct, node.MambaNetAddr);
      set_address_struct(node.MambaNetAddr, node);
      node.LastSeen = time(NULL);
    }
    else
    {
      log_write("Address request of %04X:%04X:%04X, ignored because too short period",
        node.ManufacturerID, node.ProductID, node.UniqueIDPerProduct);
    }
    node.AddressRequests++;
    db_setnode(node.MambaNetAddr, &node);
  } else {
    /* not found, get new address and insert into the DB */
    node.ManufacturerID = nfo->ManufacturerID;
    node.ProductID = nfo->ProductID;
    node.UniqueIDPerProduct = nfo->UniqueIDPerProduct;
    node.MambaNetAddr = db_newaddress();
    node.Services = nfo->Services;
    node.Name[0] = node.Active = node.EngineAddr = 0;
    node.Parent[0] = node.Parent[1] = node.Parent[2] = 0;
    node.flags = DB_FLAGS_REFRESH;
    node.FirstSeen = node.LastSeen = time(NULL);
    node.AddressRequests = 1;
    log_write("New node added to the network: %08lX (%04X:%04X:%04X)",
      node.MambaNetAddr, node.ManufacturerID, node.ProductID, node.UniqueIDPerProduct);
    set_address_struct(node.MambaNetAddr, node);
    db_setnode(0, &node);
  }

  db_lock(0);
  return 0;
  m++;
}


void mError(struct mbn_handler *m, int code, char *str) {
  log_write("MambaNet Error: %s (%d)", str, code);
  m++;
}


void mAcknowledgeTimeout(struct mbn_handler *m, struct mbn_message *msg) {
  struct db_node node;

  db_lock(1);
  /* retry a SETNAME action when the node comes online again */
  if(msg->MessageType == MBN_MSGTYPE_OBJECT && msg->Message.Object.Action == MBN_OBJ_ACTION_SET_ACTUATOR
      && msg->Message.Object.Number == MBN_NODEOBJ_NAME) {
    log_write("Acknowledge timeout for SETNAME for %08lX", msg->AddressTo);
    if(db_getnode(&node, msg->AddressTo)) {
      node.flags |= DB_FLAGS_SETNAME;
      db_setnode(msg->AddressTo, &node);
    }
  } else
    log_write("Acknowledge timeout for message to %08lX", msg->AddressTo);

  db_lock(0);
  m++;
}

void mWriteLogMessage(struct mbn_handler *m, char *msg) {
  log_write(msg);
  return;
  m=NULL;
}

void init(int argc, char **argv) {
  struct mbn_interface *itf;
  char err[MBN_ERRSIZE];
  char ethdev[50];
  char dbstr[256];
  int c;
  int verbose;
  char oem_name[32];
  char cmdline[1024];
  int cnt;
  char socket_path[UNIX_PATH_MAX];
  char use_eth = 0;

  verbose = 0;

  strcpy(ethdev, DEFAULT_ETH_DEV);
  strcpy(dbstr, DEFAULT_DB_STR);
  strcpy(log_file, DEFAULT_LOG_FILE);
  strcpy(hwparent_path, DEFAULT_UNIX_HWPARENT_PATH);
  strcpy(socket_path, DEFAULT_UNIX_MAMBANET_PATH);

  /* parse options */
  while((c = getopt(argc, argv, "e:d:l:g:i:v")) != -1) {
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
        verbose=1;
        break;
      default:
        fprintf(stderr, "Usage: %s [-e dev] [-u path] [-g path] [-d str] [-l path] [-i id]\n", argv[0]);
        fprintf(stderr, "  -e dev   Ethernet device for MambaNet communication.\n");
        fprintf(stderr, "  -i id    UniqueIDPerProduct for the MambaNet node\n");
        fprintf(stderr, "  -g path  Hardware parent or path to gateway socket.\n");
        fprintf(stderr, "  -l path  Path to log file.\n");
        fprintf(stderr, "  -d str   PostgreSQL database connection options.\n");
        fprintf(stderr, "  -v       Verbose debug output.\n");
        exit(1);
    }
  }

  if (!verbose){
    daemonize();
    log_open();
  }

  if (oem_name_short(oem_name, 32))
  {
    strncpy(this_node.Name, oem_name, 32);
    strcat(this_node.Name, " Address Server");
  }

  hwparent(&this_node);
  db_init(dbstr);

  /* initialize the MambaNet node */
  if (!use_eth)
  {
    if((itf = mbnUnixOpen(socket_path, NULL, err)) == NULL) {
      fprintf(stderr, "mbnUnixOpen: %s\n", err);
      log_close();
      db_free();
      exit(1);
    }
  }
  else
  {
    if((itf = mbnEthernetOpen(ethdev, err)) == NULL) {
      fprintf(stderr, "Opening %s: %s\n", ethdev, err);
      log_close();
      db_free();
      exit(1);
    }
  }

  if((mbn = mbnInit(&this_node, NULL, itf, err)) == NULL) {
    fprintf(stderr, "mbnInit: %s\n", err);
    log_close();
    db_free();
    exit(1);
  }

  mbnForceAddress(mbn, 0x0001FFFF);
  mbnSetAddressTableChangeCallback(mbn, mAddressTableChange);
  mbnSetSensorDataResponseCallback(mbn, mSensorDataResponse);
  mbnSetActuatorDataResponseCallback(mbn, mActuatorDataResponse);
  mbnSetReceiveMessageCallback(mbn, mReceiveMessage);
  mbnSetErrorCallback(mbn, mError);
  mbnSetAcknowledgeTimeoutCallback(mbn, mAcknowledgeTimeout);
  mbnSetWriteLogMessageCallback(mbn, mWriteLogMessage);

  mbnStartInterface(itf, err);

  if(!verbose)
    daemonize_finish();
  log_write("--------------------------------------------------");
  log_write("%s Initialized", this_node.Name);
  log_write("Version %d.%d, compiled at %s (%s)", this_node.FirmwareMajorRevision, this_node.FirmwareMinorRevision, __DATE__, __TIME__);
  sprintf(cmdline, "command line:");
  for (cnt=0; cnt<argc; cnt++)
  {
    strcat(cmdline, " ");
    strcat(cmdline, argv[cnt]);
  }
  log_write(cmdline);
  log_write(mbnVersion());
}


int main(int argc, char **argv) {
  init(argc, argv);

  while(!main_quit && !db_loop())
    ;

  /* free */
  log_write("Closing Address Server");
  mbnFree(mbn);
  db_free();
  log_close();
  return 0;
}

