/* File Server Emulator   */
/* fsem.c                 */
/* (c) 2021 Martin Mather */

#include <stdio.h>
//#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "fsem.h"
#include "ebuf.h"

#define KB 1024
#define HOSTMEM 32
#define MMSIZE (64+HOSTMEM)*KB
#define MMROM 0xf800
#define SCSI_SECSIZE 0x100

#define WORD(a, l) (a[l] | (a[l+1] << 8))
#define DWORD(a, l) (a[l] | (a[l+1] << 8) | (a[l+2] << 16) | (a[l+3] << 24))
#define WWORD(a, l, m) a[l] = m; a[l+1]=m >> 8
#define WDWORD(a, l, m) a[l] = m; a[l+1]=m >> 8; a[l+2]=m >> 16; a[l+3]=m >> 24

#define GBYTE (MM[PC++])
#define GWORD (MM[PC++] | (MM[PC++] << 8))
#define STACK(l) MM[0x100 + l]
#define PUSH(m) STACK(SP--) = m
#define PULL STACK(++SP)

#define _BREAK_ brk = 1

static uint8_t MM[MMSIZE], A, X, Y, SP, M;
static uint16_t PC, XPC, L, R;
static int N, V, Z, C, F, brk = 0;

static int state = FSEM_BUSY;

static uint16_t mystn;
static struct ebuf_t *txbuf = NULL;
static FILE *scsi = NULL;

static int keyevent = 0;
#define EVENTV 0x0220


static void _mmdump(uint32_t addr, int len) {
	printf("mmdump: %08x:\n", addr);
	if (addr >= 0x10000)
		addr = (addr & 0xffff) | 0x10000;//host memory
	for (int i = 0; i < len; i++)
		printf("%02x ", MM[addr + i]);
	printf("\n");
}
 
int fsem_open(char *fname, uint32_t loadaddr, uint16_t stn, char *scsiname) {//load and run the fileserver
	printf("FSEM:Run '%s' at %04x as station %d\n", fname, loadaddr, stn);
	int result = 0;
	mystn = stn;
	FILE *fp = fopen(fname, "r");

	if (fp) {
		fseek(fp, 0, SEEK_END);
		long fsize = ftell(fp);
		uint32_t endaddr = loadaddr + fsize;
		//printf("filesize = %d, %08x\n", fsize, endaddr);

		if (endaddr >= MMROM) {
			printf("FSEM:File too large\n");
		} else  {
			//printf("Loaded to %04x to %04x\n", loadaddr, endaddr - 1);
			fseek(fp, 0, SEEK_SET);
			fread(MM + loadaddr, 1, fsize, fp);
			A = 1;	//fileserver checks if A==1 on entry, and fails if it isn't.
			SP = 0xff;
			PC = loadaddr;
			//_mmdump(loadaddr, 32);
			
			scsi = fopen(scsiname, "r+");
			if (scsi)
				result = 1;
			else
				printf("FSEM:Could not open SCSI disk image '%s'\n", scsiname);
		}

		fclose(fp);
	} else
		printf("FSEM:File not found\n");
	
	return result;
}

void fsem_close(void) {
	printf("FSEM:Close\n");
	fclose(scsi);
}

void fsem_sendkey(double optime, char key) {
	if (keyevent) {
		//printf("FSEM:Send key '%c', V=%04x\n", key, WORD(MM, EVENTV));
		uint8_t a = A, y = Y;//save registers
		uint16_t pc = PC;
		A = 2;//event 2
		Y = key;
		PC = WORD(MM, EVENTV);
		fsem_exec(optime, 1);//flags & X preserved
		PC = pc;//restore
		A = a;
		Y = y;
	}
}


static void _gettime(uint8_t *p) {
	time_t t;
	time(&t);
	struct tm *n = localtime(&t);
	//printf("time: %d/%d/%d %d:%d:%d\n", n->tm_mday, n->tm_mon+1, 1900+n->tm_year, n->tm_hour, n->tm_min, n->tm_sec);
	p[0] = n->tm_year-100;//year (0-99), assume we're in the 21st century
	p[1] = n->tm_mon+1;//month (1-12)
	p[2] = n->tm_mday;//day (0-31)
	p[3] = n->tm_wday+1;//day of week (1-7, Sun=1)
	p[4] = n->tm_hour;//hour (0-23)
	p[5] = n->tm_min;//minute (0-59)
	p[6] = n->tm_sec;//second (0-59);
}

static void _getline(uint8_t *p) {
	static int n = 1;//number of drives

	sprintf(p, "%d\r", n);
	printf("%d\n", n);

	n = 10;//number of stations

	Y = 1;
	C = 0;
}

static int _nettransmit(uint8_t *p) {
	/*printf("\n%04x TRANSMIT :\n", XPC);
	printf("cb        = %02X\n", p[0]);
	printf("dest port = %02x\n", p[1]);
	printf("dest stn  = %04x\n", WORD(p, 2));
	printf("buf start = %08x\n", DWORD(p, 4));
	printf("buf end   = %08x\n", DWORD(p, 8));
	printf("rem addr  = %08x\n", DWORD(p, 12));*/

	if (p[0] >= 0x80) {//control byte >= 0x80
		if (p[1]) {//port != 0
			uint16_t stn = WORD(p, 2);

			if (stn != 0xffff) {
				txbuf = ebuf_txfind();//ALWAYS ebufs[0]
				uint32_t start = DWORD(p, 4), end = DWORD(p, 8);
				int len = end - start;
				if (start >= 0x10000)
					start = (start & 0xffff) | 0x10000;
	
				uint8_t *t = ebuf_malloc(txbuf, len);
				memcpy(t, MM + start, len);
				txbuf->station = stn;
				txbuf->port = p[1];
				txbuf->control = p[0];
	
				txbuf->state = EB_STATE_SEND;
				return 0;//Actually want to send this.
			}//else broadcast
		}//else immediate operation indicated by control byte
	}//else malformed
	
	return -1;
}

static void _netreceive(uint8_t *p) {
	//printf("\n%04x RECEIVE :\n", XPC);
	struct ebuf_t *rxbuf;

	if (!p[0]) {//create new receive block
		rxbuf = ebuf_new();
	
		if (rxbuf) {
			uint32_t start = DWORD(p, 5), end = DWORD(p, 9);
			int len = end - start;

			rxbuf->station = WORD(p, 3);// station
			rxbuf->control = p[1];//control
			rxbuf->port = p[2];// port
			rxbuf->len = len;// max length
			rxbuf->state = EB_STATE_LISTENING;
			rxbuf->addr = start;
	
			p[0] = rxbuf->index;
			//printf("%04x new buf %d\n", XPC, rxbuf->index);
			//ebuf_list();
		} else {
			p[0] = 0;
			printf("FSEM:Couldn't create ebuf\n");
		}
	} else {// read & delete receive block
		//printf("%04x read rx block %d\n", XPC, p[0]);
		rxbuf = ebuf_x(p[0]);

		if (rxbuf) {// assume state == EB_STATE_RECEIVED
			if (rxbuf->state == EB_STATE_RECEIVED) {
				uint32_t start = rxbuf->addr, end = rxbuf->addr + rxbuf->len;
				if (start >= 0x10000)
					start = (start & 0xffff) | 0x10000;

				/*printf("start = %x\n", start);
				printf("len   = %x\n", rxbuf->len);
				printf("sa  = %x\n", rxbuf->addr);
				printf("ea  = %x\n", end);*/

				memcpy(MM + start, rxbuf->buf, rxbuf->len);

				//_mmdump(rxbuf->addr,rxbuf->len);

				p[1] = rxbuf->control;
				p[2] = rxbuf->port;
				WWORD(p, 3, rxbuf->station);
				WDWORD(p, 5, rxbuf->addr);
				WDWORD(p, 9, end);
			}

			ebuf_kill(rxbuf);
		}
	}

	/*printf("block     = %02x\n", p[0]);
	printf("cb        = %02X\n", p[1]);
	printf("to port   = %02x\n", p[2]);
	printf("from stn  = %04x\n", WORD(p, 3));
	printf("buf start = %08x\n", DWORD(p, 5));
	printf("buf end   = %08x\n", DWORD(p, 9));*/
}

static void _scsi(uint8_t *p) {
	uint32_t addr = DWORD(p, 1);
	if (addr >= 0x10000)
		addr = (addr & 0xffff) | 0x10000;//host memory
	uint8_t *datap = MM + addr;
	int result = 4, rw = 0, sec = ((p[6] & 0x1f) << 16) | (p[7] << 8) | p[8];
	int len = p[9] * SCSI_SECSIZE;
	if (!len)
		len = DWORD(p, 11);

	switch (p[5]) {
		case 0x08://read
		case 0x0a://write
			//printf("%04x SCSI read/write: cmd=%x addr=%08x sec=%06x len=%04x", XPC, p[5], addr, sec, len);
			if (!fseek(scsi, sec * SCSI_SECSIZE, SEEK_SET)) {
				if (p[5] == 0x08)
					rw = fread(datap, 1, len, scsi);
				else {
					rw = fwrite(datap, 1, len, scsi);
					//fflush(scsi);
				}
			}

			//printf(" : rw=%04x\n", rw);
			if (rw == len)
				result = 0;
			break;
		default:
			/*printf("Unknown scsi command:\n");
			printf("result    = %02x\n", p[0]);
			printf("data addr = %08x\n", DWORD(p, 1));
			printf("command   = %02x\n", p[5]);
			printf("drive     = %x\n", p[6]>>5);
			printf("sector    = %06x\n", ((p[6] & 0x1f) << 16) | (p[7] << 8) | p[8]);
			printf("sec count = %02x\n", p[9]);
			printf("?         = %02x\n", p[10]);
			printf("data len  = %08x\n", DWORD(p, 11));//valid if XY?9 = 0*/
			_BREAK_;
			break;
	}

	p[0] = result;
}

static void _oswrch(char chr) {
	int pcount[] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 5, 0, 0, 1, 9, 8, 5, 0, 0, 4, 4, 0, 2, 0};
	static int c = 0, v = 1, e = 1;
	static char queue[10];
	if (c) {
		queue[c--] = chr;//reverse order

		if (!c) {
			/*printf("ctl %d : ", queue[0]);
			for (int i = pcount[queue[0]]; i > 0; i--)
				printf("%d ", queue[i]);
			printf("\n");*/

			if (queue[0] == 28)//text window
				v = (queue[3] - queue[1]) > 2;//height, hide small windows
		}
	} else {
		if (chr < 0x20) {
			c = pcount[chr];
			queue[0] = chr;

			if (chr == 0x06)
				e = 1;//enable VDU
			else if (chr == 0x15)
				e = 0;//disable VDU
		}

		if (v && e) {
			if (!c)
				printf("%c", chr);
			//else
			//	printf("\n");
		}
	}
}

static void _osword() {
	uint8_t *p = MM + ((Y << 8) | X);//pointer to control block
	switch (A) {
		case 0x00://read line input
			//printf("get input from user max=%d buf@%04x\n", p[2], WORD(p, 0));
			_getline(MM + WORD(p, 0));
			break;
		case 0x0e://read clock
			//printf("CLOCK : %02x\n", p[0]);
			//assume function 1 and return date & time (but not in BCD)
			_gettime(p);
			break;
		case 0x10://transmit
			PULL;
			PULL;//return to calling routine
			
			if (!_nettransmit(p)) {
				state = FSEM_SEND;
				_BREAK_;
				
			} else//couldn't transmit
				fsem_loadA(0x40);//Return error!
			break;
		case 0x11://receive
			_netreceive(p);//create or read & delete RXCB
			break;
		case 0x13://read/write station info
			//printf("\n%04x STATION INFO : %02x\n", XPC, p[0]);
			//assume function 8 and return station number
			p[1] = mystn;
			break;
		case 0x72://SCSI command
			_scsi(p);
			break;
		case 0x73://SCSI last error
			//printf("\nSCSI ERROR? :\n");
			p[0]=0;//sector
			p[1]=0;
			p[2]=0;
			p[3]=0;
			p[4]=0;
			break;
		default:
			printf("\n%04x OSWORD : A=%02x X=%02x Y=%02x\n", XPC, A, X, Y);
			_BREAK_;
			break;
	}
}

static void _osbyte() {
	switch (A) {
		case 0x0d://disable event X
			keyevent = 0;//assume X=2
			break;
		case 0x0e://enable event X
			keyevent = 1;//assume X=2
			break;
		case 0x0f://flush buffer X
			break;
		case 0x32://poll transmit
			_BREAK_;//Shouldn't happen.
			break;
		case 0x33://poll receive, X=block number
			//printf("%04X POLL RCV  (BLOCK)X=%d  (WAIT)Y=%d\n", XPC, X, Y);
			
			PULL;
			PULL;//return to calling routine
	
			//WAIT TIMES:
			//Y=1 wait until something received or key pressed
			//Y=2 wait for 1 second only
			//    (Only used when expecting reply to a broadcast which will not be sent,
			//     so not implemented.)
			//Y>2 wait 1 minute
			
			//Should return with result in X:
			//X==0x00 == nothing received
			//X==0x80 == data received
			
			if (Y == 2)//No broadcast, so
				fsem_loadX(0x00);//nothing received
			else {
				ebuf_listen(X);//X=RXBN
				
				if (Y == 1)
					state = FSEM_WAIT0;
				else
					state = FSEM_WAIT2;

				_BREAK_;//STOP EXECUTING
			}
			break;
		case 0x34://delete receive control block X
			ebuf_kill(ebuf_x(X));
			break;
		case 0x35://terminate remote connection (*ROFF)
			break;
		case 0x86://return cursor position
			X = Y = 0;
			break;
		case 0x85://read bottom of display ram (HIMEM) for Mode X
			//exit YX = address
			X = 0;
			Y = HOSTMEM << 2;
			break;
		case 0x87://read character at text cursor, and screen mode
			//exit X=character, Y=mode
			break;
		case 0x96://Read SHEILA
		case 0x97://Write SHEILA
			break;
		case 0xb4://read/write OSHWM (PAGE)
			X = 0x00;//PAGE
			break;
		case 0xe5://read/write ESCAPE key status
			break;
		default:
			printf("\n%04x OSBYTE : A=%02x X=%02x Y=%02x\n", XPC, A, X, Y);
			_BREAK_;
			break;
	}
}

static int _romcall() {
	switch (PC) {
		case 0xf800://reset
			printf("RESET\n");
			_BREAK_;
			break;
		case 0xffe0://OSRDCH
			//S, A, * only
			A = 'S';
			C = 0;
			break;
		case 0xffe7://OSNEWL
			A = 13;
		case 0xffe3://OSASCI
			if (A == 13)
				_oswrch(10);
		case 0xffee://OSWRCH
			_oswrch(A);
			break;
		case 0xfff1://OSWORD
			_osword();
			break;
		case 0xfff4://OSBYTE
			_osbyte();
			break;
		case 0xfff7://OSCLI
			//printf("%04x OSCLI : X=%02x Y=%02x\n", XPC, X, Y);
			break;
		default:
			printf("%04x ROM CALL %04x\n", XPC, PC);
			_BREAK_;
			break;
	}
}

static uint8_t NZ(uint8_t M) {
	N = (M >= 0x80);
	Z = (M == 0x00);
	return M;
}

void fsem_loadA(uint8_t v) {
	A = NZ(v);
}

void fsem_loadX(uint8_t v) {
	X = NZ(v);
}

int fsem_exec(double optime, int jsr) {
	int op, i, i2, j;
	clock_t timeout = clock() + optime * CLOCKS_PER_SEC;
	
	//printf("EXEC: PC=%04x A=%02x X=%02x Y=%02x  N=%d V=%d Z=%d C=%d\n", PC, A, X, Y, N, V, Z, C);

	state = FSEM_BUSY;
	brk = 0;
	while (!brk && clock() < timeout) {
		XPC = PC;
		if (PC >= 0xf800)
			op = 0x60;//ROM, read rts
		else
			op = GBYTE;

		i = op & 0x1f;//row
		i2 = i & 0x03;
		j = op >> 5;//column

		if (i == 0 && j < 4) {//all implicit except JSR
			switch (j) {
				case 0://brk
					PC = WORD(MM, 0x0202);//BRKV
					printf("FSEM:BRK PC=%04x\n", PC);
					_BREAK_;
					break;
				case 1://jsr, absolute
					L = GWORD;
					PUSH((--PC) >> 8);
					PUSH(PC);
					PC = L;
					break;
				case 2://rti
					break;
				case 3://rts
					if (jsr)
						_BREAK_;
					else {
						PC = PULL;
						PC |= (PULL << 8);
						PC++;
					}
					break;
			}
		} else if (i == 8) {//all implicit
			switch (j) {
				case 0://php
					M = (N << 7) | (V << 6) | (Z << 1) | C;
					PUSH(M);
					break;
				case 1://plp
					M = PULL;
					N = (M & 0x80) > 0;
					V = (M & 0x40) > 0;
					Z = (M & 2) > 0;
					C = M & 1;
					break;
				case 2://pha
					PUSH(A);
					break;
				case 3://pla
					A = NZ(PULL);
					break;
				case 4://dey
					NZ(--Y);
					break;
				case 5://tay
					Y = NZ(A);
					break;
				case 6://iny
					NZ(++Y);
					break;
				case 7://inx
					NZ(++X);
					break;
			}
		} else if (i == 10 && j > 3) {//all implicit
			switch (j) {
				case 4://txa
					A = NZ(X);
					break;
				case 5://tax
					X = NZ(A);
					break;
				case 6://dex
					NZ(--X);
					break;
				case 7://nop
					break;
			}
		} else if (i == 16) {//branch, all relative
			int flag = ~j & 1;
			switch (j >> 1) {
				case 0:
					flag ^= N;
					break;
				case 1:
					flag ^= V;
					break;
				case 2:
					flag ^= C;
					break;
				case 3:
					flag ^= Z;
					break;
			}

			M = GBYTE;

			if (flag) {
				if (M & 0x80)
					PC -= 0x100;
				PC += M;
			}
		} else if (i == 24) {//all implicit
			switch (j) {
				case 0://clc
					C = 0;
					break;
				case 1://sec
					C = 1;
					break;
				case 2://cli
					break;
				case 3://sei
					break;
				case 4://tya
					A = NZ(Y);
					break;
				case 5://clv
					V = 0;
					break;
				case 6://cld
					break;
				case 7://sed
					break;
			}
		} else if (i == 26) {//all implicit
			if (j & 1)//tsx
				X = NZ(SP);
			else//txs
				SP = X;
		} else {//multiple address modes
			int acc = 0, imm = 0, store = 0;

			if (i == 10) {
				acc = 1;
			} else {
				if (i == 1) {//(zp,X)
					L = GBYTE + X;
					L = WORD(MM, L);
				} else if (i == 25 || op == 190) {//LDX abs,Y
					L = GWORD + Y;
				} else {
					switch (i >> 2) {
						case 0://#
						case 2://#
							imm = 1;
							break;
						case 1://zp
							L = GBYTE;
							break;
						case 3://abs
							L = GWORD;
							break;
						case 4://(zp),Y
							L = GBYTE;
							L = WORD(MM, L) + Y;
							break;
						case 5:
							if (op == 150)//STX zp,Y
								L = GBYTE + Y;
							else//zp,X
								L = GBYTE + X;
							break;
						case 7://abs,X
							L = GWORD + X;
							break;
					}
				}
			}

			if (acc)
				M = A;
			else if (imm)
				M = GBYTE;
			else
				M = MM[L];

			switch (i2) {
				case 0:
					switch (j) {
						case 1://bit
							NZ(A & M);
							V = (M & 0x40) > 0;
							N = (M & 0x80) > 0;
							break;
						case 2://jmp absolute
							PC = L;
							break;
						case 3://jmp indirect
							PC = WORD(MM, L);
							break;
						case 4://sty
							M = Y;
							store = 1;
							break;
						case 5://ldy
							Y = NZ(M);
							break;
						case 6://cpy
							NZ(Y-M);
							C = (Y >= M);
							break;
						case 7://cpx
							NZ(X-M);
							C = (X >= M);
							break;
					}
					break;
				case 1:
					switch (j) {
						case 0://ora
							A = NZ(A | M);
							break;
						case 1://and
							A = NZ(A & M);
							break;
						case 2://eor
							A = NZ(A ^ M);
							break;
						case 7://sbc
							M = ~M;
						case 3://adc
							R = A + M + C;
							C = (R >= 0x100);
							V = ((A^R) & (M^R) & 0x80) > 0;
							A = NZ(R);
							break;
						case 4://sta
							M = A;
							store = 1;
							break;
						case 5://lda
							A = NZ(M);
							break;
						case 6://cmp
							NZ(A-M);
							C = (A >= M);
							break;
					}
					break;
				case 2:
					store = 1;
					switch (j) {
						case 0://asl
							C = 0;
						case 1://rol
							F = (M & 0x80) > 0;
							M = NZ((M << 1) | C);
							C = F;
							break;
						case 2://lsr
							C = 0;
						case 3://ror
							F = M & 1;
							M = NZ((M >> 1) | (C << 7));
							C = F;
							break;
						case 4://stx
							M = X;
							break;
						case 5://ldx
							X = NZ(M);
							store = 0;
							break;
						case 6://dec
							NZ(--M);
							break;
						case 7://inc
							NZ(++M);
							break;
					}
					break;
			}

			if (store) {
				if (acc)
					A = M;
				else
					MM[L] = M;
			}
		}

		if (PC >= 0xf800)//ROM CALL
			_romcall();
	}

	if (!jsr && brk && state == FSEM_BUSY) {
		printf("%04x BREAK ***************************\n", PC);
		state = FSEM_BREAK;
	}
	
	return state;
}

