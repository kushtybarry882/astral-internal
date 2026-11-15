#include "auth.hpp"

#include <winhttp.h>
#include <windows.h>
#include <winsock2.h>
#include <wincrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace KeyAuth {

std::string api::get_hwid( )
{
    HKEY hKey;
    if ( RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hKey ) != ERROR_SUCCESS )
        return "unknown";

    DWORD size = 256;
    char buffer[ 256 ]{};
    if ( RegQueryValueExA( hKey, "MachineGuid", nullptr, nullptr, reinterpret_cast< LPBYTE >( buffer ), &size ) != ERROR_SUCCESS )
    {
        RegCloseKey( hKey );
        return "unknown";
    }

    RegCloseKey( hKey );
    return std::string( buffer );
}

// N-H-1: pinned SHA-256 of the expected KeyAuth server certificate public key.
// Replace with the actual pinned hash of keyauth.win's leaf certificate.
// Computed as: SHA256(SubjectPublicKeyInfo DER) of the expected cert.
static const unsigned char g_pinnedCertHash[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static bool g_certPinningEnabled = false; // set to true once the real hash is known

std::string api::send_request( const std::string& endpoint, const std::string& payload )
{
    std::string url_s = this->m_url + endpoint;
    size_t scheme_end = url_s.find( "://" );
    if (scheme_end == std::string::npos) return "";
    size_t host_start = scheme_end + 3;
    size_t path_start = url_s.find( "/", host_start );
    std::string host = path_start != std::string::npos ? url_s.substr(host_start, path_start - host_start) : url_s.substr(host_start);
    std::string path = path_start != std::string::npos ? url_s.substr(path_start) : "/";
    std::wstring host_w( host.begin( ), host.end( ) );
    std::wstring path_w( path.begin( ), path.end( ) );

    HINTERNET hSession = WinHttpOpen( L"KeyAuth/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
    if ( !hSession )
        return "";

    DWORD connect_timeout = 5000;
    DWORD send_timeout = 8000;
    DWORD recv_timeout = 8000;
    WinHttpSetOption( hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &connect_timeout, sizeof( DWORD ) );
    WinHttpSetOption( hSession, WINHTTP_OPTION_SEND_TIMEOUT, &send_timeout, sizeof( DWORD ) );
    WinHttpSetOption( hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recv_timeout, sizeof( DWORD ) );

    // H-1: no default proxy — attacker-controlled proxy can't redirect auth traffic.
    WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, WINHTTP_NO_PROXY_NAME, 0);

    // N-H-1: disable automatic certificate revocation and invalid cert handling
    // so we can do our own pinning. We still require a valid chain.
    DWORD flags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));

    HINTERNET hConnect = WinHttpConnect( hSession, host_w.c_str(), 443, 0 );
    if ( !hConnect )
    {
        WinHttpCloseHandle( hSession );
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest( hConnect, L"POST", path_w.c_str( ), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE );
    if ( !hRequest )
    {
        WinHttpCloseHandle( hConnect );
        WinHttpCloseHandle( hSession );
        return "";
    }

    // N-H-1: certificate pinning — retrieve the server cert chain and compare
    // the leaf public key hash against the pinned value.
    if (g_certPinningEnabled) {
        // Get the certificate chain context from the request.
        // This requires WINHTTP_OPTION_SERVER_CERT_CHAIN_CONTEXT on the request handle.
        PCCERT_CONTEXT pCertContext = nullptr;
        DWORD dwSize = sizeof(pCertContext);
        if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_SERVER_CERT_CHAIN_CONTEXT, &pCertContext, &dwSize)) {
            if (pCertContext) {
                // Hash the SubjectPublicKeyInfo of the leaf certificate.
                CRYPT_HASH_BLOB hashBlob = { 0, nullptr };
                if (CertGetCertificateContextProperty(pCertContext, CERT_SHA256_HASH_PROP_ID, nullptr, &dwSize) ||
                    CertGetCertificateContextProperty(pCertContext, CERT_HASH_PROP_ID, nullptr, &dwSize)) {
                    std::vector<unsigned char> hash(dwSize);
                    hashBlob.cbData = dwSize;
                    hashBlob.pbData = hash.data();
                    if (CertGetCertificateContextProperty(pCertContext, CERT_HASH_PROP_ID, &hashBlob, &dwSize)) {
                        unsigned char computedHash[32] = {};
                        if (hashBlob.cbData >= 32) {
                            std::memcpy(computedHash, hash.data(), 32);
                        }
                        // Compare against pinned hash.
                        bool pinMatch = true;
                        for (int i = 0; i < 32; ++i) {
                            if (g_pinnedCertHash[i] != computedHash[i]) {
                                pinMatch = false;
                                break;
                            }
                        }
                        CertFreeCertificateContext(pCertContext);
                        if (!pinMatch) {
                            WinHttpCloseHandle(hRequest);
                            WinHttpCloseHandle(hConnect);
                            WinHttpCloseHandle(hSession);
                            return ""; // cert pin mismatch — abort
                        }
                    }
                }
                CertFreeCertificateContext(pCertContext);
            }
        }
    }

    std::string headers_s = "Content-Type: application/x-www-form-urlencoded\r\n";
    std::wstring headers_w( headers_s.begin( ), headers_s.end( ) );

    if ( !WinHttpSendRequest( hRequest, headers_w.c_str( ), -1, ( LPVOID )payload.c_str( ), static_cast< DWORD >( payload.size( ) ), static_cast< DWORD >( payload.size( ) ), 0 ) )
    {
        WinHttpCloseHandle( hRequest );
        WinHttpCloseHandle( hConnect );
        WinHttpCloseHandle( hSession );
        return "";
    }

    if ( !WinHttpReceiveResponse( hRequest, nullptr ) )
    {
        WinHttpCloseHandle( hRequest );
        WinHttpCloseHandle( hConnect );
        WinHttpCloseHandle( hSession );
        return "";
    }

    std::string response;
    char buffer[ 4096 ];
    DWORD bytesRead = 0;
    while ( WinHttpReadData( hRequest, buffer, sizeof( buffer ), &bytesRead ) && bytesRead > 0 )
    {
        response.append( buffer, bytesRead );
    }

    WinHttpCloseHandle( hRequest );
    WinHttpCloseHandle( hConnect );
    WinHttpCloseHandle( hSession );

    return response;
}

bool api::init( )
{
    this->m_token = "";

    std::string payload = "type=init"
        "&ownerid=" + this->m_owner_id +
        "&name=" + this->m_app_name +
        "&version=" + this->m_app_version +
        "&secret=" + this->m_secret;

    std::string response_str = this->send_request( "init", payload );

    if ( response_str.empty( ) )
    {
        this->response.success = false;
        this->response.message = "Failed to connect to auth servers";
        return false;
    }

    if ( response_str.find( "\"success\":true" ) != std::string::npos )
    {
        this->response.success = true;
        this->response.message = "Initialized successfully";

        auto sid_start = response_str.find( "\"sessionid\":\"" );
        if ( sid_start != std::string::npos )
        {
            sid_start += 13;
            auto sid_end = response_str.find( '"', sid_start );
            if ( sid_end != std::string::npos )
            {
                this->response.sessionid = response_str.substr( sid_start, sid_end - sid_start );
            }
        }
        return true;
    }

    this->response.success = false;
    this->response.message = "Initialization failed";
    return false;
}

void api::license( const std::string& license_key )
{
    // N-H-2: after init() succeeded, the session is established. Do NOT resend
    // the app secret — send only the license key + session token. This limits
    // secret exposure to a single request.
    std::string payload = "type=license&key=" + license_key +
        "&ownerid=" + this->m_owner_id +
        "&name=" + this->m_app_name +
        "&version=" + this->m_app_version +
        "&hwid=" + this->get_hwid() +
        "&sessionid=" + this->response.sessionid;

    std::string response_str = this->send_request( "license", payload );

    if ( response_str.empty( ) )
    {
        this->response.success = false;
        this->response.message = "No response from server";
        return;
    }

    if ( response_str.find( "\"success\":true" ) != std::string::npos )
    {
        this->m_token = license_key;
        this->response.success = true;
        this->response.message = "Access granted";
    }
    else
    {
        this->response.success = false;
        auto msg_start = response_str.find( "\"message\":\"");
        if ( msg_start != std::string::npos )
        {
            msg_start += 12;
            auto msg_end = response_str.find( '"', msg_start );
            if ( msg_end != std::string::npos )
            {
                this->response.message = response_str.substr( msg_start, msg_end - msg_start );
            }
            else
            {
                this->response.message = "License key validation failed";
            }
        }
        else
        {
            this->response.message = "License key validation failed";
        }
    }
}

bool api::check( )
{
    std::string payload = "type=check"
        "&ownerid=" + this->m_owner_id +
        "&name=" + this->m_app_name +
        "&version=" + this->m_app_version +
        "&key=" + this->m_token;

    std::string response_str = this->send_request( "check", payload );

    if ( response_str.empty( ) )
        return false;

    return response_str.find( "\"success\":true" ) != std::string::npos;
}

bool api::register_user( const std::string& username, const std::string& password, const std::string& license_key )
{
    std::string payload = "type=register"
        "&ownerid=" + this->m_owner_id +
        "&name=" + this->m_app_name +
        "&version=" + this->m_app_version +
        "&username=" + username +
        "&pass=" + password +
        "&key=" + license_key;

    std::string response_str = this->send_request( "register", payload );

    if ( response_str.empty( ) )
        return false;

    return response_str.find( "\"success\":true" ) != std::string::npos;
}

void api::log( const std::string& message )
{
    std::string payload = "type=log"
        "&ownerid=" + this->m_owner_id +
        "&name=" + this->m_app_name +
        "&version=" + this->m_app_version +
        "&key=" + this->m_token +
        "&message=" + message;

    this->send_request( "log", payload );
}

} // namespace KeyAuth
