#include "httpconfig.h"
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <vector>
namespace http {
class HttpRequest {
public:
    HttpRequest()
        : m_strPostParams("") {
    }
    typedef std::vector<std::pair<std::string, std::string>> ResourceMap;
    template <class T> void                                  setHeader(const std::string &key, const T &val) {
        std::string tmpVal = std::stringstream(val).str();
        if (key == "Host")
            m_strRequestHost = tmpVal;
        else if (key == "Range")
            m_strRangeBytes = tmpVal;
        else
            m_vReqestHeader.push_back(std::pair<std::string, std::string>(key, tmpVal));
    }
    std::string          toStringHeader();
    std::string          get(const std::string &key);
    std::string          getRequestType() const;
    void                 setRequestType(const std::string &strRequestType);
    std::string          getHttpVersion() const;
    void                 setHttpVersion(const std::string &strHttpVersion);
    std::string          getPostParams() const;
    void                 setPostParams(const std::string &strPostParams);
    std::string          getRequestPath() const;
    void                 setRequestPath(const std::string &strRequestPath);
    std::string          getRequestFilePath() const;
    void                 setRequestFilePath(const std::string &strRequestFilePath);
    friend std::ostream &operator<<(std::ostream &os, HttpRequest &obj) {
        os << "> " << obj.m_strRequestType << " " << obj.m_strRequestPath << " " << obj.m_strHttpVersion << CTRL;
        for (auto &item : obj.m_vReqestHeader)
            os << "> " << item.first << ": " << item.second << CTRL;
        if (!obj.m_strRangeBytes.empty())
            os << "Range: " << obj.m_strRangeBytes << CTRL;
        os << "> Host: " << obj.m_strRequestHost << CTRL;
        if (!obj.m_strPostParams.empty())
            os << obj.m_strPostParams << CTRL;
        return os;
    }
    void setParams(const std::map<std::string, std::string> &headerMap);

    std::string                        getParams(const std::string &key);
    std::map<std::string, std::string> getAllParams() const;

private:
    std::string m_strRequestType;
    std::string m_strHttpVersion;
    std::string m_strPostParams;
    std::string m_strRequestPath;
    std::string m_strRequestFilePath;

    ResourceMap                        m_vReqestHeader;
    std::string                        m_strRequestHost;
    std::string                        m_strRangeBytes;
    std::map<std::string, std::string> m_HeaderMap;
};
} // namespace http