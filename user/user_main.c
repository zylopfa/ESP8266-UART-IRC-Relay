#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "ip_addr.h"
#include "espconn.h"
#include "user_interface.h"
#include "user_config.h"
#include <mem.h> 
#include <strings.h>
#include <stdlib.h>

#define DEFAULT_SECURITY 1

char* BOT_VERSION = "Zylobot v1.0";
int isReady =0;

struct espconn irc_conn;
ip_addr_t irc_ip;
esp_tcp irc_tcp;

char irc_host[] = "irc.example.com";
int irc_port = 6667;
char irc_nick[] = "ESP8266bot";
char entryPage[] = "";
char pass[] = "";
char defaultChannel[] = "#bots";


char CTCP_VERSION[10] = {0x01, 'V','E','R','S','I','O','N',0x01,0x00};
char CTCP_PING[6] = {0x01,'P','I','N','G',0x00};

bool MovementRegistered = 0;


char buf[513];       // buffer for incomming irc
char cmdBuffer[1000]; // buffer for outgoing messages from the bot
char cmdTarget[50];   // if the bot needs to leav irc to do stuff, who is the person he needs to communicate results with when he return.
int returnFunction=0;   //  what function has the bot completed, must be set if the bot do tasks and re-connect to irc, so we can see what
                        // task it did and then deliver the data
                        // 0 = no taks done
			// 1 = !SCAN, wifi scanning done
int reconnectTries=0;
int connectionError=0; // 1= too many login tries (10) to requested accesspoint

os_timer_t scanTimer;

unsigned char *default_certificate;
unsigned int default_certificate_len = 0;
unsigned char *default_private_key;
unsigned int default_private_key_len = 0;


int sentProgression = 0;  // code for the data-sent callback to know which function its working on (multiple packets belonging together
uint32  CHIP_ID;

void mygpio_init(void);
void timerCallback(void *pArg);
void irc_wifi_scan(void);
static void scanDoneCallback (void * arg, STATUS status);
void wifi_callback( System_Event_t*);
void scanTimerCallback(void *);
void commandTimerReturnCallback(void *pArg);
void ircMultiLineCallback(void *pArg);
void irc_send_multi(int total);
void irc_connect_to_wifi(int index);
void apConnectTimerCallback(void *pArg);
void connectCurrentAP();


struct accesspoint 
{
  bool isOpen;
  uint8 essid[32];
  char passcode[64];
  int preference;
};

struct Buffer
{
  char data[500];
};

#define IRC_SENDBUFFER_SIZE 50

int BufferToSendTotal = 0;
int BufferCurrentIndex = 0;

struct Buffer sendBuffer[IRC_SENDBUFFER_SIZE];
struct accesspoint openApList[20];

struct accesspoint myAccessPoints[5]; // the access points we currently "own"
int currentAccessPointIndex = 0;


int maxAdded; // how many entries have we added to the list currently

void user_rf_pre_init( void )
{
}


void data_received( void *arg, char *pdata, unsigned short len )
{
    struct espconn *conn = arg;

    char *user, *command, *where, *message, *sep, *target;
    int i, j, l, sl, o = -1, start, wordcount;


    for ( i=0; i < len; i++) {
     o++;
     buf[o] = pdata[i];

     if (   (  (i > 0) && (pdata[i] == '\n') && (pdata[i - 1] == '\r')) || o == 512) {

         buf[o + 1] = '\0';
         l = o;
         o = -1;
         if (!os_strncmp(buf, "PING", 4)) {
              buf[1] = 'O';
              espconn_sent(conn,buf,os_strlen(buf));
              sentProgression = 3;

         } else if (buf[0] == ':') {
                    wordcount = 0;
                    user = command = where = message = NULL;
                    for (j = 1; j < l; j++) {
                        if (buf[j] == ' ') {
                            buf[j] = '\0';
                            wordcount++;
                            switch(wordcount) {
                                case 1: user = buf + 1; break;
                                case 2: command = buf + start; break;
                                case 3: where = buf + start; break;
                            }
                            if (j == l - 1) continue;
                            start = j + 1;
                        } else if (buf[j] == ':' && wordcount == 3) {
                            if (j < l - 1) message = buf + j + 1;
                            break;
                        }
                    }

                    if (wordcount < 2) continue;

                    if (!os_strncmp(command,"001",3) && ( os_strlen(pass) > 3)) {
		       os_sprintf( cmdBuffer, "PRIVMSG NickServ IDENTIFY %s\r\n",pass);
                       espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
                    }
                    if (!os_strncmp(command, "001", 3) && (defaultChannel != NULL)) {
                        os_sprintf( cmdBuffer, "JOIN %s\r\n",defaultChannel);
                        espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
                    } 
                    else if (!os_strncmp(command, "JOIN", 4)) {
			isReady = 1;

			if (returnFunction > 0) {

			  if (returnFunction == 1) {
			    os_timer_setfn(&scanTimer, commandTimerReturnCallback, NULL);
                            os_timer_arm(&scanTimer, 4000, 0);
			  }
		          if (returnFunction == 2) {
                            os_timer_setfn(&scanTimer, commandTimerReturnCallback, NULL);
                            os_timer_arm(&scanTimer, 4000, 0);
		          }


			}

                    }
                    else if (!strncmp(command,"VERSION",7)) {
                        // os_printf("VERSION COMMAND RECEIVED!!\n");
                    }

                    else if (!os_strncmp(command, "PRIVMSG", 7) || !os_strncmp(command, "NOTICE", 6)) {

                        if (where == NULL || message == NULL) continue;
                        if ((sep = strchr(user, '!')) != NULL) user[sep - user] = '\0';
                        if (where[0] == '#' || where[0] == '&' || where[0] == '+' || where[0] == '!') target = where; else target = user;

                        // Message in channel, not directly to the bot
                        if (!os_strcmp(where,defaultChannel)) {
                          if ( !os_strncmp(message,irc_nick,strlen(irc_nick))) {
                            os_sprintf( cmdBuffer, "PRIVMSG %s Hejsa %s\r\n",defaultChannel,user);
                            espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
                          } 
                        }

                        // Message directed to the bot
			// The command !UART sends the following command string directly to the
                        // micro controller, connected via uart to tx,rx on the ESP module
                        if (!strcasecmp(where,irc_nick)) {

			  if (!os_strncmp(message,"!LDR",4) ) {
                            uint16 adcValue = system_adc_read();
                            os_sprintf( cmdBuffer, "NOTICE %s %d\r\n",target,adcValue);
                            espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
			  }
			  if (!os_strncmp(message,"!BEEP",5) ) {
                            os_sprintf( cmdBuffer, "NOTICE %s Toggeling Beeper\r\n",target);

		            if (GPIO_REG_READ(GPIO_OUT_ADDRESS) & BIT4)
			    {
		              //Set GPIO4 to LOW
			      gpio_output_set(0, BIT4, BIT4, 0);
			    }
			    else
			    {
		             //Set GPIO4 to HIGH
			     gpio_output_set(BIT4, 0, BIT4, 0);
			    }
                            espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
			  }
                          if (!os_strncmp(message,"!LEFT",5)) {
                             gpio_output_set(0, BIT2, BIT2, 0);  // set 2 and 0 to low
                             gpio_output_set(0, BIT0, BIT0, 0);
                             gpio_output_set(BIT2, 0, BIT2, 0); // ser 2 high
			  }
                          if (!os_strncmp(message,"!RIGHT",6)) {
                             gpio_output_set(0, BIT2, BIT2, 0);  // set 2 and 0 to low
                             gpio_output_set(0, BIT0, BIT0, 0);
                             gpio_output_set(BIT0, 0, BIT0, 0); // ser 0 high
			  }
                          if (!os_strncmp(message,"!STOP",5)) {
                             gpio_output_set(0, BIT2, BIT2, 0);  // set 2 and 0 to low
                             gpio_output_set(0, BIT0, BIT0, 0);
			  }
			  if (!os_strncmp(message,"!CHIP",5)) {
                            os_sprintf( cmdBuffer, "NOTICE %s CHIP ID: %d\r\n",target,CHIP_ID);
                            espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
			  }
			  if (!os_strncmp(message,"!SCAN",5)) {
			    returnFunction=1;
			    // os_printf("Scan user:%s\r\n",target);
                            os_sprintf(cmdTarget,"%s",target);
			    irc_wifi_scan();
			  }
			  if (!os_strncmp(message,"!IP",3)) {
			    static struct ip_info info;
			    if (wifi_get_ip_info(0x00,&info)) {
                              os_sprintf(sendBuffer[0].data,"NOTICE %s My LOCAL IP-Address is: %d.%d.%d.%d\r\n",target,IP2STR(&info.ip));
                              os_sprintf(sendBuffer[1].data,"NOTICE %s My LOCAL Netmask    is: %d.%d.%d.%d\r\n",target,IP2STR(&info.netmask));
                              os_sprintf(sendBuffer[2].data,"NOTICE %s My LOCAL Gateway    is: %d.%d.%d.%d\r\n",target,IP2STR(&info.gw));
                              irc_send_multi(3);
			    }
			  }
                          if ( !os_strncmp(message,"!TESTUART",9)) {
                             os_printf("ZYLOLINK\r\n");
                          }
			  if ( (!os_strncmp(message,"!UART",5)) && os_strlen(message) > 7 ) {
                             os_printf("%s",message+6);
                          }
			  if(!os_strncmp(message,"!HELP",5)) {
                            os_sprintf(sendBuffer[0].data,"NOTICE %s : So you wan't help, %s ,ehh?\r\n",target,target);
                            os_sprintf(sendBuffer[1].data,"NOTICE %s : Very well, you can whisper me the following commands:\r\n",target);
                            os_sprintf(sendBuffer[2].data,"NOTICE %s : \r\n ",target);
                            os_sprintf(sendBuffer[3].data,"NOTICE %s : !LDR - Shows the light level from 0 to 1023, 0 is dark 1023 is brightest\r\n",target);
                            os_sprintf(sendBuffer[4].data,"NOTICE %s : !BEEP turns on a buzzer, be quick to turn it off again or the neighbors complain!\r\n",target);
                            os_sprintf(sendBuffer[5].data,"NOTICE %s : !CHIP shows you my chip id, its very personal to me so whats yours?\r\n",target);
                            os_sprintf(sendBuffer[6].data,"NOTICE %s : !IP shows you my current ip address, netmask and gateway\r\n",target);
                            os_sprintf(sendBuffer[7].data,"NOTICE %s : !SCAN will make me scan surrounding air for wifi accesspoints, i will log back and tell you the result!\r\n",target);
                            os_sprintf(sendBuffer[8].data,"NOTICE %s : !AP-JOIN <essid>,[pasword] join the ap, if I am unable to join in 10 tries, i will log back on this ap and tell you about it\r\n",target);

                            os_sprintf(sendBuffer[9].data, "NOTICE %s : \r\n",target);
                            os_sprintf(sendBuffer[10].data, "NOTICE %s : - messages in the irc channel stating movement in sector 9, is a warning that I have\r\n",target);
                            os_sprintf(sendBuffer[11].data,"NOTICE %s :   picked up movement and will make short work of the enemy, i will reduce him to a bool\r\n",target);
			    irc_send_multi(12);
			  }
			  if (!os_strncmp(message,"!AP-JOIN",8)) {
			    char * pch;
                            int cnr = 0;
			    pch = strtok (message," ");

			    os_sprintf(sendBuffer[10].data,"\0");
			    os_sprintf(sendBuffer[11].data,"\0");
			    os_sprintf(sendBuffer[12].data,"\0");

			    while (pch != NULL && (cnr < 3)) {
			      os_sprintf(sendBuffer[10+cnr].data,"%s",pch);
			      cnr++;
			      pch = strtok (NULL, ",\r");
			    }

			    if ( os_strlen(sendBuffer[10].data) < 2 || os_strlen(sendBuffer[11].data) < 2 || os_strlen(sendBuffer[12].data) < 2) {
                              os_sprintf(sendBuffer[0].data,"NOTICE %s No no you are doing it wrong please use the command like this: \r\n",target);
                              os_sprintf(sendBuffer[1].data,"NOTICE %s \t!AP-JOIN <ssid>,<passcode>\r\n",target);
                              irc_send_multi(2);
			    }
			    else { // ok we try to connect, but we will warn if the accesspoint is not in our list
                              os_sprintf(sendBuffer[0].data,"NOTICE %s Ok I will try to connect to the accesspoint you gave me \r\n",target);
                              os_sprintf(sendBuffer[1].data,"NOTICE %s If I am not back here in 10 seconds, it failed and I am dead! \r\n",target);
                              os_sprintf(sendBuffer[2].data,"NOTICE %s Naa just kidding, I will log back to the current accesspoint and complain!\r\n",target);
                              irc_send_multi(3);

			      os_strncpy(myAccessPoints[1].essid,sendBuffer[11].data,32 );
			      os_strncpy(myAccessPoints[1].passcode,sendBuffer[12].data,64 );

		   	      myAccessPoints[1].preference = 2;
                              os_sprintf(cmdTarget,"%s",target);
			      returnFunction=2;
			      irc_connect_to_wifi(1);
			    }

                          }
                          if (!os_strncmp(message,CTCP_VERSION,9)) {
                              //os_printf("%s wants our Version!!\n",target);
                              //os_sprintf( cmdBuffer, "NOTICE %s %c%s%s%c\r\n",target,0x01,"VERSION ",BOT_VERSION,0x01);
                              //espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
                          }

                          if (!os_strncmp(message,CTCP_PING,5)) {
                             //os_printf("%s wants our ping time!!\n",target);

                             char *timeStamp = (char*) os_malloc(20);
                             os_strncpy(timeStamp, message+6, os_strlen(message)-8);
                             timeStamp[(os_strlen(message)-8)] = '\0';
                             // os_printf("%s wants our ping time:%s!!\n",target,timeStamp);
			
			     os_sprintf( cmdBuffer, "NOTICE %s %c%s%s%c\r\n",target,0x01,"PING ",timeStamp,0x01);
                             espconn_sent(conn,cmdBuffer,os_strlen(cmdBuffer));
                          }


                        } // end of messages directed to the bot




		   	

		    } // end of cmd="PRIVMSG" || cmd="NOTICE"

         } // else if not PING
        


     } // if

    } // for

}



// global var cmdTarget contains the irc target to talk to when we return from scan
void irc_wifi_scan(void) {

  os_timer_setfn(&scanTimer, scanTimerCallback, NULL);
  os_timer_arm(&scanTimer, 3000, 0);

}


void commandTimerReturnCallback(void *pArg) {
    if ( returnFunction == 1) { // scan done callbck
     returnFunction = 0;
     os_sprintf( cmdBuffer, "NOTICE %s Returning with info regarding wifi scan\r\n",cmdTarget);
     espconn_sent(&irc_conn,cmdBuffer,os_strlen(cmdBuffer));

     int i = 0;

     for (i=0;i<maxAdded;i++) {
       os_sprintf(sendBuffer[i].data,"NOTICE %s %d\t\t%s\r\n",cmdTarget,openApList[i].isOpen,openApList[i].essid);
     }
     BufferToSendTotal= i+1;
     os_timer_setfn(&scanTimer, ircMultiLineCallback, NULL);
     os_timer_arm(&scanTimer, 2000, 0);
    }
    else if (returnFunction == 2) { // change of ap done callback
     returnFunction = 0;

     if (connectionError==1) {
       os_sprintf(sendBuffer[0].data,"NOTICE %s Fuck don't do that again, tried 10 times to connect to the requested ap. Logged back on this AP %s!\r\n",cmdTarget,myAccessPoints[0].essid);
     }
     else {
       os_sprintf(sendBuffer[0].data,"NOTICE %s We have successfully joined the requested accesspoint!\r\n",cmdTarget);
     }

     connectionError=0;	
     BufferToSendTotal=1;
     os_timer_setfn(&scanTimer, ircMultiLineCallback, NULL);
     os_timer_arm(&scanTimer, 2000, 0);
    }


}

void ircMultiLineCallback(void *pArg) {
     irc_send_multi(BufferToSendTotal);
}





void irc_send_multi(int total) {

   if ( total > 0 ) {
     BufferToSendTotal  = total;
     BufferCurrentIndex = 0;
   }

   if ( (total > 0)  &&  !(total < IRC_SENDBUFFER_SIZE) ) { // too much to send exit
    return;
   }

   if ( BufferCurrentIndex == BufferToSendTotal) {
     BufferToSendTotal = 0;
     BufferCurrentIndex = 0;
     sentProgression=0;
     return;
   }
   sentProgression=5;  // inform the packetSent callback that we are sending multiple packets
   espconn_sent(&irc_conn,sendBuffer[BufferCurrentIndex].data,os_strlen(sendBuffer[BufferCurrentIndex].data));
   BufferCurrentIndex++;
}


void disconnectTimerCallback(void *pArg) {
  espconn_disconnect(&irc_conn); // will also disconnect wifi
				 // we now jump to tcp disconnect handler + wifi disconnect handler
                                 // and from there we initialize the function specified by global var returnFunction
}

void scanTimerCallback(void *pArg) {

  if (isReady) {  // disconnect from irc if we are logged in
    os_sprintf( cmdBuffer, "PART %s The best bot is leaving!!\r\n",defaultChannel);
    espconn_sent(&irc_conn,cmdBuffer,os_strlen(cmdBuffer));    
    isReady = 0; // we are logged off irc
  }
  // os_printf("starting scan!\r\n");

  os_timer_setfn(&scanTimer, disconnectTimerCallback, NULL);
  os_timer_arm(&scanTimer, 3000, 0);

} 

void apConnectTimerCallback(void *pArg) {
  if (isReady) {  // disconnect from irc if we are logged in
    os_sprintf( cmdBuffer, "PART %s The best bot is leaving!!\r\n",defaultChannel);
    espconn_sent(&irc_conn,cmdBuffer,os_strlen(cmdBuffer));    
    isReady = 0; // we are logged off irc
  }
  // os_printf("starting new ap connection!\r\n");

  os_timer_setfn(&scanTimer, disconnectTimerCallback, NULL);
  os_timer_arm(&scanTimer, 3000, 0);
} 



void data_sent (void *arg) {
   struct espconn *conn = arg;    


   if ( sentProgression == 1) {
     char buffer2[50];
     os_sprintf( buffer2, "NICK %s\r\n",irc_nick);
     sentProgression = 2;
     espconn_sent(conn,buffer2,os_strlen(buffer2));
   }

   if ( sentProgression == 3) {
     // os_printf("Sent Ping!\n");
     sentProgression = 0;
   }

   if ( sentProgression == 5) { // multi irc packet sending
     irc_send_multi(0);
   }

   // espconn_disconnect( conn );
}


LOCAL void ICACHE_FLASH_ATTR
user_tcp_recon_cb(void *arg, sint8 err)
{
   //error occured , tcp connection broke. user can try to reconnect here.
   // os_printf("reconnect callback, error code %d !!! \r\n",err);
}



void tcp_connected( void *arg )
{
    struct espconn *conn = arg;
    reconnectTries=0; 
    

    // set our current access point myAccessPoints[0] to the access point nr we are NOW logged in to.
    if(currentAccessPointIndex != 0) {
     os_memcpy( &myAccessPoints[0].essid,&myAccessPoints[currentAccessPointIndex].essid ,32 );
     os_memcpy( &myAccessPoints[0].passcode,&myAccessPoints[currentAccessPointIndex].passcode ,64 );
    }
    // os_printf( "%s\n", __FUNCTION__ );
    espconn_regist_recvcb( conn, data_received );
    espconn_regist_sentcb( conn, data_sent);

    char buffer1[50];
    sentProgression=1;
    os_sprintf( buffer1, "USER %s 0 0 :%s\r\n",irc_nick,irc_nick);
    espconn_sent(conn,buffer1,os_strlen(buffer1));

}


void tcp_disconnected( void *arg )
{
    struct espconn *conn = arg;
    // os_printf( "%s\n", __FUNCTION__ );
    wifi_station_disconnect();
}


void dns_done( const char *name, ip_addr_t *ipaddr, void *arg )
{
    struct espconn *conn = arg;    
    // os_printf( "%s\n", __FUNCTION__ );

    if ( ipaddr == NULL) 
    {
        // os_printf("DNS lookup failed\n");
        wifi_station_disconnect();
    }
    else
    {
        // os_printf("Connecting...\n" );
        conn->type = ESPCONN_TCP;
        conn->state = ESPCONN_NONE;
        conn->proto.tcp=&irc_tcp;
        conn->proto.tcp->local_port = espconn_port();
        conn->proto.tcp->remote_port = irc_port;
        os_memcpy( conn->proto.tcp->remote_ip, &ipaddr->addr, 4 );
        espconn_regist_connectcb( conn, tcp_connected );
        espconn_regist_reconcb(conn, user_tcp_recon_cb); // register reconnect callback as error handler
        espconn_regist_disconcb( conn, tcp_disconnected );
        espconn_connect( conn );
    }
}


void wifi_callback( System_Event_t *evt )
{

    switch ( evt->event )
    {
        case EVENT_STAMODE_CONNECTED:
        {
            //os_printf("connect to ssid %s, channel %d\n",
            //            evt->event_info.connected.ssid,
            //            evt->event_info.connected.channel);
            break;
        }

        case EVENT_STAMODE_DISCONNECTED:
        {
            //os_printf("disconnect from ssid %s, reason %d\n",
            //            evt->event_info.disconnected.ssid,
            //            evt->event_info.disconnected.reason);


	    if ( returnFunction == 1 ) { // disconnect, in ap scan mode!
             // os_printf("We prepare to scan from the wifi disconnect hndler \r\n");
	     static struct scan_config scanConfig;
	     wifi_set_event_handler_cb( wifi_callback );
	     wifi_set_opmode_current(STATION_MODE);
	     wifi_set_opmode(STATION_MODE);
	     wifi_promiscuous_enable(0);
	     wifi_station_scan(NULL,scanDoneCallback);
	    }
	    if ( returnFunction == 2) { // returning from logging on to other ap!
             // os_printf("We wanna connect to new ap, in wifi_callback\r\n");
	     connectCurrentAP();
	    }

           // deep_sleep_set_option( 0 );
           // system_deep_sleep( 2 * 1000 * 1000 );  // 60 seconds
            break;
        }

        case EVENT_STAMODE_GOT_IP:
        {
            //os_printf("ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR,
            //            IP2STR(&evt->event_info.got_ip.ip),
            //            IP2STR(&evt->event_info.got_ip.mask),
            //            IP2STR(&evt->event_info.got_ip.gw));
            //os_printf("\n");

            espconn_gethostbyname( &irc_conn, irc_host, &irc_ip, dns_done );
            break;
        }

        default:
        {
            break;
        }
    }
}

static void ICACHE_FLASH_ATTR
scanDoneCallback (void* arg, STATUS status) {

    struct bss_info *bssInfo;
    bssInfo = (struct bss_info *)arg;
    maxAdded=0;

    // skip the first in the chain ... it is invalid
    bssInfo = STAILQ_NEXT(bssInfo, next);
    while(bssInfo != NULL) {
       if (maxAdded >= 20){ maxAdded=19; break; }
       openApList[maxAdded].isOpen = (bssInfo->authmode == AUTH_OPEN);
       os_memcpy(&openApList[maxAdded].essid,bssInfo->ssid,32);
       maxAdded++;
       bssInfo = STAILQ_NEXT(bssInfo, next);
    }

    connectCurrentAP();
}


void connectCurrentAP() {

  reconnectTries++;

  if (reconnectTries == 10) {
   reconnectTries = 0;
   connectionError=1; // too many connection tries on AP
   currentAccessPointIndex = 0; // switch to last working ap
  }

  static struct station_config config;
  config.bssid_set = 0;
  wifi_station_set_hostname( "zylopfian-sensor" );
  wifi_set_opmode_current( STATION_MODE );

  os_memcpy( &config.ssid,&myAccessPoints[currentAccessPointIndex].essid, 32 );
  os_memcpy( &config.password,&myAccessPoints[currentAccessPointIndex].passcode, 64 );
  wifi_station_set_config( &config );
  wifi_set_event_handler_cb( wifi_callback );
  wifi_station_connect();

}




void pinIntrptCallback(uint32 interruptMask, void *arg) {


  uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);

     // if the interrupt was by GPIO5 
    if (gpio_status & BIT(5))
    {
        // disable interrupt for GPIO4
        gpio_pin_intr_state_set(GPIO_ID_PIN(5), GPIO_PIN_INTR_DISABLE);


        if (MovementRegistered == 0) {
	  // os_printf("PIN %d\r\n",interruptMask);
	  MovementRegistered = 1;

          if (isReady) {  // if we are connected to irc with a valid nick we send a message
             os_sprintf( cmdBuffer, "PRIVMSG %s Movement Detected in sector 9\r\n",defaultChannel);
             espconn_sent(&irc_conn,cmdBuffer,os_strlen(cmdBuffer));
 	     // setup timer to reset movement registered after a brief time
             os_timer_setfn(&scanTimer, timerCallback, NULL);
             os_timer_arm(&scanTimer, 1000, 0);
          }

	}

        //clear interrupt status for GPIO4
        GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(5));
    }

}


void timerCallback(void *pArg) {
   // os_printf("Tick!\n");
   MovementRegistered = 0;
   gpio_pin_intr_state_set(GPIO_ID_PIN(5),5);
}


// connect to wifi that we have told the module 
void irc_connect_to_wifi(int index) {

  currentAccessPointIndex = index;

  os_timer_setfn(&scanTimer, apConnectTimerCallback, NULL);
  os_timer_arm(&scanTimer, 5000, 0);

}



void ICACHE_FLASH_ATTR
init_done(void)
{
   if(wifi_get_opmode() == SOFTAP_MODE)
   {
     return;
   }

   mygpio_init();  // initialize gpio pins + interrupt
   static struct station_config config;

   wifi_station_set_hostname( "zylopfian-sensor" );
   wifi_set_opmode_current( STATION_MODE );

   currentAccessPointIndex = 0;

   os_memcpy(&myAccessPoints[0].essid,"AndroidAP",32);
   os_memcpy(&myAccessPoints[0].passcode, "testingPassword", 64 );
   myAccessPoints[0].preference = 1;

   connectCurrentAP();

}


void ICACHE_FLASH_ATTR 
mygpio_init(void) {

   // on  the module the pin 4 is pin 5 and vice versa. labeled wrong
   PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO4_U, FUNC_GPIO4);
   gpio_output_set(0, GPIO_ID_PIN(4),GPIO_ID_PIN(4) , 0);

   ETS_GPIO_INTR_ATTACH(pinIntrptCallback, NULL);
   ETS_GPIO_INTR_DISABLE();
   PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5);
   gpio_output_set(0, 0, 0, GPIO_ID_PIN(5));

   gpio_register_set(GPIO_PIN_ADDR(5), GPIO_PIN_INT_TYPE_SET(GPIO_PIN_INTR_DISABLE)
                          | GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_DISABLE)
                          | GPIO_PIN_SOURCE_SET(GPIO_AS_PIN_SOURCE));

   GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(5));

   gpio_pin_intr_state_set(GPIO_ID_PIN(5), 5); // interrupt on high
   ETS_GPIO_INTR_ENABLE();

   PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
   GPIO_OUTPUT_SET(2, 0);

   PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
   GPIO_OUTPUT_SET(0, 0);
}




void user_init( void )
{
    CHIP_ID=system_get_chip_id();

    maxAdded = 0;
    wifi_set_opmode_current(STATION_MODE);
    wifi_set_opmode(STATION_MODE);

    wifi_station_set_auto_connect(0); // do not connect automatically thanks
    system_set_os_print(1);

    uart_div_modify( 0, UART_CLK_FREQ / ( 9600 ) );
    gpio_init();

    wifi_station_disconnect(); // disconnect as the wifi module might connect itself
    system_init_done_cb(init_done); // jump to init routine after environment is set.

}

