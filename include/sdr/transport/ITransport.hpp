#pragma once
#include <cstdint>
#include <sys/types.h>

namespace sdr {

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual ssize_t read (uint8_t* buf, size_t maxlen) = 0;
    virtual ssize_t write(const uint8_t* buf, size_t len) = 0;
    virtual int     fd()   const = 0;   // for select()/poll()
    virtual bool    valid()const = 0;
};

} // namespace sdr
