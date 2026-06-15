#pragma once
#include <stdexcept>
#include <string>

/**
 * INTERVIEW TIP: Senior engineers use custom exception hierarchies.
 * It allows for precise error catching (e.g., catching SocketException 
 * separately from ProtocolException) and keeps the code clean.
 */
class TelecomException : public std::runtime_error {
public:
    explicit TelecomException(const std::string& msg) : std::runtime_error(msg) {}
};

class SocketException : public TelecomException {
public:
    explicit SocketException(const std::string& msg) : TelecomException("SOCKET_ERR: " + msg) {}
};

class ProtocolException : public TelecomException {
public:
    explicit ProtocolException(const std::string& msg) : TelecomException("PROTOCOL_ERR: " + msg) {}
};

class AuthException : public TelecomException {
public:
    explicit AuthException(const std::string& msg) : TelecomException("AUTH_FAILURE: " + msg) {}
};