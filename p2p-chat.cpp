/*
 * P2P chat between two participants (planned to be extended to N participtans).
 * A STUN information is generated via an external STUN server, 
 * and is exchanged between participants via an external node.js server.
 * Afterwards it's all p2p.
 *
 * Build:
 *    1. Compile Socket.IO C++ Client (https://github.com/socketio/socket.io-client-cpp/)
 *    2. Put the following files into the 'object' folder: 
 *          libboost_date_time.a, libboost_random.a, libboost_system.a, libsioclient.a, libsioclient_tls.a
 *    3. build libnice (https://nice.freedesktop.org/wiki/)
 *
 * Run:
 *    p2p-chat <nickname> $(host -4 -t A stun.stunprotocol.org | awk '{ print $4 }')
 */
 

#include "sio_client.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <agent.h>

#include <gio/gnetworking.h>

#include <mutex>
#include <condition_variable>

// colors for printf
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KWHT  "\x1B[37m"

using namespace sio;
using namespace std;
std::mutex                  _lock;
std::condition_variable_any _cond;
bool connect_finish = false;

// arrays of size two since there are two connections: broadcasting and recieving (TODO: remove global)
std::string                 _remote_candidate[2];
bool                        _candidate_finish[2] = {false, false};
bool                        _got_remote_candidate[2] = {false, false};
NiceAgent *_agent[2];
std::string _candidate_data[2];
static guint stream_id[2];

static GMainLoop *gloop;
static GIOChannel* io_stdin;
static const gchar *candidate_type_name[] = {"host", "srflx", "prflx", "relay"};
static const gchar *state_name[] = {"disconnected", "gathering", "connecting",
                                    "connected", "ready", "failed"};

/* Socket.io class, function and variable */
class connection_listener
{
    sio::client &handler;

public:
    
    connection_listener(sio::client& h): handler(h) {}
    
    void on_connected()
    {
        _lock.lock();
        _cond.notify_all();
        connect_finish = true;
        _lock.unlock();
    }
    void on_close(client::close_reason const& reason)
    {
        printf("sio closed ");
        exit(0);
    }
    
    void on_fail()
    {
        printf("sio failed ");
        exit(0);
    }
};

void bind_events(socket::ptr &socket);

socket::ptr current_socket;


/* ICE protocol functions */
static int gather_local_data(NiceAgent *agent, guint stream_id,
    guint component_id, std::string & stun);
static int parse_remote_data(NiceAgent *agent, guint stream_id,
    guint component_id, const char *line);
static void cb_candidate_gathering_done(NiceAgent *agent, guint stream_id,
    gpointer data); // 'cb' is for "callback"
static void cb_new_selected_pair(NiceAgent *agent, guint stream_id,
    guint component_id, gchar *lfoundation,
    gchar *rfoundation, gpointer data);
static void cb_component_state_changed(NiceAgent *agent, guint stream_id,
    guint component_id, guint state,
    gpointer data);
static void cb_nice_recv(NiceAgent *agent, guint stream_id, guint component_id,
    guint len, gchar *buf, gpointer data);
static gboolean stdin_remote_info_cb (gpointer data);
static gboolean stdin_send_data_cb (GIOChannel *source, GIOCondition cond,
    gpointer data);


int  main(int argc, char *argv[]) {

  /* BEGIN Nicelib vars */
  NiceAgent *agent[2];
  gchar *stun_addr = NULL;
  guint stun_port = 0;
  guint i;
  gboolean controlling; //determines if socket is broadcasting or recieving
  /* END Nicelib vars */

  /* BEGIN socket.io */
  sio::client h;
    connection_listener l(h);
    
    h.set_open_listener(std::bind(&connection_listener::on_connected, &l));
    h.set_close_listener(std::bind(&connection_listener::on_close, &l,std::placeholders::_1));
    h.set_fail_listener(std::bind(&connection_listener::on_fail, &l));
    h.connect("http://127.0.0.1:1981");
    _lock.lock();
    if(!connect_finish)
    {
        _cond.wait(_lock);
    }
    _lock.unlock();
    current_socket = h.socket();
  /* END socket.io */

  bind_events(current_socket);

  /* BEGIN Parse arguments */
  if (argc > 4 || argc < 2) {
    fprintf(stderr, "Usage: %s <nickname> stun_addr [stun_port]\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (argc > 2) {
    stun_addr = argv[2];
    if (argc > 3)
      stun_port = atoi(argv[3]);
    else
      stun_port = 3478;

    g_debug("Using stun server '[%s]:%u'\n", stun_addr, stun_port);
  }
  /* END Parse arguments */

  /* BEGIN Init networking */
  g_networking_init();

  gloop = g_main_loop_new(NULL, FALSE);
  
  #ifdef G_OS_WIN32
    io_stdin = g_io_channel_win32_new_fd(_fileno(stdin));
  #else
    io_stdin = g_io_channel_unix_new(fileno(stdin));
  #endif
  /* END Init networking */

  /* BEGIN Nice part */
  for (i=0; i<2; i++) { //creating two agents, each with different "controlling" value
    // Create the nice agent
    agent[i] = nice_agent_new(g_main_loop_get_context (gloop),
        NICE_COMPATIBILITY_RFC5245);
    if (agent[i] == NULL)
      g_error("Failed to create agent");

    // Set the STUN settings and controlling mode
    if (stun_addr) {
      controlling = i;
      g_object_set(agent[i], "stun-server", stun_addr, NULL);
      g_object_set(agent[i], "stun-server-port", stun_port, NULL);
    }
    g_object_set(agent[i], "controlling-mode", controlling, NULL);

    // Copy agent data to global agent
    _agent[i] = agent[i];

    // Connect to the signals
    g_signal_connect(agent[i], "candidate-gathering-done",
        G_CALLBACK(cb_candidate_gathering_done), NULL);
    g_signal_connect(agent[i], "new-selected-pair",
        G_CALLBACK(cb_new_selected_pair), NULL);
    g_signal_connect(agent[i], "component-state-changed",
        G_CALLBACK(cb_component_state_changed), NULL);

    // Create a new stream with one component
    stream_id[i] = nice_agent_add_stream(agent[i], 1);
    if (stream_id[i] == 0)
      g_error("Failed to add stream");

    // Attach to the component to receive the data
    // Without this call, candidates cannot be gathered
    nice_agent_attach_recv(agent[i], stream_id[i], 1,
        g_main_loop_get_context (gloop), cb_nice_recv, NULL);

    // Start gathering local candidates
    if (!nice_agent_gather_candidates(agent[i], stream_id[i]))
      g_error("Failed to start candidate gathering");

    g_debug("waiting for candidate-gathering-done signal...");
  }

  // Run the mainloop. Everything else will happen asynchronously
  // when the candidates are done gathering.
  g_main_loop_run (gloop);

  g_main_loop_unref(gloop);
  g_object_unref(agent);
  g_io_channel_unref (io_stdin);

  return EXIT_SUCCESS;
}

// handles socket.io recieved messages
void bind_events(socket::ptr &socket) {
  current_socket->on("message", sio::socket::event_listener_aux([&](string const& name, message::ptr const& data, bool isAck,message::list &ack_resp)
                       {
                            std::string remote_candidate;
                            int remote_candidate_control_flag;
                            guint i;

                            _lock.lock();

                            remote_candidate = data->get_string();
                            i = remote_candidate[0] == '0' ? 1 : 0; //send candidate 0 (controlled) of the other client to 1 (controlling) here, and vice-versa
                            remote_candidate.erase(0,1); //leave only STUN info
                            _remote_candidate[i] = remote_candidate;
                            if (_candidate_finish[i])
                              stdin_remote_info_cb (_agent[i]);
                            _got_remote_candidate[i] = true;

                            _lock.unlock();
                       }));
}

static void cb_candidate_gathering_done(NiceAgent *agent, guint _stream_id, gpointer data) {

  gboolean bool_controlling_mode;
  g_object_get(agent,"controlling-mode", &bool_controlling_mode, NULL);
  guint controlling_mode = bool_controlling_mode ? 1 : 0;
  std::string candidate_data = "";

  g_debug("SIGNAL candidate gathering done\n");

  // Candidate gathering is done. Send our local candidates on stdout
  gather_local_data(agent, _stream_id, 1, candidate_data);
  _candidate_data[controlling_mode] = std::to_string(controlling_mode) + candidate_data;
  if (!controlling_mode) {
    printf("Broadcasting local connection data.\n");
    current_socket->emit("message", _candidate_data[controlling_mode]); 
    printf("Controlled local connection data broadcasted.\n");
  }
  if (_got_remote_candidate[controlling_mode]) {
    stdin_remote_info_cb (_agent[controlling_mode]);
  }

  _candidate_finish[controlling_mode] = true;
}

static gboolean stdin_remote_info_cb (gpointer data)
{     
  NiceAgent *agent = (NiceAgent*) data;
  int rval;
  gboolean ret = TRUE;

  gboolean bool_controlling_mode;
  g_object_get(agent,"controlling-mode", &bool_controlling_mode, NULL);
  guint controlling_mode = bool_controlling_mode ? 1 : 0;

  // Parse remote candidate list and set it on the agent
  rval = parse_remote_data(agent, stream_id[controlling_mode], 1, _remote_candidate[controlling_mode].c_str());
  if (rval == EXIT_SUCCESS) {
    // Return FALSE so we stop listening to stdin since we parsed the
    // candidates correctly
    ret = FALSE;
    g_debug("waiting for state READY or FAILED signal...");
    printf("succesffully parsed remote data\n");
    if (controlling_mode) {
      current_socket->emit("message", _candidate_data[controlling_mode]); 
      printf("Controlling local connection data broadcasted.\n");
    }
    } else {
      fprintf(stderr, "ERROR: failed to parse remote data\n");
      printf("Enter remote data (single line, no wrapping):\n");
      printf("> ");
      fflush (stdout);
    }

  return ret;
}

static void cb_component_state_changed(NiceAgent *agent, guint _stream_id,
    guint component_id, guint state,
    gpointer data) {

  gboolean bool_controlling_mode;
  g_object_get(agent,"controlling-mode", &bool_controlling_mode, NULL);
  guint controlling_mode = bool_controlling_mode ? 1 : 0;

  printf("SIGNAL: state of agent %d changed %d %d %s[%d]\n",
      controlling_mode, _stream_id, component_id, state_name[state], state);
  g_debug("SIGNAL: state state of agent %d changed %d %d %s[%d]\n",
      controlling_mode, _stream_id, component_id, state_name[state], state);

  if (state == NICE_COMPONENT_STATE_CONNECTED) {
    NiceCandidate *local, *remote;

    // Get current selected candidate pair and print IP address used
    if (nice_agent_get_selected_pair (agent, _stream_id, component_id,
                &local, &remote)) {
      gchar ipaddr[INET6_ADDRSTRLEN];

      nice_address_to_string(&local->addr, ipaddr);
      printf("\nNegotiation complete: ([%s]:%d,",
          ipaddr, nice_address_get_port(&local->addr));
      nice_address_to_string(&remote->addr, ipaddr);
      printf(" [%s]:%d)\n", ipaddr, nice_address_get_port(&remote->addr));
    }

    // If controlling-mode: Listen to stdin and send data written to it
    if (controlling_mode) {
      printf("\nSend lines to remote (Ctrl-D to quit):\n");
      g_io_add_watch(io_stdin, G_IO_IN, stdin_send_data_cb, agent);
      printf("> ");
      fflush (stdout);
    }
  } else if (state == NICE_COMPONENT_STATE_FAILED) {
    g_main_loop_quit (gloop);
  }
}

static gboolean stdin_send_data_cb (GIOChannel *source, GIOCondition cond,
    gpointer data) {
  NiceAgent *agent = (NiceAgent*) data;
  gchar *line = NULL;

  gboolean bool_controlling_mode;
  g_object_get(agent,"controlling-mode", &bool_controlling_mode, NULL);
  guint controlling_mode = bool_controlling_mode ? 1 : 0;

  if (g_io_channel_read_line (source, &line, NULL, NULL, NULL) ==
      G_IO_STATUS_NORMAL) {
    nice_agent_send(agent, stream_id[controlling_mode], 1, strlen(line), line);
    g_free (line);
    printf("%s > %s", KWHT, KGRN);
    fflush (stdout);
  } else {
    nice_agent_send(agent, stream_id[controlling_mode], 1, 1, "\0");
    // Ctrl-D was pressed.
    g_main_loop_quit (gloop);
  }

  return TRUE;
}

static void cb_new_selected_pair(NiceAgent *agent, guint _stream_id,
    guint component_id, gchar *lfoundation, gchar *rfoundation, gpointer data) {
  g_debug("SIGNAL: selected pair %s %s", lfoundation, rfoundation);
}

static void cb_nice_recv(NiceAgent *agent, guint _stream_id, guint component_id,
    guint len, gchar *buf, gpointer data) {
  if (len == 1 && buf[0] == '\0')
    g_main_loop_quit (gloop);
  printf("%s%.*s", KRED, len, buf);
  printf("%s > %s", KWHT, KGRN);
  fflush(stdout);
}

static NiceCandidate * parse_candidate(char *scand, guint _stream_id) {
  NiceCandidate *cand = NULL;
  NiceCandidateType ntype;
  gchar **tokens = NULL;
  guint i;

  tokens = g_strsplit (scand, ",", 5);
  for (i = 0; tokens[i]; i++);
  if (i != 5)
    goto end;

  for (i = 0; i < G_N_ELEMENTS (candidate_type_name); i++) {
    if (strcmp(tokens[4], candidate_type_name[i]) == 0) {
      ntype = (NiceCandidateType) i;
      break;
    }
  }
  if (i == G_N_ELEMENTS (candidate_type_name))
    goto end;

  cand = nice_candidate_new(ntype);
  cand->component_id = 1;
  cand->stream_id = _stream_id;
  cand->transport = NICE_CANDIDATE_TRANSPORT_UDP;
  strncpy(cand->foundation, tokens[0], NICE_CANDIDATE_MAX_FOUNDATION);
  cand->foundation[NICE_CANDIDATE_MAX_FOUNDATION - 1] = 0;
  cand->priority = atoi (tokens[1]);

  if (!nice_address_set_from_string(&cand->addr, tokens[2])) {
    g_message("failed to parse addr: %s", tokens[2]);
    nice_candidate_free(cand);
    cand = NULL;
    goto end;
  }

  nice_address_set_port(&cand->addr, atoi (tokens[3]));

 end:
  g_strfreev(tokens);

  return cand;
}


static int gather_local_data (NiceAgent *agent, guint _stream_id, guint component_id, std::string & stun) {
  
  int result = EXIT_FAILURE;
  gchar *local_ufrag = NULL;
  gchar *local_password = NULL;
  gchar ipaddr[INET6_ADDRSTRLEN];
  GSList *cands = NULL, *item;

  if (!nice_agent_get_local_credentials(agent, _stream_id,
      &local_ufrag, &local_password))
    goto end;

  cands = nice_agent_get_local_candidates(agent, _stream_id, component_id);
  if (cands == NULL)
    goto end;

  stun += std::string(local_ufrag) + " " + std::string(local_password);

  for (item = cands; item; item = item->next) {
    NiceCandidate *c = (NiceCandidate *)item->data;

    nice_address_to_string(&c->addr, ipaddr);

    // (foundation),(prio),(addr),(port),(type)
    stun += " " + std::string(c->foundation) + "," + std::to_string(c->priority) + "," + std::string(ipaddr) + "," 
                + std::to_string(nice_address_get_port(&c->addr)) + "," + std::string(candidate_type_name[c->type]);
  }
  result = EXIT_SUCCESS;

 end:
  if (local_ufrag)
    g_free(local_ufrag);
  if (local_password)
    g_free(local_password);
  if (cands)
    g_slist_free_full(cands, (GDestroyNotify)&nice_candidate_free);

  return result;
}



static int parse_remote_data(NiceAgent *agent, guint _stream_id,
    guint component_id, const char *line){
  GSList *remote_candidates = NULL;
  gchar **line_argv = NULL;
  const gchar *ufrag = NULL;
  const gchar *passwd = NULL;
  int result = EXIT_FAILURE;
  int i;


  line_argv = g_strsplit_set (line, " \t\n", 0);
  for (i = 0; line_argv && line_argv[i]; i++) {
    if (strlen (line_argv[i]) == 0)
      continue;

    // first two args are remote ufrag and password
    if (!ufrag) {
      ufrag = line_argv[i];
    } else if (!passwd) {
      passwd = line_argv[i];
    } else {
      // Remaining args are serialized canidates (at least one is required)
      NiceCandidate *c = parse_candidate(line_argv[i], _stream_id);

      if (c == NULL) {
        g_message("failed to parse candidate: %s", line_argv[i]);
        goto end;
      }
      remote_candidates = g_slist_prepend(remote_candidates, c);
    }
  }
  if (ufrag == NULL || passwd == NULL || remote_candidates == NULL) {
    g_message("line must have at least ufrag, password, and one candidate");
    goto end;
  }

  if (!nice_agent_set_remote_credentials(agent, _stream_id, ufrag, passwd)) {
    g_message("failed to set remote credentials");
    goto end;
  }

  // Note: this will trigger the start of negotiation.
  if (nice_agent_set_remote_candidates(agent, _stream_id, component_id,
      remote_candidates) < 1) {
    g_message("failed to set remote candidates");
    goto end;
  }

  result = EXIT_SUCCESS;

 end:
  if (line_argv != NULL)
    g_strfreev(line_argv);
  if (remote_candidates != NULL)
    g_slist_free_full(remote_candidates, (GDestroyNotify)&nice_candidate_free);

  return result;
}
