#include <chrono>

#include <cstdlib>
#include <json/json.h>

#define LDAP_DEPRECATED 1
#include <ldap.h>

#include <errno.h>

#include "util.h"
#include "logic.h"

using namespace std;

Logic::Logic(const chrono::seconds tokenTimeout,
             const string &ldapUri,
             const string &bindDN,
             const string &webPrefix,
             const string &serDev) :
    _logger(Logger::get()),
    _door(serDev),
    _tokenTimeout(tokenTimeout),
    _ldapUri(ldapUri),
    _bindDN(bindDN),
    _webPrefix(webPrefix)
{
    srand(time(NULL));
    _createNewToken(false);

    _tokenUpdater = thread([this] () {
        while (_run)
        {
            unique_lock<mutex> l(_mutex);
            _tokenCondition.wait_for(l, _tokenTimeout);
            if (_run == false)
            {
                break;
            } else {
                _createNewToken(true);
            }
        }
    });
}

Logic::~Logic()
{
    _run = false;
    _tokenCondition.notify_one();
    _tokenUpdater.join();
}

Logic::Response Logic::parseRequest(const string &str)
{
    unique_lock<mutex> l(_mutex);

    _logger(LogLevel::info, "Incoming request...");
    Json::Reader reader;
    Json::Value root;
    Response retval = Fail;
    string action, user, password, ip, token;

    bool suc = reader.parse(str, root, false);
    if (!suc)
    {
        _logger(LogLevel::warning, "Request ist not valid JSON!");
        retval = NotJson;
        goto out;
    }

    try {
        action = getJsonOrFail<string>(root, "action");
        ip = getJsonOrFail<string>(root, "ip");
        user = getJsonOrFail<string>(root, "user");
        password = getJsonOrFail<string>(root, "password");
        token = getJsonOrFail<string>(root, "token");
    }
    catch (...)
    {
        _logger(LogLevel::warning, "Error parsing JSON");
        retval = JsonError;
        goto out;
    }

    _logger("  Action: " + action, LogLevel::notice);
    _logger("  User  : " + user, LogLevel::notice);
    _logger("  IP    : " + ip, LogLevel::notice);
    _logger("  Token : " + token, LogLevel::notice);

    if (_checkToken(token) == false)
    {
        _logger(LogLevel::error, "User provided invalid token");
        retval = InvalidToken;
        goto out;
    }

    retval = _checkLDAP(user,password);
    if (retval != Success)
    {
        _logger(LogLevel::error, "Ldap error");
        goto out;
    }

    if (action == "lock")
    {
        retval = _lock();
    } else if (action == "unlock") {
        retval = _unlock();
    } else {
        _logger(LogLevel::error, "Unknown Action: %s", action.c_str());
        retval = UnknownAction;
    }

out:
    return retval;
}

Logic::Response Logic::_lock()
{
    if (_door.state() == Door::State::Locked)
    {
        _logger(LogLevel::warning, "Unable to lock: already closed");
        return AlreadyLocked;
    }
 
    _door.lock();
    _createNewToken(false);

    return Success;
}

Logic::Response Logic::_unlock()
{
    const auto oldState = _door.state();
    _door.unlock();
    _createNewToken(false);

    if (oldState == Door::State::Unlocked)
    {
        _logger(LogLevel::warning, "Unable to unlock: already unlocked");
        return AlreadyUnlocked;
    }

    return Success;
}

bool Logic::_checkToken(const string &strToken)
{
    try {
        uint64_t token = toUint64(strToken);
        if (token == _curToken || (_prevValid == true && token == _prevToken))
        {
            _logger(LogLevel::info, "Token check successful");
            return true;
        }
    }
    catch (const char* const &ex)
    {
        _logger(LogLevel::error, "Check Token failed for token \"%s\" (expected %s): %s", strToken.c_str(), toHexString(_curToken).c_str(), ex);
    }
    return false;
}

Logic::Response Logic::_checkLDAP(const string &user, const string &password)
{
    constexpr int BUFFERSIZE = 1024;
    char buffer[BUFFERSIZE];
    Response retval = Fail;
    int rc = -1;
    LDAP* ld = nullptr;
    unsigned long version = LDAP_VERSION3;

    _logger(LogLevel::notice, "Trying to authenticate as user \"%s\"", user.c_str());
    snprintf(buffer, BUFFERSIZE, _bindDN.c_str(), user.c_str());

    rc = ldap_initialize(&ld, _ldapUri.c_str());
    if(rc != LDAP_SUCCESS)
    {   
        _logger(LogLevel::error, "LDAP initialize error: %s", ldap_err2string(rc));
        retval = LDAPInit;
        goto out2;
    }

    rc = ldap_set_option(ld,
                         LDAP_OPT_PROTOCOL_VERSION,
                         (void*)&version);
    if (rc != LDAP_SUCCESS)
    {
        _logger(LogLevel::error, "LDAP set version failed");
        retval = LDAPInit;
        goto out;
    }

    rc = ldap_simple_bind_s(ld, buffer, password.c_str());
    if (rc != LDAP_SUCCESS)
    {   
        _logger(LogLevel::error, "Credential check for user \"%s\" failed: %s", user.c_str(), ldap_err2string(rc));
        retval = InvalidCredentials;
        goto out;
    }

    _logger(LogLevel::notice, "user \"%s\" successfully authenticated", user.c_str());
    retval = Success;

out:
    ldap_unbind(ld);
    ld = nullptr;
out2:
    return retval;
}

void Logic::_createNewToken(const bool stillValid)
{
    // Todo Mutex einführen

    _prevToken = _curToken;
    _prevValid = stillValid;

    _curToken = (((uint64_t)rand())<<32) | ((uint64_t)rand());

    // TODO make things more pretty
    const string uri = _webPrefix + toHexString(_curToken);

    const int ARRAY_SIZE=1024;
    char buffer[ARRAY_SIZE];
    snprintf(buffer, ARRAY_SIZE,
             "qrencode -l M -d 100 -s 5 \"%s\" -t png -o /tmp/qr.png", uri.c_str());
    system(buffer);


    ostringstream message;
    message << "New Token generated: " << toHexString(_curToken) << " old Token: " << toHexString(_prevToken) << " is " << (_prevValid?"still":"not") << " valid";
    _logger(message, LogLevel::info);
}
