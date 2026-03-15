#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <mutex>

namespace vita_ma {

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    bool success = false;
    std::string error;
};

using HttpCallback = std::function<void(const HttpResponse&)>;

class HttpClient {
public:
    static HttpClient& instance();

    // Synchronous requests
    HttpResponse get(const std::string& url,
                     const std::map<std::string, std::string>& headers = {});
    HttpResponse post(const std::string& url,
                      const std::string& body = "",
                      const std::map<std::string, std::string>& headers = {});

    // Download to file
    bool downloadFile(const std::string& url, const std::string& filepath,
                      std::function<void(float)> progressCallback = nullptr);

    // Download to memory (for images)
    std::vector<uint8_t> downloadBinary(const std::string& url);

    // URL encode/decode
    static std::string urlEncode(const std::string& str);
    static std::string urlDecode(const std::string& str);

    // Set default headers
    void setDefaultHeader(const std::string& key, const std::string& value);

private:
    HttpClient();
    ~HttpClient();

    void* m_curl = nullptr;  // CURL handle
    std::mutex m_mutex;
    std::map<std::string, std::string> m_defaultHeaders;

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata);
    static int progressCallback(void* clientp, double dltotal, double dlnow,
                                double ultotal, double ulnow);
};

} // namespace vita_ma
