#include <boost/chrono.hpp>
#include <boost/optional.hpp>

#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include <unordered_map>
#include <cassert>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <queue>
#include <functional>
#include <thread>

#include "app.hh"
#include "common.hh"
#include "request_parser.h"
#include "httpops.hh"
#include <dmtr/types.h>
#include <dmtr/annot.h>
#include <dmtr/latency.h>
#include <dmtr/libos.h>
#include <dmtr/wait.h>
#include <dmtr/libos/mem.h>
#include <dmtr/libos/io_queue.hh>
#include <dmtr/libos/persephone.hh>

#define MAX_CLIENTS 64

int g_argc = 0;
char **g_argv = nullptr;

uint16_t cpu_offset = 4;

bool no_op;
uint32_t no_op_time;

std::string label, log_dir;
std::string uris_list;
std::unordered_map<std::string, std::vector<char>> uri_store;

Psp psp;

void sig_handler(int signo) {
    printf("Entering signal handler\n");
    for (auto &w: psp.workers) {
        log_info("Scheduling worker %d to terminate", w->whoami);
        w->terminate = true;
    }
    printf("Exiting signal handler\n");
}

void read_from_file(char *filepath, char *body, int *code, char *mime_type) {
    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        fprintf(stdout, "Failed to access requested file %s: %s\n", filepath, strerror(errno));
        strncpy(mime_type, "text/html", MAX_MIME_TYPE);
    } else {
        // Get file size
        fseek(file, 0, SEEK_END);
        int size = ftell(file);
        if (size == -1) {
            printf("could not ftell the file :%s\n", strerror(errno));
        }
        fseek(file, 0, SEEK_SET);
        /*
        body = reinterpret_cast<char *>(malloc(size+1));
        body_len = fread(body, sizeof(char), size, file);
        body[body_len] = '\0';
        */
        body = static_cast<char *>(malloc(size));
        size_t char_read = fread(body, sizeof(char), size, file);
        if (char_read < (unsigned) size) {
            printf("fread() read less bytes than file's size\n");
        }
        if (strstr(body, "somedummystring") != NULL) {
            printf("dummy string comparison worked\n");
        }
        free(body);
        /*
        if (body_len != size) {
            fprintf(stdout, "Only read %d of %u bytes from file %s\n", body_len, size, filepath);
        }
        */
        fclose(file);
        path_to_mime_type(filepath, mime_type, MAX_MIME_TYPE);
        *code = 200;
        //fprintf(stdout, "Found file: %s\n", filepath);
    }
}

static void file_work(char *url, char **response, int *response_len, uint32_t req_id) {
    char filepath[MAX_FILEPATH_LEN];
    url_to_path(url, FILE_DIR, filepath, MAX_FILEPATH_LEN);
    struct stat st;
    int status = stat(filepath, &st);

    char *body = NULL;
    int body_len = 0;
    int code = 404;
    char mime_type[MAX_MIME_TYPE];
    if (status != 0 || S_ISDIR(st.st_mode)) {
        if (status != 0) {
            fprintf(stderr, "Failed to get status of requested file %s\n", filepath);
        } else {
            fprintf(stderr, "Directory requested (%s). Returning 404.\n", filepath);
        }
        strncpy(mime_type, "text/html", MAX_MIME_TYPE);
    } else {
        if (uri_store.empty()) {
                read_from_file(filepath, body, &code, mime_type);
        } else {
            auto it = uri_store.find(std::string(filepath));
            if (it == uri_store.end()) {
                log_error("Requested non registered file (%s)", filepath);
            } else {
                //size_t size = it->second.size();
                //std::cout << "found file: " << it->first << "(" << size << ")" << std::endl;
                /* We copy bytes here to simulate transfering them from memory to client */
                std::vector<char> data(it->second);
                /*
                body = static_cast<char *>(malloc(size));
                memcpy(body, it->second.data(), it->second.size());
                if (strstr(body, "somedummystring") != NULL) {
                    printf("dummy string comparison worked\n");
                }
                free(body);
                */
            }
        }
    }
    //FIXME: maybe it's time to move on to giving the full answer now that segmentation is fixed...
    body = NULL;
    body_len = 0;

    char *header = NULL;
    int header_len = generate_header(&header, code, body_len, mime_type);
    generate_response(response, header, body, header_len, body_len, response_len, req_id);
}

static void regex_work(char *url, char **response, int *response_len, uint32_t req_id) {
    char *body = NULL;
    int body_len = 0;
    int code = 200;
    char mime_type[MAX_MIME_TYPE];
    char regex_value[MAX_REGEX_VALUE_LEN];
    int rtn = get_regex_value(url, regex_value);
    if (rtn != 0) {
        fprintf(stderr, "Non-regex URL passed to craft_regex_response!\n");
        code = 501;
    } else {
        char html[8192];
        rtn = regex_html(regex_value, html, 8192);
        if (rtn < 0) {
            fprintf(stderr, "Error crafting regex response\n");
            code = 501;
        }
        body_len = strlen(html);
        body = reinterpret_cast<char *>(malloc(body_len+1));
        snprintf(body, body_len+1, "%s", html);
        strncpy(mime_type, "text/html", MAX_MIME_TYPE);
    }

    char *header = NULL;
    int header_len = generate_header(&header, code, body_len, mime_type);
    generate_response(response, header, body, header_len, body_len, response_len, req_id);
}

static void clean_state(struct parser_state *state) {
    if (state->url) {
       free(state->url);
       state->url = NULL;
    }
    if (state->body) {
        free(state->body);
        state->body = NULL;
    }
}

static inline void no_op_loop(uint32_t iter) {
    volatile uint32_t j = 1;
    for (uint32_t i = j; i < iter+1; ++i) {
        j = j+i;
    }
}

int http_work(struct parser_state *state, dmtr_qresult_t &wait_out,
              dmtr_qtoken_t &token, int out_qfd, std::shared_ptr<Worker> me) {
#ifdef LEGACY_PROFILING
    hr_clock::time_point start;
    hr_clock::time_point end;
    if (me->type == HTTP) {
        start = take_time();
    }
#endif
    std::unique_ptr<Request> req(reinterpret_cast<Request *>(
        wait_out.qr_value.sga.sga_segs[1].sgaseg_buf
    ));
#ifdef DMTR_TRACE
    req->start_http = take_time();
#endif

    /* If we are in no_op mode, just send back the request, as an echo server */
    if (no_op) {
        if (no_op_time > 0) {
            no_op_loop(no_op_time);
        }
#ifdef LEGACY_PROFILING
        if (me->type == HTTP) {
            /* Record http work */
            end = take_time();
            me->runtimes.push_back(
                std::pair<uint64_t, uint64_t>(since_epoch(start), ns_diff(start, end))
            );
        }
#endif
#ifdef DMTR_TRACE
        req->end_http = take_time();
#endif
        /* Strip Request from sga if needed */
        if (me->type == NET) {
            wait_out.qr_value.sga.sga_numsegs = 1;
#ifdef DMTR_TRACE
            me->req_states.push_back(std::move(req));
#endif
        }
        DMTR_OK(me->psp_su->ioqapi.push(token, out_qfd, wait_out.qr_value.sga));
        int status;
        while ((status = me->psp_su->wait(NULL, token)) == EAGAIN) {
            if (me->terminate) {
                break;
            }
        }
        DMTR_TRUE(EINVAL, status == 0);

        if (me->type == NET) {
            //free(wait_out.qr_value.sga.sga_buf);
            dmtr_free_mbuf(&wait_out.qr_value.sga);
        }
        return 0;
    }

    /* Do the HTTP work */
    /* First cast the buffer and get request id. Then increment ptr to the start of the payload */
    uint32_t * const req_id =
        reinterpret_cast<uint32_t *>(wait_out.qr_value.sga.sga_segs[0].sgaseg_buf);
    req->id = *req_id;
    char *req_c = reinterpret_cast<char *>(wait_out.qr_value.sga.sga_segs[0].sgaseg_buf);
    req_c += sizeof(uint32_t);
    log_debug("HTTP worker popped req %d: %s", req->id, req_c);
    size_t req_size = wait_out.qr_value.sga.sga_segs[0].sgaseg_len - sizeof(uint32_t);

    dmtr_sgarray_t resp_sga;
    init_parser_state(state);
    enum parser_status pstatus = parse_http(state, req_c, req_size);
    switch (pstatus) {
        case REQ_COMPLETE:
            //fprintf(stdout, "HTTP worker got complete request\n");
            break;
        case REQ_INCOMPLETE:
        case REQ_ERROR:
            log_warn("HTTP worker got incomplete or malformed request: %.*s",
                    (int) req_size, req_c);
            clean_state(state);
            //free(wait_out.qr_value.sga.sga_buf);
            //wait_out.qr_value.sga.sga_buf = NULL;
            dmtr_free_mbuf(&wait_out.qr_value.sga);

            resp_sga.sga_segs[0].sgaseg_buf =
                malloc(strlen(BAD_REQUEST_HEADER) + 1 + sizeof(uint32_t));
            memcpy(resp_sga.sga_segs[0].sgaseg_buf, (uint32_t *) &req->id, sizeof(uint32_t));
            resp_sga.sga_segs[0].sgaseg_len =
                snprintf(
                    reinterpret_cast<char *>(resp_sga.sga_segs[0].sgaseg_buf) + sizeof(uint32_t),
                    strlen(BAD_REQUEST_HEADER) + 1, "%s", BAD_REQUEST_HEADER
                );
            resp_sga.sga_segs[0].sgaseg_len += sizeof(uint32_t);
#ifdef DMTR_TRACE
            req->end_http = take_time();
#endif
            if (me->type == HTTP) {
                resp_sga.sga_numsegs = 2;
                resp_sga.sga_segs[1].sgaseg_len = sizeof(req.get());
                resp_sga.sga_segs[1].sgaseg_buf = reinterpret_cast<void *>(req.release());
            } else {
                resp_sga.sga_numsegs = 1;
            }

            DMTR_OK(me->psp_su->ioqapi.push(token, out_qfd, resp_sga));
            while (me->psp_su->wait(NULL, token) == EAGAIN) {
                if (me->terminate) {
                    break;
                }
            }

            if (me->type == NET) {
#ifdef DMTR_TRACE
                me->req_states.push_back(std::move(req));
#endif
                free(resp_sga.sga_segs[0].sgaseg_buf);
            }
            return -1;
    }

    char *response = NULL;
    int response_size;
    switch(get_request_type(state->url)) {
        case REGEX_REQ:
            regex_work(state->url, &response, &response_size, req->id);
            break;
        case FILE_REQ:
            file_work(state->url, &response, &response_size, req->id);
            break;
    }

    if (response == NULL) {
        log_error("Error formatting HTTP response");
        clean_state(state);
        return -1;
    }

    /**
     * Free the sga, prepare a new one:
     * we should not reuse it because it was sized for the request
     */
    clean_state(state);
    //free(wait_out.qr_value.sga.sga_buf);
    //wait_out.qr_value.sga.sga_buf = NULL;
    dmtr_free_mbuf(&wait_out.qr_value.sga);

    resp_sga.sga_segs[0].sgaseg_len = response_size;
    resp_sga.sga_segs[0].sgaseg_buf = response;

#ifdef LEGACY_PROFILING
    if (me->type == HTTP) {
        /* Record http work */
        hr_clock::time_point end = take_time();
        me->runtimes.push_back(
            std::pair<uint64_t, uint64_t>(since_epoch(start), ns_diff(start, end))
        );
    }
#endif

#ifdef DMTR_TRACE
    req->end_http = take_time();
#endif
    /* release ReqState last */
    if (me->type == HTTP) {
        resp_sga.sga_numsegs = 2;
        resp_sga.sga_segs[1].sgaseg_len = sizeof(req.get());
        resp_sga.sga_segs[1].sgaseg_buf = reinterpret_cast<void *>(req.release());
    } else {
        resp_sga.sga_numsegs = 1;
    }

    DMTR_OK(me->psp_su->ioqapi.push(token, out_qfd, resp_sga));
    while (me->psp_su->wait(NULL, token) == EAGAIN) {
        if (me->terminate) {
            break;
        }
    }

    /* If we are called as part of a HTTP worker, the next component (network) will need
     * the buffer for sending on the wire. Otherwise -- if called as part of NET
     * work -- we need to free now.*/
    if (me->type == NET) {
#ifdef DMTR_TRACE
        me->req_states.push_back(std::move(req));
#endif
        free(response);
    }

    return 0;
}

static void *http_worker(void *args) {
    std::shared_ptr<Worker> me = *static_cast<std::shared_ptr<Worker> *>(args);
    //FIXME: the variables are not print, even if they are already set. OOO?
    printf("Hello I am HTTP worker %d running on core %d\n",
           me->whoami, me->core_id
    );

    struct parser_state *state =
        (struct parser_state *) malloc(sizeof(*state));
    state->url = NULL;
    state->body = NULL;

    dmtr_qtoken_t token;
    if (me->psp_su->ioqapi.pop(token, me->shared_qfd)) {
        log_info("POP FAILED");
    }
    while (!me->terminate) {
        dmtr_qresult_t wait_out;
        int status = me->psp_su->wait(&wait_out, token);
        if (status == 0) {
            http_work(state, wait_out, token, me->shared_qfd, me);
            if (me->psp_su->ioqapi.pop(token, me->shared_qfd)) {
                log_info("POP FAILED");
            }
        } else {
            log_debug("dmtr_wait returned status %d", status);
        }
    }
    log_info("HTTP worker %d set to terminate", me->whoami);

    log_debug("Cleaning HTTP worker %d", me->whoami);
    clean_state(state);
    free(state);
    me->psp_su->ioqapi.close(me->shared_qfd);
    delete static_cast<std::shared_ptr<Worker>*>(args);
    log_debug("Exiting HTTP worker %d", me->whoami);
    pthread_exit(NULL);
}

void check_availability(enum req_type type, std::vector<std::shared_ptr<Worker> > &workers) {
    if (workers.empty()) {
        log_error(
            "%d request given to HTTP_REQ_TYPE dispatch policy,"
            " but no such workers are to be found",
            type
        );
        exit(1);
    }
}

int net_work(std::vector<int> &http_q_pending, std::vector<bool> &clients_in_waiting,
             dmtr_qresult_t &wait_out,
             std::vector<dmtr_qtoken_t> &tokens, dmtr_qtoken_t &token,
             struct parser_state *state, std::shared_ptr<Worker> me, int lqd) {
#if defined DMTR_TRACE || defined LEGACY_PROFILING
    hr_clock::time_point start = take_time();
#endif
    if (wait_out.qr_qd == lqd) {
        assert(DMTR_OPC_ACCEPT == wait_out.qr_opcode);
        /* Enable reading on the accepted socket */
        DMTR_OK(me->psp_su->ioqapi.pop(token, wait_out.qr_value.ares.qd));
        tokens.push_back(token);
        /* Re-enable accepting on the listening socket */
        DMTR_OK(me->psp_su->ioqapi.accept(token, lqd));
        tokens.push_back(token);
        log_debug("Accepted a new connection (%d) on %d", wait_out.qr_value.ares.qd, lqd);
    } else {
        assert(DMTR_OPC_POP == wait_out.qr_opcode);

        auto it = std::find(
            http_q_pending.begin(),
            http_q_pending.end(),
            wait_out.qr_qd
        );
        if (it == http_q_pending.end()) {
            assert(wait_out.qr_value.sga.sga_numsegs == 1);
            log_debug(
                "received new request on queue %d: %s\n",
                wait_out.qr_qd,
                reinterpret_cast<char *>(
                    wait_out.qr_value.sga.sga_segs[0].sgaseg_buf) + sizeof(uint32_t
                )
            );
            /*
            uint32_t * const ridp =
                reinterpret_cast<uint32_t *>(wait_out.qr_value.sga.sga_segs[0].sgaseg_buf);
            printf("POPED %d/%lu", *ridp, token);
            */
            /* This is a new request */
            std::unique_ptr<Request> req(new Request(wait_out.qr_qd));
            req->net_qd = wait_out.qr_qd;
#ifdef DMTR_TRACE
            req->net_receive = start;
            req->pop_token = token;
#endif
            me->num_rcvd++;
            /*
            if (me->num_rcvd % 1000 == 0) {
                log_info("received: %d requests\n", me->num_rcvd);
            }
            */
            if (me->args.split) {
                /* Load balance incoming requests among HTTP workers */
                std::shared_ptr<Worker> dest_worker;
                if (me->args.dispatch_p == RR) {
                    dest_worker = psp.http_workers[me->num_rcvd % psp.http_workers.size()];
                } else if (me->args.dispatch_p == HTTP_REQ_TYPE) {
                    std::string req_str(reinterpret_cast<const char *>(
                        wait_out.qr_value.sga.sga_segs[0].sgaseg_buf
                    ) +  sizeof(uint32_t));
                    enum req_type type = me->args.dispatch_f(req_str);
                    size_t index;
                    switch (type) {
                        default:
                        case ALL:
                        case UNKNOWN:
                            //TODO handle failure
                            log_error("Unknown request type");
                            break;
                        case REGEX:
                            check_availability(type, psp.regex_workers);
                            index = me->type_counts[REGEX] % psp.regex_workers.size();
                            dest_worker = psp.regex_workers[index];
                            me->type_counts[REGEX]++;
                            break;
                        case PAGE:
                            check_availability(type, psp.page_workers);
                            index = me->type_counts[PAGE] % psp.page_workers.size();
                            dest_worker = psp.page_workers[index];
                            me->type_counts[PAGE]++;
                            break;
                        case POPULAR_PAGE:
                            check_availability(type, psp.popular_page_workers);
                            index = me->type_counts[POPULAR_PAGE] % psp.popular_page_workers.size();
                            dest_worker = psp.popular_page_workers[index];
                            me->type_counts[POPULAR_PAGE]++;
                            break;
                        case UNPOPULAR_PAGE:
                            check_availability(type, psp.unpopular_page_workers);
                            index = me->type_counts[UNPOPULAR_PAGE] % psp.unpopular_page_workers.size();
                            dest_worker = psp.unpopular_page_workers[index];
                            me->type_counts[UNPOPULAR_PAGE]++;
                            break;
                    }
                } else if (me->args.dispatch_p == ONE_TO_ONE) {
                    dest_worker = psp.workers[me->whoami];
                } else {
                    log_error("Non implemented network dispatch policy, falling back to RR");
                    dest_worker = psp.http_workers[me->num_rcvd % psp.http_workers.size()];
                }
                log_debug("NET worker %d sending request %s to HTTP worker %d\n",
                       me->whoami,
                       reinterpret_cast<char *>(
                           wait_out.qr_value.sga.sga_segs[0].sgaseg_buf
                       ) + sizeof(uint32_t),
                       dest_worker->whoami
                );

                dmtr_sgarray_t req_sga;
                req_sga.sga_numsegs = 2;
                /** First copy the original SGA */
                req_sga.sga_buf = wait_out.qr_value.sga.sga_buf;
                req_sga.mbuf = wait_out.qr_value.sga.mbuf;
                req_sga.sga_segs[0].sgaseg_buf = wait_out.qr_value.sga.sga_segs[0].sgaseg_buf;
                req_sga.sga_segs[0].sgaseg_len = wait_out.qr_value.sga.sga_segs[0].sgaseg_len;

                int dest_qfd = me->args.shared_qfds[dest_worker->whoami];
                http_q_pending.push_back(dest_qfd);
                clients_in_waiting[wait_out.qr_qd] = true;

                req_sga.sga_segs[1].sgaseg_len = sizeof(req.get());

#ifdef LEGACY_PROFILING
                hr_clock::time_point end = take_time();
                me->runtimes.push_back(
                    std::pair<uint64_t, uint64_t>(since_epoch(start), ns_diff(start, end))
                );
#endif
#ifdef DMTR_TRACE
                req->http_dispatch = take_time();
#endif

                /** Set Request obj last due to release */
                req_sga.sga_segs[1].sgaseg_buf = reinterpret_cast<void *>(req.release());

                log_debug("Scheduling request %d/%lu for send", *ridp, token);
                DMTR_OK(me->psp_su->ioqapi.push(token, dest_qfd, req_sga));
                //FIXME we don't need to wait here.
                int status;
                while ((status = me->psp_su->wait(NULL, token)) == EAGAIN) {
                    if (me->terminate) {
                        return 0;
                    }
                    continue;
                }
                DMTR_TRUE(EINVAL, status == 0);

                /* Enable reading from HTTP result queue */
                DMTR_OK(me->psp_su->ioqapi.pop(token, dest_qfd));
                tokens.push_back(token);
                /* Re-enable NET queue for reading */
                DMTR_OK(me->psp_su->ioqapi.pop(token, wait_out.qr_qd));
                tokens.push_back(token);
            } else {
                /* Append Request */
                wait_out.qr_value.sga.sga_numsegs = 2;
                wait_out.qr_value.sga.sga_segs[1].sgaseg_len = sizeof(req.get());
#ifdef DMTR_TRACE
                req->http_dispatch = take_time();
#endif
                wait_out.qr_value.sga.sga_segs[1].sgaseg_buf = reinterpret_cast<void *>(req.release());
                http_work(state, wait_out, token, wait_out.qr_qd, me);
#ifdef LEGACY_PROFILING
                hr_clock::time_point end = take_time();
                me->runtimes.push_back(
                    std::pair<long int, long int>(since_epoch(start), ns_diff(start, end))
                );
#endif
                /* Re-enable NET queue for reading */
                DMTR_OK(me->psp_su->ioqapi.pop(token, wait_out.qr_qd));
                tokens.push_back(token);
            }
        } else {
            assert(wait_out.qr_value.sga.sga_numsegs == 2);
            std::unique_ptr<Request> req(reinterpret_cast<Request *>(
                wait_out.qr_value.sga.sga_segs[1].sgaseg_buf
            ));
            log_debug(
                "received response for request %d from HTTP queue %d: %s\n",
                req->id,
                wait_out.qr_qd,
                reinterpret_cast<char *>(wait_out.qr_value.sga.sga_segs[0].sgaseg_buf)
            );
#ifdef DMTR_TRACE
            req->http_done = start;
#endif

            /** The client should still be "in the wait".
             * (Likely counter example: the connection was closed by the client)
             */
            if (clients_in_waiting[req->net_qd] == false) {
                /* Otherwise ignore this message and fetch the next one */
                DMTR_OK(me->psp_su->ioqapi.pop(token, wait_out.qr_qd));
                tokens.push_back(token);
                log_warn("Dropping obsolete message aimed towards closed connection");
                free(wait_out.qr_value.sga.sga_segs[0].sgaseg_buf);
                wait_out.qr_value.sga.sga_segs[0].sgaseg_buf = NULL;
#ifdef DMTR_TRACE
                req->net_send = take_time();
                req->push_token = -1;
                me->req_states.push_back(std::move(req));
#endif
                return 0;
            }
            http_q_pending.erase(it);

            if (no_op) {
                if (no_op_time > 0) {
                    no_op_loop(no_op_time);
                }
            }

#ifdef LEGACY_PROFILING
            hr_clock::time_point end = take_time();
            me->runtimes.push_back(
                std::pair<uint64_t, uint64_t>(since_epoch(start), ns_diff(start, end))
            );
#endif
            /* Mask Request from sga */
            wait_out.qr_value.sga.sga_numsegs = 1;
            /* Answer the client */
#ifdef DMTR_TRACE
            req->net_send = take_time();
#endif
            DMTR_OK(me->psp_su->ioqapi.push(token, req->net_qd, wait_out.qr_value.sga));
            log_debug("Scheduled PUSH %d/%lu\n", req->id, token);

            //FIXME we don't have to wait.
            // We can free when dmtr_wait returns that the push was done.
            while (me->psp_su->wait(NULL, token) == EAGAIN) {
                if (me->terminate) {
                    return 0;
                }
            }
            log_debug("Answered the client on queue %d", req->net_qd);
#ifdef DMTR_TRACE
            req->push_token = token;
            me->req_states.push_back(std::move(req));
#endif
            free(wait_out.qr_value.sga.sga_segs[0].sgaseg_buf);
            wait_out.qr_value.sga.sga_segs[0].sgaseg_buf = NULL;
        }
    }
    return 0;
}

static void *net_worker(void *args) {
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        std::cout << "\ncan't ignore SIGPIPE\n";

    auto me = *static_cast<std::shared_ptr<Worker> *>(args);
    printf("Hello I am net worker %d running on core %d\n",
           me->whoami, me->core_id
    );

    /* In case we need to do the HTTP work */
    struct parser_state *state =
        (struct parser_state *) malloc(sizeof(*state));
    state->url = NULL;
    state->body = NULL;

    std::vector<dmtr_qtoken_t> tokens;
    dmtr_qtoken_t token = 0; //temporary token

    /* Create and bind the worker's accept socket */
    int lqd = 0;
    me->psp_su->ioqapi.socket(lqd, AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr = me->args.saddr;
    if (me->psp_su->ioqapi.bind(lqd, reinterpret_cast<struct sockaddr *>(&saddr), sizeof(saddr))) {
        log_error("Binding failed");
    }
    me->psp_su->ioqapi.listen(lqd, 100);
    me->psp_su->ioqapi.accept(token, lqd);
    tokens.push_back(token);
    std::vector<int> http_q_pending;
    http_q_pending.reserve(1024);
    std::vector<bool> clients_in_waiting;
    clients_in_waiting.reserve(MAX_CLIENTS);
    int start_offset = 0;
    while (1) {
        if (me->terminate) {
            log_info("Network worker %d set to terminate", me->whoami);
            break;
        }
        dmtr_qresult_t wait_out;
        int idx;
        int status = me->psp_su->wait_any(&wait_out, &start_offset, &idx, tokens.data(), tokens.size());
        if (status == EAGAIN) {
            continue;
        }
        token = tokens[idx];
        tokens.erase(tokens.begin()+idx);
        if (status == 0) {
#ifdef OP_DEBUG
            update_pql(tokens.size(), &me->pql);
#endif
            net_work(
                http_q_pending, clients_in_waiting, wait_out,
                tokens, token, state, me, lqd
            );
        } else {
            assert(status == ECONNRESET || status == ECONNABORTED);
            if (wait_out.qr_opcode == DMTR_OPC_ACCEPT) {
                log_debug("An accept task failed with connreset or aborted??");
            }
            if (clients_in_waiting[wait_out.qr_qd]) {
                log_debug("Removing closed client connection from answerable list");
                clients_in_waiting[wait_out.qr_qd] = false;
            }
            log_info("closing pseudo connection on %d", wait_out.qr_qd);
            me->psp_su->ioqapi.close(wait_out.qr_qd);
        }
    }

    log_info("Cleaning net worker %d", me->whoami);
    clean_state(state);
    free(state);
    me->psp_su->ioqapi.close(lqd);
    //TODO also close the connected io queues.
    delete static_cast<std::shared_ptr<Worker>*>(args);
    log_debug("Exiting net worker %d", me->whoami);
    if (me->me > 0) {
        pthread_exit(NULL);
    }
    log_debug("... which was running in main thread %d", me->whoami);
    return NULL;
}

void pin_thread(pthread_t thread, u_int16_t cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    int rtn = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (rtn != 0) {
        fprintf(stderr, "could not pin thread: %s\n", strerror(errno));
    }
}

int work_setup(Psp &psp, bool split) {
    /* Aggregate types map */
    psp.types_map[REGEX] = std::pair(&psp.regex_workers, psp.n_regex_workers);
    psp.types_map[PAGE] = std::pair(&psp.page_workers, psp.n_page_workers);
    psp.types_map[POPULAR_PAGE] = std::pair(&psp.popular_page_workers, psp.n_popular_page_workers);
    psp.types_map[UNPOPULAR_PAGE] = std::pair(&psp.unpopular_page_workers, psp.n_unpopular_page_workers);

    /* Create NET worker threads */
    for (int i = 0; i < psp.n_net_workers; ++i) {
        std::shared_ptr<Worker> worker(new Worker());
        worker->whoami = i;
        worker->type = NET;

        worker->args.dispatch_p = psp.net_dispatch_policy;
        worker->args.dispatch_f = psp.net_dispatch_f;

        /* Setup PspServiceUnit */
        worker->psp_su = new PspServiceUnit(
            worker->whoami,
            dmtr::io_queue::category_id::NETWORK_Q,
            g_argc, g_argv
        );

        /* Define which NIC this thread will be using */
        struct sockaddr_in saddr = {};
        saddr.sin_family = AF_INET;
        if (boost::none == server_ip_addr) {
            std::cerr << "Listening on `*:" << port << "`..." << std::endl;
            saddr.sin_addr.s_addr = INADDR_ANY;
        } else {
            /* We increment the base IP (given for worker #1) */
            const char *s = boost::get(server_ip_addr).c_str();
            in_addr_t address = inet_addr(s);
            address = ntohl(address);
            address += i*2;
            address = htonl(address);
            saddr.sin_addr.s_addr = address;
            log_info("NET worker %d set to listen on %s:%d", i, inet_ntoa(saddr.sin_addr), port);
        }
        saddr.sin_port = htons(port);

        worker->args.saddr = saddr; // Pass by copy
        worker->args.split = split;
        worker->core_id = i + cpu_offset;

        psp.workers.push_back(worker);
        psp.net_workers.push_back(worker);
    }

    if (split) {
        //XXX
        std::shared_ptr<Worker> worker = psp.workers.front();
        if (psp.n_http_workers > 0) {
            for (int i = 0; i < psp.n_http_workers; ++i) {
                uint16_t index = psp.n_net_workers + i;
                dmtr::shared_item *producer_si = new dmtr::shared_item();
                dmtr::shared_item *consumer_si = new dmtr::shared_item();
                create_http_worker(psp, true, ALL, index, producer_si, consumer_si);
                int shared_qfd;
                worker->psp_su->ioqapi.shared_queue(shared_qfd, consumer_si, producer_si);
                worker->args.shared_qfds[index] = shared_qfd;
            }
        } else {
            for (auto &it: psp.types_map) {
                for (uint16_t i = 0; i < it.second.second; ++i) {
                    uint16_t index = psp.n_net_workers + psp.http_workers.size();
                    dmtr::shared_item *producer_si = new dmtr::shared_item();
                    dmtr::shared_item *consumer_si = new dmtr::shared_item();
                    it.second.first->push_back(
                        create_http_worker(psp, true, it.first, index, producer_si, consumer_si)
                    );
                    int shared_qfd;
                    worker->psp_su->ioqapi.shared_queue(shared_qfd, consumer_si, producer_si);
                    worker->args.shared_qfds[index] = shared_qfd;
                }
            }
        }
    }

    for (auto &worker: psp.net_workers) {
        if (pthread_create(&worker->me, NULL, &net_worker,
                    static_cast<void *>(new std::shared_ptr<Worker>(worker)))) {
            log_error("pthread_create error: %s", strerror(errno));
        }
        pin_thread(worker->me, worker->core_id);
    }

    return 0;
}

std::shared_ptr<Worker> create_http_worker(Psp &psp, bool typed, enum req_type type, uint16_t index,
                                           dmtr::shared_item *producer_si, dmtr::shared_item *consumer_si) {
    std::shared_ptr<Worker> worker(new Worker());
    worker->type = HTTP;
    worker->me = 0;
    worker->handling_type = type;
    worker->whoami = index;
    // create the shared queue, set the shared item given as parameter
    worker->psp_su =
        new PspServiceUnit(worker->whoami, dmtr::io_queue::category_id::MEMORY_Q, 0, nullptr);
    if (worker->psp_su->ioqapi.shared_queue(worker->shared_qfd, producer_si, consumer_si) != 0) {
        log_error("Could not create shared queue for http worker %d", worker->whoami);
    }

    if (pthread_create(&worker->me, NULL, &http_worker,
                       static_cast<void *>(new std::shared_ptr<Worker>(worker)))) {
        log_error("pthread_create error: %s", strerror(errno));
        exit(1);
    }

    worker->core_id = index + cpu_offset;
    pin_thread(worker->me, worker->core_id);
    psp.http_workers.push_back(worker);
    psp.workers.push_back(worker);

    return worker;
}

std::shared_ptr<Worker> no_pthread_work_setup(Psp &psp) {
    std::shared_ptr<Worker> worker(new Worker());
    worker->whoami = 0;
    worker->args.split = false;
    worker->type = NET;
    worker->me = 0;
    worker->core_id = 0;

    /* Define which NIC this thread will be using */
    struct sockaddr_in saddr = {};
    saddr.sin_family = AF_INET;
    if (boost::none == server_ip_addr) {
        std::cerr << "Listening on `*:" << port << "`..." << std::endl;
        saddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        /* We increment the base IP (given for worker #1) */
        const char *s = boost::get(server_ip_addr).c_str();
        saddr.sin_addr.s_addr = inet_addr(s);
        log_info("NET worker set to listen on %s:%d", inet_ntoa(saddr.sin_addr), port);
    }
    saddr.sin_port = htons(port);
    worker->args.saddr = saddr;
    psp.workers.push_back(worker);

    net_worker(static_cast<void *>(new std::shared_ptr<Worker>(worker)));

    return worker;
}

int main(int argc, char *argv[]) {
    bool no_pthread, no_split;
    bool split = true;
    std::string net_dispatch_pol;
    options_description desc{"HTTP server options"};
    desc.add_options()
        ("label", value<std::string>(&label), "experiment label")
        ("log-dir, L", value<std::string>(&log_dir)->default_value("./"), "experiment log_directory")
        ("regex-workers,w", value<u_int16_t>(&psp.n_regex_workers)->default_value(0), "num REGEX workers")
        ("page-workers,w", value<u_int16_t>(&psp.n_page_workers)->default_value(0), "num PAGE workers")
        ("popular-page-workers,w", value<u_int16_t>(&psp.n_popular_page_workers)->default_value(0), "num POPULAR page workers")
        ("unpopular-page-workers,w", value<u_int16_t>(&psp.n_unpopular_page_workers)->default_value(0), "num UNPOPULAR page workers")
        ("http-workers,w", value<u_int16_t>(&psp.n_http_workers)->default_value(0), "num HTTP workers")
        ("tcp-workers,t", value<u_int16_t>(&psp.n_net_workers)->default_value(1), "num NET workers")
        ("no-op", bool_switch(&no_op), "run no-op workers only")
        ("no-op-time", value<uint32_t>(&no_op_time)->default_value(10000), "tune no-op sleep time")
        ("no-split", bool_switch(&no_split), "do all work in a single component")
        ("no-pthread", bool_switch(&no_pthread), "use pthread or not (main thread will do everything)")
        ("net-dispatch-policy", value<std::string>(&net_dispatch_pol)->default_value("RR"), "dispatch policy used by the network component")
        ("uris-list", value<std::string>(&uris_list)->default_value(""), "URIs to load in memory");
    parse_args(argc, argv, true, desc);
    dmtr_log_directory = log_dir;

    g_argc = argc;
    g_argv = argv;

    if (no_split) {
        log_info(
            "Starting in 'no split' mode. Network workers will handle all the load."
            " Typed workers and dispatch policy are ignored"
        );
        split = false;
    }

    if (no_op) {
        if (no_op_time > 1 && split) {
            no_op_time /= 2;
        }
        log_info("Starting HTTP server in no-op mode (%d iterations per component)", no_op_time);
    }

    /* Some checks about requested dispatch policy and workers */
    uint32_t types_sum =
        psp.n_regex_workers + psp.n_page_workers + psp.n_popular_page_workers + psp.n_unpopular_page_workers;
    if (types_sum > 0) {
        if (net_dispatch_pol == "ONE_TO_ONE" || net_dispatch_pol == "RR") {
            log_error("Typed workers are for HTTP_REQ_TYPE policy only.");
            exit(1);
        }
        if (psp.n_http_workers > 0) {
            log_error("Cannot request psp.n_http_workers with typed workers.");
            exit(1);
        }
        log_info("Instantiating typed workers. psp.n_http_workers will be ignored.");
    } else {
        if (net_dispatch_pol == "HTTP_REQ_TYPE") {
            log_error("Cannot use HTTP_REQ_TYPE policy without specifying a number of typed workers.");
            exit(1);
        } else if (net_dispatch_pol == "ONE_TO_ONE" && psp.n_net_workers < psp.n_http_workers) {
            log_error("Cannot set 1:1 workers mapping with %d net workers and %d http workers.",
                      psp.n_net_workers, psp.n_http_workers);
            exit(1);
        }
    }

    /* Load URIs in a map if list provided */
    if (!uris_list.empty()) {
        std::ifstream urifile(uris_list.c_str());
        if (urifile.bad() || !urifile.is_open()) {
            log_error("Failed to open uri list file");
        }
        std::string uri;
        while (std::getline(urifile, uri)) {
            FILE *file = fopen(uri.c_str(), "rb");
            if (file == NULL) {
                fprintf(stdout, "Failed to open '%s': %s\n", uri.c_str(), strerror(errno));
            } else {
                // Get file size
                fseek(file, 0, SEEK_END);
                int size = ftell(file);
                if (size == -1) {
                    printf("could not ftell the file :%s\n", strerror(errno));
                }
                fseek(file, 0, SEEK_SET);
                std::vector<char> body(size);
                size_t char_read = fread(&body[0], sizeof(char), size, file);
                if (char_read < (unsigned) size) {
                    printf("fread() read less bytes than file's size\n");
                }
                //std::cout << "Read " << char_read << " Bytes " << std::endl;
                uri_store.insert(std::pair<std::string, std::vector<char>>(uri, body));
            }
        }
    }

    /* Set network dispatch policy */
    if (net_dispatch_pol == "RR") {
        psp.net_dispatch_policy = RR;
    } else if (net_dispatch_pol == "HTTP_REQ_TYPE") {
        psp.net_dispatch_policy = HTTP_REQ_TYPE;
        psp.net_dispatch_f = psp_get_req_type;
    } else if (net_dispatch_pol == "ONE_TO_ONE") {
        psp.net_dispatch_policy = ONE_TO_ONE;
    }

    sigset_t mask, oldmask;
    if (!no_pthread) {
        /* Block SIGINT to ensure handler will only be run in main thread */
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);

        int ret = pthread_sigmask(SIG_BLOCK, &mask, &oldmask);
        if (ret != 0) {
            fprintf(stderr, "Couln't block SIGINT and SIGTERM: %s\n", strerror(errno));
        }
    }

    if (signal(SIGINT, sig_handler) == SIG_ERR)
        std::cout << "\ncan't catch SIGINT\n";
    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        std::cout << "\ncan't catch SIGTERM\n";

    /* Pin main thread */
    pin_thread(pthread_self(), 3);
    if (!no_pthread) {
        /* Create worker threads */
        work_setup(psp, split);

        /* Re-enable SIGINT and SIGQUIT */
        int ret = pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        if (ret != 0) {
            fprintf(stderr, "Couln't block SIGINT: %s\n", strerror(errno));
        }

        for (auto &w: psp.workers) {
            if (pthread_join(w->me, NULL) != 0) {
                log_error("pthread_join error: %s", strerror(errno));
            }
            log_debug("Joined worker %d", w->whoami);
#ifdef LEGACY_PROFILING
            dump_latencies(*w, log_dir, label);
#endif
#ifdef OP_DEBUG
            dump_pql(&w->pql, log_dir, label);
#endif
#ifdef DMTR_TRACE
            if (w->type == NET) {
                dump_traces(w, log_dir, label);
            }
#endif
            //TODO: release shared items
            w.reset();
        }
    } else {
        std::shared_ptr<Worker> w = no_pthread_work_setup(psp);
#ifdef DMTR_TRACE
        dump_traces(w, log_dir, label);
#endif
    }

    return 0;
}
