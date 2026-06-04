#include "status_servlet.h"

#include <iomanip>

#include "sylar/fiber.h"
#include "sylar/iomanager.h"
#include "sylar/log.h"
#include "sylar/util.h"

namespace sylar {
namespace http {

namespace {

const uint64_t s_start_time_ms = sylar::GetCurrentMS();

const char* GetFiberTypeStr() {
    if (FIBER_CONTEXT_TYPE == FIBER_UCONTEXT) {
        return "FIBER_UCONTEXT";
    } else if (FIBER_CONTEXT_TYPE == FIBER_FCONTEXT) {
        return "FIBER_FCONTEXT";
    } else if (FIBER_CONTEXT_TYPE == FIBER_LIBCO) {
        return "FIBER_LIBCO";
    } else if (FIBER_CONTEXT_TYPE == FIBER_LIBACO) {
        return "FIBER_LIBACO";
    }
    return "UNKNOW";
}

std::string FormatUsedTime(uint64_t ms) {
    uint64_t ts = ms / 1000;
    std::stringstream ss;
    bool v = false;
    if (ts >= 3600 * 24) {
        ss << (ts / 3600 / 24) << "d ";
        ts = ts % (3600 * 24);
        v = true;
    }
    if (ts >= 3600) {
        ss << (ts / 3600) << "h ";
        ts = ts % 3600;
        v = true;
    } else if (v) {
        ss << "0h ";
    }
    if (ts >= 60) {
        ss << (ts / 60) << "m ";
        ts = ts % 60;
    } else if (v) {
        ss << "0m ";
    }
    ss << ts << "s";
    return ss.str();
}

}  // namespace

StatusServlet::StatusServlet() : Servlet("StatusServlet") {}

int32_t StatusServlet::handle(sylar::http::HttpRequest::ptr request,
                              sylar::http::HttpResponse::ptr response,
                              sylar::SocketStream::ptr session) {
    (void)session;
    response->setHeader("Content-Type", "text/plain; charset=utf-8");

#define XX(key) ss << std::setw(24) << std::right << key ": "
    std::stringstream ss;
    ss << "================ IO/HTTP demo status ================" << std::endl;
    XX("server_version") << "sylar/1.0.0" << std::endl;
    XX("request_method") << HttpMethodToString(request->getMethod()) << std::endl;
    XX("request_path") << request->getPath() << std::endl;
    XX("thread") << sylar::Thread::GetName() << std::endl;
    XX("fiber_type") << GetFiberTypeStr() << std::endl;
    XX("fibers") << sylar::Fiber::TotalFibers() << std::endl;
    XX("iomanager") << (sylar::IOManager::GetThis() ? "running" : "none") << std::endl;
    XX("uptime") << FormatUsedTime(sylar::GetCurrentMS() - s_start_time_ms) << std::endl;
    ss << "================ logger config ======================" << std::endl;
    ss << sylar::LoggerMgr::GetInstance()->toYamlString() << std::endl;
#undef XX

    response->setBody(ss.str());
    return 0;
}

}  // namespace http
}  // namespace sylar
