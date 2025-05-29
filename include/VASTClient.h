#ifndef VASTCLIENT_H
#define VASTCLIENT_H

#include <string>
#include <map>

class VASTClient {
public:
    VASTClient(const std::string& baseUrl, const std::string& token);

    std::string get(const std::string& endpoint, const std::map<std::string, std::string>& headers = {});
    std::string post(const std::string& endpoint, const std::string& body, const std::map<std::string, std::string>& headers = {});
    std::string put(const std::string& endpoint, const std::string& body, const std::map<std::string, std::string>& headers = {});
    std::string del(const std::string& endpoint, const std::map<std::string, std::string>& headers = {});

private:
    std::string baseUrl;
    std::string token;

    std::string request(const std::string& method, const std::string& endpoint, const std::string& body, const std::map<std::string, std::string>& headers);
};

#endif // VASTCLIENT_H