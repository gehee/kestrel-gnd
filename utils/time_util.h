//
// Created by gaeta on 2024-06-20.
//

#ifndef FPVUE_TIME_UTIL_H
#define FPVUE_TIME_UTIL_H


#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>


inline uint64_t get_real_time_ms(void) // in milliseconds
{
    struct timespec ts;
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    if (rc < 0) {
        fprintf(stderr, "Error getting time: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

inline uint64_t get_real_time_us(void) // in microseconds
{
    struct timespec ts;
    int rc = clock_gettime(CLOCK_REALTIME, &ts);
    if (rc < 0) {
        fprintf(stderr, "Error getting time: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    uint64_t r = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    return r;
}

inline uint64_t get_time_ms(void) // in milliseconds
{
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (rc < 0) {
        fprintf(stderr, "Error getting time: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

inline uint64_t get_time_us(void) // in microseconds
{
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    if (rc < 0) {
        fprintf(stderr, "Error getting time: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    uint64_t r = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    return r;
}

#endif //FPVUE_TIME_UTIL_H