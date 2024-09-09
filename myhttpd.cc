#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> 
#include <sys/wait.h>
#include <sys/stat.h>
#include <netdb.h>
#include <unistd.h>
#include <dirent.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <cmath>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

const int QueueLength = 5;

const char * auth = "c3dhc3RpazphZ2Fyd2FsYQ==";
pthread_attr_t attr;
pthread_mutex_t master_mutex;
time_t start_time;
int req_count = 0;

double min_req_time, max_req_time;
std::string min_req_time_url, max_req_time_url;

int masterSocket = -1;

void closeMasterSocket() {
  if (masterSocket != -1) close(masterSocket);
  masterSocket = -1;
}


typedef struct client_request {
  int socket;
  struct sockaddr_in client;
} client_request_t;


void waitForRequest(char mode);
void handleRequest(client_request_t * client_request);
std::string processRequest(client_request_t * client_request);
void processDirectoryRequest(int socket, std::string orig_path,
                             std::string path, std::string query,
                             std::string parent, DIR * dir);

void processCgiBinRequest(int socket, std::string path, std::string query);
void processStatsRequest(int socket);
void processLogsRequest(int socket);

extern "C" void cntrlc(int sig) {
  fprintf(stdout, "\n");
  closeMasterSocket();
  exit(0);
}

extern "C" void zombie(int sig) {
	while (waitpid(-1, NULL, WNOHANG) > 0);
}



int main(int argc, char ** argv) {

  {
    struct sigaction sig;
    sig.sa_handler = zombie;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = SA_RESTART;
    int sigerr = sigaction(SIGCHLD, &sig, NULL);
    if (sigerr) {
      perror("sigaction");
      exit(-1);
    }
  }

  {
    struct sigaction sig;
    sig.sa_handler = cntrlc;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = SA_RESTART;
    int sigerr = sigaction(SIGINT, &sig, NULL);
    if (sigerr) {
      perror("sigaction");
      exit(-1);
    }
  }

  {
    pthread_attr_init(&attr);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_mutex_init(&master_mutex, NULL);
  }

  char mode = 'b';
  int port = 8800;

  if (argc == 1) {}
  else if (argc == 2) port = atoi(argv[1]);
  else if (argc == 3) {
    mode = argv[1][1];
    port = atoi(argv[2]);
  } else {
    fprintf(stderr, "Usage: myhttpd [-f|-t|-p]  [<port>]\n");
    exit(-1);
  }
  
  // Set the IP address and port for this server
  struct sockaddr_in serverIPAddress;
  memset(&serverIPAddress, 0, sizeof(serverIPAddress));
  serverIPAddress.sin_family = AF_INET;
  serverIPAddress.sin_addr.s_addr = INADDR_ANY;
  serverIPAddress.sin_port = htons((u_short) port);
  
  // Allocate a socket
  masterSocket = socket(PF_INET, SOCK_STREAM, 0);
  if (masterSocket < 0) {
    perror("socket");
    exit(-1);
  }

  // Set socket options to reuse port. Otherwise we will
  // have to wait about 2 minutes before reusing the sae port number
  int optval = 1; 
  int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR,
		                   (char *) &optval, sizeof(int));
   
  // Bind the socket to the IP address and port
  int error = bind(masterSocket,
		               (struct sockaddr *) &serverIPAddress,
		               sizeof(serverIPAddress));
  
  if (error) {
    perror("bind");
    exit(-1);
  }
  
  // Put socket in listening mode and set the 
  // size of the queue of unprocessed connections
  error = listen(masterSocket, QueueLength);
  if (error) {
    perror("listen");
    exit(-1);
  }

  start_time = time(NULL);

  if (mode == 'p') {
    pthread_t f_thread;
    char b_mode = 'b';
    for (int i = 0; i < QueueLength; i++) {
      pthread_t n_thread;
      pthread_create(&n_thread, NULL, (void * (*) (void *)) waitForRequest, (void *) b_mode);
      if (i == 0) f_thread = n_thread;
    }

    
    pthread_join(f_thread, NULL);
  } else {
    waitForRequest(mode);
  }
}

void waitForRequest(char mode) {

  while (1) {
    // Accept incoming connections
    struct sockaddr_in clientIPAddress;
    int alen = sizeof(clientIPAddress);

    pthread_mutex_lock(&master_mutex);
    int slaveSocket = accept(masterSocket,
			                       (struct sockaddr *) &clientIPAddress,
			                       (socklen_t*) &alen);

    req_count++;
    pthread_mutex_unlock(&master_mutex);

    client_request_t * client_request = (client_request_t *) malloc(sizeof(client_request_t));
    client_request->socket = slaveSocket;
    client_request->client = clientIPAddress;

    if (slaveSocket < 0) {
      perror("accept");
      exit(-1);
    }

    if (mode == 'b') {
      handleRequest(client_request);
    }

    if (mode == 'f') {
      pid_t child = fork();
      if (child == -1) {
        perror("fork");
        exit(-1);
      } else if (child == 0) {
        closeMasterSocket();
        handleRequest(client_request);
        exit(0);
      } else {
        close(slaveSocket);
      }
    }

    if (mode == 't') {
      // printf("Slave %d\n", slaveSocket);
      pthread_t n_thread;
      pthread_create(&n_thread, &attr, (void * (*) (void *)) handleRequest, (void *) client_request);
    }
  }
}

void handleRequest(client_request_t * client_request) {
  int fd = client_request->socket;

  // printf("Opened %d\n", fd);

  clock_t start_clock = clock();
  //fd_node_t * fd_node = add_node(fd);
  std::string path = processRequest(client_request);
  close(fd);
  free(client_request);
  //remove_node(fd_node);
  clock_t end_clock = clock();

  // printf("Closed %d\n", fd);

  if (!path.empty()) {
    double req_clock = ((double) (end_clock - start_clock)) / CLOCKS_PER_SEC;
    if (max_req_time_url.empty() || req_clock > max_req_time) {
      max_req_time = req_clock;
      max_req_time_url = path;
    }
    if (min_req_time_url.empty() || req_clock < min_req_time) {
      min_req_time = req_clock;
      min_req_time_url = path;
    }
  }
}

std::string processRequest(client_request_t * client_request) {
  int fd = client_request->socket;
  struct sockaddr_in client = client_request->client;

  // printf("Processing %d\n", fd);

  const int MaxRequest = 4 * 1024;
  char req[MaxRequest + 1];
  int requestLen = 0;
  int n;
    
  while (requestLen < MaxRequest &&
	       (n = read(fd, req + requestLen, sizeof(char))) > 0) {

    requestLen++;

    if (requestLen > 3 && req[requestLen - 4] == '\r' 
                       && req[requestLen - 3] == '\n' 
                       && req[requestLen - 2] == '\r' 
                       && req[requestLen - 1] == '\n') {
      requestLen-=4;
      break;
    }
  }

  // printf("Processed %d\n", fd);

  // Add null character at the end of the string
  req[requestLen] = 0;

  // printf("Request: %d\n%s\n", fd, req);

  std::string reqStr = req;
  std::string lookFor = "Authorization: Basic ";
  lookFor += auth;

  size_t auth_idx = reqStr.find(lookFor);
  if (auth_idx == std::string::npos) {
    const char * err = "HTTP/1.1 401 Unauthorized\r\nServer: CS 252 lab5\r\nWWW-Authenticate: Basic realm=\"myhttpd-cs252\"\r\n\r\n";
    write(fd, err, strlen(err));
    return "";
  }

  size_t fs = reqStr.find(' ');
  size_t ss = reqStr.find(' ', fs + 1);

  std::string reqFile = reqStr.substr(fs + 1, ss - fs - 1);
  size_t qm = reqFile.find('?');
  
  std::string path;
  std::string query;

  if (qm == std::string::npos) {
    path = reqFile;
  } else {
    path = reqFile.substr(0, qm);
    query = reqFile.substr(qm);
  }

  struct in_addr ipAddr = client.sin_addr;
  char * addr = inet_ntoa(ipAddr);
  std::string host = addr;
  std::string n_entry = host + "\t\t" + path + "\r\n";

  FILE * logs_f = fopen("logs.txt", "a");
  fprintf(logs_f, "%s", n_entry.c_str());
  fclose(logs_f);

  if (path == "/stats") {
    processStatsRequest(fd);
    return "";
  }

  if (path == "/logs") {
    processLogsRequest(fd);
    return "";
  }

  if (path == "/") {
    path += "index.html";
  }

  char actual_resolved_path[1024];
  realpath("http-root-dir", actual_resolved_path);
  int actual_resolved_path_len = strlen(actual_resolved_path);

  std::string parent = path.substr(0, path.find_last_of('/', path.size() - 2) + 1);
  if (parent == "") parent = "/";

  std::string orig_path = path;
  path = "http-root-dir/htdocs" + path;

  DIR * dir = opendir(path.c_str());
  if (dir) {
    char req_resolved_path[1024];
    realpath(path.c_str(), req_resolved_path);
    int req_resolved_path_len = strlen(req_resolved_path);

    if (req_resolved_path_len <= actual_resolved_path_len || strncmp(req_resolved_path, actual_resolved_path, actual_resolved_path_len) != 0) {
      const char * err = "HTTP/1.1 403 Forbidden\r\nServer: CS 252 lab5\"\r\n\r\n";
      write(fd, err, strlen(err));
      closedir(dir);
      return "";
    }

    processDirectoryRequest(fd, orig_path, path, query, parent, dir);
    closedir(dir);
    return orig_path;
  }

  FILE * fptr = fopen(path.c_str(), "rb");
  if (!fptr) {
    path = "http-root-dir" + orig_path;

    dir = opendir(path.c_str());
    if (dir) {
      char req_resolved_path[1024];
      realpath(path.c_str(), req_resolved_path);
      int req_resolved_path_len = strlen(req_resolved_path);

      if (req_resolved_path_len <= actual_resolved_path_len || strncmp(req_resolved_path, actual_resolved_path, actual_resolved_path_len) != 0) {
        const char * err = "HTTP/1.1 403 Forbidden\r\nServer: CS 252 lab5\"\r\n\r\n";
        write(fd, err, strlen(err));
        closedir(dir);
        return "";
      }

      processDirectoryRequest(fd, orig_path, path, query, parent, dir);
      closedir(dir);
      return orig_path;
    }

    fptr = fopen(path.c_str(), "rb");
    if (!fptr) {
      const char * err = "HTTP/1.1 404 File Not Found\r\nServer: CS 252 lab5\r\nContent-type: text/html\r\n\r\n<html><body><h1>404 Not Found</h1></body></html>";
      write(fd, err, strlen(err));
      return orig_path;
    }

    char req_resolved_path[1024];
    realpath(path.c_str(), req_resolved_path);
    int req_resolved_path_len = strlen(req_resolved_path);

    if (req_resolved_path_len <= actual_resolved_path_len || strncmp(req_resolved_path, actual_resolved_path, actual_resolved_path_len) != 0) {
      const char * err = "HTTP/1.1 403 Forbidden\r\nServer: CS 252 lab5\"\r\n\r\n";
      write(fd, err, strlen(err));
      fclose(fptr);
      return "";
    }

    if (path.find("/cgi-bin/") != std::string::npos) {
      fclose(fptr);
      processCgiBinRequest(fd, path, query);
      return orig_path;
    }
  }

  std::string ext = reqFile.substr((int) reqFile.find_last_of('.') + 1);
  if (ext == "gif" || ext == "png") {
    ext = "image/" + ext;
  } else {
    ext = "text/html";
  }

  fseek(fptr, 0, SEEK_END);
  int fsize = ftell(fptr);
  fseek(fptr, 0, SEEK_SET);

  char * fcontent = (char *) calloc(fsize + 1, sizeof(char));
  fread(fcontent, sizeof(char), fsize, fptr);
  fcontent[fsize] = 0;

  std::string resp = "HTTP/1.1 200 OK\r\nServer: CS 252 lab5\r\nContent-type: " + ext + "\r\n\r\n";
  write(fd, resp.c_str(), resp.size());
  write(fd, fcontent, fsize);

  free(fcontent);
  fclose(fptr);

  return orig_path;
}


typedef struct file_desc {
  std::string name;
  time_t last_modified;
  unsigned char type;
  off_t size;

  std::string getImage() {
    if (type == DT_DIR) return "/icons/menu.gif";
    if (name.find(".gif") == name.size() - 4) return "/icons/image.gif";
    return "/icons/unknown.gif";
  }
} file_desc_t;


void processDirectoryRequest(int fd, std::string orig_path,
                             std::string path, std::string query,
                             std::string parent, DIR * dir) {

  if (path.back() != '/') path += '/';
  if (orig_path.back() != '/') orig_path += '/';
  
  char column, order, opp_order;

  size_t col = query.find("C=");
  if (col != std::string::npos && col < query.size() - 2) column = query[col + 2];

  size_t ord = query.find("O=");
  if (ord != std::string::npos && ord < query.size() - 2) order = query[ord + 2];

  std::string col_options = "NMS";
  if (col_options.find(column) == std::string::npos) column = 'N';

  if (order == 'A') opp_order = 'D';
  else if (order == 'D') opp_order = 'A';
  else {
    order = 'A';
    opp_order = 'D';
  }

  std::string resp = "HTTP/1.1 200 OK\r\nServer: CS 252 lab5\r\nContent-type: text/html\r\n\r\n";
  resp = resp + "<html><head><title>Index of " + path + "</title></head>";
  resp = resp + "<body><h1>Index of " + path + "</h1><table><tbody><tr>";
  resp += "<th valign=\"top\"></th>";
  
  resp = resp + "<th><a href=\"?C=N;O=" + ((column == 'N') ? opp_order : 'A') + "\">Name</a></th>";
  resp = resp + "<th><a href=\"?C=M;O=" + ((column == 'M') ? opp_order : 'A') + "\">Last modified</a></th>";
  resp = resp + "<th><a href=\"?C=S;O=" + ((column == 'S') ? opp_order : 'A') + "\">Size</a></th>";

  resp += "</tr><tr><th colspan=\"4\"><hr></th></tr>";

  resp += "<tr><td valign=\"top\">..</td>";
  resp = resp + "<td><a href=\"" + parent + "\">Parent Directory</a></td><td>&nbsp;</td>";
  resp += "<td align=\"right\">-</td></tr>";

  std::vector<file_desc_t> files;

  struct dirent * entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    std::string entrypath = path + entry->d_name;

    struct stat filestat;
    if (stat(entrypath.c_str(), &filestat) == -1) {
      continue;
    }

    file_desc_t file;
    file.name = entry->d_name;
    file.last_modified = filestat.st_mtime;
    file.type = entry->d_type;

    if (file.type == DT_DIR) file.size = -1;
    else file.size = filestat.st_size;

    files.push_back(file);
  }

  bool (*comp)(const file_desc_t &, const file_desc_t &);

  if (column == 'M') comp = [](const file_desc_t & a, const file_desc_t & b) { return a.last_modified < b.last_modified; };
  else if (column == 'S') comp = [](const file_desc_t & a, const file_desc_t & b) { return (long long) a.size < (long long) b.size; };
  else comp = [](const file_desc_t & a, const file_desc_t & b) { return a.name < b.name; };

  if (order == 'A') std::sort(files.begin(), files.end(), comp);
  else std::sort(files.rbegin(), files.rend(), comp);

  for (auto file : files) {
    resp = resp + "<tr><td valign=\"top\"><img src=\"" + file.getImage() + "\" alt=\"[   ]\"></td>";
    resp = resp + "<td><a href=\"" + orig_path + file.name + "\">" + file.name + "</a></td>";

    std::string timstr;
    {
      struct tm * tm_info = localtime(&file.last_modified);
      char buff[30] = {0};
      strftime(buff, 30, "%Y-%m-%d %H:%M", tm_info);
      timstr = buff;
    }

    resp = resp + "<td align=\"right\">" + timstr + "</td>";

    long long sz = (long long) file.size;
    std::string szstr;
    if (sz < 100) szstr = std::to_string(sz) + "B";
    else {
      char buff[20] = {0};
      if (sz < 100000) sprintf(buff, "%.1fK", sz / 1024.0);
      else sprintf(buff, "%.1fM", sz / (1024.0 * 1024.0));
      szstr = buff;
    }

    resp = resp + "<td align=\"right\">" + ((file.type == DT_DIR) ? "-" : szstr) + "</td>";
    resp += "</tr>";
  }

  resp += "</tr><tr><th colspan=\"4\"><hr></th></tr>";
  resp += "</tbody></table></body></html>";
  write(fd, resp.c_str(), resp.size());
}

void processCgiBinRequest(int fd, std::string path, std::string query) {
  pid_t pid = fork();
  if (pid == -1) {
    perror("fork");
    exit(-1);
  } else if (pid == 0) {
    setenv("REQUEST_METHOD", "GET", 1);

    if (query.empty()) setenv("QUERY_STRING", "", 1);
    else setenv("QUERY_STRING", query.c_str() + 1, 1);

    dup2(fd, 1);

    const char * resp = "HTTP/1.1 200 OK\r\nServer: CS 252 lab5\r\n";
    write(1, resp, strlen(resp));

    const char * args[2];
    args[0] = path.c_str();
    args[1] = NULL;

    execv(args[0], (char * const *) args);
    perror("execv");
    exit(-1);
  }
}

void processStatsRequest(int fd) {
  const char * resp = "HTTP/1.1 200 OK\r\nServer: CS 252 lab5\r\n\r\n"
                      "The names of the student who wrote the project: Swastik Agarwala\r\n"
                      "The time the server has been up: %ds\r\n"
                      "The number of requests since the server started: %d\r\n"
                      "The minimum service time and the URL request that took this time: %s\r\n"
                      "The maximum service time and the URL request that took this time: %s";

  std::string min_req = min_req_time_url + " (" + std::to_string(min_req_time) + ")";
  if (min_req_time_url.empty()) min_req = "-";

  std::string max_req = max_req_time_url + " (" + std::to_string(max_req_time) + ")";
  if (max_req_time_url.empty()) max_req = "-";

  char buff[1000];
  sprintf(buff, resp, time(NULL) - start_time,
                      req_count,
                      min_req.c_str(),
                      max_req.c_str());

  write(fd, buff, strlen(buff));

}

void processLogsRequest(int fd) {
  const char * resp = "HTTP/1.1 200 OK\r\nServer: CS 252 lab5\r\n\r\n";
  write(fd, resp, strlen(resp));

  FILE * fptr = fopen("logs.txt", "rb");
  fseek(fptr, 0, SEEK_END);
  int fsize = ftell(fptr);
  fseek(fptr, 0, SEEK_SET);

  char * fcontent = (char *) calloc(fsize + 1, sizeof(char));
  fread(fcontent, sizeof(char), fsize, fptr);
  fcontent[fsize] = 0;

  write(fd, fcontent, fsize);

  free(fcontent);
  fclose(fptr);
}
