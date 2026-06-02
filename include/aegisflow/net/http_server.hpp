#pragma once

#include "aegisflow/app/risk_service.hpp"
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <string>
#include <cstdint>

namespace aegisflow::net {

namespace http = boost::beast::http;

class HttpServer {
public:
    HttpServer(
        std::string host, 
        uint16_t port, 
        int io_threads, 
        uint64_t max_body_size, 
        aegisflow::app::RiskService& risk_service
    );
    void run();

private:
    using StringRequest =
        boost::beast::http::request<boost::beast::http::string_body>;

    using StringResponse =
        boost::beast::http::response<boost::beast::http::string_body>;

    using TcpSocket = boost::asio::ip::tcp::socket;

    void doSession(TcpSocket socket);
    StringResponse handleRequest(const StringRequest& request);
    StringResponse handleReportEvent(const StringRequest& request);
    
private:
    std::string host_;
    uint16_t port_;
    int io_threads_;
    uint64_t max_body_size_;
    aegisflow::app::RiskService& risk_service_;
};

} //namespace aegisflow::net