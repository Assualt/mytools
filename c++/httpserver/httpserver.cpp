#include "httpserver.h"
#include "httputils.h"
#include <fcntl.h>
#include <sys/resource.h>
#include <thread>
#define BUF_SIZE 32768
namespace http {
RequestMapper::Key::Key(const std::string &pattern, const std::string &method, bool needval)
    : pattern(pattern)
    , method(method)
    , needval(needval) {
    auto itemVector = utils::split(pattern, '/');
    if (needval) {
        int i = 0;
        for (auto &item : itemVector) {
            if (item.front() == '{' && item.back() == '}') {
                keyPoint.push_back(i);
                keySet.push_back(item.substr(1, item.size() - 2));
            }
            i++;
        }
    }
}
bool RequestMapper::Key::MatchFilter(const std::string &reqPath, const std::string &reqType, std::map<std::string, std::string> &valMap, bool &MethodAllowed) {
    if (strcasecmp(reqType.c_str(), method.c_str()) == 0)
        MethodAllowed = true;
    else
        MethodAllowed = false;
    if (!needval)
        return reqPath == pattern;
    else {
        std::string path = reqPath;
        if (reqPath.find("?") != std::string::npos)
            path = path.substr(0, path.find("?"));
        auto itemList    = utils::split(path, '/');
        auto patternList = utils::split(pattern, '/');
        // reqpath: /index/
        // pattern: /index/{user}/{pass}  false
        if (itemList.size() != patternList.size())
            return false;
        // reqpath: /index/x/y
        // pattern: /indep/{user}/{pass}  false
        for (int i = 0; i < keyPoint.front(); i++) {
            if (itemList[ i ] != patternList[ i ])
                return false;
        }
        for (auto i = 0, j = 0; i < keyPoint.size(); i++) {
            int pos = keyPoint[ i ];
            if (pos >= itemList.size())
                break;
            valMap.insert(std::pair<std::string, std::string>(keySet[ j++ ], itemList[ pos ]));
        }
        return true;
    }
}

bool RequestMapper::Key::MatchFilter(const std::string &reqPath, const std::string &reqType, bool &MethodAllowed) {
    if (strcasecmp(reqType.c_str(), method.c_str()) == 0)
        MethodAllowed = true;
    return reqPath == pattern;
}

void RequestMapper::addRequestMapping(const Key &key, http::Func &&F) {
    m_vRequestMapper.push_back(std::pair<RequestMapper::Key, http::Func>(key, F));
}

http::Func RequestMapper::find(const std::string &RequestPath, const std::string &reqType, std::map<std::string, std::string> &resultMap) {
    for (auto iter : m_vRequestMapper) {
        bool MethodAllowed;
        if (iter.first.MatchFilter(RequestPath, reqType, resultMap, MethodAllowed)) {
            logger.debug("request path:%s, handle path:%s, method:%s allow:%d", RequestPath, iter.first.pattern, reqType, MethodAllowed);
            if (!MethodAllowed)
                return find("/405", "GET");
            return iter.second;
        }
    }
    return find(NOTFOUND, reqType, resultMap);
}
http::Func RequestMapper::find(const std::string &RequestPath, const std::string &reqType) {
    for (auto iter : m_vRequestMapper) {
        bool MethodAllowed;
        if (iter.first.MatchFilter(RequestPath, reqType, MethodAllowed)) {
            logger.debug("request path:%s, handle path:%s, method:%s allow:%d", RequestPath, iter.first.pattern, reqType, MethodAllowed);
            if (!MethodAllowed)
                return find("/405", "GET");
            return iter.second;
        }
    }
    return [ = ](HttpRequest &request, HttpResponse &response, HttpConfig &config) {
        response.setStatusMessage(404, "HTTP/1.1", "404 not found");
        response.setBodyString(NOTFOUNDHTML);
        response.setHeader(ContentType, "text/html");
        return true;
    };
}

bool ClientThread::parseHeader(ConnectionInfo *info, HttpRequest &request, HttpConfig *config) {
    char           temp[ BUF_SIZE ];
    bool           bFindBody = false;
    std::string    strLine, strKey, strVal;
    int            LineCnt = 0;
    int            BodySizeInRequest, recvSize = 0;
    Encoding       EncodingType;
    int            HeaderSize = 0;
    std::string    HttpVersion, RequestType, RequestPath;
    MyStringBuffer mybuf;

#ifdef USE_OPENSSL
    if (info->ssl && config->supportSSL()) {
        fcntl(info->m_nClientFd, F_SETFD, info->m_nFDFlag);
        logger.info("set client fd be block mode");
    }
#endif
    while (!bFindBody) {
        memset(temp, 0, BUF_SIZE);
        int nRead;

        nRead = recvData(info, temp, BUF_SIZE, 0);
        if (nRead < 0) {
            if (errno == EAGAIN)
                continue;
            else {
                logger.info("recv data from fd:%d error, read bytes:%d", info->m_nClientFd, nRead);
                return false;
            }
        } else if (nRead == 0) {
            break;
        }
        // client fd is non block
        for (size_t i = 0; i < nRead;) {
            if (i + 1 < nRead && temp[ i ] == '\r' && temp[ i + 1 ] == '\n' && !bFindBody) {
                if (LineCnt == 0) {
                    ParseFirstLine(strLine, HttpVersion, RequestPath, RequestType);
                    request.setRequestType(RequestType);
                    request.setRequestPath(RequestPath);
                    request.setHttpVersion(HttpVersion);
                    if (!config->checkHttpVersion(HttpVersion)) {
                        logger.warning("unsupported httpversion %s...", HttpVersion);
                        return false;
                    }
                    if (!config->checkMethod(RequestType)) {
                        logger.warning("unsupported requestMethod %s...", RequestType);
                        return false;
                    }
                    // logger.info("httpversion:%s requesttype:%s requestpath:%s", HttpVersion, RequestType, RequestPath);
                    LineCnt++;
                } else if (!strLine.empty()) {
                    ParseHeaderLine(strLine, strKey, strVal);
                    strKey = utils::trim(strKey);
                    strVal = utils::trim(strVal);
                    // logger.debug("Find Response Body Header: %s->%s", strKey, strVal);
                    if (strcasecmp(strKey.c_str(), ContentLength) == 0) {
                        BodySizeInRequest = atoi(strVal.c_str());
                        EncodingType      = EncodingLength;
                    } else if (strcasecmp(strKey.c_str(), TransferEncoding) == 0 && strcasecmp(strVal.c_str(), "chunked") == 0) {
                        EncodingType = EncodingChunk;
                    }
                    request.setHeader(strKey, strVal);
                } else if (strLine.empty()) {
                    bFindBody = true;
                }
                i += 2;
                HeaderSize += strLine.size() + 2;
                strLine.clear();
            } else if (!bFindBody) { // Header
                strLine.push_back(temp[ i ]);
                i++;
            } else if (strcasecmp(RequestType.c_str(), "get") == 0) {
                break;
            } else if (strcasecmp(RequestType.c_str(), "post") == 0 && BodySizeInRequest != 0) {
                mybuf.sputc(temp[ i ]);
                recvSize++;
                i++;
            }
        }
    }
    if (strcasecmp(RequestType.c_str(), "post") == 0) {
        if (recvSize != 0)
            request.setPostParams(mybuf.toString());
        logger.debug("recv body size:%d", recvSize);
    }
    return true;
}

void ClientThread::ParseHeaderLine(const std::string &line, std::string &key, std::string &val) {
    int nBlankCnt = 0;
    key.clear();
    val.clear();
    for (size_t i = 0; i < line.size(); i++) {
        if (line[ i ] == ':')
            if (nBlankCnt == 0)
                nBlankCnt = 1;
            else
                val.push_back(line[ i ]);
        else if (nBlankCnt == 0) {
            key.push_back(line[ i ]);
        } else if (nBlankCnt == 1) {
            val.push_back(line[ i ]);
        }
    }
}

void ClientThread::ParseFirstLine(const std::string &line, std::string &HttpVersion, std::string &RequestPath, std::string &RequestType) {
    int nBlankCnt = 0;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[ i ] == ' ') {
            if (nBlankCnt != 2)
                nBlankCnt++;
            else
                HttpVersion.push_back(line[ i ]);
        } else if (nBlankCnt == 0) {
            RequestType.push_back(line[ i ]);
        } else if (nBlankCnt == 1) {
            RequestPath.push_back(line[ i ]);
        } else if (nBlankCnt == 2) {
            HttpVersion.push_back(line[ i ]);
        }
    }
}
void ClientThread::handleRequest(RequestMapper *handlerMapping, HttpConfig *config, ConnectionInfo *info) {
    HttpRequest  request;
    HttpResponse response;
    if (!parseHeader(info, request, config)) {
        logger.info("disconnect from client:%d ...", info->m_nClientFd);
        http::Func iter = handlerMapping->find("/400", request.getRequestType());
        iter(request, response, *config);
        int nWrite = response.WriteBytes(info);
        info->disconnect();
        logger.info("%d %s:%d -- [%s] \"%s %s %s\" %d %d \"-\" \"%s\" \"-\" %s", info->m_nClientFd, info->m_strConnectIP, info->m_nPort, utils::requstTimeFmt(), request.getRequestType(),
                    request.getRequestPath(), request.getHttpVersion(), response.getStatusCode(), nWrite, request.get(UserAgent), request.get("Connection"));
        return;
    }

    // check the access right of html
    if (config->needAuth()) {
        if (!config->checkAuth(request.get(Authorization))) {
            http::Func iter = handlerMapping->find("/401", request.getRequestType());
            iter(request, response, *config);
            int nWrite = response.WriteBytes(info);
            logger.info("%d %s:%d -- [%s] \"%s %s %s\" %d %d \"-\" \"%s\" \"-\" %s", info->m_nClientFd, info->m_strConnectIP, info->m_nPort, utils::requstTimeFmt(), request.getRequestType(),
                        request.getRequestPath(), request.getHttpVersion(), response.getStatusCode(), nWrite, request.get(UserAgent), request.get("Connection"));
            info->disconnect();
            return;
        }
    }

    std::map<std::string, std::string> recvHeaderMap;
    int                                nWrite;
    std::string                        basicRequest = info->m_strServerRoot;
    if (basicRequest.back() != '/')
        basicRequest += "/";
    if (request.getRequestFilePath().front() == '/')
        basicRequest += request.getRequestFilePath().substr(1);
    else
        basicRequest += request.getRequestFilePath();

    bool FileExists = utils::FileExists(basicRequest);
    if (FileExists) {
        // resource
        logger.debug("requst path:%s is exists", basicRequest);
        if (!utils::ISDir(basicRequest)) {
            http::Func iter = handlerMapping->find("/#/", request.getRequestType());
            request.setRequestFilePath(basicRequest);
            iter(request, response, *config);
            nWrite = response.WriteBytes(info);
        } else {
            if (basicRequest.back() != '/')
                basicRequest.append("/");
            bool redirectFile = false;
            for (auto &suffix : config->getSuffixSet()) {
                if ((redirectFile = utils::FileExists(basicRequest + suffix)) == true) {
                    request.setRequestFilePath(basicRequest + suffix);
                    http::Func iter = handlerMapping->find("/#/", request.getRequestType());
                    iter(request, response, *config);
                    nWrite = response.WriteBytes(info);
                    break;
                }
            }
            if (!redirectFile) {
                http::Func iter = handlerMapping->find("/#//", request.getRequestType());
                iter(request, response, *config);
                nWrite = response.WriteBytes(info);
            }
            // printf("%s\n", response.toResponseHeader());
        }
    } else {
        logger.info("request path:%s is not exists", basicRequest);
        http::Func iter = handlerMapping->find(request.getRequestPath(), request.getRequestType(), recvHeaderMap);
        request.setParams(recvHeaderMap);
        iter(request, response, *config);
        nWrite = response.WriteBytes(info);
    }
    info->disconnect();
    logger.info("%d %s:%d -- [%s] \"%s %s %s\" %d %d \"-\" \"%s\" \"-\" %s", info->m_nClientFd, info->m_strConnectIP, info->m_nPort, utils::requstTimeFmt(), request.getRequestType(),
                request.getRequestPath(), request.getHttpVersion(), response.getStatusCode(), nWrite, request.get(UserAgent), request.get("Connection"));
}

ssize_t ClientThread::writeResponse(int client_fd, HttpResponse &response) {
    std::string responseHeader = response.toResponseHeader();
    logger.info("write data size:%d", responseHeader.size());
    return writeData(client_fd, (void *)responseHeader.c_str(), responseHeader.size(), 0);
}

int ClientThread::recvData(ConnectionInfo *info, void *buf, size_t n, int ops) {
#ifdef USE_OPENSSL
    if (info->ssl)
        return SSL_read(info->ssl, buf, n);
    return ::recv(info->m_nClientFd, buf, n, ops);
#endif
    return ::recv(info->m_nClientFd, buf, n, ops);
}
int ClientThread::writeData(int fd, void *buf, size_t n, int ops) {
    return ::send(fd, buf, n, ops);
}

HttpServer::HttpServer()
    : m_nPort(0)
    , m_nMaxListenClients(20)
    , m_nServerFd(-1)
    , m_nEpollTimeOut(10) {

#ifdef USE_OPENSSL
    ctx  = nullptr;
    meth = nullptr;
#endif
}

bool HttpServer::ExecForever() {
    int ret = 0;
    // 设置 每个进程允许打开的最大文件数
    struct rlimit rt;
    rt.rlim_max = rt.rlim_cur = m_nMaxListenClients;
    if (setrlimit(RLIMIT_NOFILE, &rt) == -1) {
        logger.warning("setrlimit failed. %s", strerror(errno));
        return false;
    }
    logger.info("set rlimit %d ok.", m_nMaxListenClients);

    // 1.创建server socket 用作监听使用，使用TCP可靠的连接和 IPV4的形式进行事件进行监听，协议使用IPV4
    m_nServerFd = socket(PF_INET, SOCK_STREAM, 0);
    if (m_nServerFd == -1) {
        logger.warning("create socket error with erros:%s", strerror(errno));
        return false;
    }
    // 设置socket属性端口可以重用
    // int opt = SO_REUSEADDR;
    // if (setsockopt(m_nServerFd, SOL_SOCKET, SO_ACCEPTCONN, &opt, sizeof(opt)) == -1) {
    //     logger.warning("set socket opt to reuse failed. %s", strerror(errno));
    //     return false;
    // }
    // 设置端口为非阻塞模式
    int oldFlag = setFDnonBlock(m_nServerFd);
    // 2.server端进行bind 并初始化server的地址结构体, 注意字节序的转化
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family      = PF_INET;
    server_addr.sin_addr.s_addr = inet_addr(m_strServerIP.c_str());
    server_addr.sin_port        = htons(m_nPort);
    if ((ret = bind(m_nServerFd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr))) == -1) {
        logger.warning("bind error with erros:%s", strerror(errno));
        return false;
    }
    // 3.server端进行监听
    if ((ret = listen(m_nServerFd, m_nMaxListenClients)) == -1) {
        logger.warning("listen error with erros:%s", strerror(errno));
        return false;
    }

    logger.info("httpserver:%s listening at %s:%d ....", m_strServerName, m_strServerIP, m_nPort);
#ifdef USE_OPENSSL
    if (m_mConfig.supportSSL()) {
        switchToSSLServer();
        logger.info("init https safe server ok.");
    }
#endif
    // 1. epoll wait
    int epoll_fd = epoll_create(m_nMaxListenClients);
    // 2.定义并填充epoll_event,和监听连接接的server的所有epoll_events事件数组
    struct epoll_event event;
    event.events                           = EPOLLIN | EPOLLET;
    event.data.fd                          = m_nServerFd; //服务器绑定的fd
    struct epoll_event *event_client_array = new epoll_event[ m_nMaxListenClients ];
    // 3.注册事件,将当前的epoll_fd注册到内核中去用于事件的监听
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_nServerFd, &event) < 0) {
        logger.warning("epoll set insertion error. fd:%d", epoll_fd);
        return false;
    }
    int                                     current_fds = 1;
    std::unordered_map<int, ConnectionInfo> ConnectionInfoMap;
    // 4. 进入事件等待
    while (true) {
        //设置最大的监听的数量,以及EPOLL 的超时时间
        int ready_count = epoll_wait(epoll_fd, event_client_array, m_nMaxListenClients, m_nEpollTimeOut);
        // 5.从就绪的event里面进行accept,用于创建链接到客户端
        for (int i = 0; i < ready_count; i++) {
            struct sockaddr_in client_addr;
            socklen_t          addr_len = sizeof(client_addr);
            int                client_fd;

            if (event_client_array[ i ].data.fd == m_nServerFd) { //确定为连接服务器的请求
                int client_fd = accept(m_nServerFd, (struct sockaddr *)&client_addr, &addr_len);
                // 6.把接收到client的fd 添加到epoll的监听事件中去
                if (client_fd < 0) {
                    logger.info("accept error. ");
                    continue;
                }
                int            client_oldFlag = setFDnonBlock(client_fd);
                ConnectionInfo info;
                event.events  = EPOLLIN | EPOLLET;
                event.data.fd = client_fd;
                if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event)) == -1) {
                    logger.info("failed to epoll_add client fd:%d , errmsg:%s", client_fd, strerror(errno));
                    continue;
                }
#ifdef USE_OPENSSL
                if (m_mConfig.supportSSL()) {
                    info.ssl = SSL_new(ctx);
                    if (nullptr == info.ssl) {
                        logger.info("ssl connect failed. closing transmission channel.");
                        close(client_fd);
                        continue;
                    }
                    if (0 == SSL_set_fd(info.ssl, client_fd)) {
                        logger.warning("attach client fd:%d failed", client_fd);
                        close(client_fd);
                        continue;
                    }
                    bool isContinue = true;
                    bool sslAccept  = false;
                    while (isContinue) {
                        if ((ret = SSL_accept(info.ssl)) != 1) {
                            int icode = -1;
                            int iret  = SSL_get_error(info.ssl, icode);
                            if ((iret == SSL_ERROR_WANT_WRITE) || (iret == SSL_ERROR_WANT_READ)) {
                                isContinue = true;
                            } else {
                                ERR_print_errors_fp(stderr);
                                logger.info("ssl accept error with error %d %d %d", ret, iret, icode);
                                break;
                            }
                        } else {
                            sslAccept = true;
                            break;
                        }
                    }
                    if (sslAccept) {
                        logger.info("ssl accept success. client cipher is:%s", SSL_get_cipher(info.ssl));

                        X509 *client_cert = SSL_get_peer_certificate(info.ssl);
                        if (client_cert != nullptr) {
                            logger.warning("certificate error.");
                            char *str = X509_NAME_oneline(X509_get_subject_name(client_cert), 0, 0);
                            if (str != nullptr) {
                                logger.warning("get subject from client fd:%d. %s.", client_fd, str);
                                OPENSSL_free(str);
                            }
                            char *str1 = X509_NAME_oneline(X509_get_issuer_name(client_cert), 0, 0);
                            if (str1 != nullptr) {
                                logger.warning("get issuser from client fd:%d. %s", client_fd, str1);
                                OPENSSL_free(str1);
                            }
                            X509_free(client_cert);
                        } else {
                            logger.info("can't get client certificate yet. ignore check here");
                        }
                        char buf[ 1024 ] = {0};
                        int  nRead       = SSL_read(info.ssl, buf, 1024);
                        close(client_fd);
                        continue;
                    } else {
                        logger.info("ssl accept failed. closing transmission channel.");
                        close(client_fd);
                        continue;
                    }
                }
#endif

                logger.debug("recv client:%d from address %s:%d success.", client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                current_fds++;
                //{inet_ntoa(client_addr.sin_addr), m_strServerRoot, ntohs(client_addr.sin_port), current_fd, client_oldFlag};
                info.m_strConnectIP  = inet_ntoa(client_addr.sin_addr);
                info.m_strServerRoot = m_mConfig.getServerRoot();
                info.m_nPort         = ntohs(client_addr.sin_port);
                info.m_nClientFd     = client_fd;
                info.m_nFDFlag       = client_oldFlag;
                ConnectionInfoMap.insert(std::pair<int, ConnectionInfo>(client_fd, info));
            } else if (event_client_array[ i ].events & EPOLLIN) {
                // 7.进行数据的交流
                // 可创建一个线程与此fd进行数据的处理操作
                // 线程处理完毕之后就直接可以直接从监听的的fd中移除掉
                // 为当前线程为非阻塞模式
                int current_fd = event_client_array[ i ].data.fd;
                logger.debug("begin to assign one thread to handle the request from fd:%d, left free thread count is %d", current_fd, ThreadsPool.idlCount());
                if (ThreadsPool.idlCount() < 0) {
                    logger.info("can't assign one thread to handle such request from [%s:%d]", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    // fcntl(client_fd, F_SETFL, oldFlag);
                    // epoll_ctl(epoll_fd, EPOLL_CTL_DEL,)
                    continue;
                } else {
                    ThreadsPool.commit(ClientThread::handleRequest, &m_mapper, &m_mConfig, &ConnectionInfoMap.at(current_fd));
                    // std::thread tempThread(ClientThread::handleRequest, &m_mapper, &m_mConfig, &info);
                    // tempThread.join();
                }
            } else if (event_client_array[ i ].events & EPOLLOUT) {
                int current_fd = event_client_array[ i ].data.fd;
                // ConnectionInfoMap.erase(current_fd);
                // logger.info("closing transmission channel. %d", current_fd);
            } else {
                int fd = event_client_array[ i ].data.fd;
                close(fd);
                logger.info("closing transmission channel %d ...", fd);
            }
        }
    }
    // 8.关闭和释放支援
    close(epoll_fd);
    close(m_nServerFd);
    delete[] event_client_array;
    return false;
}

#ifdef USE_OPENSSL
bool HttpServer::switchToSSLServer() {
    SSLConfig config = m_mConfig.getSSLConfig();
    // ssl init
#if OPENSSL_VERSION_NUMBER < 0x10100003L
    OPENSSL_config(nullptr);
    // Register the error strings for libcrypto & libssl
    SSL_load_error_strings();

    // Register the available ciphers and digests
    SSL_library_init();

    OpenSSL_add_all_algorithms();
#else
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr);
    ERR_clear_error();
#endif
    if (config.protocol == Type_TLS1_1)
        meth = (SSL_METHOD *)TLSv1_1_server_method();
    else if (config.protocol == Type_TLS1_2)
        meth = (SSL_METHOD *)TLSv1_2_server_method();
    else
        meth = (SSL_METHOD *)SSLv23_method();
    ctx = SSL_CTX_new(meth);
    if (nullptr == ctx) {
        logger.warning("create ssl ctx for server failed.");
        return false;
    }
    // 要求校验对方的证书
    // SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    // 夹在 CA证书
    // SSL_CTX_load_verify_locations(ctx, config.ca.c_str(), nullptr);

    // load certificate
    if (SSL_CTX_use_certificate_file(ctx, config.certificate.c_str(), SSL_FILETYPE_PEM) == 0) {
        ERR_print_errors_fp(stderr);
        return false;
    }
    // load private file
    if (SSL_CTX_use_PrivateKey_file(ctx, config.certificate_private_key.c_str(), SSL_FILETYPE_PEM) == 0) {
        ERR_print_errors_fp(stderr);
        return false;
    }
    // check private key
    if (!SSL_CTX_check_private_key(ctx)) {
        logger.error("Private key does not match the certificate public key");
        return false;
    }
    // SSL_CTX_set_cipher_list(ctx, config.ciphers.c_str());
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    return true;
}

#endif

bool HttpServer::loadHttpConfig(const std::string &strHttpServerConfig) {
    logger.info("begin to load config %s ...", strHttpServerConfig);
    bool ret = m_mConfig.loadConfig(strHttpServerConfig);
    logger.info("end to load config %s ...", strHttpServerConfig);

    m_strServerIP       = m_mConfig.getServerHost();
    m_nPort             = m_mConfig.getServerPort();
    m_nMaxListenClients = m_mConfig.getMaxAcceptClient();
    return ret;
}
int HttpServer::setFDnonBlock(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
    return flag;
}

} // namespace http