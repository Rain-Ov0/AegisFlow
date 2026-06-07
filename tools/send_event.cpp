#include <boost/beast.hpp>
#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <exception>

#include "event.pb.h"
#include "decision.pb.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;

using tcp = asio::ip::tcp;

// 获取当前时间戳（毫秒级）
static uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count()
    );
}

// 转换决策操作为字符串
static const char* actionToString(aegisflow::v1::DecisionAction action) {
    switch (action) {
        case aegisflow::v1::PASS:
            return "PASS";
        case aegisflow::v1::REVIEW:
            return "REVIEW";
        case aegisflow::v1::REJECT:
            return "REJECT";
        default:
            return "UNKNOWN";
    }
}

// 客户端发送事件
/*
1. 构造事件请求体
2. 序列化请求体
3. 发送请求
4. 解析响应体
5. 打印响应结果
*/
int main(int argc, char* argv[]) {
    try {
        GOOGLE_PROTOBUF_VERIFY_VERSION;
        std::string host = "127.0.0.1";
        std::string port = "8080";
        std::string user_id = "user_001";

        if (argc >= 2) {
            host = argv[1];
        }

        if (argc >= 3) {
            port = argv[2];
        }

        if (argc >= 4) {
            std::string arg = argv[3];
            if (arg == "--empty-user") {
                user_id = "";
            } else {
                user_id = arg;
            }
        }

        const std::string target = "/v1/event/report";
        const int http_version = 11;

        aegisflow::v1::ReportEventRequest request_pb;

        auto* event = request_pb.mutable_event();
        event->set_event_id(1001);
        event->set_timestamp_ms(nowMs());
        event->set_user_id(user_id);
        event->set_ip("127.0.0.1");
        event->set_device_id("device_demo_001");
        event->set_scene("scene_demo_001");
        event->set_type(aegisflow::v1::LOGIN);
        event->set_result(aegisflow::v1::SUCCESS);

        // 序列化请求体
        std::string request_body;
        if (!request_pb.SerializeToString(&request_body)) {
            std::cerr << "Failed to serialize request body" << std::endl;
            return 1;
        }

        asio::io_context io_context;

        tcp::resolver resolver(io_context);
        beast::tcp_stream stream(io_context);

        auto results = resolver.resolve(host, port);
        stream.connect(results);

        http::request<http::string_body> req {
            http::verb::post,
            target,
            http_version
        };

        req.set(http::field::host, host + ":" + port);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/x-protobuf");
        req.set(http::field::accept, "*application/x-protobuf");

        req.body() = std::move(request_body);
        req.prepare_payload();

        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);

        if (res.result() != http::status::ok) {
            std::cerr << "sever returned HTTP error: "
                << res.result_int()
                << " "
                << res.reason()
                << std::endl;

            if (!res.body().empty()) {
                std::cerr << "response body: " << res.body() << std::endl;
            }

            return 1;
        }

        aegisflow::v1::ReportEventResponse response_pb;

        // 解析响应体
        if (!response_pb.ParseFromString(res.body())) {
            std::cerr << "prase ReportEventResponse failed" << std::endl;
            return 1;
        }

        const auto& decision = response_pb.decision();

        std::cout << "event_id=" << decision.event_id() << std::endl
            << "user_id=" << decision.user_id() << std::endl
            << "action=" << actionToString(decision.action()) << std::endl
            << "risk_score=" << decision.risk_score() << std::endl
            << "cost_us=" << decision.cost_us() 
            << std::endl;

        const auto& features = decision.features();

        std::cout << "features.user_login_1m=" << features.user_login_1m() << std::endl
            << "features.user_login_5m=" << features.user_login_5m() << std::endl
            << "features.user_login_1h=" << features.user_login_1h() << std::endl
            << "features.user_login_fail_5m=" << features.user_login_fail_5m()
            << std::endl;
        
        for (const auto& reason : decision.reasons()) {
            std::cout << "reason code=" << reason.code()
                << " message=" << reason.message()
                << " severity=" << reason.severity()
                << std::endl;
        }

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if (ec && ec != beast::errc::not_connected) {
            throw beast::system_error{ec};
        }

        google::protobuf::ShutdownProtobufLibrary();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "send_event failed: " << e.what() << std::endl;
        return 1;
    }
}