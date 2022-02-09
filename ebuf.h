/* File Server Emulator   */
/* ebuf.h                 */
/* (c) 2021 Martin Mather */

#define EB_STATE_VOID		0
#define EB_STATE_LISTENING	1
#define EB_STATE_RECEIVED	2
#define EB_STATE_SEND		3
#define EB_STATE_SENDING	4
#define EB_STATE_SEND2		5
#define EB_STATE_SENDING2	6
#define EB_STATE_SENT		7

#define EB_RESULT_SUCCESSFUL	0
#define EB_RESULT_TIMEDOUT	1
#define EB_RESULT_OTHER		2

struct ebuf_t {
	int index;
	int state;
	int result;
	uint16_t station;
	int port;
	uint8_t *buf, *buf2;
	int len, len2;

	uint8_t control;//Econet TX control byte
	uint32_t addr;//in Beeb memory
};


void ebuf_listen(int x);
void ebuf_open(int max_buffers);
void ebuf_close(void);
struct ebuf_t *ebuf_new(void);
struct ebuf_t *ebuf_x(int x);
struct ebuf_t *ebuf_rxfind(uint16_t stn, int port);
struct ebuf_t *ebuf_txfind(void);
void ebuf_bind(struct ebuf_t *p, uint8_t *buf, int len);
uint8_t *ebuf_malloc(struct ebuf_t *p, int len);
void ebuf_kill(struct ebuf_t *p);
void ebuf_print(struct ebuf_t *p);
void ebuf_list(void);