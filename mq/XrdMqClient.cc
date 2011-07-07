//          $Id: XrdMqClient.cc,v 1.00 2007/10/04 01:34:19 ajp Exp $

const char *XrdMqClientCVSID = "$Id: XrdMqClient.cc,v 1.0.0 2007/10/04 01:34:19 ajp Exp $";

#include <mq/XrdMqClient.hh>
#include <mq/XrdMqTiming.hh>

#include <XrdNet/XrdNetDNS.hh>
#include <XrdClient/XrdClientUrlSet.hh>

/******************************************************************************/
/*                        X r d M q C l i e n t                               */
/******************************************************************************/


/*----------------------------------------------------------------------------*/
/* SetXrootVariables                                                          */
/*----------------------------------------------------------------------------*/
void XrdMqClient::SetXrootVariables() {
  EnvPutInt(NAME_READCACHESIZE,0);
  EnvPutInt(NAME_MAXREDIRECTCOUNT,2);
  EnvPutInt(NAME_RECONNECTWAIT,10);
  EnvPutInt(NAME_CONNECTTIMEOUT,10);
  EnvPutInt(NAME_REQUESTTIMEOUT,300);
}


/*----------------------------------------------------------------------------*/
/* Subscribe                                                                  */
/*----------------------------------------------------------------------------*/
bool XrdMqClient::Subscribe(const char* queue) {
  if (queue) {
    // at the moment we support subscrition to a single queue only - queue has to be 0 !!!
    XrdMqMessage::Eroute.Emsg("Subscribe", EINVAL, "subscribe to additional user specified queue");
    return false;
  }
  for (int i=0;i< kBrokerN;i++) { 
    if (!GetBrokerXrdClientReceiver(i)->Open(0,0,false)) {
      // open failed
      continue;
    }
  }
  return true;
}

/*----------------------------------------------------------------------------*/
/* Unsubscribe                                                                */
/*----------------------------------------------------------------------------*/
bool XrdMqClient::Unsubscribe(const char* queue) {
  if (queue) {
    XrdMqMessage::Eroute.Emsg("Unubscribe", EINVAL, "unsubscribe from additional user specified queue");
    return false;
  }

  for (int i=0; i< kBrokerN;i++) {
    if (!GetBrokerXrdClientReceiver(i)->Close()) {    
      // open failed
      continue;
    }
  }
  return true;
}
  
/*----------------------------------------------------------------------------*/
/* SendMessage                                                                */
/*----------------------------------------------------------------------------*/
bool XrdMqClient::SendMessage(XrdMqMessage &msg, const char* receiverid, bool sign, bool encrypt) {
  bool rc = true;
  int i=0;
  // tag the sender
  msg.kMessageHeader.kSenderId = kClientId;
  // tag the send time
  XrdMqMessageHeader::GetTime(msg.kMessageHeader.kSenderTime_sec,msg.kMessageHeader.kSenderTime_nsec);
  // tag the receiver queue
  if (!receiverid) {
    msg.kMessageHeader.kReceiverQueue = kDefaultReceiverQueue;
  } else {
    msg.kMessageHeader.kReceiverQueue = receiverid;
  }
  if (encrypt) {
    msg.Sign(true);
  } else {
    if (sign) 
      msg.Sign(false);
    else 
      msg.Encode();
  }

  XrdOucString message = msg.kMessageHeader.kReceiverQueue;
  message += "?";
  message += msg.GetMessageBuffer();

  XrdClientAdmin* admin=0;
  //  msg.Print();
  for (i=0 ;i< kBrokerN; i++) {
    CheckBrokerXrdClientSender(i);
    admin = GetBrokerXrdClientSender(i);
    if (admin) {
      char result[16384];
      size_t result_size=0;
      Mutex.Lock();
      admin->Connect();
      admin->GetClientConn()->ClearLastServerError();
      admin->GetClientConn()->SetOpTimeLimit(10);
      admin->Query(kXR_Qopaquf,
                   (kXR_char *) message.c_str(),
                   (kXR_char *) result, result_size);
      if (!admin->LastServerResp()) {
        rc = false;
      }
      switch (admin->LastServerResp()->status) {
      case kXR_ok:
        rc = true;
        break;
      case kXR_error:
        rc = false;
        break;
      default:
        rc = false;
        break;
      }
    }
    // we continue until any of the brokers accepts the message
  }
  Mutex.UnLock();

  if (!rc) {
    //  XrdMqMessage::Eroute.Emsg("SendMessage", EINVAL, "send message to all brokers");  
    if (admin) {
      XrdMqMessage::Eroute.Emsg("SendMessage", admin->LastServerError()->errnum, admin->LastServerError()->errmsg);
    } else {
      XrdMqMessage::Eroute.Emsg("SendMessage", EINVAL, "no broker available");
    }
  }

  if (!rc) {
    Mutex.Lock();
    // TODO: does not work if there would be several broker !!
    ReNewBrokerXrdClientSender(0);
    Mutex.UnLock();
  }
  return true;
}

/*----------------------------------------------------------------------------*/
/* RecvMessage                                                                */
/*----------------------------------------------------------------------------*/
XrdMqMessage* XrdMqClient::RecvFromInternalBuffer() {
  if ( (kMessageBuffer.length()-kInternalBufferPosition) > 0) {
    //    fprintf(stderr,"Message Buffer %ld\n", kMessageBuffer.length());
    // there is still a message in the buffer
    int nextmessage;
    int firstmessage;

    //    fprintf(stderr,"#### %ld Entering at position %ld %ld\n", time(NULL),kInternalBufferPosition, kMessageBuffer.length());
    firstmessage = kMessageBuffer.find(XMQHEADER,kInternalBufferPosition);

    if (firstmessage == STR_NPOS)
      return 0;
    else {
      if ( (firstmessage>0) && ((size_t)firstmessage > kInternalBufferPosition)) {
        kMessageBuffer.erase(0,firstmessage);
        kInternalBufferPosition=0;
      }
    }
    
    if ( (kMessageBuffer.length()+kInternalBufferPosition) < (int)strlen(XMQHEADER))
      return 0;

    nextmessage = kMessageBuffer.find(XMQHEADER,kInternalBufferPosition+strlen(XMQHEADER));
    char savec=0;
    if (nextmessage != STR_NPOS) {savec = kMessageBuffer.c_str()[nextmessage]; ((char*)kMessageBuffer.c_str())[nextmessage] = 0;}
    XrdMqMessage* message = XrdMqMessage::Create(kMessageBuffer.c_str()+kInternalBufferPosition);
    if (!message) {
      fprintf(stderr,"couldn't get any message\n");
      return 0;
    }
    XrdMqMessageHeader::GetTime(message->kMessageHeader.kReceiverTime_sec,message->kMessageHeader.kReceiverTime_nsec);
    if (nextmessage != STR_NPOS) ((char*)kMessageBuffer.c_str())[nextmessage] = savec;
    if (nextmessage == STR_NPOS) {
      // last message
      kMessageBuffer="";
      kInternalBufferPosition=0;
    } else {
      // move forward
      //kMessageBuffer.erase(0,nextmessage);
      kInternalBufferPosition = nextmessage;
    }
    return message;
  } else {
    kMessageBuffer="";
    kInternalBufferPosition=0;
  }
  return 0;
}


XrdMqMessage* 
XrdMqClient::RecvMessage() {
  if (kBrokerN == 1) {
    // single broker case
    // try if there is still a buffered message
    XrdMqMessage* message;
    
    message = RecvFromInternalBuffer();
    
    if (message) return message;
    
    CheckBrokerXrdClientReceiver(0);

    XrdClient* client = GetBrokerXrdClientReceiver(0);
    if (!client) {
      // fatal no client 
      XrdMqMessage::Eroute.Emsg("RecvMessage", EINVAL, "receive message - no client present");  
      return 0;
    }
    struct XrdClientStatInfo stinfo;
    
    while (!client->Stat(&stinfo,true)) {
      ReNewBrokerXrdClientReceiver(0);
      client = GetBrokerXrdClientReceiver(0);
      sleep(1);
    }
    
    if (!stinfo.size) {
      return 0;
    }

    // mantain a receiver buffer which fits the need
    if (kRecvBufferAlloc < stinfo.size) {
      int allocsize = 1024*1024;
      if (stinfo.size > allocsize) {
        allocsize = stinfo.size + 1;
      }
      kRecvBuffer = (char*)realloc(kRecvBuffer,allocsize);
      if (!kRecvBuffer) {
        // this is really fatal - we exit !
        exit(-1);
      }
      kRecvBufferAlloc = allocsize;
    }
    // read all messages
    size_t nread = client->Read(kRecvBuffer, 0, stinfo.size);
    if (nread>0) {
      kRecvBuffer[nread] = 0;
      // add to the internal message buffer
      kInternalBufferPosition=0;
      kMessageBuffer = kRecvBuffer;
    }
    return RecvFromInternalBuffer();
    // ...
  } else {
    // multiple broker case
    return 0;
  }

  return 0;
}

/*----------------------------------------------------------------------------*/
/* RegisterRecvCallback                                                       */
/*----------------------------------------------------------------------------*/
bool XrdMqClient::RegisterRecvCallback(void (*callback_func)(void *arg)) {
  return false;
}

/*----------------------------------------------------------------------------*/
/* GetBrokerUrl                                                              */
/*----------------------------------------------------------------------------*/
XrdOucString*
XrdMqClient::GetBrokerUrl(int i) {
  XrdOucString n = "";
  n+= i;
  return kBrokerUrls.Find(n.c_str());
}

/*----------------------------------------------------------------------------*/
/* GetBrokerId                                                                */
/*----------------------------------------------------------------------------*/
XrdOucString
XrdMqClient::GetBrokerId(int i) {
  XrdOucString brokern;
  if (i==0) 
    brokern = "0";
  else 
    brokern += kBrokerN;
  return brokern;
}

/*----------------------------------------------------------------------------*/
/* GetBrokerXrdClientReceiver                                                 */
/*----------------------------------------------------------------------------*/
XrdClient*
XrdMqClient::GetBrokerXrdClientReceiver(int i) {
  return kBrokerXrdClientReceiver.Find(GetBrokerId(i).c_str());
}

/*----------------------------------------------------------------------------*/
/* GetBrokerXrdClientSender                                                   */
/*----------------------------------------------------------------------------*/
XrdClientAdmin*
XrdMqClient::GetBrokerXrdClientSender(int i) {
  return kBrokerXrdClientSender.Find(GetBrokerId(i).c_str());
}


/*----------------------------------------------------------------------------*/
/* ReNewBrokerXrdClientSender                                                 */
/*----------------------------------------------------------------------------*/

void XrdMqClient::ReNewBrokerXrdClientSender(int i) {
  fprintf(stderr,"XrdMqClient::ReNewBorkerXrdClientSender %d", i);
  kBrokerXrdClientSender.Del(GetBrokerId(i).c_str());
  SetXrootVariables();
  kBrokerXrdClientSender.Add(GetBrokerId(i).c_str(), new XrdClientAdmin(GetBrokerUrl(i)->c_str()));
  GetBrokerXrdClientSender(i)->Connect();
}

/*----------------------------------------------------------------------------*/
/* ReNewBrokerXrdClientReceiver                                               */
/*----------------------------------------------------------------------------*/

void XrdMqClient::ReNewBrokerXrdClientReceiver(int i) {
  fprintf(stderr,"XrdMqClient::ReNewBorkerXrdClientReceiver %d", i);
  kBrokerXrdClientReceiver.Del(GetBrokerId(i).c_str());
  SetXrootVariables();
  
  kBrokerXrdClientReceiver.Add(GetBrokerId(i).c_str(), new XrdClient(GetBrokerUrl(i)->c_str()));
  if (!GetBrokerXrdClientReceiver(i)->Open(0,0,false)) {
    fprintf(stderr,"XrdMqClient::Reopening of new alias failed ...\n");
  }
}

/*----------------------------------------------------------------------------*/
/* CheckBrokerXrdClientSender                                                 */
/*----------------------------------------------------------------------------*/

void XrdMqClient::CheckBrokerXrdClientSender(int i) {
  Mutex.Lock();
  XrdClientAdmin* client = GetBrokerXrdClientSender(i);
  if (i < 256) {
    if (kBrokerXrdClientSenderAliasTimeStamp[i] &&
        ( (time(NULL) - kBrokerXrdClientSenderAliasTimeStamp[i]) < 10 ) ) {
      // do nothing
    } else {
      XrdClientUrlSet alias(GetBrokerUrl(i)->c_str());
      alias.Rewind();
      XrdClientUrlInfo* currentalias = alias.GetNextUrl();
      if (currentalias->GetUrl() != client->GetClientConn()->GetCurrentUrl().GetUrl()) {
        fprintf(stderr,"XrdMqClient::CheckBrokerXrdClientSender => Broker alias changed from %s => %s\n", client->GetClientConn()->GetCurrentUrl().GetUrl().c_str(), currentalias->GetUrl().c_str());
        // the alias has been switched, del the client and create a new one to connect to the new alias
        ReNewBrokerXrdClientSender(i);
        // get the new client object
        GetBrokerXrdClientSender(i);
        kBrokerXrdClientSenderAliasTimeStamp[i] = time(NULL);
      }
    }
  }
  Mutex.UnLock();
}
   
/*----------------------------------------------------------------------------*/
/* CheckBrokerXrdClientReceiver                                               */
/*----------------------------------------------------------------------------*/
 
void XrdMqClient::CheckBrokerXrdClientReceiver(int i) {
  Mutex.Lock();
  XrdClient* client = GetBrokerXrdClientReceiver(i);
  if (i < 256) {
    if (kBrokerXrdClientReceiverAliasTimeStamp[i] &&
        ( (time(NULL) - kBrokerXrdClientReceiverAliasTimeStamp[i]) < 10 ) ) {
      // do nothing
    } else {
      XrdClientUrlSet alias(GetBrokerUrl(i)->c_str());
      alias.Rewind();
      XrdClientUrlInfo* currentalias = alias.GetNextUrl();
      if (currentalias->GetUrl() != client->GetClientConn()->GetCurrentUrl().GetUrl()) {
        fprintf(stderr,"XrdMqClient::CheckBrokerXrdClientReceiver => Broker alias changed from %s => %s\n", client->GetClientConn()->GetCurrentUrl().GetUrl().c_str(), currentalias->GetUrl().c_str());

        ReNewBrokerXrdClientReceiver(i);
        // get the new client object
        GetBrokerXrdClientReceiver(i);
        // the alias has been switched, del the client and create a new one to connect to the new alias

        kBrokerXrdClientReceiverAliasTimeStamp[i] = time(NULL);
      }
    }
  }

  Mutex.UnLock();
}


/*----------------------------------------------------------------------------*/
/* AddBroker                                                                  */
/*----------------------------------------------------------------------------*/
bool XrdMqClient::AddBroker(const char* brokerurl, bool advisorystatus, bool advisoryquery) {
  bool exists=false;
  if (!brokerurl) return false;

  XrdOucString newBrokerUrl = brokerurl;
  if ((newBrokerUrl.find("?"))==STR_NPOS) {
    newBrokerUrl+= "?";
  }

  newBrokerUrl+= "&"; newBrokerUrl += XMQCADVISORYSTATUS; newBrokerUrl+="=";newBrokerUrl += advisorystatus;
  newBrokerUrl+= "&"; newBrokerUrl += XMQCADVISORYQUERY; newBrokerUrl+="=";newBrokerUrl += advisoryquery;

  printf("==> new Broker %s\n",newBrokerUrl.c_str());
  for (int i=0;i< kBrokerN;i++) { 
    XrdOucString* brk = GetBrokerUrl(i);
    if (brk && ((*brk) == newBrokerUrl)) exists = true;
  }
  if (!exists) {
    XrdOucString brokern = GetBrokerId(kBrokerN);
    kBrokerUrls.Add(brokern.c_str(), new XrdOucString(newBrokerUrl.c_str()));
    kBrokerXrdClientSender.Add(GetBrokerId(kBrokerN).c_str(), new XrdClientAdmin(newBrokerUrl.c_str()));

    SetXrootVariables();

    kBrokerXrdClientReceiver.Add(GetBrokerId(kBrokerN).c_str(), new XrdClient(newBrokerUrl.c_str()));

    if (!GetBrokerXrdClientSender(kBrokerN)->Connect()) {
      kBrokerUrls.Del(brokern.c_str());
      kBrokerXrdClientSender.Del(GetBrokerId(kBrokerN).c_str());
      kBrokerXrdClientReceiver.Del(GetBrokerId(kBrokerN).c_str());
      XrdMqMessage::Eroute.Emsg("AddBroker", EPERM, "add and connect to broker:", newBrokerUrl.c_str());
      return false;
    }
    kBrokerN++;
  }
  return (!exists);
}

/*----------------------------------------------------------------------------*/
/* Constructor                                                                */
/*----------------------------------------------------------------------------*/
XrdMqClient::XrdMqClient(const char* clientid, const char* brokerurl, const char* defaultreceiverid) {
  kBrokerN=0;
  kMessageBuffer="";
  kRecvBuffer=0;
  kRecvBufferAlloc=0;
  
  memset(kBrokerXrdClientReceiverAliasTimeStamp,0,sizeof(int) * 256);
  memset(kBrokerXrdClientSenderAliasTimeStamp  ,0,sizeof(int) * 256);

  if (brokerurl && (!AddBroker(brokerurl))) {
    fprintf(stderr,"error: cannot add broker %s\n", brokerurl);
  }
  if (defaultreceiverid) {
    kDefaultReceiverQueue = defaultreceiverid;
  } else {
    // default receiver is always a master
    kDefaultReceiverQueue = "/xmessage/*/master/*";
  }

  if (clientid) {
    kClientId = clientid;
    if (kClientId.beginswith("root://")) {
      // truncate the URL away
      int pos = kClientId.find("//",7);
      if (pos!=STR_NPOS) {
        kClientId.erase(0,pos+1);
      }
    }
  } else {
    // the default is to create the client id as /xmesssage/<domain>/<host>/
    XrdOucString FullName      = XrdNetDNS::getHostName();
    int ppos=0;
    XrdOucString HostName = FullName;
    XrdOucString Domain   = FullName;
    if ( (ppos = FullName.find("."))!=STR_NPOS) {
      HostName.assign(FullName, 0,ppos-1);
      Domain.assign(FullName,ppos+1);
    } else {
      Domain = "unknown";
    }

    kClientId = "/xmessage/"; kClientId += HostName; kClientId+="/"; kClientId += Domain;
  }
}

/*----------------------------------------------------------------------------*/
/* Destructor                                                                 */
/*----------------------------------------------------------------------------*/
XrdMqClient::~XrdMqClient() {
}

