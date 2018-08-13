// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * common/library.h
 *   Zeus general-purpose queue library implementation
 *
 * Copyright 2018 Irene Zhang  <irene.zhang@microsoft.com>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/
 
#ifndef _COMMON_LIBRARY_H_
#define _COMMON_LIBRARY_H_

#include "include/io-queue.h"
#include "queue.h"
#include <list>
#include <unordered_map>
#include <thread>
#include <assert.h>
#include <unistd.h>
#include <cstdlib>

#define BUFFER_SIZE 1024
#define MAGIC 0x10102010
#define PUSH_MASK 0x1
#define TOKEN_MASK 0x0000FFFF
#define QUEUE_MASK 0xFFFF0000
#define TOKEN(t) t & TOKEN_MASK
#define QUEUE(t) t >> 32
#define IS_PUSH(t) t & PUSH_MASK
// qtoken format
// | 32 bits = queue id | 31 bits = token | 1 bit = push or pop |

namespace Zeus {

thread_local static int64_t queue_counter = 10;
thread_local static int64_t token_counter = 10;
    
template <class QueueType>
class QueueLibrary
{
    std::unordered_map<int, QueueType *> queues;
    
public:
    QueueLibrary() {
        queue_counter = rand();
        token_counter = rand();
    };

    // ================================================
    // Queue Management functions
    // ================================================

    int HasQueue(int qd) {
        int ret = (queues.find(qd) != queues.end());
        return ret;
    };

    QueueType& GetQueue(int qd) {
        assert(HasQueue(qd));
        return *queues.at(qd);
    };
    
    qtoken GetNewToken(int qd, bool isPush) {
        if (token_counter == 0) token_counter++;
        qtoken t = (token_counter << 1 & TOKEN_MASK) | ((qtoken)qd << 32); 
        if (isPush) t |= PUSH_MASK;
        //printf("GetNewToken qd:%lx\n", t);
        token_counter++;
        return t;
    };

    QueueType& NewQueue(BasicQueueType type) {
        int qd = queue_counter++ & ~QUEUE_MASK;
        if (type == BASIC)
            queues[qd] = (QueueType *) new Queue(type, qd);
        else
            queues[qd] = new QueueType(type, qd);
        return *queues[qd];
    };

    void InsertQueue(QueueType *q) {
        int qd = q->GetQD();
        assert(qd == (qd & ~QUEUE_MASK));
        //printf("library.h/InsertQueue() qd: %d\n", qd);
        assert(queues.find(qd) == queues.end());
        queues[qd] = q;
    };


    void RemoveQueue(int qd) {
        auto it = queues.find(qd);
        assert(it != queues.end());
        delete it->second;
        queues.erase(it);    
    };

    // ================================================
    // Generic interfaces to libOS syscalls
    // ================================================

    int queue() {
        return NewQueue(BASIC).GetQD();
    }
    
    int socket(int domain, int type, int protocol) {
        QueueType &q = NewQueue(NETWORK_Q);
        int ret = q.socket(domain, type, protocol);
        if (ret < 0) {
            RemoveQueue(q.GetQD());
            return ret;
        }
        return q.GetQD();
    };

    int getsockname(int qd, struct sockaddr *saddr, socklen_t *size) {
        QueueType &q = GetQueue(qd);
        return q.getsockname(saddr, size);
    };

    int bind(int qd, struct sockaddr *saddr, socklen_t size) {
        QueueType &q = GetQueue(qd);
        return q.bind(saddr, size);
    };

    int accept(int qd, struct sockaddr *saddr, socklen_t *size) {
        QueueType &q = GetQueue(qd);
        int newqd = q.accept(saddr, size);
        if (newqd > 0){
            //printf("will InsertQueue for newqd:%d\n", newqd);
            InsertQueue(new QueueType(NETWORK_Q, newqd));
            return newqd;
        } else if (newqd < 0) {
            return newqd;
        } else {
            return NewQueue(NETWORK_Q).GetQD();
        }
    };

    int listen(int qd, int backlog) {
        QueueType &q = GetQueue(qd);
        return q.listen(backlog);
    };

    int connect(int qd, struct sockaddr *saddr, socklen_t size) {
        QueueType &q = GetQueue(qd);
        int newqd = q.connect(saddr, size);
        if (newqd > 0)
            InsertQueue(new QueueType(NETWORK_Q, newqd));
        return newqd;
    };

    int open(const char *pathname, int flags) {
        // use the fd as qd
        QueueType &q = NewQueue(FILE_Q);
        int ret = q.open(pathname, flags);
        if (ret < 0) {
            RemoveQueue(q.GetQD());
            return ret;
        } else {
            return q.GetQD();
        }
    };

    int open(const char *pathname, int flags, mode_t mode) {
        // use the fd as qd
        QueueType &q = NewQueue(FILE_Q);
        int ret = q.open(pathname, flags, mode);
        if (ret < 0) {
            RemoveQueue(q.GetQD());
            return ret;
        } else {
            return q.GetQD();
        }
    };

    int creat(const char *pathname, mode_t mode) {
        // use the fd as qd
        QueueType &q = NewQueue(FILE_Q);
        int ret = q.creat(pathname, mode);
        if (ret < 0) {
            RemoveQueue(q.GetQD());
            return ret;
        } else {
            return q.GetQD();
        }
    };
    
    int close(int qd) {
        if (!HasQueue(qd))
            return -1;
        
        QueueType &queue = GetQueue(qd);
        int res = queue.close();
        RemoveQueue(qd);    
        return res;
    };

    int qd2fd(int qd) {
        if (!HasQueue(qd))
            return -1;
        QueueType &q = GetQueue(qd);
        return q.getfd();
    };
    
    qtoken push(int qd, struct Zeus::sgarray &sga) {
        if (!HasQueue(qd))
            return -1;

        QueueType &queue = GetQueue(qd);
        if (queue.GetType() == FILE_Q)
            // pushing to files not implemented yet
            return -1;

        qtoken t = GetNewToken(qd, true);
        ssize_t res;
        if (queue.GetType() == BASIC)
            res = ((Queue &)queue).push(t, sga);
        else 
            res = queue.push(t, sga);
        // if push returns 0, then the sga is enqueued, but not pushed
        if (res == 0) {
            return t;
        } else {
            // if push returns something else, then sga has been
            // successfully pushed
            return 0;
        }
    };

    qtoken pop(int qd, struct Zeus::sgarray &sga) {
        if (!HasQueue(qd))
            return -1;
        
        QueueType &queue = GetQueue(qd);
        if (queue.GetType() == FILE_Q)
            // popping from files not implemented yet
            return -1;

        qtoken t = GetNewToken(qd, false);
        ssize_t res;
        if (queue.GetType() == BASIC)
            res = ((Queue &)queue).pop(t, sga);
        else 
            res = queue.pop(t, sga);

	if (res > 0)
	    return 0;
	else if (res == 0)
            return t;
	else
            return -1;
    };

    ssize_t peek(int qd, struct Zeus::sgarray &sga) {
        //printf("call peekp\n");
        if (!HasQueue(qd))
            return -1;
        
        QueueType &queue = GetQueue(qd);
        if (queue.GetType() == FILE_Q)
            // popping from files not implemented yet
            return -1;
        
        ssize_t res;
        if (queue.GetType() == BASIC)
            res = ((Queue &)queue).peek(sga);
        else 
            res = queue.peek(sga);

        return res;
    };

    ssize_t wait(qtoken qt, struct sgarray &sga) {
        int qd = QUEUE(qt);
        assert(HasQueue(qd));

        QueueType &queue = GetQueue(qd);
        if (queue.GetType() == FILE_Q)
            // waiting on files not implemented yet
            return 0;

        return queue.wait(qt, sga); 
    }

    ssize_t wait_any(qtoken tokens[],
                     size_t num,
                     int &offset,
                     int &qd,
                     struct sgarray &sga) {
        ssize_t res = 0;
        QueueType *waitingQs[num];
        for (unsigned int i = 0; i < num; i++) {
            int qd2 = QUEUE(tokens[i]);
            auto it2 = queues.find(qd2);
            assert(it2 != queues.end());
            
            // do a quick check if something is ready
            if (it2->second->GetType() == BASIC) {
                Queue *queue = (Queue *)it2->second; 
                res = queue->poll(tokens[i], sga);
            } else {
                res = it2->second->poll(tokens[i], sga);
            }

            if (res != 0) {
                offset = i;
                qd = qd2;
                return res;
            }
            waitingQs[i] = it2->second;
        }
        
        while (true) {
            for (unsigned int i = 0; i < num; i++) {
                QueueType *q = waitingQs[i];
                if (q->GetType() == BASIC) {
                    Queue *queue = (Queue *)q;
                    res = queue->poll(tokens[i], sga);
                } else {
                    res = q->poll(tokens[i], sga);
                }
                if (res != 0) {
                    offset = i;
                    qd = q->GetQD();
		    return res;
                }                    
            }
        }
    };
            
    ssize_t wait_all(qtoken tokens[],
                     size_t num,
                     struct sgarray **sgas) {
        ssize_t res = 0;
        for (unsigned int i = 0; i < num; i++) {
            QueueType &q = GetQueue(QUEUE(tokens[i]));
            
            ssize_t r = q.wait(tokens[i], *sgas[i]);
            if (r > 0) res += r;
        }
        return res;
    }

    ssize_t blocking_push(int qd,
                          struct sgarray &sga) {
        if (!HasQueue(qd))
            return -1;
        
        QueueType &q = GetQueue(qd);
        if (q.GetType() == FILE_Q)
            // popping from files not implemented yet
            return -1;

        qtoken t = GetNewToken(qd, false);
        ssize_t res = q.push(t, sga);
        if (res == 0) {
            return wait(t, sga);
        } else {
            // if push returns something else, then sga has been
            // successfully popped and result is in sga
            return res;
        }
    }

    ssize_t blocking_pop(int qd,
                         struct sgarray &sga) {
        if (!HasQueue(qd))
            return -1;
        
        QueueType &q = GetQueue(qd);
        if (q.GetType() == FILE_Q)
            // popping from files not implemented yet
            return -1;

        qtoken t = GetNewToken(qd, false);
        ssize_t res = q.pop(t, sga);
        if (res == 0) {
            return wait(t, sga);
        } else {
            // if push returns something else, then sga has been
            // successfully popped and result is in sga
            return res;
        }
    }
    
    int merge(int qd1, int qd2) {
        if (!HasQueue(qd1) || !HasQueue(qd2))
            return -1;

        return 0;
    };

    int filter(int qd, bool (*filter)(struct sgarray &sga)) {
        if (!HasQueue(qd))
            return -1;

        return 0;
    };
};

} // namespace Zeus
#endif /* _COMMON_LIBRARY_H_ */
