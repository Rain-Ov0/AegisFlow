#include "aegisflow/net/http_server.hpp"
#include "aegisflow/log/logger.hpp"

#include <boost/beast.hpp>
#include <boost/asio.hpp>

#include <iostream>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

namespace aegisflow::net {
namespace {

    using StringRequest = http::request<http::string_body>;
    using StringResponse = http::response<http::string_body>;

    // 构造200 ok 响应
    StringResponse makeHealthResponse(
        const StringRequest& request
    ) {
        StringResponse response {
            http::status::ok,
            request.version()
        };

        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"status": "ok"})";
        response.prepare_payload();

        return response;
    }

    // 构造404 not found 响应
    StringResponse makeNotFoundResponse(
        const StringRequest& request
    ) {
        StringResponse response {
            http::status::not_found,
            request.version()
        };
        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"status": "not found"})";
        response.prepare_payload();

        return response;
    }
} // namespace

// 处理事件报告请求
HttpServer::StringResponse HttpServer::handleReportEvent(
    const StringRequest& request
) {
    LOG_INFO("request: received: POST /v1/event/report");
    const auto content_type = request[http::field::content_type];

    const bool is_protobuf = 
        content_type.find("application/x-protobuf") != beast::string_view::npos || 
        content_type.find("application/octet-stream") != beast::string_view::npos;

    // 检查内容类型是否为 protobuf
    if (!is_protobuf) {
        StringResponse response{
            http::status::unsupported_media_type,
            request.version()
        };

        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"error": "content-type must be application/x-protobuf"})";
        response.prepare_payload();

        LOG_WARN("invalid content-type");

        return response;
    }

    // 检查请求体大小是否超过最大限制 
    if (request.body().size() > max_body_size_) {
        StringResponse response{
            http::status::payload_too_large,
            request.version()
        };

        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"error": "request body too large"})";
        response.prepare_payload();

        LOG_WARN("request body too large");

        return response;
    }

    aegisflow::v1::ReportEventRequest pb_request;

    // 解析 protobuf 请求体
    if (!pb_request.ParseFromString(request.body())) {
        StringResponse response{
            http::status::bad_request,
            request.version()
        };

        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"error": "protobuf parse error"})";
        response.prepare_payload();

        LOG_WARN("protobuf parse failed");

        return response;
    }

        if (!pb_request.has_event()) {
        StringResponse response{
            http::status::bad_request,
            request.version()
        };

        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"error":"missing event"})";
        response.prepare_payload();

        LOG_WARN("missing event in ReportEventRequest");

        return response;
    }

    auto pb_response = risk_service_.handleEvent(pb_request);
    std::string response_body;
    // 序列化 protobuf 响应体
    if (!pb_response.SerializeToString(&response_body)) {
        StringResponse response {
            http::status::internal_server_error,
            request.version()
        };

        response.set(http::field::server, "aegisflow");
        response.set(http::field::content_type, "application/json");
        response.keep_alive(request.keep_alive());
        response.body() = R"({"error": "protobuf serialize error"})";
        response.prepare_payload();

        LOG_ERROR("protobuf serialize failed");

        return response;
    }

    // 构造200 ok 响应

    StringResponse response {
        http::status::ok,
        request.version()
    };

    response.set(http::field::server, "aegisflow");
    response.set(http::field::content_type, "application/x-protobuf");
    response.keep_alive(request.keep_alive());
    response.body() = std::move(response_body);
    response.prepare_payload();

    const auto& decision = pb_response.decision();
    LOG_INFO("response decision action=" + 
    aegisflow::v1::DecisionAction_Name(decision.action()) + 
    " event_id=" + 
    std::to_string(decision.event_id())
     );


    return response;
}

// 处理请求
HttpServer::StringResponse HttpServer::handleRequest(
    const StringRequest& request
) {
    if (request.method() == http::verb::get && 
    request.target() == "/health") {
        return makeHealthResponse(request);
    } else if (request.method() == http::verb::post && 
    request.target() == "/v1/event/report") {
        return handleReportEvent(request);
    }

    return makeNotFoundResponse(request);

}

HttpServer::HttpServer(
    std::string host, 
    uint16_t port, 
    int io_threads, 
    uint64_t max_body_size,
    aegisflow::app::RiskService& risk_service
)
    : host_(std::move(host)), 
    port_(port),
    io_threads_(std::max(1, io_threads)),
    max_body_size_(max_body_size),
    risk_service_(risk_service) {}

void HttpServer::run() {
    try {
        asio::io_context io_context{1};
        auto address = asio::ip::make_address(host_);
        tcp::endpoint endpoint{address, port_};
        tcp::acceptor acceptor{io_context};
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(asio::socket_base::max_listen_connections);
        asio::thread_pool workers{
            static_cast<size_t>(io_threads_)
        };

        LOG_INFO("server started on " + 
            host_ + 
            ":" + 
            std::to_string(port_) + 
            ", io_threads=" + 
            std::to_string(io_threads_)
        );

        for (;;) {
            tcp::socket socket{io_context};
            acceptor.accept(socket);
            auto socket_ptr = std::make_shared<tcp::socket>(
                std::move(socket)
            );

            asio::post(
                workers,
                [this, socket_ptr]() mutable {
                    doSession(std::move(*socket_ptr));
                }
            );
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("server run failed: ") + e.what());
        throw;
    }
}

// 处理会话
void HttpServer::doSession(TcpSocket socket) {
    beast::error_code ec;
    beast::flat_buffer buffer;

    for (;;) {
        http::request_parser<http::string_body> parser;
        parser.body_limit(max_body_size_);
        http::read(socket, buffer, parser, ec);
        if (ec == http::error::end_of_stream) {
            break;
        }
        
        if (ec == http::error::body_limit) {
            StringResponse response {
                http::status::payload_too_large,
                11
            };

            response.set(http::field::server, "aegisflow");
            response.set(http::field::content_type, "application/json");
            response.keep_alive(false);
            response.body() = R"({"error":"request body too large"})";
            response.prepare_payload();

            http::write(socket, response, ec);

            LOG_WARN("request body too large");

            break;
        }

        if (ec) {
            break;
        }

        StringRequest request = parser.release();
        StringResponse response = handleRequest(request);
        bool keep_alive = response.keep_alive();
        http::write(socket, response, ec);

        if (ec) {
            LOG_WARN(std::string("http write failed: ") + ec.message());
            break;
        }

        if (!keep_alive) {
            break;
        }
    }


    socket.shutdown(tcp::socket::shutdown_both);
}

} //namespace aegisflow::net