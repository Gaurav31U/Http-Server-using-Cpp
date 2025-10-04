#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <vector>
#include <filesystem>

void handle_client(int client_socket,std::string &folderpath) {
    while (true) {
  
      char buffer[1024];
      ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
      if (bytes_received < 0) {
        std::cerr << "recv failed\n";
        return;
      }
      buffer[bytes_received] = '\0'; // Null-terminate the received data
      std::string body(buffer);
      std::string url_path= body.substr(body.find("GET ") + 4, body.find(" HTTP/") - (body.find("GET ") + 4));
      if(url_path == "/"){
          send(client_socket, "HTTP/1.1 200 OK\r\n\r\n", 20, 0);
      }else if(url_path.substr(0,5) == "/echo"){
          std::string message = url_path.substr(6);
          std::string response = "HTTP/1.1 200 OK\r\n";
          response += "Content-Type: text/plain\r\n";
          response += "Content-Length: " + std::to_string(message.size()) + "\r\n\r\n";
          response += message;
          send(client_socket, response.c_str(), response.size(), 0);
      }else if(url_path == "/user-agent"){
          std::string user_agent = "Unknown";
          size_t ua_pos = body.find("User-Agent: ");
          if (ua_pos != std::string::npos) {
              size_t ua_end = body.find("\r\n", ua_pos);
              user_agent = body.substr(ua_pos + 12, ua_end - (ua_pos + 12));
          }
          std::string response = "HTTP/1.1 200 OK\r\n";
          response += "Content-Type: text/plain\r\n";
          response += "Content-Length: " + std::to_string(user_agent.size()) + "\r\n\r\n";
          response += user_agent;
          send(client_socket, response.c_str(), response.size(), 0);
  
      }else if(url_path.substr(0,6) == "/files"){
          std::string filename = url_path.substr(7);
          std::vector<std::string> files;
          // how to checkout the files in the folderpath
          std::string command = "ls " + folderpath;
          for(auto & p : std::filesystem::directory_iterator(folderpath))
              files.push_back(p.path().filename());
          bool file_found = false;
          for(const auto & file : files){
              if(file == filename){
                  file_found = true;
                  break;
              }
          }
          if(file_found){
              std::ifstream file_stream(folderpath + "/" + filename, std::ios::binary);
              if(file_stream){
                  std::ostringstream ss;
                  ss << file_stream.rdbuf();
                  std::string file_content = ss.str();
                  std::string response = "HTTP/1.1 200 OK\r\n";
                  response += "Content-Type: application/octet-stream\r\n";
                  response += "Content-Length: " + std::to_string(file_content.size()) + "\r\n\r\n";
                  response += file_content;
                  send(client_socket, response.c_str(), response.size(), 0);
              }
            }else{
              send(client_socket, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
            }      
      }else{
          send(client_socket, "HTTP/1.1 404 Not Found\r\n\r\n", 26, 0);
      }
      close(client_socket);
    }
}


int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   std::cerr << "Failed to create server socket\n";
   return 1;
  }
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }
  
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(4221);
  
  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 4221\n";
    return 1;
  }
  
  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }
  
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  
  std::cout << "Waiting for a client to connect...\n";
  std::string command;
  std::string filepath;
  if(argc == 3){
      command = argv[1];
      filepath = argv[2];
  }
  while(true){
    std::cout << "Waiting for a client to connect...\n";
    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);
    std::cout << "Client connected\n";
    std::thread client_thread(handle_client, client_fd,filepath);
    client_thread.detach();
  }
  
  
  
  close(server_fd);

  return 0;
}
