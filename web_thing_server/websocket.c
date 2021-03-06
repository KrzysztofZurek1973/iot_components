/*
 * websocket.c
 *
 *  This file is a part of the "Simple Web Thing Server" project
 *  Created on: Jul 1, 2019
 *  Last edit:	Feb 19, 2021
 *      Author: Krzysztof Zurek
 *		e-mail: krzzurek@gmail.com
 */
#include <stdio.h>
#include <sys/param.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"

//for mdns announcement triggering
#include "mdns.h"
#include "lwip/api.h"

#include "websocket.h"
#include "simple_web_thing_server.h"
#include "common.h"

#define MAX_PAYLOAD_LEN			1024
#define SHA1_RES_LEN			20	//sha1 result length
#define CLOSE_TIMEOUT_MS		5000 //ms
#define CLOSE_TIMEOUT_MS_SHORT	2000 //ms
#define WS_MAX_ERRORS			5

//global server variables
static int8_t ws_server_is_running = 0;
static uint16_t ws_port = 0;
xQueueHandle ws_output_queue;

//websocket task functions
static void ws_send_task(void* arg);
static uint8_t head_buff[MAX_PAYLOAD_LEN + 4]; //sending buffer

//functions prototypes
void add_ws_header(ws_queue_item_t *q, ws_send_data *ws_data);
int8_t ws_close(connection_desc_t *conn_desc);
int8_t ws_handshake(char *rq, connection_desc_t *conn_desc, ws_queue_item_t *ws_item);
void vCloseTimeoutCallback(TimerHandle_t xTimer);
int8_t set_property(char *rq, thing_t *t, uint16_t tcp_len);
int8_t run_action(char *rq, thing_t *t, uint16_t tcp_len);
int8_t event_subscribe(char *rq, thing_t *t, uint16_t tcp_len);
int8_t parse_ws_request(char *rq, uint16_t len, connection_desc_t *conn);

// This is the data from the busy server
//static char error_busy_page[] =
//		"HTTP/1.1 503 Service Unavailable\r\n\r\n";
//handshake strings
const char ws_sec_key[] = "Sec-WebSocket-Key";
const char ws_upgrade[] = "Upgrade: websocket";
const char ws_conn_1[] = "Connection: Upgrade";
const char ws_conn_2[] = "Connection: keep-alive, Upgrade";
const char ws_conn_3[] = "Sec-WebSocket-Protocol: webthing";
const char ws_ver[] = "Sec-WebSocket-Version: 13";
const char ws_sec_conKey[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const char ws_server_hs[] = "HTTP/1.1 101 Switching Protocols\r\n"\
							"Upgrade: websocket\r\n"\
							"Connection: Upgrade\r\n"\
							"Sec-WebSocket-Accept: %s\r\n"\
							"%s"\
							"Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
const char ws_hs_subpro[] = "Sec-WebSocket-Protocol: webthing\r\n";

extern connection_desc_t connection_tab[];


/*************************************************************************
 *
 * parse websocket request
 *
 * ***********************************************************************/
int8_t parse_ws_request(char *rq, uint16_t len, connection_desc_t *conn){
	int8_t res = 0;

	if (strstr((char *)rq, "setProperty")){
		res = set_property(rq, conn -> thing, len);
	}
	else if (strstr((char *)rq, "requestAction")){
		res = run_action(rq, conn -> thing, len);
	}
	else if (strstr((char *)rq, "addEventSubscription")){
		res = event_subscribe(rq, conn -> thing, len);
	}

	return res;
}


/*******************************************************************
 *
 * run action requested by websocket API
 *
 ****************************************************************** */
int8_t event_subscribe(char *rq, thing_t *t, uint16_t tcp_len){
	int8_t res = 0;
	
	//ignore this message, event info is send to all thing's subscribers
	printf("event subscribe:\n%s\n", rq);
	
	return res;
}


/*******************************************************************
 *
 * run action requested by websocket API
 *
 ****************************************************************** */
int8_t run_action(char *rq, thing_t *t, uint16_t tcp_len){
	int8_t res = 0;
	char *action_id = NULL, *inputs = NULL, *ptr_1, *ptr_2;

	//get action name
	ptr_1 = strstr(rq, "\"data\":");
	if (ptr_1 == NULL){
		goto run_action_end;
	}
	ptr_1 = strchr(ptr_1, '{');
	if (ptr_1 == NULL){
		goto run_action_end;
	}
	ptr_1 = strchr(ptr_1, '"');
	if (ptr_1 == NULL){
		goto run_action_end;
	}
	ptr_2 = strchr(ptr_1 + 1, '"');
	if (ptr_2 == NULL){
		goto run_action_end;
	}
	int len = ptr_2 - ptr_1;
	if (len > 0){
		action_id = malloc(len);
		memset(action_id, 0, len);
		memcpy(action_id, ptr_1 + 1, len - 1);
	}
	//get inputs
	ptr_1 = strchr(ptr_2, '{');
	if (ptr_1 == NULL){
		goto run_action_end;
	}
	ptr_2 = strchr(ptr_1, '}');
	if (ptr_2 == NULL){
		goto run_action_end;
	}
	len = ptr_2 - ptr_1;
	if (len > 0){
		inputs = malloc(len);
		memset(inputs, 0, len);
		memcpy(inputs, ptr_1 + 1, len - 1);
	}

	if ((action_id != NULL) && (inputs != NULL)){
		request_action(t -> thing_nr, action_id, inputs);
	}

	run_action_end:
	free(action_id);
	free(inputs);

	return res;
}


/*******************************************************************
 *
 *set property value by websocket API
 *
 ****************************************************************** */
int8_t set_property(char *rq, thing_t *t, uint16_t tcp_len){
	char *name_start, *name_end, *ptr_start, *ptr_end, c;
	int len = 0;
	int16_t square = 0, curly = 0, quotation = 0, counter = 0, cnt = 0;
	uint8_t go_out = 0;
	uint8_t end_of_item = 0;
	char *name_str = NULL, *value_str = NULL;
	int8_t out = 0;

	ptr_end = strstr(rq, "\"data\":{");
	ptr_end = strchr(ptr_end, '{');
	ptr_start = ptr_end;
	name_start = NULL;
	name_end = NULL;

	while (go_out == 0){
		c = *(ptr_end++);
		switch (c){
		case '[':
			square++;
			break;
		case ']':
			square--;
			break;
		case '{':
			curly++;
			break;
		case '}':
			curly--;
			if (curly == 0){
				end_of_item = 1;
			}
			else if (curly < 0){
				go_out = 1;
			}
			break;
		case '"':
			quotation++;
			if (name_start == NULL){
				name_start = ptr_end;
			}
			else if (name_end == NULL){
				name_end = ptr_end - 1;
			}
			break;
		case ',':
			if ((square == 0) && (curly == 1)){
				end_of_item = 1;
			}
			break;
		case ':':
			if (curly == 1){
				ptr_start = ptr_end;
			}
			break;
		default:
			cnt++;
			if (cnt > tcp_len){
				go_out = 1;
				out = -1;
			}
		}

		if (end_of_item == 1){
			end_of_item = 0;
			counter++;
			//copy name into buffer
			len = name_end - name_start;
			name_str = malloc(len + 1);
			memset(name_str, 0, len + 1);
			memcpy(name_str, name_start, len);
			//copy value into buffer
			len = ptr_end - ptr_start - 1;
			value_str = malloc(len + 1);
			memset(value_str, 0, len + 1);
			memcpy(value_str, ptr_start,len);
			set_resource_value(t -> thing_nr, name_str, value_str);
			free(value_str);
			free(name_str);
			name_start = NULL;
			name_end = NULL;
		}
	}
	return out;
}

/*************************************************************************
 *
 * websocket receive function
 * includes: handshake, open, close, receive data
 *
 * ***********************************************************************/
int8_t ws_receive(char *rq, uint16_t tcp_len, connection_desc_t *conn_desc){
	int msg_ok = 0, msg_start = 0;
	uint16_t ws_len = 0;
	uint8_t *msg = NULL;
	ws_frame_header_t *ws_header;
	uint8_t masking_key[4];
	uint8_t offset = 2, mask = 0, finish;
	WS_OPCODES opcode;
	ws_queue_item_t *ws_item;
	int8_t res = 0;
	bool free_msg = true;

	opcode = 0;
	msg_ok = 0;

	if (conn_desc -> ws_state == WS_CLOSING){
		//ignore received messages in CLOSING state
		goto ws_receive_end;
	}

	//read data from input buffer
	if (conn_desc -> ws_state != WS_CLOSED){
		//receive websocket data from client
		offset = 0;
		if (msg_start == 0){
			//start of the message
			ws_header = (ws_frame_header_t *)rq;
			ws_len = ws_header -> payload_len;
			opcode = ws_header -> opcode;
			
			//TEST
			//printf("ws opcode: %i, ID: %i, ping: %i\n", 
			//		opcode, conn_desc -> index, conn_desc -> ws_pongs); //test
			//TEST
			
			finish = ws_header -> fin;
			if (finish == 0x0){
				//fragmentation not supported
				printf("websocket: fragmentation not supported\n");
				conn_desc -> ws_close_initiator = WS_CLOSE_BY_SERVER;
				conn_desc -> ws_status_code = DATA_INCONSIST;
				ws_close(conn_desc);
				return -1;
			}
			offset = 2;
			if (ws_len == 126){
				//message length are bytes 2 and 3
				ws_len = (rq[offset] << 8) + rq[offset + 1];
				offset = 4;
				if (ws_len > MAX_PAYLOAD_LEN){
					printf("websocket: message too long\n");
					conn_desc -> ws_close_initiator = WS_CLOSE_BY_SERVER;
					conn_desc -> ws_status_code = DATA_TO_BIG;
					ws_close(conn_desc);
					return -1;
				}
			}
			else if (ws_len == 127){
				//64bit addresses are not supported
				printf("websocket: 64bit addresses are not supported\n");
				conn_desc -> ws_close_initiator = WS_CLOSE_BY_SERVER;
				conn_desc -> ws_status_code = DATA_TO_BIG;
				ws_close(conn_desc);
				return -1;
			}
			mask = ws_header -> mask;
			if (mask == 0x1){
				masking_key[0] = rq[offset];
				masking_key[1] = rq[offset + 1];
				masking_key[2] = rq[offset + 2];
				masking_key[3] = rq[offset + 3];
				offset += 4;
			}
			//allocate memory for message
			if (ws_len > 0){
				msg = malloc(ws_len + 1);
			}
		}
		if (msg != NULL){
			//copy data to buffer
			int msg_start_temp = msg_start + tcp_len - offset;
			
			if (msg_start_temp <= ws_len){
				memcpy(msg + msg_start, rq + offset, tcp_len - offset);
				msg_start += tcp_len - offset;
				if (msg_start == ws_len){
					//all data received, unmask data
					if ((mask == 1) && (ws_len > 0)){
						//unmask data
						for (int i = 0; i < ws_len; i++){
							msg[i] = msg[i] ^ masking_key[i%4];
						}
					}
					msg_ok = 1;
					msg[ws_len] = 0;
					msg_start = 0;
				}
			}
			else {
				//message length error, close connection
				printf("msg length error, %i, %i\nmsg:\n%s\n", msg_start, ws_len, msg);
				conn_desc -> ws_close_initiator = WS_CLOSE_BY_SERVER;
				conn_desc -> ws_status_code = SERVER_ERR;
				ws_close(conn_desc);
				free(msg);
				return -1;
			}
		}
		else{
			if ((ws_len == 0) && (opcode == WS_OP_PIN || opcode == WS_OP_CLS)){
				//ping messages
				msg_ok = 1;
			}
		}
	}

	//collect message, check it
	switch (conn_desc -> ws_state){
	case WS_OPEN:
		if (msg_ok == 1){
			switch(opcode){
			case WS_OP_TXT:
			case WS_OP_BIN:
				//client data received
				parse_ws_request((char *)msg, tcp_len, conn_desc);
				break;
			case WS_OP_CLS:
				//close connection
				delete_subscriber(conn_desc);
				conn_desc -> ws_close_initiator = WS_CLOSE_BY_CLIENT;
				if (ws_len > 0){
					conn_desc -> ws_status_code = (msg[0] << 8) + msg[1];
					ws_close(conn_desc);
				}
				else{
					conn_desc -> ws_status_code = 0;
					ws_close(conn_desc);
				}
				break;
			case WS_OP_PIN:
				//ping control frame, answer with "pong"
				ws_item = malloc(sizeof(ws_queue_item_t));
				if (ws_len == 0){
					ws_item -> payload = NULL;
				}
				else{
					ws_item -> payload = msg;
				}
				ws_item -> len = ws_len;
				ws_item -> conn_desc = conn_desc;
				ws_item -> opcode = WS_OP_PON;
				ws_item -> ws_frame = 0x1;
				ws_item -> text = 0x0;
				free_msg = false;

				xSemaphoreTake(conn_desc -> mutex, portMAX_DELAY);
				conn_desc -> msg_to_send++;
				xSemaphoreGive(conn_desc -> mutex);
				xQueueSend(ws_output_queue, &ws_item, portMAX_DELAY);
				break;
			case WS_OP_PON:
				//conn_desc -> ws_pongs++;
				break;
			case WS_OP_CON:
			default:
				//TODO: what to do if happen?
				printf("incorrect opcode received: %X\n", opcode);
				conn_desc -> ws_close_initiator = WS_CLOSE_BY_SERVER;
				conn_desc -> ws_status_code = POLICY_ERR;
				ws_close(conn_desc);
				break;
			}
		}
		break;

	case WS_CLOSED:
		//check if request was 'GET /\r\n'
		if(rq[0] == 'G' && rq[1] == 'E' && rq[2] == 'T'
				&& rq[3] == ' ' && rq[4] == '/') {
			ws_item = malloc(sizeof(ws_queue_item_t));

			if (ws_item != NULL){
				uint8_t res = ws_handshake(rq, conn_desc, ws_item);
				if (res == 1){
					xSemaphoreTake(conn_desc -> mutex, portMAX_DELAY);
					conn_desc -> msg_to_send++;
					xSemaphoreGive(conn_desc -> mutex);
					
					xQueueSendToFront(ws_output_queue, &ws_item, portMAX_DELAY);

					//get thing number from url
					char buff_nr[3];
					char *c1, *c2, *b1;
					uint8_t thing_nr = 0, len;

					c2 = strstr(rq, "HTTP");
					c1 = strstr(rq, "://");
					if (c1 != NULL){
						b1 = c1 + 3;
					}
					else{
						b1 = rq;
					}
					c1 = strchr(b1, '/');
					len = c2 - c1 - 1;
					if (len <= 4){
						memset(buff_nr, 0, 3);
						memcpy(buff_nr, c1 + 1, len - 1);
						thing_nr = atoi(buff_nr);
						conn_desc -> thing = get_thing_ptr(thing_nr);
						add_subscriber(conn_desc);
					}
					else{
						conn_desc -> connection = CONN_WS_CLOSE;
						printf("Thing number ERROR in handshake URL\n");
					}
				}
				else{
					printf("ws_handshake returned error\n");
				}
			}
			else{
				printf("handshake, no heap memory\n");
			}
		}
		else{
			//not correct handshake request, close connection
			conn_desc -> connection = CONN_WS_CLOSE;
			printf("ERROR: bad http request at handshake\n%s\n", rq);
		}
		break;
	case WS_OPENING:
		//should not happen
		printf("ws state is OPENING, received opcode = %X, msg = %s\n", opcode, msg);
		break;
	case WS_CLOSING:
		if (opcode == WS_OP_CLS){
			printf("client answer on close frame, close code = %i", (msg[0] << 8) + msg[1]);
			conn_desc -> connection = CONN_WS_CLOSE;
			//TODO: if this is not answer for server's CLOSE, but client's first
			//CLOSE frame, then client is waiting for server's CLOSE
		}
		else{
			printf("state CLOSING, incorrect ws frame, opcode = %X\n", opcode);
		}
		//ignore other opcodes
		break;
	default:
		conn_desc -> connection = CONN_WS_CLOSE;
	}//switch(ws_state)

ws_receive_end:	
	if (free_msg == true){
		free(msg);
	}

	return res;
}


// ***************************************************************************
int8_t ws_handshake(char *rq, connection_desc_t *conn_desc, ws_queue_item_t *ws_item){
	uint8_t msg_flags = 0;
	int8_t ret;
	char *server_ans;
	char *res1, *res2;
	bool sub_pro = false;

	server_ans = NULL;

	//upgrade
	if (strstr((char *)rq, ws_upgrade)){
		msg_flags |= 0x01;
	}
	//connection
	if (strstr((char *)rq, ws_conn_1)){
		msg_flags |= 0x02;
	}
	else if (strstr((char *)rq, ws_conn_2)){
		msg_flags |= 0x02;
	}
	//ver
	if (strstr((char *)rq, ws_ver)){
		msg_flags |= 0x04;
	}
	//subprotocol
	if (strstr((char *)rq, ws_conn_3)){
		sub_pro = true;
	}
	if (msg_flags == 0x07){
		size_t  out_len;

		res1 = strstr((char *)rq, ws_sec_key);
		if (res1 != NULL){
			msg_flags |= 0x08;

			res2 = strstr(res1, ": ");
			res1 = strstr(res2, "\r\n");
			char *buff_1 = malloc(80);
			memset(buff_1, 0, 80);
			memcpy(buff_1, res2 + 2, res1 - res2 - 2);

			//concatenate websocket GUID
			strcpy((char *)&buff_1[res1 - res2 - 2], ws_sec_conKey);
			int buff_1_len = res1 - res2 - 2 + strlen(ws_sec_conKey);
			buff_1[buff_1_len] = 0;

			char *buff_2 = malloc(20);
			mbedtls_sha1_ret((unsigned char*)buff_1,
							  buff_1_len,
							 (unsigned char*)buff_2);
			free(buff_1);

			out_len = 4 + SHA1_RES_LEN*4/3 + 1;
			char *buff_3 = malloc(out_len);
			size_t olen;
			mbedtls_base64_encode((unsigned char *)buff_3,
								   out_len, &olen,
								  (unsigned char *)buff_2,
								   SHA1_RES_LEN);
			free(buff_2);

			//prepare server answer
			server_ans = malloc(olen + strlen(ws_server_hs) + 10);
			if (sub_pro == false){
				sprintf(server_ans, ws_server_hs, buff_3, "");
			}
			else{
				sprintf(server_ans, ws_server_hs, buff_3, ws_conn_3);
			}
			
			free(buff_3);
		}
	}

	//send answer to the client
	if ((msg_flags == 0x0F) && (server_ans != NULL)){

		conn_desc -> ws_state = WS_OPENING;

		ws_item -> payload = (uint8_t *)server_ans;
		ws_item -> len = strlen(server_ans);
		ws_item -> opcode = 0;
		ws_item -> ws_frame = 0;
		//ws_item -> index = conn_desc -> index;
		ws_item -> conn_desc = conn_desc;
		//printf("data length = %i\n", ws_item -> len);
		ret = 1;
	}
	else{
		ret = -1;
		printf("ws_handshake error, msg_flags = %X\n", msg_flags);
	}
	return ret;
}


//create and start time-out timer for ending connection 
int8_t create_connection_timeout(connection_desc_t *conn_desc){
	TimerHandle_t timeout_timer;
	int32_t timeout;
	
	if (conn_desc -> timer == NULL){

		if (conn_desc -> ws_close_initiator == WS_CLOSE_BY_CLIENT){
			//close initiated by client, only 1 CLS frame must be sent
			timeout = CLOSE_TIMEOUT_MS_SHORT;
		}
		else if (conn_desc -> ws_close_initiator == WS_CLOSE_BY_SERVER){
			//close initiated by server, 1 CLS frame will be sent 
			//and 1 frame should be received from client, more time needed
			timeout = CLOSE_TIMEOUT_MS;
		}
		else {
			//should not happen
			return -1;
		}
	
		timeout_timer = xTimerCreate("timeout", 
									pdMS_TO_TICKS(timeout),
									pdFALSE, 
									(void *)&(conn_desc -> index),
									vCloseTimeoutCallback);
	
		//start timer
		BaseType_t res = xTimerStart(timeout_timer, 5);
		if (res == pdPASS) {
			conn_desc -> timer = timeout_timer;
			return 1;
		}
		else {
			conn_desc -> timer = NULL;
			xTimerDelete(timeout_timer, 10);
			return -1;
		}
	}
	else {
		return -1;
	}
}

// ****************************************************************************
//close websocket
int8_t ws_close(connection_desc_t *conn_desc){
	char *payload;
	ws_queue_item_t *ws_item;
	int16_t len;
	uint16_t cls_status;

	if (conn_desc -> ws_state == WS_CLOSING){
		return -1;
	}
	
	cls_status = conn_desc -> ws_status_code;
	conn_desc -> ws_state = WS_CLOSING;

	//prepare close frame with close code
	if (cls_status == 0){
		len = 0;
		payload = NULL;
	}
	else{
		len = 2;
		payload = malloc(2);
		payload[0] = cls_status >> 8; //network byte order
		payload[1] = cls_status;
	}

	ws_item = malloc(sizeof(ws_queue_item_t));
	ws_item -> payload = (uint8_t *)payload;
	ws_item -> len = len;
	ws_item -> opcode = WS_OP_CLS; //close
	ws_item -> ws_frame = 0x1;
	ws_item -> conn_desc = conn_desc;
	
	xSemaphoreTake(conn_desc -> mutex, portMAX_DELAY);
	conn_desc -> msg_to_send++;
	xSemaphoreGive(conn_desc -> mutex);
	
	xQueueSend(ws_output_queue, &ws_item, portMAX_DELAY);

	return 1;
}


/*************************************************
*
* timer callback for closing websocket
*
**************************************************/

void vCloseTimeoutCallback( TimerHandle_t xTimer ){
	uint8_t index;
	connection_desc_t *conn_desc = NULL;

	index = *(uint8_t *)pvTimerGetTimerID(xTimer);

	conn_desc = &connection_tab[index];
	
	if (conn_desc != NULL){
		//clear connection resources
		if (conn_desc -> msg_to_send > 0){
			//wait until all messages are sent
			xTimerReset(xTimer, 5);
			printf("ws reset timer\n"); //TEST
			return;
		}
	
		close_thing_connection(conn_desc, "WS TIME OUT");
	}
}


// ****************************************************************************
//prepare websocket header and send data to client
static void ws_send_task(void* arg){
	ws_queue_item_t *q_item;
	ws_send_data ws_data;
	connection_desc_t *conn_desc = NULL;
	WS_STATE state;

	for(;;){
		xQueueReceive(ws_output_queue, &q_item, portMAX_DELAY);

		memset(head_buff, 0, q_item -> len + 4);
		if (q_item -> ws_frame == 0x1){
			add_ws_header(q_item, &ws_data);
		}
		else{
			memcpy(head_buff, q_item -> payload, q_item -> len);
			ws_data.payload = head_buff;
			ws_data.len = q_item -> len;
		}
		conn_desc = q_item -> conn_desc;
		free(q_item -> payload);

		//check if connection is not deleted
		if (conn_desc -> netconn_ptr == NULL){
			printf("ws_send: connection DELETED\n");
			goto ws_send_connection_deleted;
		}
		
		//send data to the client
		state = conn_desc -> ws_state;
		if ((state == WS_OPEN) || (state == WS_OPENING) || (state == WS_CLOSING)){
			//decrement nr of messages to send for this connection
			xSemaphoreTake(conn_desc -> mutex, portMAX_DELAY);
			conn_desc -> msg_to_send--;
			xSemaphoreGive(conn_desc -> mutex);
			
			if (conn_desc -> msg_to_send < 0){
				printf("msg to send ERROR: %i\n", conn_desc -> msg_to_send);
			}
			
			err_t err = netconn_write(conn_desc -> netconn_ptr,
										ws_data.payload,
										ws_data.len,
										NETCONN_COPY);
			
			if (err != ERR_OK){
				//data not sent, TCP error occured
				conn_desc -> send_errors++;
				//TODO: what if answer for open handshake was not sent?
				printf("data not sent to one, index = %i, err = %i, \ndata:%s\n",
						conn_desc -> index, err, (char *)ws_data.payload);
				if (state != WS_CLOSING){
					if (conn_desc -> send_errors >= WS_MAX_ERRORS){
						printf("WS SEND: too much errors\n");
						conn_desc -> ws_state = WS_CLOSING;
						conn_desc -> ws_status_code = ABNORMAL_CLS;
						conn_desc -> ws_close_initiator = WS_CLOSE_BY_SERVER;
						create_connection_timeout(conn_desc);
					}
				}
				else if ((state == WS_CLOSING) && (q_item -> opcode == WS_OP_CLS)) {
					create_connection_timeout(conn_desc);
				}
				else{
					printf("WS_CLOSING send error, %i\n", err);
				}
			}
			else{
				//data sent correctly
				if (conn_desc -> send_errors > 0){
					conn_desc -> send_errors = 0;
				}
				
				if (conn_desc -> ws_state == WS_OPENING){
					conn_desc -> ws_state = WS_OPEN;
				}

				int8_t opcode = q_item -> opcode;
				if (opcode == WS_OP_CLS){
					create_connection_timeout(conn_desc);
				}
				else if (opcode == WS_OP_PON){
					conn_desc -> ws_pongs++;
				}
				else if (opcode == WS_OP_PIN){
					conn_desc -> ws_pings++;
				}
				
				conn_desc -> packets++;
				conn_desc -> bytes += ws_data.len;
			}
		}
		else{
			printf("ERROR by sending data: websocket incorrect state\n");
		}
ws_send_connection_deleted:
		free(q_item);
	}
}

// ****************************************************************************
//add websocket header to sending data
void add_ws_header(ws_queue_item_t *q, ws_send_data *out){
	ws_frame_header_u_t header;

	out -> payload = head_buff;
	//add websocket header
	header.h.opcode = q -> opcode;
	header.h.fin = 0x1;
	header.h.mask = 0x0;	//only client masks data
	if (q -> len <= 125){
		header.h.payload_len = q -> len;
		head_buff[0] = header.bytes[0];
		head_buff[1] = header.bytes[1];
		if (q -> len > 0){
			memcpy(head_buff + 2, q -> payload, q -> len);
		}
		out -> len = q -> len + 2;
	}
	else if (q -> len <= MAX_PAYLOAD_LEN){
		header.h.payload_len = 126;
		head_buff[0] = header.bytes[0];
		head_buff[1] = header.bytes[1];
		head_buff[2] = (q -> len) >> 8;
		head_buff[3] = (q -> len) & 0x00FF;
		memcpy(head_buff + 4, q -> payload, q -> len);
		out -> len = q -> len + 4;
	}
	else{
		//too long message
		out -> len = 0;
		out -> payload = NULL;
	}
}


/*************************************************************************
 *
 * Initialize WebSocket server
 *
 * ***********************************************************************/
int8_t ws_server_init(uint16_t port){
	int8_t ret;

	ws_port = port;

	if (ws_server_is_running == 0){
		vTaskDelay(1000 / portTICK_PERIOD_MS);
		ws_output_queue = xQueueCreate(10, sizeof(ws_queue_item_t *));
		if (ws_output_queue != NULL){
		}
		else{
			printf("OUT queue not created\n");
		}

		if (ws_output_queue != NULL){
			ws_server_is_running = 1;
		}
		else{
			printf("ws server not created\n");
		}
		//open output (sending) queue
		xTaskCreate(ws_send_task, "ws_send_task", 2048, NULL, 1, NULL);
		ret = 1;
	}
	else{
		ret = -1;
	}
	return ret;
}


// ***************************************************************
//
// send data via websocket
// for external usage only (for web things)
// don't use for opening/closing websocket connection!
//
// ***********************************************************
int8_t ws_send(ws_queue_item_t *item, int32_t wait_ms){
	
	if (item -> conn_desc -> ws_state != WS_OPEN){
		return -1;
	}
	
	xSemaphoreTake(item -> conn_desc -> mutex, portMAX_DELAY);
	item -> conn_desc -> msg_to_send++;
	xSemaphoreGive(item -> conn_desc -> mutex);
	
	return xQueueSend(ws_output_queue, &item, wait_ms / portTICK_RATE_MS);
}

// ****************************************************************************
int8_t ws_server_stop(){
	ws_server_is_running = 0;
	//TODO: delete output queue and stop it's task

	return 1;
}
