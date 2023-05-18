/*******************************************************************************
 * player.c
 *
 * history:
 *   2023-05-18 - [piaodazhu]     Fix: return value uncheck
 *   2023-05-18 - [piaodazhu]     Improve: better print message
 *   2023-05-17 - [piaodazhu]     Improve: support seek
 *   2023-05-17 - [piaodazhu]     Improve: support window resize
 *   2023-05-17 - [piaodazhu]     Fix: audio cannot be paused
 *   2023-05-16 - [piaodazhu]     Fix: cannot normally quit
 *   2023-05-16 - [piaodazhu]     Fix: AVPacketList is deprecated
 * 
 *   2018-11-27 - [lei]     Create file: a simplest ffmpeg player
 *   2018-12-01 - [lei]     Playing audio
 *   2018-12-06 - [lei]     Playing audio&vidio
 *   2019-01-06 - [lei]     Add audio resampling, fix bug of unsupported audio 
 *                          format(such as planar)
 *   2019-01-16 - [lei]     Sync video to audio.
 *
 * details:
 *   A simple ffmpeg player.
 *
 * refrence:
 *   ffplay.c in FFmpeg 4.1 project.
 *******************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include "player.h"
#include "frame.h"
#include "packet.h"
#include "demux.h"
#include "video.h"
#include "audio.h"

static player_stat_t *player_init(const char *p_input_file);
static int player_deinit(player_stat_t *is);

// 返回值：返回上一帧的pts更新值(上一帧pts+流逝的时间)
double get_clock(play_clock_t *c)
{
    if (*c->queue_serial != c->serial)
    {
        return NAN;
    }
    if (c->paused)
    {
        return c->pts;
    }
    else
    {
        double time = av_gettime_relative() / 1000000.0;
        double ret = c->pts_drift + time;   // 展开得： c->pts + (time - c->last_updated)
        return ret;
    }
}

void set_clock_at(play_clock_t *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void set_clock(play_clock_t *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(play_clock_t *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

void init_clock(play_clock_t *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_play_clock_to_slave(play_clock_t *c, play_clock_t *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static void do_exit(player_stat_t *is)
{
    if (is)
    {
        player_deinit(is);
    }

    if (is->sdl_video.renderer)
        SDL_DestroyRenderer(is->sdl_video.renderer);
    if (is->sdl_video.window)
        SDL_DestroyWindow(is->sdl_video.window);
    
    avformat_network_deinit();

    SDL_Quit();
    av_log(NULL, AV_LOG_INFO, "\nQUIT\n");
    exit(0);
}

static player_stat_t *player_init(const char *p_input_file)
{
    player_stat_t *is;

    is = av_mallocz(sizeof(player_stat_t));
    if (!is)
    {
        return NULL;
    }

    is->filename = av_strdup(p_input_file);
    if (is->filename == NULL)
    {
        goto fail;
    }

    /* start video display */
    if (frame_queue_init(&is->video_frm_queue, &is->video_pkt_queue, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0 ||
        frame_queue_init(&is->audio_frm_queue, &is->audio_pkt_queue, SAMPLE_QUEUE_SIZE, 1) < 0)
    {
        goto fail;
    }

    if (packet_queue_init(&is->video_pkt_queue) < 0 ||
        packet_queue_init(&is->audio_pkt_queue) < 0)
    {
        goto fail;
    }

    packet_queue_put_nullpacket(&is->video_pkt_queue, is->video_idx);
    packet_queue_put_nullpacket(&is->audio_pkt_queue, is->audio_idx);

    if (!(is->continue_read_thread = SDL_CreateCond()))
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
fail:
        player_deinit(is);
        exit(1);
    }

    init_clock(&is->video_clk, &is->video_pkt_queue.serial);
    init_clock(&is->audio_clk, &is->audio_pkt_queue.serial);

    is->abort_request = 0;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    return is;
}


static int player_deinit(player_stat_t *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    packet_queue_abort(&is->video_pkt_queue);
    packet_queue_abort(&is->audio_pkt_queue);
    
    SDL_WaitThread(is->read_tid, NULL);
    avformat_close_input(&is->p_fmt_ctx);

    
    SDL_WaitThread(is->audio_dec_tid, NULL);
    SDL_WaitThread(is->video_dec_tid, NULL);
    SDL_WaitThread(is->video_ply_tid, NULL);
    
    packet_queue_destroy(&is->video_pkt_queue);
    packet_queue_destroy(&is->audio_pkt_queue);

    /* free all pictures */
    frame_queue_destory(&is->video_frm_queue);
    frame_queue_destory(&is->audio_frm_queue);

    SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->img_convert_ctx);
    av_free(is->filename);
    if (is->sdl_video.texture)
    {
        SDL_DestroyTexture(is->sdl_video.texture);
    }

    av_free(is);
    return 0; 
}

/* pause or resume the video */
static void stream_toggle_pause(player_stat_t *is)
{
    if (is->paused)
    {
        // 这里表示当前是暂停状态，将切换到继续播放状态。在继续播放之前，先将暂停期间流逝的时间加到frame_timer中
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->video_clk.last_updated;
        set_clock(&is->video_clk, get_clock(&is->video_clk), is->video_clk.serial);
    }
    is->paused = is->audio_clk.paused = is->video_clk.paused = !is->paused;
}

static void toggle_pause(player_stat_t *is)
{
    stream_toggle_pause(is);
    is->step = 0;
}

/* seek in the stream */
static void stream_seek(player_stat_t *is, int64_t pos, int64_t rel)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

int player_running(const char *p_input_file)
{
    player_stat_t *is = NULL;
    int ret;

    // 初始化队列，初始化SDL系统，分配player_stat_t结构体
    is = player_init(p_input_file);
    if (is == NULL)
    {
        do_exit(is);
    }

    // 文件解封装
    ret = open_demux(is);
    if (ret < 0) {
        do_exit(is);
    }

    // 视频解码与播放
    ret = open_video(is);
    if (ret < 0) {
        do_exit(is);
    }

    // 音频解码与播放
    ret = open_audio(is);
    if (ret < 0) {
        do_exit(is);
    }

    SDL_Event event;
    double incr, pos;

    while (1)
    {
        SDL_PumpEvents();
        // SDL event队列为空，则在while循环中播放视频帧。否则从队列头部取一个event，退出当前函数，在上级函数中处理event
        while (!SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
        {
            double t = is->audio_clk.pts;
            double integer = floor(t);
            double fractional = t - integer;
            
            int i = (int)integer;
            int f = (int)(100 * fractional);
            int hh = i / 3600;
            i %= 3600;
            int mm = i / 60;
            i %= 60;
            int ss = i;

            av_log(NULL, AV_LOG_INFO, "- %02d:%02d:%02d.%02d -\t quit:<ESC> | pause/unpause: <SPACE> | >>/<< <R/L/U/D>\r", hh, mm, ss, f);

            av_usleep(100000);
            SDL_PumpEvents();
        }

        switch (event.type) {
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE) // ESC: 退出
            {
                do_exit(is);
                break;
            }

            switch (event.key.keysym.sym) {
            case SDLK_SPACE:        // 空格键: 暂停
                toggle_pause(is);
                break;
            case SDLK_LEFT:         // 方向键: 快进快退
                incr = -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                    pos = is->audio_clk.pts;
                    pos += incr;
                    if (is->start_time != AV_NOPTS_VALUE && pos < is->start_time / (double)AV_TIME_BASE)
                        pos = is->start_time / (double)AV_TIME_BASE;
                    stream_seek(is, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE));
                break;
            default:
                break;
            }
            break;
        case SDL_WINDOWEVENT:
            // 窗口大小伸缩 -> 画面适应
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    is->sdl_video.window_width = event.window.data1;
                    is->sdl_video.window_height = event.window.data2;
                    if (is->sdl_video.window_width * is->sdl_video.height_width_ratio < (double)is->sdl_video.window_height) {
                        is->sdl_video.width = is->sdl_video.window_width;
                        is->sdl_video.height = (int)(is->sdl_video.window_width * is->sdl_video.height_width_ratio);
                    } else {
                        is->sdl_video.height = is->sdl_video.window_height;
                        is->sdl_video.width = (int)(is->sdl_video.window_height / is->sdl_video.height_width_ratio);
                    }
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit(is);
            break;
        default:
            break;
        }
    }

    return 0;
}
