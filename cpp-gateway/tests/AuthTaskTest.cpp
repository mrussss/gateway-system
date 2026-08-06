#include "TestHarness.hpp"

#include <memory>

#include "business/AuthTask.hpp"

namespace
{
Request request(MessageType type)
{
    Request value{};
    value.fd = 7;
    value.conn_id = 9;
    value.version = 1;
    value.type = type;
    value.request_id = 11;
    return value;
}
} // namespace

int main()
{
    return runTests({
        {"factory accepts only AUTH with a cancellation token", []
         {
             auto cancellation = std::make_shared<AuthCancellation>();
             auto auth = makeAuthTask(request(MessageType::AUTH), cancellation);
             CHECK(auth.has_value());
             CHECK(auth->request.type == MessageType::AUTH);
             CHECK(auth->cancellation == cancellation);

             CHECK(!makeAuthTask(request(MessageType::ECHO), cancellation).has_value());
             CHECK(!makeAuthTask(request(MessageType::AUTH), nullptr).has_value());
         }},
        {"cancellation is shared across Reactor and task", []
         {
             auto cancellation = std::make_shared<AuthCancellation>();
             auto auth = makeAuthTask(request(MessageType::AUTH), cancellation);
             cancellation->cancelled.store(true, std::memory_order_relaxed);
             CHECK(auth->cancellation->cancelled.load(std::memory_order_relaxed));
         }},
    });
}
