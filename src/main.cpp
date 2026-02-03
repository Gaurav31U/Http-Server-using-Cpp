#include <iostream>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <fstream>
#include <sstream>
#include <zlib.h>

constexpr size_t BUFFER_SIZE = 4096;

std::string g_directory = "";

// --- Helper: GZIP Compression ---
std::string gzip_compress(const std::string& data) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));

    // windowBits = 15 + 16 (gzip header)
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return "";
    }

    zs.next_in = (Bytef*)data.data();
    zs.avail_in = data.size();

    int ret;
    char outbuffer[32768];
    std::string compressed;

    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (compressed.size() < zs.total_out) {
            compressed.append(outbuffer, zs.total_out - compressed.size());
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        return "";
    }

    return compressed;
}

// --- Helper: Extract Header Value ---
std::string extract_header_value(const std::string& request, const std::string& header_name) {
    std::string search = header_name + ": ";
    auto start_pos = std::search(
        request.begin(), request.end(),
        search.begin(), search.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); }
    );

    if (start_pos == request.end()) return "";

    size_t start_idx = std::distance(request.begin(), start_pos) + search.length();
    size_t end_idx = request.find("\r\n", start_idx);
    
    if (end_idx == std::string::npos) return "";

    return request.substr(start_idx, end_idx - start_idx);
}

// --- Helper: Sending Responses ---
void send_raw(int client_fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(client_fd, data.c_str() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "Send failed: " << std::strerror(errno) << "\n";
            return;
        }
        if (n == 0) return; // Connection closed
        sent += n;
    }
}

// --- Logic: GET Requests ---
void handle_get(int client_fd, const std::string& path, const std::string& req_str, bool close_requested) {
    
    // 1. Root
    if (path == "/") {
        send_raw(client_fd, "HTTP/1.1 200 OK\r\n\r\n");
        return;
    }

    // 2. Echo
    if (path.rfind("/echo/", 0) == 0) { // starts_with
        std::string content = path.substr(6);
        std::string encoding = extract_header_value(req_str, "Accept-Encoding");
        
        bool use_gzip = false;
        // Check for 'gzip' in the comma separated list
        if (encoding.find("gzip") != std::string::npos) { 
             use_gzip = true;
        }

        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
        
        if (use_gzip) {
            std::string compressed = gzip_compress(content);
            // If compression worked, swap content. If not, fallback to plain.
            if (!compressed.empty()) {
                content = compressed;
                response += "Content-Encoding: gzip\r\n";
            }
        }

        response += "Content-Length: " + std::to_string(content.size()) + "\r\n";
        if (close_requested) response += "Connection: close\r\n";
        response += "\r\n";
        response += content;
        
        send_raw(client_fd, response);
        return;
    }

    // 3. User-Agent
    if (path == "/user-agent") {
        std::string agent = extract_header_value(req_str, "User-Agent");
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
        response += "Content-Length: " + std::to_string(agent.size()) + "\r\n\r\n" + agent;
        send_raw(client_fd, response);
        return;
    }

    // 4. Files
    if (path.rfind("/files/", 0) == 0) {
        std::string filename = path.substr(7);
        std::string filepath = g_directory + "/" + filename;
        
        std::ifstream file(filepath, std::ios::binary);
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
            response += "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content;
            send_raw(client_fd, response);
        } else {
            send_raw(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n");
        }
        return;
    }

    send_raw(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n");
}

// --- Logic: POST Requests ---
void handle_post(int client_fd, const std::string& path, const std::string& body) {
    if (path.rfind("/files/", 0) == 0) {
        std::string filename = path.substr(7);
        std::string filepath = g_directory + "/" + filename;
        
        std::ofstream file(filepath, std::ios::binary);
        if (file) {
            file << body;
            send_raw(client_fd, "HTTP/1.1 201 Created\r\n\r\n");
        } else {
            send_raw(client_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
        }
        return;
    }
    
    send_raw(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n");
}

// --- Request Parsing Logic ---
// Returns string containing complete request if found in buffer, else empty string
std::string try_parse_request(std::string& buffer) {
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos) return ""; // Headers not fully received yet

    // Include the delimiter size
    size_t headers_size = header_end + 4;
    
    // Parse headers just to find Content-Length
    std::string headers = buffer.substr(0, header_end);
    std::string cl_str = extract_header_value(headers, "Content-Length");
    size_t content_len = 0;
    if (!cl_str.empty()) {
        try {
            content_len = std::stoul(cl_str);
        } catch(...) { content_len = 0; }
    }

    size_t total_size = headers_size + content_len;

    // Do we have the full body?
    if (buffer.size() >= total_size) {
        std::string request = buffer.substr(0, total_size);
        buffer.erase(0, total_size); // Remove processed request from buffer
        return request;
    }

    return ""; // Need more data
}


// --- Main Client Handler Loop ---
void handle_client(int client_fd) {
    std::string accum_buffer;
    char buffer[BUFFER_SIZE];

    while (true) {
        // 1. Read available data into accumulator
        ssize_t bytes_read = ::recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            accum_buffer.append(buffer, bytes_read);
        } else if (bytes_read == 0) {
            break; // Client closed
        } else {
            // Error
            break; 
        }

        // 2. Loop to process ALL requests currently in the buffer (Pipelining support)
        while (true) {
            std::string req_str = try_parse_request(accum_buffer);
            if (req_str.empty()) break; // Waiting for more data

            // Parse Method, Path
            std::stringstream ss(req_str);
            std::string method, path;
            ss >> method >> path;

            // Extract Body (manually, as ss skips whitespace)
            size_t header_end = req_str.find("\r\n\r\n");
            std::string body = (header_end != std::string::npos) ? req_str.substr(header_end + 4) : "";

            // Check headers for Connection: close
            std::string conn_val = extract_header_value(req_str, "Connection");
            bool close_conn = (conn_val.find("close") != std::string::npos);

            // Dispatch
            if (method == "GET") {
                handle_get(client_fd, path, req_str, close_conn);
            } else if (method == "POST") {
                handle_post(client_fd, path, body);
            } else {
                send_raw(client_fd, "HTTP/1.1 405 Method Not Allowed\r\n\r\n");
            }
            
            if (close_conn) {
                close(client_fd);
                return;
            }
        }
    }

    close(client_fd);
}

int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--directory" && i + 1 < argc) {
          g_directory = argv[i + 1];
          i++;
      }
  }

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
  
  if (listen(server_fd, 5) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }
  
  std::cout << "Server listening on port 4221\n";
  
  while (true) {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
      
      if (client_fd < 0) continue;
      
      std::thread(handle_client, client_fd).detach();
  }

  close(server_fd);
  return 0;
}