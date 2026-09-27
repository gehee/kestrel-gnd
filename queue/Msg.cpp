#include "Msg.hpp"

#include <atomic>


namespace {

MsgUID generateUniqueId()
{
    static std::atomic<MsgUID> i(0);
    return ++i;
}

}

Msg::Msg(uint64_t msgId)
  : msgId_(msgId), uniqueId_(generateUniqueId())
{
}

std::unique_ptr<Msg> Msg::move()
{
    return std::unique_ptr<Msg>(new Msg(std::move(*this)));
}

uint64_t Msg::getMsgId() const
{
    return msgId_;
}

MsgUID Msg::getUniqueId() const
{
    return uniqueId_;
}

