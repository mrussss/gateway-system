#include "TestHarness.hpp"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "concurrent/BlockQueue.hpp"

int main()
{
    return runTests({
        {"fifo push and pop", []
         {
             BlockQueue<int> queue(3);
             CHECK(queue.push(1) == PushResult::OK);
             CHECK(queue.push(2) == PushResult::OK);
             int value = 0;
             CHECK(queue.pop(value));
             CHECK_EQ(value, 1);
             CHECK(queue.tryPop(value));
             CHECK_EQ(value, 2);
         }},
        {"full and stopped results are explicit", []
         {
             BlockQueue<int> queue(1);
             CHECK(queue.push(1) == PushResult::OK);
             CHECK(queue.push(2) == PushResult::FULL);
             queue.stop();
             CHECK(queue.push(3) == PushResult::STOPPED);
         }},
        {"stop drains queued elements", []
         {
             BlockQueue<int> queue(3);
             CHECK(queue.push(1) == PushResult::OK);
             CHECK(queue.push(2) == PushResult::OK);
             queue.stop();
             int value = 0;
             CHECK(queue.pop(value));
             CHECK_EQ(value, 1);
             CHECK(queue.pop(value));
             CHECK_EQ(value, 2);
             CHECK(!queue.pop(value));
         }},
        {"abort discards queued elements", []
         {
             BlockQueue<int> queue(3);
             CHECK(queue.push(1) == PushResult::OK);
             CHECK(queue.push(2) == PushResult::OK);
             CHECK_EQ(queue.abort(), size_t{2});
             int value = 0;
             CHECK(!queue.pop(value));
             CHECK(queue.push(3) == PushResult::STOPPED);
         }},
        {"multiple producers and consumers", []
         {
             constexpr int producer_count = 4;
             constexpr int consumer_count = 4;
             constexpr int items_per_producer = 1000;
             BlockQueue<int> queue(producer_count * items_per_producer);
             std::atomic<int> consumed{0};
             std::vector<std::thread> consumers;
             for (int i = 0; i < consumer_count; ++i)
             {
                 consumers.emplace_back([&]
                 {
                     int value = 0;
                     while (queue.pop(value))
                     {
                         (void)value;
                         consumed.fetch_add(1);
                     }
                 });
             }

             std::vector<std::thread> producers;
             for (int i = 0; i < producer_count; ++i)
             {
                 producers.emplace_back([&, i]
                 {
                     for (int n = 0; n < items_per_producer; ++n)
                     {
                         CHECK(queue.push(i * items_per_producer + n) == PushResult::OK);
                     }
                 });
             }
             for (auto &producer : producers)
             {
                 producer.join();
             }
             queue.stop();
             for (auto &consumer : consumers)
             {
                 consumer.join();
             }
             CHECK_EQ(consumed.load(), producer_count * items_per_producer);
             CHECK(queue.peakSize() <= queue.capacity());
         }},
    });
}
