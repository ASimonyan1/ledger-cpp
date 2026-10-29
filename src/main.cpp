#include "ledger/ledger.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

using json = nlohmann::json;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
std::string env(const char* name, std::string fallback = {}) {
    const char* p = std::getenv(name); return p ? p : std::move(fallback);
}
void reply(httplib::Response& res, int status, const json& body) {
    res.status=status; res.set_content(body.dump(), "application/json");
}
json body(const httplib::Request& req, std::initializer_list<const char*> fields) {
    auto j=json::parse(req.body);
    if(!j.is_object() || j.size()!=fields.size()) throw ledger::Error(400,"invalid_fields");
    for(auto field:fields) if(!j.contains(field)) throw ledger::Error(400,"invalid_fields");
    return j;
}
std::string text(const json& j, const char* field) {
    if(!j.at(field).is_string()) throw ledger::Error(400,"expected_string");
    return j.at(field).get<std::string>();
}
std::int64_t amount(const json& j, const char* field, std::int64_t max) {
    const auto& n=j.at(field);
    if(!n.is_number_integer() || n<0 || n>max) throw ledger::Error(400,"expected_bounded_integer");
    return n.get<std::int64_t>();
}
template<class F> auto guarded(F f) {
    return [f](const httplib::Request& req, httplib::Response& res) {
        try { f(req,res); }
        catch(const ledger::Error& e) { reply(res,e.status,{{"error",e.what()}}); }
        catch(const json::exception&) { reply(res,400,{{"error","invalid_json"}}); }
        catch(const std::exception&) { reply(res,500,{{"error","internal_error"}}); }
    };
}
}
int main() {
    try {
        const auto token=env("API_TOKEN");
        if(token.size()<16) throw std::runtime_error("API_TOKEN must contain at least 16 characters");
        const auto dsn=env("DATABASE_URL");
        if(dsn.empty()) throw std::runtime_error("DATABASE_URL is required (include connect_timeout=3)");
        ledger::Ledger service(dsn);
        httplib::Server server;
        server.new_task_queue=[] { return new httplib::ThreadPool(8,8,32); };
        server.set_read_timeout(2); server.set_write_timeout(2);
        server.set_keep_alive_max_count(20); server.set_payload_max_length(4096);
        server.set_pre_routing_handler([&](const auto& req, auto& res) {
            if(req.path!="/health" && req.get_header_value("Authorization")!="Bearer "+token) {
                reply(res,401,{{"error","unauthorized"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/health",guarded([&](const auto&, auto& res) {
            const bool ready=service.healthy(); reply(res,ready?200:503,{{"ready",ready}});
        }));
        server.Post("/v1/accounts",guarded([&](const auto& req, auto& res) {
            const auto j=body(req,{"id","opening_balance"});
            const auto a=service.create_account(text(j,"id"),amount(j,"opening_balance",ledger::max_balance));
            reply(res,201,{{"id",a.id},{"balance",a.balance}});
        }));
        server.Get(R"(/v1/accounts/([A-Za-z0-9_-]+))",guarded([&](const auto& req, auto& res) {
            const auto a=service.account(req.matches[1]); reply(res,200,{{"id",a.id},{"balance",a.balance}});
        }));
        server.Get(R"(/v1/accounts/([A-Za-z0-9_-]+)/entries)",guarded([&](const auto& req, auto& res) {
            json result=json::array();
            for(const auto& e:service.entries(req.matches[1])) result.push_back({{"transfer_key",e.key},{"delta",e.delta}});
            reply(res,200,{{"entries",result},{"limit",100}});
        }));
        server.Post("/v1/transfers",guarded([&](const auto& req, auto& res) {
            const auto j=body(req,{"from","to","amount"});
            const auto t=service.transfer(req.get_header_value("Idempotency-Key"),text(j,"from"),text(j,"to"),amount(j,"amount",ledger::max_transfer));
            reply(res,t.replayed?200:201,{{"key",t.key},{"from",t.from},{"to",t.to},{"amount",t.amount},{"replayed",t.replayed}});
        }));
        server.set_error_handler([](const auto&,auto& res) {
            if(res.body.empty()) reply(res,res.status,{{"error","http_error"}});
        });
        std::signal(SIGINT,on_signal); std::signal(SIGTERM,on_signal);
        const int port=std::stoi(env("PORT","8080"));
        if(port<1 || port>65535) throw std::runtime_error("Invalid PORT");
        if(!server.bind_to_port(env("HOST","127.0.0.1"),port)) throw std::runtime_error("Cannot bind HTTP port");
        std::jthread watcher([&](std::stop_token stop) {
            while(!stop.stop_requested() && !interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if(interrupted) server.stop();
        });
        std::cout<<"ledger listening on port "<<port<<std::endl;
        const bool ok=server.listen_after_bind(); watcher.request_stop();
        return ok?0:1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
