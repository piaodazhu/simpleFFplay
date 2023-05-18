#include "packet.h"

int packet_queue_init(packet_queue_t *q)
{
    memset(q, 0, sizeof(packet_queue_t));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 0;
    return 0;
}


// 写队列尾部。pkt是一包还未解码的音频数据
int packet_queue_put(packet_queue_t *q, AVPacket *pkt)
{
    packet_listnode_t *pkt_listnode;

    pkt_listnode = av_malloc(sizeof(packet_listnode_t));
    if (!pkt_listnode)
    {
        return -1;
    }
    pkt_listnode->pkt = av_packet_alloc();
    if (!pkt_listnode->pkt)
    {
        av_free(pkt_listnode);
        return -1;
    }
    
    av_packet_move_ref(pkt_listnode->pkt, pkt);
    pkt_listnode->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)   // 队列为空
    {
        q->first_pkt = pkt_listnode;
    }
    else
    {
        q->last_pkt->next = pkt_listnode;
    }
    q->last_pkt = pkt_listnode;
    q->nb_packets++;
    q->size += pkt_listnode->pkt->size;
    // 发个条件变量的信号：重启等待q->cond条件变量的一个线程
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

// 读队列头部。
int packet_queue_get(packet_queue_t *q, AVPacket *pkt, int block)
{
    packet_listnode_t *p_pkt_node;
    int ret;

    SDL_LockMutex(q->mutex);

    while (1)
    {
        p_pkt_node = q->first_pkt;
        if (p_pkt_node)             // 队列非空，取一个出来
        {
            q->first_pkt = p_pkt_node->next;
            if (!q->first_pkt)
            {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= p_pkt_node->pkt->size;
            av_packet_move_ref(pkt, p_pkt_node->pkt);
            av_packet_free(&p_pkt_node->pkt);
            av_free(p_pkt_node);
            ret = 1;
            break;
        }
        else if (!block)            // 队列空且阻塞标志无效，则立即退出
        {
            ret = 0;
            break;
        }
        else                        // 队列空且阻塞标志有效，则等待
        {
            // SDL_CondWait(q->cond, q->mutex);
            int signaled = 1;
            while (signaled && !q->abort_request) {
                signaled = SDL_CondWaitTimeout(q->cond, q->mutex, 20);
            }
            if (q->abort_request) {
                SDL_UnlockMutex(q->mutex);
                return -1;
            }
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

int packet_queue_put_nullpacket(packet_queue_t *q, int stream_index)
{
    // just alloc in stack
    AVPacket *pkt = av_packet_alloc();
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    int ret = packet_queue_put(q, pkt);
    av_packet_free(&pkt);
    return ret;
}

void packet_queue_flush(packet_queue_t *q)
{
    packet_listnode_t *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_free(&pkt->pkt);
        av_free(pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

void packet_queue_destroy(packet_queue_t *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

void packet_queue_abort(packet_queue_t *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

