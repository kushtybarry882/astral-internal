#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <curl/curl.h>
#endif

#pragma comment(lib, "ws2_32.lib")

namespace KeyAuth {

struct user_data {
    std::string user_id{};
    std::string username{};
    std::string email{};
    std::string hwid{};
    std::string ip{};
    std::string data{};
    std::string created_at{};
    std::string expires_at{};
    std::string last_login{};
    std::string subscription_status{};
    std::string auth_type{};
    std::string avatar_id{};
};

struct app_data {
    std::string num_users{};
    std::string num_keys{};
    std::string app_status{};
    std::string app_version{};
    std::string app_latest_version{};
    std::string app_download_url{};
    std::string app_changelog{};
};

struct response {
    bool success{};
    std::string message{};
    std::string sessionid{};
};

class api {
public:
    api(const std::string& app_name, const std::string& owner_id, const std::string& app_version, const std::string& url = "https://keyauth.win/api/1.2/", const std::string& secret = "")
        : m_app_name(app_name), m_owner_id(owner_id), m_app_version(app_version), m_url(url), m_secret(secret), m_token("") {}

    bool init();
    bool register_user(const std::string& username, const std::string& password, const std::string& license_key);
    void license(const std::string& license_key);
    bool check();
    void log(const std::string& message);
    bool fetch_stats();
    bool upgrade(const std::string& license_key, const std::string& new_level);
    bool redeem_coins(const std::string& redeem_code);
    bool use_feature(const std::string& feature_id);
    bool ban(const std::string& user_id);
    bool unban(const std::string& user_id);
    bool get_var(const std::string& var_id, const std::string& var_name, int var_index, std::string& out_value);
    bool set_var(const std::string& var_id, const std::string& var_name, int var_index, const std::string& value);
    bool check_captcha();

    response response{};

    // S-12: clear the app secret from memory after init() — limits exposure.
    void clear_secret() {
        m_secret.clear();
        m_secret.shrink_to_fit();
    }

    [[nodiscard]] const user_data& get_user_data() const { return m_user_data; }
    [[nodiscard]] const app_data& get_app_data() const { return m_app_data; }

private:
    std::string m_app_name;
    std::string m_owner_id;
    std::string m_app_version;
    std::string m_url;
    std::string m_secret;
    std::string m_token;

    user_data m_user_data{};
    app_data m_app_data{};

    std::string send_request(const std::string& endpoint, const std::string& payload);
    std::string get_hwid();
};

} // namespace KeyAuth
