
#include <iostream>
#include <unistd.h>
#include <list>
#include <mutex>
#include <thread>
#include <string.h>
#include <errno.h>

#include <event2/thread.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/event.h>

#include <event2/listener.h> //server only

#define SERVER_ADDRESS "127.0.0.1"
#define SERVER_PORT 9995

static const bool TESTING_CLIENT_TIME_OUT = true; //turn it on or off

struct RequestInfo {
  std::string msg;
  int request_to_go = 3;
};

class AsyncClient {
 public:
  explicit AsyncClient(uint32_t max_queue_size = 10240)
      : max_queue_size_(max_queue_size) {}
  ~AsyncClient() = default;
  int Enqueue(RequestInfo *request_info);
  size_t QueueSize() { return task_queue_.size(); }
  void Start();
  void Stop();
  void Join();

 private:
  //// multi thread access
  std::mutex mu_;
  bool stop_ = false;
  uint32_t max_queue_size_;
  std::list<RequestInfo*> task_queue_;
  std::thread thread_;

  const int max_active_requests_ = 1000;

  //// event loop access
  //accessed by multi-threads, thread-safe with evthread_use_pthreads() initial
  struct event_base *base_ = nullptr;

  //accessed by multi-threads with event_add(), event_active()
  //thread-safe with mutex mu_
  struct event* enqueue_event_ = nullptr; 

  ////
  // data wrapper passed around static call backs
  ////
  struct RequestInfoWrap
  {
    explicit RequestInfoWrap(AsyncClient *cleint, RequestInfo *request)
             : this_client_(cleint), request_info_(request) {}
    AsyncClient *this_client_;
    RequestInfo *request_info_;
  };

  //all private functions, static & non-static, run on event-loop thread
  void run();
  int Dequeue(bool enqueue_event_triggered = false);
  int ConnectToServer(const char *server_ip, const unsigned short server_port, 
                      struct RequestInfoWrap *request_data);

  static void enqueue_event_cb(evutil_socket_t s, short what, void *ptr);
  static void socket_connect_cb(struct bufferevent *bev, short events, void *ptr);
  static void socket_write_cb(struct bufferevent *bev, void *ptr);
  static void socket_read_cb(struct bufferevent *bev, void *ptr);
};

int AsyncClient::Enqueue(RequestInfo *request_info) {
 
  std::lock_guard<std::mutex> guard(mu_);
  if (stop_) {
    return -1; //Status(Status::NotOK, "the runner was stopped");
  }
  if (task_queue_.size() >= max_queue_size_) {
    //TODO: disgard the front one, add new one to the tail, LOG warning
    return -1; //Status(Status::NotOK, "the task queue was reached max length");
  }
  task_queue_.push_back(request_info);
  
  //notify event-loop dispatch, this causes enqueue_event_cb() be called
  event_active(enqueue_event_, EV_READ, 0); 
  return 0; //Status::OK();
}

void AsyncClient::enqueue_event_cb(evutil_socket_t s, short what, void *ptr)
{

  AsyncClient *notifyee = static_cast<AsyncClient *>(ptr);
  notifyee->Dequeue(true);
}

void AsyncClient::socket_read_cb(struct bufferevent *bev, void *ptr)
{
  //TODO: dynamic buffer allocation: i.e. redis_request.cc Tokenize()
  char buf[1024];
  int n;
  struct evbuffer *input = bufferevent_get_input(bev);

  std::cout << "client> ";
  while ((n = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
    fwrite(buf, 1, n, stdout);
  }

  //done with one server request/reply round-trip
  bufferevent_free(bev); //this closes the connection
  //bufferevent_disable(bev, EV_READ); TODO: re-use this connection later

  RequestInfoWrap *prequest_wrap = static_cast<RequestInfoWrap*>(ptr);
  if (nullptr == prequest_wrap)
  {
    std::cout << "client> assertion failed\n";
    return;
  }
  if (nullptr == prequest_wrap->request_info_)
  {
    std::cout << "client> assertion failed\n";
    delete prequest_wrap;
    return;
  }

  std::cout << " -- " << prequest_wrap->request_info_->msg << "\n";

  prequest_wrap->request_info_->request_to_go--;
  if (0 == prequest_wrap->request_info_->request_to_go)
  { //Done with this task request
    auto this_client = prequest_wrap->this_client_;
    delete prequest_wrap->request_info_;
    delete prequest_wrap;

    //if more task in queue
    int connections_fired = this_client->Dequeue();
    std::cout << "client> " << connections_fired << " new request task started\n";
  }
  else //connect to another server
  {
    prequest_wrap->request_info_->msg += " more time";
    if (0 != prequest_wrap->this_client_->ConnectToServer(SERVER_ADDRESS, SERVER_PORT, prequest_wrap))
    {
      delete prequest_wrap->request_info_;
      delete prequest_wrap;
    }
  }
}

void AsyncClient::socket_write_cb(struct bufferevent *bev, void *ptr) {
  struct evbuffer *output = bufferevent_get_output(bev);
  if (evbuffer_get_length(output) == 0) {
    bufferevent_enable(bev, EV_READ);

    //set read timeout, socket_connect_cb() will be called if timeout happens
    struct timeval tv_r = {2,0}; //2 seconds
    bufferevent_set_timeouts(bev, &tv_r, nullptr);
  }
}

void AsyncClient::socket_connect_cb(struct bufferevent *bev, short events, void *ptr) {
  //RequestInfo *prequest_info = static_cast<RequestInfo*>(ptr);
  RequestInfoWrap *prequest_wrap = static_cast<RequestInfoWrap*>(ptr);
  if (nullptr == prequest_wrap || nullptr == prequest_wrap->request_info_)
  {
    printf("client> error: socket_connect_cb: no request data.\n");
    bufferevent_free(bev);
    return;
  }
  auto prequest_info = prequest_wrap->request_info_;

  if (events & BEV_EVENT_CONNECTED) {
    //printf("client> Connected to server.\n");
    bufferevent_enable(bev, EV_WRITE);

    //set write timeout, socket_connect_cb() will be called if timeout happens
    struct timeval tv_w = {2,0}; //2 seconds
    bufferevent_set_timeouts(bev, nullptr, &tv_w);

    bufferevent_write(bev, prequest_info->msg.c_str(), prequest_info->msg.size());
    return;
  } else if (events & BEV_EVENT_EOF) {
    printf("client> Connection closed.\n");
  } else if (events & BEV_EVENT_ERROR) {
    printf("client> Network error: %s\n", strerror(errno));
  } else if (events & (BEV_EVENT_READING|BEV_EVENT_TIMEOUT)) { 
    printf("client> Reading time out, closing connection\n");
  } else if (events & (BEV_EVENT_WRITING|BEV_EVENT_TIMEOUT)) {
    printf("client> Writing time out, closing connection\n");
  }

  //clean up
  bufferevent_free(bev);
  delete prequest_info;
  delete prequest_wrap;
}

//
//return number of connections to server are fired.
//
int AsyncClient::Dequeue(bool enqueue_event_triggered)
{
  
  std::lock_guard<std::mutex> guard(mu_);
  if (stop_)
  {
    std::cout << "client> task thread stopped" << "\n";
    return 0;
  }

  int num_of_connections_fired = 0;

  while (!task_queue_.empty()) {
  
   int num_events = event_base_get_num_events(base_,
                                               EVENT_BASE_COUNT_ACTIVE|
                                               EVENT_BASE_COUNT_ADDED|
                                               EVENT_BASE_COUNT_VIRTUAL);
    if (num_events >= max_active_requests_ && enqueue_event_triggered)
    {
      std::cout << "client> IO capacity reached: " << num_events << "\n";
      break;
    }
    else
      std::cout << "client> number of events: " << num_events << "\n";
  
    RequestInfo* request_info = nullptr;
    request_info = task_queue_.front();
    task_queue_.pop_front();
  
    if (nullptr == request_info)
      break;
     
    struct RequestInfoWrap *request_data = new RequestInfoWrap(this, request_info);
    if (0 != ConnectToServer(SERVER_ADDRESS, SERVER_PORT, request_data)) //non block here
    {
      delete request_info;
      delete request_data;
      break;
    }
    else 
      num_of_connections_fired += 1;

    if (!enqueue_event_triggered) //called after completion of a request, one IO slot freed 
      break;
  } //end of while

  if (enqueue_event_triggered)
  {
      //std::cout << "client> reset enqueue event" << "\n";
      event_add(enqueue_event_, NULL); //reset enqueue event
  }
 
  return num_of_connections_fired;
}

/*
 * setup call backs
 * start to make connection to server, nonblocking
 */
int AsyncClient::ConnectToServer(const char *server_ip, const unsigned short server_port, 
                      struct RequestInfoWrap *request_data)
{
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(server_port);
  if (evutil_inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
    printf("Invalid address.\n");
    return -1;
  }

  struct bufferevent *bev;
  bev = bufferevent_socket_new(base_, -1, BEV_OPT_CLOSE_ON_FREE); //thead safe
  if (!bev) {
    printf("Error creating bufferevent.\n");
    return -1;
  }
  bufferevent_setcb(bev, socket_read_cb, socket_write_cb, socket_connect_cb, request_data);

  //nonblocking here, will fire call back socket_connect_cb()
  if (bufferevent_socket_connect(bev, (struct sockaddr *)&server_addr,
                              sizeof(server_addr)) < 0) {
    printf("Error connecting to server.\n");

    bufferevent_free(bev);
    delete request_data->request_info_;
    delete request_data;

    return -1;
  }
  return 0;
}

/*
 * create thread
 */
void AsyncClient::Start() {
  stop_ = false;
  thread_ = std::thread([this]() {
    pthread_setname_np(pthread_self(), "client-runner");
    this->run();
  });
}

void AsyncClient::Stop() {
  std::lock_guard<std::mutex> guard(mu_);
  stop_ = true;
  task_queue_.clear(); //TODO memory leak here
  //event_active(enqueue_event_, EV_READ, 0); //to complete the already added enqueue event
  //event_del(enqueue_event_); //not working

  if (TESTING_CLIENT_TIME_OUT)
  {
    event_del(enqueue_event_); 
  }
  else
  {
    struct timeval delay = { 2, 0 };
    printf("client> exiting cleanly in 2 seconds.\n");
    event_base_loopexit(base_, &delay); //delay a bit, let message out
  }
}

void AsyncClient::Join() {
  if (thread_.joinable()) thread_.join();
}

/*
 * create event base
 * set enqueue event
 * run thread on event-dispatch-loop waiting for events
 */
void AsyncClient::run() {
  base_ = event_base_new();
  if (!base_) {
    printf("Error creating event base\n");
    return;
  }
  enqueue_event_ = event_new(base_, -1, EV_READ, 
                      enqueue_event_cb, this); //pass this to static call back fn
  event_add(enqueue_event_, NULL); //set enqueue event

  std::cout << "client> " << thread_.get_id() << " running ..." << "\n"; 
  event_base_dispatch(base_);
  std::cout << "client> out dispatch" << "\n"; 

  event_base_free(base_);
  event_free(enqueue_event_);

  for (auto request_info: task_queue_)
    delete request_info;
}

////////////////////////////
class MockupServer {
 public:
  MockupServer(){}
  ~MockupServer() = default;
  void Start();
  void Join();


 private:
  void run();
  std::thread thread_;

  static bool s_ready_to_quit;
  static int s_requests_to_reply;

  //// event loop access
  struct event_base *base_ = nullptr;
  struct evconnlistener *listener_ = nullptr;

  //call backs must be static
  static void listener_cb(struct evconnlistener *, evutil_socket_t,
                          struct sockaddr *, int socklen, void *);
  static void conn_writecb(struct bufferevent *, void *);
  static void conn_readcb(struct bufferevent *bev, void *user_data);
  static void conn_eventcb(struct bufferevent *, short, void *);
};
bool MockupServer::s_ready_to_quit = false;
int MockupServer::s_requests_to_reply = 0;

void MockupServer::listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *sa, int socklen, void *user_data)
{
  struct event_base *base = static_cast<struct event_base *>(user_data);
  struct bufferevent *bev;

  bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  if (!bev) {
    fprintf(stderr, "Error constructing bufferevent!");
    event_base_loopbreak(base);
    return;
  }
  bufferevent_setcb(bev, conn_readcb, conn_writecb, conn_eventcb, NULL);
  //bufferevent_enable(bev, EV_READ|EV_WRITE);
  bufferevent_enable(bev, EV_READ); //accept a connection from a client, first read only
  s_requests_to_reply++;
}

void MockupServer::conn_writecb(struct bufferevent *bev, void *user_data)
{
struct evbuffer *output = bufferevent_get_output(bev);
  //printf("  in conn_writecb\n");
  if (evbuffer_get_length(output) == 0) {
    printf("server> flushed answer %i\n", s_requests_to_reply);
  }
}

void MockupServer::conn_readcb(struct bufferevent *bev, void *user_data)
{
static const char MESSAGE[] = "Hello, World!";
  char buf[1024];
  int n;
  struct evbuffer *input = bufferevent_get_input(bev);
  std::cout << "server> ";
  while ((n = evbuffer_remove(input, buf, sizeof(buf))) > 0) {
    fwrite(buf, 1, n, stdout);
  }
  std::cout << "\n";

  if (strncmp(buf, "quit", 4) == 0)
  {
    s_ready_to_quit = true;
    //testing client read timeout
    if (TESTING_CLIENT_TIME_OUT)
      sleep(3);
  }


  bufferevent_enable(bev, EV_WRITE);
  bufferevent_write(bev, MESSAGE, strlen(MESSAGE));

//  if (strncmp(buf, "quit", 4) == 0)
//  {
//    struct timeval delay = { 1, 0 };
//    printf("server> exiting cleanly in one seconds.\n");
//    struct event_base *pbase = bufferevent_get_base(bev);
//    event_base_loopexit(pbase, &delay); //delay a bit, let message out
//  }
}

void MockupServer::conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
  if (events & BEV_EVENT_EOF) {
    s_requests_to_reply--;
    //printf("server> Connection closed:  %i\n", s_requests_to_reply);
    std::cout << "server> Connection closed: " <<  s_requests_to_reply 
              << " (" << s_ready_to_quit << ")\n";
  
    if (s_ready_to_quit)
    {
      auto base = bufferevent_get_base(bev);
      //int num_events = event_base_get_num_events(base, EVENT_BASE_COUNT_ACTIVE);
      //std::cout << "BOXU: " << num_events << "\n";
      if (s_requests_to_reply == 0)
      {
        struct timeval delay = { 1, 0 };
        printf("server> exiting cleanly in one seconds.\n");
        event_base_loopexit(base, &delay); //delay a bit, let message out
      }
    }
  } else if (events & BEV_EVENT_ERROR) {
    printf("Got an error on the connection: %s\n",
        strerror(errno));/*XXX win32*/
  }
  /* None of the other events can happen here, since we haven't enabled
   * timeouts */
  bufferevent_free(bev);
}

/*
 * create event base
 * create thread
 * run thread
 */
void MockupServer::Start() {
  thread_ = std::thread([this]() {
    pthread_setname_np(pthread_self(), "MockupServer");
    this->run();
  });
}

void MockupServer::run() {

  base_ = event_base_new();
  if (!base_) {
    fprintf(stderr, "Could not initialize libevent!\n");
    return;
  }

  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_port = htons(SERVER_PORT);

  listener_ = evconnlistener_new_bind(base_, listener_cb, (void *)base_,
      LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1,
      (struct sockaddr*)&sin,
      sizeof(sin));

  if (!listener_) {
    fprintf(stderr, "Could not create a listener!\n");
    return;
  }

  std::cout << "server> " << thread_.get_id() << " running ..." << "\n"; 
  event_base_dispatch(base_);
  std::cout << "server> out dispatch" << "\n"; 

  event_base_free(base_);
  evconnlistener_free(listener_);
}

void MockupServer::Join() {
  if (thread_.joinable()) thread_.join();
}

////////////////////////////
class BatchInput {
 public:
  explicit BatchInput(AsyncClient *ac, const std::string prefix) :
                  async_client_(ac),
                  prefix_(prefix)
                  {}
  //~BatchInput();
  void Start() {
    thread_ = std::thread([this]() {
      pthread_setname_np(pthread_self(), "MockupServer");
      this->run();
    });
  }
  void Join() {
    if (thread_.joinable()) thread_.join();
  }
 private:
  void run()
  {
    for (int i = 0; i < 1000; i++)
    {
      RequestInfo *prequest_info = new RequestInfo();
      prequest_info->msg = prefix_ + std::to_string(10000 + i);
      async_client_->Enqueue(prequest_info); //non blocking here
    }
  }

 private:
  std::thread thread_;
  AsyncClient *async_client_;
  std::string prefix_;

};

///////////////
int main(int argc, char **argv) {
  evthread_use_pthreads();

  MockupServer server_thread;
  server_thread.Start();

  AsyncClient async_client;
  async_client.Start();

  const bool InteractiveMode = true; 

  //do batch enqueue
  std::list<RequestInfo *> requests;

  if (InteractiveMode)
  {
    for (std::string line; std::getline(std::cin, line);) {
      if (line.size() < 1)
        continue; 
  
      RequestInfo *prequest_info = new RequestInfo();
      prequest_info->msg = line;
      if (line == "quit")
      {
        prequest_info->request_to_go = 1; //only once
        async_client.Enqueue(prequest_info); //tell server to quit
        sleep(2); //let message sent out to server
        async_client.Stop();
        break;
      }
      requests.push_back(prequest_info);
      if (requests.size() >= 2)
      {
        while (requests.size() > 0)
        {
          auto prequest = requests.front();
          requests.pop_front();
          async_client.Enqueue(prequest); //non blocking here
          //std::cout << "Enqueue called " << requests.size() << "\n";
        }
      }
    }
  }
  else
  { //Batch mode
    BatchInput input_a(&async_client, "aaaaa");
    input_a.Start();

    BatchInput input_b(&async_client, "bbbbb");
    input_b.Start();

    BatchInput input_c(&async_client, "ccccc");
    input_c.Start();

    input_a.Join();
    input_b.Join();
    input_c.Join();

    //tell mockup server to quit
    RequestInfo *prequest_info = new RequestInfo();
    prequest_info->msg = "quit";
    prequest_info->request_to_go = 1; //only once
    async_client.Enqueue(prequest_info);
    sleep(2); //let message sent out to server
    async_client.Stop();
  }
    
  server_thread.Join(); //blocking wait for server thread stop
  async_client.Join(); //blocking wait for client to stop

  for (auto prequest: requests) //clean up 
    delete prequest;

  std::cout << " out main ... " << "\n"; 
  return 0;
}
