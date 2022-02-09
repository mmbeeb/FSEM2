/* File Server Emulator   */
/* fsem.h                 */
/* (c) 2021 Martin Mather */

#define FSEM_BUSY		0
#define FSEM_BREAK		1

#define FSEM_WAIT		2

#define FSEM_WAIT0		10//Wait until rx or key pressed
#define FSEM_WAIT1		11//Wait for 1 second
#define FSEM_WAIT2		12//Wait for 60 seconds

#define FSEM_SEND		20//Send something!

int fsem_open(char *fname, uint32_t loadaddr, uint16_t stn, char *scsiname);
void fsem_close(void);
void fsem_sendkey(double optime, char key);
int fsem_exec(double optime, int jsr);
void fsem_loadX(uint8_t v);
void fsem_loadA(uint8_t v);