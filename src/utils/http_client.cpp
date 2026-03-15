#include "utils/http_client.hpp"
#include <borealis.hpp>
#include <curl/curl.h>
#include <cstring>
#include <fstream>

namespace vita_ma {

HttpClient& HttpClient::instance() {
    static HttpClient instance;
    return instance;
}

HttpClient::HttpClient() {
    m_curl = curl_easy_init();
    if (m_curl) {
        auto* curl = static_cast<CURL*>(m_curl);

        // SSL settings
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

        // Timeouts
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        // TCP keepalive
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);

        // Follow redirects
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

        // Buffer size
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);

        // HTTP/1.1
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

        // User agent
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "VitaMusicAssistant/1.0");

        // Low speed limit
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    }
}

HttpClient::~HttpClient() {
    if (m_curl) {
        curl_easy_cleanup(static_cast<CURL*>(m_curl));
    }
}

size_t HttpClient::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

size_t HttpClient::headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t totalSize = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);

    std::string line(buffer, totalSize);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
        (*headers)[key] = val;
    }

    return totalSize;
}

HttpResponse HttpClient::get(const std::string& url,
                              const std::map<std::string, std::string>& headers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    HttpResponse response;

    if (!m_curl) {
        response.error = "CURL not initialized";
        return response;
    }

    auto* curl = static_cast<CURL*>(m_curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    // Set headers
    struct curl_slist* headerList = nullptr;
    for (auto& h : m_defaultHeaders) {
        std::string hdr = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, hdr.c_str());
    }
    for (auto& h : headers) {
        std::string hdr = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, hdr.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
        response.success = (response.status_code >= 200 && response.status_code < 300);
    } else {
        response.error = curl_easy_strerror(res);
        brls::Logger::error("HTTP GET error: {} ({})", response.error, url);
    }

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);

    return response;
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body,
                               const std::map<std::string, std::string>& headers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    HttpResponse response;

    if (!m_curl) {
        response.error = "CURL not initialized";
        return response;
    }

    auto* curl = static_cast<CURL*>(m_curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    struct curl_slist* headerList = nullptr;
    for (auto& h : m_defaultHeaders) {
        std::string hdr = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, hdr.c_str());
    }
    for (auto& h : headers) {
        std::string hdr = h.first + ": " + h.second;
        headerList = curl_slist_append(headerList, hdr.c_str());
    }
    headerList = curl_slist_append(headerList, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
        response.success = (response.status_code >= 200 && response.status_code < 300);
    } else {
        response.error = curl_easy_strerror(res);
    }

    curl_slist_free_all(headerList);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);

    return response;
}

std::vector<uint8_t> HttpClient::downloadBinary(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint8_t> data;

    if (!m_curl) return data;

    auto* curl = static_cast<CURL*>(m_curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    // Write to vector
    auto writeCb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        size_t totalSize = size * nmemb;
        auto* vec = static_cast<std::vector<uint8_t>*>(userp);
        auto* bytes = static_cast<uint8_t*>(contents);
        vec->insert(vec->end(), bytes, bytes + totalSize);
        return totalSize;
    };

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_write_callback>(writeCb));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        data.clear();
    }

    return data;
}

bool HttpClient::downloadFile(const std::string& url, const std::string& filepath,
                               std::function<void(float)> progressCb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_curl) return false;

    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    auto* curl = static_cast<CURL*>(m_curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    auto writeCb = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        auto* f = static_cast<std::ofstream*>(userp);
        size_t totalSize = size * nmemb;
        f->write(static_cast<char*>(contents), totalSize);
        return totalSize;
    };

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_write_callback>(writeCb));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);

    CURLcode res = curl_easy_perform(curl);
    file.close();

    return res == CURLE_OK;
}

std::string HttpClient::urlEncode(const std::string& str) {
    std::string result;
    for (char c : str) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            result += buf;
        }
    }
    return result;
}

std::string HttpClient::urlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '%' && i + 2 < str.size()) {
            char hex[3] = {str[i + 1], str[i + 2], '\0'};
            result += static_cast<char>(strtol(hex, nullptr, 16));
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

void HttpClient::setDefaultHeader(const std::string& key, const std::string& value) {
    m_defaultHeaders[key] = value;
}

} // namespace vita_ma
