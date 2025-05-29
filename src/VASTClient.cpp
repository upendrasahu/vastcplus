#include "VASTClient.h"
#include <curl/curl.h>
#include <stdexcept>
#include <sstream>

VASTClient::VASTClient(const std::string& baseUrl, const std::string& token)
    : baseUrl(baseUrl), token(token) {}

std::string VASTClient::get(const std::string& endpoint, const std::map<std::string, std::string>& headers) {
    return request("GET", endpoint, "", headers);
}

std::string VASTClient::post(const std::string& endpoint, const std::string& body, const std::map<std::string, std::string>& headers) {
    return request("POST", endpoint, body, headers);
}

std::string VASTClient::put(const std::string& endpoint, const std::string& body, const std::map<std::string, std::string>& headers) {
    return request("PUT", endpoint, body, headers);
}

std::string VASTClient::del(const std::string& endpoint, const std::map<std::string, std::string>& headers) {
    return request("DELETE", endpoint, "", headers);
}

std::string VASTClient::request(const std::string& method, const std::string& endpoint, const std::string& body, const std::map<std::string, std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string url = baseUrl + endpoint;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());

    struct curl_slist* headerList = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        headerList = curl_slist_append(headerList, header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    std::ostringstream responseStream;
    auto writeCallback = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        std::ostringstream* stream = static_cast<std::ostringstream*>(userdata);
        size_t totalSize = size * nmemb;
        stream->write(ptr, totalSize);
        return totalSize;
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStream);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        throw std::runtime_error(curl_easy_strerror(res));
    }

    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return responseStream.str();
}