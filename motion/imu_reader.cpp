#include "motion/imu_reader.hpp"

#include <pthread.h>

#include <cstdio>

#include "motion/imu_stream.hpp"

namespace motion {

ImuReader::ImuReader(air::Imu& imu) : imu_(imu) {
    // Every batch, in order, each one waking it.
    imu_.watch(&wake_);
    thread_ = std::thread(&ImuReader::run, this);
}

ImuReader::~ImuReader() {
    stop_.store(true);
    wake_.poke();
    thread_.join();
}

void ImuReader::run() {
    pthread_setname_np(pthread_self(), "imu");
    air::ImuBatch b;
    uint64_t dropped = 0;
    while (!stop_.load()) {
        wake_.wait(500000);
        while (imu_.take(b)) ImuStream::get().add(b);
        const uint64_t d = imu_.dropped();
        if (d != dropped) {
            printf("imu: %llu batches came faster than they were taken (%llu so far)\n",
                   (unsigned long long)(d - dropped), (unsigned long long)d);
            dropped = d;
        }
    }
}

}  // namespace motion
