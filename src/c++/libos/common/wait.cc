// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <dmtr/wait.h>

#include <boost/chrono.hpp>
#include <cerrno>
#include <mutex>
#include <unordered_map>
#include <pthread.h>

#include <dmtr/annot.h>
#include <dmtr/fail.h>
#include <dmtr/latency.h>
#include <dmtr/time.hh>
#include <dmtr/libos.h>

#define WAIT_MAX_ITER 10000

#define DMTR_PROFILE 1

#if DMTR_PROFILE
static std::unordered_map<pthread_t, latency_ptr_type> success_poll_latencies;
std::mutex poll_latencies_mutex;

thread_local latency_ptr_type *thread_poll_latencies;
#endif

int dmtr_wait(dmtr_qresult_t *qr_out, dmtr_qtoken_t qt) {
    int ret = EAGAIN;
    uint16_t iter = 0;
    while (EAGAIN == ret) {
        if (iter++ == WAIT_MAX_ITER) {
            return EAGAIN;
        }
        ret = dmtr_poll(qr_out, qt);
    }
    DMTR_OK(dmtr_drop(qt));
    return ret;
}

int dmtr_wait_any(dmtr_qresult_t *qr_out, int *start_offset, int *ready_offset, dmtr_qtoken_t qts[], int num_qts) {
    uint16_t iter = 0;
    while (1) {
        for (int i = start_offset? *start_offset : 0; i < num_qts; i++) {
#if DMTR_PROFILE
            auto t0 = take_time();
#endif
            int ret = dmtr_poll(qr_out, qts[i]);
            if (ret != EAGAIN) {
                if (ret == 0 || ret == ECONNABORTED) {
                    DMTR_OK(dmtr_drop(qts[i]));
#if DMTR_PROFILE
                    auto now = take_time();
                    auto dt = now - t0;
                    if (!thread_poll_latencies) {
                        pthread_t me = pthread_self();
                        std::lock_guard<std::mutex> lock(poll_latencies_mutex);
                        auto it = success_poll_latencies.find(me);
                        if (it == success_poll_latencies.end()) {
                            DMTR_OK(dmtr_register_latencies("poll", success_poll_latencies));
                            it = success_poll_latencies.find(me);
                        }
                        thread_poll_latencies = &it->second;
                    }
                    DMTR_OK(dmtr_record_timed_latency(thread_poll_latencies->get(), since_epoch(now), dt.count()));
#endif
                    if (ready_offset != NULL) {
                        *ready_offset = i;
                    }
                    if (start_offset != NULL && *start_offset != 0) {
                        *start_offset = 0;
                    }
                    return ret;
                }
            } else {
                if (iter++ == WAIT_MAX_ITER) {
                    if (start_offset != NULL) {
                        *start_offset = i;
                    }
                    return EAGAIN;
                }
            }
        }
    }

    DMTR_UNREACHABLE();
}
