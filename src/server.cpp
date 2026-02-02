#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <functional>
#include <sstream>
#include <format>
#include <vector>
#include <thread>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <zlib.h>

std::vector<char> gzip_zlib(const std::string& in) {
  z_stream zs{};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16 /* gzip wrapper */, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    throw std::runtime_error("deflateInit2 failed");

  zs.next_in   = (Bytef*)in.data();
  zs.avail_in  = in.size();

  std::vector<char> out;
  out.resize(deflateBound(&zs, in.size()));

  zs.next_out  = (Bytef*)out.data();
  zs.avail_out = out.size();

  if (deflate(&zs, Z_FINISH) != Z_STREAM_END)
    throw std::runtime_error("deflate failed");

  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

class HTTPClientConnection {
public:
  struct HTTPResponse {
    int status_code;
    std::string reason;
    std::vector<std::string> headers;
    std::string body;

    HTTPResponse(int code, const std::string& msg)
      : status_code(code), reason(msg), headers{}, body{} {}

    HTTPResponse(const std::string& content, const std::string& content_type = "text/plain")
      : status_code(200), reason("OK"), body(content) {
      headers.push_back("Content-Type: " + content_type);
      headers.push_back(std::format("Content-Length: {}", body.size()));
    }

    std::string to_raw_string() const {
      std::ostringstream oss;
      oss << "HTTP/1.1 " << status_code << " " << reason << "\r\n";
      for (const std::string& header : headers) {
        oss << header << "\r\n";
      }
      oss << "\r\n" << body;
      return oss.str();
    }

    void set_gzip() {
      std::vector<char> tmp = gzip_zlib(body);
      body = std::string(tmp.begin(), tmp.end());
      for (int i = 0; i < headers.size(); i++) {
        if (headers[i].starts_with("Content-Length")) {
          headers[i] = std::format("Content-Length: {}", body.size());
        }
      }
      headers.push_back("Content-Encoding: gzip");
    }
  };

  HTTPClientConnection() = default;

  void acceptConnection(int server_fd) {
    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_len);
    if (client_fd < 0) {
      perror("accept");
      throw std::runtime_error("Failed to accept client connection");
    }
  }

  bool handle(const std::function<HTTPResponse(const std::string&)>& responseHandler) {
    char buffer[4096];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
      perror("read");
      return true;
    }
    if (bytes_read == 0) {
      return true;
    }
    buffer[bytes_read] = '\0';
    std::string request(buffer);

    std::cout << "Received request:\n" << request << std::endl;

    HTTPResponse response = responseHandler(request);
    std::string response_str = response.to_raw_string();
    ssize_t bytes_written = write(client_fd, response_str.c_str(), response_str.size());
    if (bytes_written < 0) {
      perror("write");
    }
    return std::find(response.headers.begin(), response.headers.end(), "Connection: close") == response.headers.end();
  }

  HTTPClientConnection(const HTTPClientConnection&) = delete;
  HTTPClientConnection& operator=(const HTTPClientConnection&) = delete;

  HTTPClientConnection(HTTPClientConnection&& other) noexcept
    : client_fd(other.client_fd)
    , client_addr(other.client_addr)
    , client_addr_len(other.client_addr_len)
  {
    other.client_fd = -1;
  }
  HTTPClientConnection& operator=(HTTPClientConnection&& other) noexcept {
    if (this != &other) {
      if (client_fd >= 0) close(client_fd);
      client_fd = other.client_fd;
      client_addr = other.client_addr;
      client_addr_len = other.client_addr_len;
      other.client_fd = -1;
    }
    return *this;
  }

  ~HTTPClientConnection() {
    if (client_fd >= 0)
      close(client_fd);
  }

private:
  int client_fd = -1;
  sockaddr_in client_addr{};
  socklen_t client_addr_len = sizeof(client_addr);
};

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   std::cerr << "Failed to create server socket\n";
   return 1;
  }
  
  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
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

  std::string directory = "."; // default
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--directory" && i + 1 < argc) {
      directory = argv[i + 1];
      ++i;
    }
  }
  std::cout << "Serving from directory: " << directory << "\n";

  auto handler = [&directory](const std::string& request) {
    std::istringstream request_iss(request);

    std::string request_line, method, path;
    std::getline(request_iss, request_line);
    std::istringstream request_line_iss(request_line);
    request_line_iss >> method >> path;

    std::unordered_map<std::string, std::string> headers;
    while (true) {
      std::string header;
      getline(request_iss, header);
      header = header.substr(0, header.size() - 1);
      if (header.empty()) {
        break;
      }
      auto colon = header.find(':');
      headers[header.substr(0, colon)] = header.substr(colon + 2);
    }

    std::string body((std::istreambuf_iterator<char>(request_iss)), std::istreambuf_iterator<char>());

    HTTPClientConnection::HTTPResponse response{404, "Not Found"};
    if (path == "/") {
      response = {200, "OK"};
    } else if (path.starts_with("/echo/")) {
      std::string content = path.substr(6);
      response = {content};
    } else if (path == "/user-agent") {
      response = {headers["User-Agent"]};
    } else if (path.starts_with("/files/")) {
      std::filesystem::path file_path = std::filesystem::path(directory) / path.substr(7);
      if (method == "GET") {
        std::ifstream file(file_path);
        if (file) {
          std::ostringstream buffer;
          buffer << file.rdbuf();
          response = {buffer.str(), "application/octet-stream"};
        } else {
          response = {404, "Not Found"};
        }
      } else {
        std::ofstream file(file_path);
        file << body;
        response = {201, "Created"};
      }
    }

    std::istringstream accept_encoding_iss(headers["Accept-Encoding"]);
    std::string encoding;
    std::vector<std::string> encodings;
    while (std::getline(accept_encoding_iss, encoding, ',')) {
      if (encoding[0] == ' ') {
        encoding = encoding.substr(1);
      }
      encodings.push_back(encoding);
    }
    if (!response.body.empty() && std::find(encodings.begin(), encodings.end(), "gzip") != encodings.end()) {
      response.set_gzip();
    }
    if (headers["Connection"] == "close") {
      response.headers.push_back("Connection: close");
    }
    return response;
  };
  
  while (true) {
    std::cout << "Waiting for a client to connect...\n";
    
    HTTPClientConnection client_conn;
    client_conn.acceptConnection(server_fd);
    std::cout << "Client connected\n";

    std::thread t([conn = std::move(client_conn), handler]() mutable {
      while (conn.handle(handler));
    });
    t.detach();
  }
  
  close(server_fd);

  return 0;
}
