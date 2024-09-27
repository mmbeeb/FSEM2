/* File Server Emulator   */
/* aun.h                  */
/* (c) 2021 Martin Mather */

#define AUN_MAX_STATIONS	255
#define AUN_MAX_BUFFERS	5
#define AUN_PORT_BASE	10000

#define AUN_RXBUFLEN	2048	// max length of receive buffer
#define AUN_RXTIMEOUT	5		// seconds

#define AUN_HDR_SIZE	8

//#define AUN_TYPE_BROADCAST	1
#define AUN_TYPE_UNICAST	2
#define AUN_TYPE_ACK		3
//#define AUN_TYPE_NACK		4
#define AUN_TYPE_IMMEDIATE	5
#define AUN_TYPE_IMM_REPLY	6

#define ECONET_MACHINEPEEK	8

#include <netinet/in.h>

int aun_open(uint16_t stn, in_addr_t listen_addr);
int aun_close(void);
int aun_receiver(int ackwait);
int aun_transmitter(int retry);

