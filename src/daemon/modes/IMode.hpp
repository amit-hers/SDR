#pragma once
#include "sdr/stats/LinkStats.hpp"

namespace sdr {

class IMode {
public:
    virtual void  start()           = 0;
    virtual void  stop()            = 0;
    virtual bool  running()  const  = 0;
    virtual const LinkStats& stats() const = 0;
    virtual ~IMode() = default;
};

} // namespace sdr
