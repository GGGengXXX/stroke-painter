// Minimal HTTP server (POSIX sockets, no deps).
//   GET  /            -> index.html
//   POST /paint       -> body: raw RGB bytes (w*h*3), headers: X-Width, X-Height, X-Rounds
//                        response: JSON list of strokes
#include "painter.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static std::string toJson(const std::vector<Stroke>& strokes, int w, int h) {
    std::ostringstream o;
    o << "{\"width\":" << w << ",\"height\":" << h << ",\"strokes\":[";
    for (size_t i = 0; i < strokes.size(); ++i) {
        const Stroke& s = strokes[i];
        if (i) o << ',';
        o << "{\"round\":" << s.round << ",\"radius\":" << s.radius
          << ",\"color\":[" << (int)s.color.r << ',' << (int)s.color.g << ',' << (int)s.color.b << "],\"pts\":[";
        for (size_t j = 0; j < s.pts.size(); ++j) {
            if (j) o << ',';
            o << '[' << s.pts[j].first << ',' << s.pts[j].second << ']';
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

static void sendAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) break;
        off += n;
    }
}

static void respond(int fd, int code, const std::string& type, const std::string& body) {
    std::ostringstream h;
    h << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error") << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << "Connection: close\r\n\r\n";
    sendAll(fd, h.str() + body);
}

static std::string header(const std::string& head, const std::string& name) {
    std::string lower = head;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    std::string key = name;
    for (auto& c : key) c = (char)tolower((unsigned char)c);
    size_t p = lower.find("\n" + key + ":");
    if (p == std::string::npos) return "";
    size_t s = p + key.size() + 2, e = head.find('\r', s);
    if (e == std::string::npos) e = head.find('\n', s);
    return head.substr(s, e - s);
}

static void handle(int fd) {
    std::string buf;
    char tmp[65536];
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return;
        buf.append(tmp, n);
        hdrEnd = buf.find("\r\n\r\n");
    }
    std::string head = buf.substr(0, hdrEnd);
    std::string body = buf.substr(hdrEnd + 4);
    std::istringstream line(head);
    std::string method, path;
    line >> method >> path;

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", readFile("index.html"));
        return;
    }
    if (method == "POST" && path == "/paint") {
        int w = std::stoi(header(head, "X-Width"));
        int h = std::stoi(header(head, "X-Height"));
        int rounds = header(head, "X-Rounds").empty() ? 5 : std::stoi(header(head, "X-Rounds"));
        int detail = header(head, "X-Detail").empty() ? 3 : std::stoi(header(head, "X-Detail"));
        size_t need = (size_t)w * h * 3;
        size_t clen = std::stoul(header(head, "Content-Length"));
        while (body.size() < clen) {
            ssize_t n = recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            body.append(tmp, n);
        }
        if (body.size() < need || w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            respond(fd, 400, "text/plain", "bad image dimensions");
            return;
        }
        Image img;
        img.w = w; img.h = h;
        img.px.resize((size_t)w * h);
        for (size_t i = 0; i < (size_t)w * h; ++i)
            img.px[i] = {(float)(uint8_t)body[3 * i], (float)(uint8_t)body[3 * i + 1], (float)(uint8_t)body[3 * i + 2]};

        PainterParams P = makeParams(w, detail, rounds);
        auto strokes = paint(img, P);
        respond(fd, 200, "application/json", toJson(strokes, w, h));
        return;
    }
    respond(fd, 404, "text/plain", "not found");
}

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(srv, (sockaddr*)&addr, sizeof addr) < 0 || listen(srv, 16) < 0) {
        perror("bind/listen");
        return 1;
    }
    std::cout << "Serving on http://127.0.0.1:" << port << "\n";
    for (;;) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) continue;
        handle(fd);
        close(fd);
    }
}
