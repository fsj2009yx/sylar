#include "sylar/http/http_server.h"
#include "sylar/log.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_ROOT();
sylar::IOManager::ptr worker;
void run() {
    g_logger->setLevel(sylar::LogLevel::INFO);
    sylar::Address::ptr addr = sylar::Address::LookupAnyIPAddress("0.0.0.0:18096");
    if (!addr) {
        SYLAR_LOG_ERROR(g_logger) << "get address error";
        return;
    }

    sylar::http::HttpServer::ptr http_server(new sylar::http::HttpServer(true, worker.get()));
    // sylar::http::HttpServer::ptr http_server(new sylar::http::HttpServer(true));
    bool ssl = false;
    while (!http_server->bind(addr, ssl)) {
        SYLAR_LOG_ERROR(g_logger) << "bind " << *addr << " fail";
        sleep(1);
    }

    // /bench接口，返回OK，用于测试服务器性能
    http_server->getServletDispatch()->addServlet(
        "/bench", [](sylar::http::HttpRequest::ptr request, sylar::http::HttpResponse::ptr response,
                     sylar::SocketStream::ptr session) {
            (void)request;
            (void)session;
            response->setHeader("Content-Type", "text/plain");
            response->setBody("OK\n");
            return 0;
        });

    if (ssl) {
        // http_server->loadCertificates("/home/apps/soft/sylar/keys/server.crt",
        // "/home/apps/soft/sylar/keys/server.key");
    }

    http_server->start();
}

int main(int argc, char** argv) {
    sylar::IOManager iom(1);
    worker.reset(new sylar::IOManager(4, false));
    iom.schedule(run);
    return 0;
}
