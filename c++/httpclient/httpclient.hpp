#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netdb.h> /* gethostbyname */
#include <netinet/in.h>
#include <sstream>
#include <streambuf>
#include <sys/socket.h>
#include <sys/types.h> /* See NOTES */
#include <unistd.h>
#include <vector>

#include <thread>
#ifdef USE_GURL
#include "gurl/url/gurl.h"
#else
#include "url.h"
#endif
#include "hashutils.hpp"
#include "logging.h"
using namespace std;

#ifdef USE_OPENSSL
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

#define CTRL "\r\n"
#define ContentType "Content-Type"
#define ContentEncoding "Content-Encoding"
#define Accept "Accept"
#define AcceptEncoding "Accept-Encoding"
#define Authorization "Authorization"
#define AcceptLanguage "Accept-Language"
#define UserAgent "User-Agent"
#define ContentLength "Content-Length"
#define TransferEncoding "Transfer-Encoding"
#define AcceptRanges "Accept-Ranges"
#define Location "Location"
#define Cookie "Cookie"
#define Referer "Referer"

#define HTTP1_1 "HTTP/1.1"
#define HTTP1_0 "HTTP/1.0"
#include <exception>
#define TempFile ".httpclient.download"

// 如果 接收数据大小大于 50M 如果没有指定 output,则提示用户不会展示
#define MAX_RECV_BYTES 50 * 1024 * 1024

#include "threadpool.h"

using cstring = const std::string &;
namespace http {
typedef enum { EncodingLength, EncodingChunk, EncodingGzip, EncodingOther } Encoding;

namespace utils {

template <class T> std::string toString(const T &val) {
    std::stringstream ss;
    ss << val;
    return ss.str();
}

enum HttpVersion { HTTP_1_0, HTTP_1_1 };

static std::string _ltrim(cstring src, char ch = ' ') {
    std::string           temp = src;
    std::string::iterator p    = std::find_if(temp.begin(), temp.end(), [ &ch ](char c) { return ch != c; });
    temp.erase(temp.begin(), p);
    return temp;
}

static std::string _rtrim(cstring src, char ch = ' ') {
    string                   temp = src;
    string::reverse_iterator p    = find_if(temp.rbegin(), temp.rend(), [ &ch ](char c) { return ch != c; });
    temp.erase(p.base(), temp.end());
    return temp;
}

static std::string trim(cstring src, char ch = ' ') {
    return _rtrim(_ltrim(src, ch), ch);
}

static size_t chunkSize(cstring strChunkSize) {
    std::string temp;
    for (size_t i = 0; i < strChunkSize.size(); i++) {
        if ((strChunkSize[ i ] >= '0' && strChunkSize[ i ] <= '9') || (strChunkSize[ i ] >= 'A' && strChunkSize[ i ] <= 'F') || (strChunkSize[ i ] >= 'a' && strChunkSize[ i ] <= 'f')) {
            temp.push_back(strChunkSize[ i ]);
        } else {
            break;
        }
    }
    if (temp.empty())
        return -1;
    return std::stoi(temp, nullptr, 16);
}

} // namespace utils
struct HttpUrl {
public:
    std::string scheme;
    std::string username;
    std::string password;
    std::string host;
    int         port;
    std::string path;
    std::string query;
    std::string fragment;
    std::string fullurl;
    std::string netloc;

public:
    HttpUrl(cstring url)
        : fullurl(url)
        , scheme("http")
        , port(80) {
        parse();
    }
    HttpUrl() = default;

    void resetUrl(cstring url) {
        fullurl = url;
        parse();
    }

    std::string getHostUrl() const {
        std::stringstream ss;
        ss << scheme << "://";
        if (!username.empty())
            ss << username;
        if (!username.empty() && !password.empty())
            ss << ":" << password << "@";
        ss << host << path;
        return ss.str();
    }

private:
    void parse() {
        if (fullurl.find("://") == std::string::npos)
            fullurl = "http://" + fullurl;
#ifdef USE_GURL
        GURL url(fullurl);
        if (!url.is_valid() || !url.IsStandard())
            return;
        if (url.has_scheme())
            scheme = url.scheme();
        if (url.has_username())
            username = url.username();
        if (url.has_password())
            password = url.password();
        if (url.has_host())
            host = url.host();
        if (url.has_port())
            port = atoi(url.port().c_str());
        if (url.has_path())
            path = url.path();
        if (url.has_query())
            query = url.query();
        if (url.has_ref())
            fragment = url.ref();
#else
        Url url = Url::create(fullurl);
        host    = url.getHost();
        if (host.empty())
            return;
        scheme   = url.getScheme();
        username = url.getUsername();
        password = url.getPassword();
        port     = url.getPort();
        path     = url.getPath();
        query    = url.getQuery();
        if (!query.empty() && query.front() == '?')
            query = query.substr(1);
        fragment = url.getFragment();
#endif
        netloc = host + ":" + to_string(port);
    }
};
typedef std::vector<std::pair<std::string, std::string>> ResourceMap;
class HttpRequest {
public:
    HttpRequest() {
        m_strRequestParams = "";
        m_strRequestHost   = "";
        m_strRangeBytes    = "";
    }
    HttpRequest(const ResourceMap &headerMap)
        : m_vReqestHeader(headerMap) {
    }

    template <class T> void setHeader(cstring key, const T &val) {
        if (key == "Host")
            m_strRequestHost = utils::toString(val);
        else if (key == "Range")
            m_strRangeBytes = utils::toString(val);
        else
            m_vReqestHeader.push_back(std::pair<std::string, std::string>(key, utils::toString(val)));
    }
    void setRequestType(cstring reqType) {
        m_strRequestType = reqType;
    }

    void setRequestPath(cstring reqPath) {
        m_strRequestPath = reqPath;
    }

    std::string getRequestPath() const {
        return m_strRequestPath;
    }

    void setHttpVersion(cstring httpversion) {
        m_strRequestHttpVersion = httpversion;
    }

    void setParams(cstring params) {
        m_strRequestParams = params;
    }

    std::string toStringHeader() {
        stringstream ss;
        ss << m_strRequestType << " " << m_strRequestPath << " " << m_strRequestHttpVersion << CTRL;
        for (auto &item : m_vReqestHeader)
            ss << item.first << ": " << item.second << CTRL;
        if (!m_strRangeBytes.empty())
            ss << "Range: " << m_strRangeBytes << CTRL;
        ss << "Host: " << m_strRequestHost << CTRL;

        if (!m_strRequestParams.empty())
            ss << CTRL << m_strRequestParams;
        ss << CTRL;
        return ss.str();
    }

    friend ostream &operator<<(ostream &os, HttpRequest &obj) {
        os << "> " << obj.m_strRequestType << " " << obj.m_strRequestPath << " " << obj.m_strRequestHttpVersion << CTRL;
        for (auto &item : obj.m_vReqestHeader)
            os << "> " << item.first << ": " << item.second << CTRL;
        if (!obj.m_strRangeBytes.empty())
            os << "Range: " << obj.m_strRangeBytes << CTRL;
        os << "> Host: " << obj.m_strRequestHost << CTRL;
        if (!obj.m_strRequestParams.empty())
            os << CTRL << obj.m_strRequestParams;
        os << CTRL;
        return os;
    }
    ResourceMap getHeader() const {
        return m_vReqestHeader;
    }

private:
    ResourceMap m_vReqestHeader;
    std::string m_strRequestType, m_strRequestPath, m_strRequestHttpVersion;
    std::string m_strRequestParams;
    std::string m_strRequestHost;
    std::string m_strRangeBytes;
};

class HttpResponse {
public:
    HttpResponse()
        : m_nBodyBytes(0)
        , m_nBodyDecodeBytes(0)
        , m_bDecodeBodyStatus(false) {
    }
    void setBody(cstring key, cstring val) {
        if (val.empty())
            m_vResponseBody.back().second += key;
        else
            m_vResponseBody.push_back(std::pair<std::string, std::string>(key, val));
    }
    std::string getResponseText() const {
        return m_strResponseText;
    }
    void getResponseBytes(std::ostream &os) {
        char *WriteBuffer = new char[ MAX_BUF_SIZE ];
        m_ResponseBuffer.seekReadPos(0);
        while (1) {
            memset(WriteBuffer, 0, MAX_BUF_SIZE);
            std::streamsize nReadSize = m_ResponseBuffer.sgetn(WriteBuffer, MAX_BUF_SIZE);
            if (nReadSize == 0)
                break;
            os.write(WriteBuffer, nReadSize);
        }
        m_ResponseBuffer.seekReadPos(0);
        delete[] WriteBuffer;
    }
    size_t getResponseBytesSize() const {
        return m_nBodyBytes;
    }
    size_t getResponseDecodeBytesSize() const {
        return m_nBodyDecodeBytes;
    }
    std::string getResponseItem(cstring key) {
        for (auto &item : m_vResponseBody) {
            if (strcasecmp(item.first.c_str(), key.c_str()) == 0)
                return item.second;
        }
        return "";
    }
    std::string getCookie() {
        std::string strCookie;
        for (auto &item : m_vResponseBody) {
            std::string firstItem = item.first;
            std::transform(firstItem.begin(), firstItem.end(), firstItem.begin(), ::tolower);
            if (firstItem.find("cookie") != std::string::npos) {
                strCookie.append(item.second);
                strCookie.append("; ");
            }
        }
        return strCookie;
    }
    void WriteBodyBytes(const char *buf, size_t nBytes) {
        m_ResponseBuffer.sputn(buf, nBytes);
        m_nBodyBytes += nBytes;
        m_strResponseText.append(std::string(buf, nBytes));
        m_nBodyDecodeBytes += nBytes;
    }
    void WriteBodyByte(char ch) {
        m_ResponseBuffer.sputc(ch);
        m_nBodyBytes += 1;
        m_strResponseText.push_back(ch);
        m_nBodyDecodeBytes += 1;
    }
    void tryDecodeBody() {
        if (this->getResponseItem(ContentEncoding) == "gzip") {
            m_ResponseBuffer.seekReadPos(0);
            logger.info("Try Decode Bytes %d for gzip.", m_nBodyBytes);
            if (m_nBodyBytes == 0)
                return;
            std::stringstream strstring;
            int               nBytes = HashUtils::GzipDecompress(m_ResponseBuffer, strstring);
            if (nBytes) {
                logger.info("Try Decode Bytes %d, and Decoded Bytes %d", m_nBodyBytes, nBytes);
                m_strResponseText   = strstring.str();
                m_nBodyDecodeBytes  = m_strResponseText.size();
                m_bDecodeBodyStatus = true;
            } else {
                logger.info("Decode Gzip Error.. bytes:%d", nBytes);
            }
        } else if (this->getResponseItem(ContentEncoding) == "deflate") {
            m_ResponseBuffer.seekReadPos(0);
            logger.info("Try Decode Bytes %d for deflate.", m_nBodyBytes);
            std::stringstream strstring;
            size_t            nBytes = HashUtils::DeflateDecompress(m_ResponseBuffer, strstring);
            if (nBytes) {
                logger.info("Try Decode Bytes %d, and Decoded Bytes %d", m_nBodyBytes, nBytes);
                m_strResponseText   = strstring.str();
                m_nBodyDecodeBytes  = m_strResponseText.size();
                m_bDecodeBodyStatus = true;
            } else {
                logger.info("Decode deflate Error.. bytes:%d", nBytes);
            }
        }
    }
    bool BodyIsDecoded() const {
        return m_bDecodeBodyStatus;
    }
    const ResourceMap &GetResponse() const {
        return m_vResponseBody;
    }
    friend ostream &operator<<(ostream &os, HttpResponse &obj) {
        for (auto &item : obj.m_vResponseBody) {
            os << "< " << item.first << ": " << item.second << CTRL;
        }
        if (!obj.m_strResponseText.empty()) {
            os << "<\r\n";
            if (strcasecmp(obj.getResponseItem(ContentEncoding).c_str(), "gzip") == 0 || strcasecmp(obj.getResponseItem(ContentEncoding).c_str(), "deflate") == 0)
                os << "[Binary] " << obj.m_nBodyBytes << "Bytes" << CTRL;
            else if (obj.getResponseItem(ContentType).find("application/json") != std::string::npos)
                os << obj.m_strResponseText << CTRL;
            else
                os << "[Binary] " << obj.m_nBodyBytes << "Bytes" << CTRL;
        }
        return os;
    }
    void clear() {
        m_nBodyBytes = m_nBodyDecodeBytes = 0;
        m_bDecodeBodyStatus               = false;
        m_vResponseBody.clear();
        m_strResponseText.clear();
        m_ResponseBuffer.clear();
    }

private:
    ssize_t        m_nBodyBytes;
    ssize_t        m_nBodyDecodeBytes;
    bool           m_bDecodeBodyStatus;
    ResourceMap    m_vResponseBody;
    std::string    m_strResponseText;
    MyStringBuffer m_ResponseBuffer;
};

struct HttpResult {
private:
    int         m_nStatusCode;
    std::string m_strText;
    std::string m_strRelayError;

public:
    HttpResult()
        : m_nStatusCode(200) {
    }
    HttpResult(int code, cstring text, cstring relayError)
        : m_nStatusCode(code)
        , m_strText(text)
        , m_strRelayError(relayError) {
    }

    int status_code() const {
        return m_nStatusCode;
    }

    std::string text() const {
        return m_strText;
    }

    std::string error() const {
        return m_strRelayError;
    }
};

enum HTTP_TYPE { TYPE_GET, TYPE_POST, TYPE_DELETE, TYPE_PUT };

enum CHUNK_STATE { CHUNK_BEGIN, CHUNK_PROCESS_1, CHUNK_PROCESS_2, CHUNK_END };

#define MAX_SIZE 32768
class SocketClient {
public:
    SocketClient()
        : m_nConnectFd(-1)
        , m_nChunkSize(0)
        , m_nConnectTimeout(10)
        , m_bUseSSL(false)
        , m_bConnected(false) {
#ifdef USE_OPENSSL
        initSSL();
        m_pConnection = nullptr;
#endif
    }
    ~SocketClient() {
        disconnect();
        m_bConnected = false;
    }

private:
#ifdef USE_OPENSSL
    void initSSL() {
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
    }
    bool SSLConnect() {
        m_pConnection               = new SSL_Connection();
        m_pConnection->m_ptrContext = SSL_CTX_new(TLS_client_method());
        if (m_pConnection->m_ptrContext == nullptr) {
            SSLDisConnect();
            ERR_print_errors_fp(stderr);
            return false;
        }
        // Create an SSL struct for the connection
        m_pConnection->m_ptrHandle = SSL_new(m_pConnection->m_ptrContext);
        if (m_pConnection->m_ptrHandle == nullptr) {
            SSLDisConnect();
            ERR_print_errors_fp(stderr);
            return false;
        }

        // Connect the SSL struct to our connection
        if (!SSL_set_fd(m_pConnection->m_ptrHandle, m_nConnectFd)) {
            SSLDisConnect();
            ERR_print_errors_fp(stderr);
            return false;
        }
        // Initiate SSL handshake
        if (SSL_connect(m_pConnection->m_ptrHandle) != 1) {
            ERR_print_errors_fp(stderr);
            SSLDisConnect();
            return false;
        }

        printSSLConnection(m_pConnection->m_ptrHandle);

        return true;
    }

    void printSSLConnection(SSL *ssl) {
        X509 *cert = SSL_get_peer_certificate(ssl);
        if (cert == nullptr) {
            logger.warning("get server's certifiacte error. ");
            return;
        }
        X509_NAME *name = X509_get_subject_name(cert);
        if (name == nullptr) {
            logger.warning("get server's x509 error. ");
            return;
        }
        char buf[ 8192 ] = {0};
        X509_NAME_oneline(name, buf, 8191);

        X509_free(cert);
        logger.info("server subject name:%s", buf);
    }

    void SSLDisConnect() {
        if (m_pConnection) {
            if (m_pConnection->m_ptrHandle) {
                SSL_shutdown(m_pConnection->m_ptrHandle);
                SSL_free(m_pConnection->m_ptrHandle);
            }
            if (m_pConnection->m_ptrContext)
                SSL_CTX_free(m_pConnection->m_ptrContext);

            delete m_pConnection;
            m_pConnection = nullptr;
        }
    }

    ssize_t SSLRecv(int fd, void *buf, ssize_t size) {
        return SSL_read(m_pConnection->m_ptrHandle, buf, size);
    }

    ssize_t SSLSend(const void *buf, ssize_t size) {
        if (m_pConnection != nullptr) {
            return SSL_write(m_pConnection->m_ptrHandle, buf, size);
        }
        return 0;
    }
#endif

    int RecvChunkData(const char *buffer, size_t &nPos, int size, HttpResponse &Response, CHUNK_STATE &ChunkState, size_t &ChunkSize, ssize_t &AllChunkBodySize, size_t &blockCount) {
        size_t      nWriteBytes = 0;
        std::string strChunkSize;
        for (; nPos < size;) {
            if (ChunkState == CHUNK_BEGIN) {
                if ((nPos + 1) < size && buffer[ nPos ] == '\r' && buffer[ nPos + 1 ] == '\n') {
                    ChunkSize    = utils::chunkSize(strChunkSize);
                    m_nChunkSize = ChunkSize;
                    ChunkState   = CHUNK_PROCESS_1;
                    // printf(" %02x %02x ChunkSize:%d %s\n", buffer[nPos], buffer[nPos + 1],
                    // ChunkSize, strChunkSize.c_str());
                    nPos += 2;
                    if (ChunkSize == 0) {
                        ChunkState = CHUNK_END;
                        logger.debug("read chunkdata size is  0. Exit now..");
                        return 0;
                    } else if (ChunkSize == -1) {
                        logger.error("Recv Bytes Error. Exit now...");
                        return 0;
                    }
                } else {
                    strChunkSize.push_back(buffer[ nPos ]);
                    // printf("+ \033[31m%02x\033[0m ", buffer[nPos]);
                    nPos += 1;
                }
            } else if (ChunkState == CHUNK_PROCESS_1) {
                if ((nPos + 1) < size && buffer[ nPos ] == '\r' && buffer[ nPos + 1 ] == '\n' && ChunkSize == 0) {
                    ChunkState = CHUNK_PROCESS_2;
                    nPos += 2;
                } else if (ChunkSize) {
                    // printf("%02x ", buffer[nPos] & 0xFF);
                    Response.WriteBodyByte(buffer[ nPos ] & 0xFF);
                    AllChunkBodySize += 1;
                    nPos += 1;
                    nWriteBytes++;
                    ChunkSize--;
                } else {
                }
            } else if (ChunkState == CHUNK_PROCESS_2) {
                ChunkState = CHUNK_BEGIN;
                // printf("%d \n", m_nChunkSize);
                logger.debug("Recv %dth block data, current block size:%d total recv blocks Bytes:%d", ++blockCount, m_nChunkSize, AllChunkBodySize);
                strChunkSize.clear();
            }
        }
        char   tempBuf[ MAX_BUF_SIZE ];
        size_t nRead = recvData(m_nConnectFd, tempBuf, MAX_BUF_SIZE, 0);
        nPos         = 0;
        RecvChunkData(tempBuf, nPos, nRead, Response, ChunkState, ChunkSize, AllChunkBodySize, blockCount);
        return nWriteBytes;
    }

    Encoding ParseHeader(const char *buffer, ssize_t size, HttpResponse &Response, ssize_t &HeaderSize, ssize_t &recvBodySize, ssize_t &LeftBodySize, ssize_t &BodySizeInResponse) {
        std::string    strKey, strVal, strLine;
        int            LineCnt      = 0;
        bool           bFindBody    = false;
        Encoding       EncodingType = EncodingGzip; // 0: Content-Length 1: Chunked 2: other way
        int            nCnt         = 0;
        MyStringBuffer chunkedBuffer;

        // Chunk mode
        CHUNK_STATE ChunkState = CHUNK_BEGIN;
        size_t      chunkSize  = 0;
        size_t      blockCount = 0;
        char        LeftBuffer[ MAX_SIZE ];
        bool        parseFirst = true;
        const char *sbuf       = nullptr;
        while (!bFindBody) {
            if (parseFirst) {
                sbuf       = buffer;
                parseFirst = false;
            } else {
                int nsize = recvData(m_nConnectFd, LeftBuffer, MAX_SIZE, 0);
                if (nsize < 0)
                    break;
                sbuf = LeftBuffer;
            }
            for (size_t i = 0; i < size;) {
                if (i + 1 < size && sbuf[ i ] == '\r' && sbuf[ i + 1 ] == '\n' && !bFindBody) {
                    if (LineCnt == 0) {
                        std::string HttpVersion, statusMessage;
                        int         StatusCode;
                        ParseFirstLine(strLine, HttpVersion, StatusCode, statusMessage);
                        if (to_string(StatusCode).size() != 3) {
                            continue;
                        }
                        Response.setBody("code", to_string(StatusCode));
                        Response.setBody("message", statusMessage);
                        logger.info("code:%s, message:%s, Line:%s", to_string(StatusCode), statusMessage, strLine);
                        LineCnt++;
                    } else if (!strLine.empty()) {
                        ParseHeaderLine(strLine, strKey, strVal);
                        strKey = utils::trim(strKey);
                        strVal = utils::trim(strVal);
                        logger.debug("Find Response Body Header: %s->%s", strKey, strVal);
                        if (strcasecmp(strKey.c_str(), ContentLength) == 0) {
                            BodySizeInResponse = atol(strVal.c_str());
                            EncodingType       = EncodingLength;
                        } else if (strcasecmp(strKey.c_str(), TransferEncoding) == 0 && strcasecmp(strVal.c_str(), "chunked") == 0) {
                            EncodingType = EncodingChunk;
                        }
                        Response.setBody(strKey, strVal);
                    } else if (strLine.empty()) {
                        bFindBody = true;
                        if (strcasecmp(m_strRequestType.c_str(), "head") == 0)
                            break;
                    }
                    i += 2;
                    HeaderSize += strLine.size() + 2;
                    strLine.clear();
                } else if (!bFindBody) { // Header
                    strLine.push_back(sbuf[ i ]);
                    i++;
                } else if (EncodingType != EncodingChunk && bFindBody) {
                    Response.WriteBodyByte(sbuf[ i ] & 0xFF);
                    recvBodySize += 1;
                    i += 1;
                } else if (EncodingType == EncodingChunk && bFindBody) {
                    // Server Apache mode_deflate is not suitable for zlib.
                    RecvChunkData(sbuf, i, size, Response, ChunkState, chunkSize, recvBodySize, blockCount);
                    break;
                } else {
                    logger.info("never reach here");
                }
            }
        }

        if (!strLine.empty() && !bFindBody) {
            ParseHeaderLine(strLine, strKey, strVal);
            strKey = utils::trim(strKey);
            strVal = utils::trim(strVal);
            logger.debug("Find Response Body Header: %s->%s", strKey, strVal);
            if (strKey == ContentLength) {
                BodySizeInResponse = atoi(strVal.c_str());
                EncodingType       = EncodingLength;
            } else if (strKey == TransferEncoding && strVal == "chunked") {
                EncodingType = EncodingChunk;
            }
            Response.setBody(strKey, strVal);
        }

        if (EncodingType == EncodingLength && BodySizeInResponse != 0 && BodySizeInResponse != -1) {
            LeftBodySize = BodySizeInResponse - recvBodySize;
            logger.info("Header Size:%d recvBodySize:%d totalSize:%d BodySizeInResponse:%d "
                        "LeftBodySize:%d EncodingType:%d",
                        HeaderSize, recvBodySize, size, BodySizeInResponse, LeftBodySize, EncodingType);
        } else if (EncodingType == EncodingChunk) {
            if (chunkSize == 0 && ChunkState != CHUNK_END) {
                size_t tempPos = size;
                RecvChunkData(buffer, tempPos, size, Response, ChunkState, chunkSize, recvBodySize, blockCount);
            }
        } else if (BodySizeInResponse == -1 && Response.getResponseItem(ContentEncoding) == "gzip") {
            EncodingType = EncodingGzip;
        }
        return EncodingType;
    }
    void ParseHeaderLine(cstring line, std::string &key, std::string &val) {
        int nBlankCnt = 0;
        key.clear();
        val.clear();
        for (size_t i = 0; i < line.size(); i++) {
            if (line[ i ] == ':')
                nBlankCnt = 1;
            else if (nBlankCnt == 0) {
                key.push_back(line[ i ]);
            } else if (nBlankCnt == 1) {
                val.push_back(line[ i ]);
            }
        }
    }

    void ParseFirstLine(cstring line, std::string &HttpVersion, int &StatusCode, std::string &StatusMessage) {
        int nBlankCnt = 0;
        StatusCode    = 0;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[ i ] == ' ') {
                if (nBlankCnt != 2)
                    nBlankCnt++;
                else
                    StatusMessage.push_back(line[ i ]);
            } else if (nBlankCnt == 0) {
                HttpVersion.push_back(line[ i ]);
            } else if (nBlankCnt == 1) {
                StatusCode = StatusCode * 10 + line[ i ] - '0';
            } else if (nBlankCnt == 2) {
                StatusMessage.push_back(line[ i ]);
            }
        }
    }

public:
    ssize_t read(HttpResponse &Response, cstring outfile) {
        auto    tBegin = std::chrono::system_clock::now();
        char    tempBuf[ MAX_SIZE ];
        ssize_t HeaderSize = 0, BodySize = 0;
        ssize_t recvBodySize = 0;
        ssize_t nRead        = recvData(m_nConnectFd, tempBuf, MAX_SIZE, 0);
        if (nRead < 0)
            return 0;
        size_t nTotal = 0;
        // GET Content Length From first Buffer
        std::string temp;
        ssize_t     LeftSize = -1;
        double      speed;
        ssize_t     BodySizeInResponse = -1;
        // ParseHeader 中 chunk模式会收集完所有数据后才返回最终结果
        // 其他模式直解析第一次获取的字节数,不做后续的解析
        Encoding EncodingType = ParseHeader(tempBuf, nRead, Response, HeaderSize, BodySize, LeftSize, BodySizeInResponse);
        if (strcasecmp(m_strRequestType.c_str(), "head") == 0) {
            return 0;
        }
        recvBodySize += BodySize;
        if (EncodingType == EncodingLength) {
            nTotal = nRead;
        } else if (EncodingType == EncodingChunk) {
            nTotal = nRead + BodySize;
            // write bytes header first
            if (!Response.getResponseItem(ContentEncoding).empty()) {
                Response.tryDecodeBody();
            }
            std::ofstream fout;
            if (!outfile.empty()) {
                fout.open(outfile.c_str(), std::ios::binary | std::ios::out);
                if (!fout.is_open()) {
                    logger.warning("open %s file failed. err:%s", outfile, strerror(errno));
                    return nTotal;
                } else {
                    if (Response.BodyIsDecoded())
                        fout << Response.getResponseText();
                    else
                        Response.getResponseBytes(fout);
                    fout.close();
                }
            }
        }
        if (LeftSize <= 0 && EncodingType == EncodingLength) {
            logger.debug("Request Finished.");
        } else if (EncodingType == EncodingLength) {
            bool          Appended = false;
            std::ofstream fout;
            if (!outfile.empty()) {
                Appended = true;
                fout.open(outfile.c_str(), std::ios::binary | std::ios::out);
                if (!fout.is_open()) {
                    logger.warning("open %s file failed. err:%s", outfile, strerror(errno));
                    return nTotal;
                } else {
                    // write bytes header first
                    Response.getResponseBytes(fout);
                }
            } else if (outfile.empty() && BodySizeInResponse >= MAX_RECV_BYTES) {
                logger.warning("response bytes %d is out of range(%d). data is not shown. use -o output instead", BodySizeInResponse, MAX_RECV_BYTES);
                Appended = false;
            } else {
                Appended = true;
            }
            while (LeftSize > 0) {
                memset(tempBuf, 0, MAX_SIZE);
                nRead = recvData(m_nConnectFd, tempBuf, MAX_SIZE, 0);
                if (nRead <= 0)
                    break;
                if (Appended && !fout.is_open()) // < limit
                    Response.WriteBodyBytes(tempBuf, nRead);
                else if (Appended && fout.is_open()) // 直接写入文件
                    fout.write(tempBuf, nRead);
                recvBodySize += nRead;
                nTotal += nRead;
                LeftSize -= nRead;
                BodySize += nRead;
                speed = getCurrentSpeed(tBegin, recvBodySize, 'k');

                double leftTime = LeftSize / 1000.0 / speed;
                double Progress = recvBodySize * 100.0 / BodySizeInResponse;
                logger.debug("Process:%.2f%%, Recv Size:%d bytes , LeftSize:%d bytes, "
                             "recvBodySize:%d, speed:%.2f kb/s left time:%.2fs",
                             Progress, nRead, LeftSize, recvBodySize, speed, leftTime);
            }
            if (fout.is_open())
                fout.close();
        } else if (EncodingType == EncodingGzip) {
            while (1) {
                memset(tempBuf, 0, MAX_SIZE);
                nRead = recvData(m_nConnectFd, tempBuf, MAX_SIZE, 0);
                if (nRead <= 0)
                    break;
                Response.WriteBodyBytes(tempBuf, nRead);
                recvBodySize += nRead;
                nTotal += nRead;
                BodySize += nRead;
                speed = getCurrentSpeed(tBegin, recvBodySize, 'k');
                logger.debug("Process:--.--%%, Recv Size:%d bytes , LeftSize:--bytes, "
                             "recvBodySize:%d, speed:%.2f kb/s left time:--s",
                             nRead, recvBodySize, speed);
            }
        }
        speed = getCurrentSpeed(tBegin, recvBodySize, 'k');

        logger.info("total size:%d [%.2fKB], cost Time:%.2fs, Response Header:%d Bytes, "
                    "dataSize:%d bytes speed:%.2f kb/s, EncodingType:%s",
                    nTotal, nTotal / 1000.0, getSpendTime(tBegin), HeaderSize, BodySize, speed, getChunkString(EncodingType));
        return nTotal;
    }

    std::string getChunkString(Encoding nType) {
        switch (nType) {
            case EncodingLength:
                return "Content-Length";
            case EncodingChunk:
                return "Chunked";
            case EncodingGzip:
            default:
                return "Gzip";
        }
        return "Gzip";
    }

    static double getSpendTime(std::chrono::system_clock::time_point &tBegin) {
        auto end      = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - tBegin);
        return double(duration.count()) * std::chrono::microseconds::period::num / std::chrono::microseconds::period::den;
    }

    static double getCurrentSpeed(std::chrono::system_clock::time_point &tBegin, ssize_t receivedBytes, char nType = 'b') {
        auto end      = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - tBegin);
        auto costTime = double(duration.count()) * std::chrono::microseconds::period::num / std::chrono::microseconds::period::den;
        if (nType == 'b') {
            return receivedBytes / costTime;
        } else if (nType == 'k') {
            return receivedBytes / costTime / 1000;
        } else if (nType == 'm') {
            return receivedBytes / costTime / 1000 / 1000;
        }
        return receivedBytes / costTime;
    }

    ssize_t write(const char *writeBuf, ssize_t nSize) {
#ifndef USE_OPENSSL
        return ::send(m_nConnectFd, writeBuf, nSize, MSG_DONTWAIT);
#else
        if (m_bUseSSL)
            return SSLSend(writeBuf, nSize);
        return ::send(m_nConnectFd, writeBuf, nSize, MSG_DONTWAIT);
#endif
    }

    bool connect(cstring url) {
        return connect(HttpUrl(url));
    }

    bool connect(const HttpUrl &url) {
        m_strConnectUrl    = url.fullurl;
        std::string netloc = url.host;
        int         nPort  = url.port;
        if (url.scheme == "https") {
            nPort     = 443;
            m_bUseSSL = true;
        }
        auto currentTime = std::chrono::system_clock::now();
        logger.info("begin to Connect %s:%d ...", netloc, nPort);
        hostent *host = gethostbyname(netloc.c_str());
        if (nullptr == host) {
            m_strErrorString = "can't get host ip addr yet";
            logger.error("can't get host[%s] ip addr yet", netloc);
            return false;
        }
        bool ConnectOK = false;
        int  i         = 0;
        logger.info("get host[%s] by name cost time %.4f s", netloc, getSpendTime(currentTime));
        std::string ipAddress;
        for (; host->h_addr_list[ i ]; ++i) {
            int AFType   = host->h_addrtype;
            m_nConnectFd = socket(AFType, SOCK_STREAM, 0);
            if (m_nConnectFd == -1) {
                logger.info("Create Socket error. errmsg:%s", strerror(errno));
                m_strErrorString = "Create Socket error.";
                m_strErrorString.append(strerror(errno));
                continue;
            }
            sockaddr_in ServerAddress;
            memset(&ServerAddress, 0, sizeof(ServerAddress));
            ServerAddress.sin_family = AFType;
            ServerAddress.sin_port   = htons(nPort);
            ServerAddress.sin_addr   = *(struct in_addr *)host->h_addr_list[ i ];
            ipAddress                = inet_ntoa(*(struct in_addr *)host->h_addr_list[ i ]);
            logger.info("Begin to connect address %s[%s:%d] with timeout:%d", url.fullurl, ipAddress, nPort, m_nConnectTimeout);
            // Set connect timeout
            int oldFlag   = setFDnonBlock(m_nConnectFd);
            int connected = ::connect(m_nConnectFd, (struct sockaddr *)&ServerAddress, sizeof(sockaddr_in));
            if (connected == 0) {
                fcntl(m_nConnectFd, F_SETFL, oldFlag);
                ConnectOK = true;
                logger.info("Connect to Server %s[%s:%d] immediately", url.fullurl, ipAddress, nPort);
                break;
            } else if (errno != EINPROGRESS) {
                logger.error("connect address %s[%s:%d] error ...", url.fullurl, ipAddress, nPort);
                m_strErrorString = "connect address error.";
                m_strErrorString.append(strerror(errno));
                continue;
            } else {
                logger.debug("unblock mode socket is connecting %s[%s:%d]...", url.fullurl, ipAddress, nPort);
                struct timeval tm;
                tm.tv_sec  = m_nConnectTimeout;
                tm.tv_usec = 0;
                fd_set WriteSet;
                FD_ZERO(&WriteSet);
                FD_SET(m_nConnectFd, &WriteSet);
                int res = select(m_nConnectFd + 1, nullptr, &WriteSet, nullptr, &tm);
                if (res < 0) {
                    continue;
                    logger.error("connect address %s[%s:%d] error ..., try next url", url.fullurl, ipAddress, nPort);
                } else if (res == 0) {
                    logger.error("connect address %s[%s:%d] timeout[%d] ... try next url", url.fullurl, ipAddress, nPort, m_nConnectTimeout);
                    continue;
                }
                if (!FD_ISSET(m_nConnectFd, &WriteSet)) {
                    logger.warning("no Events on socket:%d found", m_nConnectFd);
                    close(m_nConnectFd);
                    continue;
                }
                int       error = -1;
                socklen_t len   = sizeof(error);
                if (getsockopt(m_nConnectFd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
                    logger.warning("get socket option failed");
                    close(m_nConnectFd);
                    continue;
                }
                if (error != 0) {
                    logger.warning("connection failed after select with the error:%d", error);
                    close(m_nConnectFd);
                    continue;
                }
                logger.info("connection ready after select with the socket:%d", m_nConnectFd);
                fcntl(m_nConnectFd, F_SETFL, oldFlag);
                ConnectOK = true;
                break;
            }
        }

        // if (::connect(m_nConnectFd, (struct sockaddr *)&ServerAddress, sizeof(sockaddr_in)) < 0)
        // {
        //     logger.error("connect address %s[%s:%d] error ...", url.fullurl, ipAddress, nPort);
        //     continue;
        // }
        if (ConnectOK) {
            logger.info("connect address %s[%s:%d] Success ...", url.fullurl, ipAddress, nPort);
            if (url.scheme == "https") {
#if USE_OPENSSL
                ConnectOK = SSLConnect();
                logger.info("Switch to SSL Connection Channle... and Result is :%d, SSL_VERSION %s", ConnectOK, OPENSSL_VERSION_TEXT);
#endif
            }
            m_bConnected = ConnectOK;
        } else {
            logger.error("connect address %s error ... try times:%d", url.fullurl, i);
            close(m_nConnectFd);
        }

        return ConnectOK;
    }

    void disconnect() {
        ::close(m_nConnectFd);
#if USE_OPENSSL
        SSLDisConnect();
#endif
    }

    int recvData(int fd, void *buf, size_t size, int ops) {
#ifndef USE_OPENSSL
        return ::recv(fd, buf, size, ops);
#else
        if (m_bUseSSL)
            return SSLRecv(fd, buf, size);
        return ::recv(fd, buf, size, ops);
#endif
    }

    void setConnectTimeout(size_t nSecond) {
        m_nConnectTimeout = nSecond;
    }

    int setFDnonBlock(int fd) {
        int flag = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flag | O_NONBLOCK); // 保留原始状态 flag
        return flag;
    }

    bool isConnected() const {
        return m_bConnected;
    }

    std::string getErrorString() const {
        return m_strErrorString;
    }

    void setRequestType(cstring RequestType) {
        m_strRequestType = RequestType;
    }

    std::string getRequestType() const {
        return m_strRequestType;
    }

    int getConnectedFd() const {
        return m_nConnectFd;
    }

private:
    int         m_nConnectFd;
    std::string m_strConnectUrl;
    int         m_nChunkSize;
    size_t      m_nConnectTimeout;
    bool        m_bUseSSL;
    bool        m_bSSLOpened;
    bool        m_bConnected;
    std::string m_strErrorString;
    std::string m_strRequestType;

#ifdef USE_OPENSSL
    typedef struct {
        SSL *    m_ptrHandle;
        SSL_CTX *m_ptrContext;
    } SSL_Connection;
    SSL_Connection *m_pConnection;
#endif

}; // namespace http
class HttpClient {

protected:
    HttpRequest                           m_ReqHeader;
    HttpResponse                          m_Response;
    std::chrono::system_clock::time_point m_ConnectTime;
    SocketClient                          m_SocketClient;
    std::string                           m_strRequestHost;
    std::threadpool                       m_DownloadsThreadPool;
    std::string                           m_strOutputFile;

public:
    class DownloadThreadMgr {
    public:
        struct RangeInfo {
            RangeInfo() = default;
            RangeInfo(ssize_t begin, ssize_t end)
                : nBegin(begin)
                , nEnd(end) {
            }
            ssize_t     nBegin;
            ssize_t     nEnd;
            std::string strRequstHeader;
            std::string strTmpFile;
            std::string strReqUrl;
            std::string strRange() const {
                std::stringstream ss;
                ss << "bytes=" << nBegin << "-" << nEnd;
                return ss.str();
            }
            void setRequestHeader(cstring header) {
                strRequstHeader = header;
            }
            void setThreadTmpFile(cstring tmpFile) {
                strTmpFile = tmpFile;
            }
            void setReqUrl(cstring strUrl) {
                strReqUrl = strUrl;
            }
        };
        DownloadThreadMgr(ssize_t FileLength, int nThreadCount, cstring reqUrl, HttpRequest &RequestHeader, cstring strDownloadPath)
            : m_nThreadCount(nThreadCount)
            , m_strDownLoadFile(strDownloadPath)
            , m_nReceivedBytes(0) {
            ssize_t nBeginBytes      = 0;
            ssize_t nSignlePartBytes = FileLength / max(nThreadCount, 1);
            char    tempName[ L_tmpnam ];
            RequestHeader.setRequestType("GET");
            for (auto i = 0; i < m_nThreadCount; i++) {
                std::string threadName = "Thread ";
                threadName.append(to_string(i));
                RangeInfo info(nBeginBytes, min(nBeginBytes + nSignlePartBytes, FileLength));
                RequestHeader.setHeader("Range", info.strRange());
                info.setRequestHeader(RequestHeader.toStringHeader());
                if (i == 0) {
                    tmpnam(tempName);
                    info.setThreadTmpFile(strDownloadPath + "." + tempName);
                } else {
                    char *ptr = tmpnam(nullptr);
                    info.setThreadTmpFile(strDownloadPath + "." + ptr);
                }
                info.setReqUrl(reqUrl);
                info.setThreadTmpFile(strDownloadPath + "." + threadName);
                m_vthreadsInfo.push_back(std::pair<std::string, RangeInfo>(threadName, info));
                nBeginBytes += nSignlePartBytes + 1;
            }
        }

        bool startThread() {
            auto                     timeBegin = std::chrono::system_clock::now();
            std::vector<std::thread> threadArray;
            for (auto i = 0; i < m_nThreadCount; i++) {
                threadArray.push_back(std::thread(
                    [ this ](int index) {
                        SocketClient client;
                        std::string  strUrl   = m_vthreadsInfo[ 0 ].second.strReqUrl;
                        int          TryTimes = 3;
                        while (TryTimes--) {
                            if (!client.connect(strUrl)) {
                                logger.warning("can't connect to %s url try next %d time...", strUrl, TryTimes);
                                sleep(2);
                                continue;
                            }
                            break;
                        }
                        if (!client.isConnected()) {
                            logger.info("connect %s with error.", strUrl);
                            return;
                        }

                        std::string      threadName      = "Thread " + to_string(index);
                        auto             ThreadtimeBegin = std::chrono::system_clock::now();
                        const RangeInfo &tempRangeInfo   = this->m_vthreadsInfo[ index ].second;
                        cstring          ThreadName      = this->m_vthreadsInfo[ index ].first;

                        ssize_t nReadBytes = tempRangeInfo.nEnd - tempRangeInfo.nBegin;
                        // std::cout << "Requst Header:\n" << tempRangeInfo.strRequstHeader << " -->" << nReadBytes << std::endl;
                        // this->m_lock.lock();
                        int nWrite = client.write(tempRangeInfo.strRequstHeader.c_str(), tempRangeInfo.strRequstHeader.size());
                        // this->m_lock.unlock();
                        if (nWrite != tempRangeInfo.strRequstHeader.size()) {
                            logger.info("threadName:%s write %d bytes failed. sent %d bytes", ThreadName, tempRangeInfo.strRequstHeader.size(), nWrite);
                            return;
                        }
                        std::string   threadFile = tempRangeInfo.strTmpFile;
                        std::ofstream fout(threadFile.c_str(), std::ios::binary);
                        char          temp[ MAX_SIZE ];
                        bool          FirstData      = true;
                        ssize_t       totalReadBytes = 0;
                        while (nReadBytes) {
                            memset(temp, 0, MAX_SIZE);
                            // this->m_lock.lock();
                            int nRead = client.recvData(client.getConnectedFd(), temp, MAX_SIZE, 0);
                            // this->m_lock.unlock();
                            if (nRead <= 0) {
                                logger.info("threadName:%s recv bytes blow 0", threadName);
                                break;
                            }
                            if (FirstData) {
                                bool        bFindBody = false;
                                std::string strLine;
                                for (size_t i = 0; i < nRead;) {
                                    if (i + 1 < nRead && temp[ i ] == '\r' && temp[ i + 1 ] == '\n' && !bFindBody) {
                                        i += 2;
                                        if (strLine.empty()) {
                                            bFindBody = true;
                                            continue;
                                        }
                                        strLine.clear();
                                    } else if (!bFindBody) {
                                        strLine.push_back(temp[ i ]);
                                        i += 1;
                                        continue;
                                    } else if (bFindBody) {
                                        fout << temp[ i ];
                                        nReadBytes -= 1;
                                        totalReadBytes += 1;
                                        i++;
                                    }
                                }
                                FirstData = false;
                            } else {
                                nReadBytes -= nRead;
                                fout.write(temp, nRead);
                                totalReadBytes += nRead;
                            }
                            if (nReadBytes < 0) {
                                logger.debug("threadName:%s left bytes < 0 exit..", threadName);
                                break;
                            }
                            logger.debug("threadName:%s recv bytes:%d totalReadBytes:%d Left bytes:%d speed:%s kb/s", ThreadName, nRead, totalReadBytes, nReadBytes,
                                         client.getCurrentSpeed(ThreadtimeBegin, totalReadBytes, 'k'));
                        }
                        fout.close();
                        this->m_lock.lock();
                        this->m_nReceivedBytes += totalReadBytes;
                        this->m_lock.unlock();
                        return;
                    },
                    i));
            }
            for (int i = 0; i < m_nThreadCount; i++) {
                if (threadArray[ i ].joinable()) {
                    threadArray[ i ].join();
                    logger.info("%d thread exit...", i);
                }
            }
            logger.info("write bytes size:%d, spent time:%.2fs, speed:%s kb/s begin to adjust tmpfiles...", m_nReceivedBytes, SocketClient::getSpendTime(timeBegin),
                        SocketClient::getCurrentSpeed(timeBegin, m_nReceivedBytes, 'k'));
            auto  AdjustBegin = std::chrono::system_clock::now();
            FILE *fp          = fopen(m_strDownLoadFile.c_str(), "wb");
            if (fp == nullptr)
                return false;
            int n = 0;
            for (int i = 0; i < m_nThreadCount; i++) {
                cstring resultTmpFile = m_vthreadsInfo[ i ].second.strTmpFile;
                FILE *  fd            = fopen(resultTmpFile.c_str(), "rb");
                char    ch;

                if (fd == nullptr)
                    continue;
                while ((ch = fgetc(fd)) != EOF) {
                    fputc(ch, fp);
                    n++;
                }
                fclose(fd);
                unlink(resultTmpFile.c_str());
            }
            fclose(fp);
            logger.info("write bytes size:%d , speed time:%.2f s", n, SocketClient::getSpendTime(AdjustBegin));
            return false;
        }

    protected:
        ssize_t                                        m_nFileLength;
        int                                            m_nThreadCount;
        std::vector<std::pair<std::string, RangeInfo>> m_vthreadsInfo;
        ssize_t                                        m_nReceivedBytes;
        std::string                                    m_strDownLoadFile;
        std::mutex                                     m_lock;
    };

    void setOutputFile(cstring file) {
        m_strOutputFile = file;
    }

    HttpResult Request(cstring reqUrl, bool bRedirect = false, bool verbose = false) {
        HttpUrl tempUrl(reqUrl);
    request:
        m_Response.clear();
        if (tempUrl.host != m_strRequestHost) { // last has been created. no need to connect again.
            m_SocketClient.disconnect();
            if (!m_SocketClient.connect(tempUrl))
                return HttpResult(400, "", "Connect timeout.");
            m_ReqHeader.setHeader("Host", tempUrl.host);
            m_strRequestHost = tempUrl.host;
        } else {
            logger.info("last conencted host %s is same to this requst. skip connect again.", m_strRequestHost);
        }
        std::stringstream header;
        std::string       RequestPath = tempUrl.path;
        if (!tempUrl.query.empty()) {
            if (RequestPath.back() != '?')
                RequestPath.append("?");
            RequestPath.append(tempUrl.query);
        }
        m_ReqHeader.setRequestPath(RequestPath);
        // 请求数据部分
        std::string HttpRequestString = m_ReqHeader.toStringHeader();
        if (verbose) {
            std::cout << "* Request Header\n";
            std::cout << m_ReqHeader;
        }
        ssize_t nWrite = m_SocketClient.write(HttpRequestString.c_str(), HttpRequestString.size());
        if (nWrite != HttpRequestString.size())
            return HttpResult(500, "", "Write data to server error");
        ssize_t nRead = m_SocketClient.read(m_Response, m_strOutputFile);
        if (verbose) {
            std::cout << "* Response Body:\n" << m_Response;
            std::cout << "------------------\n";
        }

        if (atoi(m_Response.getResponseItem("code").c_str()) / 100 == 3 && bRedirect) {
            std::string strRedirectUrl = GetRedirectLocation(m_Response.getResponseItem(Location), m_ReqHeader.getRequestPath());
            logger.info("begin to redirect url:%s", strRedirectUrl);
            if (strncmp(strRedirectUrl.c_str(), "http", 4) == 0) {
                tempUrl = HttpUrl(strRedirectUrl);
            } else {
                std::string requestUrl = tempUrl.getHostUrl() + strRedirectUrl;
                tempUrl.resetUrl(requestUrl);
            }
            goto request;
        }

        if (strcasecmp(m_SocketClient.getRequestType().c_str(), "head") == 0) {
            logger.debug("Requst Header Only.");
        } else if (m_Response.getResponseBytesSize() == 0 && atoi(m_Response.getResponseItem("code").c_str()) / 100 != 2) {
            logger.info("get response data is empty.");
            return HttpResult(500, "", "Can't get any data from url");
        }

        return HttpResult(atoi(m_Response.getResponseItem("code").c_str()), m_Response.getResponseText(), m_Response.getResponseItem("message"));
    }

    std::string GetRedirectLocation(cstring strLocation, cstring RequestPath) {
        std::string strTempLocation;
        // remove #fragment
        std::string strLocationWithOutFragment = strLocation;
        if (strLocation.find("#") != std::string::npos)
            strLocationWithOutFragment = strLocationWithOutFragment.substr(0, strLocation.find("#"));
        if (strLocation.find("http//") == 0) {
            strTempLocation = "http://";
            strTempLocation.append(strLocationWithOutFragment.substr(std::string("http//").size()));
        } else if (strLocationWithOutFragment.find("https//") == 0) {
            strTempLocation = "https://";
            strTempLocation.append(strLocationWithOutFragment.substr(std::string("https//").size()));
        } else {
            if (RequestPath.rfind("/") != std::string::npos)
                strTempLocation = RequestPath.substr(0, RequestPath.rfind("/"));
            strTempLocation.append("/");
            strTempLocation.append(strLocationWithOutFragment);
        }
        return strTempLocation;
    }

    HttpResult Get(cstring reqUrl, bool bRedirect = false, bool verbose = false) {
        m_ReqHeader.setRequestType("GET");
        m_SocketClient.setRequestType("GET");
        return Request(reqUrl, bRedirect, verbose);
    }

    HttpResult Post(cstring reqUrl, cstring strParams, bool bRedirect = false, bool verbose = false) {
        m_ReqHeader.setRequestType("POST");
        m_SocketClient.setRequestType("POST");
        m_ReqHeader.setParams(strParams);
        return Request(reqUrl, bRedirect, verbose);
    }

    HttpResult Head(cstring reqUrl, bool verbose = false) {
        m_ReqHeader.setRequestType("HEAD");
        m_SocketClient.setRequestType("HEAD");
        return Request(reqUrl, false, verbose);
    }

    void SaveResultToFile(cstring fileName) {
        if (m_Response.getResponseBytesSize() == 0) {
            logger.warning("Download File bytes is empty. skip download");
            return;
        }
        std::ofstream fout(fileName, std::ios_base::binary);
        if (!fout.is_open()) {
            logger.warning("Download File %s open failed. skip download", fileName);
            return;
        }
        if (m_Response.BodyIsDecoded()) {
            fout << m_Response.getResponseText();
        } else {
            m_Response.getResponseBytes(fout);
        }
        fout.close();
    }

    void setBasicAuthUserPass(cstring user, cstring passwd) {
        std::stringstream ss;
        ss << user << ":" << passwd;
        std::string Base64String;
        HashUtils::EncodeBase64(ss.str(), Base64String);
        this->setHeader(Authorization, "Basic " + Base64String);
    }

    void setUserAgent(cstring AgentVal) {
        this->setHeader(UserAgent, AgentVal);
    }

    void setContentType(cstring ContentTypeVal) {
        this->setHeader(ContentType, ContentTypeVal);
    }

    void setAcceptLanguage(cstring AcceptLanguageVal) {
        this->setHeader(AcceptLanguage, AcceptLanguageVal);
    }

    void setAcceptEncoding(cstring AcceptEncodingVal) {
        this->setHeader(AcceptEncoding, AcceptEncodingVal);
    }

    void setAccept(cstring AcceptVal) {
        this->setHeader(Accept, AcceptVal);
    }

    void setHttpVersion(utils::HttpVersion version) {
        if (version == utils::HTTP_1_0)
            this->m_ReqHeader.setHttpVersion(HTTP1_0);
        else if (version == utils::HTTP_1_1)
            this->m_ReqHeader.setHttpVersion(HTTP1_1);
    }
    template <class T> void setHeader(cstring key, const T &val) {
        this->m_ReqHeader.setHeader(key, val);
    }

    void setConnectTimeout(size_t nSeconds) {
        m_SocketClient.setConnectTimeout(nSeconds);
    }

    void setCookie(cstring strCookie) {
        this->setHeader(Cookie, strCookie);
    }

    void SaveTempFile() {
        std::ofstream fout(TempFile, std::ios::binary);
        if (!fout.is_open())
            return;
        fout << "Request Header:\n" << m_ReqHeader.toStringHeader() << std::endl << "<" << std::endl;
        if (m_Response.getResponseBytesSize() != 0) {
            std::string strvalue = m_Response.getResponseItem(ContentLength);
            fout << "BodyBytes:";
            if (!strvalue.empty())
                fout << strvalue << std::endl;
            fout << "ContentEncoding:" << m_Response.getResponseItem(ContentEncoding) << std::endl;
            fout << "ContentRecvLength:" << m_Response.getResponseBytesSize() << std::endl;
            fout << "Bytes:\n";
            m_Response.getResponseBytes(fout);
        }
        fout.close();
        m_SocketClient.disconnect();
    }

    void setReferer(cstring strReferer) {
        this->setHeader(Referer, strReferer);
    }

    std::string getCookie() {
        return m_Response.getCookie();
    }

    void DownloadFile(cstring reqUrl, cstring downloadPath, int nThreadCount = 10, bool verbose = false) {
        HttpResult  result          = Head(reqUrl, verbose);
        std::string strDownLoadPath = downloadPath;
        if (strDownLoadPath.empty()) {
            strDownLoadPath = "http.download";
        }
        if (result.status_code() / 100 == 2) {
            m_SocketClient.disconnect();
            logger.info("current request url:%s is exist. and download it to file with %d threads", reqUrl, nThreadCount);
            ssize_t           fileLength = atol(m_Response.getResponseItem(ContentLength).c_str());
            DownloadThreadMgr mgr(fileLength, nThreadCount, reqUrl, m_ReqHeader, strDownLoadPath);
            mgr.startThread();
        } else {
            logger.info("current request url:%s is not exist. errcode:%d %s and ignore to download", reqUrl, result.status_code(), result.error());
        }
    }
};

} // namespace http
