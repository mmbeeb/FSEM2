/* File Server Emulator   */
/* aun.c                  */
/* (c) 2021 Martin Mather */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>//timeval
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <time.h>

#include "aun.h"
#include "ebuf.h"

static struct aun_t {
	uint32_t in_addr;
	struct sockaddr_in si;
	uint32_t rxhandle;
	time_t rxtime;
	uint32_t txhandle;
} stations[AUN_MAX_STATIONS], *stnp, *stntx;

static struct sockaddr_in si_me, si_other;
static int mysock, slen = sizeof(si_other), rxlen;
static uint8_t *rxbuf = NULL;
static uint16_t mystn, otherstn;
static uint8_t riscos_mode = 0;

static void die(char *s) {
	perror(s);
	exit(1);
}

static void _opensock(in_addr_t listen_addr) {
	//create a UDP socket
	if ((mysock=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		die("socket");
	
	//zero out the structure
	memset((char *) &si_me, 0, sizeof(si_me));
	
	si_me.sin_family = AF_INET;
	if (listen_addr == INADDR_ANY)
		si_me.sin_port = htons(AUN_PORT_BASE + mystn);
	else
	{
		si_me.sin_port = htons(32768);
		riscos_mode = 1;
	}
	si_me.sin_addr.s_addr = listen_addr;
	
	//bind socket to port
	if (bind(mysock, (struct sockaddr*) &si_me, sizeof(si_me)) == -1)
		die("bind");

	//set receive timeout
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 1000000/3;
	
	if (setsockopt(mysock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
		die("set sock opt");
}

static void _sendack(void) {
	rxbuf[0] = AUN_TYPE_ACK;//reuse rest of received header
	if (sendto(mysock, rxbuf, AUN_HDR_SIZE, 0, (struct sockaddr*) &si_other, slen) == -1)
		die("sendto()");
	//printf("ACK SENT\n");
}

static void _sendmachinetype(void) {
	//printf("send immediate reply ack\n");
	rxbuf[0] = AUN_TYPE_IMM_REPLY;//reuse rest of received header
	rxbuf[8] = 1;//bbc micro
	rxbuf[9] = 0;
	rxbuf[10] = 0x60;//nfs x.60
	rxbuf[11] = 3;//nfs 3.xx
	if (sendto(mysock, rxbuf, AUN_HDR_SIZE + 4, 0, (struct sockaddr*) &si_other, slen) == -1)
		die("sendto()");
}

static int _gotdata(int ackwait) {
	uint8_t port;
	uint32_t handle;
	int received = 0;

	//printf("ip=%08x\n", stnp->in_addr);
	//for (int i = 0; i < rxlen; i++)
	//	printf("%02x ", rxbuf[i]);
	//printf("\n");

	port = rxbuf[1];
	handle = rxbuf[7] << 24 | rxbuf[6] << 16 | rxbuf[5] << 8 | rxbuf[4];

	//printf("AUN:RX type=%02x port=%02x cb=%02x handle=%08x\n", rxbuf[0], port, rxbuf[2], handle);

	switch (rxbuf[0]) {// type
		case AUN_TYPE_UNICAST:
			//printf("UNICAST\n");
			if (!ackwait) {
				if ((handle > stnp->rxhandle) || (time(0) > stnp->rxtime)) {
					stnp->rxhandle = handle;
					stnp->rxtime = time(0) + AUN_RXTIMEOUT;
			
					struct ebuf_t *p = ebuf_rxfind(otherstn, port);
					if (p) {
						//printf("ebuf %d found\n", p->index);

						if (rxlen <= (p->len + AUN_HDR_SIZE)) {
							_sendack();// do this first
							p->station = otherstn;//from station
							p->port = port;//to port
							p->control = rxbuf[2] | 0x80;//control byte
							ebuf_bind(p, rxbuf, rxlen);
							p->state = EB_STATE_RECEIVED;
							
							received = 1;// WE RECEIVED DATA
							rxbuf = NULL;
						} else
							printf("AUN:buffer too small\n");
					} else
						printf("AUN:ebuf not found\n");
				} else if (handle == stnp->rxhandle) {
					_sendack();//duplicate of last packet, send ack
				}// else duplicate of old packet, ignore
			} // else we're waiting for an ACK
			break;
		case AUN_TYPE_ACK:
			//printf("ACK RECEIVED\n");
			if (ackwait) {
				if (stnp == stntx && handle == stnp->txhandle)
					received = 1;// WE RECEIVED AN ACK TO LAST TRANSMISSION
			}// else not expecting an ACK
			break;
		case AUN_TYPE_IMMEDIATE:
			//printf("IMM %02x : ", rxbuf[2]);//control byte
			//for (int i = 8; i < rxlen && i < 32; i++)
			//	printf("%02x ", rxbuf[i]);
			//printf("\n");
			switch (rxbuf[2]) {//control byte
				case ECONET_MACHINEPEEK:
					//printf("MACHINE PEEK\n");
					_sendmachinetype();
					break;
				default:
					//printf("Unhandled\n");
					break;
			}
			break;
		case AUN_TYPE_IMM_REPLY:
		default:
			//printf("Type?\n");
			break;
	}
	
	return received;
}

int aun_receiver(int ackwait) {
	
	int received = 0;
	
	if (!rxbuf)
		rxbuf = malloc(AUN_RXBUFLEN);

	if (rxbuf)
		rxlen = recvfrom(mysock, rxbuf, AUN_RXBUFLEN, 0, (struct sockaddr *) &si_other, &slen);
	else
		rxlen = 0;

	if (rxlen == -1) {
		if (errno != EWOULDBLOCK) 
			die("recvfrom()");
	} else if (rxlen >= AUN_HDR_SIZE) {
		//printf("Received packet from %s:%d length=%d\n", inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port), rxlen);
		if (riscos_mode && ntohs(si_other.sin_port) == 32768)
			otherstn = ntohl(si_other.sin_addr.s_addr) & 255;
		else
			otherstn = ntohs(si_other.sin_port) - AUN_PORT_BASE;
		//printf("stn=%d\n", otherstn);
		if (otherstn < AUN_MAX_STATIONS) {
			if (otherstn == mystn) 
				printf("AUN:Duplicate station %d\n", otherstn);
			else {
				stnp = &stations[otherstn];
				uint32_t in_addr = ntohl(si_other.sin_addr.s_addr);
				if (stnp->in_addr == 0) {
					//printf("New station\n");
					stnp->in_addr = in_addr;
					stnp->si = si_other;
				}
				
				if (stnp->in_addr != in_addr)
					printf("AUN:Duplicate station %d\n", otherstn);
				else {
					//printf("Station OK\n");
					received = _gotdata(ackwait);
				}
			}
		} else
			printf("AUN:Station number out of range\n");
	}
	
	return received;
}


int aun_transmitter(int retry) {
	struct ebuf_t *p = ebuf_txfind();//ALWAYS ebufs[0]
	int send = 0;

	//printf("tx %d %d %04x %02x\n", p->index, p->state, p->station, p->port);
	if (p->station < AUN_MAX_STATIONS) {
		stntx = &stations[p->station]; 

		if (stntx->in_addr) {
				//pop AUN header
				if (!retry)
					stntx->txhandle += 4;

				uint8_t *hdr = p->buf2;
				uint32_t handle = stntx->txhandle;

				hdr[0] = AUN_TYPE_UNICAST;
				hdr[1] = p->port;
				hdr[2] = p->control & 0x7f;
				hdr[3] = 0;
				hdr[4] = handle & 0xff;
				hdr[5] = (handle >> 8) & 0xff;
				hdr[6] = (handle >> 16) & 0xff;
				hdr[7] = (handle >> 24) & 0xff;

				//printf("AUN:TX type=%02x port=%02x cb=%02x handle=%08x\n", hdr[0], p->port, hdr[2], handle);
				send = 1;
		} else	{
			printf("TX: stn doesn't have an ip address!\n");
		}

		if (send) {
			//printf("TX: Sending packet to %s:%d length=%d, handle=%08x\n", 
			//		inet_ntoa(stntx->si.sin_addr), ntohs(stntx->si.sin_port), p->len2, stntx->txhandle);
			if (sendto(mysock, p->buf2, p->len2, 0, (struct sockaddr*) &stntx->si, slen) == -1)
				die("sendto()");
		}
	} //else station number out of range
	
	return send;
}


int aun_open(uint16_t stn, in_addr_t listen_addr) {
	//printf("aun_open stn=%d\n", stn);

	mystn = stn;	// remember my station number
	ebuf_open(AUN_MAX_BUFFERS);
	_opensock(listen_addr);
}

int aun_close(void) {
	//printf("aun_close\n");
	close(mysock);
	ebuf_close();
}


