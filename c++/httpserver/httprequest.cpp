#include "httprequest.h"
namespace http {

std::string HttpRequest::toStringHeader() {
    std::stringstream ss;
    ss << m_strRequestType << " " << m_strRequestPath << " " << m_strHttpVersion << CTRL;
    for (auto &item : m_vReqestHeader)
        ss << item.first << ": " << item.second << CTRL;
    if (!m_strRangeBytes.empty())
        ss << "Range: " << m_strRangeBytes << CTRL;
    ss << "Host: " << m_strRequestHost << CTRL;

    if (!m_strPostParams.empty())
        ss << CTRL << m_strPostParams;
    ss << CTRL;
    return ss.str();
}

void HttpRequest::setParams(const std::map<std::string, std::string> &headerMap) {
    for (auto iter : headerMap) {
        m_HeaderMap.insert(iter);
    }
    size_t nPos = m_strRequestPath.find("?");
    if (nPos != std::string::npos) {
        std::string strKey, strVal;
        bool        bFindKey = false;
        for (int i = nPos + 1; i < m_strRequestPath.size(); ++i) {
            if (m_strRequestPath[ i ] == '=')
                bFindKey = true;
            else if (m_strRequestPath[ i ] == '&') {
                bFindKey = false;
                m_HeaderMap.insert({strKey, strVal});
                strKey = strVal = "";
            } else if (bFindKey) {
                strVal.push_back(m_strRequestPath[ i ]);
            } else if (!bFindKey)
                strKey.push_back(m_strRequestPath[ i ]);
        }
        if(!strKey.empty())
            m_HeaderMap.insert({strKey, strVal});
    }
}

std::string HttpRequest::get(const std::string &key) {
    for (auto item : m_vReqestHeader) {
        if (strcasecmp(item.first.c_str(), key.c_str()) == 0)
            return item.second;
    }
    return "";
}

std::string HttpRequest::getRequestType() const {
    return m_strRequestType;
}
void HttpRequest::setRequestType(const std::string &strRequestType) {
    m_strRequestType = strRequestType;
}
std::string HttpRequest::getHttpVersion() const {
    return m_strHttpVersion;
}
void HttpRequest::setHttpVersion(const std::string &strHttpVersion) {
    m_strHttpVersion = strHttpVersion;
}
std::string HttpRequest::getPostParams() const {
    return m_strPostParams;
}
void HttpRequest::setPostParams(const std::string &strPostParams) {
    m_strPostParams = strPostParams;
}
std::string HttpRequest::getRequestPath() const {
    return m_strRequestPath;
}
void HttpRequest::setRequestPath(const std::string &strRequestPath) {
    m_strRequestPath     = strRequestPath;
    m_strRequestFilePath = strRequestPath;
    if (strRequestPath.find("?") != std::string::npos)
        m_strRequestFilePath = m_strRequestFilePath.substr(0, m_strRequestFilePath.find("?"));
}
std::string HttpRequest::getRequestFilePath() const {
    return m_strRequestFilePath;
}
void HttpRequest::setRequestFilePath(const std::string &strRequestFilePath) {
    m_strRequestFilePath = UrlUtils::UrlDecode(strRequestFilePath);
}

std::string HttpRequest::getParams(const std::string &key) {
    if (!m_HeaderMap.count(key))
        return "";
    return m_HeaderMap.at(key);
}
std::map<std::string, std::string> HttpRequest::getAllParams() const {
    return m_HeaderMap;
}

} // namespace http