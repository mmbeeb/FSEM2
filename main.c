/* File Server Emulator   */
/* main.c                 */
/* (c) 2021 Martin Mather */

/* 17/09/21, some changes made to make it run in Cygwin64 */

#define _POSIX_C_SOURCE 1

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>	//sleep()
#include <time.h>
#include <sys/socket.h> //for Cygwin
#include <getopt.h>
#include <arpa/inet.h>

#include "aun.h"
#include "ebuf.h"
#include "fsem.h"

void set_no_buffer() {
	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag &= ~ICANON;
	tcsetattr(0, TCSANOW, &term);
}

int charsWaiting(int fd) {
	int count;
	
	if (ioctl(fd, FIONREAD, &count) == -1)
		exit (EXIT_FAILURE);
	
	return count;
}

int main(int argc, char *argv[]) {
	char c, skey;
	int ex = 0, rx = 0, tx = 0, loops = 0, rc, txcount = 0;
	int rxto, flg, txretry;
	time_t timeout1;
	int my_stn=254;
	in_addr_t listen_addr=INADDR_ANY;
	struct in_addr inp;
	int opt;

	while ((opt = getopt(argc, argv, "s:a:")) != -1) {
		switch (opt) {
			case 's':
				my_stn = atoi(optarg);
				break;
			case 'a':
				opt=inet_pton(AF_INET,optarg,&inp);
				listen_addr=inp.s_addr;
				break;
			default:
				fprintf(stderr, "Usage: %s [-s stn_id] [-a ip.address.]\n",argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	printf("File Server Emulator\n\n");

	if (fsem_open("$.FS", 0x0400, my_stn, "scsi1.dat")) {
		aun_open(my_stn,listen_addr);
	
		set_no_buffer();
		do {
			loops++;
			flg = 0;

			//printf("P %d\n", loops);
				
			if (!ex) {
				rc = fsem_exec(1, 0);//Execute
				//printf("RC=%d\n", rc);
				ex = 1;
				switch (rc) {
					case FSEM_WAIT0://no timeout, allow key press
						rx = 1;
						rxto = 0;
						break;
					case FSEM_WAIT2://60 second timeout
						rx = 2;
						rxto = 60;
						break;
					case FSEM_SEND:
						tx = 1;
						txretry = 0;
						txcount=10;//number of tx attempts
						break;
					default:
						rx = 0;
						ex = 0;
						break;
				}
			}
				
			if (tx > 0) {
				aun_transmitter(txretry++);
				tx = -1;
				rx = 1;//wait for ACK
				rxto = 1;//timeout seconds
			}
			
			if (rxto > 0) {//Set timeout
				//printf("SET TIMEOUT TO %d SECONDS\n", rxto);
				timeout1 = time(0) + rxto;
				rxto = -1;
			}
			
			skey = 0;
			c = 0;
			int count = charsWaiting(fileno(stdin));
			if (count != 0) {
				c = tolower(getchar());
				switch (c) {
					case 'q':
						printf("Quit\n");
						break;
					case 'x':
						printf("Enable exec\n");
						ex = 0;
						rx = 1;
						break;
					case 'z':
						printf("Disable exec\n");
						ex = 1;
						break;
					case 'm'://monitor on/off
						skey = 'M';
						break;
					case 'r'://restart
						skey = 'Q';
						break;
				}
			}
			
			if (rxto < 0) {//Check for timeout
				if (time(0) >= timeout1) {
					//printf("RX TIMEOUT\n");
					
					if (tx < 0) {//retry tx?
						if (--txcount)
							tx = 1;
						else//tx failed
							flg = 4;
					} else
						flg = 1;
				}
			}
			
			if (skey && rx != 2) {
				//printf("SKEY!\n");
				fsem_sendkey(1, skey);
				flg = 1;
			}
			
			if (rx) {//Check receiver
				if (aun_receiver(tx)) {
					if (tx < 0)
						flg = 3;//Acked
					else
						flg = 2;//We've got something
				}
			}
			
			if (flg) {
				//printf("FLAG = %d\n", flg);
				//flag set means return to emulator
				//flg==1 : timeout or key pressed, X=0x00
				//flg==2 : data received, X=0x80
				//flg==3 : transmission succeeded (ACK received)
				//flg==4 : transmission failed
				if (flg == 1)
					fsem_loadX(0x00);
				else if (flg == 2)
					fsem_loadX(0x80);
				else if (flg == 3)
					fsem_loadA(0x00);
				else
					fsem_loadA(0x40);//generic error
				
				tx = 0;//disable transmitter
				rx = 0;//disable receiver
				ex = 0;//enable execution
				rxto = 0;//stop timer
				ebuf_listen(0);//stop listening
			}

		} while (c != 'q');
	
		aun_close();
		fsem_close();
	}
}
