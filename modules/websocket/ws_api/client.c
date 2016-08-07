#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <libwebsockets.h>

#include "client.h"

static struct lws_protocols protocols[2] = {
  {
    NULL,
    NULL,
    0,
    CLIENT_RX_BUFFER_BYTES,
    0,
    NULL,
  },
  {NULL, NULL, 0, 0, 0, NULL},
};

static struct lws_context_creation_info info;

static struct lws_client_connect_info ccinfo;
static struct lws* wsi;

static int destroy_flag = 0;
static int connection_flag = 0;


// the message ring buffer
static char* ringbuffer[CLIENT_MAX_MESSAGE_QUEUE];
static int ringbuffer_head=0;
static int ringbuffer_tail=0;


static void set_destroy_flag(int sign_no)
{
  if(sign_no == SIGINT)
    destroy_flag = 1;
}



static int websocket_write_back(struct lws *wsi_in, char *str)
{
  if (str == NULL || wsi_in == NULL)
    return -1;

  int n;
  int len = strlen(str);
  unsigned char *out = NULL;

  // we must prepare the buffer containing the padding.
  out = (unsigned char *)malloc(sizeof(unsigned char)*(LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
  memcpy (out + LWS_SEND_BUFFER_PRE_PADDING, str, len );

  n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

  lwsl_notice("[websocket_write_back] %s\n", str);

  free(out);

  return n;
}


static int ws_service_callback(
             struct lws *wsi,
             enum lws_callback_reasons reason, void *user,
             void *in, size_t len)
{

  switch (reason) {

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      lwsl_notice("[Main Service] Connect with server success.\n");
      connection_flag = 1;
      break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      lwsl_notice("[Main Service] Connect with server error.\n");
      destroy_flag = 1;
      connection_flag = 0;
      break;

    case LWS_CALLBACK_CLOSED:
      lwsl_notice("[Main Service] LWS_CALLBACK_CLOSED\n");
      destroy_flag = 1;
      connection_flag = 0;
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      lwsl_notice("[Main Service] Client recvived: %s\n", (char *)in);

      break;
    case LWS_CALLBACK_CLIENT_WRITEABLE :
      lwsl_notice("[Main Service] On writeable is called. send some message\n");
      while (ringbuffer_tail != ringbuffer_head) {
        websocket_write_back(wsi, ringbuffer[ringbuffer_tail]);
        free(ringbuffer[ringbuffer_tail]);
        ringbuffer_tail = (ringbuffer_tail + 1) % CLIENT_MAX_MESSAGE_QUEUE;
      }
      break;

    default:
      break;
  }

  return 0;
}


static struct lws_context *
create_context(int use_ssl, char* protocol, char* cert, char* key, char* cacert)
{
  protocols[0].name = protocol;
  protocols[0].callback = ws_service_callback;

  memset(&info, 0, sizeof info);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  if (use_ssl)
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.ssl_cert_filepath = cert;
  info.ssl_private_key_filepath = key;
  info.ssl_ca_filepath = cacert;
  return  lws_create_context(&info);
}

/**
 * set websocket client connection info.
 * None-zero is returned if errors occurs.
 */
static int
set_client_connect_info(char* protocol, char* address, int port, char* path, int use_ssl, char* cert, char* key, char* cacert)
{
  memset(&ccinfo, 0, sizeof(ccinfo));
  ccinfo.context = create_context(use_ssl, protocol, cert, key, cacert);
  if (ccinfo.context == NULL) {
    lwsl_notice("context is NULL.\n");
    return -1;
  }
  ccinfo.address = address;
  ccinfo.path = path;
  ccinfo.protocol = protocol;
  ccinfo.port = port;
  ccinfo.ssl_connection = use_ssl;
  ccinfo.host = address;
  ccinfo.origin = address;
  ccinfo.ietf_version_or_minus_one = -1;
  return 0;
}

/**
 * set wsi info
 * None-zero is returned if errors occur.
 */
static int
set_wsi()
{
  wsi = lws_client_connect_via_info(&ccinfo);
  if (wsi == NULL) {
    lwsl_notice("wsi create error.\n");
    return -1;
  }
  lwsl_notice("wsi create success.\n");
  return 0;
}

static long int get_client_message_type(int port) {
  return (2 << 16) + port;
}

static int
enqueue_client_msg(char* msg)
{
  //* waiting for connection with server done.*/
  while(!connection_flag && !destroy_flag)
    usleep(1000*20);
  if (!connection_flag && destroy_flag) {
    lwsl_notice("The connection is closed. You can't send messages\n");
    return 1;  // If we are not connected. We cant send the message
  }
  int len = strlen(msg);
  ringbuffer[ringbuffer_head] = (char*) malloc(len + 1);
  memcpy(ringbuffer[ringbuffer_head], msg, len);
  ringbuffer[ringbuffer_head][len] = '\0';
  ringbuffer_head = (ringbuffer_head + 1) % CLIENT_MAX_MESSAGE_QUEUE;
  lws_callback_on_writable(wsi);
  return 0;
}


static void
run_server(int msgqid) {
  struct client_msg_buf buf;
  while(!destroy_flag)
  {
    lws_service(ccinfo.context, 50);
    while (msgrcv(msgqid, &buf, sizeof(buf.data), get_client_message_type(ccinfo.port), IPC_NOWAIT) != -1) {
      enqueue_client_msg(buf.data);
    }
  }
  lws_context_destroy(ccinfo.context);
  lwsl_notice("[notice] Service has ended.\n");
  _exit(0);  // exit the process, otherwise it will go on running parent program.
}


int
websocket_client_create(char* protocol, char* address, int port, char* path, int use_ssl, char* cert, char* key, char* cacert, int* msgqid, int* service_pid)
{

  int ret_code;
  *msgqid = msgget(IPC_PRIVATE, IPC_CREAT|IPC_EXCL);
  if (*msgqid == -1) {
    lwsl_err("msgget error!!!!");
    return -1;
  }

  if ((*service_pid = fork()) == 0) {
    if ((ret_code = set_client_connect_info(protocol, address, port, path, use_ssl, cert, key, cacert)) != 0)
      return ret_code;

    if ((ret_code = set_wsi()) != 0)
      return ret_code;

    signal(SIGINT, set_destroy_flag);
    run_server(*msgqid);
  }
  return 0;
}



int
websocket_client_send_msg(char* msg, int msgqid, int port) {
  struct client_msg_buf buf;
  strncpy(buf.data, msg, CLIENT_RX_BUFFER_BYTES);
  buf.mtype = get_client_message_type(port);
  if (msgsnd(msgqid,&buf,sizeof(buf.data),IPC_NOWAIT) == -1) {
    lwsl_err("sending message error.\n");
    return -1;
  }
  return 0;
}


void
websocket_client_disconnect(int service_pid)
{
  while (ringbuffer_head != ringbuffer_tail && !destroy_flag)
    usleep(20 * 1000);

  kill(service_pid, SIGINT);
  lwsl_notice("shutting down the server\n");
  waitpid(service_pid,NULL,0);
}