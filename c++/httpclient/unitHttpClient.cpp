#include "cmdline.hpp"
#include "httpclient.hpp"
#include <signal.h>
#define AcceptEncoding_Default "gzip, deflate"
#define AcceptLanguage_Default "zh-CN,zh;q=0.9"
#define Accept_Default "*/*"
#define UserAgent_Default "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/86.0.4240.183 Safari/537.36"
int main(int argc, char **argv) {
    logger.BasicConfig("%(thread)s %(levelname)s %(ctime)s %(message)s", tlog::detail::asctime.c_str());
    cmdline::parser CommandParse;
    CommandParse.add("help", 'h', "This is a simple http client which support http/https");
    CommandParse.add("version", 'v', "show version of client cpp");
    CommandParse.add<string>("url", 'u', "Input the request url", true, "");
    CommandParse.add<string>("type", 't', "requst url type", false, "GET", cmdline::oneof<string>("GET", "POST", "DELETE", "PUT", "HEAD", "Download"));
    CommandParse.add<bool>("enable_redirect", 0, "when response of request's url contains `location` and status code is 3xx, redirect now?", false, false, cmdline::oneof<bool>(true, false));
    CommandParse.add<bool>("verbose", 'V', "display the debug process message", false, false, cmdline::oneof<bool>(true, false));
    CommandParse.add<string>("userAgent", 0, "set request's header userAgent", false, UserAgent_Default);
    CommandParse.add<string>("acceptEncoding", 0, "set request's header AcceptEncoding", false, AcceptEncoding_Default);
    CommandParse.add<string>("acceptLanguage", 0, "set request's header AcceptLanguage", false, AcceptLanguage_Default);
    CommandParse.add<string>("accept", 0, "set request's header Accept Type", false, Accept_Default);
    CommandParse.add<int>("http_version", 0, "set request version type", false, static_cast<int>(http::utils::HTTP_1_1),
                          cmdline::oneof<int>(static_cast<int>(http::utils::HTTP_1_0), static_cast<int>(http::utils::HTTP_1_1)));
    CommandParse.add<string>("cookie", 0, "set request header", false, "");
    CommandParse.add<string>("body", 0, "the post parameter", false, "");
    CommandParse.add<string>("output", 'o', "ouput file path", false, "");
    CommandParse.add<string>("content-type", 0, "the post data content type", false, "application/x-www-form-urlencoded");
    CommandParse.add<int>("logLevel", 'l', "the logger level.(0.debug, 1.info 2.warning 3.alert", false, 1, cmdline::range<int>(0, 3));
    CommandParse.add<int>("timeout", 0, "set connection timeout(s)", false, 10, cmdline::range<int>(0, 30));
    CommandParse.add<string>("auth-basic", 0, "set auth basic user", false, "");
    CommandParse.add<int>("threads", 0, "download threads count", false, 5, cmdline::range<int>(1, 10));
    CommandParse.set_program_name("httpclient");

    bool ok = CommandParse.parse(argc, argv);

    if (!ok || argc < 2) {
        std::cout << CommandParse.error() << std::endl;
        std::cout << CommandParse.usage() << std::endl;
    } else {
        if (CommandParse.exist("help")) {
            std::cout << CommandParse.usage() << std::endl;
            return 0;
        }
        if (CommandParse.exist("version")) {
            std::cout << "v1.0.0" << std::endl;
            return 0;
        }
        std::string      reqUrl = CommandParse.get<std::string>("url");
        http::HttpClient client;

        int nLevel = CommandParse.get<int>("logLevel");
        logger.setLevel(static_cast<tlog::detail::Level>(nLevel));
        bool bRedireect = CommandParse.get<bool>("enable_redirect");
        bool bVerbose   = CommandParse.get<bool>("verbose");
        // Make header
        client.setAccept(CommandParse.get<string>("accept"));
        client.setConnectTimeout(CommandParse.get<int>("timeout"));
        client.setUserAgent(CommandParse.get<string>("userAgent"));
        client.setAcceptEncoding(CommandParse.get<string>("acceptEncoding"));
        client.setAcceptLanguage(CommandParse.get<string>("acceptLanguage"));
        client.setHeader("Connection", "keep-alive");
        int httpVersion = CommandParse.get<int>("http_version");
        client.setHttpVersion(static_cast<http::utils::HttpVersion>(httpVersion));

        std::string strCookie = CommandParse.get<string>("cookie");
        if (!strCookie.empty())
            client.setCookie(strCookie);

        std::string strAuthBasic = CommandParse.get<string>("auth-basic");
        if (!strAuthBasic.empty() && strAuthBasic.find(":") != std::string::npos) {
            std::string strUser = strAuthBasic.substr(0, strAuthBasic.find(":"));
            std::string strPass = strAuthBasic.substr(strAuthBasic.find(":") + 1);
            client.setBasicAuthUserPass(strUser, strPass);
        }

        std::string      reqType = CommandParse.get<std::string>("type");
        http::HttpResult Result;
        std::string      downloadPath = CommandParse.get<string>("output");
        if (!downloadPath.empty()) {
            client.setOutputFile(downloadPath);
        }
        if (strcasecmp(reqType.c_str(), "get") == 0) {
            Result = client.Get(reqUrl, bRedireect, bVerbose);
        } else if (strcasecmp(reqType.c_str(), "post") == 0) {
            std::string body        = CommandParse.get<string>("body");
            std::string contentType = CommandParse.get<string>("content-type");
            if (body.empty()) {
                std::cout << "post data is empty. ignored";
            } else {
                client.setHeader(ContentType, contentType);
                client.setHeader(ContentLength, body.size());
                Result = client.Post(reqUrl, body, bRedireect, bVerbose);
            }
        } else if (strcasecmp(reqType.c_str(), "head") == 0) {
            Result = client.Head(reqUrl, bVerbose);
        } else if (strcasecmp(reqType.c_str(), "Download") == 0) {
            int nThreads = CommandParse.get<int>("threads");
            client.DownloadFile(reqUrl, downloadPath, nThreads, bVerbose);
        }
        std::cout << "\nStatus Code:" << Result.status_code() << std::endl << "Text Size:" << Result.text().size() << std::endl << "Relay Message:" << Result.error() << std::endl;
        if (downloadPath.empty()) {
            std::cout << "Text :" << Result.text() << std::endl;
        }
    }
    return 0;
}
